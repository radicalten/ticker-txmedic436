// main.c - Single-file "TwinBee-like" vertical shmup demo for GBA using tonc.
// Features: tilemap starfield scroll, player ship, enemies, bullets, bell powerups,
//           simple collisions, lives/power, all graphics generated in code (no assets).
//
// Build (typical devkitARM + tonc setup):
//   arm-none-eabi-gcc -mthumb-interwork -mthumb -O2 -Wall -Wextra -std=c11 main.c -ltonc -o game.elf
// Then convert/link as your environment expects (or use a tonc template Makefile).

#include <tonc.h>
#include <stdbool.h>
#include <stddef.h>

// ----------------------------- Config ---------------------------------

#define SCR_W 240
#define SCR_H 160

#define MAX_ENEMIES       24
#define MAX_PBULLETS      40
#define MAX_EBULLETS      40
#define MAX_POWERUPS      10

// OBJ tile indices (1D mapping, 4bpp):
// 16x16 sprites take 4 consecutive 8x8 tiles.
enum
{
	TID_PLAYER  = 0,   // 0..3
	TID_ENEMY   = 4,   // 4..7
	TID_PBULLET = 8,   // 8
	TID_EBULLET = 9,   // 9
	TID_BELL    = 10,  // 10
};

// Pal indices (OBJ palbank 0, BG palbank 0)
enum
{
	BG_PAL = 0,
	OBJ_PAL = 0
};

// ----------------------------- Small utils ----------------------------

static inline int clampi(int v, int lo, int hi)
{
	if(v < lo) return lo;
	if(v > hi) return hi;
	return v;
}

static inline bool aabb(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
	return (ax < bx+bw) && (ax+aw > bx) && (ay < by+bh) && (ay+ah > by);
}

// LCG rng (deterministic)
static u32 g_rng = 0x1234ABCD;
static inline u32 rng_u32(void)
{
	g_rng = 1664525u*g_rng + 1013904223u;
	return g_rng;
}
static inline int rng_range(int lo, int hi_inclusive)
{
	u32 r = rng_u32() >> 16;
	int span = (hi_inclusive - lo) + 1;
	return lo + (int)(r % (u32)span);
}

// ----------------------------- Tile plotting ---------------------------
// 4bpp TILE in tonc is 8 u32 words; each u32 = one 8px row, 4bpp packed.
static inline void tile4_clear(TILE *t)
{
	for(int i=0;i<8;i++) t->data[i]=0;
}
static inline void tile4_pset(TILE *t, int x, int y, u8 c)
{
	// x:0..7 y:0..7, c:0..15
	u32 *row = &t->data[y];
	u32 shift = (u32)(x*4);
	u32 mask = 0xFu << shift;
	*row = (*row & ~mask) | ((u32)(c & 0xF) << shift);
}

// Plot on a 16x16 sprite stored as 4 consecutive tiles in 1D (2x2 tiles).
static inline void spr16_pset(int base_tid, int x, int y, u8 c)
{
	if((unsigned)x >= 16u || (unsigned)y >= 16u) return;

	int tx = x >> 3;             // 0..1
	int ty = y >> 3;             // 0..1
	int tid = base_tid + tx + (ty<<1);

	TILE *t = &tile_mem[4][tid];
	tile4_pset(t, x & 7, y & 7, c);
}

// ----------------------------- Game state ------------------------------

typedef struct
{
	int x, y;           // px
	int shot_cd;        // frames
	int power;          // 0..3
	int lives;          // >=0
	int invuln;         // frames
	bool alive;
} Player;

typedef struct
{
	int x, y;
	int vx, vy;
	int hp;
	int fire_t;         // timer for enemy shooting
	bool active;
} Enemy;

typedef struct
{
	int x, y;
	int vx, vy;
	int ttl;
	bool active;
	bool friendly;
} Bullet;

typedef struct
{
	int x, y;
	int vy;
	int color;          // 0..2
	int cycle_t;
	bool active;
} Powerup;

static Player  g_plr;
static Enemy   g_enemies[MAX_ENEMIES];
static Bullet  g_bullets[MAX_PBULLETS + MAX_EBULLETS];
static Powerup g_pows[MAX_POWERUPS];

