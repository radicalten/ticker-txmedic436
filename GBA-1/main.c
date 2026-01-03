// zork_like_gba.c
//
// Minimal Zork-like text adventure for GBA using tonc.
// Single-file, mode 3, with a tiny built‑in 3x5 font and
// a simple on-screen keyboard controlled by the D‑pad.
//
// Controls:
//   D-Pad : Move selection on keyboard
//   A     : Add selected letter (or space) to command
//   B     : Backspace
//   START : Submit command
//
// Build (example):
//   arm-none-eabi-gcc -std=c99 -O2 -mthumb -mthumb-interwork \
//       -I/path/to/tonc/include -L/path/to/tonc/lib \
//       zork_like_gba.c -ltonc -o zork_like_gba.elf
//
// Then convert ELF to GBA with your usual toolchain (e.g. objcopy).

#include <string.h>
#include "tonc.h"

// -----------------------------------------------------------------------------
// Basic video setup (Mode 3)
// -----------------------------------------------------------------------------

#define SCREEN_W 240
#define SCREEN_H 160

static u16 *const m3_fb = (u16 *)0x6000000;

// Simple busy-wait VBlank sync (no interrupts needed).
static void vsync(void)
{
    while (REG_VCOUNT >= 160);
    while (REG_VCOUNT < 160);
}

// Colors (RGB15, 0-31 each)
#define CLR_BG     RGB15(0, 0, 0)
#define CLR_TEXT   RGB15(31, 31, 31)
#define CLR_TITLE  RGB15(0, 31, 0)
#define CLR_HUD    RGB15(31, 31, 0)
#define CLR_SEL    RGB15(31, 0, 0)

// -----------------------------------------------------------------------------
// Tiny 3x5 font (scaled 2x to 6x10, in 8x12 cells)
// -----------------------------------------------------------------------------

#define FONT_SCALE  2
#define FONT_W      3
#define FONT_H      5
#define CELL_W      ((FONT_W+1)*FONT_SCALE)   // 8 px
#define CELL_H      ((FONT_H+1)*FONT_SCALE)   // 12 px
#define TEXT_COLS   (SCREEN_W / CELL_W)       // 30 columns
#define TEXT_ROWS   (SCREEN_H / CELL_H)       // 13 rows

typedef unsigned char u8;

#define B3(a,b,c)  ((u8)(((a)<<2)|((b)<<1)|(c)))

