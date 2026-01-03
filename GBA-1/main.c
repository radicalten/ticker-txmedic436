// main.c - Single-screen top-down racer (Super Off Road-ish) for GBA using tonc.
// Controls:  A = accelerate, B = brake, Left/Right = steer, START = reset race
//
// Build: compile/link with tonc (tonc.h + libtonc). No external assets used.

#include <tonc.h>
#include <stdlib.h>
#include <string.h>

#define FIX_SHIFT 8
#define FIX(n)    ((n) << FIX_SHIFT)
#define UNFIX(n)  ((n) >> FIX_SHIFT)

#define SCREEN_W 240
#define SCREEN_H 160

// BG0 map is 32x32 tiles (256x256), but we only use the visible 30x20 area.
#define MAP_W 32
#define MAP_H 32

// Track tile IDs (in BG charblock 0)
enum
{
	T_GRASS = 0,
	T_ROAD  = 1,
	T_CURB  = 2,
	T_START = 3,
};

// Sprite tile IDs (in OBJ charblock; 1D mapping)
// Car uses tiles 0..3 (16x16 made of 2x2 8x8 tiles).
#define OBJ_CAR_TILE_BASE 0
// Lap indicator tiles:
#define OBJ_LAP_EMPTY_TILE 4
#define OBJ_LAP_FULL_TILE  5

#define NUM_CARS 4
#define NUM_AI   3
#define NUM_WP   12
#define LAPS_TO_WIN 3

typedef struct { s16 x, y; } Pt;

typedef enum
{
	SURF_ROAD,
	SURF_CURB,
	SURF_GRASS
} Surface;

typedef struct
{
	s32 x, y;      // Q8.8, car center in pixels
	s32 vx, vy;    // Q8.8, velocity in pixels/frame
	u16 ang;       // 0..511 (full circle) for tonc LUT trig
	u8  palbank;   // OBJ palette bank (0..3)
	u8  aff_id;    // affine matrix index (0..31)
	u8  oam_id;    // OAM entry index
	u8  lap;       // completed laps
	u8  next_wp;   // next waypoint index for progress/laps
} Car;

static OBJ_ATTR obj_buf[128];

static Car cars[NUM_CARS];
static Pt  wps[NUM_WP];

static u8 race_finished = 0;

static inline s32 iabs32(s32 v) { return v < 0 ? -v : v; }

static inline u32 pack8_4bpp(const u8 *p8)
{
	// Packs 8 pixels (each 0..15) into a 32-bit row for 4bpp tiles.
	u32 w = 0;
	for(int i=0;i<8;i++)
		w |= (u32)(p8[i] & 0xF) << (4*i);
	return w;
}

static void tile4_fill(TILE *t, u8 c)
{
	u32 v = (c & 0xF);
	v |= v<<4;
	v |= v<<8;
	v |= v<<16;
	for(int r=0;r<8;r++)
		t->data[r] = v;
}

static void tile4_checker(TILE *t, u8 c0, u8 c1)
{
	for(int r=0;r<8;r++)
	{
		u8 row[8];
		for(int x=0;x<8;x++)
			row[x] = ((x ^ r) & 1) ? c0 : c1;
		t->data[r] = pack8_4bpp(row);
	}
}

static void make_bg_tiles(void)
{
	// BG palette (indices 0..15)
	// 0: dark, 1: grass, 2: road, 3: curb red, 4: curb white, 5: start white, 6: start dark
	pal_bg_mem[0] = RGB15(2,2,3);
	pal_bg_mem[1] = RGB15(3,18,4);
	pal_bg_mem[2] = RGB15(12,12,13);
	pal_bg_mem[3] = RGB15(20,4,4);
	pal_bg_mem[4] = RGB15(28,28,28);
	pal_bg_mem[5] = RGB15(30,30,30);
	pal_bg_mem[6] = RGB15(2,2,2);

	// Tiles in charblock 0
	tile4_fill(&tile_mem[0][T_GRASS], 1);
	tile4_fill(&tile_mem[0][T_ROAD ], 2);
	tile4_checker(&tile_mem[0][T_CURB], 3, 4);
	tile4_checker(&tile_mem[0][T_START], 5, 6);
}

