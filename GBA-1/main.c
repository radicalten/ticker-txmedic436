// mario3_like_tonc.c
// Single-file, asset-free, Mario 3–style side-scrolling platformer *demo* for GBA using tonc.
// Controls: D-Pad = move, A = jump
//
// Build (example):
//   arm-none-eabi-gcc -mthumb-interwork -mthumb -O2 -Wall -Wextra -I<path-to-tonc> mario3_like_tonc.c -o game.elf
//   (Linking depends on your tonc setup; many templates link libtonc + gba crt0 automatically.)

#include <tonc.h>
#include <string.h>
#include <stdbool.h>

#define SCREEN_W 240
#define SCREEN_H 160

#define FIX_SHIFT 8
#define FIX(x) ((x) << FIX_SHIFT)

#define MAP_W 64     // tiles
#define MAP_H 32     // tiles
#define TILE_SZ 8    // pixels

// Tile indices in our BG tileset
enum {
	TILE_SKY   = 0,
	TILE_GROUND= 1,
	TILE_BRICK = 2,
	TILE_COIN  = 3,

	BG_TILE_COUNT = 4
};

// Player
#define PL_W 12
#define PL_H 16

// Physics (Q8.8)
#define PL_MAX_SPD   (FIX(2))    // 2 px/frame
#define PL_JUMP_V    (-(FIX(5))) // -5 px/frame
#define PL_GRAV      (80)        // ~0.3125 px/frame^2 (80/256)
#define PL_TERM_V    (FIX(6))    // 6 px/frame

static u8 g_world[MAP_H][MAP_W];        // collision + coin state (tile indices)
static OBJ_ATTR g_oam[128];

static int g_cam_x = 0;

typedef struct Player {
	int x, y;      // Q8.8
	int vx, vy;    // Q8.8
	int on_ground; // bool-ish
} Player;

static Player g_pl;

// --- Utility ----------------------------------------------------------------

static inline int clampi(int v, int lo, int hi)
{
	if(v < lo) return lo;
	if(v > hi) return hi;
	return v;
}

static inline int tile_at(int tx, int ty)
{
	if((unsigned)tx >= MAP_W || (unsigned)ty >= MAP_H)
		return TILE_SKY;
	return g_world[ty][tx];
}

static inline bool is_solid(int t)
{
	return (t == TILE_GROUND || t == TILE_BRICK);
}

static inline void bg_set_tile(int tx, int ty, int tile_id)
{
	if((unsigned)tx >= MAP_W || (unsigned)ty >= MAP_H)
		return;

	g_world[ty][tx] = (u8)tile_id;

	// Write into the correct screenblock(s) for a 64x32 map:
	// BG_SIZE1 => 64x32 => 2 screenblocks side-by-side.
	int sbb = 31 + (tx >> 5);            // 31 or 32
	int idx = (ty * 32) + (tx & 31);     // within that 32x32 block
	se_mem[sbb][idx] = (SCR_ENTRY)tile_id;
}

// --- Procedural tile/sprite generation --------------------------------------

static inline u32 nibble_fill_u32(int color_idx_0_to_15)
{
	return (u32)(color_idx_0_to_15 & 0xF) * 0x11111111u;
}

static void make_bg_tiles(u32 *out_tiles /* BG_TILE_COUNT*8 u32 */)
{
	// Each 4bpp 8x8 tile is 32 bytes = 8 u32 words (one u32 per row of 8 pixels).
	// BG palette indices:
	//  0 sky, 1 brown, 2 green, 3 yellow, 4 dark

	// TILE_SKY: all 0 (sky color via pal_bg_mem[0])
	for(int r=0; r<8; r++)
		out_tiles[TILE_SKY*8 + r] = nibble_fill_u32(0);

	// TILE_GROUND: top rows green, rest brown with a tiny speckle
	for(int r=0; r<8; r++)
	{
		u32 row = (r < 2) ? nibble_fill_u32(2) : nibble_fill_u32(1);
		if(r == 5) row = 0x11114111u; // small darker dot
		out_tiles[TILE_GROUND*8 + r] = row;
	}

	// TILE_BRICK: simple checker-ish pattern using brown + dark
	for(int r=0; r<8; r++)
	{
		// Alternate between 1 and 4 per pixel: pattern 14141414 / 41414141
		u32 row = (r & 1) ? 0x14141414u : 0x41414141u;
		out_tiles[TILE_BRICK*8 + r] = row;
	}

	// TILE_COIN: 8x8 coin circle in yellow (3) with dark outline (4)
	// Hand-ish pattern, readable at 8x8:
	// ..4444..
	// .433334.
	// 43333334
	// 43333334
	// 43333334
	// 43333334
	// .433334.
	// ..4444..
	static const u8 coin[8][8]={
		{0,0,4,4,4,4,0,0},
		{0,4,3,3,3,3,4,0},
		{4,3,3,3,3,3,3,4},
		{4,3,3,3,3,3,3,4},
		{4,3,3,3,3,3,3,4},
		{4,3,3,3,3,3,3,4},
		{0,4,3,3,3,3,4,0},
		{0,0,4,4,4,4,0,0},
	};

	for(int y=0; y<8; y++)
	{
		u32 row=0;
		for(int x=0; x<8; x++)
			row |= (u32)(coin[y][x] & 0xF) << (4*x);
		out_tiles[TILE_COIN*8 + y] = row;
	}
}

