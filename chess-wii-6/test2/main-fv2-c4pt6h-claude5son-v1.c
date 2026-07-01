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
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <malloc.h>

// Nintendo Wii DevkitPro Hardware Headers
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <fat.h>

#include "bitboard.h"
#include "endgame.h"
#include "pawns.h"
#include "polybook.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "tbprobe.h"

// Restore standard printing and file operations inside the GUI's main thread
#undef printf
#undef fgets
#undef getline

#define MAX_HISTORY 2048

// =============================================================================
// OPT-13: 32-byte aligned BoardState for cache-line-friendly memcpy/memcmp
// OPT-4:  King squares cached directly in BoardState — find_king() is now O(1)
// OPT-17: Incrementally-maintained occupancy bitboard, feeds Stockfish's
//          magic-bitboard attacks_bb() for O(1) slider attack lookups
// =============================================================================
typedef struct __attribute__((aligned(32))) {
    int board[64];       // P=1,N=2,B=3,R=4,Q=5,K=6 (positive=White, negative=Black)
    int turn;            // 1=White, -1=Black
    int castle;          // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep;              // En-passant square (0-63), -1 if none
    int halfmoves;       // For 50-move rule
    int fullmoves;
    int white_king_sq;   // OPT-4: Always-current white king position
    int black_king_sq;   // OPT-4: Always-current black king position
    Bitboard occupied;   // OPT-17: Always-current occupancy, GUI-square-mapped via flip_sq()
} BoardState;

typedef struct {
    int from;
    int to;
    int promo; // 0=None, 2=N, 3=B, 4=R, 5=Q
} GuiMove;

// Global GUI state
BoardState current_state;
BoardState history[MAX_HISTORY];
GuiMove    move_history[MAX_HISTORY];

// OPT-10: Pre-computed PGN strings — computed once on push, free on render
char pgn_cache[MAX_HISTORY][16];

int history_count = 0;

int cursor_r = 6;
int cursor_c = 4;
int selected_sq = -1;

int board_orientation = 1;
int user_side         = 1;

int time_control_type = 0;
int time_control_val  = 1;

int  engine_thinking   = 0;
bool gui_mode_active   = true;

// Handshake tracking
bool engine_recvd_uciok   = false;
bool engine_recvd_readyok = false;

// Engine live performance metrics
long long engine_nps      = 0;
int       engine_depth    = 0;
int       engine_seldepth = 0;
int       engine_time_ms  = 0;
long long engine_nodes    = 0;
int       engine_hashfull = 0;
long long engine_tbhits   = 0;

int engine_score_type = -1;
int engine_score_val  = 0;

char engine_pv[1024] = "";

// =============================================================================
// OPT-1/3: Cached game state — avoids redundant O(64²) scans per frame
// =============================================================================
typedef struct {
    bool     has_legal_moves;
    int      king_in_check_sq;    // Square of the active king in check, or -1
    uint64_t legal_dests_mask;    // Bit-mask of legal destination squares for selected_sq
    int      repetition_count;    // OPT-3: Cached repetition count — not recomputed per frame
} CachedGuiState;

CachedGuiState cached_gui;
bool gui_needs_redraw = true; // OPT-2: Event-driven render flag

// =============================================================================
// OPT-5/12: Lock-Free SPSC Ring-Buffers with power-of-2 bitmask (no modulo)
// =============================================================================
#define FIFO_SIZE 16384
#define FIFO_MASK (FIFO_SIZE - 1)  // OPT-12: Replaces expensive % FIFO_SIZE

// =============================================================================
// OPT-15: Lightweight barrier instead of __sync_synchronize().
//          __sync_synchronize() emits a full "sync" on PPC (heavyweight,
//          system-wide store-drain), despite the old comment claiming
//          "lwsync". Broadway is single-core and these FIFOs are only ever
//          touched by the two cooperating KThreads on that one core, so a
//          full hw sync buys nothing — we only need to stop the *compiler*
//          from reordering the data writes/reads across the head/tail
//          publish point. A plain compiler barrier is correct and much
//          cheaper. If this is ever ported to a true SMP target, swap in
//          the "lwsync" asm below instead.
// =============================================================================
#define FIFO_COMPILER_BARRIER()  __asm__ __volatile__("" ::: "memory")
// #define FIFO_COMPILER_BARRIER()  __asm__ __volatile__("lwsync" ::: "memory") // SMP-safe alt.

typedef struct {
    char     data[FIFO_SIZE];
    volatile int head;
    volatile int tail;
} SimpleFIFO;

SimpleFIFO gui_to_eng_pipe;
SimpleFIFO eng_to_gui_pipe;

KThread engine_thread;
void   *engine_thread_stack  = NULL;
static volatile bool engine_running = false;

void fifo_init(SimpleFIFO *f) {
    memset(f, 0, sizeof(SimpleFIFO));
    f->head = 0;
    f->tail = 0;
}

// =============================================================================
// OPT-5: memcpy-based FIFO write — eliminates per-character loop + modulo
// OPT-12: Bitmask replaces modulo on all index arithmetic
// =============================================================================
void fifo_write(SimpleFIFO *f, const char *str, int len) {
    if (!f || !str || len <= 0) return;

    int h = f->head;
    int t = f->tail;

    // Available space (one slot kept empty to distinguish full vs. empty)
    int avail = (t - h - 1 + FIFO_SIZE) & FIFO_MASK;
    if (len > avail) len = avail;
    if (len == 0) return;

    int space_to_end = FIFO_SIZE - h;
    if (len <= space_to_end) {
        memcpy(&f->data[h], str, len);
    } else {
        memcpy(&f->data[h],            str,                space_to_end);
        memcpy(&f->data[0],            str + space_to_end, len - space_to_end);
    }

    FIFO_COMPILER_BARRIER();
    f->head = (h + len) & FIFO_MASK;
}

// =============================================================================
// OPT-5: memcpy-based FIFO read — eliminates per-character loop + modulo
// =============================================================================
int fifo_read(SimpleFIFO *f, char *out, int max_len) {
    if (!f || !out || max_len <= 0) return 0;

    int h = f->head;
    int t = f->tail;
    if (t == h) return 0;

    int available = (h - t + FIFO_SIZE) & FIFO_MASK;
    int len       = (available < max_len) ? available : max_len;

    int to_end = FIFO_SIZE - t;
    if (len <= to_end) {
        memcpy(out, &f->data[t], len);
    } else {
        memcpy(out,          &f->data[t], to_end);
        memcpy(out + to_end, &f->data[0], len - to_end);
    }

    FIFO_COMPILER_BARRIER();
    f->tail = (t + len) & FIFO_MASK;
    return len;
}

// Redirected stdout for the Cfish UCI thread
int engine_printf(const char *format, ...) {
    char buf[2048];
    va_list args;
    va_start(args, format);
    int ret = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    if (gui_mode_active) {
        fifo_write(&eng_to_gui_pipe, buf, ret);
    } else {
        fwrite(buf, 1, ret, stdout);
        fflush(stdout);
    }
    return ret;
}

