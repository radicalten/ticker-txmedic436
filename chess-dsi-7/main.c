/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  See the
  GNU General Public License for more details.
*/

#define IS_GUI // Tells thread.h to let this file write directly to the screens
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

// Stockfish Engine Headers
#include "bitboard.h"
#include "endgame.h"
#include "pawns.h"
#include "polybook.h"
#include "position.h"
#include "movegen.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "tbprobe.h"

// OPTIMIZATION: Reduced from 2048 to 512 to reclaim RAM, preventing thread spawn failure.
#define MAX_HISTORY 512

// Forward declaration: topConsole is defined further down in this file, but
// gui_alloc_position()/gui_reset_game() (used for early boot diagnostics)
// are defined earlier and need to reference it.
extern PrintConsole topConsole;

// ==========================================================================
// ENGINE ADAPTER
// ==========================================================================
// The GUI's game-state, legality checking, check detection, SAN generation,
// draw detection, and move application are now driven entirely by Cfish's
// real Position (position.h / movegen.h) - the exact same code the search
// thread uses - instead of a separate hand-rolled mailbox rules engine.
//
// IMPORTANT: g_pos is a completely separate, GUI-private Position. It is
// NEVER registered into Threads.pos[] and NEVER passed to
// threads_start_thinking()/thread_search(). The engine subprocess's own
// search thread has its own Position (Threads.pos[0], built by
// thread_init() in thread.c) which the GUI must never touch directly - all
// GUI<->engine communication continues to go over the existing UCI text
// bridge (sf_send_command / sf_get_output), exactly as before.
//
// DESIGN NOTE / ASSUMPTION (please verify against movegen.c / position.c if
// possible): do_move(), undo_move(), is_legal(), is_pseudo_legal(),
// gives_check_special(), generate_legal(), and is_draw() are assumed to
// touch only board/bitboard/Stack state - never pos->pawnTable,
// materialTable, mainHistory, captureHistory, lowPlyHistory,
// counterMoveHistory, counterMoves, rootMoves, or moveList. Those are
// search/move-ordering/evaluation-only structures in every known
// Stockfish/Cfish version (thread_init() in thread.c only allocates them
// for use by movepick.c/search.c/pawns.c/material.c). We deliberately leave
// them NULL here rather than duplicating thread_init()'s multi-megabyte
// allocations (CounterMoveHistoryStat alone is ~8MB - duplicating that for
// a second, GUI-only Position is not viable on DSi's RAM budget). If this
// assumption is wrong for this fork, the failure mode is an immediate,
// deterministic NULL-pointer crash on the very first do_move() call - not a
// subtle/rare bug - so it will surface immediately in testing.
// ==========================================================================

// The GUI's live game position.
static Position g_pos;

// Stack buffer backing g_pos->st.
//
// OPTIMIZATION / BUGFIX: this now mirrors thread.c's own thread_init()
// sizing EXACTLY (63 + (MAX_PLY + 110) slots), rather than an independently
// guessed (128 + MAX_HISTORY + 128) size. That original guess was ~45%
// larger and is suspected to have caused a silent calloc() failure (hard
// black screen, no output) given how tight this device's RAM budget already
// is - see the MAX_HISTORY 2048->512 comment above. This formula is
// known-good because it's the exact same one already succeeding, today, for
// the real engine thread's own Position (Threads.pos[0]).
//
// NOTE: this does bound gameplay/undo depth to roughly this many plies -
// same implicit constraint that already exists engine-side for
// Threads.pos[0], so this isn't a new limitation.
#define GUI_STACK_SLOTS (63 + (MAX_PLY + 110))
static void *g_stack_alloc = NULL;

// The exact Move object applied at each ply, needed to call undo_move()
// later (undo_move requires the identical Move that was passed to do_move).
static Move cfish_move_history[MAX_HISTORY];

#define GUI_TURN() (g_pos.sideToMove == WHITE ? 1 : -1)

// Involution: GUI square numbering (0 = a8, row-major, row0 = rank 8) <->
// Cfish square numbering (0 = a1, row-major, row0 = rank 1). Flipping the
// rank is self-inverse, so one function handles both directions.
static inline int gui_cfish_flip_sq(int sq) {
    int row = sq >> 3, col = sq & 7;
    return ((7 - row) << 3) + col;
}
#define gui_to_cfish_sq(s) gui_cfish_flip_sq(s)
#define cfish_to_gui_sq(s) gui_cfish_flip_sq(s)

typedef struct {
    int from;
    int to;
    int promo; // 0=None, 2=N, 3=B, 4=R, 5=Q  (matches Cfish's PieceType numbering)
} GuiMove;

// Piece at a GUI square, in the GUI's signed convention
// (P=1,N=2,B=3,R=4,Q=5,K=6, positive=White, negative=Black, 0=empty).
// NOTE: piece_on()/color_of() etc. below are position.h macros that expand
// to "pos->something" verbatim, so a local variable literally named 'pos'
// must be in scope for them to resolve correctly.
static inline int gui_piece_at(int gui_sq) {
    Position *pos = &g_pos;
    Piece pc = piece_on(gui_to_cfish_sq(gui_sq));
    if (pc == 0) return 0;
    int val = (int)type_of_p(pc);
    return (color_of(pc) == WHITE) ? val : -val;
}

// GUI square of the side-to-move's king if it is currently in check, else -1.
static inline int gui_king_in_check_sq(void) {
    Position *pos = &g_pos;
    if (!checkers()) return -1;
    Square ksq = square_of(stm(), KING);
    return cfish_to_gui_sq(ksq);
}

// Converts a Cfish Move into the GUI's (from,to,promo) representation.
// Castling is special-cased: Cfish internally encodes castling as
// "king captures its own rook" (to_sq() is the ROOK's square), but the GUI
// represents castling the way a player sees it - king moving two squares -
// so we translate to the visual king-destination square here.
static GuiMove gui_move_of(Move m) {
    GuiMove r;
    int from = from_sq(m);
    int to   = to_sq(m);
    r.from = cfish_to_gui_sq(from);
    if (type_of_m(m) == CASTLING) {
        int kto = (to > from) ? (from + 2) : (from - 2);
        r.to = cfish_to_gui_sq(kto);
    } else {
        r.to = cfish_to_gui_sq(to);
    }
    r.promo = (type_of_m(m) == PROMOTION) ? (int)promotion_type(m) : 0;
    return r;
}

