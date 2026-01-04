// puyo_gba_tonc_singlefile.c
//
// Single-file "Puyo Puyo-like" puzzle game for GBA using tonc.
// - Mode 3 bitmap rendering (simple colored blocks)
// - 6x12 board (+2 hidden rows)
// - Falling 2-piece capsule, rotate/move/drop
// - Clear groups of 4+ connected same-color puyos
//
// Controls:
//   Left/Right : move
//   Down       : soft drop
//   Up         : hard drop
//   A          : rotate CW
//   B          : rotate CCW
//   Start      : restart (also from game over)
//
// Build: put this in a tonc/devkitARM project and compile normally with tonc.
// You need tonc available (tonc.h, libtonc).

#include <tonc.h>
#include <stdio.h>
#include <string.h>

// -------------------- Config --------------------

#define GRID_W      6
#define GRID_H      12
#define HIDDEN_ROWS 2
#define TOTAL_H     (GRID_H + HIDDEN_ROWS)

#define CELL        12

#define BOARD_X     16
#define BOARD_Y     8

#define PREVIEW_X   120
#define PREVIEW_Y   24

// -------------------- Colors --------------------

enum
{
	C_EMPTY = 0,
	C_RED   = 1,
	C_GRN   = 2,
	C_BLU   = 3,
	C_YEL   = 4,
	C_PUR   = 5,
	C_MAX
};

static const u16 g_bg_col      = RGB15(3, 3, 5);
static const u16 g_border_col  = RGB15(0, 0, 0);
static const u16 g_text_col    = RGB15(31, 31, 31);

static u16 puyo_col(int c)
{
	switch(c)
	{
	case C_RED: return RGB15(31, 6, 6);
	case C_GRN: return RGB15(6, 31, 10);
	case C_BLU: return RGB15(8, 12, 31);
	case C_YEL: return RGB15(31, 28, 6);
	case C_PUR: return RGB15(22, 8, 28);
	default:    return g_bg_col;
	}
}

static u16 puyo_col_dark(int c)
{
	switch(c)
	{
	case C_RED: return RGB15(18, 2, 2);
	case C_GRN: return RGB15(2, 18, 6);
	case C_BLU: return RGB15(3, 5, 18);
	case C_YEL: return RGB15(18, 16, 2);
	case C_PUR: return RGB15(12, 3, 16);
	default:    return RGB15(1,1,2);
	}
}

static u16 puyo_col_light(int c)
{
	switch(c)
	{
	case C_RED: return RGB15(31, 20, 20);
	case C_GRN: return RGB15(20, 31, 22);
	case C_BLU: return RGB15(20, 22, 31);
	case C_YEL: return RGB15(31, 31, 18);
	case C_PUR: return RGB15(28, 20, 31);
	default:    return RGB15(10,10,12);
	}
}

// -------------------- RNG --------------------

static u32 g_rng = 0x12345678;

static u32 rng_next(void)
{
	// Simple LCG
	g_rng = 1664525u*g_rng + 1013904223u;
	return g_rng;
}

static int rng_range(int lo, int hi_inclusive)
{
	u32 r = rng_next();
	int span = (hi_inclusive - lo + 1);
	return lo + (int)(r % (u32)span);
}

// -------------------- Game State --------------------

typedef struct
{
	int x, y;     // pivot position in grid coords (includes hidden rows)
	int rot;      // 0=2nd above, 1=right, 2=below, 3=left
	u8  c1, c2;   // colors
} Piece;

typedef enum
{
	ST_FALLING = 0,
	ST_RESOLVE = 1,
	ST_GAMEOVER = 2
} GameState;

static u8 field[TOTAL_H][GRID_W];   // 0 empty, else color

static Piece cur, nextp;
static GameState state = ST_FALLING;

static int score = 0;
static int chain = 0;

static int fall_ctr = 0;
static int base_fall_interval = 28; // frames per step at normal speed

// -------------------- Drawing Helpers (Mode 3) --------------------

static inline void m3_hline(int x, int y, int w, u16 c)
{
	u16 *dst = vid_mem + y*240 + x;
	memset16(dst, c, w);
}

