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

#define IS_GUI // Tells 3ds_bridge.h to let this file write directly to the screens

// System Headers
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// Stockfish Engine Headers
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

// Constants
#define MAX_HISTORY 2048
#define ENGINE_STACK_SIZE (32 * 1024)

// Handshake state machine definitions
typedef enum {
    ENGINE_STATE_BOOTING,
    ENGINE_STATE_WAIT_UCIOK,
    ENGINE_STATE_WAIT_READYOK,
    ENGINE_STATE_READY,
} EngineInitState;

EngineInitState engine_state = ENGINE_STATE_BOOTING;

typedef struct {
    int board[64];      // P=1, N=2, B=3, R=4, Q=5, K=6 (Positive=White, Negative=Black)
    int turn;           // 1 = White, -1 = Black
    int castle;         // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep;             // En-passant square (0-63), -1 if none
    int halfmoves;      // For 50-move rule
    int fullmoves;
    int white_king_sq;  // OPT: Cached white king square (avoids linear search)
    int black_king_sq;  // OPT: Cached black king square (avoids linear search)
} GuiBoardState;

typedef struct {
    int from;
    int to;
    int promo; // 0=None, 2=N, 3=B, 4=R, 5=Q
} GuiMove;

GuiBoardState current_state;
GuiBoardState history[MAX_HISTORY];
GuiMove move_history[MAX_HISTORY];
int history_count = 0;

int cursor_r = 6;
int cursor_c = 4;
int selected_sq = -1;

int board_orientation = 1;
int user_side = 1;

int time_control_type = 0;
int time_control_val = 1;

int engine_thinking = 0;
long long engine_nps = 0;
int engine_score_type = -1;
int engine_score_val = 0;

// Global Optimization Registers
int ui_dirty = 1;
int is_cached = 0;
int cached_king_in_check = -1;
int cached_has_legal_moves = 1;
int cached_is_check = 0;
int cached_repetition_count = 1;    // OPT: Cached repetition count

// OPT: Incremental position command string
static char position_cmd[8192]  = "position startpos";
static int  position_cmd_len    = 17;
static int  position_cmd_move_count = 0;

PrintConsole topConsole, bottomConsole;

// Forward Declarations
void gui_init_board(GuiBoardState *state);
int gui_is_legal_move(const GuiBoardState *state, GuiMove m);
int gui_has_legal_moves(const GuiBoardState *state);
int gui_is_square_attacked(const GuiBoardState *state, int sq, int attacker);
void gui_make_move(const GuiBoardState *src, GuiBoardState *dst, GuiMove m);
void trigger_engine_move(void);
int gui_find_king(const GuiBoardState *state, int color);
int gui_count_repetitions(const GuiBoardState *state);
int get_promo_choice(void);
GuiMove gui_uci_to_move(const char *str);
void reset_position_cmd(void);
void append_move_to_position_cmd(GuiMove m);

// ============================================================================
// Stockfish Engine Thread Entry Point
// ============================================================================
int main_stockfish(int argc, char **argv)
{
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
}

void stockfish_thread_func(void *arg) {
    char *argv[] = {"stockfish", NULL};
    main_stockfish(1, argv);
}

// ============================================================================
// GUI / Board Geometry and Math
// ============================================================================
int screen_to_board_sq(int r, int c) {
    if (board_orientation == 1) {
        return r * 8 + c;
    } else {
        return (7 - r) * 8 + (7 - c);
    }
}

void move_to_uci(GuiMove m, char *buf) {
    int f_col = m.from % 8;
    int f_row = 8 - (m.from / 8);
    int t_col = m.to % 8;
    int t_row = 8 - (m.to / 8);
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
    m.from = f_row * 8 + f_col;
    m.to   = t_row * 8 + t_col;
    if (strlen(str) == 5) {
        char p = str[4];
        if      (p == 'n') m.promo = 2;
        else if (p == 'b') m.promo = 3;
        else if (p == 'r') m.promo = 4;
        else               m.promo = 5;
    }
    return m;
}

// ============================================================================
// OPT: Incremental Position Command Helpers
// ============================================================================

// Resets the position command to "position startpos" (call on new game / full rebuild)
void reset_position_cmd(void) {
    strcpy(position_cmd, "position startpos");
    position_cmd_len        = 17;
    position_cmd_move_count = 0;
}

// Appends one move to the incremental position command string
void append_move_to_position_cmd(GuiMove m) {
    char uci_m[10];
    move_to_uci(m, uci_m);
    int move_len = (int)strlen(uci_m);

    // First move ever: insert the " moves" keyword
    if (position_cmd_move_count == 0) {
        if (position_cmd_len + 7 < (int)sizeof(position_cmd)) {
            strcpy(position_cmd + position_cmd_len, " moves");
            position_cmd_len += 6;
        }
    }

    // Append " <uci_move>"
    if (position_cmd_len + 1 + move_len + 1 < (int)sizeof(position_cmd)) {
        position_cmd[position_cmd_len++] = ' ';
        strcpy(position_cmd + position_cmd_len, uci_m);
        position_cmd_len += move_len;
        position_cmd[position_cmd_len] = '\0';
        position_cmd_move_count++;
    }
}

// Rebuilds the position command from scratch using the move history array.
// Called after an undo so the string stays consistent.
void rebuild_position_cmd(void) {
    reset_position_cmd();
    for (int i = 0; i < history_count; i++) {
        append_move_to_position_cmd(move_history[i]);
    }
}