static void make_player_sprite_16x16(u32 *out_tiles /* 4 tiles * 8 u32 = 32 u32 */)
{
	// OBJ palette indices:
	//  0 transparent, 1 red, 2 blue, 3 skin, 4 white, 5 black

	// Build a simple 16x16 dude procedurally into pixels[16][16]
	u8 px[16][16];
	memset(px, 0, sizeof(px));

	for(int y=0; y<16; y++)
	{
		for(int x=0; x<16; x++)
		{
			u8 c = 0;

			// hat
			if(y <= 3 && x >= 4 && x <= 11) c = 1;

			// face
			if(y >= 4 && y <= 6 && x >= 6 && x <= 9) c = 3;

			// body
			if(y >= 7 && y <= 11 && x >= 5 && x <= 10) c = 2;

			// arms (white gloves-ish)
			if(y >= 8 && y <= 9 && (x == 4 || x == 11)) c = 4;

			// legs
			if(y >= 12 && y <= 14 && (x == 6 || x == 9)) c = 2;

			// boots
			if(y == 15 && (x >= 5 && x <= 7)) c = 5;
			if(y == 15 && (x >= 8 && x <= 10)) c = 5;

			// outline (black) around nonzero pixels
			if(c != 0)
			{
				bool edge=false;
				for(int oy=-1; oy<=1; oy++)
				for(int ox=-1; ox<=1; ox++)
				{
					int nx=x+ox, ny=y+oy;
					if((unsigned)nx >= 16 || (unsigned)ny >= 16) { edge=true; continue; }
					// If neighbor is empty and we're filled, we might be edge
					// We'll decide later after we have base; quick approach:
				}
			}

			px[y][x] = c;
		}
	}

	// Add a black outline where a filled pixel touches transparency
	for(int y=0; y<16; y++)
	{
		for(int x=0; x<16; x++)
		{
			if(px[y][x] == 0) continue;
			bool touches_air=false;
			static const int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
			for(int i=0; i<4; i++)
			{
				int nx=x+dirs[i][0], ny=y+dirs[i][1];
				if((unsigned)nx >= 16 || (unsigned)ny >= 16) { touches_air=true; continue; }
				if(px[ny][nx] == 0) touches_air=true;
			}
			if(touches_air && px[y][x] != 5)  // keep boots black as-is
			{
				// Put black on the perimeter by darkening some edge pixels:
				// (Simple trick: if you're on extreme boundary of the shape)
				if(x==0 || x==15 || y==0 || y==15) px[y][x]=5;
				// else keep as-is; outline stays subtle
			}
		}
	}

	// Pack into 4 tiles (8x8) with 1D mapping:
	// tile 0: (0..7, 0..7), tile 1: (8..15, 0..7)
	// tile 2: (0..7, 8..15), tile 3: (8..15, 8..15)
	for(int t=0; t<4; t++)
	{
		int ox = (t & 1) ? 8 : 0;
		int oy = (t & 2) ? 8 : 0;

		for(int y=0; y<8; y++)
		{
			u32 row=0;
			for(int x=0; x<8; x++)
			{
				u8 c = px[oy+y][ox+x] & 0xF;
				row |= (u32)c << (4*x);
			}
			out_tiles[t*8 + y] = row;
		}
	}
}

// --- World generation --------------------------------------------------------

