#define IS_GUI // Tells 3ds_bridge.h to let this file write directly to the screens
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "3ds_bridge.h"

#define MAX_HISTORY 2048

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

typedef struct {
    int from;
    int to;
    int promo; // 0=None, 2=N, 3=B, 4=R, 5=Q
} Move;

BoardState current_state;
BoardState history[MAX_HISTORY];
Move move_history[MAX_HISTORY];
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

// Optimization Flag: Only redraw when the board state changes, cursor moves, or engine outputs
int redraw_needed = 1;

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

void init_board(BoardState *state);
int is_legal_move(const BoardState *state, Move m);
int has_legal_moves(const BoardState *state);
int is_square_attacked(const BoardState *state, int sq, int attacker);
void make_move(const BoardState *src, BoardState *dst, Move m);
void trigger_engine_move(void);
int find_king(const BoardState *state, int color);
int count_repetitions(const BoardState *state);
int get_promo_choice(void);

Move gui_uci_to_move(const char *str);

int screen_to_board_sq(int r, int c) {
    if (board_orientation == 1) {
        return r * 8 + c;
    } else {
        return (7 - r) * 8 + (7 - c);
    }
}

void move_to_uci(Move m, char *buf) {
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

Move gui_uci_to_move(const char *str) {
    Move m = {-1, -1, 0};
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

void move_to_san(const BoardState *state, Move m, char *buf) {
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
                Move alt_m = {sq, m.to, 0};
                if (is_legal_move(state, alt_m)) {
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
    make_move(state, &next, m);
    int op_king = find_king(&next, next.turn);
    int is_check = (op_king != -1) && is_square_attacked(&next, op_king, -next.turn);
    int has_moves = has_legal_moves(&next);
    
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

void push_state(const BoardState *state, Move m) {
    if (history_count < MAX_HISTORY - 1) {
        history[history_count] = *state;
        move_history[history_count] = m;
        
        // Compute and cache the SAN string immediately upon pushing the move
        move_to_san(state, m, san_history[history_count]);
        
        history_count++;
    }
}

void push_raw_log(const char *line) {
    if (raw_log_count < 10) {
        strncpy(raw_log[raw_log_count], line, 31);
        raw_log[raw_log_count][31] = '\0';
        raw_log_count++;
    } else {
        // Scroll buffer items up
        for (int i = 0; i < 9; i++) {
            memmove(raw_log[i], raw_log[i + 1], sizeof(raw_log[0]));
        }
        strncpy(raw_log[9], line, 31);
        raw_log[9][31] = '\0';
    }
    redraw_needed = 1; // Mark UI as needing to be redrawn on the next frame
}

void trigger_engine_move(void) {
    engine_nps = 0;
    engine_score_type = -1;
    engine_score_val = 0;
    engine_depth = 0;
    engine_nodes = 0;
    engine_pv[0] = '\0';

    static char cmd[8192];
    
    if (history_count == 0) {
        strcpy(cmd, "position startpos");
    } else {
        strcpy(cmd, "position startpos moves");
        int len = strlen(cmd);
        for (int i = 0; i < history_count; i++) {
            char uci_m[10];
            move_to_uci(move_history[i], uci_m);
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
    redraw_needed = 1;
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
            redraw_needed = 1;
        }
    } else if (engine_state == ENGINE_STATE_WAIT_READYOK) {
        if (strstr(line, "readyok") != NULL) {
            sf_send_command("setoption name Hash value 1"); 
            sf_send_command("setoption name Ponder value false");
            sf_send_command("ucinewgame");
            engine_state = ENGINE_STATE_READY;
            redraw_needed = 1;
        }
    }

    if (strncmp(line, "info", 4) == 0) {
        char *nps_ptr = strstr(line, " nps ");
        if (nps_ptr) {
            long long val;
            if (sscanf(nps_ptr, " nps %lld", &val) == 1) {
                engine_nps = val;
                redraw_needed = 1;
            }
        }

        char *depth_ptr = strstr(line, " depth ");
        if (depth_ptr) {
            int d_val;
            if (sscanf(depth_ptr, " depth %d", &d_val) == 1) {
                engine_depth = d_val;
                redraw_needed = 1;
            }
        }

        char *nodes_ptr = strstr(line, " nodes ");
        if (nodes_ptr) {
            long long n_val;
            if (sscanf(nodes_ptr, " nodes %lld", &n_val) == 1) {
                engine_nodes = n_val;
                redraw_needed = 1;
            }
        }

        char *pv_ptr = strstr(line, " pv ");
        if (pv_ptr) {
            strncpy(engine_pv, pv_ptr + 4, sizeof(engine_pv) - 1);
            engine_pv[sizeof(engine_pv) - 1] = '\0';
            redraw_needed = 1;
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
                redraw_needed = 1;
            }
        }
    }

    if (strncmp(line, "bestmove", 8) == 0) {
        char move_str[16];
        if (sscanf(line, "bestmove %15s", move_str) == 1) {
            if (strcmp(move_str, "(none)") == 0 || strcmp(move_str, "NULL") == 0) {
                engine_thinking = 0;
                redraw_needed = 1;
                return;
            }
            
            Move m = gui_uci_to_move(move_str);
            if (is_legal_move(&current_state, m)) {
                push_state(&current_state, m);
                BoardState next;
                make_move(&current_state, &next, m);
                current_state = next;
            }
            engine_thinking = 0;
            redraw_needed = 1;
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

int find_king(const BoardState *state, int color) {
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == color * 6) return i;
    }
    return -1;
}

int is_square_attacked(const BoardState *state, int sq, int attacker) {
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

int is_pseudo_legal_move(const BoardState *state, Move m) {
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
                            if (!is_square_attacked(state, 60, -1) &&
                                !is_square_attacked(state, 61, -1) &&
                                !is_square_attacked(state, 62, -1)) return 1;
                        }
                    }
                    if (m.to == 58 && (state->castle & 2)) {
                        if (state->board[59] == 0 && state->board[58] == 0 && state->board[57] == 0) {
                            if (!is_square_attacked(state, 60, -1) &&
                                !is_square_attacked(state, 59, -1) &&
                                !is_square_attacked(state, 58, -1)) return 1;
                        }
                    }
                }
                if (turn == -1 && fr == 0 && fc == 4) {
                    if (m.to == 6 && (state->castle & 4)) {
                        if (state->board[5] == 0 && state->board[6] == 0) {
                            if (!is_square_attacked(state, 4, 1) &&
                                !is_square_attacked(state, 5, 1) &&
                                !is_square_attacked(state, 6, 1)) return 1;
                        }
                    }
                    if (m.to == 2 && (state->castle & 8)) {
                        if (state->board[3] == 0 && state->board[2] == 0 && state->board[1] == 0) {
                            if (!is_square_attacked(state, 4, 1) &&
                                !is_square_attacked(state, 3, 1) &&
                                !is_square_attacked(state, 2, 1)) return 1;
                        }
                    }
                }
            }
            return 0;
        }
    }
    return 0;
}