static int g_scroll_y = 0;
static int g_spawn_t = 0;
static int g_frame = 0;

// Sprite staging buffer
static OBJ_ATTR obj_buf[128];

// ----------------------------- Spawning --------------------------------

static Bullet* bullet_alloc(bool friendly)
{
	int max = MAX_PBULLETS + MAX_EBULLETS;
	for(int i=0;i<max;i++)
	{
		if(!g_bullets[i].active)
		{
			g_bullets[i].active = true;
			g_bullets[i].friendly = friendly;
			g_bullets[i].ttl = 90;
			return &g_bullets[i];
		}
	}
	return NULL;
}

static Enemy* enemy_alloc(void)
{
	for(int i=0;i<MAX_ENEMIES;i++)
	{
		if(!g_enemies[i].active)
		{
			g_enemies[i].active = true;
			return &g_enemies[i];
		}
	}
	return NULL;
}

static Powerup* pow_alloc(void)
{
	for(int i=0;i<MAX_POWERUPS;i++)
	{
		if(!g_pows[i].active)
		{
			g_pows[i].active = true;
			return &g_pows[i];
		}
	}
	return NULL;
}

static void spawn_enemy(void)
{
	Enemy *e = enemy_alloc();
	if(!e) return;

	e->x = rng_range(8, SCR_W-24);
	e->y = -16;
	e->vy = rng_range(1, 2);
	e->vx = rng_range(0, 1) ? 1 : -1;
	if(rng_range(0, 3) == 0) e->vx = 0;          // some go straight
	e->hp = (rng_range(0, 5)==0) ? 3 : 1;        // occasional tougher enemy
	e->fire_t = rng_range(20, 80);
}

static void spawn_bell(int x, int y)
{
	Powerup *p = pow_alloc();
	if(!p) return;
	p->x = x;
	p->y = y;
	p->vy = 1;
	p->color = rng_range(0, 2);
	p->cycle_t = 0;
}

static void player_shoot(void)
{
	if(g_plr.shot_cd > 0) return;

	// Base origin (ship "nose")
	int ox = g_plr.x + 8;
	int oy = g_plr.y + 2;

	// Fire patterns by power level
	if(g_plr.power == 0)
	{
		Bullet *b = bullet_alloc(true);
		if(b){ b->x=ox-4; b->y=oy; b->vx=0; b->vy=-5; b->ttl=70; }
		g_plr.shot_cd = 10;
	}
	else if(g_plr.power == 1)
	{
		for(int s=-1; s<=1; s+=2)
		{
			Bullet *b = bullet_alloc(true);
			if(b){ b->x=ox-4 + s*4; b->y=oy; b->vx=0; b->vy=-5; b->ttl=70; }
		}
		g_plr.shot_cd = 10;
	}
	else if(g_plr.power == 2)
	{
		int vxs[3] = {-1, 0, 1};
		int vys[3] = {-4, -6, -4};
		for(int i=0;i<3;i++)
		{
			Bullet *b = bullet_alloc(true);
			if(b){ b->x=ox-4; b->y=oy; b->vx=vxs[i]; b->vy=vys[i]; b->ttl=70; }
		}
		g_plr.shot_cd = 9;
	}
	else // power 3
	{
		// faster "laser-ish" stream
		Bullet *b = bullet_alloc(true);
		if(b){ b->x=ox-4; b->y=oy; b->vx=0; b->vy=-7; b->ttl=55; }
		g_plr.shot_cd = 5;
	}
}

// ----------------------------- Init graphics ---------------------------

static void init_palettes(void)
{
	// BG palette
	pal_bg_mem[0] = RGB15(0,0,0);
	pal_bg_mem[1] = RGB15(31,31,31);  // bright star
	pal_bg_mem[2] = RGB15(16,16,16);  // dim star

	// OBJ palette (16 colors)
	pal_obj_mem[0]  = RGB15(0,0,0);        // transparent (index 0)
	pal_obj_mem[1]  = RGB15(31,31,31);     // white
	pal_obj_mem[2]  = RGB15(10,18,31);     // light blue (player body)
	pal_obj_mem[3]  = RGB15(4,8,20);       // dark blue (cockpit)
	pal_obj_mem[4]  = RGB15(31,8,8);       // red
	pal_obj_mem[5]  = RGB15(31,18,0);      // orange
	pal_obj_mem[6]  = RGB15(31,31,0);      // yellow
	pal_obj_mem[7]  = RGB15(8,31,8);       // green
	pal_obj_mem[8]  = RGB15(10,10,10);     // dark gray
	pal_obj_mem[9]  = RGB15(20,20,20);     // gray
	// rest unused
}