// Redirected fgets for the Cfish UCI thread
char *engine_fgets(char *str, int num, FILE *stream) {
    (void)stream;
    if (!str || num <= 1) return NULL;
    int bytes_read = 0;
    while (bytes_read < num - 1) {
        if (!engine_running) {
            if (bytes_read == 0) return NULL;
            break;
        }
        char c;
        if (fifo_read(&gui_to_eng_pipe, &c, 1) > 0) {
            str[bytes_read++] = c;
            if (c == '\n') break;
        } else {
            if (bytes_read == 0) KThreadSleepMs(1);
        }
    }
    if (bytes_read == 0) return NULL;
    str[bytes_read] = '\0';
    return str;
}

// Redirected getline for the Cfish UCI thread
ssize_t engine_getline(char **lineptr, size_t *n, FILE *stream) {
    (void)stream;
    if (!lineptr || !n) return -1;
    if (*lineptr == NULL || *n == 0) {
        *n       = 128;
        *lineptr = malloc(*n);
        if (!*lineptr) return -1;
    }

    int i = 0;
    while (1) {
        if (!engine_running) {
            if (i == 0) return -1;
            break;
        }
        char c;
        if (fifo_read(&gui_to_eng_pipe, &c, 1) > 0) {
            if (i >= (int)*n - 1) {
                size_t  new_n   = *n * 2;
                char   *new_ptr = realloc(*lineptr, new_n);
                if (!new_ptr) {
                    free(*lineptr);
                    *lineptr = NULL;
                    *n       = 0;
                    return -1;
                }
                *lineptr = new_ptr;
                *n       = new_n;
            }
            (*lineptr)[i++] = c;
            if (c == '\n') break;
        } else {
            if (i == 0) KThreadSleepMs(1);
        }
    }
    (*lineptr)[i] = '\0';
    return (ssize_t)i;
}

// =============================================================================
// Forward declarations
// =============================================================================
void init_board(BoardState *state);
int  is_legal_gui_move(const BoardState *state, GuiMove m);
int  is_legal_gui_move_nocopy(const BoardState *state, GuiMove m);
int  has_legal_moves(const BoardState *state);
int  is_square_attacked(const BoardState *state, int sq, int attacker);
void make_gui_move(const BoardState *src, BoardState *dst, GuiMove m);
void print_side_panel_line(int panel_row);
void print_recent_moves(int row);
void send_to_engine(const char *cmd);
int  find_king(const BoardState *state, int color);
int  count_repetitions(const BoardState *state);
int  get_promo_choice();
void move_to_pgn(const BoardState *state, GuiMove m, char *buf);
void reset_engine_metrics();
void adjust_time_control();
void update_gui_cache();
void update_selected_dests_cache();

// =============================================================================
// OPT-4: find_king() — O(1) cached lookup, no linear scan
// =============================================================================
int find_king(const BoardState *state, int color) {
    return (color == 1) ? state->white_king_sq : state->black_king_sq;
}

// =============================================================================
// OPT-18: GUI board index (0 = a8, top-left, row-major) <-> Stockfish Square
//          (SQ_A1 = 0, bottom-left) mapping. The two layouts differ only in
//          rank order (files line up identically), so a single XOR of the
//          rank bits (0-based rank * 8) does the conversion in both
//          directions — the mapping is self-inverse.
// =============================================================================
static inline Square flip_sq(int sq) { return (Square)(sq ^ 56); }

// =============================================================================
// Moved before draw_ui() so no forward declaration is needed
// OPT-11: bit-shifts for rank/file arithmetic
// =============================================================================
int screen_to_board_sq(int r, int c) {
    if (board_orientation == 1) return (r << 3) + c;
    else                        return ((7 - r) << 3) + (7 - c);
}

// =============================================================================
// OPT-18: is_square_attacked() rewritten on Stockfish's own precomputed
//          attack tables (PseudoAttacks / PawnAttacks) and magic-bitboard
//          slider attacks (attacks_bb), instead of hand-rolled offset tables
//          + manual ray-walking with a bounds check at every step. Sliding
//          piece attacks go from O(distance-to-blocker) loops to a single
//          magic-multiply lookup per piece type. Relies on state->occupied
//          being kept in sync (see OPT-17 update sites).
// OPT-11: sq>>3 / sq&7 no longer needed here — table lookups replace them.
// =============================================================================
int is_square_attacked(const BoardState *state, int sq, int attacker) {
    Square s    = flip_sq(sq);
    Color  us   = (attacker == 1) ? WHITE : BLACK;
    Color  them = (us == WHITE) ? BLACK : WHITE;

    // Knight attacks
    Bitboard bb = PseudoAttacks[KNIGHT][s];
    while (bb) {
        int from = flip_sq(pop_lsb(&bb));
        if (state->board[from] == attacker * 2) return 1;
    }

    // King attacks
    bb = PseudoAttacks[KING][s];
    while (bb) {
        int from = flip_sq(pop_lsb(&bb));
        if (state->board[from] == attacker * 6) return 1;
    }

    // Pawn attacks: squares from which a pawn of color `us` attacks `s` are
    // exactly the squares a pawn of the opposite color sitting on `s` would
    // attack (mirror-symmetric offsets) — standard Stockfish idiom.
    bb = PawnAttacks[them][s];
    while (bb) {
        int from = flip_sq(pop_lsb(&bb));
        if (state->board[from] == attacker) return 1;
    }

    // Diagonal sliders (bishop / queen) — magic bitboard, blockers already
    // baked into state->occupied
    bb = attacks_bb(BISHOP, s, state->occupied);
    while (bb) {
        int from = flip_sq(pop_lsb(&bb));
        int p = state->board[from];
        if (p == attacker * 3 || p == attacker * 5) return 1;
    }

    // Straight sliders (rook / queen)
    bb = attacks_bb(ROOK, s, state->occupied);
    while (bb) {
        int from = flip_sq(pop_lsb(&bb));
        int p = state->board[from];
        if (p == attacker * 4 || p == attacker * 5) return 1;
    }

    return 0;
}

