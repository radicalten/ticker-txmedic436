// kirbyish_sidescroller_tonc_singlefile.c
// A tiny “Kirby-ish” 2D side scroller demo for GBA using tonc:
// - Tilemap BG (Mode 0) with horizontal scrolling camera
// - 16x16 sprite player with floaty multi-jump
// - A couple simple patrolling enemies
// - “Inhale” (B): remove an enemy in front of you within range
//
// Art is generated at runtime (simple circles/blocks), so this is truly single-file.
// Build with devkitARM + tonc (libtonc). Example Makefile not included.
//
// Controls:
//  D-Pad: move
//  A: jump (up to 5 jumps); hold A while falling = floaty slow-fall
//  B: inhale (eat enemy in front, short range)

#include <tonc.h>

// ----------------------------- Config ---------------------------------

#define MAP_W_TILES  64
#define MAP_H_TILES  32
#define TILE_SZ      8

#define SCREEN_W     240
#define SCREEN_H     160

// Fixed-point 24.8
#define FX_SHIFT 8
#define FX_ONE   (1<<FX_SHIFT)
#define TO_FX(n) ((n)<<FX_SHIFT)
#define FX_TO_I(x) ((x)>>FX_SHIFT)

static inline int clampi(int v, int lo, int hi) { return (v<lo)?lo : (v>hi)?hi : v; }

// Physics tuned for “floaty”
enum
{
	PLAYER_W = 16,
	PLAYER_H = 16,

	MAX_JUMPS = 5,

	ACCEL_X   = (FX_ONE/6),   // ~0.166 px/frame^2
	FRICTION  = (FX_ONE/10),  // ~0.1
	MAX_VX    = (FX_ONE*2),   // 2 px/frame

	JUMP_VY   = -(FX_ONE*3),  // -3 px/frame
	GRAVITY   = (FX_ONE/6),   // ~0.166 px/frame^2
	GRAVITY_FLOAT = (FX_ONE/16), // slower when holding A and falling
	MAX_VY    = (FX_ONE*4)    // 4 px/frame
};

// ----------------------------- Level ----------------------------------

// Collision tiles
// 0 = sky (non-solid)
// 1 = grass (solid)
// 2 = brick (solid)
static u8  g_level_col[MAP_W_TILES * MAP_H_TILES];
static u16 g_level_se [MAP_W_TILES * MAP_H_TILES];

// BG tiles (4bpp) we’ll generate: 0 sky, 1 grass, 2 brick, 3 decorative stripe
static TILE g_bg_tiles[4];

// Sprite tile storage (we generate 8 tiles: player 0..3, enemy 4..7)
static TILE g_obj_tiles[8];

// Palettes (shared-ish look)
static const COLOR g_pal16[16] =
{
	RGB15(0,0,0),      // 0: black
	RGB15(31,31,31),   // 1: white
	RGB15(4,4,6),      // 2: dark outline
	RGB15(31,18,22),   // 3: pink (player)
	RGB15(31,10,12),   // 4: red-ish (cheeks / accent)
	RGB15(10,18,31),   // 5: sky blue
	RGB15(8,22,10),    // 6: grass green
	RGB15(18,10,4),    // 7: dirt brown
	RGB15(18,18,18),   // 8: gray (brick)
	RGB15(31,24,10),   // 9: yellow (enemy)
	RGB15(0,0,0),      // 10
	RGB15(0,0,0),      // 11
	RGB15(0,0,0),      // 12
	RGB15(0,0,0),      // 13
	RGB15(0,0,0),      // 14
	RGB15(0,0,0)       // 15
};

// ----------------------------- Entities -------------------------------

typedef struct Player
{
	s32 x, y;      // fx
	s32 vx, vy;    // fx
	int facing;    // -1 left, +1 right
	int on_ground;
	int jumps_left;
	int inhale_cooldown; // frames
} Player;

typedef struct Enemy
{
	s32 x, y;   // fx
	s32 vx;     // fx
	int alive;
} Enemy;

static Player g_plr;
static Enemy  g_enemies[3];

static int g_cam_x = 0;

// OAM
static OBJ_ATTR obj_buffer[128];

// ----------------------------- 4bpp Helpers ---------------------------

static void tile_fill_solid(TILE *t, u8 pal_idx)
{
	// Each byte stores 2 pixels (low nibble = even x, high nibble = odd x)
	// Solid color byte: (i | i<<4) = i*0x11
	u32 row = (u32)pal_idx * 0x11111111u;
	for(int y=0; y<8; y++)
		t->data[y] = row;
}