static int inside_ellipse(int dx, int dy, int a, int b)
{
	// Checks (dx^2)/a^2 + (dy^2)/b^2 <= 1 using integer math.
	int aa = a*a;
	int bb = b*b;
	int lhs = dx*dx*bb + dy*dy*aa;
	int rhs = aa*bb;
	return lhs <= rhs;
}

static void build_track_map(void)
{
	// Oval ring track that fits in the visible 30x20 tiles.
	const int cx = 15, cy = 10;     // center in tiles
	const int ao = 13, bo = 8;      // outer radii in tiles
	const int ai = 9,  bi = 5;      // inner radii in tiles

	// Fill everything with grass first.
	for(int y=0;y<MAP_H;y++)
		for(int x=0;x<MAP_W;x++)
			se_mem[31][y*MAP_W + x] = T_GRASS;

	// Draw ring in the visible area (0..29, 0..19)
	for(int y=0;y<20;y++)
	{
		for(int x=0;x<30;x++)
		{
			int dx = x - cx;
			int dy = y - cy;

			int outer  = inside_ellipse(dx, dy, ao, bo);
			int outer2 = inside_ellipse(dx, dy, ao-1, bo-1);
			int inner  = inside_ellipse(dx, dy, ai, bi);
			int inner2 = inside_ellipse(dx, dy, ai+1, bi+1);

			u16 tid = T_GRASS;

			if(outer && !inner)
			{
				// curb bands near outer edge and inner edge
				if(!outer2 || inner2)
					tid = T_CURB;
				else
					tid = T_ROAD;
			}

			se_mem[31][y*MAP_W + x] = tid;
		}
	}

	// Start line near the right side of the oval.
	// (Visual marker; treated as road for driving.)
	int sx = cx + ao - 1; // ~27
	int sy = cy;          // ~10
	for(int yy=sy-1; yy<=sy+1; yy++)
	{
		if(yy>=0 && yy<20 && sx>=0 && sx<30)
			se_mem[31][yy*MAP_W + sx] = T_START;
	}
}

static Surface surface_at_px(int px, int py)
{
	if(px < 0 || py < 0 || px >= SCREEN_W || py >= SCREEN_H)
		return SURF_GRASS;

	int tx = px >> 3;
	int ty = py >> 3;
	if(tx < 0 || ty < 0 || tx >= MAP_W || ty >= MAP_H)
		return SURF_GRASS;

	u16 se = se_mem[31][ty*MAP_W + tx];
	u16 tid = se & 0x03FF;

	if(tid == T_ROAD || tid == T_START) return SURF_ROAD;
	if(tid == T_CURB) return SURF_CURB;
	return SURF_GRASS;
}