static void world_build(void)
{
	// Fill with sky
	for(int y=0; y<MAP_H; y++)
		for(int x=0; x<MAP_W; x++)
			bg_set_tile(x,y, TILE_SKY);

	// Ground (4 tiles thick) with a gap
	for(int x=0; x<MAP_W; x++)
	{
		bool gap = (x >= 24 && x <= 27);
		for(int y=28; y<MAP_H; y++)
			bg_set_tile(x,y, gap ? TILE_SKY : TILE_GROUND);
	}

	// A few brick platforms
	for(int x=10; x<=16; x++) bg_set_tile(x, 22, TILE_BRICK);
	for(int x=34; x<=40; x++) bg_set_tile(x, 18, TILE_BRICK);
	for(int x=44; x<=50; x++) bg_set_tile(x, 24, TILE_BRICK);

	// Coins above platforms
	for(int x=11; x<=15; x++) bg_set_tile(x, 21, TILE_COIN);
	for(int x=35; x<=39; x++) bg_set_tile(x, 17, TILE_COIN);
	for(int x=45; x<=49; x++) bg_set_tile(x, 23, TILE_COIN);

	// A little staircase
	bg_set_tile(6, 27, TILE_BRICK);
	bg_set_tile(7, 26, TILE_BRICK);
	bg_set_tile(8, 25, TILE_BRICK);
	bg_set_tile(9, 24, TILE_BRICK);
}

// --- Collision + movement ----------------------------------------------------

static void player_collect_coins(Player *p)
{
	int x = p->x >> FIX_SHIFT;
	int y = p->y >> FIX_SHIFT;

	int left   = x;
	int right  = x + PL_W - 1;
	int top    = y;
	int bottom = y + PL_H - 1;

	int tx0 = left  >> 3;
	int tx1 = right >> 3;
	int ty0 = top   >> 3;
	int ty1 = bottom>> 3;

	for(int ty=ty0; ty<=ty1; ty++)
	{
		for(int tx=tx0; tx<=tx1; tx++)
		{
			if(tile_at(tx,ty) == TILE_COIN)
				bg_set_tile(tx,ty, TILE_SKY);
		}
	}
}

static void player_step(Player *p)
{
	// Input -> horizontal speed (snappy, arcade-y)
	if(key_is_down(KEY_LEFT))      p->vx = -PL_MAX_SPD;
	else if(key_is_down(KEY_RIGHT))p->vx =  PL_MAX_SPD;
	else                          p->vx = 0;

	// Jump
	if(key_hit(KEY_A) && p->on_ground)
	{
		p->vy = PL_JUMP_V;
		p->on_ground = 0;
	}

	// Gravity
	p->vy += PL_GRAV;
	if(p->vy > PL_TERM_V) p->vy = PL_TERM_V;

	// --- Horizontal move + collide ---
	int newx = p->x + p->vx;
	int xpix = newx >> FIX_SHIFT;
	int ypix = p->y >> FIX_SHIFT;

	if(p->vx > 0)
	{
		int right = xpix + PL_W - 1;
		int tx = right >> 3;
		int top = ypix + 1;
		int bottom = ypix + PL_H - 1;
		int ty0 = top >> 3, ty1 = bottom >> 3;

		for(int ty=ty0; ty<=ty1; ty++)
		{
			if(is_solid(tile_at(tx,ty)))
			{
				// clamp to left of tile
				int tile_left = (tx << 3);
				newx = (tile_left - PL_W) << FIX_SHIFT;
				p->vx = 0;
				break;
			}
		}
	}
	else if(p->vx < 0)
	{
		int left = xpix;
		int tx = left >> 3;
		int top = ypix + 1;
		int bottom = ypix + PL_H - 1;
		int ty0 = top >> 3, ty1 = bottom >> 3;

		for(int ty=ty0; ty<=ty1; ty++)
		{
			if(is_solid(tile_at(tx,ty)))
			{
				// clamp to right of tile
				int tile_right = (tx << 3) + 8;
				newx = (tile_right) << FIX_SHIFT;
				p->vx = 0;
				break;
			}
		}
	}
	p->x = newx;

	// --- Vertical move + collide ---
	int newy = p->y + p->vy;
	xpix = p->x >> FIX_SHIFT;
	int newypix = newy >> FIX_SHIFT;

	p->on_ground = 0;

	if(p->vy > 0)
	{
		int bottom = newypix + PL_H - 1;
		int ty = bottom >> 3;

		int left  = xpix + 1;
		int right = xpix + PL_W - 2;
		int tx0 = left >> 3;
		int tx1 = right >> 3;

		for(int tx=tx0; tx<=tx1; tx++)
		{
			if(is_solid(tile_at(tx,ty)))
			{
				int tile_top = (ty << 3);
				newy = (tile_top - PL_H) << FIX_SHIFT;
				p->vy = 0;
				p->on_ground = 1;
				break;
			}
		}
	}
	else if(p->vy < 0)
	{
		int top = newypix;
		int ty = top >> 3;

		int left  = xpix + 1;
		int right = xpix + PL_W - 2;
		int tx0 = left >> 3;
		int tx1 = right >> 3;

		for(int tx=tx0; tx<=tx1; tx++)
		{
			if(is_solid(tile_at(tx,ty)))
			{
				int tile_bottom = (ty << 3) + 8;
				newy = (tile_bottom) << FIX_SHIFT;
				p->vy = 0;
				break;
			}
		}
	}
	p->y = newy;

	// Collect coins after resolving
	player_collect_coins(p);

	// Fell off the world? Reset.
	int yint = p->y >> FIX_SHIFT;
	if(yint > MAP_H*TILE_SZ)
	{
		p->x = FIX(16);
		p->y = FIX(16);
		p->vx = p->vy = 0;
		p->on_ground = 0;
	}
}