// Finds the legal Cfish Move matching a GUI move (from/to/promo), or
// MOVE_NONE if gm isn't legal in the current position. Because this scans
// generate_legal()'s output, ALL legality (pins, checks, castling
// rights/path, en passant, etc.) comes straight from the real move
// generator - no separate pin/attack scanner needed anymore.
static Move gui_find_cfish_move(GuiMove gm) {
    ExtMove list[MAX_MOVES];
    ExtMove *end = generate_legal(&g_pos, list);
    for (ExtMove *e = list; e < end; e++) {
        GuiMove cand = gui_move_of(e->move);
        if (cand.from != gm.from || cand.to != gm.to) continue;
        if (type_of_m(e->move) == PROMOTION) {
            if (gm.promo == cand.promo) return e->move;
        } else {
            return e->move;
        }
    }
    return MOVE_NONE;
}

static inline int gui_is_legal_move(GuiMove gm) {
    return gui_find_cfish_move(gm) != MOVE_NONE;
}

static inline int gui_has_legal_moves(void) {
    ExtMove list[MAX_MOVES];
    return generate_legal(&g_pos, list) != list;
}

// One-time allocation of g_pos's Stack backing storage (mirrors
// thread_init()'s allocate+align pattern in thread.c). Must be called
// exactly once, before the first gui_reset_game().
//
// DIAGNOSTIC: prints checkpoint messages to the top screen so a hard
// black-screen hang can be localized to a specific step instead of being a
// total mystery. Remove/quiet these once startup is confirmed reliable.
static void gui_alloc_position(void) {
    memset(&g_pos, 0, sizeof(g_pos));

    consoleSelect(&topConsole);
    printf("Alloc GUI stack:\n %d slots, %u bytes\n",
           GUI_STACK_SLOTS, (unsigned)(GUI_STACK_SLOTS * sizeof(Stack)));
    fflush(stdout);

    g_stack_alloc = calloc(GUI_STACK_SLOTS, sizeof(Stack));
    if (!g_stack_alloc) {
        printf("\x1b[1;31mFATAL: OOM allocating\nGUI Stack buffer!\x1b[0m\n");
        fflush(stdout);
        while (1) threadWaitForVBlank();
    }
    printf("Stack alloc OK.\n");
    fflush(stdout);

    g_pos.stackAllocation = g_stack_alloc;
    g_pos.stack = (Stack *)(((uintptr_t)g_pos.stackAllocation + 0x3f) & ~(uintptr_t)0x3f);

    g_pos.threadIdx = -1; // sentinel: never a real Threads.pos[] index
    atomic_store(&g_pos.resetCalls, false);
}

// Resets g_pos to the standard starting position. Safe to call repeatedly
// (mirrors how uci.c's "ucinewgame"/"position" handlers repeatedly call
// pos_set() on the SAME Threads.pos[0] object, reusing its one-time
// stack/table allocations).
//
// DIAGNOSTIC: see gui_alloc_position() note above.
static void gui_reset_game(void) {
    static char startfen[] =
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    consoleSelect(&topConsole);
    printf("Calling pos_set()...\n");
    fflush(stdout);

    pos_set(&g_pos, startfen, 0 /* isChess960 */);

    printf("pos_set() returned OK.\n");
    fflush(stdout);
}

// ==========================================================================

// Handshake state machine definitions
typedef enum {
    ENGINE_STATE_BOOTING,
    ENGINE_STATE_WAIT_UCIOK,
    ENGINE_STATE_WAIT_READYOK,
    ENGINE_STATE_READY,
} EngineInitState;

EngineInitState engine_state = ENGINE_STATE_BOOTING;

GuiMove move_history[MAX_HISTORY];
char san_history[MAX_HISTORY][16]; // Cache array to store pre-calculated SAN strings
int history_count = 0;

int cursor_r = 6;
int cursor_c = 4;
int selected_sq = -1;

int board_orientation = 1; 
int user_side = 1;         

int time_control_type = 0; 
int time_control_val = 1;  // 1ms

int engine_thinking = 0;
long long engine_nps = 0;
int engine_score_type = -1; 
int engine_score_val = 0;

// Thinking Traces & Real-time Console Rolling Buffer
int engine_depth = 0;
long long engine_nodes = 0;
char engine_pv[128] = "";
char raw_log[10][32] = { {0} };
int raw_log_count = 0;

// Legacy global flag kept for bridge library compatibility
volatile int redraw_needed = 1;

// ==========================================
// OPTIMIZATION CACHES & SPLIT REDRAW FLAGS
// ==========================================
typedef struct {
    int is_check;
    int has_legal_moves;
    int is_drawn;             // true if is_draw(&g_pos) (50-move OR repetition)
    int king_in_check_sq;
} BoardAnalysisCache;

BoardAnalysisCache state_analysis;
uint8_t selected_legal_destinations[64] = {0}; // Cache legal moves for active piece

char cached_move_rows_left[10][16] = { {0} };
char cached_move_rows_right[10][16] = { {0} };

int redraw_top_needed = 1;
int redraw_bottom_needed = 1;

// OPTIMIZATION: one-shot hint used to pass an already-computed
// gui_has_legal_moves() result from gui_push_move() forward into the
// immediately-following invalidate_and_update_board_caches() call, since
// both were otherwise independently recomputing the exact same thing on the
// exact same resulting position.
int g_next_has_moves_hint = 0;
int g_next_has_moves_hint_valid = 0;

// OPTIMIZATION: persistent, incrementally-updated UCI "position" command
// buffer. Only the moves that are new since the last sync get appended.
static char engine_cmd_buf[8192] = "position startpos";
static int engine_cmd_synced_count = -1; // -1 forces a full rebuild on first use

PrintConsole topConsole, bottomConsole;

// Hardware Background Layer IDs
int bg_board_id;
int bg_pieces_id;

// Safe global read-only block for DTCM cache-safety
static const u32 solid_tile[8] = {
    0x11111111,
    0x11111111,
    0x11111111,
    0x11111111,
    0x11111111,
    0x11111111,
    0x11111111,
    0x11111111
};

void gui_push_move(GuiMove gm);
void trigger_engine_move(void);
int get_promo_choice(void);
void invalidate_and_update_board_caches(void);
void gui_build_san(Move m, char *buf, int *out_has_moves_after);

GuiMove gui_uci_to_move(const char *str);

static inline int screen_to_board_sq(int r, int c) {
    if (board_orientation == 1) {
        return (r << 3) + c;
    } else {
        return ((7 - r) << 3) + (7 - c);
    }
}

void gui_move_to_uci(GuiMove m, char *buf) {
    int f_col = m.from & 7;
    int f_row = 8 - (m.from >> 3);
    int t_col = m.to & 7;
    int t_row = 8 - (m.to >> 3);
    sprintf(buf, "%c%d%c%d", 'a' + f_col, f_row, 'a' + t_col, t_row);
    if (m.promo != 0) {
        char p = 'q';
        if (m.promo == 2) p = 'n';
        if (m.promo == 3) p = 'b';
        if (m.promo == 4) p = 'r';
        sprintf(buf + 4, "%c", p);
    }
}