static void m3_rect(int x, int y, int w, int h, u16 c)
{
	for(int iy=0; iy<h; iy++)
		m3_hline(x, y+iy, w, c);
}

static void draw_puyo_px(int px, int py, int col_id)
{
	if(col_id == C_EMPTY) return;

	// Outer block
	m3_rect(px, py, CELL, CELL, puyo_col(col_id));

	// Border
	m3_hline(px, py, CELL, puyo_col_dark(col_id));
	m3_hline(px, py+CELL-1, CELL, puyo_col_dark(col_id));
	for(int iy=0; iy<CELL; iy++)
	{
		vid_mem[(py+iy)*240 + px]           = puyo_col_dark(col_id);
		vid_mem[(py+iy)*240 + (px+CELL-1)]  = puyo_col_dark(col_id);
	}

	// Simple highlight
	m3_rect(px+2, py+2, 4, 4, puyo_col_light(col_id));
}

static void draw_cell(int gx, int gy, int col_id)
{
	// gy is in visible coordinates 0..GRID_H-1
	int px = BOARD_X + gx*CELL;
	int py = BOARD_Y + gy*CELL;
	draw_puyo_px(px, py, col_id);
}

static void draw_board_frame(void)
{
	int w = GRID_W*CELL;
	int h = GRID_H*CELL;

	// Background
	m3_rect(0, 0, 240, 160, g_bg_col);

	// Board border
	m3_rect(BOARD_X-2, BOARD_Y-2, w+4, 2, g_border_col);
	m3_rect(BOARD_X-2, BOARD_Y+h, w+4, 2, g_border_col);
	m3_rect(BOARD_X-2, BOARD_Y-2, 2, h+4, g_border_col);
	m3_rect(BOARD_X+w,  BOARD_Y-2, 2, h+4, g_border_col);

	// Sidebar area
	m3_rect(PREVIEW_X-8, 8, 240-(PREVIEW_X-8), 144, RGB15(2,2,4));
}

// -------------------- Piece Logic --------------------

static void piece_offsets(int rot, int *dx2, int *dy2)
{
	switch(rot & 3)
	{
	default:
	case 0: *dx2 = 0;  *dy2 = -1; break; // above
	case 1: *dx2 = 1;  *dy2 = 0;  break; // right
	case 2: *dx2 = 0;  *dy2 = 1;  break; // below
	case 3: *dx2 = -1; *dy2 = 0;  break; // left
	}
}

static bool in_bounds(int x, int y)
{
	return (x >= 0 && x < GRID_W && y >= 0 && y < TOTAL_H);
}

static bool cell_empty(int x, int y)
{
	return field[y][x] == C_EMPTY;
}

static bool can_place_piece_at(const Piece *p, int nx, int ny, int nrot)
{
	int dx2, dy2;
	piece_offsets(nrot, &dx2, &dy2);

	int x1 = nx, y1 = ny;
	int x2 = nx + dx2, y2 = ny + dy2;

	if(!in_bounds(x1,y1) || !in_bounds(x2,y2))
		return false;

	if(!cell_empty(x1,y1) || !cell_empty(x2,y2))
		return false;

	return true;
}

static bool try_move(int dx, int dy)
{
	int nx = cur.x + dx;
	int ny = cur.y + dy;
	if(can_place_piece_at(&cur, nx, ny, cur.rot))
	{
		cur.x = nx; cur.y = ny;
		return true;
	}
	return false;
}

static bool try_rotate(int dir) // dir: +1 cw, -1 ccw
{
	int nrot = (cur.rot + (dir>0 ? 1 : 3)) & 3;

	// Try rotate in place, then simple wall-kicks.
	static const int kicks[3] = { 0, -1, +1 };
	for(int i=0; i<3; i++)
	{
		int nx = cur.x + kicks[i];
		int ny = cur.y;
		if(can_place_piece_at(&cur, nx, ny, nrot))
		{
			cur.x = nx;
			cur.rot = nrot;
			return true;
		}
	}
	return false;
}