// =============================================================================
// OPT-11: All / and % on ranks/files replaced with >> and &
// OPT-18: Bishop/Rook/Queen legality now a single attacks_bb() membership
//          test instead of a manual step-by-step ray walk.
// =============================================================================
int is_pseudo_legal_move(const BoardState *state, GuiMove m) {
    int p      = state->board[m.from];
    int target = state->board[m.to];
    int turn   = state->turn;

    if (p == 0)                                                                  return 0;
    if ((turn == 1 && p < 0) || (turn == -1 && p > 0))                          return 0;
    if (m.from == m.to)                                                          return 0;
    if (target != 0 && ((turn == 1 && target > 0) || (turn == -1 && target < 0))) return 0;

    int fr = m.from >> 3, fc = m.from & 7;
    int tr = m.to   >> 3, tc = m.to   & 7;
    int dr = tr - fr,     dc = tc - fc;
    int abs_dr = (dr < 0) ? -dr : dr;
    int abs_dc = (dc < 0) ? -dc : dc;

    switch (abs(p)) {
        case 1: { // Pawn
            int dir     = (turn == 1) ? -1 : 1;
            int start_r = (turn == 1) ?  6 : 1;
            if (dc == 0 && dr == dir && target == 0) return 1;
            if (dc == 0 && fr == start_r && dr == 2 * dir) {
                if (state->board[((fr + dir) << 3) + fc] == 0 && target == 0) return 1;
            }
            if (abs_dc == 1 && dr == dir) {
                if (target != 0)       return 1;
                if (m.to == state->ep) return 1;
            }
            return 0;
        }
        case 2: // Knight
            return ((abs_dr == 2 && abs_dc == 1) || (abs_dr == 1 && abs_dc == 2));

        case 3: { // Bishop
            if (abs_dr != abs_dc) return 0;
            return (attacks_bb(BISHOP, flip_sq(m.from), state->occupied)
                    & sq_bb(flip_sq(m.to))) != 0;
        }
        case 4: { // Rook
            if (dr != 0 && dc != 0) return 0;
            return (attacks_bb(ROOK, flip_sq(m.from), state->occupied)
                    & sq_bb(flip_sq(m.to))) != 0;
        }
        case 5: { // Queen
            if (abs_dr != abs_dc && dr != 0 && dc != 0) return 0;
            return (attacks_bb(QUEEN, flip_sq(m.from), state->occupied)
                    & sq_bb(flip_sq(m.to))) != 0;
        }
        case 6: { // King — normal one-step moves
            if (abs_dr <= 1 && abs_dc <= 1) return 1;
            // Castling
            if (dr == 0 && abs_dc == 2) {
                if (turn == 1 && fr == 7 && fc == 4) {
                    if (m.to == 62 && (state->castle & 1)) {
                        if (state->board[61] == 0 && state->board[62] == 0)
                            if (!is_square_attacked(state, 60, -1) &&
                                !is_square_attacked(state, 61, -1) &&
                                !is_square_attacked(state, 62, -1)) return 1;
                    }
                    if (m.to == 58 && (state->castle & 2)) {
                        if (state->board[59] == 0 && state->board[58] == 0 && state->board[57] == 0)
                            if (!is_square_attacked(state, 60, -1) &&
                                !is_square_attacked(state, 59, -1) &&
                                !is_square_attacked(state, 58, -1)) return 1;
                    }
                }
                if (turn == -1 && fr == 0 && fc == 4) {
                    if (m.to == 6 && (state->castle & 4)) {
                        if (state->board[5] == 0 && state->board[6] == 0)
                            if (!is_square_attacked(state, 4, 1) &&
                                !is_square_attacked(state, 5, 1) &&
                                !is_square_attacked(state, 6, 1)) return 1;
                    }
                    if (m.to == 2 && (state->castle & 8)) {
                        if (state->board[3] == 0 && state->board[2] == 0 && state->board[1] == 0)
                            if (!is_square_attacked(state, 4, 1) &&
                                !is_square_attacked(state, 3, 1) &&
                                !is_square_attacked(state, 2, 1)) return 1;
                    }
                }
            }
            return 0;
        }
    }
    return 0;
}

// =============================================================================
// OPT-1: In-place mutation + restore for legality check — eliminates the
//         full BoardState copy that make_gui_move() previously required.
//         Safe because the GUI runs single-threaded.
// OPT-17: state->occupied is saved/restored as a single scalar alongside the
//          other mutated fields — O(1) regardless of how many bits changed.
// =============================================================================
int is_legal_gui_move_nocopy(const BoardState *state, GuiMove m) {
    if (!is_pseudo_legal_move(state, m)) return 0;

    BoardState *s = (BoardState *)state;

    int saved_from   = s->board[m.from];
    int saved_to     = s->board[m.to];
    int ep_cap_sq    = -1;
    int saved_ep_cap = 0;
    Bitboard saved_occupied = s->occupied;

    // Apply move in-place
    s->board[m.from] = 0;
    s->board[m.to]   = (m.promo != 0) ? state->turn * m.promo : saved_from;
    s->occupied = inv_sq(s->occupied, flip_sq(m.from));
    s->occupied |= sq_bb(flip_sq(m.to));

    // En-passant: remove the captured pawn
    if (abs(saved_from) == 1 && m.to == state->ep) {
        ep_cap_sq           = m.to + (state->turn == 1 ? 8 : -8);
        saved_ep_cap        = s->board[ep_cap_sq];
        s->board[ep_cap_sq] = 0;
        s->occupied = inv_sq(s->occupied, flip_sq(ep_cap_sq));
    }

    // OPT-4: Temporarily update cached king square for king moves
    int saved_wk = s->white_king_sq;
    int saved_bk = s->black_king_sq;
    if (abs(saved_from) == 6) {
        if (state->turn == 1) s->white_king_sq = m.to;
        else                  s->black_king_sq = m.to;
    }

    int king  = find_king(s, state->turn);
    int legal = (king != -1) && !is_square_attacked(s, king, -state->turn);

    // Restore all mutated fields
    s->board[m.from] = saved_from;
    s->board[m.to]   = saved_to;
    if (ep_cap_sq != -1) s->board[ep_cap_sq] = saved_ep_cap;
    s->white_king_sq = saved_wk;
    s->black_king_sq = saved_bk;
    s->occupied      = saved_occupied;

    return legal;
}

// Thin wrapper — all callers now use the no-copy path
int is_legal_gui_move(const BoardState *state, GuiMove m) {
    return is_legal_gui_move_nocopy(state, m);
}

// =============================================================================
// OPT-1: Pre-collects own piece squares, filters friendly targets early,
//         uses is_legal_gui_move_nocopy() to avoid any BoardState copies.
// =============================================================================
int has_legal_moves(const BoardState *state) {
    int own_pieces[16];
    int own_count = 0;

    for (int f = 0; f < 64 && own_count < 16; f++) {
        int p = state->board[f];
        if (p == 0) continue;
        if (state->turn == 1 ? p > 0 : p < 0) own_pieces[own_count++] = f;
    }

    for (int pi = 0; pi < own_count; pi++) {
        int f     = own_pieces[pi];
        int abs_p = abs(state->board[f]);

        for (int t = 0; t < 64; t++) {
            int tgt = state->board[t];
            if (tgt != 0 && (state->turn == 1 ? tgt > 0 : tgt < 0)) continue;

            GuiMove m = {f, t, 0};
            if (abs_p == 1 && ((t >> 3) == 0 || (t >> 3) == 7)) m.promo = 5;
            if (is_legal_gui_move_nocopy(state, m)) return 1;
        }
    }
    return 0;
}

int count_repetitions(const BoardState *state) {
    int count = 1;
    for (int i = 0; i < history_count; i++) {
        if (state->turn   == history[i].turn   &&
            state->castle == history[i].castle &&
            state->ep     == history[i].ep     &&
            memcmp(state->board, history[i].board, sizeof(state->board)) == 0) {
            count++;
        }
    }
    return count;
}