static void tile_fill_checker(TILE *t, u8 a, u8 b)
{
	for(int y=0; y<8; y++)
	{
		u32 row = 0;
		for(int x=0; x<8; x+=2)
		{
			u8 p0 = ((x+y)&1) ? a : b;
			u8 p1 = (((x+1)+y)&1) ? a : b;
			u8 byte = (p0 & 0xF) | ((p1 & 0xF)<<4);
			row |= (u32)byte << ((x/2)*8);
		}
		t->data[y] = row;
	}
}

static void tile_from_img8x8_4bpp(TILE *out, const u8 *img, int stride)
{
	// img: 8x8 pixels, each pixel is 0..15
	for(int y=0; y<8; y++)
	{
		u32 row = 0;
		for(int x=0; x<8; x+=2)
		{
			u8 p0 = img[y*stride + x] & 0xF;
			u8 p1 = img[y*stride + (x+1)] & 0xF;
			u8 byte = (p0) | (p1<<4);
			row |= (u32)byte << ((x/2)*8);
		}
		out->data[y] = row;
	}
}

static void gen_circle_sprite_16x16(u8 *dst16x16, u8 fill, u8 outline, u8 eye, u8 cheek)
{
	// Very simple “Kirby-ish” circle: fill with outline ring
	// Pixels are palette indices 0..15. 0 is transparent for OBJ.
	for(int y=0; y<16; y++)
	for(int x=0; x<16; x++)
		dst16x16[y*16+x] = 0;

	// Circle center (7.5, 7.5)
	for(int y=0; y<16; y++)
	{
		for(int x=0; x<16; x++)
		{
			int dx = x*2 - 15;
			int dy = y*2 - 15;
			int d2 = dx*dx + dy*dy;
			// radii in this doubled coordinate system
			// outer ~7.5 -> r^2 ~ (15)^2 = 225
			// inner ~6.5 -> r^2 ~ (13)^2 = 169
			if(d2 <= 225) dst16x16[y*16+x] = outline;
			if(d2 <= 169) dst16x16[y*16+x] = fill;
		}
	}

	// Eyes (two dots)
	dst16x16[6*16 + 6] = eye;
	dst16x16[6*16 + 10] = eye;

	// Cheeks
	dst16x16[9*16 + 5] = cheek;
	dst16x16[9*16 + 11] = cheek;
}

static void upload_obj_16x16_as_4tiles(int base_tile_id, const u8 *img16x16)
{
	// Split 16x16 into four 8x8 tiles:
	// (0,0)->base, (8,0)->base+1, (0,8)->base+2, (8,8)->base+3
	u8 tmp[8*8];

	// top-left
	for(int y=0; y<8; y++) for(int x=0; x<8; x++) tmp[y*8+x] = img16x16[(y)*16 + (x)];
	tile_from_img8x8_4bpp(&g_obj_tiles[base_tile_id+0], tmp, 8);

	// top-right
	for(int y=0; y<8; y++) for(int x=0; x<8; x++) tmp[y*8+x] = img16x16[(y)*16 + (x+8)];
	tile_from_img8x8_4bpp(&g_obj_tiles[base_tile_id+1], tmp, 8);

	// bottom-left
	for(int y=0; y<8; y++) for(int x=0; x<8; x++) tmp[y*8+x] = img16x16[(y+8)*16 + (x)];
	tile_from_img8x8_4bpp(&g_obj_tiles[base_tile_id+2], tmp, 8);

	// bottom-right
	for(int y=0; y<8; y++) for(int x=0; x<8; x++) tmp[y*8+x] = img16x16[(y+8)*16 + (x+8)];
	tile_from_img8x8_4bpp(&g_obj_tiles[base_tile_id+3], tmp, 8);
}

// ----------------------------- Level Build ----------------------------

static u8 get_col_tile(int tx, int ty)
{
	if(tx < 0 || ty < 0 || tx >= MAP_W_TILES || ty >= MAP_H_TILES)
		return 1; // treat out-of-bounds as solid
	return g_level_col[ty*MAP_W_TILES + tx];
}

static int is_solid(u8 t)
{
	return (t == 1 || t == 2);
}