// Characters 32 (' ') .. 95 ('_')
static const u8 font_3x5[64][5] = {
    // 32 ' '
    [32-32] = { 0, 0, 0, 0, 0 },

    // 33 '!'
    [33-32] = {
        B3(0,1,0),
        B3(0,1,0),
        B3(0,1,0),
        B3(0,0,0),
        B3(0,1,0)
    },

    // 46 '.'
    [46-32] = {
        B3(0,0,0),
        B3(0,0,0),
        B3(0,0,0),
        B3(0,0,0),
        B3(0,1,0)
    },

    // 65 'A'
    [65-32] = {
        B3(0,1,0),
        B3(1,0,1),
        B3(1,1,1),
        B3(1,0,1),
        B3(1,0,1)
    },
    // 66 'B'
    [66-32] = {
        B3(1,1,0),
        B3(1,0,1),
        B3(1,1,0),
        B3(1,0,1),
        B3(1,1,0)
    },
    // 67 'C'
    [67-32] = {
        B3(0,1,1),
        B3(1,0,0),
        B3(1,0,0),
        B3(1,0,0),
        B3(0,1,1)
    },
    // 68 'D'
    [68-32] = {
        B3(1,1,0),
        B3(1,0,1),
        B3(1,0,1),
        B3(1,0,1),
        B3(1,1,0)
    },
    // 69 'E'
    [69-32] = {
        B3(1,1,1),
        B3(1,0,0),
        B3(1,1,0),
        B3(1,0,0),
        B3(1,1,1)
    },
    // 70 'F'
    [70-32] = {
        B3(1,1,1),
        B3(1,0,0),
        B3(1,1,0),
        B3(1,0,0),
        B3(1,0,0)
    },
    // 71 'G'
    [71-32] = {
        B3(0,1,1),
        B3(1,0,0),
        B3(1,0,1),
        B3(1,0,1),
        B3(0,1,1)
    },
    // 72 'H'
    [72-32] = {
        B3(1,0,1),
        B3(1,0,1),
        B3(1,1,1),
        B3(1,0,1),
        B3(1,0,1)
    },
    // 73 'I'
    [73-32] = {
        B3(1,1,1),
        B3(0,1,0),
        B3(0,1,0),
        B3(0,1,0),
        B3(1,1,1)
    },
    // 74 'J'
    [74-32] = {
        B3(0,0,1),
        B3(0,0,1),
        B3(0,0,1),
        B3(0,0,1),
        B3(1,1,0)
    },
    // 75 'K'
    [75-32] = {
        B3(1,0,1),
        B3(1,0,1),
        B3(1,1,0),
        B3(1,0,1),
        B3(1,0,1)
    },
    // 76 'L'
    [76-32] = {
        B3(1,0,0),
        B3(1,0,0),
        B3(1,0,0),
        B3(1,0,0),
        B3(1,1,1)
    },
    // 77 'M'
    [77-32] = {
        B3(1,0,1),
        B3(1,1,1),
        B3(1,0,1),
        B3(1,0,1),
        B3(1,0,1)
    },
    // 78 'N'
    [78-32] = {
        B3(1,0,0),
        B3(1,1,0),
        B3(1,0,1),
        B3(1,0,1),
        B3(1,0,1)
    },
    // 79 'O'
    [79-32] = {
        B3(0,1,0),
        B3(1,0,1),
        B3(1,0,1),
        B3(1,0,1),
        B3(0,1,0)
    },
    // 80 'P'
    [80-32] = {
        B3(1,1,0),
        B3(1,0,1),
        B3(1,1,0),
        B3(1,0,0),
        B3(1,0,0)
    },
    // 81 'Q'
    [81-32] = {
        B3(0,1,0),
        B3(1,0,1),
        B3(1,0,1),
        B3(0,1,0),
        B3(0,0,1)
    },
    // 82 'R'
    [82-32] = {
        B3(1,1,0),
        B3(1,0,1),
        B3(1,1,0),
        B3(1,0,1),
        B3(1,0,1)
    },
    // 83 'S'
    [83-32] = {
        B3(1,1,1),
        B3(1,0,0),
        B3(1,1,1),
        B3(0,0,1),
        B3(1,1,1)
    },
    // 84 'T'
    [84-32] = {
        B3(1,1,1),
        B3(0,1,0),
        B3(0,1,0),
        B3(0,1,0),
        B3(0,1,0)
    },
    // 85 'U'
    [85-32] = {
        B3(1,0,1),
        B3(1,0,1),
        B3(1,0,1),
        B3(1,0,1),
        B3(0,1,1)
    },
    // 86 'V'
    [86-32] = {
        B3(1,0,1),
        B3(1,0,1),
        B3(1,0,1),
        B3(0,1,0),
        B3(0,1,0)
    },
    // 87 'W'
    [87-32] = {
        B3(1,0,1),
        B3(1,0,1),
        B3(1,0,1),
        B3(1,1,1),
        B3(1,1,1)
    },
    // 88 'X'
    [88-32] = {
        B3(1,0,1),
        B3(1,0,1),
        B3(0,1,0),
        B3(1,0,1),
        B3(1,0,1)
    },
    // 89 'Y'
    [89-32] = {
        B3(1,0,1),
        B3(1,0,1),
        B3(0,1,0),
        B3(0,1,0),
        B3(0,1,0)
    },
    // 90 'Z'
    [90-32] = {
        B3(1,1,1),
        B3(0,0,1),
        B3(0,1,0),
        B3(1,0,0),
        B3(1,1,1)
    },

    // 95 '_'
    [95-32] = {
        B3(0,0,0),
        B3(0,0,0),
        B3(0,0,0),
        B3(1,1,1),
        B3(0,0,0)
    }
};

static void m3_clear(u16 clr)
{
    u32 packed = (clr << 16) | clr;
    u32 *dst = (u32 *)m3_fb;
    int count = (SCREEN_W * SCREEN_H) / 2;
    for (int i = 0; i < count; i++)
        dst[i] = packed;
}