static void make_bg_tiles_and_map(void)
{
	// BG0 tiles in charblock 0
	// Tile 0: empty
	tile4_clear(&tile_mem[0][0]);

	// Tile 1: bright star
	tile4_clear(&tile_mem[0][1]);
	tile4_pset(&tile_mem[0][1], 3,3, 1);
	tile4_pset(&tile_mem[0][1], 4,3, 1);
	tile4_pset(&tile_mem[0][1], 3,4, 1);
	tile4_pset(&tile_mem[0][1], 4,4, 1);

	// Tile 2: dim star
	tile4_clear(&tile_mem[0][2]);
	tile4_pset(&tile_mem[0][2], 4,4, 2);

	// Fill a 32x32 map in screenblock 31 with random stars
	u16 *map = se_mem[31];
	for(int i=0;i<32*32;i++)
	{
		int roll = rng_range(0, 99);
		u16 tid = 0;
		if(roll < 6) tid = 2;
		if(roll < 2) tid = 1;
		map[i] = tid | SE_PALBANK(BG_PAL);
	}
}

static void make_obj_tiles(void)
{
	// Clear a small range of OBJ tiles we use
	for(int i=0;i<16;i++)
		tile4_clear(&tile_mem[4][i]);

	// --- Player 16x16 (TID_PLAYER..TID_PLAYER+3) ---
	// Procedural little "ship": white outline, light-blue body, dark-blue cockpit.
	for(int y=0;y<16;y++)
	{
		int l, r;
		if(y < 4)      { l = 7 - y; r = 8 + y; }
		else if(y < 10){ l = 3;     r = 12; }
		else           { l = 5;     r = 10; }

		for(int x=l; x<=r; x++)
		{
			u8 col = 2; // body
			// outline
			if(x==l || x==r || y==0 || y==15) col = 1;
			spr16_pset(TID_PLAYER, x, y, col);
		}

		// cockpit
		if(y>=5 && y<=8)
		{
			for(int x=7; x<=8; x++)
				spr16_pset(TID_PLAYER, x, y, 3);
		}
	}

	// --- Enemy 16x16 (TID_ENEMY..TID_ENEMY+3) ---
	// Simple "bee-ish" round blob with orange stripe.
	for(int y=0;y<16;y++)
	{
		for(int x=0;x<16;x++)
		{
			int dx = x-7;
			int dy = y-7;
			int d2 = dx*dx + dy*dy;
			if(d2 <= 7*7)
			{
				u8 col = 4; // red
				if((y/3) & 1) col = 5; // orange stripes
				// outline at edge-ish
				if(d2 >= 6*6) col = 1;
				spr16_pset(TID_ENEMY, x, y, col);
			}
		}
	}
	// tiny "eyes"
	spr16_pset(TID_ENEMY, 5,6, 1);
	spr16_pset(TID_ENEMY, 10,6, 1);

	// --- Player bullet 8x8 (TID_PBULLET) ---
	tile4_clear(&tile_mem[4][TID_PBULLET]);
	for(int y=1;y<=6;y++)
	{
		tile4_pset(&tile_mem[4][TID_PBULLET], 3, y, 6);
		tile4_pset(&tile_mem[4][TID_PBULLET], 4, y, 6);
	}
	tile4_pset(&tile_mem[4][TID_PBULLET], 3, 0, 1);
	tile4_pset(&tile_mem[4][TID_PBULLET], 4, 0, 1);

	// --- Enemy bullet 8x8 (TID_EBULLET) ---
	tile4_clear(&tile_mem[4][TID_EBULLET]);
	for(int y=2;y<=5;y++)
	{
		tile4_pset(&tile_mem[4][TID_EBULLET], 3, y, 4);
		tile4_pset(&tile_mem[4][TID_EBULLET], 4, y, 4);
	}
	tile4_pset(&tile_mem[4][TID_EBULLET], 2, 4, 5);
	tile4_pset(&tile_mem[4][TID_EBULLET], 5, 4, 5);

	// --- Bell 8x8 (TID_BELL) ---
	// Color-cycled by swapping OBJ palette indices in draw step (simple trick).
	tile4_clear(&tile_mem[4][TID_BELL]);
	for(int y=1;y<=6;y++)
	{
		for(int x=2;x<=5;x++)
			tile4_pset(&tile_mem[4][TID_BELL], x, y, 7); // will recolor via pal trick? (we'll just pick colors by OAM palette swap not possible per sprite)
	}
	// outline
	for(int x=2;x<=5;x++){ tile4_pset(&tile_mem[4][TID_BELL], x, 1, 1); tile4_pset(&tile_mem[4][TID_BELL], x, 6, 1); }
	for(int y=1;y<=6;y++){ tile4_pset(&tile_mem[4][TID_BELL], 2, y, 1); tile4_pset(&tile_mem[4][TID_BELL], 5, y, 1); }
	// clapper
	tile4_pset(&tile_mem[4][TID_BELL], 3, 6, 6);
	tile4_pset(&tile_mem[4][TID_BELL], 4, 6, 6);
}

