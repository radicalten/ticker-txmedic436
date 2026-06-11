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
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <termios.h>

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

#define MAX_HISTORY 2048

// POSIX terminal input simulation maps
#define KEY_UP      (1 << 0)
#define KEY_DOWN    (1 << 1)
#define KEY_LEFT    (1 << 2)
#define KEY_RIGHT   (1 << 3)
#define KEY_A       (1 << 4) // Select/Execute
#define KEY_B       (1 << 5) // Undo
#define KEY_X       (1 << 6) // Flip Orientation
#define KEY_Y       (1 << 7) // Change Side
#define KEY_L       (1 << 8) // Limit Type
#define KEY_R       (1 << 9) // Limit Adjust
#define KEY_SELECT  (1 << 10)// Reset Board
#define KEY_START   (1 << 11)// Exit Game

// Handshake state machine definitions
typedef enum {
    ENGINE_STATE_BOOTING,
    ENGINE_STATE_WAIT_UCIOK,
    ENGINE_STATE_WAIT_READYOK,
    ENGINE_STATE_READY,
} EngineInitState;

EngineInitState engine_state = ENGINE_STATE_BOOTING;

typedef struct {
    int board[64]; // P=1, N=2, B=3, R=4, Q=5, K=6 (Positive=White, Negative=Black)
    int turn;      // 1 = White, -1 = Black
    int castle;    // Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep;        // En-passant square (0-63), -1 if none
    int halfmoves; // For 50-move rule
    int fullmoves;
} BoardState;

// Renamed to GuiMove to prevent namespace collision with types.h "Move" typedef
typedef struct {
    int from;
    int to;
    int promo; // 0=None, 2=N, 3=B, 4=R, 5=Q
} GuiMove;

BoardState current_state;
BoardState history[MAX_HISTORY];
GuiMove gui_move_history[MAX_HISTORY];
int history_count = 0;

int cursor_r = 6;
int cursor_c = 4;
int selected_sq = -1;

int board_orientation = 1; 
int user_side = 1;         

int time_control_type = 0; 
int time_control_val = 100; // Default: 100ms

int engine_thinking = 0;
long long engine_nps = 0;
int engine_score_type = -1; 
int engine_score_val = 0;

// POSIX communication pipes and physical terminal file descriptor
int pipe_gui_to_eng[2];
int pipe_eng_to_gui[2];
int term_fd = -1; // File descriptor dedicated to writing directly to the active screen
struct termios orig_termios;

// Scrollable background log console variables
#define LOG_WINDOW_SIZE 4
char bottom_logs[LOG_WINDOW_SIZE][128];

// Function prototypes prefixed with gui_ to prevent collisions with Stockfish internals
void gui_init_board(BoardState *state);
int gui_is_legal_move(const BoardState *state, GuiMove m);
int gui_has_legal_moves(const BoardState *state);
int gui_is_square_attacked(const BoardState *state, int sq, int attacker);
void gui_make_move(const BoardState *src, BoardState *dst, GuiMove m);
void trigger_engine_move(void);
int gui_find_king(const BoardState *state, int color);
int gui_count_repetitions(const BoardState *state);
int get_promo_choice(void);
void move_to_san(const BoardState *state, GuiMove m, char *buf);

// Safe POSIX Engine Interfacing
void sf_send_command(const char *cmd) {
    char packet[8200];
    snprintf(packet, sizeof(packet), "%s\n", cmd);
    write(pipe_gui_to_eng[1], packet, strlen(packet));
}

int sf_get_output(char *buf, int max_len) {
    ssize_t bytes = read(pipe_eng_to_gui[0], buf, max_len);
    if (bytes < 0) return 0;
    return (int)bytes;
}

// Redirect stderr/stdout logs to lower panel log list
void add_log(const char *line) {
    char clean[128];
    strncpy(clean, line, sizeof(clean));
    clean[sizeof(clean) - 1] = '\0';
    int len = strlen(clean);
    while (len > 0 && (clean[len - 1] == '\r' || clean[len - 1] == '\n')) {
        clean[len - 1] = '\0';
        len--;
    }
    if (len == 0) return;

    for (int i = 0; i < LOG_WINDOW_SIZE - 1; i++) {
        strcpy(bottom_logs[i], bottom_logs[i + 1]);
    }
    strncpy(bottom_logs[LOG_WINDOW_SIZE - 1], clean, 127);
    bottom_logs[LOG_WINDOW_SIZE - 1][127] = '\0';
}

// POSIX non-blocking raw terminal configurations on /dev/tty
void disable_raw_mode(void) {
    if (term_fd != -1) {
        tcsetattr(term_fd, TCSAFLUSH, &orig_termios);
        dprintf(term_fd, "\x1b[?25h\x1b[2J\x1b[H\x1b[0m\n"); // Restore standard pointer visibility and clear screen
    }
}

