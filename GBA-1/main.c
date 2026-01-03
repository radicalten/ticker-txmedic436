// outrunish.c - single-file pseudo-3D "into the screen" racer (OutRun-ish) for GBA using tonc
// Controls:
//   Left/Right: steer
//   A: accelerate
//   B: brake
// Notes:
//   - Mode 4 (240x160, 8bpp paletted) with page flipping
//   - Procedural rendering: sky gradient + perspective road + stripes + simple "car" block

#include <tonc.h>

// ----------------------------- Mode 4 helpers -----------------------------
// In Mode 4: 240x160 pixels, 2 pixels packed per u16, 120 u16 per scanline.

static inline u16* m4_backbuffer(void)
{
	// When DCNT_PAGE is set, page 1 is visible at 0x600A000 and page 0 is backbuffer.
	// When DCNT_PAGE is clear, page 0 is visible and page 1 is backbuffer.
	return (REG_DISPCNT & DCNT_PAGE) ? (u16*)0x6000000 : (u16*)0x600A000;
}

static inline void m4_flip(void)
{
	REG_DISPCNT ^= DCNT_PAGE;
}

static inline int clampi(int v, int lo, int hi)
{
	if(v < lo) return lo;
	if(v > hi) return hi;
	return v;
}

static inline void m4_rect(u16* dstBase, int x, int y, int w, int h, u8 clrid)
{
	if(w <= 0 || h <= 0) return;
	for(int iy=0; iy<h; iy++)
		m4_hline(dstBase, y+iy, x, x+w-1, clrid);
}

// ------------------------------ Palette IDs ------------------------------
enum
{
	C_BLACK = 0,

	// sky gradient (8 steps)
	C_SKY0 = 1, C_SKY1, C_SKY2, C_SKY3, C_SKY4, C_SKY5, C_SKY6, C_SKY7,

	// grass
	C_GRASS_D = 16,
	C_GRASS_L = 17,

	// road
	C_ROAD_D = 24,
	C_ROAD_L = 25,

	// rumble
	C_RUMBLE_R = 32,
	C_RUMBLE_W = 33,

	// markings
	C_MARK = 40,

	// car
	C_CAR = 48,
	C_CAR_D = 49,
};

// ------------------------------ Rendering ------------------------------
static void init_palette(void)
{
	// Background palette for Mode 4
	pal_bg_mem[C_BLACK] = RGB15(0,0,0);

	// Sky gradient (dark -> bright)
	pal_bg_mem[C_SKY0] = RGB15( 2, 4, 10);
	pal_bg_mem[C_SKY1] = RGB15( 3, 6, 14);
	pal_bg_mem[C_SKY2] = RGB15( 4, 8, 18);
	pal_bg_mem[C_SKY3] = RGB15( 6,10, 22);
	pal_bg_mem[C_SKY4] = RGB15( 8,12, 26);
	pal_bg_mem[C_SKY5] = RGB15(10,14, 28);
	pal_bg_mem[C_SKY6] = RGB15(12,16, 30);
	pal_bg_mem[C_SKY7] = RGB15(14,18, 31);

	// Grass + road
	pal_bg_mem[C_GRASS_D] = RGB15(2, 10, 2);
	pal_bg_mem[C_GRASS_L] = RGB15(4, 15, 4);

	pal_bg_mem[C_ROAD_D]  = RGB15(5, 5, 6);
	pal_bg_mem[C_ROAD_L]  = RGB15(8, 8, 9);

	// Rumble + markings
	pal_bg_mem[C_RUMBLE_R] = RGB15(27, 3, 3);
	pal_bg_mem[C_RUMBLE_W] = RGB15(28,28,28);
	pal_bg_mem[C_MARK]     = RGB15(31,31,31);

	// Car
	pal_bg_mem[C_CAR]   = RGB15(31, 2, 2);
	pal_bg_mem[C_CAR_D] = RGB15(18, 1, 1);
}