GuiMove gui_uci_to_move(const char *str) {
    GuiMove m = {-1, -1, 0};
    if (strlen(str) < 4) return m;
    int f_col = str[0] - 'a';
    int f_row = 8 - (str[1] - '0');
    int t_col = str[2] - 'a';
    int t_row = 8 - (str[3] - '0');
    m.from = (f_row << 3) + f_col;
    m.to = (t_row << 3) + t_col;
    if (strlen(str) == 5) {
        char p = str[4];
        if (p == 'n') m.promo = 2;
        else if (p == 'b') m.promo = 3;
        else if (p == 'r') m.promo = 4;
        else m.promo = 5;
    }
    return m;
}

// Builds the SAN string for legal move 'm' (must be legal in g_pos right
// now) and, as a side effect of computing the check/mate suffix, reports
// via *out_has_moves_after whether the resulting position has any legal
// replies - saving the caller from generating moves on it a second time.
//
// Disambiguation and check/mate detection now come straight from
// generate_legal() on the real Position instead of a hand-rolled pin
// scanner.
void gui_build_san(Move m, char *buf, int *out_has_moves_after) {
    Position *pos = &g_pos;
    int from = from_sq(m);
    int to   = to_sq(m);
    Piece pc = piece_on(from);
    int pt = (int)type_of_p(pc);
    int mtype = type_of_m(m);

    if (mtype == CASTLING) {
        int kingside = to > from;
        strcpy(buf, kingside ? "O-O" : "O-O-O");
    } else {
        char *ptr = buf;
        int is_cap = (piece_on(to) != 0) || (mtype == ENPASSANT);

        if (pt == PAWN) {
            if (is_cap) {
                *ptr++ = 'a' + file_of(from);
                *ptr++ = 'x';
            }
        } else {
            const char p_chars[] = "?PNBRQK";
            *ptr++ = p_chars[pt];

            ExtMove list[MAX_MOVES];
            ExtMove *end = generate_legal(pos, list);
            int another_can_move = 0, same_file = 0, same_rank = 0;
            for (ExtMove *e = list; e < end; e++) {
                Move m2 = e->move;
                if (m2 == m) continue;
                if (type_of_m(m2) == CASTLING) continue;
                if (to_sq(m2) != (Square)to) continue;
                int f2 = from_sq(m2);
                if (f2 == from) continue;
                if ((int)type_of_p(piece_on(f2)) != pt) continue;
                another_can_move = 1;
                if (file_of(f2) == file_of(from)) same_file = 1;
                if (rank_of(f2) == rank_of(from)) same_rank = 1;
            }
            if (another_can_move) {
                if (!same_file) {
                    *ptr++ = 'a' + file_of(from);
                } else if (!same_rank) {
                    *ptr++ = '1' + rank_of(from);
                } else {
                    *ptr++ = 'a' + file_of(from);
                    *ptr++ = '1' + rank_of(from);
                }
            }
            if (is_cap) *ptr++ = 'x';
        }

        *ptr++ = 'a' + file_of(to);
        *ptr++ = '1' + rank_of(to);

        if (mtype == PROMOTION) {
            *ptr++ = '=';
            const char p_chars[] = "?PNBRQK";
            *ptr++ = p_chars[promotion_type(m)];
        }
        *ptr = '\0';
    }

    // Apply the move momentarily to determine the check/mate suffix, then
    // undo it - g_pos is left exactly as it was.
    int gives_chk = gives_check(pos, pos->st, m);
    do_move(pos, m, gives_chk);

    int in_check_after = (checkers() != 0);
    ExtMove list2[MAX_MOVES];
    int has_moves = (generate_legal(pos, list2) != list2);
    if (out_has_moves_after) *out_has_moves_after = has_moves;

    undo_move(pos, m);

    if (in_check_after) {
        char *p2 = buf + strlen(buf);
        *p2++ = has_moves ? '+' : '#';
        *p2 = '\0';
    }
}

// Applies GuiMove gm to the live game (must already be verified/looked-up
// via gui_find_cfish_move). Records history, SAN, and hands the
// has-legal-moves-after result forward via g_next_has_moves_hint.
void gui_push_move(GuiMove gm) {
    if (history_count >= MAX_HISTORY - 1) return;

    Move m = gui_find_cfish_move(gm);
    if (m == MOVE_NONE) return; // not legal - ignore defensively

    int has_moves_after = 0;
    gui_build_san(m, san_history[history_count], &has_moves_after);

    move_history[history_count] = gm;
    cfish_move_history[history_count] = m;

    Position *pos = &g_pos;
    int gives_chk = gives_check(pos, pos->st, m);
    do_move(pos, m, gives_chk);
    history_count++;

    g_next_has_moves_hint = has_moves_after;
    g_next_has_moves_hint_valid = 1;

    redraw_top_needed = 1;
    redraw_bottom_needed = 1;
}

void push_raw_log(const char *line) {
    if (raw_log_count < 10) {
        strncpy(raw_log[raw_log_count], line, 31);
        raw_log[raw_log_count][31] = '\0';
        raw_log_count++;
    } else {
        for (int i = 0; i < 9; i++) {
            memmove(raw_log[i], raw_log[i + 1], sizeof(raw_log[0]));
        }
        strncpy(raw_log[9], line, 31);
        raw_log[9][31] = '\0';
    }
    redraw_bottom_needed = 1; 
}

void engine_cmd_sync(void) {
    if (engine_cmd_synced_count == history_count) return;

    if (engine_cmd_synced_count < 0 || engine_cmd_synced_count > history_count) {
        if (history_count == 0) {
            strcpy(engine_cmd_buf, "position startpos");
        } else {
            strcpy(engine_cmd_buf, "position startpos moves");
            int len = strlen(engine_cmd_buf);
            for (int i = 0; i < history_count; i++) {
                char uci_m[10];
                gui_move_to_uci(move_history[i], uci_m);
                int move_len = strlen(uci_m);
                if (len + 1 + move_len + 1 >= (int)sizeof(engine_cmd_buf)) break;
                engine_cmd_buf[len++] = ' ';
                strcpy(engine_cmd_buf + len, uci_m);
                len += move_len;
            }
            engine_cmd_buf[len] = '\0';
        }
    } else {
        if (engine_cmd_synced_count == 0) {
            strcpy(engine_cmd_buf, "position startpos moves");
        }
        int len = strlen(engine_cmd_buf);
        for (int i = engine_cmd_synced_count; i < history_count; i++) {
            char uci_m[10];
            gui_move_to_uci(move_history[i], uci_m);
            int move_len = strlen(uci_m);
            if (len + 1 + move_len + 1 >= (int)sizeof(engine_cmd_buf)) break;
            engine_cmd_buf[len++] = ' ';
            strcpy(engine_cmd_buf + len, uci_m);
            len += move_len;
        }
        engine_cmd_buf[len] = '\0';
    }

    engine_cmd_synced_count = history_count;
}