static void build_level(void)
{
	// Clear to sky
	for(int i=0; i<MAP_W_TILES*MAP_H_TILES; i++)
		g_level_col[i] = 0;

	// Ground band
	for(int y=24; y<MAP_H_TILES; y++)
		for(int x=0; x<MAP_W_TILES; x++)
			g_level_col[y*MAP_W_TILES + x] = 1;

	// Some platforms (brick)
	for(int x=10; x<20; x++)
		g_level_col[18*MAP_W_TILES + x] = 2;

	for(int x=28; x<38; x++)
		g_level_col[14*MAP_W_TILES + x] = 2;

	for(int x=44; x<54; x++)
		g_level_col[20*MAP_W_TILES + x] = 2;

	// A couple columns
	for(int y=20; y<24; y++)
		g_level_col[y*MAP_W_TILES + 22] = 2;

	for(int y=16; y<24; y++)
		g_level_col[y*MAP_W_TILES + 40] = 2;

	// Convert to screen entries (tile id == collision id for this demo)
	for(int y=0; y<MAP_H_TILES; y++)
	{
		for(int x=0; x<MAP_W_TILES; x++)
		{
			u8 tid = g_level_col[y*MAP_W_TILES + x];
			// Make sky tile id 0 show blue; grass is 1, brick is 2
			g_level_se[y*MAP_W_TILES + x] = (u16)tid;
		}
	}
}

// ----------------------------- Collision ------------------------------

static void player_resolve_x(Player *p)
{
	int px = FX_TO_I(p->x);
	int py = FX_TO_I(p->y);

	int left   = px;
	int right  = px + PLAYER_W - 1;
	int top    = py;
	int bottom = py + PLAYER_H - 1;

	// Sample at two y positions (top+2, bottom-2)
	int sy1 = top + 2;
	int sy2 = bottom - 2;

	int tx_left  = left / TILE_SZ;
	int tx_right = right / TILE_SZ;
	int ty1 = sy1 / TILE_SZ;
	int ty2 = sy2 / TILE_SZ;

	// If moving right, check right side; if moving left, check left side
	if(p->vx > 0)
	{
		if(is_solid(get_col_tile(tx_right, ty1)) || is_solid(get_col_tile(tx_right, ty2)))
		{
			int new_right = tx_right*TILE_SZ - 1;
			px = new_right - (PLAYER_W - 1);
			p->x = TO_FX(px);
			p->vx = 0;
		}
	}
	else if(p->vx < 0)
	{
		if(is_solid(get_col_tile(tx_left, ty1)) || is_solid(get_col_tile(tx_left, ty2)))
		{
			int new_left = (tx_left+1)*TILE_SZ;
			px = new_left;
			p->x = TO_FX(px);
			p->vx = 0;
		}
	}
}

static void player_resolve_y(Player *p)
{
	int px = FX_TO_I(p->x);
	int py = FX_TO_I(p->y);

	int left   = px;
	int right  = px + PLAYER_W - 1;
	int top    = py;
	int bottom = py + PLAYER_H - 1;

	// Sample at two x positions (left+2, right-2)
	int sx1 = left + 2;
	int sx2 = right - 2;

	int ty_top    = top / TILE_SZ;
	int ty_bottom = bottom / TILE_SZ;
	int tx1 = sx1 / TILE_SZ;
	int tx2 = sx2 / TILE_SZ;

	p->on_ground = 0;

	if(p->vy > 0) // falling, check bottom
	{
		if(is_solid(get_col_tile(tx1, ty_bottom)) || is_solid(get_col_tile(tx2, ty_bottom)))
		{
			int new_bottom = ty_bottom*TILE_SZ - 1;
			py = new_bottom - (PLAYER_H - 1);
			p->y = TO_FX(py);
			p->vy = 0;
			p->on_ground = 1;
			p->jumps_left = MAX_JUMPS;
		}
	}
	else if(p->vy < 0) // rising, check top
	{
		if(is_solid(get_col_tile(tx1, ty_top)) || is_solid(get_col_tile(tx2, ty_top)))
		{
			int new_top = (ty_top+1)*TILE_SZ;
			py = new_top;
			p->y = TO_FX(py);
			p->vy = 0;
		}
	}
}

// ----------------------------- Enemy ----------------------------------

static void enemy_update(Enemy *e)
{
	if(!e->alive) return;

	// Basic patrol with wall/edge detection
	int ex = FX_TO_I(e->x);
	int ey = FX_TO_I(e->y);

	// Apply horizontal move
	e->x += e->vx;

	// Collide with walls (simple: if solid at left/right side at feet, reverse)
	ex = FX_TO_I(e->x);

	int feet_y = ey + 15;
	int tx_front = (e->vx > 0) ? ((ex + 15) / TILE_SZ) : (ex / TILE_SZ);
	int ty_feet  = feet_y / TILE_SZ;

	// Wall in front?
	if(is_solid(get_col_tile(tx_front, ty_feet)) || is_solid(get_col_tile(tx_front, (ey+8)/TILE_SZ)))
	{
		e->vx = -e->vx;
	}

	// Edge detection: check tile below front foot
	int tx_foot = (e->vx > 0) ? ((ex + 15) / TILE_SZ) : (ex / TILE_SZ);
	int ty_below = (feet_y + 1) / TILE_SZ;
	if(!is_solid(get_col_tile(tx_foot, ty_below)))
	{
		e->vx = -e->vx;
	}

	// Clamp within world bounds
	int world_px_w = MAP_W_TILES * TILE_SZ;
	ex = clampi(FX_TO_I(e->x), 0, world_px_w - 16);
	e->x = TO_FX(ex);
}

