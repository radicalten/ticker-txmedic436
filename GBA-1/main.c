// iso_offroad_tonc.c
// Single-screen isometric racer demo for GBA (Mode 3 bitmap), using tonc.
// Track is procedural (no external art assets).
//
// Build idea (devkitARM + tonc):
//   arm-none-eabi-gcc iso_offroad_tonc.c -mthumb -O2 -Wall -Wextra -I<tonc>/include -L<tonc>/lib -ltonc -lm -o game.elf
//
// Notes:
// - Uses Mode 3 (240x160 16-bit) and draws isometric diamonds in software.
// - This is a small demo inspired by the feel of classic off-road racers,
//   not a content/asset clone.
//
// References:
// - Tonc (GBA dev tutorial / headers): https://www.coranac.com/tonc/text/
// - GBATEK (registers, display modes, VCOUNT, keys): https://problemkaputt.de/gbatek.htm

#include <tonc.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define SCREEN_W 240
#define SCREEN_H 160

// Isometric tile geometry (diamond)
#define TILE_W 16
#define TILE_H 8

// Single-screen map size (picked to fit comfortably on 240x160)
#define MAP_W 14
#define MAP_H 14

// Key mask if not provided
#ifndef KEY_MASK
#define KEY_MASK 0x03FF
#endif

typedef uint16_t COLOR;

// Tile types
enum {
	T_GRASS = 0,
	T_ROAD  = 1,
	T_MUD   = 2,
	T_FINISH= 3,
};

static uint8_t g_map[MAP_H][MAP_W];

// ------------------------------
// Minimal Mode 3 drawing helpers
// ------------------------------
static inline void vsync(void)
{
	// Wait until we're out of vblank, then wait until vblank begins.
	while(REG_VCOUNT >= 160) {}
	while(REG_VCOUNT < 160) {}
}