// --- Main -------------------------------------------------------------------

int main(void)
{
	// Display: Mode 0 (tiled), BG0 on, OBJ on, 1D OBJ tile mapping
	REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

	// BG0: charblock 0, screenblock 31, 4bpp, size 64x32
	REG_BG0CNT = BG_CBB(0) | BG_SBB(31) | BG_4BPP | BG_SIZE1 | BG_PRIO(1);

	// Palettes
	// BG palette: index 0 is sky; BG does not treat index 0 as transparent.
	pal_bg_mem[0] = RGB15(18, 25, 31); // sky
	pal_bg_mem[1] = RGB15(14,  8,  2); // brown
	pal_bg_mem[2] = RGB15( 0, 20,  0); // green
	pal_bg_mem[3] = RGB15(31, 24,  0); // yellow
	pal_bg_mem[4] = RGB15( 6,  6,  6); // dark

	// OBJ palette: index 0 is transparent for sprites.
	pal_obj_mem[0] = RGB15(0,0,0);     // transparent
	pal_obj_mem[1] = RGB15(31, 0, 0);  // red
	pal_obj_mem[2] = RGB15(0, 0, 31);  // blue
	pal_obj_mem[3] = RGB15(31, 24, 18);// skin
	pal_obj_mem[4] = RGB15(31, 31, 31);// white
	pal_obj_mem[5] = RGB15(0, 0, 0);   // black

	// Build BG tiles into charblock 0
	u32 bg_tiles[BG_TILE_COUNT * 8];
	make_bg_tiles(bg_tiles);
	memcpy32((u32*)tile_mem[0], bg_tiles, sizeof(bg_tiles)/4);

	// Build player sprite tiles into OBJ charblock (tile_mem[4])
	u32 pl_tiles[4 * 8];
	make_player_sprite_16x16(pl_tiles);
	memcpy32((u32*)tile_mem[4], pl_tiles, sizeof(pl_tiles)/4);

	// Init OAM buffer
	oam_init(g_oam, 128);

	// Setup player sprite (16x16, 4bpp, tile id 0 in OBJ charblock)
	obj_set_attr(&g_oam[0],
		ATTR0_SQUARE,
		ATTR1_SIZE_16,
		ATTR2_PALBANK(0) | 0);

	// Build the world + copy map via bg_set_tile
	world_build();

	// Player start
	g_pl.x = FIX(16);
	g_pl.y = FIX(16);
	g_pl.vx = 0;
	g_pl.vy = 0;
	g_pl.on_ground = 0;

	while(1)
	{
		key_poll();

		player_step(&g_pl);

		// Camera follows player X
		int plx = g_pl.x >> FIX_SHIFT;
		int max_cam = MAP_W*TILE_SZ - SCREEN_W;
		g_cam_x = clampi(plx - SCREEN_W/2, 0, max_cam);

		REG_BG0HOFS = (u16)g_cam_x;
		REG_BG0VOFS = 0;

		// Draw player relative to camera
		int screen_x = (g_pl.x >> FIX_SHIFT) - g_cam_x;
		int screen_y = (g_pl.y >> FIX_SHIFT);

		// Hide sprite if way off-screen (optional safety)
		if(screen_x < -PL_W || screen_x > SCREEN_W || screen_y < -PL_H || screen_y > SCREEN_H)
			obj_hide(&g_oam[0]);
		else
		{
			obj_unhide(&g_oam[0], ATTR0_SQUARE);
			obj_set_pos(&g_oam[0], screen_x, screen_y);
		}

		vid_vsync();
		oam_copy(oam_mem, g_oam, 128);
	}

	return 0;
}