int is_legal_move(const BoardState *state, Move m) {
    if (!is_pseudo_legal_move(state, m)) return 0;
    BoardState next;
    make_move(state, &next, m);
    int king = find_king(&next, state->turn);
    if (king == -1) return 0;
    return !is_square_attacked(&next, king, -state->turn);
}

int has_legal_moves(const BoardState *state) {
    for (int f = 0; f < 64; f++) {
        if (state->board[f] == 0) continue;
        if ((state->turn == 1 && state->board[f] < 0) || (state->turn == -1 && state->board[f] > 0)) continue;
        for (int t = 0; t < 64; t++) {
            Move m = {f, t, 0};
            if (abs(state->board[f]) == 1 && (t / 8 == 0 || t / 8 == 7)) {
                m.promo = 5;
            }
            if (is_legal_move(state, m)) return 1;
        }
    }
    return 0;
}

int count_repetitions(const BoardState *state) {
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

void make_move(const BoardState *src, BoardState *dst, Move m) {
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

// Map coordinates helper
void set_tile(u16* map, int x, int y, u16 tile, u16 palette) {
    if (x >= 0 && x < 32 && y >= 0 && y < 32) {
        map[y * 32 + x] = tile | (palette << 12);
    }
}

// String printing on graphics map helper
void draw_string(u16* map, int x, int y, const char* str, u16 palette) {
    while (*str) {
        set_tile(map, x, y, (u16)(*str), palette);
        x++;
        str++;
    }
}

// Configure DS Palette memory on main display for chess elements matching 3DS color configurations
void init_custom_palettes(void) {
    // Solid background squares configurations
    BG_PALETTE[0 * 16 + 1] = RGB15(27, 22, 17); // Palette 0: Maple Wood Light Square (Tan #48;5;180m representation)
    BG_PALETTE[1 * 16 + 1] = RGB15(17, 12,  0); // Palette 1: Walnut Wood Dark Square (Brown #48;5;94m representation)
    BG_PALETTE[2 * 16 + 1] = RGB15( 0, 17,  0); // Palette 2: Forest Green Selected Square (#48;5;34m representation)
    BG_PALETTE[3 * 16 + 1] = RGB15(31, 16,  0); // Palette 3: Vibrant Orange Cursor Selection (#48;5;208m representation)
    
    // Previous Move Tracer (Light/Dark paths)
    BG_PALETTE[4 * 16 + 1] = RGB15(12, 22, 31); // Palette 4: Sky Blue Prev Move (Light square #48;5;75m)
    BG_PALETTE[5 * 16 + 1] = RGB15(12, 17, 22); // Palette 5: Steel Blue Prev Move (Dark square #48;5;68m)

    // Legal Destination Targets (Light/Dark targets)
    BG_PALETTE[6 * 16 + 1] = RGB15(22, 27, 22); // Palette 6: Pale Sage Green target (Light square #48;5;151m)
    BG_PALETTE[7 * 16 + 1] = RGB15(17, 22, 17); // Palette 7: Muted Sage Green target (Dark square #48;5;108m)

    // King in Check Danger Warning
    BG_PALETTE[8 * 16 + 1] = RGB15(31,  0,  0); // Palette 8: Threat warning red (#48;5;196m representation)

    // Alphanumeric coordinate label text (Muted Slate Grey representation)
    BG_PALETTE[9 * 16 + 1] = RGB15(20, 20, 20); 
    BG_PALETTE[9 * 16 + 15] = RGB15(20, 20, 20);

    // Foreground piece overlay configurations (Color index 0 must be transparent)
    BG_PALETTE[10 * 16 + 0] = 0;
    BG_PALETTE[10 * 16 + 1] = RGB15(31, 31, 31);  // Palette 10: Bright White Pieces (Bold #38;5;255m representation)
    BG_PALETTE[10 * 16 + 15] = RGB15(31, 31, 31);

    BG_PALETTE[11 * 16 + 0] = 0;
    BG_PALETTE[11 * 16 + 1] = RGB15(3,  3,  3);    // Palette 11: Deep Slate Black Pieces (#38;5;232m representation)
    BG_PALETTE[11 * 16 + 15] = RGB15(3,  3,  3);
}

// Overwrite Sub Screen ANSI default console colors with custom palette
void init_bottom_palette(void) {
    BG_PALETTE_SUB[0]  = RGB15(0, 0, 0);       // Black Background
    BG_PALETTE_SUB[1]  = RGB15(22, 4, 4);      // Dark Red
    BG_PALETTE_SUB[2]  = RGB15(4, 22, 4);      // Dark Green
    BG_PALETTE_SUB[3]  = RGB15(22, 22, 4);     // Dark Yellow
    BG_PALETTE_SUB[4]  = RGB15(4, 4, 22);      // Dark Blue
    BG_PALETTE_SUB[5]  = RGB15(22, 4, 22);     // Dark Magenta
    BG_PALETTE_SUB[6]  = RGB15(4, 22, 22);     // Dark Cyan
    BG_PALETTE_SUB[7]  = RGB15(24, 24, 24);    // Light Gray
    BG_PALETTE_SUB[8]  = RGB15(12, 12, 12);    // Intense Dark Gray
    BG_PALETTE_SUB[9]  = RGB15(31, 5, 5);      // High Red
    BG_PALETTE_SUB[10] = RGB15(5, 31, 5);      // High Green
    BG_PALETTE_SUB[11] = RGB15(31, 31, 5);     // High Yellow
    BG_PALETTE_SUB[12] = RGB15(5, 5, 31);      // High Blue
    BG_PALETTE_SUB[13] = RGB15(31, 5, 31);     // High Magenta
    BG_PALETTE_SUB[14] = RGB15(5, 31, 31);     // High Cyan
    BG_PALETTE_SUB[15] = RGB15(31, 31, 31);    // Bright White
}

// Draw the Top Screen Board (Pristine, Direct Hardware Layering, double-height formats)
void draw_top_board(void) {
    // Late binding enforcement: Refresh our custom solid tile at index 255
    u8* tile_memory = (u8*)bgGetGfxPtr(bg_board_id);
    memcpy(tile_memory + (255 * 32), solid_tile, sizeof(solid_tile));

    // Force palette mapping recovery on every single redraw cycle to prevent console wipes
    init_custom_palettes();

    u16* board_map = bgGetMapPtr(bg_board_id);
    u16* pieces_map = bgGetMapPtr(bg_pieces_id);

    // Completely wipe map memory areas
    memset(board_map, 0, 32 * 32 * sizeof(u16));
    memset(pieces_map, 0, 32 * 32 * sizeof(u16));

    // Render coordinate files (columns a-h) at safe margins (now shifted 1 unit left to 7 + col * 2 offset)
    for (int col = 0; col < 8; col++) {
        char file_lbl = (board_orientation == 1) ? ('a' + col) : ('h' - col);
        char str[2] = {file_lbl, '\0'};
        draw_string(pieces_map, 7 + col * 2, 2, str, 9);
        draw_string(pieces_map, 7 + col * 2, 21, str, 9);
    }

    int king_in_check = -1;
    int w_king = find_king(&current_state, 1);
    int b_king = find_king(&current_state, -1);
    if (w_king != -1 && is_square_attacked(&current_state, w_king, -1)) {
        king_in_check = w_king;
    } else if (b_king != -1 && is_square_attacked(&current_state, b_king, 1)) {
        king_in_check = b_king;
    }

    for (int r = 0; r < 8; r++) {
        int rank_lbl = (board_orientation == 1) ? (8 - r) : (r + 1);
        char lbl_str[2] = {'0' + rank_lbl, '\0'};

        // Draw rank labels on sides (shifted 1 unit left to x=5 and x=24)
        draw_string(pieces_map, 5, 4 + r * 2, lbl_str, 9);
        draw_string(pieces_map, 24, 4 + r * 2, lbl_str, 9);

        for (int c = 0; c < 8; c++) {
            int sq = screen_to_board_sq(r, c);
            int p = current_state.board[sq];

            int is_light = ((sq / 8) + (sq % 8)) % 2 == 0;
            int palette_idx = is_light ? 0 : 1; // Default Maple / Walnut Brown

            int is_selected = (sq == selected_sq);
            int is_cursor = (r == cursor_r && c == cursor_c);

            int is_prev_move = 0;
            if (history_count > 0) {
                Move last_move = move_history[history_count - 1];
                if (sq == last_move.from || sq == last_move.to) {
                    is_prev_move = 1;
                }
            }

            int is_legal_dest = 0;
            if (selected_sq != -1) {
                Move test_m = {selected_sq, sq, 0};
                if (abs(current_state.board[selected_sq]) == 1 && (sq / 8 == 0 || sq / 8 == 7)) {
                    test_m.promo = 5;
                }
                if (is_legal_move(&current_state, test_m)) {
                    is_legal_dest = 1;
                }
            }

            // High-fidelity palette assignment mapping matching 3DS color layouts
            if (is_cursor) {
                palette_idx = 3; // Bright Orange
            } else if (is_selected) {
                palette_idx = 2; // Forest Green
            } else if (sq == king_in_check) {
                palette_idx = 8; // Danger Warning Red
            } else if (is_prev_move) {
                palette_idx = is_light ? 4 : 5; // Sky / Steel Blue
            } else if (is_legal_dest) {
                palette_idx = is_light ? 6 : 7; // Sage Light / Dark Greens
            }

            // Render background square cells on BG2 (shifted 1 unit left to 7 + c * 2)
            set_tile(board_map, 7 + c * 2,     4 + r * 2,     255, palette_idx);
            set_tile(board_map, 7 + c * 2 + 1, 4 + r * 2,     255, palette_idx);
            set_tile(board_map, 7 + c * 2,     4 + r * 2 + 1, 255, palette_idx);
            set_tile(board_map, 7 + c * 2 + 1, 4 + r * 2 + 1, 255, palette_idx);

            // Render pieces on high-priority BG1 (shifted 1 unit left to 7 + c * 2)
            if (p != 0) {
                u16 fg_palette = (p > 0) ? 10 : 11; // White is palette 10, Black is palette 11
                char piece_str = ' ';
                switch (abs(p)) {
                    case 1: piece_str = 'P'; break;
                    case 2: piece_str = 'N'; break;
                    case 3: piece_str = 'B'; break;
                    case 4: piece_str = 'R'; break;
                    case 5: piece_str = 'Q'; break;
                    case 6: piece_str = 'K'; break;
                }
                // Draw piece centered in top-left cell quadrant of our 2x2 board square
                set_tile(pieces_map, 7 + c * 2, 4 + r * 2, piece_str, fg_palette);
            }
        }
    }
}

// Draw the Bottom Screen (Hyper-Condensed Layout with Live UCI Console)
void draw_bottom_stats(void) {
    consoleSelect(&bottomConsole);
    printf("\x1b[1;1H"); // Reset printing cursor to top-left of screen

    int king = find_king(&current_state, current_state.turn);
    int is_ch = is_square_attacked(&current_state, king, -current_state.turn);
    int has_mov = has_legal_moves(&current_state);
    int repetitions = count_repetitions(&current_state);

    // --- LINE 1: Turn Status & Player Config (W:Hum B:Eng) ---
    char status_str[64] = "";
    if (engine_state != ENGINE_STATE_READY) {
        strcpy(status_str, "Booting...");
    } else if (current_state.halfmoves >= 100) {
        strcpy(status_str, "\x1b[1;31mDraw (50m-rule)\x1b[0m");
    } else if (repetitions >= 3) {
        strcpy(status_str, "\x1b[1;31mDraw (3-fold)\x1b[0m");
    } else if (!has_mov) {
        if (is_ch) {
            strcpy(status_str, "\x1b[1;31mCHECKMATE!\x1b[0m");
        } else {
            strcpy(status_str, "\x1b[1;36mSTALEMATE!\x1b[0m");
        }
    } else if (is_ch) {
        if (current_state.turn == 1) {
            strcpy(status_str, "\x1b[1;31mW-Check!\x1b[0m");
        } else {
            strcpy(status_str, "\x1b[1;31mB-Check!\x1b[0m");
        }
    } else {
        if (current_state.turn == 1) {
            strcpy(status_str, "\x1b[1;32mWhite's turn\x1b[0m");
        } else {
            strcpy(status_str, "\x1b[1;35mBlack's turn\x1b[0m");
        }
    }

    const char *w_play = (user_side == 1 || user_side == 0) ? "Hum" : "Eng";
    const char *b_play = (user_side == -1 || user_side == 0) ? "Hum" : "Eng";

    printf("%s | W:%s B:%s\x1b[K\n", status_str, w_play, b_play);

    // --- LINE 2: Score, NPS, and Time Limits ---
    char eval_str[16] = "";
    if (engine_score_type == 0) {
        double eval = (double)engine_score_val / 100.0;
        sprintf(eval_str, "%+.2f", eval);
    } else if (engine_score_type == 1) {
        if (engine_score_val > 0) sprintf(eval_str, "+M%d", engine_score_val);
        else sprintf(eval_str, "-M%d", -engine_score_val);
    } else {
        strcpy(eval_str, "----");
    }

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
    } else {
        if (engine_thinking) {
            strcpy(nps_str, "---- nps");
        } else {
            strcpy(nps_str, "Offline");
        }
    }

    char lim_str[24] = "";
    if (time_control_type == 0) {
        sprintf(lim_str, "Lim: %dms", time_control_val);
    } else if (time_control_type == 1) {
        sprintf(lim_str, "Lim: depth %d", time_control_val);
    } else {
        sprintf(lim_str, "Lim: %d nod", time_control_val);
    }

    printf("%s | %s | %s\x1b[K\n", eval_str, nps_str, lim_str);

    // --- LINE 3: Recent Moves Title ---
    printf("\x1b[1;33mRECENT MOVES:\x1b[0m\x1b[K\n");

    // --- LINES 4-13: Move List Display (Scaled to strictly 10 rows / 20 moves) ---
    int total_full_moves = (history_count + 1) / 2;
    int max_visible_moves = 20;
    int half_visible = max_visible_moves / 2; // 10 rows total
    int start_move = (total_full_moves > max_visible_moves) ? (total_full_moves - (max_visible_moves - 1)) : 1;

    for (int r = 0; r < half_visible; r++) {
        int left_display = start_move + r;
        int right_display = start_move + half_visible + r;

        char left_str[32] = "";
        char right_str[32] = "";

        // Render Left Column Move Data
        if (total_full_moves > 0 && left_display <= total_full_moves) {
            int w_idx = (left_display - 1) * 2;
            int b_idx = w_idx + 1;
            char w_str[10] = "-----";
            char b_str[10] = "-----";
            if (w_idx < history_count) {
                // Instantly copy from the pre-calculated cache
                strcpy(w_str, san_history[w_idx]);
            }
            if (b_idx < history_count) {
                // Instantly copy from the pre-calculated cache
                strcpy(b_str, san_history[b_idx]);
            } else if (w_idx < history_count) {
                strcpy(b_str, "...");
            }
            sprintf(left_str, "%2d.%-5s%-5s", left_display, w_str, b_str);
        } else {
            sprintf(left_str, "%2d. ---  --- ", left_display);
        }

        // Render Right Column Move Data
        if (total_full_moves > 0 && right_display <= total_full_moves) {
            int w_idx = (right_display - 1) * 2;
            int b_idx = w_idx + 1;
            char w_str[10] = "-----";
            char b_str[10] = "-----";
            if (w_idx < history_count) {
                // Instantly copy from the pre-calculated cache
                strcpy(w_str, san_history[w_idx]);
            }
            if (b_idx < history_count) {
                // Instantly copy from the pre-calculated cache
                strcpy(b_str, san_history[b_idx]);
            } else if (w_idx < history_count) {
                strcpy(b_str, "...");
            }
            sprintf(right_str, "%2d.%-5s%-5s", right_display, w_str, b_str);
        } else {
            sprintf(right_str, "%2d. ---  --- ", right_display);
        }

        printf(" %s\x1b[1;30m|\x1b[0m%s\x1b[K\n", left_str, right_str);
    }

    // --- LINES 14-23: Real-Time Dynamic Rolling Raw UCI Engine Terminal Console (10 Rows) ---
    // Prints immediately below the moves block, filling empty slots dynamically to keep alignment stable
    for (int i = 0; i < 10; i++) {
        if (i < raw_log_count) {
            // Newest incoming line of console traffic is colored in high-contrast Green
            if (i == raw_log_count - 1) {
                printf("\x1b[1;32m%s\x1b[0m\x1b[K", raw_log[i]);
            } else {
                printf("\x1b[1;30m%s\x1b[0m\x1b[K", raw_log[i]);
            }
        } else {
            printf("\x1b[K"); // Silently clear unoccupied terminal log space
        }
        
        // No trailing newline on the 24th line of the screen to prevent automatic terminal scroll register
        if (i < 9) {
            printf("\n");
        }
    }
    printf("\x1b[J"); // Instantly clear any extra screen leftovers below index 23

    fflush(stdout);
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
        // Optimize loop waiting by yielding to other Calico threads on VBlank
        threadWaitForVBlank();
    }
    redraw_needed = 1;
    return choice;
}

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

    if (!has_legal_moves(&current_state) || current_state.halfmoves >= 100 || count_repetitions(&current_state) >= 3) {
        return;
    }

    int sq = screen_to_board_sq(cursor_r, cursor_c);
    if (selected_sq == -1) {
        int p = current_state.board[sq];
        if (p != 0 && ((current_state.turn == 1 && p > 0) || (current_state.turn == -1 && p < 0))) {
            selected_sq = sq;
        }
    } else {
        Move m = {selected_sq, sq, 0};
        int p = current_state.board[selected_sq];
        int is_promo = (abs(p) == 1 && (sq / 8 == 0 || sq / 8 == 7));
        if (is_promo) m.promo = 5;

        if (is_legal_move(&current_state, m)) {
            if (is_promo) {
                m.promo = get_promo_choice();
            }
            push_state(&current_state, m);
            BoardState next;
            make_move(&current_state, &next, m);
            current_state = next;
            selected_sq = -1;
            
            // engine calculations are preserved on screen instead of being manually cleared here
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
    engine_depth = 0;
    engine_nodes = 0;
    engine_pv[0] = '\0';
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
    engine_depth = 0;
    engine_nodes = 0;
    engine_pv[0] = '\0';
    init_board(&current_state);
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
    engine_depth = 0;
    engine_nodes = 0;
    engine_pv[0] = '\0';
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
        time_control_val = (time_control_val % 20) + 1; // 1 to 20 range
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

void init_board(BoardState *state) {
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

extern int main_stockfish(int argc, char **argv);
void stockfish_thread_func(void* arg) {
    (void)arg;
    char *argv[] = {"stockfish", NULL};
    main_stockfish(1, argv);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    
    // Enable DS Interrupt System
    irqEnable(IRQ_VBLANK);

    // Set 2D Text Modes on both displays
    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);

    // Initialize both standard consoles. 
    // Initializing topConsole on BG3 forces VRAM Bank A to load the default font glyphs into Tile Base 0.
    consoleInit(&topConsole, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&bottomConsole, 3, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);

    // Initialize top custom backgrounds (Sharing Tile Base 0 with loaded console font)
    bg_board_id = bgInit(2, BgType_Text4bpp, BgSize_T_256x256, 29, 0);  // Low Priority: Swatches
    bg_pieces_id = bgInit(1, BgType_Text4bpp, BgSize_T_256x256, 30, 0); // High Priority: Pieces/Labels

    // Initialize Stockfish/handshake bridge
    sf_bridge_init();
    init_board(&current_state);

    // Copy solid color tile array safely to Index 255 of Main Engine VRAM via CPU Memcpy
    u8* tile_memory = (u8*)bgGetGfxPtr(bg_board_id);
    memcpy(tile_memory + (255 * 32), solid_tile, sizeof(solid_tile));

    // Map hardware palette colors after bridge loaded to bypass overwrites
    init_custom_palettes();
    init_bottom_palette();

    // Explicitly force hardware Backgrounds 1, 2, and 3 active on main screen
    REG_DISPCNT |= DISPLAY_BG1_ACTIVE | DISPLAY_BG2_ACTIVE | DISPLAY_BG3_ACTIVE;

    // Clear graphics maps and console text screen maps before starting
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
    
    // Perform early wait using preemptive VBlank engine
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

    // Yield control delay for engine initialization setup
    for (int i = 0; i < 3; i++) {
        ds_yield();
        threadWaitForVBlank();
    }

    // Initiate Phase 1 of Handshake
    sf_send_command("uci");
    engine_state = ENGINE_STATE_WAIT_UCIOK;

    redraw_needed = 1;

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
            input_detected = 1;
        }
        if (kDown & KEY_R) {
            adjust_time_control();
            input_detected = 1;
        }

        if (input_detected) {
            redraw_needed = 1;
        }

        int engine_active = 0;
        if (engine_state == ENGINE_STATE_READY && has_legal_moves(&current_state) && current_state.halfmoves < 100 && count_repetitions(&current_state) < 3) {
            if (user_side == 2) engine_active = 1;
            else if (user_side == 1 && current_state.turn == -1) engine_active = 1;
            else if (user_side == -1 && current_state.turn == 1) engine_active = 1;
        }

        if (engine_active && !engine_thinking) {
            engine_thinking = 1;
            trigger_engine_move();
        }

        read_from_engine();

        // Only redraw UI when needed
        if (redraw_needed) {
            draw_ui();
            redraw_needed = 0;
        }

        // IMPORTANT OPTIMIZATION: threadWaitForVBlank() yields CPU back to Stockfish 
        // asynchronously, giving the engine maximum processing power while the GUI is idle.
        threadWaitForVBlank();
    }

    sf_send_command("quit");
    return 0;
}
