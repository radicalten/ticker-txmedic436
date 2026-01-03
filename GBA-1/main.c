// zork_gba_tonc_singlefile.c
// A tiny Zork-like text adventure for GBA (Mode 3 bitmap) using tonc headers.
//
// Controls (default):
//   UP/DOWN        Pick character
//   LEFT/RIGHT     Jump +/-5 characters
//   A              Add selected character to command line
//   B              Backspace
//   SELECT         Clear command line
//   START          Submit command
//   R + D-PAD      Quick-move (R+UP=N, R+DOWN=S, R+LEFT=W, R+RIGHT=E)
//
// Supported commands (examples):
//   LOOK / L
//   INVENTORY / I
//   GO NORTH  (or: NORTH, N, GO N)
//   TAKE LANTERN
//   DROP KEY
//   USE LANTERN
//   USE KEY
//   USE COIN
//   EXAMINE BOOK
//   HELP
//
// Build note:
//   - This file expects you have tonc available so `#include <tonc.h>` works.
//   - You can link without tonclib because this file avoids tonc helper funcs.
//     It only uses register/key/color macros/types from tonc.h.

#include <tonc.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

// ------------------------------------------------------------
// Video (Mode 3 bitmap) helpers
// ------------------------------------------------------------
#define SCREEN_W 240
#define SCREEN_H 160

static volatile u16 *const VRAM = (volatile u16*)0x06000000;

static inline void vsync(void)
{
	// Wait until we're in VBlank then out of it (simple, no IRQ needed).
	while(REG_VCOUNT >= 160) {}
	while(REG_VCOUNT <  160) {}
}

static inline void pset(int x, int y, u16 clr)
{
	if((unsigned)x < SCREEN_W && (unsigned)y < SCREEN_H)
		VRAM[y*SCREEN_W + x] = clr;
}

static void fill_screen(u16 clr)
{
	for(int i=0; i<SCREEN_W*SCREEN_H; i++)
		VRAM[i] = clr;
}

static void fill_rect(int x, int y, int w, int h, u16 clr)
{
	if(w <= 0 || h <= 0) return;
	if(x < 0) { w += x; x = 0; }
	if(y < 0) { h += y; y = 0; }
	if(x+w > SCREEN_W) w = SCREEN_W-x;
	if(y+h > SCREEN_H) h = SCREEN_H-y;
	if(w <= 0 || h <= 0) return;

	for(int yy=y; yy<y+h; yy++)
	{
		volatile u16 *row = &VRAM[yy*SCREEN_W + x];
		for(int xx=0; xx<w; xx++) row[xx] = clr;
	}
}

// ------------------------------------------------------------
// Minimal 5x7 font (drawn into 6x8 cells with 1px spacing)
// Glyph rows are 5 bits wide: bit4..bit0
// ------------------------------------------------------------
typedef struct Glyph
{
	char c;
	u8 rows[7];
} Glyph;

#define G(ch,r0,r1,r2,r3,r4,r5,r6) { (ch), { (u8)(r0),(u8)(r1),(u8)(r2),(u8)(r3),(u8)(r4),(u8)(r5),(u8)(r6) } }