void enable_raw_mode(void) {
    tcgetattr(term_fd, &orig_termios);
    atexit(disable_raw_mode);
    
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    tcsetattr(term_fd, TCSAFLUSH, &raw);

    // Make terminal descriptor non-blocking
    int flags = fcntl(term_fd, F_GETFL, 0);
    fcntl(term_fd, F_SETFL, flags | O_NONBLOCK);
    dprintf(term_fd, "\x1b[?25l"); // Hide structural cursor pointer
}

// Read inputs from /dev/tty and map them to standard actions
unsigned int read_terminal_input(void) {
    char c;
    ssize_t r = read(term_fd, &c, 1);
    if (r == 1) {
        if (c == '\x1b') { // Handle arrow escape sequences
            char seq[3];
            if (read(term_fd, &seq[0], 1) == 1 && read(term_fd, &seq[1], 1) == 1) {
                if (seq[0] == '[') {
                    switch (seq[1]) {
                        case 'A': return KEY_UP;
                        case 'B': return KEY_DOWN;
                        case 'C': return KEY_RIGHT;
                        case 'D': return KEY_LEFT;
                    }
                }
            }
            return KEY_START; // Escape exits execution
        }
        switch (c) {
            case '\r':
            case '\n':
            case ' ': return KEY_A;      // Space/Enter = Select
            case 'u':
            case 'U':
            case 'b':
            case 'B': return KEY_B;      // B / U = Undo
            case 'f':
            case 'F':
            case 'o':
            case 'O': return KEY_X;      // F / O = Flip board orientation
            case 's':
            case 'S': return KEY_Y;      // S = Switch Sides
            case 'l':
            case 'L': return KEY_L;      // L = Toggle limits
            case 't':
            case 'T': return KEY_R;      // T = Adjust limit options
            case 'r':
            case 'R': return KEY_SELECT; // R = Reset engine
            case 'q':
            case 'Q': return KEY_START;  // Q = Quit
        }
    }
    return 0;
}

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
    m.to = t_row * 8 + t_col;
    if (strlen(str) == 5) {
        char p = str[4];
        if (p == 'n') m.promo = 2;
        else if (p == 'b') m.promo = 3;
        else if (p == 'r') m.promo = 4;
        else m.promo = 5;
    }
    return m;
}

void move_to_san(const BoardState *state, GuiMove m, char *buf) {
    int p = abs(state->board[m.from]);
    int turn = state->turn;
    
    if (p == 6) {
        if (m.from == 60 && m.to == 62 && turn == 1) { strcpy(buf, "O-O"); goto append_suffixes; }
        if (m.from == 60 && m.to == 58 && turn == 1) { strcpy(buf, "O-O-O"); goto append_suffixes; }
        if (m.from == 4 && m.to == 6 && turn == -1) { strcpy(buf, "O-O"); goto append_suffixes; }
        if (m.from == 4 && m.to == 2 && turn == -1) { strcpy(buf, "O-O-O"); goto append_suffixes; }
    }

    char *ptr = buf;
    int is_cap = (state->board[m.to] != 0) || (p == 1 && m.to == state->ep);

    if (p == 1) {
        if (is_cap) {
            *ptr++ = 'a' + (m.from % 8);
            *ptr++ = 'x';
        }
    } else {
        char p_chars[] = "?PNBRQK";
        *ptr++ = p_chars[p];

        int another_can_move = 0;
        int same_file = 0;
        int same_rank = 0;
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

        if (is_cap) {
            *ptr++ = 'x';
        }
    }

    *ptr++ = 'a' + (m.to % 8);
    *ptr++ = '8' - (m.to / 8);

    if (m.promo != 0) {
        *ptr++ = '=';
        char p_chars[] = "?PNBRQK";
        *ptr++ = p_chars[m.promo];
    }
    *ptr = '\0';

append_suffixes:;
    BoardState next;
    gui_make_move(state, &next, m);
    int op_king = gui_find_king(&next, next.turn);
    int is_check = (op_king != -1) && gui_is_square_attacked(&next, op_king, -next.turn);
    int has_moves = gui_has_legal_moves(&next);
    
    ptr = buf + strlen(buf);
    if (is_check) {
        if (!has_moves) {
            *ptr++ = '#';
        } else {
            *ptr++ = '+';
        }
    }
    *ptr = '\0';
}

void push_state(const BoardState *state, GuiMove m) {
    if (history_count < MAX_HISTORY - 1) {
        history[history_count] = *state;
        gui_move_history[history_count] = m;
        history_count++;
    }
}