// =============================================================================
// OPT-4: Updates cached king squares on every king move
// OPT-11: Bit-shifts for rank/file arithmetic
// OPT-17: Incrementally maintains state->occupied alongside board[] so
//          attacks_bb()/is_square_attacked() never need to rebuild it.
// =============================================================================
void make_gui_move(const BoardState *src, BoardState *dst, GuiMove m) {
    *dst = *src;
    int p = dst->board[m.from];

    int is_capture = (src->board[m.to] != 0) || (abs(p) == 1 && m.to == src->ep);

    dst->board[m.from] = 0;
    dst->board[m.to]   = (m.promo != 0) ? dst->turn * m.promo : p;

    // OPT-17: origin vacated, destination occupied (idempotent on captures,
    // since a captured square's bit was already set and simply stays set).
    dst->occupied = inv_sq(dst->occupied, flip_sq(m.from));
    dst->occupied |= sq_bb(flip_sq(m.to));

    // En-passant capture
    if (abs(p) == 1 && m.to == dst->ep) {
        int p_dir  = (dst->turn == 1) ? 8 : -8;
        int cap_sq = m.to + p_dir;
        dst->board[cap_sq] = 0;
        dst->occupied = inv_sq(dst->occupied, flip_sq(cap_sq));
    }

    // Update en-passant square
    dst->ep = -1;
    if (abs(p) == 1) {
        int diff = m.from - m.to;
        if (diff == 16 || diff == -16)
            dst->ep = m.from + (dst->turn == 1 ? -8 : 8);
    }

    // Castling rook moves + OPT-4: update cached king square
    if (abs(p) == 6) {
        if (m.from == 60 && m.to == 62) {
            dst->board[61] = dst->board[63]; dst->board[63] = 0;
            dst->occupied |= sq_bb(flip_sq(61));
            dst->occupied = inv_sq(dst->occupied, flip_sq(63));
        } else if (m.from == 60 && m.to == 58) {
            dst->board[59] = dst->board[56]; dst->board[56] = 0;
            dst->occupied |= sq_bb(flip_sq(59));
            dst->occupied = inv_sq(dst->occupied, flip_sq(56));
        } else if (m.from == 4 && m.to == 6) {
            dst->board[5] = dst->board[7]; dst->board[7] = 0;
            dst->occupied |= sq_bb(flip_sq(5));
            dst->occupied = inv_sq(dst->occupied, flip_sq(7));
        } else if (m.from == 4 && m.to == 2) {
            dst->board[3] = dst->board[0]; dst->board[0] = 0;
            dst->occupied |= sq_bb(flip_sq(3));
            dst->occupied = inv_sq(dst->occupied, flip_sq(0));
        }

        if (dst->turn == 1) dst->white_king_sq = m.to;
        else                dst->black_king_sq = m.to;
    }

    // Castling rights
    if (m.from == 60)                dst->castle &= ~3;
    if (m.from == 4)                 dst->castle &= ~12;
    if (m.from == 63 || m.to == 63) dst->castle &= ~1;
    if (m.from == 56 || m.to == 56) dst->castle &= ~2;
    if (m.from == 7  || m.to == 7)  dst->castle &= ~4;
    if (m.from == 0  || m.to == 0)  dst->castle &= ~8;

    dst->turn = -dst->turn;

    if (abs(p) == 1 || is_capture) dst->halfmoves = 0;
    else                           dst->halfmoves++;

    if (dst->turn == 1) dst->fullmoves++;
}

// =============================================================================
// OPT-10: push_state() computes and caches the PGN string immediately
// =============================================================================
void move_to_pgn(const BoardState *state, GuiMove m, char *buf);

void push_state(const BoardState *state, GuiMove m) {
    if (history_count < MAX_HISTORY - 1) {
        history[history_count]      = *state;
        move_history[history_count] = m;
        move_to_pgn(state, m, pgn_cache[history_count]);
        history_count++;
    }
}

void reset_engine_metrics() {
    engine_nps        = 0;
    engine_score_type = -1;
    engine_score_val  = 0;
    engine_pv[0]      = '\0';
    engine_depth      = 0;
    engine_seldepth   = 0;
    engine_time_ms    = 0;
    engine_nodes      = 0;
    engine_hashfull   = 0;
    engine_tbhits     = 0;
}

void move_to_uci(GuiMove m, char *buf) {
    int f_col = m.from & 7,  f_row = 8 - (m.from >> 3);
    int t_col = m.to   & 7,  t_row = 8 - (m.to   >> 3);
    buf[0] = 'a' + f_col; buf[1] = '0' + f_row;
    buf[2] = 'a' + t_col; buf[3] = '0' + t_row;
    if (m.promo != 0) {
        buf[4] = (m.promo == 2) ? 'n' : (m.promo == 3) ? 'b' : (m.promo == 4) ? 'r' : 'q';
        buf[5] = '\0';
    } else {
        buf[4] = '\0';
    }
}

// =============================================================================
// OPT-10: move_to_pgn() — called once per move in push_state(), never per frame
// OPT-1:  Uses is_legal_gui_move_nocopy() for disambiguation checks
// OPT-4:  find_king() is O(1)
// OPT-11: Bit-shifts for rank/file arithmetic
// =============================================================================
void move_to_pgn(const BoardState *state, GuiMove m, char *buf) {
    int p      = state->board[m.from];
    int abs_p  = abs(p);
    int target = state->board[m.to];
    int is_cap = (target != 0) || (abs_p == 1 && m.to == state->ep);

    int dist = m.from - m.to;
    if (dist < 0) dist = -dist;

    if (abs_p == 6 && dist == 2) {
        strcpy(buf, (m.to > m.from) ? "O-O" : "O-O-O");
    } else {
        char *ptr = buf;

        if (abs_p == 1) {
            if (is_cap) { *ptr++ = 'a' + (m.from & 7); *ptr++ = 'x'; }
        } else {
            static const char piece_chars[] = "\0\0NBRQK";
            *ptr++ = piece_chars[abs_p];

            if (abs_p >= 2 && abs_p <= 5) {
                int file_conflict = 0, rank_conflict = 0, conflict_exists = 0;
                for (int sq = 0; sq < 64; sq++) {
                    if (sq == m.from || state->board[sq] != p) continue;
                    GuiMove test_m = {sq, m.to, 0};
                    if (is_legal_gui_move_nocopy(state, test_m)) {
                        conflict_exists = 1;
                        if ((sq & 7) == (m.from & 7)) file_conflict = 1;
                        if ((sq >> 3) == (m.from >> 3)) rank_conflict = 1;
                    }
                }
                if (conflict_exists) {
                    if      (!file_conflict) *ptr++ = 'a' + (m.from & 7);
                    else if (!rank_conflict) *ptr++ = '8' - (m.from >> 3);
                    else {
                        *ptr++ = 'a' + (m.from & 7);
                        *ptr++ = '8' - (m.from >> 3);
                    }
                }
            }

            if (is_cap) *ptr++ = 'x';
        }

        *ptr++ = 'a' + (m.to & 7);
        *ptr++ = '8' - (m.to >> 3);

        if (abs_p == 1 && m.promo != 0) {
            *ptr++ = '=';
            static const char promo_chars[] = "\0\0NBRQ";
            *ptr++ = promo_chars[m.promo];
        }
        *ptr = '\0';
    }

    // Check / checkmate annotation
    BoardState next;
    make_gui_move(state, &next, m);
    int opp_king = find_king(&next, next.turn);
    if (opp_king != -1 && is_square_attacked(&next, opp_king, -next.turn)) {
        strcat(buf, has_legal_moves(&next) ? "+" : "#");
    }
}