void trigger_engine_move(void) {
    engine_nps = 0;
    engine_score_type = -1;
    engine_score_val = 0;
    engine_depth = 0;
    engine_nodes = 0;
    engine_pv[0] = '\0';

    engine_cmd_sync();
    sf_send_command(engine_cmd_buf);

    char go_cmd[128];
    if (time_control_type == 0) {
        sprintf(go_cmd, "go movetime %d", time_control_val);
    } else if (time_control_type == 1) {
        sprintf(go_cmd, "go depth %d", time_control_val);
    } else {
        sprintf(go_cmd, "go nodes %d", time_control_val);
    }
    sf_send_command(go_cmd);
    redraw_bottom_needed = 1;
}

void process_engine_output(char *line) {
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
        line[len - 1] = '\0';
        len--;
    }

    if (strlen(line) > 0) {
        push_raw_log(line);
    }

    if (engine_state == ENGINE_STATE_WAIT_UCIOK) {
        if (strstr(line, "uciok") != NULL) {
            sf_send_command("isready");
            engine_state = ENGINE_STATE_WAIT_READYOK;
            redraw_bottom_needed = 1;
        }
    } else if (engine_state == ENGINE_STATE_WAIT_READYOK) {
        if (strstr(line, "readyok") != NULL) {
            sf_send_command("setoption name Hash value 1"); 
            sf_send_command("setoption name Ponder value false");
            sf_send_command("ucinewgame");
            engine_state = ENGINE_STATE_READY;
            redraw_bottom_needed = 1;
        }
    }

    if (strncmp(line, "info", 4) == 0) {
        char *nps_ptr = strstr(line, " nps ");
        if (nps_ptr) {
            long long val;
            if (sscanf(nps_ptr, " nps %lld", &val) == 1) {
                engine_nps = val;
                redraw_bottom_needed = 1;
            }
        }

        char *depth_ptr = strstr(line, " depth ");
        if (depth_ptr) {
            int d_val;
            if (sscanf(depth_ptr, " depth %d", &d_val) == 1) {
                engine_depth = d_val;
                redraw_bottom_needed = 1;
            }
        }

        char *nodes_ptr = strstr(line, " nodes ");
        if (nodes_ptr) {
            long long n_val;
            if (sscanf(nodes_ptr, " nodes %lld", &n_val) == 1) {
                engine_nodes = n_val;
                redraw_bottom_needed = 1;
            }
        }

        char *pv_ptr = strstr(line, " pv ");
        if (pv_ptr) {
            strncpy(engine_pv, pv_ptr + 4, sizeof(engine_pv) - 1);
            engine_pv[sizeof(engine_pv) - 1] = '\0';
            redraw_bottom_needed = 1;
        }

        char *score_ptr = strstr(line, " score ");
        if (score_ptr) {
            int score_val = 0;
            char score_type[16];
            if (sscanf(score_ptr, " score %15s %d", score_type, &score_val) == 2) {
                if (strcmp(score_type, "cp") == 0) {
                    engine_score_type = 0;
                    engine_score_val = score_val * GUI_TURN();
                } else if (strcmp(score_type, "mate") == 0) {
                    engine_score_type = 1;
                    engine_score_val = score_val * GUI_TURN();
                }
                redraw_bottom_needed = 1;
            }
        }
    }

    if (strncmp(line, "bestmove", 8) == 0) {
        char move_str[16];
        if (sscanf(line, "bestmove %15s", move_str) == 1) {
            if (strcmp(move_str, "(none)") == 0 || strcmp(move_str, "NULL") == 0) {
                engine_thinking = 0;
                redraw_bottom_needed = 1;
                return;
            }
            
            GuiMove m = gui_uci_to_move(move_str);
            if (gui_is_legal_move(m)) {
                gui_push_move(m);
                invalidate_and_update_board_caches();
            }
            engine_thinking = 0;
            redraw_top_needed = 1;
            redraw_bottom_needed = 1;
        }
    }
}

void read_from_engine(void) {
    char tmp[256];
    static char line_buf[512];
    static int line_len = 0;
    int bytes_read;
    int chunks_processed = 0;

    while (chunks_processed < 8 && (bytes_read = sf_get_output(tmp, sizeof(tmp) - 1)) > 0) {
        tmp[bytes_read] = '\0';
        chunks_processed++;
        
        for (int i = 0; i < bytes_read; i++) {
            char c = tmp[i];
            if (c == '\n' || c == '\r') {
                if (line_len > 0) {
                    line_buf[line_len] = '\0';
                    process_engine_output(line_buf);
                    line_len = 0;
                }
            } else {
                if (line_len < (int)sizeof(line_buf) - 1) {
                    line_buf[line_len++] = c;
                }
            }
        }
    }
}

// ==========================================
// CACHE VALIDATION & CALCULATION HOOKS
// ==========================================
void update_selected_destinations(void) {
    memset(selected_legal_destinations, 0, sizeof(selected_legal_destinations));
    if (selected_sq == -1) return;

    ExtMove list[MAX_MOVES];
    ExtMove *end = generate_legal(&g_pos, list);
    for (ExtMove *e = list; e < end; e++) {
        GuiMove gm = gui_move_of(e->move);
        if (gm.from == selected_sq) selected_legal_destinations[gm.to] = 1;
    }
}

void preformat_move_list_cache(void) {
    int total_full_moves = (history_count + 1) / 2;
    int max_visible_moves = 20;
    int half_visible = 10;
    int start_move = (total_full_moves > max_visible_moves) ? (total_full_moves - (max_visible_moves - 1)) : 1;

    for (int r = 0; r < half_visible; r++) {
        int left_display = start_move + r;
        int right_display = start_move + half_visible + r;

        if (total_full_moves > 0 && left_display <= total_full_moves) {
            int w_idx = (left_display - 1) * 2;
            int b_idx = w_idx + 1;
            char w_str[10] = "-----";
            char b_str[10] = "-----";
            if (w_idx < history_count) strcpy(w_str, san_history[w_idx]);
            if (b_idx < history_count) {
                strcpy(b_str, san_history[b_idx]);
            } else if (w_idx < history_count) {
                strcpy(b_str, "...");
            }
            sprintf(cached_move_rows_left[r], "%2d.%-5s%-5s", left_display, w_str, b_str);
        } else {
            sprintf(cached_move_rows_left[r], "%2d. ---  --- ", left_display);
        }

        if (total_full_moves > 0 && right_display <= total_full_moves) {
            int w_idx = (right_display - 1) * 2;
            int b_idx = w_idx + 1;
            char w_str[10] = "-----";
            char b_str[10] = "-----";
            if (w_idx < history_count) strcpy(w_str, san_history[w_idx]);
            if (b_idx < history_count) {
                strcpy(b_str, san_history[b_idx]);
            } else if (w_idx < history_count) {
                strcpy(b_str, "...");
            }
            sprintf(cached_move_rows_right[r], "%2d.%-5s%-5s", right_display, w_str, b_str);
        } else {
            sprintf(cached_move_rows_right[r], "%2d. ---  --- ", right_display);
        }
    }
}