static void init_video(void)
{
	REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

	// BG0: 4bpp, 32x32, CBB0, SBB31, low priority (behind sprites)
	REG_BG0CNT = BG_CBB(0) | BG_SBB(31) | BG_4BPP | BG_REG_32x32 | BG_PRIO(3);

	// IRQ for VBlankIntrWait
	REG_IME = 0;
	REG_IE = IRQ_VBLANK;
	REG_DISPSTAT |= DSTAT_VBL_IRQ;
	REG_IME = 1;
}

// ----------------------------- Game init -------------------------------

static void reset_game(void)
{
	g_plr.x = (SCR_W/2) - 8;
	g_plr.y = SCR_H - 24;
	g_plr.shot_cd = 0;
	g_plr.power = 0;
	g_plr.lives = 3;
	g_plr.invuln = 90;
	g_plr.alive = true;

	for(int i=0;i<MAX_ENEMIES;i++) g_enemies[i].active = false;
	for(int i=0;i<MAX_PBULLETS + MAX_EBULLETS;i++) g_bullets[i].active = false;
	for(int i=0;i<MAX_POWERUPS;i++) g_pows[i].active = false;

	g_scroll_y = 0;
	g_spawn_t = 0;
	g_frame = 0;
}

// ----------------------------- Update ----------------------------------

static void update_player(void)
{
	if(!g_plr.alive) return;

	// movement
	int spd = key_is_down(KEY_B) ? 1 : 2;
	if(key_is_down(KEY_LEFT))  g_plr.x -= spd;
	if(key_is_down(KEY_RIGHT)) g_plr.x += spd;
	if(key_is_down(KEY_UP))    g_plr.y -= spd;
	if(key_is_down(KEY_DOWN))  g_plr.y += spd;

	g_plr.x = clampi(g_plr.x, 0, SCR_W-16);
	g_plr.y = clampi(g_plr.y, 0, SCR_H-16);

	// shooting
	if(key_is_down(KEY_A))
		player_shoot();

	if(g_plr.shot_cd > 0) g_plr.shot_cd--;
	if(g_plr.invuln > 0) g_plr.invuln--;
}

static void update_enemies(void)
{
	for(int i=0;i<MAX_ENEMIES;i++)
	{
		Enemy *e = &g_enemies[i];
		if(!e->active) continue;

		e->x += e->vx;
		e->y += e->vy;

		// gentle bounce horizontally
		if(e->x < 4) { e->x = 4; e->vx = 1; }
		if(e->x > SCR_W-20) { e->x = SCR_W-20; e->vx = -1; }

		// enemy shooting
		e->fire_t--;
		if(e->fire_t <= 0 && e->y >= 0)
		{
			Bullet *b = bullet_alloc(false);
			if(b)
			{
				b->x = e->x + 8 - 4;
				b->y = e->y + 12;
				b->vx = 0;
				b->vy = 3;
				b->ttl = 80;
			}
			e->fire_t = rng_range(40, 90);
		}

		// offscreen cleanup
		if(e->y > SCR_H + 24)
			e->active = false;
	}
}