GuiMove uci_to_gui_move(const char *str) {
    GuiMove m = {-1, -1, 0};
    if (strlen(str) < 4) return m;
    int f_col = str[0] - 'a', f_row = 8 - (str[1] - '0');
    int t_col = str[2] - 'a', t_row = 8 - (str[3] - '0');
    m.from = (f_row << 3) + f_col;
    m.to   = (t_row << 3) + t_col;
    if (str[4] != '\0') {
        char p = str[4];
        m.promo = (p == 'n') ? 2 : (p == 'b') ? 3 : (p == 'r') ? 4 : 5;
    }
    return m;
}

// =============================================================================
// OPT-1/11: update_selected_dests_cache() uses the no-copy legality check
//            and bit-shifts for rank detection
// =============================================================================
void update_selected_dests_cache() {
    cached_gui.legal_dests_mask = 0ULL;
    if (selected_sq == -1) return;

    int abs_p = abs(current_state.board[selected_sq]);

    for (int sq = 0; sq < 64; sq++) {
        int tgt = current_state.board[sq];
        if (tgt != 0 && (current_state.turn == 1 ? tgt > 0 : tgt < 0)) continue;

        GuiMove test_m = {selected_sq, sq, 0};
        if (abs_p == 1 && ((sq >> 3) == 0 || (sq >> 3) == 7)) test_m.promo = 5;
        if (is_legal_gui_move_nocopy(&current_state, test_m))
            cached_gui.legal_dests_mask |= (1ULL << sq);
    }
}

// =============================================================================
// OPT-3: update_gui_cache() caches the repetition count
// =============================================================================
void update_gui_cache() {
    cached_gui.has_legal_moves  = has_legal_moves(&current_state);
    cached_gui.repetition_count = count_repetitions(&current_state);

    int king_in_check = -1;
    int w_king = find_king(&current_state, 1);
    int b_king = find_king(&current_state, -1);
    if (w_king != -1 && is_square_attacked(&current_state, w_king, -1)) king_in_check = w_king;
    else if (b_king != -1 && is_square_attacked(&current_state, b_king, 1)) king_in_check = b_king;
    cached_gui.king_in_check_sq = king_in_check;

    update_selected_dests_cache();
}

// =============================================================================
// OPT-7: draw_ui() builds the entire frame into a single buffer then
//         flushes it with one fwrite() call instead of 50+ printf() calls.
// OPT-16: fb_raw()/FB_LIT() avoid runtime strlen() for literals whose length
//          is known at compile time via sizeof(); the 64x-per-frame board
//          cell render no longer goes through vsnprintf() format parsing.
// =============================================================================
static char frame_buf[16384];
static int  frame_pos = 0;

static inline void fb_raw(const char *s, int l) {
    if (frame_pos + l < (int)sizeof(frame_buf) - 1) {
        memcpy(frame_buf + frame_pos, s, l);
        frame_pos += l;
    }
}
#define FB_LIT(s) fb_raw((s), (int)(sizeof(s) - 1))

static inline void fb_str(const char *s) {
    fb_raw(s, (int)strlen(s));
}

static inline void fb_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(frame_buf + frame_pos,
                            sizeof(frame_buf) - frame_pos, fmt, ap);
    va_end(ap);
    if (written > 0) frame_pos += written;
}

void draw_ui() {
    frame_pos = 0;

    FB_LIT("\033[H\r\n");

    const char *turn_str = (current_state.turn == 1)
                           ? "\033[1;33mWhite\033[0m"
                           : "\033[1;35mBlack\033[0m";

    int is_ch       = (cached_gui.king_in_check_sq != -1);
    int has_mov     = cached_gui.has_legal_moves;
    int repetitions = cached_gui.repetition_count;

    const char *w_play = (user_side == 1 || user_side == 0) ? "Hum" : "Eng";
    const char *b_play = (user_side == -1 || user_side == 0) ? "Hum" : "Eng";

    FB_LIT("  ");
    if (current_state.halfmoves >= 100) {
        FB_LIT("\033[1;36mDRAW (50-move rule)\033[0m");
    } else if (repetitions >= 3) {
        FB_LIT("\033[1;36mDRAW (threefold repetition)\033[0m");
    } else if (!has_mov) {
        if (is_ch) FB_LIT("\033[1;31mCHECKMATE!\033[0m");
        else       FB_LIT("\033[1;36mSTALEMATE!\033[0m");
    } else if (is_ch) {
        fb_printf("%s (\033[1;31mCHECK!\033[0m)", turn_str);
    } else {
        fb_printf("%s's Turn", turn_str);
    }
    fb_printf(" | W:%s B:%s", w_play, b_play);

    static const char *types[] = {"Time-Limit", "Depth-Limit", "Node-Limit"};
    fb_printf(" | %s", types[time_control_type]);
    if      (time_control_type == 0) fb_printf(" (%d ms)",    time_control_val);
    else if (time_control_type == 1) fb_printf(" (depth %d)", time_control_val);
    else                             fb_printf(" (%d nodes)",  time_control_val);
    FB_LIT("\033[K\r\n\r\n");

    if (board_orientation == 1) FB_LIT("     a  b  c  d  e  f  g  h    ");
    else                        FB_LIT("     h  g  f  e  d  c  b  a    ");
    print_side_panel_line(0);
    FB_LIT("\033[K\r\n");

    for (int r = 0; r < 8; r++) {
        int rank_lbl = (board_orientation == 1) ? (8 - r) : (r + 1);
        fb_printf("  %d ", rank_lbl);

        for (int c = 0; c < 8; c++) {
            int sq = screen_to_board_sq(r, c);
            int p  = current_state.board[sq];

            int is_light    = (((sq >> 3) + (sq & 7)) & 1) == 0;
            int is_selected = (sq == selected_sq);
            int is_cursor   = (r == cursor_r && c == cursor_c);

            int is_prev_move = 0;
            if (history_count > 0) {
                GuiMove lm = move_history[history_count - 1];
                if (sq == lm.from || sq == lm.to) is_prev_move = 1;
            }

            int is_legal_dest = (cached_gui.legal_dests_mask >> sq) & 1;

            const char *bg_color;
            if      (is_cursor)                          bg_color = "\033[48;5;208m";
            else if (is_selected)                        bg_color = "\033[48;5;34m";
            else if (sq == cached_gui.king_in_check_sq)  bg_color = "\033[48;5;196m";
            else if (is_prev_move)
                bg_color = is_light ? "\033[48;5;75m"  : "\033[48;5;68m";
            else if (is_legal_dest)
                bg_color = is_light ? "\033[48;5;151m" : "\033[48;5;108m";
            else
                bg_color = is_light ? "\033[48;5;180m" : "\033[48;5;94m";

            const char *piece_str = " ";
            const char *fg_color  = "\033[38;5;232m";
            if (p != 0) {
                if (p > 0) fg_color = "\033[38;5;255m\033[1m";
                switch (abs(p)) {
                    case 1: piece_str = "P"; break;
                    case 2: piece_str = "N"; break;
                    case 3: piece_str = "B"; break;
                    case 4: piece_str = "R"; break;
                    case 5: piece_str = "Q"; break;
                    case 6: piece_str = "K"; break;
                }
            }

            // OPT-16: No format-string parsing for the 64x-per-frame cell render.
            fb_str(bg_color);
            fb_str(fg_color);
            FB_LIT(" ");
            fb_str(piece_str);
            FB_LIT(" \033[0m");
        }

        fb_printf(" %d ", rank_lbl);
        print_side_panel_line(r + 1);
        FB_LIT("\033[K\r\n");
    }

    if (board_orientation == 1) FB_LIT("     a  b  c  d  e  f  g  h    ");
    else                        FB_LIT("     h  g  f  e  d  c  b  a    ");
    print_side_panel_line(9);
    FB_LIT("\033[K\r\n\r\n");

    FB_LIT(" \033[38;5;245m[D-PAD] Move | [A] Select | [B] Undo | [1] Cycle Level | [2] Flip | [+] Sides | [-] Cycle Type | [HOME] Quit\033[0m\033[K\r\n");

    FB_LIT(" \033[1;34mEngine State:\033[0m ");
    if      (engine_thinking)      FB_LIT("\033[1;32mThinking\033[0m");
    else if (engine_recvd_readyok) FB_LIT("\033[1;30mIdle (Ready)\033[0m");
    else                           FB_LIT("\033[1;31mConnecting...\033[0m");

    if      (engine_nps >= 1000000) fb_printf(" | \033[38;5;250mSpeed:\033[0m %.2fM nps", (double)engine_nps / 1000000.0);
    else if (engine_nps >= 1000)    fb_printf(" | \033[38;5;250mSpeed:\033[0m %.1fk nps", (double)engine_nps / 1000.0);
    else                            fb_printf(" | \033[38;5;250mSpeed:\033[0m %lld nps",   engine_nps);

    if (engine_score_type == 0) {
        if      (engine_score_val > 0) fb_printf(" | \033[38;5;250mEval:\033[0m \033[1;32m%+.2f\033[0m", (double)engine_score_val / 100.0);
        else if (engine_score_val < 0) fb_printf(" | \033[38;5;250mEval:\033[0m \033[1;31m%+.2f\033[0m", (double)engine_score_val / 100.0);
        else                           FB_LIT  (" | \033[38;5;250mEval:\033[0m 0.00");
    } else if (engine_score_type == 1) {
        if      (engine_score_val > 0) fb_printf(" | \033[38;5;250mEval:\033[0m \033[1;32m+M%d\033[0m",  engine_score_val);
        else if (engine_score_val < 0) fb_printf(" | \033[38;5;250mEval:\033[0m \033[1;31m-M%d\033[0m", -engine_score_val);
        else                           FB_LIT  (" | \033[38;5;250mEval:\033[0m M0");
    } else {
        FB_LIT(" | \033[38;5;250mEval:\033[0m -");
    }
    FB_LIT("\033[K\r\n");

    if (engine_pv[0] != '\0')
        fb_printf(" \033[1;33mBest Line:\033[0m \033[38;5;250m%s\033[0m\033[K\r\n", engine_pv);
    else
        FB_LIT   (" \033[1;33mBest Line:\033[0m \033[38;5;243m-\033[0m\033[K\r\n");

    FB_LIT("\033[J");

    fwrite(frame_buf, 1, frame_pos, stdout);
    fflush(stdout);
}