void invalidate_and_update_board_caches(void) {
    // 1. Check state straight from the position's real checkers bitboard.
    state_analysis.is_check = (g_pos.st->checkersBB != 0);
    state_analysis.king_in_check_sq = state_analysis.is_check ? gui_king_in_check_sq() : -1;

    // 2. Legal-move existence, reusing the hint computed by gui_push_move()
    // (via gui_build_san) if available, else generate directly.
    if (g_next_has_moves_hint_valid) {
        state_analysis.has_legal_moves = g_next_has_moves_hint;
        g_next_has_moves_hint_valid = 0;
    } else {
        state_analysis.has_legal_moves = gui_has_legal_moves();
    }

    // 3. Draw detection (50-move rule OR repetition) straight from the
    // engine's own is_draw(), instead of a hand-rolled hash/memcmp scanner.
    state_analysis.is_drawn = is_draw(&g_pos);

    // 4. Cache the legal highlights for the selected piece
    update_selected_destinations();

    // 5. Update formatted bottom lists
    preformat_move_list_cache();
}

// Map coordinates helper
void set_tile(u16* map, int x, int y, u16 tile, u16 palette) {
    if (x >= 0 && x < 32 && y >= 0 && y < 32) {
        map[y * 32 + x] = tile | (palette << 12);
    }
}

static inline void set_tile_fast(u16* map, int x, int y, u16 tile, u16 palette) {
    map[y * 32 + x] = tile | (palette << 12);
}

void draw_string(u16* map, int x, int y, const char* str, u16 palette) {
    while (*str) {
        set_tile(map, x, y, (u16)(*str), palette);
        x++;
        str++;
    }
}

static inline void draw_string_fast(u16* map, int x, int y, const char* str, u16 palette) {
    u16* dst = &map[y * 32 + x];
    u16 pal_shifted = palette << 12;
    while (*str) {
        *dst++ = (u16)(*str++) | pal_shifted;
    }
}

void draw_string_sub(u16* map, int x, int y, const char* str, u16 palette) {
    while (*str) {
        if (x >= 0 && x < 32 && y >= 0 && y < 32) {
            map[y * 32 + x] = (u16)(*str) | (palette << 12);
        }
        x++;
        str++;
    }
}

static inline void draw_string_sub_fast(u16* map, int x, int y, const char* str, u16 palette) {
    u16* dst = &map[y * 32 + x];
    u16 pal_shifted = palette << 12;
    while (*str) {
        *dst++ = (u16)(*str++) | pal_shifted;
    }
}

void init_custom_palettes(void) {
    BG_PALETTE[0 * 16 + 1] = RGB15(27, 22, 17);
    BG_PALETTE[1 * 16 + 1] = RGB15(17, 12,  0);
    BG_PALETTE[2 * 16 + 1] = RGB15( 0, 17,  0);
    BG_PALETTE[3 * 16 + 1] = RGB15(31, 16,  0);
    BG_PALETTE[4 * 16 + 1] = RGB15(12, 22, 31);
    BG_PALETTE[5 * 16 + 1] = RGB15(12, 17, 22);
    BG_PALETTE[6 * 16 + 1] = RGB15(22, 27, 22);
    BG_PALETTE[7 * 16 + 1] = RGB15(17, 22, 17);
    BG_PALETTE[8 * 16 + 1] = RGB15(31,  0,  0);
    BG_PALETTE[9 * 16 + 1] = RGB15(20, 20, 20); 
    BG_PALETTE[9 * 16 + 15] = RGB15(20, 20, 20);
    BG_PALETTE[10 * 16 + 0] = 0;
    BG_PALETTE[10 * 16 + 1] = RGB15(31, 31, 31);
    BG_PALETTE[10 * 16 + 15] = RGB15(31, 31, 31);
    BG_PALETTE[11 * 16 + 0] = 0;
    BG_PALETTE[11 * 16 + 1] = RGB15(0,  0,  0);
    BG_PALETTE[11 * 16 + 15] = RGB15(0,  0,  0);
}

void init_bottom_palette(void) {
    for (int p = 0; p < 16; p++) {
        BG_PALETTE_SUB[p * 16 + 0] = RGB15(0, 0, 0); 
    }

    u16 colors[16];
    colors[0]  = RGB15(31, 31, 31);
    colors[1]  = RGB15(22, 4, 4);
    colors[2]  = RGB15(4, 22, 4);
    colors[3]  = RGB15(22, 22, 4);
    colors[4]  = RGB15(4, 4, 22);
    colors[5]  = RGB15(22, 4, 22);
    colors[6]  = RGB15(4, 22, 22);
    colors[7]  = RGB15(24, 24, 24);
    colors[8]  = RGB15(12, 12, 12);
    colors[9]  = RGB15(31, 5, 5);
    colors[10] = RGB15(5, 31, 5);
    colors[11] = RGB15(31, 31, 5);
    colors[12] = RGB15(5, 5, 31);
    colors[13] = RGB15(31, 5, 31);
    colors[14] = RGB15(5, 31, 31);
    colors[15] = RGB15(31, 31, 31);

    for (int p = 0; p < 16; p++) {
        for (int c = 1; c < 16; c++) {
            BG_PALETTE_SUB[p * 16 + c] = colors[p];
        }
    }
}