static void lock_piece(void)
{
	int dx2, dy2;
	piece_offsets(cur.rot, &dx2, &dy2);

	int x1 = cur.x, y1 = cur.y;
	int x2 = cur.x + dx2, y2 = cur.y + dy2;

	// Write into field
	if(in_bounds(x1,y1)) field[y1][x1] = cur.c1;
	if(in_bounds(x2,y2)) field[y2][x2] = cur.c2;
}

static void gen_random_piece(Piece *p)
{
	p->x = GRID_W/2;
	p->y = HIDDEN_ROWS-1; // start in hidden area
	p->rot = 2;           // second below pivot at spawn (enters board)
	p->c1 = (u8)rng_range(1, C_MAX-1);
	p->c2 = (u8)rng_range(1, C_MAX-1);
}

static void spawn_piece(void)
{
	cur = nextp;
	cur.x = GRID_W/2;
	cur.y = HIDDEN_ROWS-1;
	cur.rot = 2;

	gen_random_piece(&nextp);

	if(!can_place_piece_at(&cur, cur.x, cur.y, cur.rot))
	{
		state = ST_GAMEOVER;
	}
	else
	{
		state = ST_FALLING;
	}
}

// -------------------- Clear + Gravity --------------------

static void apply_gravity(void)
{
	for(int x=0; x<GRID_W; x++)
	{
		int write_y = TOTAL_H-1;
		for(int y=TOTAL_H-1; y>=0; y--)
		{
			u8 v = field[y][x];
			if(v != C_EMPTY)
			{
				field[y][x] = C_EMPTY;
				field[write_y][x] = v;
				write_y--;
			}
		}
		// Above write_y remains empty already.
	}
}

static int resolve_clears_once(void)
{
	bool vis[TOTAL_H][GRID_W];
	memset(vis, 0, sizeof(vis));

	bool to_clear[TOTAL_H][GRID_W];
	memset(to_clear, 0, sizeof(to_clear));

	int cleared = 0;

	// BFS buffers (max cells)
	int qx[GRID_W*TOTAL_H];
	int qy[GRID_W*TOTAL_H];

	for(int sy=0; sy<TOTAL_H; sy++)
	{
		for(int sx=0; sx<GRID_W; sx++)
		{
			u8 col = field[sy][sx];
			if(col == C_EMPTY || vis[sy][sx]) continue;

			// BFS group
			int head=0, tail=0;
			int group_n = 0;

			// store group coords for potential marking
			int gx[GRID_W*TOTAL_H];
			int gy[GRID_W*TOTAL_H];

			vis[sy][sx] = true;
			qx[tail] = sx; qy[tail] = sy; tail++;

			while(head < tail)
			{
				int x = qx[head];
				int y = qy[head];
				head++;

				gx[group_n] = x;
				gy[group_n] = y;
				group_n++;

				// 4-neighbors
				static const int dx[4] = {1,-1,0,0};
				static const int dy[4] = {0,0,1,-1};

				for(int i=0; i<4; i++)
				{
					int nx = x + dx[i];
					int ny = y + dy[i];
					if(nx<0 || nx>=GRID_W || ny<0 || ny>=TOTAL_H) continue;
					if(vis[ny][nx]) continue;
					if(field[ny][nx] != col) continue;

					vis[ny][nx] = true;
					qx[tail] = nx; qy[tail] = ny; tail++;
				}
			}

			if(group_n >= 4)
			{
				for(int i=0; i<group_n; i++)
					to_clear[gy[i]][gx[i]] = true;
			}
		}
	}

	for(int y=0; y<TOTAL_H; y++)
	{
		for(int x=0; x<GRID_W; x++)
		{
			if(to_clear[y][x] && field[y][x] != C_EMPTY)
			{
				field[y][x] = C_EMPTY;
				cleared++;
			}
		}
	}

	return cleared;
}

static void resolve_board(void)
{
	chain = 0;
	for(;;)
	{
		int cleared = resolve_clears_once();
		if(cleared <= 0)
			break;

		chain++;
		// Simple scoring: base on cleared and chain multiplier
		score += cleared * 10 * chain;

		apply_gravity();
	}
}

// -------------------- Rendering --------------------