// print_side_panel_line and print_recent_moves write into frame_buf via fb_printf
void print_side_panel_line(int panel_row) {
    FB_LIT(" ");
    print_recent_moves(panel_row);
}

void print_recent_moves(int row) {
    int total_full_moves = (history_count + 1) >> 1;
    if (total_full_moves == 0) return;

    int start_move = (total_full_moves > 10) ? total_full_moves - 9 : 1;
    int display    = start_move + row;
    if (display > total_full_moves) return;

    int w_idx = (display - 1) * 2;
    int b_idx = w_idx + 1;

    fb_printf("   %2d. ", display);

    if (w_idx < history_count) fb_printf("%-6s", pgn_cache[w_idx]);
    else                       FB_LIT("------");

    FB_LIT(" ");

    if (b_idx < history_count)      fb_printf("%-6s", pgn_cache[b_idx]);
    else if (w_idx < history_count) FB_LIT("...");
    else                            FB_LIT("------");
}

void gui_cleanup() {
    printf("\033[?25h\033[2J\033[H");
    fflush(stdout);
}

void init_wii_console() {
    VIDEO_Init();
    WPAD_Init();
    fatInitDefault();

    GXRModeObj *rmode = VIDEO_GetPreferredMode(NULL);
    void *xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
    console_init(xfb, 20, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE) VIDEO_WaitVSync();
}

static sptr engine_thread_main(void *arg) {
    (void)arg;

    psqt_init();
    bitboards_init();
    zob_init();
    bitbases_init();
#ifndef NNUE_PURE
    endgames_init();
#endif
    threads_init();
    options_init();
    search_clear();

    char *engine_argv[] = {"ucichess", NULL};
    uci_loop(1, engine_argv);

    engine_running = false;
    return 0;
}

void send_to_engine(const char *cmd) {
    fifo_write(&gui_to_eng_pipe, cmd, strlen(cmd));
}

void start_engine() {
    engine_recvd_uciok   = false;
    engine_recvd_readyok = false;

    fifo_init(&gui_to_eng_pipe);
    fifo_init(&eng_to_gui_pipe);

    engine_fgets_hook   = engine_fgets;
    engine_printf_hook  = engine_printf;
    engine_getline_hook = engine_getline;

    engine_running      = true;
    engine_thread_stack = memalign(32, 512 * 1024);

    KThreadPrepare(&engine_thread, engine_thread_main, NULL, engine_thread_stack, 0x3f);
    KThreadResume(&engine_thread);

    send_to_engine("uci\nisready\n");
}

// =============================================================================
// OPT-6: trigger_engine_move() uses a write-pointer + memcpy
// =============================================================================
void trigger_engine_move() {
    reset_engine_metrics();

    static char cmd[16384];
    char *ptr = cmd;
    char *end = cmd + sizeof(cmd) - 8;

    static const char prefix[] = "position startpos moves";
    memcpy(ptr, prefix, sizeof(prefix) - 1);
    ptr += sizeof(prefix) - 1;

    for (int i = 0; i < history_count && ptr < end; i++) {
        char uci_m[8];
        move_to_uci(move_history[i], uci_m);
        int mlen = (uci_m[4] != '\0') ? 5 : 4;
        *ptr++ = ' ';
        memcpy(ptr, uci_m, mlen);
        ptr += mlen;
    }
    *ptr++ = '\n';
    *ptr   = '\0';

    send_to_engine(cmd);

    char go_cmd[64];
    if      (time_control_type == 0) snprintf(go_cmd, sizeof(go_cmd), "go movetime %d\n", time_control_val);
    else if (time_control_type == 1) snprintf(go_cmd, sizeof(go_cmd), "go depth %d\n",    time_control_val);
    else                             snprintf(go_cmd, sizeof(go_cmd), "go nodes %d\n",     time_control_val);
    send_to_engine(go_cmd);
}

