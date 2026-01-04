// main.c - Single-file Tower Defense for GBA using tonc (Mode 3 bitmap)
// Build with devkitARM + tonc.
// This is an original mini tower defense inspired by "crystal defense" mechanics.
// No external art/assets required.

#include <tonc.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

// ----------------------------- Video helpers (Mode 3) -----------------------------

#define SCREEN_W 240
#define SCREEN_H 160

static inline void vsync(void)
{
	// Wait for VBlank without relying on any extra library helpers.
	while(REG_VCOUNT >= 160) {}
	while(REG_VCOUNT < 160) {}
}

static inline void pset(int x, int y, u16 c)
{
	if((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H)
		vid_mem[y*SCREEN_W + x] = c;
}

static void rect_fill(int x, int y, int w, int h, u16 c)
{
	if(w <= 0 || h <= 0) return;

	// Clip
	int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
	if(x0 < 0) x0 = 0;
	if(y0 < 0) y0 = 0;
	if(x1 > SCREEN_W) x1 = SCREEN_W;
	if(y1 > SCREEN_H) y1 = SCREEN_H;
	if(x0 >= x1 || y0 >= y1) return;

	for(int j = y0; j < y1; j++)
	{
		u16 *dst = &vid_mem[j*SCREEN_W + x0];
		for(int i = x0; i < x1; i++)
			*dst++ = c;
	}
}

static void rect_frame(int x, int y, int w, int h, u16 c)
{
	rect_fill(x, y, w, 1, c);
	rect_fill(x, y+h-1, w, 1, c);
	rect_fill(x, y, 1, h, c);
	rect_fill(x+w-1, y, 1, h, c);
}

static void circle_fill(int cx, int cy, int r, u16 c)
{
	if(r <= 0) return;
	int r2 = r*r;
	for(int y = -r; y <= r; y++)
	{
		int yy = y*y;
		for(int x = -r; x <= r; x++)
		{
			if(x*x + yy <= r2)
				pset(cx + x, cy + y, c);
		}
	}
}

// ----------------------------- Tiny 7-seg digits -----------------------------

// Segment bits: 0=A(top),1=B(upper-right),2=C(lower-right),3=D(bottom),4=E(lower-left),5=F(upper-left),6=G(mid)
static const u8 segmap[10] =
{
	0b00111111, // 0: A B C D E F
	0b00000110, // 1: B C
	0b01011011, // 2: A B D E G
	0b01001111, // 3: A B C D G
	0b01100110, // 4: B C F G
	0b01101101, // 5: A C D F G
	0b01111101, // 6: A C D E F G
	0b00000111, // 7: A B C
	0b01111111, // 8: A B C D E F G
	0b01101111  // 9: A B C D F G
};

static void draw_digit7(int x, int y, int d, u16 col)
{
	// Digit box ~ 10x16
	// segment thickness 2
	if(d < 0 || d > 9) return;
	u8 m = segmap[d];

	int t = 2;
	int w = 10;
	int h = 16;

	// A
	if(m & (1<<0)) rect_fill(x+t, y,     w-2*t, t, col);
	// B
	if(m & (1<<1)) rect_fill(x+w-t, y+t, t, (h/2)-t, col);
	// C
	if(m & (1<<2)) rect_fill(x+w-t, y+(h/2), t, (h/2)-t, col);
	// D
	if(m & (1<<3)) rect_fill(x+t, y+h-t, w-2*t, t, col);
	// E
	if(m & (1<<4)) rect_fill(x,     y+(h/2), t, (h/2)-t, col);
	// F
	if(m & (1<<5)) rect_fill(x,     y+t, t, (h/2)-t, col);
	// G
	if(m & (1<<6)) rect_fill(x+t, y+(h/2)-t/2, w-2*t, t, col);
}

static void draw_number7(int x, int y, int val, int digits, u16 col)
{
	// Draw fixed-width digits, leading zeros allowed.
	for(int i = digits-1; i >= 0; i--)
	{
		int d = val % 10;
		val /= 10;
		draw_digit7(x + i*12, y, d, col);
	}
}

// ----------------------------- Game constants -----------------------------

#define GRID     16
#define GW       (SCREEN_W/GRID) // 15
#define GH       (SCREEN_H/GRID) // 10

#define MAX_ENEMIES  32
#define MAX_TOWERS   32
#define MAX_BULLETS  64

// Fixed point Q8.8 for smooth movement.
#define FP_SHIFT 8
#define FP_ONE   (1<<FP_SHIFT)
#define TO_FP(x) ((x)<<FP_SHIFT)
#define FROM_FP(x) ((x)>>FP_SHIFT)

typedef struct
{
	bool active;
	int x_fp, y_fp;
	int hp, hp_max;
	int speed_fp;     // pixels/frame in Q8.8
	int wp_idx;       // waypoint target index
} Enemy;

typedef struct
{
	bool active;
	int gx, gy;       // grid position
	int range;        // pixels
	int dmg;
	int cooldown;     // frames
	int cd_left;      // frames
	int level;
} Tower;

typedef struct
{
	bool active;
	int x_fp, y_fp;
	int vx_fp, vy_fp;
	int dmg;
	int life;         // frames remaining
} Bullet;

// ----------------------------- Path / map -----------------------------

typedef struct { int x, y; } Pt;

static const Pt waypoints[] =
{
	{  0,  72 },  // enter left
	{ 64,  72 },
	{ 64,  32 },
	{144,  32 },
	{144, 112 },
	{220, 112 }   // crystal near right edge
};
static const int WP_COUNT = (int)(sizeof(waypoints)/sizeof(waypoints[0]));

static bool path_cell[GW][GH];
static int tower_at[GW][GH]; // index+1 of tower; 0 = none

static void mark_path_cells(void)
{
	memset(path_cell, 0, sizeof(path_cell));

	// Mark grid cells along each segment by sampling points.
	for(int i=0; i<WP_COUNT-1; i++)
	{
		int x0 = waypoints[i].x,     y0 = waypoints[i].y;
		int x1 = waypoints[i+1].x,   y1 = waypoints[i+1].y;

		int dx = x1 - x0;
		int dy = y1 - y0;
		int steps = (abs(dx) > abs(dy)) ? abs(dx) : abs(dy);
		if(steps < 1) steps = 1;

		for(int s=0; s<=steps; s++)
		{
			int x = x0 + (dx*s)/steps;
			int y = y0 + (dy*s)/steps;
			int gx = x / GRID;
			int gy = y / GRID;
			if((unsigned)gx < GW && (unsigned)gy < GH)
				path_cell[gx][gy] = true;
		}
	}

	// Slightly widen path by marking neighbors
	for(int x=0; x<GW; x++)
	for(int y=0; y<GH; y++)
	{
		if(!path_cell[x][y]) continue;
		for(int ox=-1; ox<=1; ox++)
		for(int oy=-1; oy<=1; oy++)
		{
			int nx=x+ox, ny=y+oy;
			if((unsigned)nx < GW && (unsigned)ny < GH)
				path_cell[nx][ny] = true;
		}
	}
}

// ----------------------------- Game state -----------------------------

static Enemy enemies[MAX_ENEMIES];
static Tower towers[MAX_TOWERS];
static Bullet bullets[MAX_BULLETS];

static int gold = 60;
static int lives = 10;
static int wave = 1;

static int spawn_timer = 0;
static int spawn_left = 0;

static int cursor_gx = 3;
static int cursor_gy = 3;

static bool paused = false;

// ----------------------------- Spawning / allocation -----------------------------

static int enemy_alloc(void)
{
	for(int i=0;i<MAX_ENEMIES;i++)
		if(!enemies[i].active) return i;
	return -1;
}

static int tower_alloc(void)
{
	for(int i=0;i<MAX_TOWERS;i++)
		if(!towers[i].active) return i;
	return -1;
}

static int bullet_alloc(void)
{
	for(int i=0;i<MAX_BULLETS;i++)
		if(!bullets[i].active) return i;
	return -1;
}

static void start_wave(int w)
{
	// Simple scaling.
	wave = w;
	spawn_left = 6 + w*2;
	spawn_timer = 0;
}

static void spawn_enemy(void)
{
	int id = enemy_alloc();
	if(id < 0) return;

	Enemy *e = &enemies[id];
	memset(e, 0, sizeof(*e));
	e->active = true;
	e->x_fp = TO_FP(waypoints[0].x);
	e->y_fp = TO_FP(waypoints[0].y);

	e->hp_max = 10 + wave*4;
	e->hp = e->hp_max;

	// speed grows slowly
	e->speed_fp = (TO_FP(1) + wave*(TO_FP(1)/6)); // ~1.0..2.0 px/frame
	e->wp_idx = 1;
}

// ----------------------------- Mechanics -----------------------------

static void enemy_take_damage(Enemy *e, int dmg)
{
	e->hp -= dmg;
	if(e->hp <= 0)
	{
		e->active = false;
		gold += 4 + wave; // reward
	}
}

static void fire_bullet(int x_fp, int y_fp, int tx_fp, int ty_fp, int dmg)
{
	int id = bullet_alloc();
	if(id < 0) return;

	Bullet *b = &bullets[id];
	memset(b, 0, sizeof(*b));
	b->active = true;
	b->x_fp = x_fp;
	b->y_fp = y_fp;
	b->dmg = dmg;
	b->life = 60; // 1 sec-ish

	// Velocity toward target, fixed magnitude.
	int dx = tx_fp - x_fp;
	int dy = ty_fp - y_fp;

	// Normalize approximately without sqrt: use max(|dx|,|dy|)
	int adx = dx < 0 ? -dx : dx;
	int ady = dy < 0 ? -dy : dy;
	int denom = (adx > ady) ? adx : ady;
	if(denom < 1) denom = 1;

	int speed_fp = TO_FP(3); // 3 px/frame
	b->vx_fp = (dx * speed_fp) / denom;
	b->vy_fp = (dy * speed_fp) / denom;
}

static void update_enemies(void)
{
	for(int i=0;i<MAX_ENEMIES;i++)
	{
		Enemy *e = &enemies[i];
		if(!e->active) continue;

		if(e->wp_idx >= WP_COUNT)
		{
			// Shouldn't happen
			e->active = false;
			continue;
		}

		int tx_fp = TO_FP(waypoints[e->wp_idx].x);
		int ty_fp = TO_FP(waypoints[e->wp_idx].y);

		int dx = tx_fp - e->x_fp;
		int dy = ty_fp - e->y_fp;

		// If close enough, snap and advance waypoint.
		int close = TO_FP(2);
		if((dx < close && dx > -close) && (dy < close && dy > -close))
		{
			e->x_fp = tx_fp;
			e->y_fp = ty_fp;
			e->wp_idx++;

			if(e->wp_idx >= WP_COUNT)
			{
				// Reached crystal
				e->active = false;
				lives--;
			}
			continue;
		}

		// Step toward target; axis-wise step keeps it simple.
		int adx = dx < 0 ? -dx : dx;
		int ady = dy < 0 ? -dy : dy;

		if(adx >= ady)
			e->x_fp += (dx > 0) ? e->speed_fp : -e->speed_fp;
		else
			e->y_fp += (dy > 0) ? e->speed_fp : -e->speed_fp;
	}
}

static void update_towers(void)
{
	for(int i=0;i<MAX_TOWERS;i++)
	{
		Tower *t = &towers[i];
		if(!t->active) continue;

		if(t->cd_left > 0)
		{
			t->cd_left--;
			continue;
		}

		// Find a target: nearest enemy in range.
		int tx = t->gx*GRID + GRID/2;
		int ty = t->gy*GRID + GRID/2;
		int best = -1;
		int best_d2 = 0x7fffffff;

		for(int j=0;j<MAX_ENEMIES;j++)
		{
			Enemy *e = &enemies[j];
			if(!e->active) continue;

			int ex = FROM_FP(e->x_fp);
			int ey = FROM_FP(e->y_fp);
			int dx = ex - tx;
			int dy = ey - ty;
			int d2 = dx*dx + dy*dy;

			if(d2 <= t->range*t->range && d2 < best_d2)
			{
				best = j;
				best_d2 = d2;
			}
		}

		if(best >= 0)
		{
			Enemy *e = &enemies[best];
			fire_bullet(TO_FP(tx), TO_FP(ty), e->x_fp, e->y_fp, t->dmg);
			t->cd_left = t->cooldown;
		}
	}
}

static void update_bullets(void)
{
	for(int i=0;i<MAX_BULLETS;i++)
	{
		Bullet *b = &bullets[i];
		if(!b->active) continue;

		b->x_fp += b->vx_fp;
		b->y_fp += b->vy_fp;
		b->life--;

		int bx = FROM_FP(b->x_fp);
		int by = FROM_FP(b->y_fp);

		// Offscreen or expired
		if(b->life <= 0 || bx < -8 || bx > SCREEN_W+8 || by < -8 || by > SCREEN_H+8)
		{
			b->active = false;
			continue;
		}

		// Hit test vs enemies (small radius)
		for(int j=0;j<MAX_ENEMIES;j++)
		{
			Enemy *e = &enemies[j];
			if(!e->active) continue;
			int ex = FROM_FP(e->x_fp);
			int ey = FROM_FP(e->y_fp);

			int dx = ex - bx;
			int dy = ey - by;
			if(dx*dx + dy*dy <= 6*6)
			{
				enemy_take_damage(e, b->dmg);
				b->active = false;
				break;
			}
		}
	}
}

// ----------------------------- Build actions -----------------------------

static bool can_build_at(int gx, int gy)
{
	if((unsigned)gx >= GW || (unsigned)gy >= GH) return false;
	if(path_cell[gx][gy]) return false;
	if(tower_at[gx][gy] != 0) return false;

	// Keep top HUD area (first row) free-ish for readability
	if(gy == 0) return false;

	return true;
}

static void place_tower(int gx, int gy)
{
	const int cost = 20;
	if(gold < cost) return;
	if(!can_build_at(gx, gy)) return;

	int id = tower_alloc();
	if(id < 0) return;

	Tower *t = &towers[id];
	memset(t, 0, sizeof(*t));
	t->active = true;
	t->gx = gx;
	t->gy = gy;
	t->range = 44;
	t->dmg = 3;
	t->cooldown = 18;
	t->cd_left = 0;
	t->level = 1;

	tower_at[gx][gy] = id+1;
	gold -= cost;
}

static void upgrade_tower_at(int gx, int gy)
{
	int idx1 = tower_at[gx][gy];
	if(idx1 == 0) return;
	Tower *t = &towers[idx1-1];
	if(!t->active) return;

	int cost = 15 + t->level*10;
	if(gold < cost) return;

	t->level++;
	t->dmg += 2;
	t->range += 6;
	if(t->cooldown > 8) t->cooldown -= 1;
	gold -= cost;
}

static void sell_tower_at(int gx, int gy)
{
	int idx1 = tower_at[gx][gy];
	if(idx1 == 0) return;
	Tower *t = &towers[idx1-1];
	if(!t->active) { tower_at[gx][gy] = 0; return; }

	// Refund half-ish based on level
	int refund = 10 + t->level*10;
	gold += refund;

	t->active = false;
	tower_at[gx][gy] = 0;
}

// ----------------------------- Rendering -----------------------------

static void draw_map(void)
{
	u16 grass = RGB15(6, 18, 6);
	u16 path  = RGB15(16, 12, 6);
	u16 gridc = RGB15(4, 10, 4);

	// Grass background
	rect_fill(0, 0, SCREEN_W, SCREEN_H, grass);

	// Path cells
	for(int gx=0; gx<GW; gx++)
	for(int gy=0; gy<GH; gy++)
	{
		if(path_cell[gx][gy])
			rect_fill(gx*GRID, gy*GRID, GRID, GRID, path);
		else
		{
			// Subtle grid on grass (very light)
			// draw only top/left edges to avoid heavy overdraw
			rect_fill(gx*GRID, gy*GRID, GRID, 1, gridc);
			rect_fill(gx*GRID, gy*GRID, 1, GRID, gridc);
		}
	}

	// Crystal at end waypoint
	int cx = waypoints[WP_COUNT-1].x;
	int cy = waypoints[WP_COUNT-1].y;
	circle_fill(cx, cy, 10, RGB15(6, 10, 24));
	circle_fill(cx, cy, 6,  RGB15(12, 18, 31));
	rect_frame(cx-12, cy-12, 24, 24, RGB15(31,31,31));
}

static void draw_towers(void)
{
	for(int i=0;i<MAX_TOWERS;i++)
	{
		Tower *t = &towers[i];
		if(!t->active) continue;

		int x = t->gx*GRID;
		int y = t->gy*GRID;

		u16 base = RGB15(12,12,12);
		u16 top  = RGB15(20,20,20);
		u16 rim  = RGB15(31,31,31);

		rect_fill(x+3, y+6, GRID-6, GRID-6, base);
		rect_fill(x+5, y+4, GRID-10, 6, top);
		rect_frame(x+3, y+6, GRID-6, GRID-6, rim);

		// Level pips
		for(int k=0; k<t->level && k<5; k++)
			pset(x+4+k*2, y+GRID-3, RGB15(31, 28, 0));
	}
}

static void draw_enemies(void)
{
	for(int i=0;i<MAX_ENEMIES;i++)
	{
		Enemy *e = &enemies[i];
		if(!e->active) continue;

		int x = FROM_FP(e->x_fp);
		int y = FROM_FP(e->y_fp);

		// Body
		circle_fill(x, y, 5, RGB15(25,6,6));
		circle_fill(x-1, y-1, 2, RGB15(31,18,18));

		// HP bar
		int w = 14;
		int hpw = (e->hp * w) / (e->hp_max ? e->hp_max : 1);
		rect_fill(x - w/2, y - 10, w, 2, RGB15(6,6,6));
		rect_fill(x - w/2, y - 10, hpw, 2, RGB15(0,28,0));
	}
}

static void draw_bullets(void)
{
	for(int i=0;i<MAX_BULLETS;i++)
	{
		Bullet *b = &bullets[i];
		if(!b->active) continue;

		int x = FROM_FP(b->x_fp);
		int y = FROM_FP(b->y_fp);

		circle_fill(x, y, 2, RGB15(31, 29, 0));
	}
}

static void draw_hud(void)
{
	// HUD bar
	rect_fill(0, 0, SCREEN_W, 20, RGB15(0,0,0));
	rect_frame(0, 0, SCREEN_W, 20, RGB15(31,31,31));

	// Gold (3 digits)
	// Lives (2 digits)
	// Wave (2 digits)
	// Draw with 7-seg digits (white)
	u16 col = RGB15(31,31,31);
	u16 col2 = RGB15(0, 28, 0);
	u16 col3 = RGB15(0, 18, 31);

	// separators
	rect_fill(82, 2, 2, 16, RGB15(31,31,31));
	rect_fill(150, 2, 2, 16, RGB15(31,31,31));

	draw_number7(6,   2, gold < 0 ? 0 : (gold > 999 ? 999 : gold), 3, col2);
	draw_number7(96,  2, lives < 0 ? 0 : (lives > 99 ? 99 : lives), 2, col);
	draw_number7(164, 2, wave  < 0 ? 0 : (wave  > 99 ? 99 : wave),  2, col3);

	// Small markers (G, L, W) as colored dots (since we didn't include a font)
	circle_fill(46, 10, 3, col2);  // gold marker
	circle_fill(126,10, 3, col);   // lives marker
	circle_fill(194,10, 3, col3);  // wave marker

	if(paused)
	{
		rect_fill(80, 70, 80, 20, RGB15(0,0,0));
		rect_frame(80, 70, 80, 20, RGB15(31,31,31));
		// "pause" as 2 bars
		rect_fill(110, 74, 8, 12, RGB15(31,31,31));
		rect_fill(130, 74, 8, 12, RGB15(31,31,31));
	}
}

static void draw_cursor(void)
{
	int x = cursor_gx*GRID;
	int y = cursor_gy*GRID;

	u16 c = RGB15(31,31,31);
	rect_frame(x, y, GRID, GRID, c);

	// If buildable, show green hint; if blocked, red hint.
	bool buildable = can_build_at(cursor_gx, cursor_gy);
	u16 hint = buildable ? RGB15(0,31,0) : RGB15(31,0,0);
	rect_frame(x+2, y+2, GRID-4, GRID-4, hint);

	// If hovering an existing tower, show its range ring (rough)
	int idx1 = tower_at[cursor_gx][cursor_gy];
	if(idx1 != 0 && towers[idx1-1].active)
	{
		Tower *t = &towers[idx1-1];
		int cx = cursor_gx*GRID + GRID/2;
		int cy = cursor_gy*GRID + GRID/2;

		// Draw a coarse ring by plotting points every few degrees-ish
		u16 rc = RGB15(10,10,31);
		int r = t->range;
		for(int a=0; a<360; a+=12)
		{
			// tiny integer "trig" via lookup-ish approximation:
			// Use a circle param with fixed table would be nicer; keep simple with rough sin/cos via tonc's lu_sin/lu_cos if available.
			// To stay single-file and robust, just plot along a diamond as an approximation:
			// (This is intentionally cheap.)
			(void)a;
		}

		// Diamond approximation of circle
		for(int dx=-r; dx<=r; dx+=4)
		{
			int dy = r - (dx < 0 ? -dx : dx);
			pset(cx+dx, cy+dy, rc);
			pset(cx+dx, cy-dy, rc);
		}
	}
}

static void render(void)
{
	draw_map();
	draw_towers();
	draw_enemies();
	draw_bullets();
	draw_hud();
	draw_cursor();
}

// ----------------------------- Input / wave control -----------------------------

static int active_enemies_count(void)
{
	int c = 0;
	for(int i=0;i<MAX_ENEMIES;i++)
		if(enemies[i].active) c++;
	return c;
}

static void handle_input(void)
{
	key_poll();

	if(key_hit(KEY_START))
		paused = !paused;

	if(paused) return;

	// Cursor move (one cell per hit)
	if(key_hit(KEY_LEFT)  && cursor_gx > 0)      cursor_gx--;
	if(key_hit(KEY_RIGHT) && cursor_gx < GW-1)   cursor_gx++;
	if(key_hit(KEY_UP)    && cursor_gy > 0)      cursor_gy--;
	if(key_hit(KEY_DOWN)  && cursor_gy < GH-1)   cursor_gy++;

	// A: place if empty, upgrade if tower present
	if(key_hit(KEY_A))
	{
		if(tower_at[cursor_gx][cursor_gy] == 0)
			place_tower(cursor_gx, cursor_gy);
		else
			upgrade_tower_at(cursor_gx, cursor_gy);
	}

	// B: sell tower
	if(key_hit(KEY_B))
		sell_tower_at(cursor_gx, cursor_gy);
}

static void update_wave_spawner(void)
{
	// Spawn enemies with a small interval while spawn_left > 0.
	if(spawn_left > 0)
	{
		if(spawn_timer <= 0)
		{
			spawn_enemy();
			spawn_left--;
			spawn_timer = 24; // spawn interval
		}
		else spawn_timer--;
	}
	else
	{
		// If wave cleared, start next wave after a brief lull.
		if(active_enemies_count() == 0)
		{
			static int lull = 0;
			lull++;
			if(lull > 60)
			{
				lull = 0;
				start_wave(wave + 1);
			}
		}
	}
}

// ----------------------------- Main -----------------------------

int main(void)
{
	// Mode 3 bitmap
	REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

	// Init map data
	memset(enemies, 0, sizeof(enemies));
	memset(towers,  0, sizeof(towers));
	memset(bullets, 0, sizeof(bullets));
	memset(tower_at,0, sizeof(tower_at));
	mark_path_cells();

	start_wave(1);

	while(1)
	{
		vsync();

		handle_input();

		if(!paused && lives > 0)
		{
			update_wave_spawner();
			update_enemies();
			update_towers();
			update_bullets();
		}
		else if(lives <= 0)
		{
			// Simple "game over" effect: darken screen a bit by drawing a box.
			rect_fill(40, 60, 160, 40, RGB15(0,0,0));
			rect_frame(40, 60, 160, 40, RGB15(31,0,0));
			paused = true;
		}

		render();
	}

	return 0;
}