static void draw_char_cell(int tx, int ty, char ch, u16 clr)
{
    if (tx < 0 || tx >= TEXT_COLS || ty < 0 || ty >= TEXT_ROWS)
        return;
    if (ch < 32 || ch > 95)
        return;

    const u8 *glyph = font_3x5[ch - 32];
    int x0 = tx * CELL_W;
    int y0 = ty * CELL_H;

    for (int row = 0; row < FONT_H; row++)
    {
        u8 bits = glyph[row];
        for (int col = 0; col < FONT_W; col++)
        {
            if (bits & (1 << (FONT_W - 1 - col)))
            {
                int px = x0 + col * FONT_SCALE;
                int py = y0 + row * FONT_SCALE;
                for (int dy = 0; dy < FONT_SCALE; dy++)
                    for (int dx = 0; dx < FONT_SCALE; dx++)
                        m3_plot(px + dx, py + dy, clr);
            }
        }
    }
}

static void draw_text_cell(int tx, int ty, const char *str, u16 clr)
{
    int x = tx;
    int y = ty;

    for (int i = 0; str[i] != '\0'; i++)
    {
        char c = str[i];
        if (c == '\n')
        {
            x = tx;
            y++;
            if (y >= TEXT_ROWS)
                break;
            continue;
        }

        draw_char_cell(x, y, c, clr);
        x++;
        if (x >= TEXT_COLS)
        {
            x = tx;
            y++;
            if (y >= TEXT_ROWS)
                break;
        }
    }
}

// -----------------------------------------------------------------------------
// Simple key input (no tonc helper functions, just REG_KEYINPUT)
// -----------------------------------------------------------------------------

#ifndef KEY_MASK
#define KEY_MASK 0x03FF
#endif

static u16 keys_curr = 0;
static u16 keys_prev = 0;

static inline void key_poll_simple(void)
{
    keys_prev = keys_curr;
    keys_curr = (u16)(~REG_KEYINPUT & KEY_MASK);
}

static inline int key_hit(u16 key)
{
    return (keys_curr & ~keys_prev) & key;
}

// -----------------------------------------------------------------------------
// Game data
// -----------------------------------------------------------------------------

enum { DIR_NORTH = 0, DIR_EAST = 1, DIR_SOUTH = 2, DIR_WEST = 3 };

typedef struct
{
    const char *name;
    const char *desc;
    int exits[4];      // {N, E, S, W}, -1 if no exit
} Room;

typedef struct
{
    const char *name;
    const char *desc;
    int location;      // room index, or LOC_INVENTORY, or LOC_NOWHERE
} Item;

#define ROOM_FOREST    0
#define ROOM_CAVE_ENT  1
#define ROOM_TUNNEL    2
#define ROOM_CHAMBER   3

#define ITEM_LAMP      0
#define ITEM_TREASURE  1

#define LOC_INVENTORY  (-1)
#define LOC_NOWHERE    (-2)

static Room rooms[] =
{
    {
        "FOREST CLEARING",
        "YOU ARE IN A SMALL FOREST CLEARING.\nA PATH LEADS NORTH TO A CAVE ENTRANCE.",
        { ROOM_CAVE_ENT, -1, -1, -1 }
    },
    {
        "CAVE ENTRANCE",
        "YOU STAND AT THE MOUTH OF A DARK CAVE.\nTHE FOREST CLEARING IS SOUTH.",
        { ROOM_TUNNEL, -1, ROOM_FOREST, -1 }
    },
    {
        "DARK TUNNEL",
        "YOU ARE IN A NARROW TUNNEL.\nTHE CAVE ENTRANCE IS SOUTH.",
        { ROOM_CHAMBER, -1, ROOM_CAVE_ENT, -1 }
    },
    {
        "UNDERGROUND CHAMBER",
        "YOU ARE IN A SMALL UNDERGROUND CHAMBER.\nTHE ONLY EXIT IS SOUTH.",
        { -1, -1, ROOM_TUNNEL, -1 }
    }
};

static Item items[] =
{
    { "LAMP",     "AN OLD BUT WORKING LAMP.",   ROOM_CAVE_ENT },
    { "TREASURE", "A SMALL CHEST OF GOLD.",     ROOM_CHAMBER  }
};

static int current_room = ROOM_FOREST;
static int game_over    = 0;
static int game_won     = 0;

static char last_msg[80] = "";

// -----------------------------------------------------------------------------
// Game helpers
// -----------------------------------------------------------------------------