// ============================================================================
// SAN / PGN Notation
// ============================================================================
void move_to_san(const GuiBoardState *state, GuiMove m, char *buf) {
    int p    = abs(state->board[m.from]);
    int turn = state->turn;

    // Castling checks
    if (p == 6) {
        if (m.from == 60 && m.to == 62 && turn ==  1) { strcpy(buf, "O-O");   goto append_suffixes; }
        if (m.from == 60 && m.to == 58 && turn ==  1) { strcpy(buf, "O-O-O"); goto append_suffixes; }
        if (m.from ==  4 && m.to ==  6 && turn == -1) { strcpy(buf, "O-O");   goto append_suffixes; }
        if (m.from ==  4 && m.to ==  2 && turn == -1) { strcpy(buf, "O-O-O"); goto append_suffixes; }
    }

    {
        char *ptr    = buf;
        int is_cap   = (state->board[m.to] != 0) || (p == 1 && m.to == state->ep);

        if (p == 1) {
            // Pawn moves
            if (is_cap) {
                *ptr++ = 'a' + (m.from % 8);
                *ptr++ = 'x';
            }
        } else {
            // Piece moves
            static const char p_chars[] = "?PNBRQK";
            *ptr++ = p_chars[p];

            // OPT: Only run disambiguation scan when another same-type piece exists
            int same_piece_count = 0;
            for (int sq = 0; sq < 64; sq++) {
                if (sq != m.from && state->board[sq] == state->board[m.from])
                    same_piece_count++;
            }

            if (same_piece_count > 0) {
                int another_can_move = 0;
                int same_file        = 0;
                int same_rank        = 0;
                for (int sq = 0; sq < 64; sq++) {
                    if (sq == m.from) continue;
                    if (state->board[sq] == state->board[m.from]) {
                        GuiMove alt_m = {sq, m.to, 0};
                        if (gui_is_legal_move(state, alt_m)) {
                            another_can_move = 1;
                            if (sq % 8 == m.from % 8) same_file = 1;
                            if (sq / 8 == m.from / 8) same_rank = 1;
                        }
                    }
                }
                if (another_can_move) {
                    if (!same_file) {
                        *ptr++ = 'a' + (m.from % 8);
                    } else if (!same_rank) {
                        *ptr++ = '8' - (m.from / 8);
                    } else {
                        *ptr++ = 'a' + (m.from % 8);
                        *ptr++ = '8' - (m.from / 8);
                    }
                }
            }

            if (is_cap) *ptr++ = 'x';
        }

        // Destination square
        *ptr++ = 'a' + (m.to % 8);
        *ptr++ = '8' - (m.to / 8);

        // Promotion suffix
        if (m.promo != 0) {
            static const char p_chars[] = "?PNBRQK";
            *ptr++ = '=';
            *ptr++ = p_chars[m.promo];
        }
        *ptr = '\0';
    }

append_suffixes:;
    // Check / checkmate suffixes
    GuiBoardState next;
    gui_make_move(state, &next, m);
    int op_king  = gui_find_king(&next, next.turn);
    int is_check = (op_king != -1) && gui_is_square_attacked(&next, op_king, -next.turn);
    int has_moves = gui_has_legal_moves(&next);

    char *ptr = buf + strlen(buf);
    if (is_check) {
        *ptr++ = has_moves ? '+' : '#';
    }
    *ptr = '\0';
}

void push_state(const GuiBoardState *state, GuiMove m) {
    if (history_count < MAX_HISTORY - 1) {
        history[history_count]      = *state;
        move_history[history_count] = m;
        history_count++;
    }
}

// ============================================================================
// Engine Communication & UCI Parsing
// ============================================================================
void trigger_engine_move(void) {
    engine_nps        = 0;
    engine_score_type = -1;
    engine_score_val  = 0;

    // OPT: Send the already-built incremental position string directly
    sf_send_command(position_cmd);

    char go_cmd[128];
    if (time_control_type == 0) {
        sprintf(go_cmd, "go movetime %d", time_control_val);
    } else if (time_control_type == 1) {
        sprintf(go_cmd, "go depth %d", time_control_val);
    } else {
        sprintf(go_cmd, "go nodes %d", time_control_val);
    }
    sf_send_command(go_cmd);
}

void process_engine_output(char *line) {
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
        line[--len] = '\0';
    }

    consoleSelect(&bottomConsole);
    printf("%s\n", line);
    fflush(stdout);

    if (engine_state == ENGINE_STATE_WAIT_UCIOK) {
        if (strstr(line, "uciok") != NULL) {
            sf_send_command("isready");
            engine_state = ENGINE_STATE_WAIT_READYOK;
            ui_dirty = 1;
            printf("[GUI] Received 'uciok' -> Sending 'isready'\n");
            fflush(stdout);
        }
    } else if (engine_state == ENGINE_STATE_WAIT_READYOK) {
        if (strstr(line, "readyok") != NULL) {
            sf_send_command("setoption name Hash value 13");
            sf_send_command("setoption name Ponder value false");
            sf_send_command("ucinewgame");
            engine_state = ENGINE_STATE_READY;
            ui_dirty = 1;
            printf("[GUI] Received 'readyok' -> Handshake Complete.\n");
            fflush(stdout);
        }
    }

    // OPT: Single forward pass to find both "nps" and "score" tokens simultaneously
    if (strncmp(line, "info", 4) == 0) {
        char *nps_ptr   = NULL;
        char *score_ptr = NULL;
        char *p         = line + 4;

        while (*p) {
            if (!nps_ptr && p[0] == ' ' && p[1] == 'n' && p[2] == 'p' && p[3] == 's' && p[4] == ' ')
                nps_ptr = p + 1;
            if (!score_ptr && p[0] == ' ' && p[1] == 's' && p[2] == 'c' && p[3] == 'o' &&
                              p[4] == 'r' && p[5] == 'e' && p[6] == ' ')
                score_ptr = p + 1;
            if (nps_ptr && score_ptr) break;
            p++;
        }

        if (nps_ptr) {
            engine_nps = strtoll(nps_ptr + 4, NULL, 10); // skip "nps "
            ui_dirty = 1;
        }

        if (score_ptr) {
            char *sv = score_ptr + 6; // skip "score "
            if (strncmp(sv, "cp ", 3) == 0) {
                engine_score_type = 0;
                engine_score_val  = atoi(sv + 3) * current_state.turn;
                ui_dirty = 1;
            } else if (strncmp(sv, "mate ", 5) == 0) {
                engine_score_type = 1;
                engine_score_val  = atoi(sv + 5) * current_state.turn;
                ui_dirty = 1;
            }
        }
    }

    if (strncmp(line, "bestmove", 8) == 0) {
        char move_str[16];
        if (sscanf(line, "bestmove %15s", move_str) == 1) {
            if (strcmp(move_str, "(none)") == 0 || strcmp(move_str, "NULL") == 0) {
                engine_thinking = 0;
                ui_dirty = 1;
                return;
            }

            GuiMove m = gui_uci_to_move(move_str);
            if (gui_is_legal_move(&current_state, m)) {
                // OPT: Append to incremental position string before push_state
                append_move_to_position_cmd(m);
                push_state(&current_state, m);
                GuiBoardState next;
                gui_make_move(&current_state, &next, m);
                current_state = next;
                is_cached = 0;
            }
            engine_thinking = 0;
            ui_dirty = 1;
        }
    }
}