void process_engine_output(char *line) {
    int len = strlen(line);
    while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n' ||
                       line[len-1] == ' '  || line[len-1] == '\t')) {
        line[--len] = '\0';
    }

    if (strcmp(line, "uciok")   == 0) engine_recvd_uciok   = true;
    if (strcmp(line, "readyok") == 0) engine_recvd_readyok = true;

    if (strncmp(line, "info", 4) == 0) {
        char *ptr;

        if ((ptr = strstr(line, " nps "))) {
            long long val;
            if (sscanf(ptr, " nps %lld", &val) == 1) engine_nps = val;
        }
        if ((ptr = strstr(line, " score "))) {
            int score_val = 0; char score_type[16];
            if (sscanf(ptr, " score %15s %d", score_type, &score_val) == 2) {
                if      (strcmp(score_type, "cp")   == 0) { engine_score_type = 0; engine_score_val = score_val * current_state.turn; }
                else if (strcmp(score_type, "mate") == 0) { engine_score_type = 1; engine_score_val = score_val * current_state.turn; }
            }
        }
        if ((ptr = strstr(line, " depth ")))    { int d  = 0; if (sscanf(ptr, " depth %d",    &d)  == 1) engine_depth    = d; }
        if ((ptr = strstr(line, " seldepth "))) { int sd = 0; if (sscanf(ptr, " seldepth %d", &sd) == 1) engine_seldepth = sd; }
        if ((ptr = strstr(line, " time ")))     { int t  = 0; if (sscanf(ptr, " time %d",     &t)  == 1) engine_time_ms  = t; }
        if ((ptr = strstr(line, " nodes ")))    { long long n  = 0; if (sscanf(ptr, " nodes %lld",   &n)  == 1) engine_nodes    = n; }
        if ((ptr = strstr(line, " hashfull "))) { int h  = 0; if (sscanf(ptr, " hashfull %d", &h)  == 1) engine_hashfull = h; }
        if ((ptr = strstr(line, " tbhits ")))   { long long tb = 0; if (sscanf(ptr, " tbhits %lld", &tb) == 1) engine_tbhits   = tb; }
        // =====================================================================
        // OPT-14: Replaced strncpy() with strlen()+memcpy(). strncpy() zero-
        //          pads the *entire* remaining destination buffer when the
        //          source is shorter than n, which fired on every single
        //          "info ... pv ..." line (the PV is almost always << 1024
        //          bytes) — wasting ~800-900 bytes of zeroing per UCI info
        //          line emitted by the engine.
        // =====================================================================
        if ((ptr = strstr(line, " pv "))) {
            const char *src = ptr + 4;
            size_t l = strlen(src);
            if (l >= sizeof(engine_pv)) l = sizeof(engine_pv) - 1;
            memcpy(engine_pv, src, l);
            engine_pv[l] = '\0';
        }
    }

    if (strncmp(line, "bestmove", 8) == 0) {
        char move_str[16];
        if (sscanf(line, "bestmove %15s", move_str) == 1) {
            if (strcmp(move_str, "(none)") == 0 || strcmp(move_str, "NULL") == 0) {
                engine_thinking = 0;
                return;
            }
            GuiMove m = uci_to_gui_move(move_str);
            if (is_legal_gui_move_nocopy(&current_state, m)) {
                push_state(&current_state, m);
                BoardState next;
                make_gui_move(&current_state, &next, m);
                current_state = next;
                update_gui_cache();
            }
            engine_thinking  = 0;
            gui_needs_redraw = true;
        }
    }
}

char engine_buffer[8192];
int  engine_buf_len = 0;

void read_from_engine() {
    char tmp[2048];
    int  n;
    while ((n = fifo_read(&eng_to_gui_pipe, tmp, sizeof(tmp) - 1)) > 0) {
        tmp[n] = '\0';
        for (int i = 0; i < n; i++) {
            if (tmp[i] == '\n') {
                if (engine_buf_len > 0) {
                    engine_buffer[engine_buf_len] = '\0';
                    process_engine_output(engine_buffer);
                    engine_buf_len = 0;
                }
            } else if (tmp[i] != '\r') {
                if (engine_buf_len < (int)sizeof(engine_buffer) - 1) {
                    engine_buffer[engine_buf_len++] = tmp[i];
                } else {
                    engine_buffer[engine_buf_len] = '\0';
                    process_engine_output(engine_buffer);
                    engine_buf_len = 0;
                    engine_buffer[engine_buf_len++] = tmp[i];
                }
            }
        }
    }
}

int get_promo_choice() {
    const char *prompt = "\r\n \033[1;33mPromote pawn to: [A] Queen  [B] Rook  [+] Bishop  [-] Knight\033[0m";
    fwrite(prompt, 1, strlen(prompt), stdout);
    fflush(stdout);

    int choice = 5;
    while (1) {
        WPAD_ScanPads();
        u32 pressed = WPAD_ButtonsDown(0);
        if (pressed & WPAD_BUTTON_A)     { choice = 5; break; }
        if (pressed & WPAD_BUTTON_B)     { choice = 4; break; }
        if (pressed & WPAD_BUTTON_PLUS)  { choice = 3; break; }
        if (pressed & WPAD_BUTTON_MINUS) { choice = 2; break; }
        VIDEO_WaitVSync();
    }
    const char *clear = "\r\033[K\033[A\033[K";
    fwrite(clear, 1, strlen(clear), stdout);
    fflush(stdout);
    return choice;
}

void handle_select() {
    if (!cached_gui.has_legal_moves ||
        current_state.halfmoves >= 100 ||
        cached_gui.repetition_count >= 3) return;
    if (engine_thinking)                           return;
    if (user_side == 2)                            return;
    if (user_side == 1  && current_state.turn != 1)  return;
    if (user_side == -1 && current_state.turn != -1) return;

    int sq = screen_to_board_sq(cursor_r, cursor_c);
    if (selected_sq == -1) {
        int p = current_state.board[sq];
        if (p != 0 && ((current_state.turn == 1 && p > 0) || (current_state.turn == -1 && p < 0))) {
            selected_sq = sq;
            update_selected_dests_cache();
            gui_needs_redraw = true;
        }
    } else {
        GuiMove m    = {selected_sq, sq, 0};
        int p        = current_state.board[selected_sq];
        int is_promo = (abs(p) == 1 && ((sq >> 3) == 0 || (sq >> 3) == 7));
        if (is_promo) m.promo = 5;

        if (is_legal_gui_move_nocopy(&current_state, m)) {
            if (is_promo) m.promo = get_promo_choice();
            push_state(&current_state, m);
            BoardState next;
            make_gui_move(&current_state, &next, m);
            current_state = next;
            selected_sq   = -1;
            reset_engine_metrics();
            update_gui_cache();
            gui_needs_redraw = true;
        } else {
            int target = current_state.board[sq];
            if (target != 0 && ((current_state.turn == 1 && target > 0) ||
                                (current_state.turn == -1 && target < 0))) {
                selected_sq = sq;
            } else {
                selected_sq = -1;
            }
            update_selected_dests_cache();
            gui_needs_redraw = true;
        }
    }
}