static int has_item(int itemId)
{
    return items[itemId].location == LOC_INVENTORY;
}

static void set_msg(const char *s)
{
    strncpy(last_msg, s, sizeof(last_msg)-1);
    last_msg[sizeof(last_msg)-1] = '\0';
}

static int match_word(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

// Split on spaces, in-place. Returns word count.
static int tokenize(char *s, char *words[], int max_words)
{
    int count = 0;
    char *p = s;

    while (*p != '\0' && count < max_words)
    {
        while (*p == ' ') p++;
        if (*p == '\0')
            break;

        words[count++] = p;

        while (*p != '\0' && *p != ' ')
            p++;

        if (*p == ' ')
        {
            *p = '\0';
            p++;
        }
    }
    return count;
}

static int parse_dir(const char *w)
{
    if (match_word(w, "N") || match_word(w, "NORTH"))
        return DIR_NORTH;
    if (match_word(w, "E") || match_word(w, "EAST"))
        return DIR_EAST;
    if (match_word(w, "S") || match_word(w, "SOUTH"))
        return DIR_SOUTH;
    if (match_word(w, "W") || match_word(w, "WEST"))
        return DIR_WEST;
    return -1;
}

// -----------------------------------------------------------------------------
// Command implementations
// -----------------------------------------------------------------------------

static void do_look(void)
{
    set_msg("YOU LOOK AROUND.");
}

static void do_help(void)
{
    set_msg("TRY GO NORTH TAKE LAMP LOOK INVENTORY HELP.");
}

static void do_go(int dir)
{
    Room *r = &rooms[current_room];
    int dest = r->exits[dir];

    if (dest < 0)
    {
        set_msg("YOU CANNOT GO THAT WAY.");
        return;
    }

    // Simple puzzle: need lamp to enter the dark tunnel
    if (dest == ROOM_TUNNEL && !has_item(ITEM_LAMP))
    {
        set_msg("IT IS TOO DARK TO ENTER WITHOUT A LAMP.");
        return;
    }

    current_room = dest;
    set_msg("OK.");
}

static void do_take(const char *noun)
{
    for (unsigned i = 0; i < sizeof(items)/sizeof(items[0]); i++)
    {
        if (items[i].location == current_room &&
            match_word(items[i].name, noun))
        {
            items[i].location = LOC_INVENTORY;
            set_msg("TAKEN.");
            return;
        }
    }
    set_msg("YOU DO NOT SEE THAT HERE.");
}

static void do_drop(const char *noun)
{
    for (unsigned i = 0; i < sizeof(items)/sizeof(items[0]); i++)
    {
        if (items[i].location == LOC_INVENTORY &&
            match_word(items[i].name, noun))
        {
            items[i].location = current_room;
            set_msg("DROPPED.");
            return;
        }
    }
    set_msg("YOU ARE NOT CARRYING THAT.");
}

static void do_inventory(void)
{
    char buf[80];
    int count = 0;

    buf[0] = '\0';
    strcpy(buf, "YOU CARRY");

    for (unsigned i = 0; i < sizeof(items)/sizeof(items[0]); i++)
    {
        if (items[i].location == LOC_INVENTORY)
        {
            if (count == 0)
                strcat(buf, " ");
            else
                strcat(buf, " AND ");

            strcat(buf, items[i].name);
            count++;
        }
    }

    if (count == 0)
    {
        set_msg("YOU ARE CARRYING NOTHING.");
        return;
    }

    strcat(buf, ".");
    set_msg(buf);
}

// -----------------------------------------------------------------------------
// Text for exits/items
// -----------------------------------------------------------------------------

static void build_exits_text(char *buf, int buf_size)
{
    Room *r = &rooms[current_room];
    int first = 1;

    buf[0] = '\0';
    strncpy(buf, "EXITS", buf_size-1);
    buf[buf_size-1] = '\0';

    if (r->exits[DIR_NORTH] >= 0)
    {
        strncat(buf, " NORTH", buf_size-1 - strlen(buf));
        first = 0;
    }
    if (r->exits[DIR_EAST] >= 0)
    {
        strncat(buf, first ? " EAST" : " EAST", buf_size-1 - strlen(buf));
        first = 0;
    }
    if (r->exits[DIR_SOUTH] >= 0)
    {
        strncat(buf, first ? " SOUTH" : " SOUTH", buf_size-1 - strlen(buf));
        first = 0;
    }
    if (r->exits[DIR_WEST] >= 0)
    {
        strncat(buf, first ? " WEST" : " WEST", buf_size-1 - strlen(buf));
        first = 0;
    }

    if (first)
    {
        strncpy(buf, "NO EXITS.", buf_size-1);
        buf[buf_size-1] = '\0';
    }
}

static void build_items_text(char *buf, int buf_size)
{
    int any = 0;

    buf[0] = '\0';
    strncpy(buf, "YOU SEE", buf_size-1);
    buf[buf_size-1] = '\0';

    for (unsigned i = 0; i < sizeof(items)/sizeof(items[0]); i++)
    {
        if (items[i].location == current_room)
        {
            strncat(buf, " ", buf_size-1 - strlen(buf));
            strncat(buf, items[i].name, buf_size-1 - strlen(buf));
            any = 1;
        }
    }

    if (any)
    {
        strncat(buf, " HERE.", buf_size-1 - strlen(buf));
    }
    else
    {
        strncpy(buf, "YOU SEE NOTHING HERE.", buf_size-1);
        buf[buf_size-1] = '\0';
    }
}

// -----------------------------------------------------------------------------
// On-screen keyboard
// -----------------------------------------------------------------------------

#define KB_COLS 9
#define KB_ROWS 3
#define KB_X0   1     // starting column for keyboard
#define KB_Y0   10    // starting row for keyboard

static void draw_keyboard(int sel)
{
    for (int r = 0; r < KB_ROWS; r++)
    {
        for (int c = 0; c < KB_COLS; c++)
        {
            int idx = r * KB_COLS + c;
            if (idx > 26)
                continue;

            char ch;
            if (idx < 26)
                ch = 'A' + idx;
            else
                ch = '_';       // label for space

            u16 col = (idx == sel) ? CLR_SEL : CLR_HUD;
            int tx = KB_X0 + c * 2;
            int ty = KB_Y0 + r;
            draw_char_cell(tx, ty, ch, col);
        }
    }
}

// -----------------------------------------------------------------------------
// Screen drawing
// -----------------------------------------------------------------------------

static void draw_world_view(const char *cmd, int kb_sel)
{
    m3_clear(CLR_BG);

    Room *r = &rooms[current_room];

    // Room name and description
    draw_text_cell(0, 0, r->name, CLR_TITLE);
    draw_text_cell(0, 1, r->desc, CLR_TEXT);   // uses embedded '\n'

    // Exits
    char buf[80];
    build_exits_text(buf, sizeof(buf));
    draw_text_cell(0, 4, buf, CLR_TEXT);

    // Items
    build_items_text(buf, sizeof(buf));
    draw_text_cell(0, 5, buf, CLR_TEXT);

    // Last message
    if (last_msg[0] != '\0')
        draw_text_cell(0, 6, last_msg, CLR_HUD);

    if (game_over)
    {
        draw_text_cell(0, 8, "GAME OVER.", CLR_HUD);
        return;
    }

    // Keyboard help line
    draw_text_cell(0, 8, "B BACKSPACE  START OK", CLR_HUD);

    // Command line
    char cmdline[40];
    cmdline[0] = '\0';
    strncpy(cmdline, "COMMAND ", sizeof(cmdline)-1);
    cmdline[sizeof(cmdline)-1] = '\0';
    strncat(cmdline, cmd, sizeof(cmdline)-1 - strlen(cmdline));
    draw_text_cell(0, 9, cmdline, CLR_TEXT);

    // On-screen keyboard
    if (kb_sel >= 0)
        draw_keyboard(kb_sel);
}

// -----------------------------------------------------------------------------
// Command input (reads a line using the on-screen keyboard)
// -----------------------------------------------------------------------------

static void get_command(char *out_cmd, int max_len)
{
    int sel = 0;          // 0..26 (A..Z, space)
    int len = 0;

    out_cmd[0] = '\0';

    while (1)
    {
        vsync();
        key_poll_simple();

        // Move selection
        if (key_hit(KEY_LEFT))
        {
            if (sel % KB_COLS > 0)
                sel--;
        }
        if (key_hit(KEY_RIGHT))
        {
            if (sel % KB_COLS < KB_COLS - 1 && sel + 1 <= 26)
                sel++;
        }
        if (key_hit(KEY_UP))
        {
            if (sel >= KB_COLS)
                sel -= KB_COLS;
        }
        if (key_hit(KEY_DOWN))
        {
            if (sel + KB_COLS <= 26)
                sel += KB_COLS;
        }

        // Add character
        if (key_hit(KEY_A))
        {
            char ch;
            if (sel < 26)
                ch = (char)('A' + sel);
            else
                ch = ' ';

            if (len < max_len - 1)
            {
                out_cmd[len++] = ch;
                out_cmd[len] = '\0';
            }
        }

        // Backspace
        if (key_hit(KEY_B))
        {
            if (len > 0)
            {
                len--;
                out_cmd[len] = '\0';
            }
        }

        // Submit
        if (key_hit(KEY_START))
        {
            break;
        }

        draw_world_view(out_cmd, sel);
    }

    // Trim leading and trailing spaces
    // (parser already copes, but this keeps things neat)
    int start = 0;
    while (start < len && out_cmd[start] == ' ')
        start++;

    int end = len - 1;
    while (end >= start && out_cmd[end] == ' ')
        end--;

    int new_len = 0;
    for (int i = start; i <= end; i++)
        out_cmd[new_len++] = out_cmd[i];

    out_cmd[new_len] = '\0';
}

// -----------------------------------------------------------------------------
// Command dispatcher
// -----------------------------------------------------------------------------

static void handle_command(char *cmd)
{
    char *words[4];
    int wc = tokenize(cmd, words, 4);

    if (wc == 0)
    {
        set_msg("TYPE A COMMAND FIRST.");
        return;
    }

    char *v = words[0];

    // LOOK
    if (match_word(v, "LOOK") || match_word(v, "L"))
    {
        do_look();
        goto after;
    }

    // HELP
    if (match_word(v, "HELP"))
    {
        do_help();
        goto after;
    }

    // One-word direction: N, NORTH, etc.
    {
        int dir = parse_dir(v);
        if (dir >= 0)
        {
            do_go(dir);
            goto after;
        }
    }

    // GO <dir>
    if (match_word(v, "GO") || match_word(v, "G"))
    {
        if (wc < 2)
        {
            set_msg("GO WHERE");
            goto after;
        }
        int dir = parse_dir(words[1]);
        if (dir < 0)
            set_msg("I DO NOT KNOW THAT WAY.");
        else
            do_go(dir);
        goto after;
    }

    // TAKE / GET <item>
    if (match_word(v, "TAKE") || match_word(v, "GET"))
    {
        if (wc < 2)
        {
            set_msg("TAKE WHAT");
            goto after;
        }
        do_take(words[1]);
        goto after;
    }

    // DROP <item>
    if (match_word(v, "DROP"))
    {
        if (wc < 2)
        {
            set_msg("DROP WHAT");
            goto after;
        }
        do_drop(words[1]);
        goto after;
    }

    // INVENTORY / I
    if (match_word(v, "INVENTORY") || match_word(v, "I"))
    {
        do_inventory();
        goto after;
    }

    // QUIT / Q
    if (match_word(v, "QUIT") || match_word(v, "Q"))
    {
        set_msg("GOODBYE.");
        game_over = 1;
        return;
    }

    // Unknown
    set_msg("I DO NOT UNDERSTAND.");

after:
    // Win condition: have treasure and be back in forest
    if (!game_won && has_item(ITEM_TREASURE) && current_room == ROOM_FOREST)
    {
        set_msg("YOU ESCAPED WITH THE TREASURE!");
        game_won  = 1;
        game_over = 1;
    }
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(void)
{
    REG_DISPCNT = DCNT_MODE3 | DCNT_BG2;

    current_room = ROOM_FOREST;
    game_over    = 0;
    game_won     = 0;

    set_msg("WELCOME. TYPE HELP FOR HELP.");

    key_poll_simple(); // prime key state

    char cmd[32];

    while (!game_over)
    {
        get_command(cmd, sizeof(cmd));
        handle_command(cmd);
    }

    // Final screen: no further input, just show result
    for (;;)
    {
        vsync();
        draw_world_view("", -1);
    }

    return 0;
}