// ----------------------------- Player Update --------------------------

static void player_update(Player *p)
{
	// Horizontal input
	int left  = key_is_down(KEY_LEFT);
	int right = key_is_down(KEY_RIGHT);

	if(left && !right)
	{
		p->vx -= ACCEL_X;
		p->facing = -1;
	}
	else if(right && !left)
	{
		p->vx += ACCEL_X;
		p->facing = +1;
	}
	else
	{
		// friction
		if(p->vx > 0) { p->vx -= FRICTION; if(p->vx < 0) p->vx = 0; }
		if(p->vx < 0) { p->vx += FRICTION; if(p->vx > 0) p->vx = 0; }
	}

	p->vx = clampi(p->vx, -MAX_VX, +MAX_VX);

	// Jump (multi-jump)
	if(key_hit(KEY_A) && p->jumps_left > 0)
	{
		p->vy = JUMP_VY;
		p->jumps_left--;
		p->on_ground = 0;
	}

	// Gravity; floaty when holding A while falling
	s32 g = GRAVITY;
	if(key_is_down(KEY_A) && p->vy > 0)
		g = GRAVITY_FLOAT;

	p->vy += g;
	p->vy = clampi(p->vy, -MAX_VY, +MAX_VY);

	// Move + resolve X then Y
	p->x += p->vx;
	player_resolve_x(p);

	p->y += p->vy;
	player_resolve_y(p);

	// Clamp to world bounds
	int world_px_w = MAP_W_TILES * TILE_SZ;
	int world_px_h = MAP_H_TILES * TILE_SZ;

	int px = clampi(FX_TO_I(p->x), 0, world_px_w - PLAYER_W);
	int py = clampi(FX_TO_I(p->y), 0, world_px_h - PLAYER_H);
	p->x = TO_FX(px);
	p->y = TO_FX(py);

	if(p->inhale_cooldown > 0) p->inhale_cooldown--;
}

static void try_inhale(Player *p)
{
	if(p->inhale_cooldown > 0) return;
	if(!key_hit(KEY_B)) return;

	int px = FX_TO_I(p->x);
	int py = FX_TO_I(p->y);

	// Simple range box in front of player
	int range_x0 = (p->facing > 0) ? (px + 10) : (px - 40);
	int range_x1 = (p->facing > 0) ? (px + 40) : (px - 10);
	int range_y0 = py - 8;
	int range_y1 = py + 24;

	for(int i=0; i<(int)(sizeof(g_enemies)/sizeof(g_enemies[0])); i++)
	{
		Enemy *e = &g_enemies[i];
		if(!e->alive) continue;

		int ex = FX_TO_I(e->x);
		int ey = FX_TO_I(e->y);

		int in_x = (ex+15 >= range_x0) && (ex <= range_x1);
		int in_y = (ey+15 >= range_y0) && (ey <= range_y1);

		if(in_x && in_y)
		{
			e->alive = 0; // “ate” it
			p->inhale_cooldown = 12;
			break;
		}
	}
}

// ----------------------------- Render ---------------------------------

static void update_camera(void)
{
	int world_px_w = MAP_W_TILES * TILE_SZ;
	int px = FX_TO_I(g_plr.x);

	int target = px - (SCREEN_W/2);
	g_cam_x = clampi(target, 0, world_px_w - SCREEN_W);

	REG_BG0HOFS = (u16)g_cam_x;
	REG_BG0VOFS = 0;
}

