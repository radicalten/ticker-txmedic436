#define IS_GUI // Tells 3ds_bridge.h to let this file write directly to the screens
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "3ds_bridge.h"

#define MAX_HISTORY 2048
#define ENGINE_STACK_SIZE (1024 * 1024) // 1MB formerly 256*1024

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
int history_count = 0;

int cursor_r = 6;
int cursor_c = 4;
int selected_sq = -1;

int board_orientation = 1; 
int user_side = 1;         

int time_control_type = 0; 
int time_control_val = 1;  // Default matched to 1ms

int engine_thinking = 0;
long long engine_nps = 0;
int engine_score_type = -1; 
int engine_score_val = 0;

PrintConsole topConsole, bottomConsole;

void init_board(BoardState *state);
int is_legal_move(const BoardState *state, Move m);
int has_legal_moves(const BoardState *state);
int is_square_attacked(const BoardState *state, int sq, int attacker);
void make_move(const BoardState *src, BoardState *dst, Move m);
void print_side_panel_line(int panel_row);
void print_recent_moves(int row);
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

void push_state(const BoardState *state, Move m) {
    if (history_count < MAX_HISTORY - 1) {
        history[history_count] = *state;
        move_history[history_count] = m;
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
}

void process_engine_output(char *line) {
    int len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
        line[len - 1] = '\0';
        len--;
    }

    consoleSelect(&bottomConsole);
    printf("%s\n", line);
    fflush(stdout);

    if (engine_state == ENGINE_STATE_WAIT_UCIOK) {
        if (strstr(line, "uciok") != NULL) {
            sf_send_command("isready");
            engine_state = ENGINE_STATE_WAIT_READYOK;
            printf("[GUI] Received 'uciok' -> Sending 'isready'\n");
            fflush(stdout);
        }
    } else if (engine_state == ENGINE_STATE_WAIT_READYOK) {
        if (strstr(line, "readyok") != NULL) {
            sf_send_command("setoption name Hash value 16"); 
            
            sf_send_command("ucinewgame");
            engine_state = ENGINE_STATE_READY;
            printf("[GUI] Received 'readyok' -> Handshake Complete.\n");
            fflush(stdout);
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
            
            Move m = gui_uci_to_move(move_str);
            if (is_legal_move(&current_state, m)) {
                push_state(&current_state, m);
                BoardState next;
                make_move(&current_state, &next, m);
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

// GUI Drawing and 3DS Screen Output matching Visual Theme of Desktop CLI
void draw_ui(void) {
    consoleSelect(&topConsole);
    printf("\x1b[1;1H"); // Flick-free coordinates frame reset

    // 1. Status Header Layout (Status, Player Sides, Time Parameters)
    const char *turn_str = (current_state.turn == 1) ? "\x1b[1;33mWhite\x1b[0m" : "\x1b[1;35mBlack\x1b[0m";
    int king = find_king(&current_state, current_state.turn);
    int is_ch = is_square_attacked(&current_state, king, -current_state.turn);
    int has_mov = has_legal_moves(&current_state);
    const char *w_play = (user_side == 1 || user_side == 0) ? "Hum" : "Eng";
    const char *b_play = (user_side == -1 || user_side == 0) ? "Hum" : "Eng";
    int repetitions = count_repetitions(&current_state);

    printf("  ");
    if (current_state.halfmoves >= 100) {
        printf("\x1b[1;36mDRAW (50-move rule)\x1b[0m");
    } else if (repetitions >= 3) {
        printf("\x1b[1;36mDRAW (3-fold rep.)\x1b[0m");
    } else if (!has_mov) {
        if (is_ch) printf("\x1b[1;31mCHECKMATE!\x1b[0m");
        else printf("\x1b[1;36mSTALEMATE!\x1b[0m");
    } else if (is_ch) {
        printf("%s (\x1b[1;31mCHECK!\x1b[0m)", turn_str);
    } else {
        printf("%s's Turn", turn_str);
    }
    printf(" | W:%s B:%s", w_play, b_play);

    const char *types[] = {"Time", "Depth", "Nodes"};
    printf(" | %s", types[time_control_type]);
    if (time_control_type == 0) {
        printf(" (%d ms)", time_control_val);
    } else if (time_control_type == 1) {
        printf(" (depth %d)", time_control_val);
    } else {
        printf(" (%d nodes)", time_control_val);
    }
    printf("\x1b[K\n\n");

    // 2. Top Rank Labels: Perfectly aligned at column 32 with 3-character horizontal spacing
    if (board_orientation == 1) {
        printf("     a  b  c  d  e  f  g  h    ");
    } else {
        printf("     h  g  f  e  d  c  b  a    ");
    }
    print_side_panel_line(0);
    printf("\x1b[K\n");

    int king_in_check = -1;
    int w_king = find_king(&current_state, 1);
    int b_king = find_king(&current_state, -1);
    if (w_king != -1 && is_square_attacked(&current_state, w_king, -1)) {
        king_in_check = w_king;
    } else if (b_king != -1 && is_square_attacked(&current_state, b_king, 1)) {
        king_in_check = b_king;
    }

    // 3. Render 8 Board Ranks: Center-aligned 3-character wide squares (" %s ")
    for (int r = 0; r < 8; r++) {
        int rank_lbl = (board_orientation == 1) ? (8 - r) : (r + 1);
        printf("  %d ", rank_lbl);

        for (int c = 0; c < 8; c++) {
            int sq = screen_to_board_sq(r, c);
            int p = current_state.board[sq];

            int is_light = ((sq / 8) + (sq % 8)) % 2 == 0;
            const char *bg_color;

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

            // CLI terminal ANSI colors
            if (is_cursor) {
                bg_color = "\x1b[48;5;208m"; // Bright orange cursor
            } else if (is_selected) {
                bg_color = "\x1b[48;5;34m";  // Forest Green selection
            } else if (sq == king_in_check) {
                bg_color = "\x1b[48;5;196m"; // Danger warnings red
            } else if (is_prev_move) {
                bg_color = is_light ? "\x1b[48;5;75m" : "\x1b[48;5;68m"; // Sky/Steel Blue history path
            } else if (is_legal_dest) {
                bg_color = is_light ? "\x1b[48;5;151m" : "\x1b[48;5;108m"; // Light green targets
            } else {
                bg_color = is_light ? "\x1b[48;5;180m" : "\x1b[48;5;94m"; // Maple/Walnut patterns
            }

            const char *piece_str = " ";
            const char *fg_color = "\x1b[38;5;232m"; // Dark pieces
            if (p != 0) {
                if (p > 0) {
                    fg_color = "\x1b[38;5;255m\x1b[1m"; // Bold White pieces
                }
                switch (abs(p)) {
                    case 1: piece_str = "P"; break;
                    case 2: piece_str = "N"; break;
                    case 3: piece_str = "B"; break;
                    case 4: piece_str = "R"; break;
                    case 5: piece_str = "Q"; break;
                    case 6: piece_str = "K"; break;
                }
            }
            // " %s " yields perfect horizontal centering within the 3-character wide block
            printf("%s%s %s \x1b[0m", bg_color, fg_color, piece_str);
        }

        printf(" %d ", rank_lbl);
        print_side_panel_line(r + 1);
        printf("\x1b[K\n");
    }

    // 4. Bottom Rank Labels: Perfectly aligned with the 3-character horizontally centered board
    if (board_orientation == 1) {
        printf("     a  b  c  d  e  f  g  h    ");
    } else {
        printf("     h  g  f  e  d  c  b  a    ");
    }
    print_side_panel_line(9);
    printf("\x1b[K\n\n");

    // Dynamic Engine metric parsing output block
    printf(" \x1b[38;5;248m\x1b[0m (%s)", (engine_state == ENGINE_STATE_READY) ? "\x1b[1;32mActive\x1b[0m" : "\x1b[1;31mConfiguring\x1b[0m");
    if (engine_state == ENGINE_STATE_READY) {
        if (engine_nps > 0) {
            if (engine_nps >= 1000000) {
                printf(" | NPS: %.2fM", (double)engine_nps / 1000000.0);
            } else if (engine_nps >= 1000) {
                printf(" | NPS: %.1fk", (double)engine_nps / 1000.0);
            } else {
                printf(" | NPS: %lld", engine_nps);
            }
        }
        if (engine_score_type == 0) {
            printf(" | \x1b[1;36mEval:\x1b[0m %+.2f", (double)engine_score_val / 100.0);
        } else if (engine_score_type == 1) {
            if (engine_score_val > 0) {
                printf(" | \x1b[1;31mEval: +M%d\x1b[0m", engine_score_val);
            } else if (engine_score_val < 0) {
                printf(" | \x1b[1;31mEval: -M%d\x1b[0m", -engine_score_val);
            } else {
                printf(" | \x1b[1;31mEval: M0\x1b[0m");
            }
        }
    }
    printf("\x1b[K\n");

    fflush(stdout); 
    consoleSelect(&bottomConsole);
}

void print_side_panel_line(int panel_row) {
    print_recent_moves(panel_row);
}

void print_recent_moves(int row) {
    int total_full_moves = (history_count + 1) / 2;
    if (total_full_moves == 0) return;
    int start_move = 1;
    if (total_full_moves > 10) {
        start_move = total_full_moves - 9;
    }
    int display = start_move + row;
    if (display > total_full_moves) return;

    int w_idx = (display - 1) * 2;
    int b_idx = w_idx + 1;

    // Fixed compact PGN column spacing to prevent wrapping and preserve formatting
    printf(" %2d.", display);
    if (w_idx < history_count) {
        char w_str[10];
        move_to_uci(move_history[w_idx], w_str);
        printf(" %-5s", w_str);
    } else {
        printf(" -----");
    }

    if (b_idx < history_count) {
        char b_str[10];
        move_to_uci(move_history[b_idx], b_str);
        printf(" %-5s", b_str);
    } else {
        if (w_idx < history_count) printf(" ...");
        else printf(" -----");
    }
}

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
    if (user_side == 1) user_side = -1;
    else if (user_side == -1) user_side = 0;
    else if (user_side == 0) user_side = 2;
    else user_side = 1;
}

// Ported dynamic lists from Desktop CLI adjusting parameters safely
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
    char *argv[] = {"stockfish", NULL};
    main_stockfish(1, argv);
}

int main(int argc, char **argv) {
    gfxInitDefault();
    
    consoleInit(GFX_TOP, &topConsole);
    consoleInit(GFX_BOTTOM, &bottomConsole);

    sf_bridge_init();
    init_board(&current_state);

    // Initial Loading screen on Top screen
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

    Thread stockfish_thread;
    s32 prio = 0x3B; 
    stockfish_thread = threadCreate(stockfish_thread_func, NULL, ENGINE_STACK_SIZE, prio, -1, false);

    if (stockfish_thread == NULL) {
        consoleSelect(&bottomConsole);
        printf("\x1b[1;31m[ERROR] Failed to spawn background Stockfish thread!\x1b[0m\n");
        fflush(stdout);
    } else {
        consoleSelect(&bottomConsole);
        printf("Background Engine Thread spawned successfully on Safe Core.\n\n");
        fflush(stdout);
    }

    svcSleepThread(50000000ULL); // 50ms

    // Initiate Phase 1 of Handshake
    sf_send_command("uci");
    engine_state = ENGINE_STATE_WAIT_UCIOK;

    while (aptMainLoop()) {
        hidScanInput();
        u32 kDown = hidKeysDown();

        if (kDown & KEY_START) break; 

        if (kDown & KEY_UP)    { if (cursor_r > 0) cursor_r--; }
        if (kDown & KEY_DOWN)  { if (cursor_r < 7) cursor_r++; }
        if (kDown & KEY_RIGHT) { if (cursor_c < 7) cursor_c++; }
        if (kDown & KEY_LEFT)  { if (cursor_c > 0) cursor_c--; }

        if (kDown & KEY_A)      handle_select();
        if (kDown & KEY_B)      handle_undo();
        if (kDown & KEY_SELECT) handle_reset_board();
        if (kDown & KEY_X)      board_orientation = -board_orientation;
        if (kDown & KEY_Y)      handle_switch_sides();

        if (kDown & KEY_L) {
            time_control_type = (time_control_type + 1) % 3;
            time_control_val = (time_control_type == 0) ? 1 : (time_control_type == 1 ? 1 : 512);
        }
        if (kDown & KEY_R) {
            adjust_time_control();
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
        draw_ui();

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