void trigger_engine_move(void) {
    engine_nps = 0;
    engine_score_type = -1;
    engine_score_val = 0;

    static char cmd[8192];
    
    if (history_count == 0) {
        strcpy(cmd, "position startpos");
    } else {
        strcpy(cmd, "position startpos moves");
        int len = strlen(cmd);
        for (int i = 0; i < history_count; i++) {
            char uci_m[10];
            move_to_uci(gui_move_history[i], uci_m);
            int move_len = strlen(uci_m);
            if (len + 1 + move_len + 2 >= (int)sizeof(cmd)) break;
            cmd[len++] = ' ';
            strcpy(cmd + len, uci_m);
            len += move_len;
        }
        cmd[len] = '\0';
    }
    
    sf_send_command(cmd);

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
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
        line[len - 1] = '\0';
        len--;
    }

    add_log(line);

    if (engine_state == ENGINE_STATE_WAIT_UCIOK) {
        if (strstr(line, "uciok") != NULL) {
            sf_send_command("isready");
            engine_state = ENGINE_STATE_WAIT_READYOK;
            add_log("[GUI] Received 'uciok' -> Sending 'isready'");
        }
    } else if (engine_state == ENGINE_STATE_WAIT_READYOK) {
        if (strstr(line, "readyok") != NULL) {
            sf_send_command("setoption name Hash value 16"); 
            sf_send_command("setoption name Ponder value false");
            sf_send_command("ucinewgame");
            engine_state = ENGINE_STATE_READY;
            add_log("[GUI] Handshake Complete.");
        }
    }

    if (strncmp(line, "info", 4) == 0) {
        char *nps_ptr = strstr(line, " nps ");
        if (nps_ptr) {
            long long val;
            if (sscanf(nps_ptr, " nps %lld", &val) == 1) {
                engine_nps = val;
            }
        }

        char *score_ptr = strstr(line, " score ");
        if (score_ptr) {
            int score_val = 0;
            char score_type[16];
            if (sscanf(score_ptr, " score %15s %d", score_type, &score_val) == 2) {
                if (strcmp(score_type, "cp") == 0) {
                    engine_score_type = 0;
                    engine_score_val = score_val * current_state.turn;
                } else if (strcmp(score_type, "mate") == 0) {
                    engine_score_type = 1;
                    engine_score_val = score_val * current_state.turn;
                }
            }
        }
    }

    if (strncmp(line, "bestmove", 8) == 0) {
        char move_str[16];
        if (sscanf(line, "bestmove %15s", move_str) == 1) {
            if (strcmp(move_str, "(none)") == 0 || strcmp(move_str, "NULL") == 0) {
                engine_thinking = 0;
                return;
            }
            
            GuiMove m = gui_uci_to_move(move_str);
            if (gui_is_legal_move(&current_state, m)) {
                push_state(&current_state, m);
                BoardState next;
                gui_make_move(&current_state, &next, m);
                current_state = next;
            }
            engine_thinking = 0;
        }
    }
}

void read_from_engine(void) {
    char tmp[512];
    static char line_buf[1024];
    static int line_len = 0;
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

int gui_find_king(const BoardState *state, int color) {
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == color * 6) return i;
    }
    return -1;
}