static const Glyph g_font[] =
{
	// Space + punctuation
	G(' ', 0x00,0x00,0x00,0x00,0x00,0x00,0x00),
	G('.', 0x00,0x00,0x00,0x00,0x00,0x04,0x04),
	G(',', 0x00,0x00,0x00,0x00,0x00,0x04,0x08),
	G('!', 0x04,0x04,0x04,0x04,0x04,0x00,0x04),
	G('?', 0x0E,0x11,0x01,0x02,0x04,0x00,0x04),
	G('\'',0x04,0x04,0x00,0x00,0x00,0x00,0x00),
	G('-', 0x00,0x00,0x00,0x1F,0x00,0x00,0x00),
	G(':', 0x00,0x04,0x04,0x00,0x04,0x04,0x00),
	G('/', 0x01,0x02,0x04,0x08,0x10,0x00,0x00),
	G('>', 0x10,0x08,0x04,0x02,0x04,0x08,0x10),
	G('<', 0x01,0x02,0x04,0x08,0x04,0x02,0x01),
	G('_', 0x00,0x00,0x00,0x00,0x00,0x00,0x1F),
	G('=', 0x00,0x1F,0x00,0x1F,0x00,0x00,0x00),

	// Digits
	G('0', 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E),
	G('1', 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E),
	G('2', 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F),
	G('3', 0x1F,0x02,0x04,0x02,0x01,0x11,0x0E),
	G('4', 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02),
	G('5', 0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E),
	G('6', 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E),
	G('7', 0x1F,0x01,0x02,0x04,0x08,0x08,0x08),
	G('8', 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E),
	G('9', 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C),

	// Uppercase letters A-Z (we’ll map lowercase to these)
	G('A', 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11),
	G('B', 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E),
	G('C', 0x0F,0x10,0x10,0x10,0x10,0x10,0x0F),
	G('D', 0x1E,0x11,0x11,0x11,0x11,0x11,0x1E),
	G('E', 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F),
	G('F', 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10),
	G('G', 0x0F,0x10,0x10,0x13,0x11,0x11,0x0F),
	G('H', 0x11,0x11,0x11,0x1F,0x11,0x11,0x11),
	G('I', 0x0E,0x04,0x04,0x04,0x04,0x04,0x0E),
	G('J', 0x01,0x01,0x01,0x01,0x11,0x11,0x0E),
	G('K', 0x11,0x12,0x14,0x18,0x14,0x12,0x11),
	G('L', 0x10,0x10,0x10,0x10,0x10,0x10,0x1F),
	G('M', 0x11,0x1B,0x15,0x15,0x11,0x11,0x11),
	G('N', 0x11,0x19,0x15,0x13,0x11,0x11,0x11),
	G('O', 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E),
	G('P', 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10),
	G('Q', 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D),
	G('R', 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11),
	G('S', 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E),
	G('T', 0x1F,0x04,0x04,0x04,0x04,0x04,0x04),
	G('U', 0x11,0x11,0x11,0x11,0x11,0x11,0x0E),
	G('V', 0x11,0x11,0x11,0x11,0x11,0x0A,0x04),
	G('W', 0x11,0x11,0x11,0x15,0x15,0x15,0x0A),
	G('X', 0x11,0x11,0x0A,0x04,0x0A,0x11,0x11),
	G('Y', 0x11,0x11,0x0A,0x04,0x04,0x04,0x04),
	G('Z', 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F),
};

static const u8 g_unknown[7] = { 0x0E,0x11,0x01,0x02,0x04,0x00,0x04 }; // '?'

