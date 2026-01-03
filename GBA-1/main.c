// racer.c - single-file top-down-ish pseudo-3D racer for GBA (Mode 3) using tonc
//
// Build idea (devkitARM + tonc):
//   arm-none-eabi-gcc racer.c -mthumb-interwork -mthumb -O2 -specs=gba.specs -ltonc -o racer.elf
//   arm-none-eabi-objcopy -O binary racer.elf racer.gba
//
// Notes:
// - Uses Mode 3 bitmap drawing (no external art/assets).
// - “Road” is rendered with a simple perspective + changing curve.
// - Car steers left/right, A accelerates, B brakes, SELECT resets.

#include <tonc.h>

#define SCREEN_W 240
#define SCREEN_H 160

// ---------- tiny helpers (no dependencies beyond tonc types/regs) ----------

static inline int clampi(int v, int lo, int hi)
{
	if(v < lo) return lo;
	if(v > hi) return hi;
	return v;
}

static inline void vsync(void)
{
	while(REG_VCOUNT >= 160) { }
	while(REG_VCOUNT < 160)  { }
}

static inline u16 key_now(void)
{
	// REG_KEYS: 0 = pressed, 1 = released
	return (~REG_KEYS) & KEY_MASK;
}

static u32 g_rng = 0x1234567;

static inline u32 rng_u32(void)
{
	// Simple LCG
	g_rng = g_rng*1664525u + 1013904223u;
	return g_rng;
}

static inline int rng_range(int lo, int hi)
{
	// inclusive range
	u32 r = rng_u32();
	int span = (hi - lo + 1);
	return lo + (int)(r % (u32)span);
}

