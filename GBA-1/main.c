// panel_pon_like_tonc_single_file.c
// Minimal Panel de Pon / Puzzle League-like prototype for GBA using tonc.
// - 6x12 grid of colored blocks
// - 2-wide cursor, swap with A
// - match 3+ in rows/cols, clear, gravity, chain-clears
// - rising stack over time (hold L to raise faster)
// - single C file, no external art assets (tiles generated at runtime)
//
// Build (typical devkitARM + tonc setup):
// arm-none-eabi-gcc ... panel_pon_like_tonc_single_file.c -ltonc -o game.elf
// (Exact makefile/link flags depend on your environment.)

#include <tonc.h>
#include <stdbool.h>
#include <string.h>

// ---------------------------- Config ---------------------------------

#define COLS        6
#define ROWS        12
#define N_COLORS    6

// BG tile IDs
#define TID_EMPTY   0
#define TID_RED     1
#define TID_GREEN   2
#define TID_BLUE    3
#define TID_YELLOW  4
#define TID_PURPLE  5
#define TID_CYAN    6
#define TID_BORDER  7

// Where the board is drawn on BG0 tilemap (in 8x8 tiles)
#define BOARD_TX    12
#define BOARD_TY    4

// Cursor uses two 8x8 sprites (OBJ) side-by-side
#define CURSOR_OBJ_COUNT 2

// Rising stack timing (in frames at ~60fps)
#define RAISE_INTERVAL_FRAMES  150u

// Initial filled rows at bottom
#define START_FILLED_ROWS  8

// Safety cap for resolve iterations (prevents pathological infinite loops)
#define RESOLVE_ITER_CAP   32

// ---------------------------- State ----------------------------------

static u8 g_board[ROWS][COLS];

static int g_cur_x = 0;   // 0..COLS-2 (cursor spans 2 horizontally)
static int g_cur_y = 0;   // 0..ROWS-1

static u32 g_raise_timer = 0;
static bool g_dirty = true;
static bool g_game_over = false;

// OAM shadow
static OBJ_ATTR g_obj_buffer[128];

// ------------------------ Tiny “art” helpers --------------------------

static inline void tile4bpp_solid(TILE *t, u8 pal_idx)
{
	// TILE.data is u32[8]; each u32 is 8 pixels (4bpp nibbles) for one row.
	const u32 v = (u32)pal_idx * 0x11111111u;
	for(int i=0; i<8; i++)
		t->data[i] = v;
}

static inline void tile4bpp_outline8(TILE *t, u8 pal_idx)
{
	// 8x8 outline: edges = pal_idx, inside = 0 (transparent for OBJ, black for BG).
	// We'll use it for OBJ; palette index 0 is transparent in OBJ.
	u8 px[8][8];
	memset(px, 0, sizeof(px));

	for(int y=0; y<8; y++)
	{
		for(int x=0; x<8; x++)
		{
			if(x==0 || x==7 || y==0 || y==7)
				px[y][x] = pal_idx;
		}
	}

	for(int y=0; y<8; y++)
	{
		u32 row = 0;
		for(int x=0; x<8; x++)
		{
			// pack nibble x into row
			row |= ((u32)(px[y][x] & 0xF)) << (4*x);
		}
		t->data[y] = row;
	}
}

// ---------------------------- Random ---------------------------------

static inline u8 rand_color(void)
{
	// Colors are 1..N_COLORS (tile IDs match palette index in this prototype)
	return (u8)(1 + (qran() % N_COLORS));
}

// --------------------------- Game logic --------------------------------

static void board_clear(void)
{
	memset(g_board, 0, sizeof(g_board));
}

static void board_init(void)
{
	board_clear();

	// Fill bottom START_FILLED_ROWS with random blocks.
	for(int y=ROWS-START_FILLED_ROWS; y<ROWS; y++)
		for(int x=0; x<COLS; x++)
			g_board[y][x] = rand_color();

	// Remove any immediate matches by resolving a few times.
	// (This keeps it from starting with obvious clears.)
	for(int i=0; i<RESOLVE_ITER_CAP; i++)
	{
		// gravity first
		bool moved = false;
		for(int x=0; x<COLS; x++)
		{
			int write = ROWS-1;
			for(int y=ROWS-1; y>=0; y--)
			{
				u8 v = g_board[y][x];
				if(v)
				{
					if(y != write)
					{
						g_board[write][x] = v;
						g_board[y][x] = 0;
						moved = true;
					}
					write--;
				}
			}
		}

		// find matches
		u8 mark[ROWS][COLS];
		memset(mark, 0, sizeof(mark));
		bool any = false;

		// horizontal
		for(int y=0; y<ROWS; y++)
		{
			int x=0;
			while(x < COLS)
			{
				u8 c = g_board[y][x];
				if(!c) { x++; continue; }
				int len=1;
				while(x+len < COLS && g_board[y][x+len]==c) len++;
				if(len >= 3)
				{
					any = true;
					for(int k=0; k<len; k++) mark[y][x+k] = 1;
				}
				x += len;
			}
		}

		// vertical
		for(int x=0; x<COLS; x++)
		{
			int y=0;
			while(y < ROWS)
			{
				u8 c = g_board[y][x];
				if(!c) { y++; continue; }
				int len=1;
				while(y+len < ROWS && g_board[y+len][x]==c) len++;
				if(len >= 3)
				{
					any = true;
					for(int k=0; k<len; k++) mark[y+k][x] = 1;
				}
				y += len;
			}
		}

		if(any)
		{
			for(int y=0; y<ROWS; y++)
				for(int x=0; x<COLS; x++)
					if(mark[y][x]) g_board[y][x] = 0;
		}
		else if(!moved)
			break;
	}

	g_cur_x = 0;
	g_cur_y = ROWS-2;
	g_raise_timer = 0;
	g_game_over = false;
	g_dirty = true;
}