static void update_bullets(void)
{
	int max = MAX_PBULLETS + MAX_EBULLETS;
	for(int i=0;i<max;i++)
	{
		Bullet *b = &g_bullets[i];
		if(!b->active) continue;

		b->x += b->vx;
		b->y += b->vy;
		b->ttl--;

		if(b->ttl <= 0 || b->x < -8 || b->x > SCR_W || b->y < -12 || b->y > SCR_H+12)
			b->active = false;
	}
}

static void update_powerups(void)
{
	for(int i=0;i<MAX_POWERUPS;i++)
	{
		Powerup *p = &g_pows[i];
		if(!p->active) continue;

		p->y += p->vy;
		p->cycle_t++;

		// cycle color every ~15 frames
		if((p->cycle_t % 15) == 0)
			p->color = (p->color + 1) % 3;

		if(p->y > SCR_H+8)
			p->active = false;
	}
}

static void do_collisions(void)
{
	// Player bullets vs enemies
	int max = MAX_PBULLETS + MAX_EBULLETS;
	for(int bi=0; bi<max; bi++)
	{
		Bullet *b = &g_bullets[bi];
		if(!b->active || !b->friendly) continue;

		for(int ei=0; ei<MAX_ENEMIES; ei++)
		{
			Enemy *e = &g_enemies[ei];
			if(!e->active) continue;

			if(aabb(b->x, b->y, 8, 8, e->x, e->y, 16, 16))
			{
				b->active = false;
				e->hp -= 1;
				if(e->hp <= 0)
				{
					// chance to drop a bell
					if(rng_range(0, 4) == 0)
						spawn_bell(e->x+4, e->y+4);
					e->active = false;
				}
				break;
			}
		}
	}

	// Player vs enemies / enemy bullets
	if(g_plr.alive && g_plr.invuln == 0)
	{
		// vs enemies
		for(int ei=0; ei<MAX_ENEMIES; ei++)
		{
			Enemy *e = &g_enemies[ei];
			if(!e->active) continue;

			if(aabb(g_plr.x, g_plr.y, 16, 16, e->x, e->y, 16, 16))
			{
				e->active = false;
				goto player_hit;
			}
		}

		// vs enemy bullets
		for(int bi=0; bi<max; bi++)
		{
			Bullet *b = &g_bullets[bi];
			if(!b->active || b->friendly) continue;

			if(aabb(g_plr.x, g_plr.y, 16, 16, b->x, b->y, 8, 8))
			{
				b->active = false;
				goto player_hit;
			}
		}
	}

	// Player vs powerups
	for(int pi=0; pi<MAX_POWERUPS; pi++)
	{
		Powerup *p = &g_pows[pi];
		if(!p->active) continue;

		if(aabb(g_plr.x, g_plr.y, 16, 16, p->x, p->y, 8, 8))
		{
			p->active = false;

			// "TwinBee-ish": bell color affects reward (simplified)
			// 0: yellow -> power up
			// 1: blue-ish -> small power
			// 2: green -> bigger power
			if(p->color == 0) g_plr.power = clampi(g_plr.power + 1, 0, 3);
			else if(p->color == 1) g_plr.power = clampi(g_plr.power + 1, 0, 3);
			else g_plr.power = clampi(g_plr.power + 2, 0, 3);
		}
	}

	return;

player_hit:
	g_plr.lives--;
	g_plr.power = 0;
	g_plr.invuln = 120;
	g_plr.x = (SCR_W/2) - 8;
	g_plr.y = SCR_H - 24;

	if(g_plr.lives < 0)
	{
		// quick reset loop
		reset_game();
	}
}

static void update_game(void)
{
	g_frame++;

	// Scroll starfield (BG hardware scroll; repeats every 256px)
	g_scroll_y = (g_scroll_y + 1) & 255;
	REG_BG0VOFS = (u16)g_scroll_y;
	REG_BG0HOFS = 0;

	// Spawn enemies over time
	g_spawn_t--;
	if(g_spawn_t <= 0)
	{
		spawn_enemy();
		// faster over time, capped
		int base = 40 - (g_frame/600);
		if(base < 12) base = 12;
		g_spawn_t = rng_range(base, base+20);
	}

	update_player();
	update_enemies();
	update_bullets();
	update_powerups();
	do_collisions();
}