static inline void pset(int x, int y, COLOR c)
{
	if((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H)
		vid_mem[y*SCREEN_W + x] = c;
}

static void hline(int x1, int x2, int y, COLOR c)
{
	if((unsigned)y >= SCREEN_H) return;
	if(x1 > x2) { int t=x1; x1=x2; x2=t; }

	if(x2 < 0 || x1 >= SCREEN_W) return;
	if(x1 < 0) x1 = 0;
	if(x2 >= SCREEN_W) x2 = SCREEN_W-1;

	COLOR *dst = &vid_mem[y*SCREEN_W + x1];
	for(int x=x1; x<=x2; x++)
		*dst++ = c;
}

static void clear_screen(COLOR c)
{
	for(int i=0; i<SCREEN_W*SCREEN_H; i++)
		vid_mem[i] = c;
}

static void rect_fill(int x, int y, int w, int h, COLOR c)
{
	int x2 = x + w - 1;
	int y2 = y + h - 1;
	for(int yy=y; yy<=y2; yy++)
		hline(x, x2, yy, c);
}

static void line(int x0, int y0, int x1, int y1, COLOR c)
{
	// Bresenham
	int dx = (x1>x0) ? (x1-x0) : (x0-x1);
	int sx = (x0<x1) ? 1 : -1;
	int dy = (y1>y0) ? (y0-y1) : (y1-y0); // negative
	int sy = (y0<y1) ? 1 : -1;
	int err = dx + dy;

	while(1)
	{
		pset(x0,y0,c);
		if(x0==x1 && y0==y1) break;
		int e2 = 2*err;
		if(e2 >= dy) { err += dy; x0 += sx; }
		if(e2 <= dx) { err += dx; y0 += sy; }
	}
}

// Draw a filled isometric diamond centered at (cx,cy)
static void diamond_fill(int cx, int cy, int w, int h, COLOR c)
{
	const int halfH = h/2;           // for TILE_H=8 -> 4
	const int step  = w/halfH;       // for TILE_W=16 -> 4

	for(int r=0; r<h; r++)
	{
		int k = (r < halfH) ? (r+1) : (h-r);
		int width = k * step;         // 4,8,12,16,16,12,8,4 for 16x8
		int y = cy - halfH + r;
		int x1 = cx - width/2;
		int x2 = x1 + width - 1;
		hline(x1, x2, y, c);
	}
}

// Striped finish tile (simple scanline stripes)
static void diamond_finish(int cx, int cy, int w, int h, int phase)
{
	const int halfH = h/2;
	const int step  = w/halfH;

	COLOR c0 = RGB15(31,31,31);
	COLOR c1 = RGB15( 2, 2, 2);

	for(int r=0; r<h; r++)
	{
		int k = (r < halfH) ? (r+1) : (h-r);
		int width = k * step;
		int y = cy - halfH + r;
		int x1 = cx - width/2;
		int x2 = x1 + width - 1;

		// Alternate every 2 pixels, offset by row & phase
		for(int x=x1; x<=x2; x++)
		{
			COLOR cc = (((x + (r<<1) + phase) >> 1) & 1) ? c0 : c1;
			pset(x, y, cc);
		}
	}
}

// ------------------------------
// Isometric projection
// ------------------------------
static inline void iso_project(float wx, float wy, int *sx, int *sy)
{
	// Center map around origin in world space.
	const float cx = (MAP_W-1) * 0.5f;
	const float cy = (MAP_H-1) * 0.5f;

	float dx = wx - cx;
	float dy = wy - cy;

	// Screen origin for centered single-screen view
	const int ORGX = 120;
	const int ORGY = 80;

	float px = (dx - dy) * (TILE_W * 0.5f);
	float py = (dx + dy) * (TILE_H * 0.5f);

	*sx = ORGX + (int)lroundf(px);
	*sy = ORGY + (int)lroundf(py);
}

// ------------------------------
// Track generation
// ------------------------------
static void build_track(void)
{
	// Fill grass
	for(int y=0; y<MAP_H; y++)
		for(int x=0; x<MAP_W; x++)
			g_map[y][x] = T_GRASS;

	// Simple square loop track (ring) with width 2
	const int o0 = 2, o1 = 11; // outer bounds
	const int w  = 2;

	for(int y=o0; y<=o1; y++)
	{
		for(int x=o0; x<=o1; x++)
		{
			bool on_outer =
				(x==o0 || x==o1 || y==o0 || y==o1 ||
				 x==o0+1 || x==o1-1 || y==o0+1 || y==o1-1);

			if(on_outer)
				g_map[y][x] = T_ROAD;
		}
	}

	// Finish line on the top straight
	g_map[o0][6] = T_FINISH;
	g_map[o0][7] = T_FINISH;

	// Mud patch on right straight
	for(int y=6; y<=8; y++)
		g_map[y][o1-1] = T_MUD;

	(void)w;
}

static inline uint8_t tile_at(float wx, float wy)
{
	int x = (int)floorf(wx);
	int y = (int)floorf(wy);
	if(x<0 || x>=MAP_W || y<0 || y>=MAP_H) return T_GRASS;
	return g_map[y][x];
}

// ------------------------------
// Car + physics
// ------------------------------
typedef struct Car
{
	float x, y;        // world in tile coords
	float vx, vy;      // world vel (tiles/frame)
	float ang;         // radians
	int laps;
	bool on_finish_prev;
} Car;

static void car_reset(Car *c)
{
	*c = (Car){
		.x = 6.5f,
		.y = 3.5f,
		.vx = 0.0f,
		.vy = 0.0f,
		.ang = 1.5707963f, // ~pi/2
		.laps = 0,
		.on_finish_prev = false
	};
}

static void car_update(Car *c, uint16_t keys)
{
	// Parameters tuned for “arcade-ish” feel in tile-space
	const float accel      = 0.020f;
	const float brake      = 0.018f;
	const float turn_rate  = 0.060f; // radians/frame
	const float max_speed  = 0.60f;

	const bool up    = (keys & KEY_UP)    != 0;
	const bool down  = (keys & KEY_DOWN)  != 0;
	const bool left  = (keys & KEY_LEFT)  != 0;
	const bool right = (keys & KEY_RIGHT) != 0;
	const bool turbo = (keys & KEY_A)     != 0;
	const bool reset = (keys & KEY_B)     != 0;

	if(reset)
	{
		car_reset(c);
		return;
	}

	uint8_t t = tile_at(c->x, c->y);

	// Surface-dependent drag/traction
	float drag = 0.985f;
	float grip = 1.00f;
	if(t == T_GRASS) { drag = 0.965f; grip = 0.70f; }
	if(t == T_MUD)   { drag = 0.955f; grip = 0.55f; }

	// Steering effectiveness scales with speed (tiny when stopped)
	float speed = sqrtf(c->vx*c->vx + c->vy*c->vy);
	float steer = turn_rate * (0.25f + 1.25f*speed) * grip;

	if(left)  c->ang -= steer;
	if(right) c->ang += steer;

	// Accel along heading
	float ax = 0.0f, ay = 0.0f;
	float hx = cosf(c->ang);
	float hy = sinf(c->ang);

	if(up)
	{
		float a = accel * (turbo ? 1.8f : 1.0f);
		ax += hx * a;
		ay += hy * a;
	}
	if(down)
	{
		ax -= hx * brake;
		ay -= hy * brake;
	}

	// Integrate velocity with drag
	c->vx = c->vx * drag + ax;
	c->vy = c->vy * drag + ay;

	// Speed clamp
	speed = sqrtf(c->vx*c->vx + c->vy*c->vy);
	if(speed > max_speed)
	{
		float s = max_speed / speed;
		c->vx *= s;
		c->vy *= s;
	}

	// Integrate position
	c->x += c->vx;
	c->y += c->vy;

	// Keep within world bounds (simple clamp + damp)
	if(c->x < 0.2f) { c->x = 0.2f; c->vx *= -0.2f; }
	if(c->y < 0.2f) { c->y = 0.2f; c->vy *= -0.2f; }
	if(c->x > (MAP_W-1) + 0.8f) { c->x = (MAP_W-1) + 0.8f; c->vx *= -0.2f; }
	if(c->y > (MAP_H-1) + 0.8f) { c->y = (MAP_H-1) + 0.8f; c->vy *= -0.2f; }

	// Lap detection: count when you newly enter a finish tile with some motion
	bool on_finish = (tile_at(c->x, c->y) == T_FINISH);
	if(on_finish && !c->on_finish_prev && speed > 0.08f)
		c->laps++;
	c->on_finish_prev = on_finish;
}

// ------------------------------
// Rendering
// ------------------------------
static inline COLOR shade(COLOR c, int darker)
{
	// Very tiny “manual shading”: choose between two pre-picked colors in caller.
	(void)c; (void)darker;
	return c;
}

static void draw_world(int frame_phase)
{
	// Back-to-front order for isometric: increasing (x+y)
	for(int s=0; s<MAP_W+MAP_H-1; s++)
	{
		for(int y=0; y<MAP_H; y++)
		{
			int x = s - y;
			if(x < 0 || x >= MAP_W) continue;

			int sx, sy;
			iso_project((float)x + 0.5f, (float)y + 0.5f, &sx, &sy);

			int checker = (x ^ y) & 1;

			uint8_t t = g_map[y][x];
			if(t == T_GRASS)
			{
				COLOR c = checker ? RGB15(6, 17, 6) : RGB15(5, 14, 5);
				diamond_fill(sx, sy, TILE_W, TILE_H, c);
			}
			else if(t == T_ROAD)
			{
				COLOR c = checker ? RGB15(12, 12, 12) : RGB15(10, 10, 10);
				diamond_fill(sx, sy, TILE_W, TILE_H, c);
			}
			else if(t == T_MUD)
			{
				COLOR c = checker ? RGB15(12, 7, 3) : RGB15(10, 6, 3);
				diamond_fill(sx, sy, TILE_W, TILE_H, c);
			}
			else // T_FINISH
			{
				diamond_finish(sx, sy, TILE_W, TILE_H, (x + y + frame_phase) & 3);
			}
		}
	}
}

static void draw_car(const Car *c)
{
	int cx, cy;
	iso_project(c->x, c->y, &cx, &cy);

	// Car body
	rect_fill(cx-3, cy-2, 7, 5, RGB15(31, 4, 4));
	rect_fill(cx-2, cy-1, 5, 3, RGB15(31, 20, 20));

	// Heading indicator: transform world heading into screen space direction
	float hx = cosf(c->ang);
	float hy = sinf(c->ang);
	float sx = (hx - hy) * (TILE_W * 0.5f);
	float sy = (hx + hy) * (TILE_H * 0.5f);

	// Normalize for consistent length
	float len = sqrtf(sx*sx + sy*sy);
	if(len < 0.0001f) len = 1.0f;
	sx /= len; sy /= len;

	int fx = cx + (int)lroundf(sx * 10.0f);
	int fy = cy + (int)lroundf(sy * 10.0f);
	line(cx, cy, fx, fy, RGB15(31, 31, 0));
}

// ------------------------------
// Main
// ------------------------------
int main(void)
{
	REG_DISPCNT = DCNT_MODE3 | DCNT_BG2; // Mode 3 bitmap on BG2 (GBATEK/Tonc)

	build_track();

	Car car;
	car_reset(&car);

	int frame = 0;

	while(1)
	{
		vsync();

		// Read keys (pressed = 1)
		uint16_t keys = (uint16_t)(~REG_KEYINPUT) & KEY_MASK;

		car_update(&car, keys);

		// Draw
		clear_screen(RGB15(0, 0, 0));     // black frame border
		draw_world(frame);
		draw_car(&car);

		// Minimal HUD (no tonc text engine used to keep this file self-contained):
		// Draw tiny lap pips at top-left.
		for(int i=0; i<car.laps && i<20; i++)
			rect_fill(4 + i*6, 4, 4, 4, RGB15(31,31,31));

		frame++;
	}

	return 0;
}