static void make_obj_tiles(void)
{
	// OBJ palette banks:
	// index 0 is transparent for OBJ.
	// indices:
	// 1 = body, 2 = window, 3 = outline/tires, 7 = HUD yellow
	const COLOR window = RGB15(8, 14, 30);
	const COLOR outline= RGB15(2, 2, 2);
	const COLOR hud_y  = RGB15(30, 28, 0);

	// Bank 0 (player): blue
	pal_obj_mem[0*16 + 0] = RGB15(0,0,0);
	pal_obj_mem[0*16 + 1] = RGB15(4, 10, 30);
	pal_obj_mem[0*16 + 2] = window;
	pal_obj_mem[0*16 + 3] = outline;
	pal_obj_mem[0*16 + 7] = hud_y;

	// Bank 1: red
	pal_obj_mem[1*16 + 0] = RGB15(0,0,0);
	pal_obj_mem[1*16 + 1] = RGB15(30, 6, 6);
	pal_obj_mem[1*16 + 2] = window;
	pal_obj_mem[1*16 + 3] = outline;

	// Bank 2: green
	pal_obj_mem[2*16 + 0] = RGB15(0,0,0);
	pal_obj_mem[2*16 + 1] = RGB15(6, 26, 8);
	pal_obj_mem[2*16 + 2] = window;
	pal_obj_mem[2*16 + 3] = outline;

	// Bank 3: purple
	pal_obj_mem[3*16 + 0] = RGB15(0,0,0);
	pal_obj_mem[3*16 + 1] = RGB15(22, 8, 28);
	pal_obj_mem[3*16 + 2] = window;
	pal_obj_mem[3*16 + 3] = outline;

	// Build a simple 16x16 "car" sprite facing up (we'll rotate with affine).
	u8 car[16][16];
	memset(car, 0, sizeof(car));

	// Body
	for(int y=2;y<=13;y++)
		for(int x=5;x<=10;x++)
			car[y][x] = 1;

	// Nose
	for(int y=0;y<=2;y++)
		for(int x=7-(2-y); x<=8+(2-y); x++)
			if(x>=0 && x<16) car[y][x] = 1;

	// Tires (outline blocks)
	for(int y=4;y<=12;y+=4)
	{
		car[y][4] = 3; car[y][11] = 3;
		car[y+1][4] = 3; car[y+1][11] = 3;
	}

	// Window
	for(int y=5;y<=7;y++)
		for(int x=6;x<=9;x++)
			car[y][x] = 2;

	// Outline around body (cheap)
	for(int y=1;y<=13;y++)
	{
		for(int x=4;x<=11;x++)
		{
			if(car[y][x]==1)
			{
				if(y>0  && car[y-1][x]==0) car[y-1][x]=3;
				if(y<15 && car[y+1][x]==0) car[y+1][x]=3;
				if(x>0  && car[y][x-1]==0) car[y][x-1]=3;
				if(x<15 && car[y][x+1]==0) car[y][x+1]=3;
			}
		}
	}

	// Pack into 4 OBJ 8x8 tiles (1D)
	for(int ty=0; ty<2; ty++)
	{
		for(int tx=0; tx<2; tx++)
		{
			TILE *t = &tile_mem[4][OBJ_CAR_TILE_BASE + ty*2 + tx];
			for(int r=0;r<8;r++)
			{
				u8 row[8];
				for(int c=0;c<8;c++)
					row[c] = car[ty*8 + r][tx*8 + c];
				t->data[r] = pack8_4bpp(row);
			}
		}
	}

	// Lap indicator tiles (8x8)
	// Empty: yellow border, transparent center
	{
		TILE *t = &tile_mem[4][OBJ_LAP_EMPTY_TILE];
		for(int r=0;r<8;r++)
		{
			u8 row[8];
			for(int c=0;c<8;c++)
			{
				int border = (r==0 || r==7 || c==0 || c==7);
				row[c] = border ? 7 : 0; // 0 transparent for OBJ
			}
			t->data[r] = pack8_4bpp(row);
		}
	}
	// Full: solid yellow
	tile4_fill(&tile_mem[4][OBJ_LAP_FULL_TILE], 7);
}

static void make_waypoints(void)
{
	// Centerline ellipse between inner and outer.
	// Track build uses cx=15,cy=10, outer (13,8), inner (9,5) in tiles.
	// Use approximate centerline radii in pixels.
	const int cx_px = 15*8 + 4;
	const int cy_px = 10*8 + 4;
	const int a_px  = 11*8;
	const int b_px  = 6*8;

	for(int i=0;i<NUM_WP;i++)
	{
		u16 th = (u16)((i * 512) / NUM_WP); // 0..511
		s16 cs = lu_cos(th); // tonc LUT
		s16 sn = lu_sin(th);

		int x = cx_px + (int)((a_px * (s32)cs) >> 12);
		int y = cy_px + (int)((b_px * (s32)sn) >> 12);

		wps[i].x = (s16)x;
		wps[i].y = (s16)y;
	}
}

static void reset_race(void)
{
	race_finished = 0;

	// Start near right side, heading up.
	// Angle for "up" in screen coords: ~270 degrees.
	const u16 ANG_UP = 384;

	// Positions (pixels)
	const int start_x = 212;
	const int start_y = 80;

	for(int i=0;i<NUM_CARS;i++)
	{
		Car *c = &cars[i];
		c->x = FIX(start_x);
		c->y = FIX(start_y + (i-1)*12);
		c->vx = 0;
		c->vy = 0;
		c->ang = ANG_UP;

		c->palbank = (u8)i;
		c->aff_id  = (u8)i;
		c->oam_id  = (u8)i;

		c->lap = 0;
		c->next_wp = 0;
	}

	// Push their next waypoint to something consistent with their start location
	// (so lap counting won’t instantly tick).
	for(int i=0;i<NUM_CARS;i++)
		cars[i].next_wp = 2;
}