void handle_undo() {
    if (engine_thinking) { send_to_engine("stop\n"); engine_thinking = 0; }
    reset_engine_metrics();
    int step_back = (user_side == 1 || user_side == -1) ? 2 : 1;
    while (step_back-- > 0 && history_count > 0) {
        history_count--;
        current_state = history[history_count];
    }
    selected_sq = -1;
    update_gui_cache();
    gui_needs_redraw = true;
}

void handle_reset_board() {
    if (engine_thinking) { send_to_engine("stop\n"); engine_thinking = 0; }
    reset_engine_metrics();
    init_board(&current_state);
    history_count = 0;
    selected_sq   = -1;
    cursor_r      = 6;
    cursor_c      = 4;
    send_to_engine("ucinewgame\nisready\n");
    update_gui_cache();
    gui_needs_redraw = true;
}

void handle_switch_sides() {
    if (engine_thinking) { send_to_engine("stop\n"); engine_thinking = 0; }
    selected_sq = -1;
    if      (user_side == 1)  user_side = -1;
    else if (user_side == -1) user_side = 0;
    else if (user_side == 0)  user_side = 2;
    else                      user_side = 1;
    update_gui_cache();
    gui_needs_redraw = true;
}

void adjust_time_control() {
    if (time_control_type == 0) {
        static const int time_list[]  = {1,10,50,100,500,1000,1500,2000,3000,5000,10000};
        static const int time_count   = sizeof(time_list) / sizeof(time_list[0]);
        int idx = -1;
        for (int i = 0; i < time_count; i++) {
            if (time_control_val == time_list[i]) { idx = i; break; }
        }
        time_control_val = (idx == -1 || idx == time_count - 1) ? time_list[0] : time_list[idx + 1];
    } else if (time_control_type == 1) {
        time_control_val = (time_control_val % 20) + 1;
    } else {
        static const int nodes_list[] = {512,1024,2048,4096,8192,16384,32768,65536,
                                         131072,262144,524288,1048576,2097152,4194304,8388608};
        static const int nodes_count  = sizeof(nodes_list) / sizeof(nodes_list[0]);
        int idx = -1;
        for (int i = 0; i < nodes_count; i++) {
            if (time_control_val == nodes_list[i]) { idx = i; break; }
        }
        time_control_val = (idx == -1 || idx == nodes_count - 1) ? nodes_list[0] : nodes_list[idx + 1];
    }
}

void handle_wii_input() {
    WPAD_ScanPads();
    u32 pressed = WPAD_ButtonsDown(0);

    if (pressed & WPAD_BUTTON_UP)    { if (cursor_r > 0) { cursor_r--; gui_needs_redraw = true; } }
    if (pressed & WPAD_BUTTON_DOWN)  { if (cursor_r < 7) { cursor_r++; gui_needs_redraw = true; } }
    if (pressed & WPAD_BUTTON_LEFT)  { if (cursor_c > 0) { cursor_c--; gui_needs_redraw = true; } }
    if (pressed & WPAD_BUTTON_RIGHT) { if (cursor_c < 7) { cursor_c++; gui_needs_redraw = true; } }
    if (pressed & WPAD_BUTTON_A)     { handle_select(); }
    if (pressed & WPAD_BUTTON_B)     { handle_undo(); }
    if (pressed & WPAD_BUTTON_1)     { adjust_time_control(); gui_needs_redraw = true; }
    if (pressed & WPAD_BUTTON_2)     { board_orientation = -board_orientation; gui_needs_redraw = true; }
    if (pressed & WPAD_BUTTON_PLUS)  { handle_switch_sides(); }
    if (pressed & WPAD_BUTTON_MINUS) {
        time_control_type = (time_control_type + 1) % 3;
        time_control_val  = (time_control_type == 0) ? 1 : (time_control_type == 1 ? 1 : 512);
        gui_needs_redraw  = true;
    }
    if (pressed & WPAD_BUTTON_HOME) { exit(0); }
}

// =============================================================================
// OPT-4: init_board() initialises the cached king squares
// OPT-17: init_board() also builds the initial occupancy bitboard
// =============================================================================
void init_board(BoardState *state) {
    static const int start[64] = {
        -4,-2,-3,-5,-6,-3,-2,-4,
        -1,-1,-1,-1,-1,-1,-1,-1,
         0, 0, 0, 0, 0, 0, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 0,
         0, 0, 0, 0, 0, 0, 0, 0,
         1, 1, 1, 1, 1, 1, 1, 1,
         4, 2, 3, 5, 6, 3, 2, 4
    };
    memcpy(state->board, start, sizeof(start));
    state->turn          = 1;
    state->castle        = 15;
    state->ep            = -1;
    state->halfmoves     = 0;
    state->fullmoves     = 1;
    state->white_king_sq = 60; // e1
    state->black_king_sq = 4;  // e8

    state->occupied = 0;
    for (int sq = 0; sq < 64; sq++)
        if (start[sq] != 0) state->occupied |= sq_bb(flip_sq(sq));
}

int run_gui_mode() {
    init_board(&current_state);
    update_gui_cache();
    start_engine();

    int live_ticks = 0;

    while (1) {
        int engine_active = 0;
        if (cached_gui.has_legal_moves &&
            current_state.halfmoves < 100 &&
            cached_gui.repetition_count < 3) {
            if      (user_side == 2)                             engine_active = 1;
            else if (user_side == 1  && current_state.turn == -1) engine_active = 1;
            else if (user_side == -1 && current_state.turn == 1)  engine_active = 1;
        }

        if (engine_active && !engine_thinking) {
            engine_thinking  = 1;
            trigger_engine_move();
            gui_needs_redraw = true;
        }

        handle_wii_input();
        read_from_engine();

        if (engine_thinking) {
            if (++live_ticks >= 10) { gui_needs_redraw = true; live_ticks = 0; }
        } else {
            live_ticks = 0;
        }

        if (gui_needs_redraw) {
            draw_ui();
            gui_needs_redraw = false;
        }

        KThreadSleepMs(16);
    }
    return 0;
}

int main(int argc, char **argv) {
    init_wii_console();

    // =========================================================================
    // OPT-19: bitboards_init() must run before any is_square_attacked()/
    //          attacks_bb() call. GUI mode calls init_board()+update_gui_cache()
    //          (which uses both) *before* the engine thread — which also calls
    //          bitboards_init() — has even started, so PseudoAttacks/
    //          PawnAttacks/magic tables would otherwise still be zeroed for
    //          the GUI's very first legality scan.
    // =========================================================================
    bitboards_init();

    bool force_gui = true;
    bool force_cli = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cli") == 0 || strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "uci") == 0) {
            force_cli = true;
            force_gui = false;
        }
    }

    if (force_cli) {
        gui_mode_active = false;
        print_engine_info(false);

        psqt_init();
        bitboards_init();
        zob_init();
        bitbases_init();
#ifndef NNUE_PURE
        endgames_init();
#endif
        threads_init();
        options_init();
        search_clear();

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
    } else {
        gui_mode_active = true;
        return run_gui_mode();
    }
}