void read_from_engine(void) {
    char tmp[512];
    static char line_buf[1024];
    static int  line_len = 0;
    int bytes_read;
    int chunks_processed = 0;

    while (chunks_processed < 32 && (bytes_read = sf_get_output(tmp, sizeof(tmp) - 1)) > 0) {
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

// ============================================================================
// Chess Rules Engine / Move Legality (GUI side)
// ============================================================================

// OPT: O(1) king lookup via cached squares in GuiBoardState
int gui_find_king(const GuiBoardState *state, int color) {
    return (color == 1) ? state->white_king_sq : state->black_king_sq;
}

int gui_is_square_attacked(const GuiBoardState *state, int sq, int attacker) {
    int r = sq / 8, c = sq % 8;

    // Knight attacks
    static const int k_r[] = {-2, -2, -1, -1,  1,  1,  2,  2};
    static const int k_c[] = {-1,  1, -2,  2, -2,  2, -1,  1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_r[i], nc = c + k_c[i];
        if ((unsigned)nr < 8 && (unsigned)nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 2) return 1;
        }
    }

    // King attacks
    static const int kg_r[] = {-1, -1, -1,  0,  0,  1,  1,  1};
    static const int kg_c[] = {-1,  0,  1, -1,  1, -1,  0,  1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kg_r[i], nc = c + kg_c[i];
        if ((unsigned)nr < 8 && (unsigned)nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 6) return 1;
        }
    }

    // Pawn attacks
    int p_offset = (attacker == 1) ? 1 : -1;
    for (int dc = -1; dc <= 1; dc += 2) {
        int nr = r + p_offset, nc = c + dc;
        if ((unsigned)nr < 8 && (unsigned)nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 1) return 1;
        }
    }

    // Diagonal rays (bishop / queen)
    static const int d_r[] = {-1, -1,  1,  1};
    static const int d_c[] = {-1,  1, -1,  1};
    for (int i = 0; i < 4; i++) {
        int nr = r + d_r[i], nc = c + d_c[i];
        while ((unsigned)nr < 8 && (unsigned)nc < 8) {
            int target = state->board[nr * 8 + nc];
            if (target != 0) {
                if (target == attacker * 3 || target == attacker * 5) return 1;
                break;
            }
            nr += d_r[i]; nc += d_c[i];
        }
    }

    // Straight rays (rook / queen)
    static const int s_r[] = {-1,  1,  0,  0};
    static const int s_c[] = { 0,  0, -1,  1};
    for (int i = 0; i < 4; i++) {
        int nr = r + s_r[i], nc = c + s_c[i];
        while ((unsigned)nr < 8 && (unsigned)nc < 8) {
            int target = state->board[nr * 8 + nc];
            if (target != 0) {
                if (target == attacker * 4 || target == attacker * 5) return 1;
                break;
            }
            nr += s_r[i]; nc += s_c[i];
        }
    }
    return 0;
}