static void update_progress(Car *c)
{
	// If close to next waypoint, advance. Lap increments on wrap (NUM_WP-1 -> 0).
	Pt t = wps[c->next_wp];

	int px = UNFIX(c->x);
	int py = UNFIX(c->y);

	int dx = t.x - px;
	int dy = t.y - py;

	int dist2 = dx*dx + dy*dy;
	if(dist2 <= 12*12)
	{
		u8 prev = c->next_wp;
		c->next_wp = (u8)((c->next_wp + 1) % NUM_WP);

		if(prev == (NUM_WP-1) && c->next_wp == 0)
		{
			if(c->lap < 250) c->lap++;
			if(c->lap >= LAPS_TO_WIN) race_finished = 1;
		}
	}
}

static void apply_common_physics(Car *c, int accel_on, int brake_on, int turn_left, int turn_right)
{
	int px = UNFIX(c->x);
	int py = UNFIX(c->y);

	Surface s = surface_at_px(px, py);

	// Turning: less responsive on grass.
	int turn = (s==SURF_GRASS) ? 2 : 4;
	if(turn_left)  c->ang = (u16)((c->ang - turn) & 511);
	if(turn_right) c->ang = (u16)((c->ang + turn) & 511);

	// Accel/brake
	s16 cs = lu_cos(c->ang);
	s16 sn = lu_sin(c->ang);

	// Acceleration strength by surface
	s32 acc = (s==SURF_GRASS) ? FIX(0) + 26 : FIX(0) + 46; // ~0.10 vs ~0.18 px/f^2 in Q8.8

	if(accel_on)
	{
		c->vx += (s32)((cs * acc) >> 12);
		c->vy += (s32)((sn * acc) >> 12);
	}
	if(brake_on)
	{
		// Brake is just opposite accel (stronger on road)
		s32 br = (s==SURF_GRASS) ? (FIX(0) + 20) : (FIX(0) + 36);
		c->vx -= (s32)((cs * br) >> 12);
		c->vy -= (s32)((sn * br) >> 12);
	}

	// Drag by surface (bitshift drag; smaller shift => more drag)
	int drag_shift = (s==SURF_ROAD) ? 5 : (s==SURF_CURB ? 4 : 3);
	c->vx -= (c->vx >> drag_shift);
	c->vy -= (c->vy >> drag_shift);

	// Clamp speed (axis clamp, simple but fine)
	s32 vmax = (s==SURF_GRASS) ? FIX(2) + 80 : (s==SURF_CURB ? FIX(3) : FIX(3) + 80);
	if(c->vx >  vmax) c->vx =  vmax;
	if(c->vx < -vmax) c->vx = -vmax;
	if(c->vy >  vmax) c->vy =  vmax;
	if(c->vy < -vmax) c->vy = -vmax;

	// Integrate
	c->x += c->vx;
	c->y += c->vy;

	// Keep within screen bounds (bounce a bit)
	px = UNFIX(c->x);
	py = UNFIX(c->y);

	if(px < 8)  { c->x = FIX(8);  c->vx = -c->vx/2; }
	if(px > SCREEN_W-9) { c->x = FIX(SCREEN_W-9); c->vx = -c->vx/2; }
	if(py < 8)  { c->y = FIX(8);  c->vy = -c->vy/2; }
	if(py > SCREEN_H-9) { c->y = FIX(SCREEN_H-9); c->vy = -c->vy/2; }

	update_progress(c);
}

static void ai_control(Car *c)
{
	// Steer toward current waypoint using cross/dot with forward vector.
	Pt t = wps[c->next_wp];

	int px = UNFIX(c->x);
	int py = UNFIX(c->y);

	s32 dx = FIX(t.x - px);
	s32 dy = FIX(t.y - py);

	s16 fx = lu_cos(c->ang);
	s16 fy = lu_sin(c->ang);

	s32 cross = (s32)fx*dy - (s32)fy*dx;
	s32 dot   = (s32)fx*dx + (s32)fy*dy;

	int turn_left  = 0;
	int turn_right = 0;

	// With screen coords, this sign convention gives a reasonable result.
	if(cross > 0) turn_right = 1;
	else if(cross < 0) turn_left = 1;

	int accel = (dot > 0);
	int brake = (dot < 0);

	apply_common_physics(c, accel, brake, turn_left, turn_right);
}