// Draw the Top Screen Board
void draw_top_board(void) {
    u8* tile_memory = (u8*)bgGetGfxPtr(bg_board_id);
    memcpy(tile_memory + (255 * 32), solid_tile, sizeof(solid_tile));

    init_custom_palettes();

    u16* board_map = bgGetMapPtr(bg_board_id);
    u16* pieces_map = bgGetMapPtr(bg_pieces_id);

    memset(board_map, 0, 32 * 32 * sizeof(u16));
    memset(pieces_map, 0, 32 * 32 * sizeof(u16));

    for (int col = 0; col < 8; col++) {
        char file_lbl = (board_orientation == 1) ? ('a' + col) : ('h' - col);
        char str[2] = {file_lbl, '\0'};
        draw_string_fast(pieces_map, 7 + col * 2, 2, str, 9);
        draw_string_fast(pieces_map, 7 + col * 2, 21, str, 9);
    }

    int king_in_check = state_analysis.king_in_check_sq;

    for (int r = 0; r < 8; r++) {
        int rank_lbl = (board_orientation == 1) ? (8 - r) : (r + 1);
        char lbl_str[2] = {'0' + rank_lbl, '\0'};

        draw_string_fast(pieces_map, 5, 4 + r * 2, lbl_str, 9);
        draw_string_fast(pieces_map, 24, 4 + r * 2, lbl_str, 9);

        for (int c = 0; c < 8; c++) {
            int sq = screen_to_board_sq(r, c);
            int p = gui_piece_at(sq);

            int is_light = (((sq >> 3) + (sq & 7)) & 1) == 0;
            int palette_idx = is_light ? 0 : 1;

            int is_selected = (sq == selected_sq);
            int is_cursor = (r == cursor_r && c == cursor_c);

            int is_prev_move = 0;
            if (history_count > 0) {
                GuiMove last_move = move_history[history_count - 1];
                if (sq == last_move.from || sq == last_move.to) {
                    is_prev_move = 1;
                }
            }

            int is_legal_dest = selected_legal_destinations[sq];

            if (is_cursor) {
                palette_idx = 3;
            } else if (is_selected) {
                palette_idx = 2;
            } else if (sq == king_in_check) {
                palette_idx = 8;
            } else if (is_prev_move) {
                palette_idx = is_light ? 4 : 5;
            } else if (is_legal_dest) {
                palette_idx = is_light ? 6 : 7;
            }

            set_tile_fast(board_map, 7 + c * 2,     4 + r * 2,     255, palette_idx);
            set_tile_fast(board_map, 7 + c * 2 + 1, 4 + r * 2,     255, palette_idx);
            set_tile_fast(board_map, 7 + c * 2,     4 + r * 2 + 1, 255, palette_idx);
            set_tile_fast(board_map, 7 + c * 2 + 1, 4 + r * 2 + 1, 255, palette_idx);

            if (p != 0) {
                u16 fg_palette = (p > 0) ? 10 : 11;
                char piece_str = ' ';
                switch (abs(p)) {
                    case 1: piece_str = 'P'; break;
                    case 2: piece_str = 'N'; break;
                    case 3: piece_str = 'B'; break;
                    case 4: piece_str = 'R'; break;
                    case 5: piece_str = 'Q'; break;
                    case 6: piece_str = 'K'; break;
                }
                set_tile_fast(pieces_map, 7 + c * 2, 4 + r * 2, piece_str, fg_palette);
            }
        }
    }
}

// Draw the Bottom Screen
void draw_bottom_stats(void) {
    u16* sub_map = bgGetMapPtr(bottomConsole.bgId);
    memset(sub_map, 0, 32 * 32 * sizeof(u16));

    int is_ch = state_analysis.is_check;
    int has_mov = state_analysis.has_legal_moves;
    int is_fifty = (g_pos.st->rule50 >= 100);
    int is_drawn = state_analysis.is_drawn;

    int curr_x = 0;
    if (engine_state != ENGINE_STATE_READY) {
        draw_string_sub_fast(sub_map, curr_x, 0, "Booting...", 15);
        curr_x += 10;
    } else if (is_fifty) {
        draw_string_sub_fast(sub_map, curr_x, 0, "[DRAW (50m-rule)]", 14);
        curr_x += 17;
    } else if (is_drawn) {
        draw_string_sub_fast(sub_map, curr_x, 0, "[DRAW (3-fold)]", 14);
        curr_x += 15;
    } else if (!has_mov) {
        if (is_ch) {
            draw_string_sub_fast(sub_map, curr_x, 0, "[CHECKMATE!]", 9);
            curr_x += 12;
        } else {
            draw_string_sub_fast(sub_map, curr_x, 0, "[STALEMATE!]", 14);
            curr_x += 12;
        }
    } else if (is_ch) {
        if (GUI_TURN() == 1) {
            draw_string_sub_fast(sub_map, curr_x, 0, "White", 10);
            curr_x += 5;
            draw_string_sub_fast(sub_map, curr_x, 0, " (CHECK!)", 9);
            curr_x += 9;
        } else {
            draw_string_sub_fast(sub_map, curr_x, 0, "Black", 13);
            curr_x += 5;
            draw_string_sub_fast(sub_map, curr_x, 0, " (CHECK!)", 9);
            curr_x += 9;
        }
    } else {
        if (GUI_TURN() == 1) {
            draw_string_sub_fast(sub_map, curr_x, 0, "White", 10);
            curr_x += 5;
            draw_string_sub_fast(sub_map, curr_x, 0, " to play", 15);
            curr_x += 8;
        } else {
            draw_string_sub_fast(sub_map, curr_x, 0, "Black", 13);
            curr_x += 5;
            draw_string_sub_fast(sub_map, curr_x, 0, " to play", 15);
            curr_x += 8;
        }
    }

    draw_string_sub_fast(sub_map, curr_x, 0, " | ", 15);
    curr_x += 3;

    const char *w_play = (user_side == 1 || user_side == 0) ? "Hum" : "Eng";
    const char *b_play = (user_side == -1 || user_side == 0) ? "Hum" : "Eng";
    char config_buf[16];
    sprintf(config_buf, "W:%s B:%s", w_play, b_play);
    draw_string_sub_fast(sub_map, curr_x, 0, config_buf, 15);

    curr_x = 0;
    if (engine_score_type == 0) {
        double eval = (double)engine_score_val / 100.0;
        char tmp_buf[16];
        sprintf(tmp_buf, "%+.2f", eval);
        if (eval > 0.0) {
            draw_string_sub_fast(sub_map, curr_x, 1, tmp_buf, 10);
        } else if (eval < 0.0) {
            draw_string_sub_fast(sub_map, curr_x, 1, tmp_buf, 9);
        } else {
            draw_string_sub_fast(sub_map, curr_x, 1, "0.00", 15);
        }
        curr_x += 6;
    } else if (engine_score_type == 1) {
        char tmp_buf[16];
        if (engine_score_val > 0) {
            sprintf(tmp_buf, "+M%d", engine_score_val);
            draw_string_sub_fast(sub_map, curr_x, 1, tmp_buf, 10);
        } else if (engine_score_val < 0) {
            sprintf(tmp_buf, "-M%d", -engine_score_val);
            draw_string_sub_fast(sub_map, curr_x, 1, tmp_buf, 9);
        } else {
            draw_string_sub_fast(sub_map, curr_x, 1, "M0", 15);
        }
        curr_x += 6;
    } else {
        curr_x += 6;
    }

    draw_string_sub_fast(sub_map, curr_x, 1, " | ", 15);
    curr_x += 3;

    char nps_str[24] = "";
    if (engine_nps > 0) {
        if (engine_nps >= 1000000) {
            sprintf(nps_str, "%.2f Mnps", (double)engine_nps / 1000000.0);
        } else if (engine_nps >= 100000) {
            sprintf(nps_str, "%lld knps", engine_nps / 1000);
        } else if (engine_nps >= 10000) {
            sprintf(nps_str, "%.1f knps", (double)engine_nps / 1000.0);
        } else if (engine_nps >= 1000) {
            sprintf(nps_str, "%.2f knps", (double)engine_nps / 1000.0);
        } else {
            sprintf(nps_str, "%lld nps", engine_nps);
        }
    }
    draw_string_sub_fast(sub_map, curr_x, 1, nps_str, 15);
    curr_x += 6;

    draw_string_sub_fast(sub_map, curr_x, 1, " | ", 15);
    curr_x += 3;

    char lim_str[24] = "";
    if (time_control_type == 0) {
        sprintf(lim_str, "%dms", time_control_val);
    } else if (time_control_type == 1) {
        sprintf(lim_str, "depth %d", time_control_val);
    } else {
        sprintf(lim_str, "%d nod", time_control_val);
    }
    draw_string_sub_fast(sub_map, curr_x, 1, lim_str, 15);

    draw_string_sub_fast(sub_map, 0, 2, "RECENT MOVES:", 11);

    for (int r = 0; r < 10; r++) {
        draw_string_sub_fast(sub_map, 0, 3 + r, cached_move_rows_left[r], 15);
        draw_string_sub_fast(sub_map, 14, 3 + r, "|", 8);
        draw_string_sub_fast(sub_map, 15, 3 + r, cached_move_rows_right[r], 15);
    }

    for (int i = 0; i < 10; i++) {
        if (i < raw_log_count) {
            if (i == raw_log_count - 1) {
                draw_string_sub_fast(sub_map, 0, 13 + i, raw_log[i], 15);
            } else {
                draw_string_sub_fast(sub_map, 0, 13 + i, raw_log[i], 8);
            }
        }
    }
}