static inline char to_upper_ascii(char c)
{
	if(c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
	return c;
}

static const u8* glyph_rows(char c)
{
	c = to_upper_ascii(c);
	for(size_t i=0; i<sizeof(g_font)/sizeof(g_font[0]); i++)
	{
		if(g_font[i].c == c) return g_font[i].rows;
	}
	return g_unknown;
}

static void draw_char_cell(int col, int row, char c, u16 fg)
{
	// 6x8 cell, text area starts at (0,0)
	int x0 = col*6;
	int y0 = row*8;

	const u8* rows = glyph_rows(c);
	for(int y=0; y<7; y++)
	{
		u8 bits = rows[y];
		for(int x=0; x<5; x++)
		{
			if(bits & (1<<(4-x)))
				pset(x0+x, y0+y, fg);
		}
	}
}

static void draw_text(int col, int row, const char *s, u16 fg)
{
	for(int i=0; s[i] && col < 40; i++, col++)
		draw_char_cell(col, row, s[i], fg);
}

// ------------------------------------------------------------
// Tiny text console (fixed grid: 40 cols x 20 rows)
// Layout:
//   row 0: status
//   row 1..18: log
//   row 19: input
// ------------------------------------------------------------
#define CON_COLS 40
#define CON_ROWS 20
#define LOG_TOP  1
#define LOG_ROWS 18
#define INPUT_ROW 19

static char g_log[LOG_ROWS][CON_COLS+1];
static int  g_log_count = 0; // number of valid lines (<= LOG_ROWS)

static void log_clear(void)
{
	for(int i=0; i<LOG_ROWS; i++)
	{
		memset(g_log[i], ' ', CON_COLS);
		g_log[i][CON_COLS] = '\0';
	}
	g_log_count = 0;
}

static void log_push_blank_line(void)
{
	// Shift up
	for(int i=0; i<LOG_ROWS-1; i++)
		memcpy(g_log[i], g_log[i+1], CON_COLS+1);

	// Clear last
	memset(g_log[LOG_ROWS-1], ' ', CON_COLS);
	g_log[LOG_ROWS-1][CON_COLS] = '\0';
	if(g_log_count < LOG_ROWS) g_log_count++;
}

static void log_write_wrapped(const char *msg)
{
	int col = 0;
	int line = LOG_ROWS-1;

	// Ensure we have at least one line to write into
	if(g_log_count == 0)
	{
		log_push_blank_line();
		line = LOG_ROWS-1;
	}

	// Find current end-of-text in the last line
	for(col=0; col<CON_COLS; col++)
	{
		if(g_log[line][col] == ' ') break;
	}
	if(col >= CON_COLS) { log_push_blank_line(); col = 0; }

	for(const char *p=msg; *p; p++)
	{
		char c = *p;
		if(c == '\n')
		{
			log_push_blank_line();
			col = 0;
			continue;
		}
		if(col >= CON_COLS)
		{
			log_push_blank_line();
			col = 0;
		}
		// Replace tabs with space
		if(c == '\t') c = ' ';
		// We keep original case in strings, but our font maps to uppercase anyway.
		g_log[LOG_ROWS-1][col++] = c;
	}
}

static void log_say(const char *msg)
{
	log_push_blank_line();
	log_write_wrapped(msg);
}

// ------------------------------------------------------------
// Game world
// ------------------------------------------------------------
enum { DIR_N=0, DIR_S, DIR_E, DIR_W, DIR_U, DIR_D, DIR_COUNT };

typedef struct Room
{
	const char *name;
	const char *desc;
	int exits[DIR_COUNT]; // room index or -1
	bool dark;
} Room;

typedef struct Item
{
	const char *name;   // single token for parser
	const char *pretty; // for listing
	const char *desc;
	int loc;            // room index, or -1 for inventory, or -2 for "nowhere"
	bool portable;
} Item;

enum
{
	ROOM_FOYER=0,
	ROOM_HALL,
	ROOM_LIBRARY,
	ROOM_STUDY,
	ROOM_GARDEN,
	ROOM_CELLAR,
	ROOM_TREASURE,
	ROOM_COUNT
};

enum
{
	ITEM_LANTERN=0,
	ITEM_KEY,
	ITEM_COIN,
	ITEM_BOOK,
	ITEM_IDOL,
	ITEM_COUNT
};

static Room g_rooms[ROOM_COUNT] =
{
	// FOYER
	{
		"FOYER",
		"YOU ARE IN A DUSTY FOYER. A HALLWAY LIES NORTH. A GARDEN IS EAST.",
		{ ROOM_HALL, -1, ROOM_GARDEN, -1, -1, -1 },
		false
	},
	// HALL
	{
		"HALL",
		"YOU ARE IN A NARROW HALL. A TRAPDOOR IS SET INTO THE FLOOR.",
		{ -1, ROOM_FOYER, ROOM_LIBRARY, -1, -1, ROOM_CELLAR },
		false
	},
	// LIBRARY
	{
		"LIBRARY",
		"YOU ARE IN A QUIET LIBRARY. BOOKS LINE THE WALLS. A STUDY IS NORTH.",
		{ ROOM_STUDY, -1, -1, ROOM_HALL, -1, -1 },
		false
	},
	// STUDY
	{
		"STUDY",
		"YOU ARE IN A SMALL STUDY. A GATE TO THE EAST HAS A COIN SLOT.",
		{ -1, ROOM_LIBRARY, ROOM_TREASURE, -1, -1, -1 },
		false
	},
	// GARDEN
	{
		"GARDEN",
		"YOU ARE IN AN OVERGROWN GARDEN. VINES CRAWL OVER BROKEN STONE.",
		{ -1, -1, -1, ROOM_FOYER, -1, -1 },
		false
	},
	// CELLAR (dark)
	{
		"CELLAR",
		"YOU ARE IN A COLD CELLAR. WATER DRIPS SOMEWHERE IN THE DARK.",
		{ -1, -1, -1, -1, ROOM_HALL, -1 },
		true
	},
	// TREASURE
	{
		"TREASURE ROOM",
		"YOU ARE IN A TREASURE ROOM. SOMETHING GLITTERS ON A PEDESTAL.",
		{ -1, -1, -1, ROOM_STUDY, -1, -1 },
		false
	},
};

static Item g_items[ITEM_COUNT] =
{
	{ "LANTERN", "A BRASS LANTERN", "AN OLD LANTERN. IT MIGHT STILL WORK.", ROOM_FOYER,  true  },
	{ "KEY",     "A SMALL KEY",     "A SMALL IRON KEY, COLD TO THE TOUCH.", ROOM_GARDEN, true  },
	{ "COIN",    "A SILVER COIN",   "A SILVER COIN WITH STRANGE MARKS.",    ROOM_CELLAR, true  },
	{ "BOOK",    "A THIN BOOK",     "A THIN BOOK. ONE PAGE IS DOG-EARED.",  ROOM_LIBRARY,true  },
	{ "IDOL",    "A GOLD IDOL",     "A GOLD IDOL. IT FEELS UNNATURALLY HEAVY.", ROOM_TREASURE, true },
};

static int  g_player_room = ROOM_FOYER;
static bool g_trapdoor_unlocked = false;
static bool g_gate_open = false;
static bool g_lantern_lit = false;
static bool g_game_won = false;

static bool player_has_item(int item_id)
{
	return g_items[item_id].loc == -1;
}

static int find_item_by_token(const char *tok)
{
	if(!tok || !tok[0]) return -1;
	for(int i=0; i<ITEM_COUNT; i++)
	{
		if(strcmp(tok, g_items[i].name) == 0)
			return i;
	}
	return -1;
}

static bool room_is_visible(int room_id)
{
	if(!g_rooms[room_id].dark) return true;
	return g_lantern_lit && player_has_item(ITEM_LANTERN);
}

static bool item_is_visible_here(int item_id)
{
	if(g_items[item_id].loc != g_player_room) return false;
	return room_is_visible(g_player_room);
}

// ------------------------------------------------------------
// Parsing helpers
// ------------------------------------------------------------
static void str_upper_inplace(char *s)
{
	for(int i=0; s[i]; i++)
	{
		char c = s[i];
		if(c >= 'a' && c <= 'z') s[i] = (char)(c - 'a' + 'A');
	}
}

static int tokenize(char *s, char *out[], int max_out)
{
	int n = 0;
	while(*s)
	{
		while(*s == ' ') s++;
		if(!*s) break;
		if(n >= max_out) break;
		out[n++] = s;
		while(*s && *s != ' ') s++;
		if(*s) { *s = '\0'; s++; }
	}
	return n;
}

static int dir_from_token(const char *t)
{
	if(!t) return -1;
	if(strcmp(t, "N") == 0 || strcmp(t, "NORTH") == 0) return DIR_N;
	if(strcmp(t, "S") == 0 || strcmp(t, "SOUTH") == 0) return DIR_S;
	if(strcmp(t, "E") == 0 || strcmp(t, "EAST")  == 0) return DIR_E;
	if(strcmp(t, "W") == 0 || strcmp(t, "WEST")  == 0) return DIR_W;
	if(strcmp(t, "U") == 0 || strcmp(t, "UP")    == 0) return DIR_U;
	if(strcmp(t, "D") == 0 || strcmp(t, "DOWN")  == 0) return DIR_D;
	return -1;
}

// ------------------------------------------------------------
// Game actions
// ------------------------------------------------------------
static void describe_room(void)
{
	if(!room_is_visible(g_player_room))
	{
		log_say("IT IS PITCH BLACK. YOU CANNOT SEE.");
		log_say("YOU CAN PROBABLY GO UP.");
		return;
	}

	log_say(g_rooms[g_player_room].desc);

	// List items in room
	bool any = false;
	char line[64];

	for(int i=0; i<ITEM_COUNT; i++)
	{
		if(item_is_visible_here(i))
		{
			if(!any)
			{
				log_say("YOU SEE:");
				any = true;
			}
			memset(line, 0, sizeof(line));
			strncpy(line, "- ", sizeof(line)-1);
			strncat(line, g_items[i].pretty, sizeof(line)-1-strlen(line));
			log_say(line);
		}
	}

	// List exits
	char exits[64];
	strcpy(exits, "EXITS: ");

	bool first = true;
	for(int d=0; d<DIR_COUNT; d++)
	{
		int to = g_rooms[g_player_room].exits[d];
		if(to < 0) continue;

		// Gate/trapdoor logic
		if(g_player_room == ROOM_HALL && d == DIR_D && !g_trapdoor_unlocked) continue;
		if(g_player_room == ROOM_STUDY && d == DIR_E && !g_gate_open) continue;

		const char *name = NULL;
		switch(d)
		{
			case DIR_N: name="N"; break;
			case DIR_S: name="S"; break;
			case DIR_E: name="E"; break;
			case DIR_W: name="W"; break;
			case DIR_U: name="U"; break;
			case DIR_D: name="D"; break;
		}

		if(!first) strncat(exits, " ", sizeof(exits)-1-strlen(exits));
		strncat(exits, name, sizeof(exits)-1-strlen(exits));
		first = false;
	}
	log_say(exits);
}

static void show_inventory(void)
{
	bool any = false;
	log_say("YOU ARE CARRYING:");

	for(int i=0; i<ITEM_COUNT; i++)
	{
		if(g_items[i].loc == -1)
		{
			any = true;
			char line[64];
			snprintf(line, sizeof(line), "- %s", g_items[i].pretty);
			log_say(line);
		}
	}

	if(!any)
		log_say("(NOTHING)");
}

static void try_move(int dir)
{
	int to = g_rooms[g_player_room].exits[dir];
	if(to < 0)
	{
		log_say("YOU CANNOT GO THAT WAY.");
		return;
	}

	// Trapdoor lock
	if(g_player_room == ROOM_HALL && dir == DIR_D && !g_trapdoor_unlocked)
	{
		log_say("THE TRAPDOOR IS LOCKED.");
		return;
	}

	// Gate lock
	if(g_player_room == ROOM_STUDY && dir == DIR_E && !g_gate_open)
	{
		log_say("THE GATE IS SHUT. THERE IS A COIN SLOT.");
		return;
	}

	g_player_room = to;

	// Auto-win condition (taking idol is the win, but entering feels good too)
	describe_room();
}

static void try_take(const char *tok)
{
	if(!room_is_visible(g_player_room))
	{
		log_say("YOU FUMBLE IN THE DARK AND FIND NOTHING.");
		return;
	}

	int it = find_item_by_token(tok);
	if(it < 0) { log_say("TAKE WHAT?"); return; }

	if(g_items[it].loc != g_player_room)
	{
		log_say("IT IS NOT HERE.");
		return;
	}
	if(!g_items[it].portable)
	{
		log_say("YOU CANNOT TAKE THAT.");
		return;
	}

	g_items[it].loc = -1;
	log_say("TAKEN.");

	if(it == ITEM_IDOL)
	{
		g_game_won = true;
		log_say("AS YOU LIFT THE IDOL, THE AIR SHIMMERS.");
		log_say("YOU HAVE WON!");
	}
}

static void try_drop(const char *tok)
{
	int it = find_item_by_token(tok);
	if(it < 0) { log_say("DROP WHAT?"); return; }

	if(g_items[it].loc != -1)
	{
		log_say("YOU ARE NOT CARRYING THAT.");
		return;
	}

	g_items[it].loc = g_player_room;
	log_say("DROPPED.");
}

static void try_examine(const char *tok)
{
	int it = find_item_by_token(tok);
	if(it < 0) { log_say("EXAMINE WHAT?"); return; }

	bool in_inv = (g_items[it].loc == -1);
	bool here   = (g_items[it].loc == g_player_room);

	if(here && !room_is_visible(g_player_room))
	{
		log_say("IT IS TOO DARK TO SEE.");
		return;
	}

	if(!in_inv && !here)
	{
		log_say("YOU DO NOT SEE THAT HERE.");
		return;
	}

	log_say(g_items[it].desc);
}

static void try_use(const char *tok)
{
	int it = find_item_by_token(tok);
	if(it < 0) { log_say("USE WHAT?"); return; }

	if(g_items[it].loc != -1)
	{
		log_say("YOU ARE NOT CARRYING THAT.");
		return;
	}

	if(it == ITEM_LANTERN)
	{
		g_lantern_lit = !g_lantern_lit;
		log_say(g_lantern_lit ? "THE LANTERN IS NOW LIT." : "THE LANTERN GOES DARK.");
		return;
	}

	if(it == ITEM_KEY)
	{
		if(g_player_room == ROOM_HALL && !g_trapdoor_unlocked)
		{
			g_trapdoor_unlocked = true;
			log_say("YOU UNLOCK THE TRAPDOOR.");
			return;
		}
		log_say("NOTHING HAPPENS.");
		return;
	}

	if(it == ITEM_COIN)
	{
		if(g_player_room == ROOM_STUDY && !g_gate_open)
		{
			g_gate_open = true;
			log_say("YOU SLIDE THE COIN INTO THE SLOT.");
			log_say("THE GATE CLICKS OPEN.");
			return;
		}
		log_say("YOU JINGLE THE COIN. NOTHING HAPPENS.");
		return;
	}

	log_say("NOTHING HAPPENS.");
}

static void show_help(void)
{
	log_say("COMMANDS:");
	log_say("LOOK (L), INVENTORY (I)");
	log_say("GO NORTH/SOUTH/EAST/WEST/UP/DOWN (OR N/S/E/W/U/D)");
	log_say("TAKE <ITEM>, DROP <ITEM>");
	log_say("USE <ITEM>, EXAMINE <ITEM>");
	log_say("TIP: HOLD R + D-PAD TO QUICK-MOVE.");
}

// ------------------------------------------------------------
// Command execution
// ------------------------------------------------------------
static void exec_command(const char *cmd_in)
{
	char buf[96];
	memset(buf, 0, sizeof(buf));
	strncpy(buf, cmd_in, sizeof(buf)-1);

	// Echo command
	{
		char echo[96];
		snprintf(echo, sizeof(echo), "> %s", buf);
		log_say(echo);
	}

	// Normalize to uppercase for parsing
	str_upper_inplace(buf);

	char *argv[4] = {0};
	int argc = tokenize(buf, argv, 4);

	if(argc == 0) { describe_room(); return; }

	// If game won, still allow LOOK/INVENTORY/HELP, otherwise just celebrate
	if(g_game_won)
	{
		if(strcmp(argv[0], "LOOK")==0 || strcmp(argv[0], "L")==0) { describe_room(); return; }
		if(strcmp(argv[0], "INVENTORY")==0 || strcmp(argv[0], "I")==0) { show_inventory(); return; }
		if(strcmp(argv[0], "HELP")==0) { show_help(); return; }
		log_say("YOU HAVE ALREADY WON. (PRESS SELECT TO CLEAR, OR KEEP EXPLORING.)");
		return;
	}

	// Single-token directions
	{
		int d = dir_from_token(argv[0]);
		if(d >= 0) { try_move(d); return; }
	}

	// GO <dir>
	if(strcmp(argv[0], "GO") == 0 && argc >= 2)
	{
		int d = dir_from_token(argv[1]);
		if(d >= 0) { try_move(d); return; }
		log_say("GO WHERE?");
		return;
	}

	if(strcmp(argv[0], "LOOK") == 0 || strcmp(argv[0], "L") == 0)
	{
		describe_room();
		return;
	}

	if(strcmp(argv[0], "INVENTORY") == 0 || strcmp(argv[0], "I") == 0)
	{
		show_inventory();
		return;
	}

	if(strcmp(argv[0], "HELP") == 0)
	{
		show_help();
		return;
	}

	if((strcmp(argv[0], "TAKE") == 0 || strcmp(argv[0], "GET") == 0) && argc >= 2)
	{
		try_take(argv[1]);
		return;
	}

	if(strcmp(argv[0], "DROP") == 0 && argc >= 2)
	{
		try_drop(argv[1]);
		return;
	}

	if(strcmp(argv[0], "USE") == 0 && argc >= 2)
	{
		try_use(argv[1]);
		return;
	}

	if((strcmp(argv[0], "EXAMINE") == 0 || strcmp(argv[0], "X") == 0) && argc >= 2)
	{
		try_examine(argv[1]);
		return;
	}

	log_say("I DO NOT UNDERSTAND THAT.");
	log_say("TYPE HELP.");
}

// ------------------------------------------------------------
// UI state + rendering
// ------------------------------------------------------------
static const char g_charset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.,!?'-/:=_";
static int  g_sel_idx = 1; // start at 'A'
static char g_cmd[64] = {0};

static u16 g_keys_curr = 0, g_keys_prev = 0;

static void keys_poll(void)
{
	g_keys_prev = g_keys_curr;
	// GBA keys are active-low. KEY_MASK is typically 0x03FF.
	g_keys_curr = (u16)(~REG_KEYINPUT & 0x03FF);
}

static inline bool key_hit(u16 k)  { return (g_keys_curr & (u16)~g_keys_prev) & k; }
static inline bool key_held(u16 k) { return (g_keys_curr & k) != 0; }

static bool key_repeat(u16 k, int delay, int interval, int *timer)
{
	if(key_hit(k))
	{
		*timer = 0;
		return true;
	}
	if(key_held(k))
	{
		(*timer)++;
		if(*timer >= delay && interval > 0 && ((*timer - delay) % interval) == 0)
			return true;
	}
	else
	{
		*timer = 0;
	}
	return false;
}

static void render_status(u16 fg, u16 bg)
{
	// Status bar background
	fill_rect(0, 0, SCREEN_W, 8, bg);

	char line[CON_COLS+1];
	memset(line, ' ', CON_COLS);
	line[CON_COLS] = '\0';

	// Left: room name
	{
		const char *rn = g_rooms[g_player_room].name;
		int i=0;
		for(; rn[i] && i<CON_COLS; i++) line[i] = rn[i];
	}

	// Right: flags
	{
		char flags[64];
		snprintf(flags, sizeof(flags),
			"LANTERN:%s  DOOR:%s  GATE:%s",
			(g_lantern_lit ? "ON" : "OFF"),
			(g_trapdoor_unlocked ? "OPEN" : "LOCK"),
			(g_gate_open ? "OPEN" : "SHUT")
		);

		int len = (int)strlen(flags);
		int start = CON_COLS - len;
		if(start < 0) start = 0;
		for(int i=0; i<len && (start+i)<CON_COLS; i++)
			line[start+i] = flags[i];
	}

	// Draw status text
	for(int c=0; c<CON_COLS; c++)
		draw_char_cell(c, 0, line[c], fg);
}

static void render_log(u16 fg)
{
	for(int r=0; r<LOG_ROWS; r++)
	{
		for(int c=0; c<CON_COLS; c++)
			draw_char_cell(c, LOG_TOP + r, g_log[r][c], fg);
	}
}

static void render_input(u16 fg, u16 bg)
{
	fill_rect(0, INPUT_ROW*8, SCREEN_W, 8, bg);

	char line[CON_COLS+1];
	memset(line, ' ', CON_COLS);
	line[CON_COLS] = '\0';

	// Build: "> <cmd> _  [X]"
	int pos = 0;
	line[pos++] = '>';
	line[pos++] = ' ';

	// cmd
	for(int i=0; g_cmd[i] && pos < CON_COLS-6; i++)
		line[pos++] = g_cmd[i];

	// caret
	if(pos < CON_COLS-6) line[pos++] = '_';

	// selected char indicator at end
	// e.g. " [A]"
	const char sel = g_charset[g_sel_idx];
	int tail = CON_COLS-4;
	line[tail+0] = '[';
	line[tail+1] = sel;
	line[tail+2] = ']';
	line[tail+3] = ' ';

	// Draw
	for(int c=0; c<CON_COLS; c++)
		draw_char_cell(c, INPUT_ROW, line[c], fg);
}

static void render_all(void)
{
	const u16 CLR_BG      = RGB15(2, 2, 4);
	const u16 CLR_FG      = RGB15(31,31,31);
	const u16 CLR_STATUSB = RGB15(31,31,31);
	const u16 CLR_STATUSF = RGB15(0, 0, 0);
	const u16 CLR_INPUTB  = RGB15(0, 0, 0);

	fill_screen(CLR_BG);
	render_status(CLR_STATUSF, CLR_STATUSB);
	render_log(CLR_FG);
	render_input(CLR_FG, CLR_INPUTB);
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main(void)
{
	REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

	log_clear();

	log_say("TINY TONC ADVENTURE");
	log_say("TYPE HELP FOR COMMANDS.");
	describe_room();

	bool dirty = true;

	int t_up=0, t_dn=0, t_lt=0, t_rt=0;

	for(;;)
	{
		vsync();
		keys_poll();

		// Quick-move with R + D-pad
		if(key_held(KEY_R))
		{
			if(key_hit(KEY_UP))    { try_move(DIR_N); dirty = true; }
			if(key_hit(KEY_DOWN))  { try_move(DIR_S); dirty = true; }
			if(key_hit(KEY_LEFT))  { try_move(DIR_W); dirty = true; }
			if(key_hit(KEY_RIGHT)) { try_move(DIR_E); dirty = true; }
		}
		else
		{
			// Character picker (with repeat)
			int charset_len = (int)strlen(g_charset);

			if(key_repeat(KEY_UP, 12, 3, &t_up))
			{
				g_sel_idx = (g_sel_idx + 1) % charset_len;
				dirty = true;
			}
			if(key_repeat(KEY_DOWN, 12, 3, &t_dn))
			{
				g_sel_idx = (g_sel_idx - 1);
				if(g_sel_idx < 0) g_sel_idx = charset_len-1;
				dirty = true;
			}
			if(key_repeat(KEY_RIGHT, 12, 3, &t_rt))
			{
				g_sel_idx = (g_sel_idx + 5) % charset_len;
				dirty = true;
			}
			if(key_repeat(KEY_LEFT, 12, 3, &t_lt))
			{
				g_sel_idx = (g_sel_idx - 5);
				while(g_sel_idx < 0) g_sel_idx += charset_len;
				dirty = true;
			}
		}

		// Add char
		if(key_hit(KEY_A))
		{
			size_t n = strlen(g_cmd);
			if(n+1 < sizeof(g_cmd))
			{
				char ch = g_charset[g_sel_idx];
				g_cmd[n] = ch;
				g_cmd[n+1] = '\0';
				dirty = true;
			}
		}

		// Backspace
		if(key_hit(KEY_B))
		{
			size_t n = strlen(g_cmd);
			if(n > 0)
			{
				g_cmd[n-1] = '\0';
				dirty = true;
			}
		}

		// Clear line
		if(key_hit(KEY_SELECT))
		{
			g_cmd[0] = '\0';
			dirty = true;
		}

		// Submit
		if(key_hit(KEY_START))
		{
			if(g_cmd[0] == '\0')
			{
				describe_room();
			}
			else
			{
				exec_command(g_cmd);
				g_cmd[0] = '\0';
			}
			dirty = true;
		}

		if(dirty)
		{
			render_all();
			dirty = false;
		}
	}

	// Unreachable
	// return 0;
}