static inline void put_px(int x, int y, u16 c)
{
	if((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H)
		vid_mem[y*SCREEN_W + x] = c;
}

static void rect_fill(int x, int y, int w, int h, u16 c)
{
	if(w <= 0 || h <= 0) return;
	int x0 = clampi(x, 0, SCREEN_W);
	int y0 = clampi(y, 0, SCREEN_H);
	int x1 = clampi(x + w, 0, SCREEN_W);
	int y1 = clampi(y + h, 0, SCREEN_H);

	for(int yy=y0; yy<y1; yy++)
	{
		u16 *row = &vid_mem[yy*SCREEN_W];
		for(int xx=x0; xx<x1; xx++)
			row[xx] = c;
	}
}

// ---------- 3x5 digits for HUD ----------
// Each row is 3 bits (MSB on left)
static const u8 g_font3x5[10][5] =
{
	{0b111,0b101,0b101,0b101,0b111}, // 0
	{0b010,0b110,0b010,0b010,0b111}, // 1
	{0b111,0b001,0b111,0b100,0b111}, // 2
	{0b111,0b001,0b111,0b001,0b111}, // 3
	{0b101,0b101,0b111,0b001,0b001}, // 4
	{0b111,0b100,0b111,0b001,0b111}, // 5
	{0b111,0b100,0b111,0b101,0b111}, // 6
	{0b111,0b001,0b001,0b010,0b010}, // 7
	{0b111,0b101,0b111,0b101,0b111}, // 8
	{0b111,0b101,0b111,0b001,0b111}, // 9
};

static void draw_digit_3x5(int x, int y, int d, int scale, u16 col)
{
	if(d < 0 || d > 9) return;
	if(scale < 1) scale = 1;

	for(int r=0; r<5; r++)
	{
		u8 bits = g_font3x5[d][r];
		for(int c=0; c<3; c++)
		{
			if(bits & (1u << (2-c)))
			{
				rect_fill(x + c*scale, y + r*scale, scale, scale, col);
			}
		}
	}
}

static void draw_number(int x, int y, int value, int digits, int scale, u16 col)
{
	// draws fixed width, leading zeros if digits > needed
	if(digits < 1) digits = 1;
	int v = value;
	if(v < 0) v = 0;

	// compute divisor = 10^(digits-1)
	int div = 1;
	for(int i=1; i<digits; i++) div *= 10;

	for(int i=0; i<digits; i++)
	{
		int d = (v / div) % 10;
		draw_digit_3x5(x + i*(3*scale + scale), y, d, scale, col);
		div /= 10;
	}
}

// ---------- game ----------

int main(void)
{
	// Video: Mode 3, BG2
	REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

	// Colors
	const u16 COL_SKY    = RGB15(10, 18, 31);
	const u16 COL_GRASS1 = RGB15( 5, 20,  5);
	const u16 COL_GRASS2 = RGB15( 4, 16,  4);
	const u16 COL_ROAD   = RGB15( 7,  7,  8);
	const u16 COL_ROAD2  = RGB15( 8,  8,  9);
	const u16 COL_RUM1   = RGB15(31, 31, 31);
	const u16 COL_RUM2   = RGB15(31,  0,  0);
	const u16 COL_LINE   = RGB15(31, 31,  0);
	const u16 COL_CAR    = RGB15(31,  3,  3);
	const u16 COL_CAR2   = RGB15(20,  0,  0);
	const u16 COL_TIRE   = RGB15( 2,  2,  2);
	const u16 COL_HUD    = RGB15(31, 31, 31);

	// Fixed-point 16.16
	const int FX = 16;
	const int ONE = 1 << FX;

	// Car state (screen-space x, but “world” feel via road center comparison)
	int car_x  = 120 * ONE;
	int car_vx = 0;

	// Forward speed / distance
	int speed = 2 * ONE;          // units per frame
	int max_speed = 6 * ONE;
	u32 dist = 0;                 // 16.16 "distance"

	// Road generation state
	int curve      = 0;           // 16.16 px shift per y
	int curve_tgt  = 0;
	int offset     = 0;           // 16.16 constant road center offset
	int offset_tgt = 0;

	// Segment control
	u32 next_seg_at = 0;          // in 16.16 distance
	const u32 seg_len = (220u << FX);

	// Camera-ish
	const int horizon = 28;
	const int car_y = 124;

	for(;;)
	{
		vsync();

		u16 keys = key_now();

		if(keys & KEY_SELECT)
		{
			car_x = 120 * ONE;
			car_vx = 0;
			speed = 2 * ONE;
			dist = 0;
			curve = curve_tgt = 0;
			offset = offset_tgt = 0;
			next_seg_at = 0;
		}

		// Pick new road segment targets as you advance
		if(dist >= next_seg_at)
		{
			next_seg_at = dist + seg_len;

			// target curve: gentle most of the time, sometimes sharper
			int c = rng_range(-42000, 42000);     // about +/-0.64 px per y
			// occasionally force a straighter segment
			if((rng_u32() & 3u) == 0) c /= 3;
			curve_tgt = c;

			// target offset drift so road doesn't live perfectly centered
			offset_tgt = rng_range(-18, 18) * ONE;
		}

		// Smoothly approach targets
		curve  += (curve_tgt  - curve ) / 32;
		offset += (offset_tgt - offset) / 64;

		// Controls
		int steer = 0;
		if(keys & KEY_LEFT)  steer -= 1;
		if(keys & KEY_RIGHT) steer += 1;

		// Accel / brake
		if(keys & KEY_A) speed += (ONE / 16);
		if(keys & KEY_B) speed -= (ONE / 14);

		// Clamp speed
		if(speed < 0) speed = 0;
		if(speed > max_speed) speed = max_speed;

		// Steering physics (more responsive with speed)
		int steer_acc = (ONE / 10) + (speed / 6);
		car_vx += steer * steer_acc;

		// Damping
		car_vx = (car_vx * 15) / 16;

		// Integrate
		car_x += car_vx;

		// Advance distance by speed (simple)
		dist += (u32)speed;

		// Road at car position for collision/slowdown
		int rel_car = car_y - horizon;
		if(rel_car < 0) rel_car = 0;

		// Perspective width at car_y
		int road_w_car = 46 + rel_car*2;
		road_w_car = clampi(road_w_car, 40, 220);

		int center_car = 120
			+ (offset >> FX)
			+ (int)(((s32)curve * rel_car) >> FX);

		int car_px = car_x >> FX;

		// Off-road penalty
		int half = road_w_car/2;
		if(car_px < center_car - half + 2 || car_px > center_car + half - 2)
		{
			// slow down on grass; also reduce max while off-road
			if(speed > ONE/2) speed -= ONE/10;
			max_speed = 5 * ONE;
		}
		else
		{
			max_speed = 6 * ONE;
		}

		// Keep car somewhat on screen
		car_x = clampi(car_x, 8*ONE, (SCREEN_W-8)*ONE);

		// ---------- render ----------
		for(int y=0; y<SCREEN_H; y++)
		{
			u16 *row = &vid_mem[y*SCREEN_W];

			if(y < horizon)
			{
				// simple sky fill
				for(int x=0; x<SCREEN_W; x++) row[x] = COL_SKY;
				continue;
			}

			int rel = y - horizon;

			// Width grows with y (perspective)
			int road_w = 44 + rel*2;
			road_w = clampi(road_w, 40, 236);

			// Center drifts with curve (linear bend with y)
			int center = 120
				+ (offset >> FX)
				+ (int)(((s32)curve * rel) >> FX);

			// Rumble strip width grows slightly with y
			int rum = 3 + rel/28;
			rum = clampi(rum, 3, 7);

			int left_road  = center - road_w/2;
			int right_road = center + road_w/2;

			int left_rum  = left_road  - rum;
			int right_rum = right_road + rum;

			// Clamp to screen
			left_rum   = clampi(left_rum,  0, SCREEN_W);
			left_road  = clampi(left_road, 0, SCREEN_W);
			right_road = clampi(right_road,0, SCREEN_W);
			right_rum  = clampi(right_rum, 0, SCREEN_W);

			// Animate striping with distance; use integer part
			int t = (int)(dist >> FX);
			int grass_phase = ((t/6) + (y/3)) & 1;
			int rum_phase   = ((t/4) + (y/2)) & 1;
			int road_phase  = ((t/10) + (y/4)) & 1;

			u16 grass = grass_phase ? COL_GRASS1 : COL_GRASS2;
			u16 rumble = rum_phase ? COL_RUM1 : COL_RUM2;
			u16 road = road_phase ? COL_ROAD : COL_ROAD2;

			// Fill segments
			for(int x=0; x<left_rum; x++) row[x] = grass;
			for(int x=left_rum; x<left_road; x++) row[x] = rumble;
			for(int x=left_road; x<right_road; x++) row[x] = road;
			for(int x=right_road; x<right_rum; x++) row[x] = rumble;
			for(int x=right_rum; x<SCREEN_W; x++) row[x] = grass;

			// Center dashed line (only if road is wide enough)
			if(road_w > 90)
			{
				int dash = ((t/2) + y) & 15;
				if(dash < 8)
				{
					int cx = clampi(center, 1, SCREEN_W-2);
					row[cx-1] = COL_LINE;
					row[cx]   = COL_LINE;
					row[cx+1] = COL_LINE;
				}
			}
		}

		// Draw car (simple sprite-like shape)
		int cx = car_x >> FX;
		int body_w = 12, body_h = 16;
		int bx = cx - body_w/2;
		int by = car_y - body_h/2;

		// shadow
		rect_fill(bx+1, by+1, body_w, body_h, RGB15(0,0,0));

		// tires
		rect_fill(bx-2, by+2, 3, 5, COL_TIRE);
		rect_fill(bx-2, by+body_h-7, 3, 5, COL_TIRE);
		rect_fill(bx+body_w-1, by+2, 3, 5, COL_TIRE);
		rect_fill(bx+body_w-1, by+body_h-7, 3, 5, COL_TIRE);

		// body + cab
		rect_fill(bx, by, body_w, body_h, COL_CAR);
		rect_fill(bx+2, by+3, body_w-4, 6, COL_CAR2);

		// HUD
		// speed_display: arbitrary units
		int speed_display = (speed * 100) >> FX;     // 0..600ish
		int dist_display  = (int)(dist >> FX);       // pixels-ish

		// background strip for readability
		rect_fill(0, 0, 94, 18, RGB15(0,0,0));

		// "SPD" as three blocks + digits (no letters, keep it simple)
		draw_number(4, 4, speed_display, 3, 2, COL_HUD);      // 3 digits

		// distance right below
		draw_number(4, 12, dist_display % 100000, 5, 1, COL_HUD);
	}

	return 0;
}