void draw_ui(void) {
    draw_top_board();
    draw_bottom_stats();
}

int get_promo_choice(void) {
    consoleSelect(&bottomConsole);
    printf("\x1b[7;1H\x1b[J"); 
    printf("\n\x1b[1;33mPROMOTION! Tap Key selection:\n");
    printf(" [Y] Queen  [X] Rook\n [B] Bishop [A] Knight\x1b[0m\n");
    fflush(stdout);
    
    int choice = 5;
    while (pmMainLoop()) {
        scanKeys();
        u32 kDown = keysDown();
        if (kDown & KEY_Y) { choice = 5; break; }
        if (kDown & KEY_X) { choice = 4; break; }
        if (kDown & KEY_B) { choice = 3; break; }
        if (kDown & KEY_A) { choice = 2; break; }
        threadWaitForVBlank();
    }
    redraw_top_needed = 1;
    redraw_bottom_needed = 1;
    return choice;
}

void handle_select(void) {
    if (engine_thinking) {
        return;
    }

    int is_engine_turn = 0;
    if (user_side == 2) {
        is_engine_turn = 1; 
    } else if (user_side == 1 && GUI_TURN() == -1) {
        is_engine_turn = 1; 
    } else if (user_side == -1 && GUI_TURN() == 1) {
        is_engine_turn = 1; 
    }

    if (is_engine_turn) {
        return; 
    }

    if (!state_analysis.has_legal_moves || state_analysis.is_drawn) {
        return;
    }

    int sq = screen_to_board_sq(cursor_r, cursor_c);
    if (selected_sq == -1) {
        int p = gui_piece_at(sq);
        if (p != 0 && ((GUI_TURN() == 1 && p > 0) || (GUI_TURN() == -1 && p < 0))) {
            selected_sq = sq;
            update_selected_destinations();
            redraw_top_needed = 1;
        }
    } else {
        GuiMove m = {selected_sq, sq, 0};
        int p = gui_piece_at(selected_sq);
        int is_promo = (abs(p) == 1 && ((sq >> 3) == 0 || (sq >> 3) == 7));
        if (is_promo) m.promo = 5;

        if (selected_legal_destinations[sq]) {
            if (is_promo) {
                m.promo = get_promo_choice();
            }
            gui_push_move(m);
            selected_sq = -1;
            
            invalidate_and_update_board_caches();
            redraw_top_needed = 1;
            redraw_bottom_needed = 1;
        } else {
            int target = gui_piece_at(sq);
            if (target != 0 && ((GUI_TURN() == 1 && target > 0) || (GUI_TURN() == -1 && target < 0))) {
                selected_sq = sq;
                update_selected_destinations();
                redraw_top_needed = 1;
            } else {
                selected_sq = -1;
                memset(selected_legal_destinations, 0, sizeof(selected_legal_destinations));
                redraw_top_needed = 1;
            }
        }
    }
}

void handle_undo(void) {
    if (engine_thinking) {
        sf_send_command("stop");
        engine_thinking = 0;
    }
    engine_nps = 0;
    engine_score_type = -1;
    engine_score_val = 0;
    engine_depth = 0;
    engine_nodes = 0;
    engine_pv[0] = '\0';
    g_next_has_moves_hint_valid = 0;
    int step_back = (user_side == 1 || user_side == -1) ? 2 : 1;
    while (step_back > 0 && history_count > 0) {
        history_count--;
        undo_move(&g_pos, cfish_move_history[history_count]);
        step_back--;
    }
    selected_sq = -1;

    invalidate_and_update_board_caches();
    redraw_top_needed = 1;
    redraw_bottom_needed = 1;
}

void handle_reset_board(void) {
    if (engine_thinking) {
        sf_send_command("stop");
        engine_thinking = 0;
    }
    engine_nps = 0;
    engine_score_type = -1;
    engine_score_val = 0;
    engine_depth = 0;
    engine_nodes = 0;
    engine_pv[0] = '\0';
    g_next_has_moves_hint_valid = 0;
    gui_reset_game();
    history_count = 0;
    selected_sq = -1;
    cursor_r = 6;
    cursor_c = 4;
    sf_send_command("ucinewgame");
    sf_send_command("isready");

    invalidate_and_update_board_caches();
    redraw_top_needed = 1;
    redraw_bottom_needed = 1;
}

void handle_switch_sides(void) {
    if (engine_thinking) {
        sf_send_command("stop");
        engine_thinking = 0;
    }
    engine_depth = 0;
    engine_nodes = 0;
    engine_pv[0] = '\0';
    if (user_side == 1) user_side = -1;
    else if (user_side == -1) user_side = 0;
    else if (user_side == 0) user_side = 2;
    else user_side = 1;
    redraw_bottom_needed = 1;
}