// ----------------------------- Draw ------------------------------------

static inline void obj_push_hide_rest(int start)
{
	for(int i=start; i<128; i++)
		obj_hide(&obj_buf[i]);
}

static int draw_player(int oid)
{
	if(!g_plr.alive) return oid;

	// Flicker during invuln
	if(g_plr.invuln > 0 && ((g_frame>>2)&1))
		return oid;

	OBJ_ATTR *o = &obj_buf[oid++];
	obj_set_attr(o,
		ATTR0_SQUARE | ATTR0_4BPP,
		ATTR1_SIZE_16,
		ATTR2_BUILD(TID_PLAYER, OBJ_PAL, 0));
	obj_set_pos(o, g_plr.x, g_plr.y);

	return oid;
}

static int draw_enemies(int oid)
{
	for(int i=0;i<MAX_ENEMIES;i++)
	{
		Enemy *e = &g_enemies[i];
		if(!e->active) continue;

		if(e->y < -15 || e->y >= SCR_H) continue;

		OBJ_ATTR *o = &obj_buf[oid++];
		obj_set_attr(o,
			ATTR0_SQUARE | ATTR0_4BPP,
			ATTR1_SIZE_16,
			ATTR2_BUILD(TID_ENEMY, OBJ_PAL, 1));
		obj_set_pos(o, e->x, e->y);

		if(oid >= 128) return oid;
	}
	return oid;
}

static int draw_bullets(int oid)
{
	int max = MAX_PBULLETS + MAX_EBULLETS;
	for(int i=0;i<max;i++)
	{
		Bullet *b = &g_bullets[i];
		if(!b->active) continue;

		if(b->y < -7 || b->y >= SCR_H) continue;

		int tid = b->friendly ? TID_PBULLET : TID_EBULLET;

		OBJ_ATTR *o = &obj_buf[oid++];
		obj_set_attr(o,
			ATTR0_SQUARE | ATTR0_4BPP,
			ATTR1_SIZE_8,
			ATTR2_BUILD(tid, OBJ_PAL, 0));
		obj_set_pos(o, b->x, b->y);

		if(oid >= 128) return oid;
	}
	return oid;
}

static int draw_powerups(int oid)
{
	for(int i=0;i<MAX_POWERUPS;i++)
	{
		Powerup *p = &g_pows[i];
		if(!p->active) continue;

		if(p->y < -7 || p->y >= SCR_H) continue;

		// Tint by swapping the fill color index in OBJ palette would require per-sprite palbank.
		// We keep single palbank for simplicity, and just "hint" color by sprite priority:
		// Instead, we do a cheap hack: change the sprite's priority (not color) based on p->color.
		// If you want true per-bell color, use multiple palbanks and different tile colors.
		int prio = 0;
		if(p->color == 1) prio = 1;
		if(p->color == 2) prio = 2;

		OBJ_ATTR *o = &obj_buf[oid++];
		obj_set_attr(o,
			ATTR0_SQUARE | ATTR0_4BPP,
			ATTR1_SIZE_8,
			ATTR2_BUILD(TID_BELL, OBJ_PAL, prio));
		obj_set_pos(o, p->x, p->y);

		if(oid >= 128) return oid;
	}
	return oid;
}

static void draw_game(void)
{
	int oid = 0;

	oid = draw_player(oid);
	oid = draw_enemies(oid);
	oid = draw_bullets(oid);
	oid = draw_powerups(oid);

	obj_push_hide_rest(oid);
	oam_copy(oam_mem, obj_buf, 128);
}

// ----------------------------- Main ------------------------------------

int main(void)
{
	irq_init(NULL);

	init_video();
	init_palettes();
	make_bg_tiles_and_map();
	make_obj_tiles();

	// Hide all sprites initially
	for(int i=0;i<128;i++)
		obj_hide(&obj_buf[i]);
	oam_copy(oam_mem, obj_buf, 128);

	reset_game();

	while(1)
	{
		VBlankIntrWait();
		key_poll();

		// quick restart
		if(key_hit(KEY_START))
			reset_game();

		update_game();
		draw_game();
	}

	// not reached
	return 0;
}