static bool apply_gravity(void)
{
	bool moved = false;
	for(int x=0; x<COLS; x++)
	{
		int write = ROWS-1;
		for(int y=ROWS-1; y>=0; y--)
		{
			u8 v = g_board[y][x];
			if(v)
			{
				if(y != write)
				{
					g_board[write][x] = v;
					g_board[y][x] = 0;
					moved = true;
				}
				write--;
			}
		}
	}
	return moved;
}

static bool find_matches(u8 mark[ROWS][COLS])
{
	memset(mark, 0, ROWS*COLS);
	bool any = false;

	// horizontal
	for(int y=0; y<ROWS; y++)
	{
		int x=0;
		while(x < COLS)
		{
			u8 c = g_board[y][x];
			if(!c) { x++; continue; }
			int len=1;
			while(x+len < COLS && g_board[y][x+len]==c) len++;
			if(len >= 3)
			{
				any = true;
				for(int k=0; k<len; k++) mark[y][x+k] = 1;
			}
			x += len;
		}
	}

	// vertical
	for(int x=0; x<COLS; x++)
	{
		int y=0;
		while(y < ROWS)
		{
			u8 c = g_board[y][x];
			if(!c) { y++; continue; }
			int len=1;
			while(y+len < ROWS && g_board[y+len][x]==c) len++;
			if(len >= 3)
			{
				any = true;
				for(int k=0; k<len; k++) mark[y+k][x] = 1;
			}
			y += len;
		}
	}

	return any;
}

static int clear_marked(const u8 mark[ROWS][COLS])
{
	int cleared = 0;
	for(int y=0; y<ROWS; y++)
	{
		for(int x=0; x<COLS; x++)
		{
			if(mark[y][x] && g_board[y][x])
			{
				g_board[y][x] = 0;
				cleared++;
			}
		}
	}
	return cleared;
}

static void resolve_board(void)
{
	for(int iter=0; iter<RESOLVE_ITER_CAP; iter++)
	{
		bool moved = apply_gravity();

		u8 mark[ROWS][COLS];
		bool any = find_matches(mark);
		if(any)
		{
			clear_marked(mark);
			g_dirty = true;
			continue;
		}

		if(!moved)
			break;
	}
}

static void swap_cursor_pair(void)
{
	const int x = g_cur_x;
	const int y = g_cur_y;

	// cursor spans x and x+1
	u8 *a = &g_board[y][x];
	u8 *b = &g_board[y][x+1];

	// Optional: do nothing if both empty
	if((*a==0) && (*b==0))
		return;

	u8 tmp = *a;
	*a = *b;
	*b = tmp;

	g_dirty = true;
	resolve_board();
}

static void raise_stack(void)
{
	// Game over if anything is already in the top row.
	for(int x=0; x<COLS; x++)
	{
		if(g_board[0][x] != 0)
		{
			g_game_over = true;
			return;
		}
	}

	// Shift everything up by 1 row.
	for(int y=0; y<ROWS-1; y++)
		for(int x=0; x<COLS; x++)
			g_board[y][x] = g_board[y+1][x];

	// New random bottom row.
	for(int x=0; x<COLS; x++)
		g_board[ROWS-1][x] = rand_color();

	g_dirty = true;
	resolve_board();
}

// --------------------------- Rendering ---------------------------------

static void draw_border(void)
{
	SCR_ENTRY *map = se_mem[31];

	// top/bottom border
	for(int x=BOARD_TX-1; x<=BOARD_TX+COLS; x++)
	{
		map[(BOARD_TY-1)*32 + x]       = TID_BORDER;
		map[(BOARD_TY+ROWS)*32 + x]    = TID_BORDER;
	}

	// left/right border
	for(int y=BOARD_TY-1; y<=BOARD_TY+ROWS; y++)
	{
		map[y*32 + (BOARD_TX-1)]       = TID_BORDER;
		map[y*32 + (BOARD_TX+COLS)]    = TID_BORDER;
	}
}