static void draw_scene(u16* bb, int horizon, int playerX, u32 scroll, u16 curvePhase)
{
	// ----- Sky -----
	for(int y=0; y<horizon; y++)
	{
		// 8-step gradient
		int idx = (y * 8) / horizon;           // 0..7
		u8 sky = (u8)(C_SKY0 + idx);
		m4_hline(bb, y, 0, 239, sky);
	}

	// ----- Road -----
	const int maxZ = 159 - horizon;          // depth in scanlines
	const int roadMinHalf = 10;              // near horizon
	const int roadMaxHalf = 130;             // bottom
	const int rumbleW = 3;

	// Curve in pixels (sin LUT in tonc: lu_sin() returns approx [-32767..32767])
	// Keep it modest so it doesn't fly offscreen.
	s16 s = lu_sin(curvePhase);
	int curvePix = (s * 55) / 32767;         // about [-55..55]

	for(int y=horizon; y<160; y++)
	{
		int z = y - horizon;                  // 0..maxZ
		// width grows with z
		int halfW = roadMinHalf + (z * (roadMaxHalf - roadMinHalf)) / maxZ;

		// shift grows with z (stronger curve near bottom)
		int shift = (curvePix * z) / maxZ;

		int center = 120 + playerX + shift;
		int left   = center - halfW;
		int right  = center + halfW;

		// Animated banding based on "scroll" + depth
		// (tweak these >> values to change stripe sizes)
		u32 v = (scroll >> 8) + (u32)(z*6);

		u8 grass = (v & 0x10) ? C_GRASS_L : C_GRASS_D;
		u8 road  = (v & 0x08) ? C_ROAD_L  : C_ROAD_D;
		u8 rumble= (v & 0x08) ? C_RUMBLE_W: C_RUMBLE_R;

		// Fill full line with grass, then paint road on top.
		m4_hline(bb, y, 0, 239, grass);
		m4_hline(bb, y, left, right, road);

		// Rumble strips
		m4_hline(bb, y, left, left+rumbleW-1, rumble);
		m4_hline(bb, y, right-rumbleW+1, right, rumble);

		// Dashed center line (only when road is wide enough)
		if(halfW > 18)
		{
			// dash pattern
			if(((v >> 1) & 0x0F) < 6)
				m4_hline(bb, y, center-1, center+1, C_MARK);
		}
	}

	// ----- Simple "car" block at bottom -----
	// (Drawn last so it sits on top.)
	int carY = 132;
	int carX = clampi(120 + playerX - 12, 0, 240-24);
	m4_rect(bb, carX, carY,     24, 18, C_CAR);
	m4_rect(bb, carX, carY+12,  24,  6, C_CAR_D);   // darker "bumper"
	m4_rect(bb, carX+4, carY+4,  6,  6, C_MARK);    // simple "headlights"
	m4_rect(bb, carX+14,carY+4,  6,  6, C_MARK);
}

// -------------------------------- Main --------------------------------
int main(void)
{
	irq_init(NULL);
	irq_add(II_VBLANK, NULL);

	REG_DISPCNT = DCNT_MODE4 | DCNT_BG2;

	init_palette();

	// Simulation state
	int horizon = 48;

	int playerX = 0;          // camera/player lateral offset (pixels)
	s32 speed   = 0;          // 8.8 fixed
	u32 scroll  = 0;          // accumulates speed for animation

	u16 curvePhase = 0;

	// Tuning
	const s32 SPD_MAX = 7<<8;
	const s32 ACC     = 10;   // per frame
	const s32 BRAKE   = 22;
	const s32 DRAG    = 6;

	while(1)
	{
		key_poll();

		// --- Speed control ---
		if(key_is_down(KEY_A)) speed += ACC;
		else                   speed -= DRAG;

		if(key_is_down(KEY_B)) speed -= BRAKE;

		speed = clampi(speed, 0, SPD_MAX);

		// --- Steering (stronger as you go faster) ---
		int steer = 0;
		if(key_is_down(KEY_LEFT))  steer -= 1;
		if(key_is_down(KEY_RIGHT)) steer += 1;

		// Pixels/frame-ish steering, scaled by speed
		playerX += steer * (1 + (speed >> 9));
		playerX = clampi(playerX, -90, 90);

		// Auto-centering a bit when not steering
		if(steer == 0)
			playerX = (playerX * 15) / 16;

		// --- Animate road movement ---
		scroll += (u32)speed;

		// --- Animate curve (also tied to speed so it "flows") ---
		curvePhase += (u16)(200 + (speed >> 1));

		// Render to backbuffer, then flip during VBlank
		u16* bb = m4_backbuffer();
		draw_scene(bb, horizon, playerX, scroll, curvePhase);

		VBlankIntrWait();
		m4_flip();
	}

	// not reached
	return 0;
}