static void render_sprites(void)
{
	// Player sprite
	int px = FX_TO_I(g_plr.x) - g_cam_x;
	int py = FX_TO_I(g_plr.y);

	// Slight visual when inhaling: flash palette bank? (kept simple)
	obj_set_attr(&obj_buffer[0],
		ATTR0_SQUARE,
		ATTR1_SIZE_16,
		ATTR2_PALBANK(0) | ATTR2_ID(0)); // player tiles start at 0

	obj_set_pos(&obj_buffer[0], px, py);

	// Enemies
	for(int i=0; i<3; i++)
	{
		int oam_id = 1 + i;
		if(!g_enemies[i].alive)
		{
			obj_hide(&obj_buffer[oam_id]);
			continue;
		}

		int ex = FX_TO_I(g_enemies[i].x) - g_cam_x;
		int ey = FX_TO_I(g_enemies[i].y);

		obj_set_attr(&obj_buffer[oam_id],
			ATTR0_SQUARE,
			ATTR1_SIZE_16,
			ATTR2_PALBANK(0) | ATTR2_ID(4)); // enemy tiles start at 4

		obj_set_pos(&obj_buffer[oam_id], ex, ey);
	}

	// Hide the rest
	for(int i=4; i<128; i++)
		obj_hide(&obj_buffer[i]);

	oam_copy(oam_mem, obj_buffer, 128);
}

// ----------------------------- Init -----------------------------------

static void init_gfx(void)
{
	// Mode 0, BG0 on, sprites on, 1D obj mapping
	REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

	// BG0: 4bpp tiles, charblock 0, screenblock 30, size 64x32
	REG_BG0CNT = BG_CBB(0) | BG_SBB(30) | BG_4BPP | BG_REG_64x32 | BG_PRIO(1);

	// Palettes
	memcpy16(pal_bg_mem,  g_pal16, 16);
	memcpy16(pal_obj_mem, g_pal16, 16);

	// Generate BG tiles
	// 0: sky (blue)
	tile_fill_solid(&g_bg_tiles[0], 5);

	// 1: grass (green with a stripe)
	tile_fill_solid(&g_bg_tiles[1], 6);
	// add a darker top line by overwriting first row with “outline”
	g_bg_tiles[1].data[0] = (u32)2 * 0x11111111u;

	// 2: brick (checker gray)
	tile_fill_checker(&g_bg_tiles[2], 8, 2);

	// 3: decorative stripe (unused by collision)
	tile_fill_checker(&g_bg_tiles[3], 5, 1);

	// Copy BG tiles into charblock 0 (tile_mem[0])
	memcpy32(&tile_mem[0][0], g_bg_tiles, sizeof(g_bg_tiles)/4);

	// Build + upload map (64x32 entries = 2048; spans two screen blocks starting at 30)
	build_level();
	memcpy16((u16*)se_mem[30], g_level_se, MAP_W_TILES*MAP_H_TILES);

	// Generate OBJ tiles (player + enemy) as circles
	u8 img[16*16];

	// Player: pink fill(3), outline(2), eyes(0 or 2?), cheeks(4)
	// For visibility, use black(0 is transparent!) so eye uses 2 (dark outline).
	gen_circle_sprite_16x16(img, 3, 2, 2, 4);
	upload_obj_16x16_as_4tiles(0, img);

	// Enemy: yellow fill(9), outline(2), eyes(2), cheeks(4)
	gen_circle_sprite_16x16(img, 9, 2, 2, 4);
	upload_obj_16x16_as_4tiles(4, img);

	// Copy OBJ tiles to charblock 4 (OBJ VRAM)
	memcpy32(&tile_mem[4][0], g_obj_tiles, sizeof(g_obj_tiles)/4);

	// Init OAM buffer hidden
	oam_init(obj_buffer, 128);
}

static void init_game(void)
{
	g_plr.x = TO_FX(24);
	g_plr.y = TO_FX(24*8 - 16 - 1); // on ground band top
	g_plr.vx = g_plr.vy = 0;
	g_plr.facing = +1;
	g_plr.on_ground = 0;
	g_plr.jumps_left = MAX_JUMPS;
	g_plr.inhale_cooldown = 0;

	// Enemies placed on platforms/ground
	g_enemies[0] = (Enemy){ TO_FX(12*8), TO_FX(24*8 - 16), TO_FX(1)/2, 1 };
	g_enemies[1] = (Enemy){ TO_FX(16*8), TO_FX(18*8 - 16), TO_FX(1)/2, 1 };
	g_enemies[2] = (Enemy){ TO_FX(48*8), TO_FX(20*8 - 16), -TO_FX(1)/2, 1 };
}

// ----------------------------- Main -----------------------------------

int main(void)
{
	irq_init(NULL);
	irq_add(II_VBLANK, NULL);

	init_gfx();
	init_game();

	while(1)
	{
		vid_vsync();
		key_poll();

		player_update(&g_plr);
		try_inhale(&g_plr);

		for(int i=0; i<3; i++)
			enemy_update(&g_enemies[i]);

		update_camera();
		render_sprites();
	}

	// not reached
	return 0;
}