int gui_is_pseudo_legal_move(const GuiBoardState *state, GuiMove m) {
    int p      = state->board[m.from];
    int target = state->board[m.to];
    int turn   = state->turn;

    if (p == 0)    return 0;
    if (turn ==  1 && p < 0) return 0;
    if (turn == -1 && p > 0) return 0;
    if (m.from == m.to) return 0;
    if (target != 0 && ((turn == 1 && target > 0) || (turn == -1 && target < 0))) return 0;

    int fr = m.from / 8, fc = m.from % 8;
    int tr = m.to   / 8, tc = m.to   % 8;
    int dr = tr - fr,    dc = tc - fc;
    int abs_dr = abs(dr), abs_dc = abs(dc);

    switch (abs(p)) {
        case 1: {
            int dir     = (turn == 1) ? -1 : 1;
            int start_r = (turn == 1) ?  6 :  1;
            if (dc == 0 && dr == dir && target == 0) return 1;
            if (dc == 0 && fr == start_r && dr == 2 * dir) {
                if (state->board[(fr + dir) * 8 + fc] == 0 && target == 0) return 1;
            }
            if (abs_dc == 1 && dr == dir) {
                if (target != 0)       return 1;
                if (m.to == state->ep) return 1;
            }
            return 0;
        }
        case 2:
            return (abs_dr == 2 && abs_dc == 1) || (abs_dr == 1 && abs_dc == 2);
        case 3: {
            if (abs_dr != abs_dc) return 0;
            int sr = (dr > 0) ? 1 : -1, sc = (dc > 0) ? 1 : -1;
            int r = fr + sr, c = fc + sc;
            while (r != tr) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 4: {
            if (dr != 0 && dc != 0) return 0;
            int sr = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
            int sc = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
            int r = fr + sr, c = fc + sc;
            while (r != tr || c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 5: {
            if (abs_dr != abs_dc && dr != 0 && dc != 0) return 0;
            int sr, sc;
            if (abs_dr == abs_dc) {
                sr = (dr > 0) ? 1 : -1;
                sc = (dc > 0) ? 1 : -1;
            } else {
                sr = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
                sc = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
            }
            int r = fr + sr, c = fc + sc;
            while (r != tr || c != tc) {
                if (state->board[r * 8 + c] != 0) return 0;
                r += sr; c += sc;
            }
            return 1;
        }
        case 6: {
            if (abs_dr <= 1 && abs_dc <= 1) return 1;
            if (dr == 0 && abs_dc == 2) {
                if (turn == 1 && fr == 7 && fc == 4) {
                    if (m.to == 62 && (state->castle & 1)) {
                        if (state->board[61] == 0 && state->board[62] == 0 &&
                            !gui_is_square_attacked(state, 60, -1) &&
                            !gui_is_square_attacked(state, 61, -1) &&
                            !gui_is_square_attacked(state, 62, -1)) return 1;
                    }
                    if (m.to == 58 && (state->castle & 2)) {
                        if (state->board[59] == 0 && state->board[58] == 0 && state->board[57] == 0 &&
                            !gui_is_square_attacked(state, 60, -1) &&
                            !gui_is_square_attacked(state, 59, -1) &&
                            !gui_is_square_attacked(state, 58, -1)) return 1;
                    }
                }
                if (turn == -1 && fr == 0 && fc == 4) {
                    if (m.to == 6 && (state->castle & 4)) {
                        if (state->board[5] == 0 && state->board[6] == 0 &&
                            !gui_is_square_attacked(state, 4, 1) &&
                            !gui_is_square_attacked(state, 5, 1) &&
                            !gui_is_square_attacked(state, 6, 1)) return 1;
                    }
                    if (m.to == 2 && (state->castle & 8)) {
                        if (state->board[3] == 0 && state->board[2] == 0 && state->board[1] == 0 &&
                            !gui_is_square_attacked(state, 4, 1) &&
                            !gui_is_square_attacked(state, 3, 1) &&
                            !gui_is_square_attacked(state, 2, 1)) return 1;
                    }
                }
            }
            return 0;
        }
    }
    return 0;
}

int gui_is_legal_move(const GuiBoardState *state, GuiMove m) {
    if (!gui_is_pseudo_legal_move(state, m)) return 0;
    GuiBoardState next;
    gui_make_move(state, &next, m);
    int king = gui_find_king(&next, state->turn);
    if (king == -1) return 0;
    return !gui_is_square_attacked(&next, king, -state->turn);
}

// OPT: Targeted move generation - only visits geometrically reachable squares
// per piece type instead of blindly testing all 64 destinations.
// Reduces gui_is_legal_move calls by ~20-40x in typical positions.
int gui_has_legal_moves(const GuiBoardState *state) {
    static const int kn_dr[] = {-2, -2, -1, -1,  1,  1,  2,  2};
    static const int kn_dc[] = {-1,  1, -2,  2, -2,  2, -1,  1};
    static const int diag_dr[] = {-1, -1,  1,  1};
    static const int diag_dc[] = {-1,  1, -1,  1};
    static const int str_dr[]  = {-1,  1,  0,  0};
    static const int str_dc[]  = { 0,  0, -1,  1};
    static const int kg_dr[]   = {-1, -1, -1,  0,  0,  1,  1,  1};
    static const int kg_dc[]   = {-1,  0,  1, -1,  1, -1,  0,  1};

    for (int f = 0; f < 64; f++) {
        int p = state->board[f];
        if (p == 0) continue;
        if (state->turn ==  1 && p < 0) continue;
        if (state->turn == -1 && p > 0) continue;

        int fr = f / 8, fc = f % 8;
        int abs_p = abs(p);

        switch (abs_p) {
            case 1: { // Pawn - at most 4 candidate squares
                int dir     = (state->turn == 1) ? -1 : 1;
                int start_r = (state->turn == 1) ?  6 :  1;
                int targets[4];
                int tc = 0;

                int fwd = f + dir * 8;
                if ((unsigned)fwd < 64) targets[tc++] = fwd;

                if (fr == start_r) {
                    int fwd2 = f + dir * 16;
                    if ((unsigned)fwd2 < 64) targets[tc++] = fwd2;
                }
                if (fc > 0) targets[tc++] = f + dir * 8 - 1;
                if (fc < 7) targets[tc++] = f + dir * 8 + 1;

                for (int i = 0; i < tc; i++) {
                    int t = targets[i];
                    if (t < 0 || t >= 64) continue;
                    GuiMove m = {f, t, 0};
                    if (t / 8 == 0 || t / 8 == 7) m.promo = 5;
                    if (gui_is_legal_move(state, m)) return 1;
                }
                break;
            }
            case 2: { // Knight - at most 8 squares
                for (int i = 0; i < 8; i++) {
                    int nr = fr + kn_dr[i], nc = fc + kn_dc[i];
                    if ((unsigned)nr >= 8 || (unsigned)nc >= 8) continue;
                    GuiMove m = {f, nr * 8 + nc, 0};
                    if (gui_is_legal_move(state, m)) return 1;
                }
                break;
            }
            case 3: { // Bishop - diagonal rays, stop on first blocker
                for (int d = 0; d < 4; d++) {
                    int nr = fr + diag_dr[d], nc = fc + diag_dc[d];
                    while ((unsigned)nr < 8 && (unsigned)nc < 8) {
                        GuiMove m = {f, nr * 8 + nc, 0};
                        int blocked = (state->board[nr * 8 + nc] != 0);
                        if (gui_is_legal_move(state, m)) return 1;
                        if (blocked) break;
                        nr += diag_dr[d]; nc += diag_dc[d];
                    }
                }
                break;
            }
            case 4: { // Rook - straight rays, stop on first blocker
                for (int d = 0; d < 4; d++) {
                    int nr = fr + str_dr[d], nc = fc + str_dc[d];
                    while ((unsigned)nr < 8 && (unsigned)nc < 8) {
                        GuiMove m = {f, nr * 8 + nc, 0};
                        int blocked = (state->board[nr * 8 + nc] != 0);
                        if (gui_is_legal_move(state, m)) return 1;
                        if (blocked) break;
                        nr += str_dr[d]; nc += str_dc[d];
                    }
                }
                break;
            }
            case 5: { // Queen - all 8 directions as rays
                for (int d = 0; d < 4; d++) {
                    // Diagonal
                    int nr = fr + diag_dr[d], nc = fc + diag_dc[d];
                    while ((unsigned)nr < 8 && (unsigned)nc < 8) {
                        GuiMove m = {f, nr * 8 + nc, 0};
                        int blocked = (state->board[nr * 8 + nc] != 0);
                        if (gui_is_legal_move(state, m)) return 1;
                        if (blocked) break;
                        nr += diag_dr[d]; nc += diag_dc[d];
                    }
                    // Straight
                    nr = fr + str_dr[d]; nc = fc + str_dc[d];
                    while ((unsigned)nr < 8 && (unsigned)nc < 8) {
                        GuiMove m = {f, nr * 8 + nc, 0};
                        int blocked = (state->board[nr * 8 + nc] != 0);
                        if (gui_is_legal_move(state, m)) return 1;
                        if (blocked) break;
                        nr += str_dr[d]; nc += str_dc[d];
                    }
                }
                break;
            }
            case 6: { // King - adjacent squares + castling candidates
                for (int i = 0; i < 8; i++) {
                    int nr = fr + kg_dr[i], nc = fc + kg_dc[i];
                    if ((unsigned)nr >= 8 || (unsigned)nc >= 8) continue;
                    GuiMove m = {f, nr * 8 + nc, 0};
                    if (gui_is_legal_move(state, m)) return 1;
                }
                // Castling
                if (state->turn == 1 && f == 60) {
                    GuiMove m1 = {60, 62, 0};
                    GuiMove m2 = {60, 58, 0};
                    if (gui_is_legal_move(state, m1)) return 1;
                    if (gui_is_legal_move(state, m2)) return 1;
                } else if (state->turn == -1 && f == 4) {
                    GuiMove m1 = {4, 6, 0};
                    GuiMove m2 = {4, 2, 0};
                    if (gui_is_legal_move(state, m1)) return 1;
                    if (gui_is_legal_move(state, m2)) return 1;
                }
                break;
            }
        }
    }
    return 0;
}

int gui_count_repetitions(const GuiBoardState *state) {
    int count = 1;
    for (int i = 0; i < history_count; i++) {
        if (state->turn    != history[i].turn    ||
            state->castle  != history[i].castle  ||
            state->ep      != history[i].ep) continue;
        if (memcmp(state->board, history[i].board, sizeof(state->board)) == 0)
            count++;
    }
    return count;
}

void gui_make_move(const GuiBoardState *src, GuiBoardState *dst, GuiMove m) {
    *dst = *src;
    int p          = dst->board[m.from];
    int is_capture = (src->board[m.to] != 0) || (abs(p) == 1 && m.to == src->ep);

    dst->board[m.from] = 0;
    dst->board[m.to]   = (m.promo != 0) ? (dst->turn * m.promo) : p;

    // En passant capture
    if (abs(p) == 1 && m.to == dst->ep) {
        int p_dir = (dst->turn == 1) ? 8 : -8;
        dst->board[m.to + p_dir] = 0;
    }

    // En passant square update
    dst->ep = -1;
    if (abs(p) == 1 && abs(m.from - m.to) == 16) {
        dst->ep = m.from + ((dst->turn == 1) ? -8 : 8);
    }

    // Castling rook moves + OPT: update cached king square
    if (abs(p) == 6) {
        // OPT: Update king square cache
        if (dst->turn == 1) dst->white_king_sq = m.to;
        else                dst->black_king_sq = m.to;

        if      (m.from == 60 && m.to == 62) { dst->board[61] = dst->board[63]; dst->board[63] = 0; }
        else if (m.from == 60 && m.to == 58) { dst->board[59] = dst->board[56]; dst->board[56] = 0; }
        else if (m.from ==  4 && m.to ==  6) { dst->board[5]  = dst->board[7];  dst->board[7]  = 0; }
        else if (m.from ==  4 && m.to ==  2) { dst->board[3]  = dst->board[0];  dst->board[0]  = 0; }
    }

    // Castling rights
    if (m.from == 60) dst->castle &= ~3;
    if (m.from ==  4) dst->castle &= ~12;
    if (m.from == 63 || m.to == 63) dst->castle &= ~1;
    if (m.from == 56 || m.to == 56) dst->castle &= ~2;
    if (m.from ==  7 || m.to ==  7) dst->castle &= ~4;
    if (m.from ==  0 || m.to ==  0) dst->castle &= ~8;

    dst->turn = -dst->turn;
    if (abs(p) == 1 || is_capture) dst->halfmoves = 0;
    else dst->halfmoves++;
    if (dst->turn == 1) dst->fullmoves++;
}

// ============================================================================
// UI Rendering
// ============================================================================
void draw_ui(void) {
    consoleSelect(&topConsole);
    printf("\x1b[1;1H");
    printf("\x1b[K\n");

    char rp[26][64];
    for (int i = 0; i < 26; i++) {
        sprintf(rp[i], "                  ");
    }

    // OPT: Lazy evaluation - recompute safety state only when cache is invalidated
    if (!is_cached) {
        int king = gui_find_king(&current_state, current_state.turn);
        cached_is_check       = (king != -1) && gui_is_square_attacked(&current_state, king, -current_state.turn);
        cached_has_legal_moves = gui_has_legal_moves(&current_state);

        int w_king = gui_find_king(&current_state, 1);
        int b_king = gui_find_king(&current_state, -1);
        cached_king_in_check = -1;
        if (w_king != -1 && gui_is_square_attacked(&current_state, w_king, -1))
            cached_king_in_check = w_king;
        else if (b_king != -1 && gui_is_square_attacked(&current_state, b_king, 1))
            cached_king_in_check = b_king;

        // OPT: Cache repetition count alongside other safety state
        cached_repetition_count = gui_count_repetitions(&current_state);

        is_cached = 1;
    }

    const char *w_play   = (user_side == 1 || user_side == 0) ? "Hum" : "Eng";
    const char *b_play   = (user_side == -1 || user_side == 0) ? "Hum" : "Eng";

    // Status line using cached repetition count
    if (current_state.halfmoves >= 100) {
        strcpy(rp[0], "  [DRAW (50m-rule)]");
    } else if (cached_repetition_count >= 3) {
        strcpy(rp[0], "  [DRAW (3-fold)]");
    } else if (!cached_has_legal_moves) {
        if (cached_is_check) {
            strcpy(rp[0], "  \x1b[1;31m[CHECKMATE!]\x1b[0m");
        } else {
            strcpy(rp[0], "  \x1b[1;36m[STALEMATE!]\x1b[0m");
        }
    } else if (cached_is_check) {
        if (current_state.turn == 1) {
            sprintf(rp[0], "  \x1b[1;32mWhite\x1b[0m (\x1b[1;31mCHECK!\x1b[0m)");
        } else {
            sprintf(rp[0], "  \x1b[1;35mBlack\x1b[0m (\x1b[1;31mCHECK!\x1b[0m)");
        }
    } else {
        if (current_state.turn == 1) {
            sprintf(rp[0], "  \x1b[1;32mWhite\x1b[0m to play");
        } else {
            sprintf(rp[0], "  \x1b[1;35mBlack\x1b[0m to play");
        }
    }

    sprintf(rp[1], "  W: %s | B: %s", w_play, b_play);
    if (time_control_type == 0) {
        sprintf(rp[2], "  Lim: %d ms", time_control_val);
    } else if (time_control_type == 1) {
        sprintf(rp[2], "  Lim: depth %d", time_control_val);
    } else {
        sprintf(rp[2], "  Lim: %d nodes", time_control_val);
    }

    if (engine_state == ENGINE_STATE_READY) {
        if (engine_score_type == 0) {
            double eval = (double)engine_score_val / 100.0;
            if      (eval > 0.0) sprintf(rp[3], "  Eval: \x1b[1;32m%+.2f\x1b[0m", eval);
            else if (eval < 0.0) sprintf(rp[3], "  Eval: \x1b[1;31m%+.2f\x1b[0m", eval);
            else                 sprintf(rp[3], "  Eval: 0.00");
        } else if (engine_score_type == 1) {
            if      (engine_score_val > 0) sprintf(rp[3], "  Eval: \x1b[1;32m+M%d\x1b[0m",  engine_score_val);
            else if (engine_score_val < 0) sprintf(rp[3], "  Eval: \x1b[1;31m-M%d\x1b[0m", -engine_score_val);
            else                           sprintf(rp[3], "  Eval: M0");
        } else {
            strcpy(rp[3], "  Eval: ----");
        }

        if (engine_nps > 0) {
            if      (engine_nps >= 1000000) sprintf(rp[4], "  NPS:  %.2fM", (double)engine_nps / 1000000.0);
            else if (engine_nps >= 1000)    sprintf(rp[4], "  NPS:  %.1fk", (double)engine_nps / 1000.0);
            else                            sprintf(rp[4], "  NPS:  %lld",  engine_nps);
        } else {
            strcpy(rp[4], "  NPS:  ----");
        }
    } else {
        strcpy(rp[3], "  Eval: Config");
        strcpy(rp[4], "  NPS:  Config");
    }

    strcpy(rp[5], "\x1b[1;33m  RECENT MOVES:\x1b[0m");

    int total_full_moves = (history_count + 1) / 2;
    int start_move       = (total_full_moves > 20) ? (total_full_moves - 19) : 1;
    for (int idx = 0; idx < 20; idx++) {
        int display = start_move + idx;
        if (total_full_moves > 0 && display <= total_full_moves) {
            int w_idx = (display - 1) * 2;
            int b_idx = w_idx + 1;
            char w_str[10] = "-----";
            char b_str[10] = "-----";
            if (w_idx < history_count) move_to_san(&history[w_idx], move_history[w_idx], w_str);
            if (b_idx < history_count) move_to_san(&history[b_idx], move_history[b_idx], b_str);
            else if (w_idx < history_count) strcpy(b_str, "...");
            sprintf(rp[6 + idx], "  %2d. %-5s %-5s", display, w_str, b_str);
        } else {
            sprintf(rp[6 + idx], "  %2d.  ---   --- ", display);
        }
    }

    // -------------------------------------------------------------------------
    // OPT: Precompute legal destination bitmask once per frame for selected piece
    // Eliminates 64 gui_is_legal_move calls inside the render loop
    // -------------------------------------------------------------------------
    uint64_t legal_dest_mask = 0ULL;
    if (selected_sq != -1) {
        int f     = selected_sq;
        int abs_p = abs(current_state.board[f]);
        int fr    = f / 8, fc = f % 8;

        static const int kn_dr2[] = {-2,-2,-1,-1, 1, 1, 2, 2};
        static const int kn_dc2[] = {-1, 1,-2, 2,-2, 2,-1, 1};
        static const int diag_dr2[]={-1,-1, 1, 1};
        static const int diag_dc2[]={-1, 1,-1, 1};
        static const int str_dr2[] ={-1, 1, 0, 0};
        static const int str_dc2[] ={ 0, 0,-1, 1};
        static const int kg_dr2[]  ={-1,-1,-1, 0, 0, 1, 1, 1};
        static const int kg_dc2[]  ={-1, 0, 1,-1, 1,-1, 0, 1};

        switch (abs_p) {
            case 1: {
                int dir     = (current_state.turn == 1) ? -1 : 1;
                int start_r = (current_state.turn == 1) ?  6 :  1;
                int ts[4]; int tc2 = 0;
                int fwd = f + dir * 8;
                if ((unsigned)fwd < 64) ts[tc2++] = fwd;
                if (fr == start_r) { int fwd2 = f + dir * 16; if ((unsigned)fwd2 < 64) ts[tc2++] = fwd2; }
                if (fc > 0) ts[tc2++] = f + dir * 8 - 1;
                if (fc < 7) ts[tc2++] = f + dir * 8 + 1;
                for (int i = 0; i < tc2; i++) {
                    int t = ts[i];
                    if (t < 0 || t >= 64) continue;
                    GuiMove m = {f, t, 0};
                    if (t / 8 == 0 || t / 8 == 7) m.promo = 5;
                    if (gui_is_legal_move(&current_state, m)) legal_dest_mask |= (1ULL << t);
                }
                break;
            }
            case 2: {
                for (int i = 0; i < 8; i++) {
                    int nr = fr + kn_dr2[i], nc = fc + kn_dc2[i];
                    if ((unsigned)nr >= 8 || (unsigned)nc >= 8) continue;
                    int t = nr * 8 + nc;
                    GuiMove m = {f, t, 0};
                    if (gui_is_legal_move(&current_state, m)) legal_dest_mask |= (1ULL << t);
                }
                break;
            }
            case 3: {
                for (int d = 0; d < 4; d++) {
                    int nr = fr + diag_dr2[d], nc = fc + diag_dc2[d];
                    while ((unsigned)nr < 8 && (unsigned)nc < 8) {
                        int t = nr * 8 + nc;
                        GuiMove m = {f, t, 0};
                        int blocked = (current_state.board[t] != 0);
                        if (gui_is_legal_move(&current_state, m)) legal_dest_mask |= (1ULL << t);
                        if (blocked) break;
                        nr += diag_dr2[d]; nc += diag_dc2[d];
                    }
                }
                break;
            }
            case 4: {
                for (int d = 0; d < 4; d++) {
                    int nr = fr + str_dr2[d], nc = fc + str_dc2[d];
                    while ((unsigned)nr < 8 && (unsigned)nc < 8) {
                        int t = nr * 8 + nc;
                        GuiMove m = {f, t, 0};
                        int blocked = (current_state.board[t] != 0);
                        if (gui_is_legal_move(&current_state, m)) legal_dest_mask |= (1ULL << t);
                        if (blocked) break;
                        nr += str_dr2[d]; nc += str_dc2[d];
                    }
                }
                break;
            }
            case 5: {
                for (int d = 0; d < 4; d++) {
                    int nr = fr + diag_dr2[d], nc = fc + diag_dc2[d];
                    while ((unsigned)nr < 8 && (unsigned)nc < 8) {
                        int t = nr * 8 + nc;
                        GuiMove m = {f, t, 0};
                        int blocked = (current_state.board[t] != 0);
                        if (gui_is_legal_move(&current_state, m)) legal_dest_mask |= (1ULL << t);
                        if (blocked) break;
                        nr += diag_dr2[d]; nc += diag_dc2[d];
                    }
                    nr = fr + str_dr2[d]; nc = fc + str_dc2[d];
                    while ((unsigned)nr < 8 && (unsigned)nc < 8) {
                        int t = nr * 8 + nc;
                        GuiMove m = {f, t, 0};
                        int blocked = (current_state.board[t] != 0);
                        if (gui_is_legal_move(&current_state, m)) legal_dest_mask |= (1ULL << t);
                        if (blocked) break;
                        nr += str_dr2[d]; nc += str_dc2[d];
                    }
                }
                break;
            }
            case 6: {
                for (int i = 0; i < 8; i++) {
                    int nr = fr + kg_dr2[i], nc = fc + kg_dc2[i];
                    if ((unsigned)nr >= 8 || (unsigned)nc >= 8) continue;
                    int t = nr * 8 + nc;
                    GuiMove m = {f, t, 0};
                    if (gui_is_legal_move(&current_state, m)) legal_dest_mask |= (1ULL << t);
                }
                // Castling
                if (current_state.turn == 1 && f == 60) {
                    GuiMove m1 = {60,62,0}; if (gui_is_legal_move(&current_state,m1)) legal_dest_mask |= (1ULL<<62);
                    GuiMove m2 = {60,58,0}; if (gui_is_legal_move(&current_state,m2)) legal_dest_mask |= (1ULL<<58);
                } else if (current_state.turn == -1 && f == 4) {
                    GuiMove m1 = {4,6,0}; if (gui_is_legal_move(&current_state,m1)) legal_dest_mask |= (1ULL<<6);
                    GuiMove m2 = {4,2,0}; if (gui_is_legal_move(&current_state,m2)) legal_dest_mask |= (1ULL<<2);
                }
                break;
            }
        }
    }

    // Rank labels header
    if (board_orientation == 1) printf("     a  b  c  d  e  f  g  h   ");
    else                        printf("     h  g  f  e  d  c  b  a   ");
    printf("%s\x1b[K\n", rp[0]);

    // Board ranks
    for (int r = 0; r < 8; r++) {
        int rank_lbl = (board_orientation == 1) ? (8 - r) : (r + 1);

        for (int sub_r = 0; sub_r < 3; sub_r++) {
            if (sub_r == 1) printf("  %d ", rank_lbl);
            else            printf("    ");

            for (int c = 0; c < 8; c++) {
                int sq = screen_to_board_sq(r, c);
                int p  = current_state.board[sq];

                int is_light    = ((sq / 8) + (sq % 8)) % 2 == 0;
                int is_selected = (sq == selected_sq);
                int is_cursor   = (r == cursor_r && c == cursor_c);

                int is_prev_move = 0;
                if (history_count > 0) {
                    GuiMove last = move_history[history_count - 1];
                    if (sq == last.from || sq == last.to) is_prev_move = 1;
                }

                // OPT: O(1) bitmask lookup instead of gui_is_legal_move call
                int is_legal_dest = (selected_sq != -1) && ((legal_dest_mask >> sq) & 1);

                const char *bg_color;
                if (is_cursor) {
                    bg_color = "\x1b[48;5;208m";
                } else if (is_selected) {
                    bg_color = "\x1b[48;5;34m";
                } else if (sq == cached_king_in_check) {
                    bg_color = "\x1b[48;5;196m";
                } else if (is_prev_move) {
                    bg_color = is_light ? "\x1b[48;5;75m" : "\x1b[48;5;68m";
                } else if (is_legal_dest) {
                    bg_color = is_light ? "\x1b[48;5;151m" : "\x1b[48;5;108m";
                } else {
                    bg_color = is_light ? "\x1b[48;5;180m" : "\x1b[48;5;94m";
                }

                if (sub_r == 1) {
                    const char *piece_str = " ";
                    const char *fg_color  = "\x1b[38;5;232m";
                    if (p != 0) {
                        if (p > 0) fg_color = "\x1b[38;5;255m\x1b[1m";
                        switch (abs(p)) {
                            case 1: piece_str = "P"; break;
                            case 2: piece_str = "N"; break;
                            case 3: piece_str = "B"; break;
                            case 4: piece_str = "R"; break;
                            case 5: piece_str = "Q"; break;
                            case 6: piece_str = "K"; break;
                        }
                    }
                    printf("%s%s %s \x1b[0m", bg_color, fg_color, piece_str);
                } else {
                    printf("%s   \x1b[0m", bg_color);
                }
            }

            if (sub_r == 1) {
                printf(" %d", rank_lbl);
                printf("%s", rp[1 + (r * 3) + sub_r]);
            } else {
                printf("  ");
                printf("%s", rp[1 + (r * 3) + sub_r]);
            }
            printf("\x1b[K\n");
        }
    }

    // Bottom rank labels
    if (board_orientation == 1) printf("     a  b  c  d  e  f  g  h   ");
    else                        printf("     h  g  f  e  d  c  b  a   ");
    printf("%s\x1b[K\n\n", rp[25]);

    fflush(stdout);
    consoleSelect(&bottomConsole);
}

// ============================================================================
// Input & State Controllers
// ============================================================================
int get_promo_choice(void) {
    consoleSelect(&bottomConsole);
    printf("\n\x1b[1;33mPROMOTION! Tap Screen / Key selection:\n");
    printf(" [Y] Queen  [X] Rook\n [B] Bishop [A] Knight\x1b[0m\n");

    int choice = 5;
    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();
        if (kDown & KEY_Y) { choice = 5; break; }
        if (kDown & KEY_X) { choice = 4; break; }
        if (kDown & KEY_B) { choice = 3; break; }
        if (kDown & KEY_A) { choice = 2; break; }
        gspWaitForVBlank();
    }
    return choice;
}

void handle_select(void) {
    if (engine_thinking) return;

    int is_engine_turn = 0;
    if (user_side == 2) is_engine_turn = 1;
    else if (user_side ==  1 && current_state.turn == -1) is_engine_turn = 1;
    else if (user_side == -1 && current_state.turn ==  1) is_engine_turn = 1;
    if (is_engine_turn) return;

    // OPT: Use cached repetition count instead of recomputing
    if (!cached_has_legal_moves || current_state.halfmoves >= 100 || cached_repetition_count >= 3) return;

    int sq = screen_to_board_sq(cursor_r, cursor_c);
    if (selected_sq == -1) {
        int p = current_state.board[sq];
        if (p != 0 && ((current_state.turn == 1 && p > 0) || (current_state.turn == -1 && p < 0))) {
            selected_sq = sq;
            ui_dirty = 1;
        }
    } else {
        GuiMove m  = {selected_sq, sq, 0};
        int p      = current_state.board[selected_sq];
        int is_promo = (abs(p) == 1 && (sq / 8 == 0 || sq / 8 == 7));
        if (is_promo) m.promo = 5;

        if (gui_is_legal_move(&current_state, m)) {
            if (is_promo) m.promo = get_promo_choice();

            // OPT: Append to incremental position string
            append_move_to_position_cmd(m);
            push_state(&current_state, m);
            GuiBoardState next;
            gui_make_move(&current_state, &next, m);
            current_state = next;
            selected_sq   = -1;

            engine_nps        = 0;
            engine_score_type = -1;
            engine_score_val  = 0;
            is_cached = 0;
        } else {
            int target = current_state.board[sq];
            if (target != 0 && ((current_state.turn == 1 && target > 0) || (current_state.turn == -1 && target < 0))) {
                selected_sq = sq;
            } else {
                selected_sq = -1;
            }
        }
        ui_dirty = 1;
    }
}

void handle_undo(void) {
    if (engine_thinking) {
        sf_send_command("stop");
        engine_thinking = 0;
    }
    engine_nps        = 0;
    engine_score_type = -1;
    engine_score_val  = 0;

    int step_back = (user_side == 1 || user_side == -1) ? 2 : 1;
    while (step_back > 0 && history_count > 0) {
        history_count--;
        current_state = history[history_count];
        step_back--;
    }
    selected_sq = -1;
    is_cached   = 0;
    ui_dirty    = 1;

    // OPT: Rebuild position string after undo
    rebuild_position_cmd();
}

void handle_reset_board(void) {
    if (engine_thinking) {
        sf_send_command("stop");
        engine_thinking = 0;
    }
    engine_nps        = 0;
    engine_score_type = -1;
    engine_score_val  = 0;

    gui_init_board(&current_state);
    history_count = 0;
    selected_sq   = -1;
    cursor_r      = 6;
    cursor_c      = 4;

    sf_send_command("ucinewgame");
    sf_send_command("isready");

    // OPT: Reset incremental position string
    reset_position_cmd();

    is_cached = 0;
    ui_dirty  = 1;
}

void handle_switch_sides(void) {
    if (engine_thinking) {
        sf_send_command("stop");
        engine_thinking = 0;
    }
    if      (user_side ==  1) user_side = -1;
    else if (user_side == -1) user_side =  0;
    else if (user_side ==  0) user_side =  2;
    else                      user_side =  1;
    ui_dirty = 1;
}

void adjust_time_control(void) {
    if (time_control_type == 0) {
        static const int time_list[]  = {1, 10, 50, 100, 500, 1000, 1500, 2000, 3000, 5000, 10000};
        static const int time_count   = sizeof(time_list) / sizeof(time_list[0]);
        int found_idx = -1;
        for (int i = 0; i < time_count; i++) {
            if (time_control_val == time_list[i]) { found_idx = i; break; }
        }
        time_control_val = (found_idx == -1 || found_idx == time_count - 1) ? time_list[0] : time_list[found_idx + 1];
    } else if (time_control_type == 1) {
        time_control_val = (time_control_val % 20) + 1;
    } else {
        static const int nodes_list[] = {512,1024,2048,4096,8192,16384,32768,65536,131072,262144,524288,1048576,2097152,4194304,8388608};
        static const int nodes_count  = sizeof(nodes_list) / sizeof(nodes_list[0]);
        int found_idx = -1;
        for (int i = 0; i < nodes_count; i++) {
            if (time_control_val == nodes_list[i]) { found_idx = i; break; }
        }
        time_control_val = (found_idx == -1 || found_idx == nodes_count - 1) ? nodes_list[0] : nodes_list[found_idx + 1];
    }
    ui_dirty = 1;
}

void gui_init_board(GuiBoardState *state) {
    static const int start[64] = {
        -4, -2, -3, -5, -6, -3, -2, -4,
        -1, -1, -1, -1, -1, -1, -1, -1,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         0,  0,  0,  0,  0,  0,  0,  0,
         1,  1,  1,  1,  1,  1,  1,  1,
         4,  2,  3,  5,  6,  3,  2,  4
    };
    memcpy(state->board, start, sizeof(start));
    state->turn       =  1;
    state->castle     = 15;
    state->ep         = -1;
    state->halfmoves  =  0;
    state->fullmoves  =  1;

    // OPT: Initialise cached king squares
    state->white_king_sq = 60; // e1
    state->black_king_sq =  4; // e8

    is_cached = 0;
    ui_dirty  = 1;
}

// ============================================================================
// System Main / Loop Initialization
// ============================================================================
int main(int argc, char **argv) {
    gfxInitDefault();
    consoleInit(GFX_TOP,    &topConsole);
    consoleInit(GFX_BOTTOM, &bottomConsole);

    sf_bridge_init();
    gui_init_board(&current_state);

    consoleSelect(&topConsole);
    printf("\x1b[2J");
    printf("\x1b[5;5H\x1b[33m-- Chess 3DS --\x1b[0m\n\n");
    printf("   Initializing Stockfish Engine...\n");
    printf("   Please wait, setting up transposition tables...\n");
    fflush(stdout);

    consoleSelect(&bottomConsole);
    printf("\x1b[2J");
    printf("Setting up console registers...\n");
    fflush(stdout);

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();

    Thread stockfish_thread = NULL;
    s32    prio             = 0x3B;

    stockfish_thread = threadCreate(stockfish_thread_func, NULL, ENGINE_STACK_SIZE, prio, 1, false);

    if (stockfish_thread == NULL) {
        consoleSelect(&bottomConsole);
        printf("[SYSTEM] Core 1 access restricted. Trying Core 0 (App Core)...\n");
        fflush(stdout);
        stockfish_thread = threadCreate(stockfish_thread_func, NULL, ENGINE_STACK_SIZE, prio, 0, false);
    }

    if (stockfish_thread == NULL) {
        consoleSelect(&bottomConsole);
        printf("[SYSTEM] Core 0 allocation failed. Attempting system default scheduler...\n");
        fflush(stdout);
        stockfish_thread = threadCreate(stockfish_thread_func, NULL, ENGINE_STACK_SIZE, prio, -1, false);
    }

    if (stockfish_thread == NULL) {
        consoleSelect(&bottomConsole);
        printf("\x1b[1;31m[ERROR] Failed to spawn background Stockfish thread on any core!\x1b[0m\n");
        fflush(stdout);
    } else {
        consoleSelect(&bottomConsole);
        printf("[SYSTEM] Background Engine Thread spawned successfully.\n\n");
        fflush(stdout);
    }

    sf_send_command("uci");
    engine_state = ENGINE_STATE_WAIT_UCIOK;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_START) break;

        if (kDown & KEY_UP)    { if (cursor_r > 0) { cursor_r--; ui_dirty = 1; } }
        if (kDown & KEY_DOWN)  { if (cursor_r < 7) { cursor_r++; ui_dirty = 1; } }
        if (kDown & KEY_RIGHT) { if (cursor_c < 7) { cursor_c++; ui_dirty = 1; } }
        if (kDown & KEY_LEFT)  { if (cursor_c > 0) { cursor_c--; ui_dirty = 1; } }

        if (kDown & KEY_A)      handle_select();
        if (kDown & KEY_B)      handle_undo();
        if (kDown & KEY_SELECT) handle_reset_board();
        if (kDown & KEY_X)      { board_orientation = -board_orientation; ui_dirty = 1; }
        if (kDown & KEY_Y)      handle_switch_sides();

        if (kDown & KEY_L) {
            time_control_type = (time_control_type + 1) % 3;
            time_control_val  = (time_control_type == 0) ? 1 : (time_control_type == 1 ? 1 : 512);
            ui_dirty = 1;
        }
        if (kDown & KEY_R) {
            adjust_time_control();
            ui_dirty = 1;
        }

        // OPT: Use cached values for engine-active check
        int engine_active = 0;
        if (engine_state == ENGINE_STATE_READY &&
            cached_has_legal_moves &&
            current_state.halfmoves < 100 &&
            cached_repetition_count < 3) {
            if (user_side == 2) engine_active = 1;
            else if (user_side ==  1 && current_state.turn == -1) engine_active = 1;
            else if (user_side == -1 && current_state.turn ==  1) engine_active = 1;
        }

        if (engine_active && !engine_thinking) {
            engine_thinking = 1;
            ui_dirty = 1;
            trigger_engine_move();
        }

        read_from_engine();

        if (ui_dirty) {
            draw_ui();
            ui_dirty = 0;
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    sf_send_command("quit");
    if (stockfish_thread) {
        threadJoin(stockfish_thread, U64_MAX);
        threadFree(stockfish_thread);
    }

    gfxExit();
    return 0;
}