int gui_is_square_attacked(const BoardState *state, int sq, int attacker) {
    int r = sq / 8, c = sq % 8;

    int k_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int k_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_r[i], nc = c + k_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 2) return 1;
        }
    }

    int kg_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int kg_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kg_r[i], nc = c + kg_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 6) return 1;
        }
    }

    int p_offset = (attacker == 1) ? 1 : -1;
    for (int dc = -1; dc <= 1; dc += 2) {
        int nr = r + p_offset, nc = c + dc;
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == attacker * 1) return 1;
        }
    }

    int d_r[] = {-1, -1, 1, 1};
    int d_c[] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + d_r[i], nc = c + d_c[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int target = state->board[nr * 8 + nc];
            if (target != 0) {
                if (target == attacker * 3 || target == attacker * 5) return 1;
                break;
            }
            nr += d_r[i]; nc += d_c[i];
        }
    }

    int s_r[] = {-1, 1, 0, 0};
    int s_c[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r + s_r[i], nc = c + s_c[i];
        while (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
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

int gui_is_pseudo_legal_move(const BoardState *state, GuiMove m) {
    int p = state->board[m.from];
    int target = state->board[m.to];
    int turn = state->turn;

    if (p == 0) return 0;
    if ((turn == 1 && p < 0) || (turn == -1 && p > 0)) return 0;
    if (m.from == m.to) return 0;
    if (target != 0 && ((turn == 1 && target > 0) || (turn == -1 && target < 0))) return 0;

    int fr = m.from / 8, fc = m.from % 8;
    int tr = m.to / 8, tc = m.to % 8;
    int dr = tr - fr, dc = tc - fc;
    int abs_dr = abs(dr), abs_dc = abs(dc);

    switch (abs(p)) {
        case 1: { 
            int dir = (turn == 1) ? -1 : 1;
            if (dc == 0 && dr == dir && target == 0) return 1;
            int start_r = (turn == 1) ? 6 : 1;
            if (dc == 0 && fr == start_r && dr == 2 * dir) {
                if (state->board[(fr + dir) * 8 + fc] == 0 && target == 0) return 1;
            }
            if (abs_dc == 1 && dr == dir) {
                if (target != 0) return 1;
                if (m.to == state->ep) return 1;
            }
            return 0;
        }
        case 2: 
            return ((abs_dr == 2 && abs_dc == 1) || (abs_dr == 1 && abs_dc == 2));
        case 3: { 
            if (abs_dr != abs_dc) return 0;
            int sr = (dr > 0) ? 1 : -1, sc = (dc > 0) ? 1 : -1;
            int r = fr + sr, c = fc + sc;
            while (r != tr && c != tc) {
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
            int sr = (dr == 0) ? 0 : ((dr > 0) ? 1 : -1);
            int sc = (dc == 0) ? 0 : ((dc > 0) ? 1 : -1);
            if (abs_dr == abs_dc) {
                sr = (dr > 0) ? 1 : -1;
                sc = (dc > 0) ? 1 : -1;
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
                        if (state->board[61] == 0 && state->board[62] == 0) {
                            if (!gui_is_square_attacked(state, 60, -1) &&
                                !gui_is_square_attacked(state, 61, -1) &&
                                !gui_is_square_attacked(state, 62, -1)) return 1;
                        }
                    }
                    if (m.to == 58 && (state->castle & 2)) {
                        if (state->board[59] == 0 && state->board[58] == 0 && state->board[57] == 0) {
                            if (!gui_is_square_attacked(state, 60, -1) &&
                                !gui_is_square_attacked(state, 59, -1) &&
                                !gui_is_square_attacked(state, 58, -1)) return 1;
                        }
                    }
                }
                if (turn == -1 && fr == 0 && fc == 4) {
                    if (m.to == 6 && (state->castle & 4)) {
                        if (state->board[5] == 0 && state->board[6] == 0) {
                            if (!gui_is_square_attacked(state, 4, 1) &&
                                !gui_is_square_attacked(state, 5, 1) &&
                                !gui_is_square_attacked(state, 6, 1)) return 1;
                        }
                    }
                    if (m.to == 2 && (state->castle & 8)) {
                        if (state->board[3] == 0 && state->board[2] == 0 && state->board[1] == 0) {
                            if (!gui_is_square_attacked(state, 4, 1) &&
                                !gui_is_square_attacked(state, 3, 1) &&
                                !gui_is_square_attacked(state, 2, 1)) return 1;
                        }
                    }
                }
            }
            return 0;
        }
    }
    return 0;
}

int gui_is_legal_move(const BoardState *state, GuiMove m) {
    if (!gui_is_pseudo_legal_move(state, m)) return 0;
    BoardState next;
    gui_make_move(state, &next, m);
    int king = gui_find_king(&next, state->turn);
    if (king == -1) return 0;
    return !gui_is_square_attacked(&next, king, -state->turn);
}

int gui_has_legal_moves(const BoardState *state) {
    for (int f = 0; f < 64; f++) {
        if (state->board[f] == 0) continue;
        if ((state->turn == 1 && state->board[f] < 0) || (state->turn == -1 && state->board[f] > 0)) continue;
        for (int t = 0; t < 64; t++) {
            GuiMove m = {f, t, 0};
            if (abs(state->board[f]) == 1 && (t / 8 == 0 || t / 8 == 7)) {
                m.promo = 5;
            }
            if (gui_is_legal_move(state, m)) return 1;
        }
    }
    return 0;
}

int gui_count_repetitions(const BoardState *state) {
    int count = 1;
    for (int i = 0; i < history_count; i++) {
        if (state->turn == history[i].turn &&
            state->castle == history[i].castle &&
            state->ep == history[i].ep &&
            memcmp(state->board, history[i].board, sizeof(state->board)) == 0) {
            count++;
        }
    }
    return count;
}

void gui_make_move(const BoardState *src, BoardState *dst, GuiMove m) {
    *dst = *src;
    int p = dst->board[m.from];
    int is_capture = (src->board[m.to] != 0) || (abs(p) == 1 && m.to == src->ep);

    dst->board[m.from] = 0;
    if (m.promo != 0) {
        dst->board[m.to] = dst->turn * m.promo;
    } else {
        dst->board[m.to] = p;
    }

    if (abs(p) == 1 && m.to == dst->ep) {
        int p_dir = (dst->turn == 1 ? 8 : -8);
        dst->board[m.to + p_dir] = 0;
    }

    dst->ep = -1;
    if (abs(p) == 1 && abs(m.from - m.to) == 16) {
        dst->ep = m.from + (dst->turn == 1 ? -8 : 8);
    }

    if (abs(p) == 6) {
        if (m.from == 60 && m.to == 62) { dst->board[61] = dst->board[63]; dst->board[63] = 0; }
        else if (m.from == 60 && m.to == 58) { dst->board[59] = dst->board[56]; dst->board[56] = 0; }
        else if (m.from == 4 && m.to == 6) { dst->board[5] = dst->board[7]; dst->board[7] = 0; }
        else if (m.from == 4 && m.to == 2) { dst->board[3] = dst->board[0]; dst->board[0] = 0; }
    }

    if (m.from == 60) dst->castle &= ~1 & ~2;
    if (m.from == 4)  dst->castle &= ~4 & ~8;
    if (m.from == 63 || m.to == 63) dst->castle &= ~1;
    if (m.from == 56 || m.to == 56) dst->castle &= ~2;
    if (m.from == 7  || m.to == 7)  dst->castle &= ~4;
    if (m.from == 0  || m.to == 0)  dst->castle &= ~8;

    dst->turn = -dst->turn;
    if (abs(p) == 1 || is_capture) dst->halfmoves = 0;
    else dst->halfmoves++;
    
    if (dst->turn == 1) dst->fullmoves++;
}

// GUI Drawing and Terminal ANSI Output (Sized strictly for 80x24 layout boundaries)
void draw_ui(void) {
    // Flick-free cursor jump home
    dprintf(term_fd, "\x1b[1;1H");

    const char *turn_str = (current_state.turn == 1) ? "\x1b[1;33mWhite\x1b[0m" : "\x1b[1;35mBlack\x1b[0m";
    int king = gui_find_king(&current_state, current_state.turn);
    int is_ch = gui_is_square_attacked(&current_state, king, -current_state.turn);
    int has_mov = gui_has_legal_moves(&current_state);
    const char *w_play = (user_side == 1 || user_side == 0) ? "Hum" : "Eng";
    const char *b_play = (user_side == -1 || user_side == 0) ? "Hum" : "Eng";
    int repetitions = gui_count_repetitions(&current_state);

    // Dynamic 8-row sidebar to output side-by-side with the compact board ranks
    char rp[8][64];
    for (int i = 0; i < 8; i++) {
        rp[i][0] = '\0';
    }

    // Row 0: Status
    if (current_state.halfmoves >= 100) {
        sprintf(rp[0], "  [DRAW (50m-rule)]");
    } else if (repetitions >= 3) {
        sprintf(rp[0], "  [DRAW (3-fold)]");
    } else if (!has_mov) {
        sprintf(rp[0], "  %s", is_ch ? "\x1b[1;31m[CHECKMATE!]\x1b[0m" : "\x1b[1;36m[STALEMATE!]\x1b[0m");
    } else if (is_ch) {
        sprintf(rp[0], "  %s (\x1b[1;31mCHECK!\x1b[0m)", turn_str);
    } else {
        sprintf(rp[0], "  %s to play", turn_str);
    }

    // Row 1: Controllers Config
    sprintf(rp[1], "  W: %s | B: %s", w_play, b_play);

    // Row 2: Limit Settings
    if (time_control_type == 0) {
        sprintf(rp[2], "  Lim: %d ms", time_control_val);
    } else if (time_control_type == 1) {
        sprintf(rp[2], "  Lim: depth %d", time_control_val);
    } else {
        sprintf(rp[2], "  Lim: %d nodes", time_control_val);
    }

    // Row 3: Eval & Engine NPS Metrics
    char eval_str[32] = "----";
    if (engine_state == ENGINE_STATE_READY) {
        if (engine_score_type == 0) {
            sprintf(eval_str, "%+.2f", (double)engine_score_val / 100.0);
        } else if (engine_score_type == 1) {
            sprintf(eval_str, "M%d", engine_score_val);
        }
    }
    char nps_str[32] = "----";
    if (engine_nps >= 1000000) {
        sprintf(nps_str, "%.1fM", (double)engine_nps / 1000000.0);
    } else if (engine_nps >= 1000) {
        sprintf(nps_str, "%.0fk", (double)engine_nps / 1000.0);
    } else if (engine_nps > 0) {
        sprintf(nps_str, "%lld", engine_nps);
    }
    sprintf(rp[3], "  Ev:%-5s Np:%-5s", eval_str, nps_str);

    // Row 4: Recent moves header
    sprintf(rp[4], "  Recent Moves:");

    // Rows 5-7: Show the last 3 turns
    int total_full_moves = (history_count + 1) / 2;
    int start_move = (total_full_moves > 3) ? (total_full_moves - 2) : 1;
    for (int idx = 0; idx < 3; idx++) {
        int display = start_move + idx;
        if (total_full_moves > 0 && display <= total_full_moves) {
            int w_idx = (display - 1) * 2;
            int b_idx = w_idx + 1;
            char w_str[16] = "-----";
            char b_str[16] = "-----";
            if (w_idx < history_count) {
                move_to_san(&history[w_idx], gui_move_history[w_idx], w_str);
            }
            if (b_idx < history_count) {
                move_to_san(&history[b_idx], gui_move_history[b_idx], b_str);
            } else if (w_idx < history_count) {
                strcpy(b_str, "...");
            }
            sprintf(rp[5 + idx], "  %2d. %-5s %-5s", display, w_str, b_str);
        } else {
            sprintf(rp[5 + idx], "  %2d.  ---   --- ", display);
        }
    }

    // Top rank coordinate layout
    if (board_orientation == 1) {
        dprintf(term_fd, "     a  b  c  d  e  f  g  h\n");
    } else {
        dprintf(term_fd, "     h  g  f  e  d  c  b  a\n");
    }

    int king_in_check = -1;
    int w_king = gui_find_king(&current_state, 1);
    int b_king = gui_find_king(&current_state, -1);
    if (w_king != -1 && gui_is_square_attacked(&current_state, w_king, -1)) {
        king_in_check = w_king;
    } else if (b_king != -1 && gui_is_square_attacked(&current_state, b_king, 1)) {
        king_in_check = b_king;
    }

    // Compact single-line board rows loop
    for (int r = 0; r < 8; r++) {
        int rank_lbl = (board_orientation == 1) ? (8 - r) : (r + 1);
        dprintf(term_fd, "  %d ", rank_lbl);

        for (int c = 0; c < 8; c++) {
            int sq = screen_to_board_sq(r, c);
            int p = current_state.board[sq];

            int is_light = ((sq / 8) + (sq % 8)) % 2 == 0;
            const char *bg_color;

            int is_selected = (sq == selected_sq);
            int is_cursor = (r == cursor_r && c == cursor_c);

            int is_prev_move = 0;
            if (history_count > 0) {
                GuiMove last_move = gui_move_history[history_count - 1];
                if (sq == last_move.from || sq == last_move.to) {
                    is_prev_move = 1;
                }
            }

            int is_legal_dest = 0;
            if (selected_sq != -1) {
                GuiMove test_m = {selected_sq, sq, 0};
                if (abs(current_state.board[selected_sq]) == 1 && (sq / 8 == 0 || sq / 8 == 7)) {
                    test_m.promo = 5;
                }
                if (gui_is_legal_move(&current_state, test_m)) {
                    is_legal_dest = 1;
                }
            }

            if (is_cursor) {
                bg_color = "\x1b[48;5;208m"; // Orange cursor
            } else if (is_selected) {
                bg_color = "\x1b[48;5;34m";  // Green selected piece
            } else if (sq == king_in_check) {
                bg_color = "\x1b[48;5;196m"; // Red checked king
            } else if (is_prev_move) {
                bg_color = is_light ? "\x1b[48;5;75m" : "\x1b[48;5;68m"; // Blue historical path
            } else if (is_legal_dest) {
                bg_color = is_light ? "\x1b[48;5;151m" : "\x1b[48;5;108m"; // Light green targets
            } else {
                bg_color = is_light ? "\x1b[48;5;180m" : "\x1b[48;5;94m"; // Classic wood tones
            }

            const char *piece_str = " ";
            const char *fg_color = "\x1b[38;5;232m"; // Black Pieces
            if (p != 0) {
                if (p > 0) fg_color = "\x1b[38;5;255m\x1b[1m"; // Bold White Pieces
                switch (abs(p)) {
                    case 1: piece_str = "♟"; break;
                    case 2: piece_str = "♞"; break;
                    case 3: piece_str = "♝"; break;
                    case 4: piece_str = "♜"; break;
                    case 5: piece_str = "♛"; break;
                    case 6: piece_str = "♚"; break;
                }
            }

            dprintf(term_fd, "%s%s %s \x1b[0m", bg_color, fg_color, piece_str);
        }

        dprintf(term_fd, " %d %s\x1b[K\n", rank_lbl, rp[r]);
    }

    // Bottom rank coordinates layout
    if (board_orientation == 1) {
        dprintf(term_fd, "     a  b  c  d  e  f  g  h\x1b[K\n\n");
    } else {
        dprintf(term_fd, "     h  g  f  e  d  c  b  a\x1b[K\n\n");
    }

    // Help Lines (Row 13 & 14)
    dprintf(term_fd, " \x1b[38;5;245m[Arrows] Move | [Space/Enter] Select | [U] Undo | [R] Reset | [O] Flip\x1b[0m\x1b[K\n");
    dprintf(term_fd, " \x1b[38;5;245m[S] Sides | [T] Lim Type | [V] Lim Adjust | [Q] Quit\x1b[0m\x1b[K\n");

    // Divider Line (Row 15)
    dprintf(term_fd, " \x1b[1;33m--- ENGINE LOG WINDOW ---\x1b[0m\x1b[K\n");

    // Dynamic Engine output streams (Rows 16-19)
    for (int i = 0; i < LOG_WINDOW_SIZE; i++) {
        dprintf(term_fd, "%s\x1b[K\n", bottom_logs[i]);
    }
}

// Blocks and prompts user synchronously to select an under-promotion piece
int get_promo_choice(void) {
    dprintf(term_fd, "\n\x1b[1;33mPROMOTION! Type Selection: [q] Queen  [r] Rook  [b] Bishop  [n] Knight\x1b[0m\n");
    while (1) {
        char c;
        if (read(term_fd, &c, 1) == 1) {
            if (c == 'q' || c == 'Q') return 5;
            if (c == 'r' || c == 'R') return 4;
            if (c == 'b' || c == 'B') return 3;
            if (c == 'n' || c == 'N') return 2;
        }
        usleep(10000);
    }
    return 5;
}

// GUI Action / Setting Handlers
void handle_select(void) {
    if (engine_thinking) {
        return;
    }

    int is_engine_turn = 0;
    if (user_side == 2) {
        is_engine_turn = 1; 
    } else if (user_side == 1 && current_state.turn == -1) {
        is_engine_turn = 1; 
    } else if (user_side == -1 && current_state.turn == 1) {
        is_engine_turn = 1; 
    }

    if (is_engine_turn) {
        return; 
    }

    if (!gui_has_legal_moves(&current_state) || current_state.halfmoves >= 100 || gui_count_repetitions(&current_state) >= 3) {
        return;
    }

    int sq = screen_to_board_sq(cursor_r, cursor_c);
    if (selected_sq == -1) {
        int p = current_state.board[sq];
        if (p != 0 && ((current_state.turn == 1 && p > 0) || (current_state.turn == -1 && p < 0))) {
            selected_sq = sq;
        }
    } else {
        GuiMove m = {selected_sq, sq, 0};
        int p = current_state.board[selected_sq];
        int is_promo = (abs(p) == 1 && (sq / 8 == 0 || sq / 8 == 7));
        if (is_promo) m.promo = 5;

        if (gui_is_legal_move(&current_state, m)) {
            if (is_promo) {
                m.promo = get_promo_choice();
            }
            push_state(&current_state, m);
            BoardState next;
            gui_make_move(&current_state, &next, m);
            current_state = next;
            selected_sq = -1;
            
            engine_nps = 0;
            engine_score_type = -1;
            engine_score_val = 0;
        } else {
            int target = current_state.board[sq];
            if (target != 0 && ((current_state.turn == 1 && target > 0) || (current_state.turn == -1 && target < 0))) {
                selected_sq = sq;
            } else {
                selected_sq = -1;
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
    int step_back = (user_side == 1 || user_side == -1) ? 2 : 1;
    while (step_back > 0 && history_count > 0) {
        history_count--;
        current_state = history[history_count];
        step_back--;
    }
    selected_sq = -1;
}

void handle_reset_board(void) {
    if (engine_thinking) {
        sf_send_command("stop");
        engine_thinking = 0;
    }
    engine_nps = 0;
    engine_score_type = -1;
    engine_score_val = 0;
    gui_init_board(&current_state);
    history_count = 0;
    selected_sq = -1;
    cursor_r = 6;
    cursor_c = 4;
    sf_send_command("ucinewgame");
    sf_send_command("isready");
}

void handle_switch_sides(void) {
    if (engine_thinking) {
        sf_send_command("stop");
        engine_thinking = 0;
    }
    if (user_side == 1) user_side = -1;
    else if (user_side == -1) user_side = 0;
    else if (user_side == 0) user_side = 2;
    else user_side = 1;
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

void gui_init_board(BoardState *state) {
    int start[64] = {
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
    state->turn = 1;
    state->castle = 15;
    state->ep = -1;
    state->halfmoves = 0;
    state->fullmoves = 1;
}

// Background POSIX Worker Core
void* stockfish_thread_func(void* arg) {
    (void)arg;
    // Pipe standard I/O redirection for the detached worker thread
    dup2(pipe_gui_to_eng[0], STDIN_FILENO);
    dup2(pipe_eng_to_gui[1], STDOUT_FILENO);
    dup2(pipe_eng_to_gui[1], STDERR_FILENO);

    close(pipe_gui_to_eng[1]);
    close(pipe_eng_to_gui[0]);

    // Setup Engine variables inside isolated context
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

    char *engine_argv[] = {"stockfish", NULL};
    uci_loop(1, engine_argv);

    threads_exit();
    TB_free();
    options_free();
    tt_free();
    pb_free();
#ifdef NNUE
    nnue_free();
#endif

    return NULL;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Open Dedicated direct output file descriptor to bypass shared Standard IO
    term_fd = open("/dev/tty", O_RDWR);
    if (term_fd < 0) {
        fprintf(stderr, "Failed to open controlling terminal /dev/tty!\n");
        return 1;
    }

    for (int i = 0; i < LOG_WINDOW_SIZE; i++) {
        strcpy(bottom_logs[i], " ");
    }

    // Set up standard POSIX read pipes
    if (pipe(pipe_gui_to_eng) < 0 || pipe(pipe_eng_to_gui) < 0) {
        dprintf(term_fd, "Failed to initialize IPC Pipes!\n");
        close(term_fd);
        return 1;
    }

    // Make the engine interface non-blocking
    int flags = fcntl(pipe_eng_to_gui[0], F_GETFL, 0);
    fcntl(pipe_eng_to_gui[0], F_SETFL, flags | O_NONBLOCK);

    gui_init_board(&current_state);

    // Clean loading layout inside safe 24-row coordinates
    dprintf(term_fd, "\x1b[2J\x1b[H\n");
    dprintf(term_fd, "   \x1b[1;33m-- Chess CLI --\x1b[0m\n\n");
    dprintf(term_fd, "   Initializing Built-In CFish Engine...\n");
    dprintf(term_fd, "   Please wait, setting up transposition tables...\n");

    enable_raw_mode();

    pthread_t thread_id;
    if (pthread_create(&thread_id, NULL, stockfish_thread_func, NULL) != 0) {
        disable_raw_mode();
        dprintf(term_fd, "Failed to launch Stockfish Thread!\n");
        close(term_fd);
        return 1;
    }

    // Handshake initialization command
    sf_send_command("uci");
    engine_state = ENGINE_STATE_WAIT_UCIOK;

    while (1) {
        unsigned int keys = read_terminal_input();

        if (keys & KEY_START) break; 

        if (keys & KEY_UP)    { if (cursor_r > 0) cursor_r--; }
        if (keys & KEY_DOWN)  { if (cursor_r < 7) cursor_r++; }
        if (keys & KEY_RIGHT) { if (cursor_c < 7) cursor_c++; }
        if (keys & KEY_LEFT)  { if (cursor_c > 0) cursor_c--; }

        if (keys & KEY_A)      handle_select();
        if (keys & KEY_B)      handle_undo();
        if (keys & KEY_SELECT) handle_reset_board();
        if (keys & KEY_X)      board_orientation = -board_orientation;
        if (keys & KEY_Y)      handle_switch_sides();

        if (keys & KEY_L) {
            time_control_type = (time_control_type + 1) % 3;
            time_control_val = (time_control_type == 0) ? 100 : (time_control_type == 1 ? 1 : 512);
        }
        if (keys & KEY_R) {
            adjust_time_control();
        }

        int engine_active = 0;
        if (engine_state == ENGINE_STATE_READY && gui_has_legal_moves(&current_state) && current_state.halfmoves < 100 && gui_count_repetitions(&current_state) < 3) {
            if (user_side == 2) engine_active = 1;
            else if (user_side == 1 && current_state.turn == -1) engine_active = 1;
            else if (user_side == -1 && current_state.turn == 1) engine_active = 1;
        }

        if (engine_active && !engine_thinking) {
            engine_thinking = 1;
            trigger_engine_move();
        }

        read_from_engine();
        draw_ui();

        // 60 FPS update rate limit
        usleep(16000); 
    }

    sf_send_command("quit");
    pthread_join(thread_id, NULL);
    disable_raw_mode();
    close(term_fd);

    return 0;
}