static void draw_board(void)
{
	SCR_ENTRY *map = se_mem[31];

	// draw cells
	for(int y=0; y<ROWS; y++)
	{
		for(int x=0; x<COLS; x++)
		{
			const u16 tid = (u16)g_board[y][x]; // 0..6
			map[(BOARD_TY+y)*32 + (BOARD_TX+x)] = tid;
		}
	}

	draw_border();
}

static void update_cursor_objs(void)
{
	if(g_game_over)
	{
		for(int i=0; i<CURSOR_OBJ_COUNT; i++)
			obj_hide(&g_obj_buffer[i]);
		return;
	}

	const int px = (BOARD_TX + g_cur_x)*8;
	const int py = (BOARD_TY + g_cur_y)*8;

	// left highlight
	obj_unhide(&g_obj_buffer[0], 0);
	obj_set_pos(&g_obj_buffer[0], px, py);

	// right highlight
	obj_unhide(&g_obj_buffer[1], 0);
	obj_set_pos(&g_obj_buffer[1], px+8, py);
}

// ------------------------- Video setup ---------------------------------

static void video_init(void)
{
	REG_DISPCNT = DCNT_MODE0 | DCNT_BG0 | DCNT_OBJ | DCNT_OBJ_1D;
	REG_BG0CNT  = BG_CBB(0) | BG_SBB(31) | BG_4BPP | BG_REG_32x32;

	// BG palette (indices match tile “colors” in this prototype)
	pal_bg_mem[0] = RGB15(0,0,0);       // empty
	pal_bg_mem[1] = RGB15(31,6,6);      // red
	pal_bg_mem[2] = RGB15(6,31,6);      // green
	pal_bg_mem[3] = RGB15(6,6,31);      // blue
	pal_bg_mem[4] = RGB15(31,31,6);     // yellow
	pal_bg_mem[5] = RGB15(31,6,31);     // purple
	pal_bg_mem[6] = RGB15(6,31,31);     // cyan
	pal_bg_mem[7] = RGB15(12,12,12);    // border

	// Generate BG tiles: solid fills for IDs 0..7
	for(int tid=0; tid<=TID_BORDER; tid++)
		tile4bpp_solid(&tile_mem[0][tid], (u8)tid);

	// Clear map
	memset16(se_mem[31], 0, 32*32);

	// OBJ palette for cursor outline
	pal_obj_mem[0] = RGB15(0,0,0);       // transparent (index 0)
	pal_obj_mem[1] = RGB15(31,31,31);    // white outline

	// Cursor tile at OBJ tile 0
	tile4bpp_outline8(&tile_mem_obj[0], 1);

	// Init OAM shadow and create 2 cursor sprites
	oam_init(g_obj_buffer, 128);

	for(int i=0; i<CURSOR_OBJ_COUNT; i++)
	{
		// 8x8 square, tile 0, palette bank 0, priority 0
		obj_set_attr(&g_obj_buffer[i],
			ATTR0_SQUARE,
			ATTR1_SIZE_8,
			ATTR2_BUILD(0, 0, 0));
		obj_hide(&g_obj_buffer[i]);
	}
}

// ------------------------------ Main -----------------------------------

int main(void)
{
	// Seed PRNG. (If you want more variety, you can mix in REG_VCOUNT/keys, etc.)
	sqran(0xC0FFEEu);

	video_init();
	board_init();

	u32 frame = 0;

	while(1)
	{
		key_poll();

		if(!g_game_over)
		{
			// Cursor movement with simple key-repeat.
			const u16 held = key_curr;
			const bool repeat_tick = ((frame & 7u) == 0);

			if(key_hit(KEY_LEFT)  || (repeat_tick && (held & KEY_LEFT)))  { if(g_cur_x > 0)        g_cur_x--; }
			if(key_hit(KEY_RIGHT) || (repeat_tick && (held & KEY_RIGHT))) { if(g_cur_x < COLS-2)   g_cur_x++; }
			if(key_hit(KEY_UP)    || (repeat_tick && (held & KEY_UP)))    { if(g_cur_y > 0)        g_cur_y--; }
			if(key_hit(KEY_DOWN)  || (repeat_tick && (held & KEY_DOWN)))  { if(g_cur_y < ROWS-1)   g_cur_y++; }

			if(key_hit(KEY_A))
				swap_cursor_pair();

			// Rising stack: hold L to speed up.
			g_raise_timer += (held & KEY_L) ? 3u : 1u;
			if(g_raise_timer >= RAISE_INTERVAL_FRAMES)
			{
				g_raise_timer = 0;
				raise_stack();
			}
		}
		else
		{
			// Restart
			if(key_hit(KEY_START) || key_hit(KEY_A))
				board_init();
		}

		update_cursor_objs();

		// VBlank section
		vid_vsync();

		if(g_dirty)
		{
			draw_board();
			g_dirty = false;
		}

		oam_copy(oam_mem, g_obj_buffer, 128);

		frame++;
	}

	// not reached
	// return 0;
}