static void update_player(Car *c)
{
	key_poll();

	if(race_finished)
	{
		// Let the car roll to a stop.
		apply_common_physics(c, 0, 0, 0, 0);
		return;
	}

	int accel = key_is_down(KEY_A);
	int brake = key_is_down(KEY_B);
	int left  = key_is_down(KEY_LEFT);
	int right = key_is_down(KEY_RIGHT);

	apply_common_physics(c, accel, brake, left, right);
}

static void set_affine_matrix(int aff_id, u16 ang)
{
	// OBJ affine matrix elements are Q8.8.
	// tonc LUT trig is Q12, so shift by 4 to go Q12 -> Q8.8 scale (1.0 = 256).
	s16 cs = lu_cos(ang);
	s16 sn = lu_sin(ang);

	OBJ_AFFINE *oa = &obj_aff_mem[aff_id];
	oa->pa = (s16)(cs >> 4);
	oa->pb = (s16)(-sn >> 4);
	oa->pc = (s16)(sn >> 4);
	oa->pd = (s16)(cs >> 4);
}

static void draw_cars_and_hud(void)
{
	// Car sprites (affine 16x16)
	for(int i=0;i<NUM_CARS;i++)
	{
		Car *c = &cars[i];
		int px = UNFIX(c->x) - 8;
		int py = UNFIX(c->y) - 8;

		OBJ_ATTR *o = &obj_buf[c->oam_id];

		obj_set_attr(o,
			ATTR0_AFF | ATTR0_SQUARE | ATTR0_Y(py),
			ATTR1_SIZE_16 | ATTR1_AFF_ID(c->aff_id) | ATTR1_X(px),
			ATTR2_ID(OBJ_CAR_TILE_BASE) | ATTR2_PALBANK(c->palbank) | ATTR2_PRIO(0)
		);

		set_affine_matrix(c->aff_id, c->ang);
	}

	// Lap indicators (3 small sprites, top-left). Uses palbank 0 (yellow at index 7).
	// OAM entries 4..6
	u8 laps = cars[0].lap;
	for(int i=0;i<LAPS_TO_WIN;i++)
	{
		int oam_id = 4 + i;
		OBJ_ATTR *o = &obj_buf[oam_id];

		int filled = (laps > (u8)i);
		int tile   = filled ? OBJ_LAP_FULL_TILE : OBJ_LAP_EMPTY_TILE;

		obj_set_attr(o,
			ATTR0_SQUARE | ATTR0_Y(4),
			ATTR1_SIZE_8 | ATTR1_X(4 + i*10),
			ATTR2_ID(tile) | ATTR2_PALBANK(0) | ATTR2_PRIO(0)
		);
	}

	// Hide everything else
	for(int i=7;i<128;i++)
		obj_hide(&obj_buf[i]);

	oam_copy(oam_mem, obj_buf, 128);
}

int main(void)
{
	irq_init(NULL);
	irq_add(II_VBLANK, NULL);

	REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;

	// BG0: track
	REG_BG0CNT = BG_CBB(0) | BG_SBB(31) | BG_4BPP | BG_REG_32x32 | BG_PRIO(2);
	REG_BG0HOFS = 0;
	REG_BG0VOFS = 0;

	oam_init(obj_buf, 128);

	make_bg_tiles();
	build_track_map();

	make_obj_tiles();
	make_waypoints();
	reset_race();

	while(1)
	{
		vid_vsync();

		if(key_hit(KEY_START))
			reset_race();

		// Player
		update_player(&cars[0]);

		// AI
		for(int i=1;i<NUM_CARS;i++)
		{
			if(race_finished)
				apply_common_physics(&cars[i], 0, 0, 0, 0);
			else
				ai_control(&cars[i]);
		}

		draw_cars_and_hud();
	}

	return 0;
}