void adjust_time_control(void) {
    if (time_control_type == 0) { 
        int time_list[] = {1, 10, 50, 100, 500, 1000, 1500, 2000, 3000, 5000, 10000};
        int time_count = sizeof(time_list) / sizeof(time_list[0]);
        int found_idx = -1;
        for (int i = 0; i < time_count; i++) {
            if (time_control_val == time_list[i]) {
                found_idx = i;
                break;
            }
        }
        time_control_val = (found_idx == -1 || found_idx == time_count - 1) ? time_list[0] : time_list[found_idx + 1];
    } else if (time_control_type == 1) { 
        time_control_val = (time_control_val % 20) + 1;
    } else { 
        int nodes_list[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576, 2097152, 4194304, 8388608};
        int nodes_count = sizeof(nodes_list) / sizeof(nodes_list[0]);
        int found_idx = -1;
        for (int i = 0; i < nodes_count; i++) {
            if (time_control_val == nodes_list[i]) {
                found_idx = i;
                break;
            }
        }
        time_control_val = (found_idx == -1 || found_idx == nodes_count - 1) ? nodes_list[0] : nodes_list[found_idx + 1];
    }
}

// Stockfish Engine Subprocess Entry Point
int main_stockfish(int argc, char **argv)
{
  print_engine_info(false);
  ds_yield();

  psqt_init();
  ds_yield();

  bitboards_init();
  ds_yield();

  zob_init();
  ds_yield();

  bitbases_init();
  ds_yield();

#ifndef NNUE_PURE
  endgames_init();
  ds_yield();
#endif

  threads_init();
  ds_yield();

  options_init();
  ds_yield();

  search_clear();
  ds_yield();

  uci_loop(argc, argv);

  threads_exit();
  TB_free();
  options_free();
  tt_free();
  pb_free();
  #ifdef NNUE
  nnue_free();
  #endif

  return 0;
}

void stockfish_thread_func(void* arg) {
    (void)arg;
    char *argv[] = {"stockfish", NULL};
    main_stockfish(1, argv);
}

// Nintendo DS Main Entry Point
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    irqEnable(IRQ_VBLANK);

    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);

    consoleInit(&topConsole, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&bottomConsole, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

    bg_board_id = bgInit(2, BgType_Text4bpp, BgSize_T_256x256, 29, 0);
    bg_pieces_id = bgInit(1, BgType_Text4bpp, BgSize_T_256x256, 30, 0);

    // DIAGNOSTIC: visible startup checkpoints on the top screen so a hard
    // hang/black-screen can be localized to a specific step.
    consoleSelect(&topConsole);
    printf("Boot: bridge init...\n");
    fflush(stdout);

    sf_bridge_init();

    printf("Boot: bitboards/zob/psqt...\n");
    fflush(stdout);

    // IMPORTANT: pos_set() relies on real Zobrist keys, PSQT scores, and
    // bitboard attack tables, so these idempotent table-builders must run
    // in the GUI thread first. The engine thread calling them again later
    // inside main_stockfish() is harmless (same reasoning as before).
    bitboards_init();
    zob_init();
    psqt_init();

    printf("Boot: alloc GUI position...\n");
    fflush(stdout);

    gui_alloc_position();
    gui_reset_game();

    printf("Boot: GUI position ready.\n");
    fflush(stdout);

    u8* tile_memory = (u8*)bgGetGfxPtr(bg_board_id);
    memcpy(tile_memory + (255 * 32), solid_tile, sizeof(solid_tile));

    init_custom_palettes();
    init_bottom_palette();

    REG_DISPCNT |= DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE;

    u16* board_map = bgGetMapPtr(bg_board_id);
    u16* pieces_map = bgGetMapPtr(bg_pieces_id);
    memset(board_map, 0, 32 * 32 * sizeof(u16));
    memset(pieces_map, 0, 32 * 32 * sizeof(u16));

    consoleSelect(&topConsole);
    printf("\x1b[2J");
    fflush(stdout);

    consoleSelect(&bottomConsole);
    printf("\x1b[2J");
    fflush(stdout);
    
    threadWaitForVBlank();

    pthread_t stockfish_thread;
    int thread_spawn = sf_pthread_create(&stockfish_thread, NULL, (void* (*)(void*))stockfish_thread_func, NULL);

    if (thread_spawn != 0) {
        consoleSelect(&bottomConsole);
        fflush(stdout);
        while(1) threadWaitForVBlank();
    } else {
        consoleSelect(&bottomConsole);
        fflush(stdout);
    }

    for (int i = 0; i < 3; i++) {
        ds_yield();
        threadWaitForVBlank();
    }

    sf_send_command("uci");
    engine_state = ENGINE_STATE_WAIT_UCIOK;

    invalidate_and_update_board_caches();
    redraw_top_needed = 1;
    redraw_bottom_needed = 1;

    while (pmMainLoop()) {
        scanKeys();
        u32 kDown = keysDown();

        if (kDown & KEY_START) break; 

        int input_detected = 0;

        if (kDown & KEY_UP)    { if (cursor_r > 0) { cursor_r--; input_detected = 1; } }
        if (kDown & KEY_DOWN)  { if (cursor_r < 7) { cursor_r++; input_detected = 1; } }
        if (kDown & KEY_RIGHT) { if (cursor_c < 7) { cursor_c++; input_detected = 1; } }
        if (kDown & KEY_LEFT)  { if (cursor_c > 0) { cursor_c--; input_detected = 1; } }

        if (kDown & KEY_A)      { handle_select(); input_detected = 1; }
        if (kDown & KEY_B)      { handle_undo(); input_detected = 1; }
        if (kDown & KEY_SELECT) { handle_reset_board(); input_detected = 1; }
        if (kDown & KEY_X)      { board_orientation = -board_orientation; input_detected = 1; }
        if (kDown & KEY_Y)      { handle_switch_sides(); input_detected = 1; }

        if (kDown & KEY_L) {
            time_control_type = (time_control_type + 1) % 3;
            time_control_val = (time_control_type == 0) ? 1 : (time_control_type == 1 ? 1 : 512);
            redraw_bottom_needed = 1;
        }
        if (kDown & KEY_R) {
            adjust_time_control();
            redraw_bottom_needed = 1;
        }

        if (input_detected) {
            redraw_top_needed = 1;
        }

        int engine_active = 0;
        if (engine_state == ENGINE_STATE_READY && state_analysis.has_legal_moves &&
            !state_analysis.is_drawn) {
            if (user_side == 2) engine_active = 1;
            else if (user_side == 1 && GUI_TURN() == -1) engine_active = 1;
            else if (user_side == -1 && GUI_TURN() == 1) engine_active = 1;
        }

        if (engine_active && !engine_thinking) {
            engine_thinking = 1;
            trigger_engine_move();
        }

        read_from_engine();

        if (redraw_top_needed) {
            draw_top_board();
            redraw_top_needed = 0;
        }
        if (redraw_bottom_needed) {
            draw_bottom_stats();
            redraw_bottom_needed = 0;
        }

        threadWaitForVBlank();
    }

    sf_send_command("quit");
    return 0;
}