static void draw_game(void)
{
	draw_board_frame();

	// Draw settled field (visible only)
	for(int y=HIDDEN_ROWS; y<TOTAL_H; y++)
	{
		for(int x=0; x<GRID_W; x++)
		{
			u8 col = field[y][x];
			if(col != C_EMPTY)
				draw_cell(x, y - HIDDEN_ROWS, col);
		}
	}

	// Draw current piece (if falling)
	if(state == ST_FALLING)
	{
		int dx2, dy2;
		piece_offsets(cur.rot, &dx2, &dy2);

		int x1 = cur.x, y1 = cur.y;
		int x2 = cur.x + dx2, y2 = cur.y + dy2;

		if(y1 >= HIDDEN_ROWS)
			draw_cell(x1, y1 - HIDDEN_ROWS, cur.c1);
		if(y2 >= HIDDEN_ROWS)
			draw_cell(x2, y2 - HIDDEN_ROWS, cur.c2);
	}

	// Text UI
	char buf[128];

	tte_set_pos(PREVIEW_X, 10);
	tte_set_ink(g_text_col);
	tte_write("NEXT");

	// Next preview (2 blocks)
	draw_puyo_px(PREVIEW_X, PREVIEW_Y, nextp.c1);
	draw_puyo_px(PREVIEW_X, PREVIEW_Y + CELL + 2, nextp.c2);

	tte_set_pos(PREVIEW_X, 90);
	snprintf(buf, sizeof(buf), "SCORE\n%d", score);
	tte_write(buf);

	tte_set_pos(PREVIEW_X, 130);
	tte_write("A/B rot\nUP drop\nSTART rst");

	if(state == ST_GAMEOVER)
	{
		// Overlay message
		m3_rect(20, 58, 200, 44, RGB15(0,0,0));
		m3_rect(22, 60, 196, 40, RGB15(6,6,10));
		tte_set_pos(62, 72);
		tte_write("GAME OVER");
		tte_set_pos(34, 86);
		tte_write("Press START to restart");
	}
}

// -------------------- Game Reset --------------------

static void reset_game(void)
{
	memset(field, 0, sizeof(field));
	score = 0;
	chain = 0;
	fall_ctr = 0;

	gen_random_piece(&nextp);
	spawn_piece();
}

// -------------------- Main --------------------

int main(void)
{
	// Video: Mode 3, BG2
	REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

	// Enable VBlank IRQ so VBlankIntrWait works reliably
	irq_init(NULL);
	irq_add(II_VBLANK, NULL);
	irq_enable(II_VBLANK);

	// Text engine for Mode 3
	tte_init_bmp_default(3);
	tte_set_margins(0,0,240,160);

	// Seed RNG a bit from hardware timers (best-effort)
	g_rng ^= REG_VCOUNT;
	g_rng ^= (u32)REG_TM0CNT_L << 16;
	g_rng ^= (u32)REG_TM1CNT_L;

	reset_game();

	while(1)
	{
		key_poll();

		if(key_hit(KEY_START))
		{
			reset_game();
		}

		if(state == ST_FALLING)
		{
			// Input: moves
			if(key_hit(KEY_LEFT))  try_move(-1, 0);
			if(key_hit(KEY_RIGHT)) try_move(+1, 0);

			// Rotations
			if(key_hit(KEY_A)) try_rotate(+1);
			if(key_hit(KEY_B)) try_rotate(-1);

			// Hard drop
			if(key_hit(KEY_UP))
			{
				while(try_move(0, +1)) { /* fall */ }
				lock_piece();
				state = ST_RESOLVE;
			}

			// Falling timer (soft drop speeds up)
			int interval = base_fall_interval;
			if(key_is_down(KEY_DOWN))
				interval = 2;

			fall_ctr++;
			if(fall_ctr >= interval)
			{
				fall_ctr = 0;
				if(!try_move(0, +1))
				{
					lock_piece();
					state = ST_RESOLVE;
				}
			}
		}

		if(state == ST_RESOLVE)
		{
			resolve_board();
			spawn_piece();
		}

		// Draw during VBlank
		VBlankIntrWait();
		draw_game();
	}

	return 0;
}
