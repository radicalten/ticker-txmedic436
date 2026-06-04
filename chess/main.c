#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/select.h>
#include <curses.h>
#include <signal.h>
#include <locale.h>

// Piece representations
#define EMPTY 0
#define PAWN 1
#define KNIGHT 2
#define BISHOP 3
#define ROOK 4
#define QUEEN 5
#define KING 6

#define WHITE 8
#define BLACK 16

#define COLOR(p) ((p) & 24)
#define TYPE(p) ((p) & 7)

// Color Pair Identifiers
#define CP_LIGHT_SQ 1
#define CP_DARK_SQ 2
#define CP_LIGHT_SEL 3
#define CP_DARK_SEL 4
#define CP_LIGHT_HL 5
#define CP_DARK_HL 6
#define CP_SIDEBAR 7

typedef struct {
    int from;
    int to;
    int promotion;
} Move;

typedef struct {
    unsigned char board[64];
    int turn;       // WHITE or BLACK
    int castling;   // Bitmask: 1: WK, 2: WQ, 4: BK, 8: BQ
    int ep;         // En passant square index (-1 if none)
    int halfmove;
    int fullmove;
} BoardState;

typedef struct {
    Move m;
    char san[16];
} PlayedMove;

typedef struct {
    char engine_path[256];
    int depth;
    int nodes;
    int movetime_ms;
    int engine_active; // 0: PvP, 1: Engine is Black, 2: Engine is White
} Settings;

// Global game state
BoardState game_state;
PlayedMove move_history[1024];
int move_history_count = 0;
BoardState state_history[1024];
int state_history_count = 0;

Settings app_settings = {
    "/opt/homebrew/bin/stockfish", // Default Apple Silicon Homebrew Stockfish path
    10,                            // Search Depth
    0,                             // Nodes limit (0 = disable)
    1000,                          // Time limit in ms
    1                              // Engine plays Black by default
};

int engine_pid = -1;
int to_engine[2];   // Pipe to engine stdin
int from_engine[2]; // Pipe from engine stdout
int engine_thinking = 0;
char engine_io_buf[8192];
int engine_io_pos = 0;
char last_engine_info[256] = "Engine idle";

// Fallback ASCII pieces used if UTF-8 environment is not initialized
const char *piece_symbols[25] = {
    [EMPTY] = "   ",
    [WHITE | PAWN]   = " ♙ ", [WHITE | KNIGHT] = " ♘ ", [WHITE | BISHOP] = " ♗ ", 
    [WHITE | ROOK]   = " ♖ ", [WHITE | QUEEN]  = " ♕ ", [WHITE | KING]   = " ♔ ",
    [BLACK | PAWN]   = " ♟ ", [BLACK | KNIGHT] = " ♞ ", [BLACK | BISHOP] = " ♝ ", 
    [BLACK | ROOK]   = " ♜ ", [BLACK | QUEEN]  = " ♛ ", [BLACK | KING]   = " ♚ "
};

int sq(int r, int c) { return r * 8 + c; }

// Forward declarations
int is_square_attacked(const BoardState *state, int sq, int attacker_color);
int generate_legal_moves(const BoardState *state, Move *legal_moves);
void apply_move(BoardState *state, Move m);
void stop_engine();
int start_engine(const char *path);

void init_board(BoardState *state) {
    memset(state->board, EMPTY, sizeof(state->board));
    int back_rank[] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    for (int c = 0; c < 8; c++) {
        state->board[sq(0, c)] = BLACK | back_rank[c];
        state->board[sq(7, c)] = WHITE | back_rank[c];
    }
    for (int c = 0; c < 8; c++) {
        state->board[sq(1, c)] = BLACK | PAWN;
        state->board[sq(6, c)] = WHITE | PAWN;
    }
    state->turn = WHITE;
    state->castling = 15;
    state->ep = -1;
    state->halfmove = 0;
    state->fullmove = 1;
}

int is_in_check(const BoardState *state, int color) {
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == (color | KING)) {
            king_sq = i;
            break;
        }
    }
    if (king_sq == -1) return 0;
    return is_square_attacked(state, king_sq, color == WHITE ? BLACK : WHITE);
}

int is_square_attacked(const BoardState *state, int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;
    if (attacker_color == WHITE) {
        if (r + 1 < 8) {
            if (c - 1 >= 0 && state->board[sq + 7] == (WHITE | PAWN)) return 1;
            if (c + 1 < 8  && state->board[sq + 9] == (WHITE | PAWN)) return 1;
        }
    } else {
        if (r - 1 >= 0) {
            if (c - 1 >= 0 && state->board[sq - 9] == (BLACK | PAWN)) return 1;
            if (c + 1 < 8  && state->board[sq - 7] == (BLACK | PAWN)) return 1;
        }
    }

    int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2}, kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_r[i], nc = c + kn_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == (attacker_color | KNIGHT)) return 1;
        }
    }

    int ki_r[] = {-1, -1, -1, 0, 0, 1, 1, 1}, ki_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + ki_r[i], nc = c + ki_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == (attacker_color | KING)) return 1;
        }
    }

    int diag_r[] = {-1, -1, 1, 1}, diag_c[] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += diag_r[i]; nc += diag_c[i];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            int p = state->board[nr * 8 + nc];
            if (p != EMPTY) {
                if (COLOR(p) == attacker_color && (TYPE(p) == BISHOP || TYPE(p) == QUEEN)) return 1;
                break;
            }
        }
    }

    int orth_r[] = {-1, 1, 0, 0}, orth_c[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += orth_r[i]; nc += orth_c[i];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            int p = state->board[nr * 8 + nc];
            if (p != EMPTY) {
                if (COLOR(p) == attacker_color && (TYPE(p) == ROOK || TYPE(p) == QUEEN)) return 1;
                break;
            }
        }
    }
    return 0;
}

int generate_moves_for_sq(const BoardState *state, int sq, Move *moves) {
    int count = 0, p = state->board[sq];
    if (p == EMPTY || COLOR(p) != state->turn) return 0;
    int r = sq / 8, c = sq % 8, type = TYPE(p), color = COLOR(p), opp_color = (color == WHITE) ? BLACK : WHITE;

    if (type == PAWN) {
        int dir = (color == WHITE) ? -1 : 1;
        int start_row = (color == WHITE) ? 6 : 1;
        int promo_row = (color == WHITE) ? 0 : 7;
        int next_r = r + dir;
        if (next_r >= 0 && next_r < 8) {
            int dest = next_r * 8 + c;
            if (state->board[dest] == EMPTY) {
                if (next_r == promo_row) {
                    moves[count++] = (Move){sq, dest, QUEEN};
                    moves[count++] = (Move){sq, dest, ROOK};
                    moves[count++] = (Move){sq, dest, BISHOP};
                    moves[count++] = (Move){sq, dest, KNIGHT};
                } else {
                    moves[count++] = (Move){sq, dest, EMPTY};
                }
                if (r == start_row && state->board[(r + 2 * dir) * 8 + c] == EMPTY) {
                    moves[count++] = (Move){sq, (r + 2 * dir) * 8 + c, EMPTY};
                }
            }
        }
        int cap_cols[] = {c - 1, c + 1};
        for (int i = 0; i < 2; i++) {
            int nc = cap_cols[i];
            if (nc >= 0 && nc < 8) {
                int dest = next_r * 8 + nc;
                if ((state->board[dest] != EMPTY && COLOR(state->board[dest]) == opp_color) || dest == state->ep) {
                    if (next_r == promo_row) {
                        moves[count++] = (Move){sq, dest, QUEEN};
                        moves[count++] = (Move){sq, dest, ROOK};
                    } else {
                        moves[count++] = (Move){sq, dest, EMPTY};
                    }
                }
            }
        }
    } else if (type == KNIGHT) {
        int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2}, kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
        for (int i = 0; i < 8; i++) {
            int nr = r + kn_r[i], nc = c + kn_c[i];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int dest = nr * 8 + nc;
                if (state->board[dest] == EMPTY || COLOR(state->board[dest]) == opp_color) moves[count++] = (Move){sq, dest, EMPTY};
            }
        }
    } else if (type == BISHOP || type == QUEEN) {
        int diag_r[] = {-1, -1, 1, 1}, diag_c[] = {-1, 1, -1, 1};
        for (int i = 0; i < 4; i++) {
            int nr = r, nc = c;
            while (1) {
                nr += diag_r[i]; nc += diag_c[i];
                if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
                int dest = nr * 8 + nc;
                if (state->board[dest] == EMPTY) {
                    moves[count++] = (Move){sq, dest, EMPTY};
                } else {
                    if (COLOR(state->board[dest]) == opp_color) moves[count++] = (Move){sq, dest, EMPTY};
                    break;
                }
            }
        }
    }
    if (type == ROOK || type == QUEEN) {
        int orth_r[] = {-1, 1, 0, 0}, orth_c[] = {0, 0, -1, 1};
        for (int i = 0; i < 4; i++) {
            int nr = r, nc = c;
            while (1) {
                nr += orth_r[i]; nc += orth_c[i];
                if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
                int dest = nr * 8 + nc;
                if (state->board[dest] == EMPTY) {
                    moves[count++] = (Move){sq, dest, EMPTY};
                } else {
                    if (COLOR(state->board[dest]) == opp_color) moves[count++] = (Move){sq, dest, EMPTY};
                    break;
                }
            }
        }
    } else if (type == KING) {
        int ki_r[] = {-1, -1, -1, 0, 0, 1, 1, 1}, ki_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
        for (int i = 0; i < 8; i++) {
            int nr = r + ki_r[i], nc = c + ki_c[i];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int dest = nr * 8 + nc;
                if (state->board[dest] == EMPTY || COLOR(state->board[dest]) == opp_color) moves[count++] = (Move){sq, dest, EMPTY};
            }
        }
        if (color == WHITE) {
            if ((state->castling & 1) && state->board[61] == EMPTY && state->board[62] == EMPTY) {
                if (!is_square_attacked(state, 60, opp_color) && !is_square_attacked(state, 61, opp_color) && !is_square_attacked(state, 62, opp_color))
                    moves[count++] = (Move){60, 62, EMPTY};
            }
            if ((state->castling & 2) && state->board[59] == EMPTY && state->board[58] == EMPTY && state->board[57] == EMPTY) {
                if (!is_square_attacked(state, 60, opp_color) && !is_square_attacked(state, 59, opp_color) && !is_square_attacked(state, 58, opp_color))
                    moves[count++] = (Move){60, 58, EMPTY};
            }
        } else {
            if ((state->castling & 4) && state->board[5] == EMPTY && state->board[6] == EMPTY) {
                if (!is_square_attacked(state, 4, opp_color) && !is_square_attacked(state, 5, opp_color) && !is_square_attacked(state, 6, opp_color))
                    moves[count++] = (Move){4, 6, EMPTY};
            }
            if ((state->castling & 8) && state->board[3] == EMPTY && state->board[2] == EMPTY && state->board[1] == EMPTY) {
                if (!is_square_attacked(state, 4, opp_color) && !is_square_attacked(state, 3, opp_color) && !is_square_attacked(state, 2, opp_color))
                    moves[count++] = (Move){4, 2, EMPTY};
            }
        }
    }
    return count;
}

int is_move_legal(const BoardState *state, Move m) {
    BoardState temp = *state;
    int piece = temp.board[m.from];
    int color = COLOR(piece);
    if (TYPE(piece) == PAWN && m.to == temp.ep) {
        temp.board[color == WHITE ? m.to + 8 : m.to - 8] = EMPTY;
    }
    temp.board[m.to] = m.promotion ? (color | m.promotion) : piece;
    temp.board[m.from] = EMPTY;

    if (TYPE(piece) == KING) {
        if (m.from == 60 && m.to == 62) { temp.board[61] = temp.board[63]; temp.board[63] = EMPTY; }
        else if (m.from == 60 && m.to == 58) { temp.board[59] = temp.board[56]; temp.board[56] = EMPTY; }
        else if (m.from == 4 && m.to == 6) { temp.board[5] = temp.board[7]; temp.board[7] = EMPTY; }
        else if (m.from == 4 && m.to == 2) { temp.board[3] = temp.board[0]; temp.board[0] = EMPTY; }
    }
    return !is_in_check(&temp, color);
}

int generate_legal_moves(const BoardState *state, Move *legal_moves) {
    Move pseudo[256];
    int pseudo_count = 0;
    for (int sq = 0; sq < 64; sq++) {
        if (COLOR(state->board[sq]) == state->turn) {
            pseudo_count += generate_moves_for_sq(state, sq, pseudo + pseudo_count);
        }
    }
    int legal_count = 0;
    for (int i = 0; i < pseudo_count; i++) {
        if (is_move_legal(state, pseudo[i])) {
            legal_moves[legal_count++] = pseudo[i];
        }
    }
    return legal_count;
}

void apply_move(BoardState *state, Move m) {
    int piece = state->board[m.from];
    int color = COLOR(piece);
    int opp_color = color == WHITE ? BLACK : WHITE;
    int next_ep = -1;

    if (TYPE(piece) == PAWN) {
        if (abs(m.to - m.from) == 16) next_ep = (m.from + m.to) / 2;
        if (m.to == state->ep) state->board[color == WHITE ? m.to + 8 : m.to - 8] = EMPTY;
    }

    state->board[m.to] = m.promotion ? (color | m.promotion) : piece;
    state->board[m.from] = EMPTY;

    if (TYPE(piece) == KING) {
        if (m.from == 60 && m.to == 62) { state->board[61] = state->board[63]; state->board[63] = EMPTY; }
        else if (m.from == 60 && m.to == 58) { state->board[59] = state->board[56]; state->board[56] = EMPTY; }
        else if (m.from == 4 && m.to == 6) { state->board[5] = state->board[7]; state->board[7] = EMPTY; }
        else if (m.from == 4 && m.to == 2) { state->board[3] = state->board[0]; state->board[0] = EMPTY; }
        if (color == WHITE) state->castling &= ~3; else state->castling &= ~12;
    }

    if (m.from == 56 || m.to == 56) state->castling &= ~2;
    if (m.from == 63 || m.to == 63) state->castling &= ~1;
    if (m.from == 0 || m.to == 0)   state->castling &= ~8;
    if (m.from == 7 || m.to == 7)   state->castling &= ~4;

    state->ep = next_ep;
    state->turn = opp_color;
    if (color == BLACK) state->fullmove++;
}

void move_to_string(Move m, char *str) {
    int f_col = m.from % 8, f_row = 8 - (m.from / 8);
    int t_col = m.to % 8, t_row = 8 - (m.to / 8);
    if (m.promotion) {
        char p = 'q';
        if (m.promotion == KNIGHT) p = 'n';
        if (m.promotion == BISHOP) p = 'b';
        if (m.promotion == ROOK) p = 'r';
        sprintf(str, "%c%d%c%d%c", 'a' + f_col, f_row, 'a' + t_col, t_row, p);
    } else {
        sprintf(str, "%c%d%c%d", 'a' + f_col, f_row, 'a' + t_col, t_row);
    }
}

Move parse_move_string(const BoardState *state, const char *str) {
    Move m = {0, 0, 0};
    if (strlen(str) < 4) return m;
    m.from = (8 - (str[1] - '0')) * 8 + (str[0] - 'a');
    m.to = (8 - (str[3] - '0')) * 8 + (str[2] - 'a');
    if (strlen(str) == 5) {
        if (str[4] == 'q') m.promotion = QUEEN;
        else if (str[4] == 'r') m.promotion = ROOK;
        else if (str[4] == 'b') m.promotion = BISHOP;
        else if (str[4] == 'n') m.promotion = KNIGHT;
    }
    return m;
}

void get_san(const BoardState *state, Move m, char *san) {
    int piece = state->board[m.from];
    int type = TYPE(piece);
    if (type == KING) {
        if (m.from == 60 && m.to == 62) { strcpy(san, "O-O"); return; }
        if (m.from == 60 && m.to == 58) { strcpy(san, "O-O-O"); return; }
        if (m.from == 4 && m.to == 6) { strcpy(san, "O-O"); return; }
        if (m.from == 4 && m.to == 2) { strcpy(san, "O-O-O"); return; }
    }
    int is_cap = (state->board[m.to] != EMPTY) || (type == PAWN && m.to == state->ep);
    int pos = 0;
    if (type == PAWN) {
        if (is_cap) {
            san[pos++] = 'a' + (m.from % 8);
            san[pos++] = 'x';
        }
    } else {
        char p_chars[] = {'?', 'P', 'N', 'B', 'R', 'Q', 'K'};
        san[pos++] = p_chars[type];
        Move legal[256];
        int l_count = generate_legal_moves(state, legal), df = 0, dr = 0, amb = 0;
        for (int i = 0; i < l_count; i++) {
            if (legal[i].from != m.from && legal[i].to == m.to && TYPE(state->board[legal[i].from]) == type) {
                amb = 1;
                if (legal[i].from % 8 == m.from % 8) df = 1;
                if (legal[i].from / 8 == m.from / 8) dr = 1;
            }
        }
        if (amb) {
            if (!df) san[pos++] = 'a' + (m.from % 8);
            else if (!dr) san[pos++] = '8' - (m.from / 8);
            else {
                san[pos++] = 'a' + (m.from % 8);
                san[pos++] = '8' - (m.from / 8);
            }
        }
        if (is_cap) san[pos++] = 'x';
    }
    san[pos++] = 'a' + (m.to % 8);
    san[pos++] = '8' - (m.to / 8);
    if (m.promotion) {
        san[pos++] = '=';
        char p_chars[] = {'?', 'P', 'N', 'B', 'R', 'Q', 'K'};
        san[pos++] = p_chars[m.promotion];
    }
    BoardState temp = *state;
    apply_move(&temp, m);
    if (is_in_check(&temp, state->turn == WHITE ? BLACK : WHITE)) {
        Move next_lvl[256];
        san[pos++] = (generate_legal_moves(&temp, next_lvl) == 0) ? '#' : '+';
    }
    san[pos] = '\0';
}

void stop_engine() {
    if (engine_pid != -1) {
        kill(engine_pid, SIGTERM);
        close(to_engine[1]);
        close(from_engine[0]);
        engine_pid = -1;
    }
}

int start_engine(const char *path) {
    stop_engine();
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) return 0;
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[1]); close(from_engine[0]);
        char *args[] = {(char *)path, NULL};
        execv(path, args);
        exit(1);
    }
    close(to_engine[0]); close(from_engine[1]);
    fcntl(from_engine[0], F_SETFL, fcntl(from_engine[0], F_GETFL, 0) | O_NONBLOCK);
    write(to_engine[1], "uci\n", 4);
    return 1;
}

void send_engine(const char *cmd) {
    if (engine_pid != -1) {
        write(to_engine[1], cmd, strlen(cmd));
        write(to_engine[1], "\n", 1);
    }
}

void trigger_engine_search() {
    char cmd[8192] = "position startpos moves";
    for (int i = 0; i < move_history_count; i++) {
        char m_str[10];
        move_to_string(move_history[i].m, m_str);
        strcat(cmd, " ");
        strcat(cmd, m_str);
    }
    send_engine(cmd);
    char go[128] = "go";
    if (app_settings.depth > 0) sprintf(go + strlen(go), " depth %d", app_settings.depth);
    if (app_settings.nodes > 0) sprintf(go + strlen(go), " nodes %d", app_settings.nodes);
    if (app_settings.movetime_ms > 0) sprintf(go + strlen(go), " movetime %d", app_settings.movetime_ms);
    send_engine(go);
    engine_thinking = 1;
}

void process_engine_line(const char *line) {
    if (strncmp(line, "info", 4) == 0) {
        strncpy(last_engine_info, line, sizeof(last_engine_info) - 1);
    } else if (strncmp(line, "bestmove ", 9) == 0) {
        char mv_str[10];
        sscanf(line, "bestmove %s", mv_str);
        if (strcmp(mv_str, "(none)") != 0 && strcmp(mv_str, "NULL") != 0) {
            Move m = parse_move_string(&game_state, mv_str);
            Move legal[256];
            int l_count = generate_legal_moves(&game_state, legal);
            for (int i = 0; i < l_count; i++) {
                if (legal[i].from == m.from && legal[i].to == m.to && (m.promotion == EMPTY || legal[i].promotion == m.promotion)) {
                    char san[16];
                    get_san(&game_state, legal[i], san);
                    state_history[state_history_count++] = game_state;
                    apply_move(&game_state, legal[i]);
                    move_history[move_history_count].m = legal[i];
                    strcpy(move_history[move_history_count].san, san);
                    move_history_count++;
                    break;
                }
            }
        }
        engine_thinking = 0;
    }
}

void update_engine_io() {
    char temp[1024];
    int n = read(from_engine[0], temp, sizeof(temp) - 1);
    if (n > 0) {
        temp[n] = '\0';
        for (int i = 0; i < n; i++) {
            if (temp[i] == '\n' || temp[i] == '\r') {
                if (engine_io_pos > 0) {
                    engine_io_buf[engine_io_pos] = '\0';
                    process_engine_line(engine_io_buf);
                    engine_io_pos = 0;
                }
            } else if (engine_io_pos < (int)sizeof(engine_io_buf) - 2) {
                engine_io_buf[engine_io_pos++] = temp[i];
            }
        }
    }
}

void draw_board(int cursor_r, int cursor_c, int sel_sq) {
    int start_y = 3, start_x = 4;
    for (int r = 0; r < 8; r++) {
        mvprintw(start_y + r * 2, start_x - 3, "%d", 8 - r);
    }
    for (int c = 0; c < 8; c++) {
        mvprintw(start_y + 16, start_x + c * 5 + 2, "%c", 'A' + c);
    }
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int current_sq = r * 8 + c;
            int piece = game_state.board[current_sq];
            int is_hl = 0;
            if (sel_sq != -1) {
                Move legal[256];
                int l_count = generate_legal_moves(&game_state, legal);
                for (int i = 0; i < l_count; i++) {
                    if (legal[i].from == sel_sq && legal[i].to == current_sq) {
                        is_hl = 1; break;
                    }
                }
            }
            int pair;
            int is_dark = (r + c) % 2;
            if (current_sq == sel_sq) pair = is_dark ? CP_DARK_SEL : CP_LIGHT_SEL;
            else if (is_hl) pair = is_dark ? CP_DARK_HL : CP_LIGHT_HL;
            else pair = is_dark ? CP_DARK_SQ : CP_LIGHT_SQ;

            attron(COLOR_PAIR(pair));
            if (r == cursor_r && c == cursor_c) attron(A_REVERSE);
            mvprintw(start_y + r * 2,     start_x + c * 5, "     ");
            mvprintw(start_y + r * 2 + 1, start_x + c * 5, "%s", piece_symbols[piece]);
            if (r == cursor_r && c == cursor_c) attroff(A_REVERSE);
            attroff(COLOR_PAIR(pair));
        }
    }
}

void draw_pgn(int start_y, int start_x, int height, int width) {
    for (int i = 0; i < height; i++) {
        mvprintw(start_y + i, start_x, "%*s", width, " ");
    }
    char pgn[4096] = "";
    int pos = 0;
    for (int i = 0; i < move_history_count; i++) {
        if (i % 2 == 0) pos += sprintf(pgn + pos, "%d. %s ", (i / 2) + 1, move_history[i].san);
        else pos += sprintf(pgn + pos, "%s  ", move_history[i].san);
    }
    int line = 0;
    char *ptr = pgn;
    int len = strlen(pgn);
    while (len > 0 && line < height) {
        int chunk = len > width ? width : len;
        if (chunk < len) {
            while (chunk > 0 && ptr[chunk] != ' ') chunk--;
            if (chunk == 0) chunk = width;
        }
        char temp[256];
        strncpy(temp, ptr, chunk);
        temp[chunk] = '\0';
        mvprintw(start_y + line, start_x, "%s", temp);
        ptr += chunk;
        if (*ptr == ' ') ptr++;
        len = strlen(ptr);
        line++;
    }
}

void draw_sidebar() {
    attron(COLOR_PAIR(CP_SIDEBAR));
    mvprintw(1, 48, "=== CHESS TERMINAL GUI ===");
    mvprintw(3, 48, "Turn: %s", game_state.turn == WHITE ? "White (Player)" : "Black");
    mvprintw(4, 48, "Status: %s", is_in_check(&game_state, game_state.turn) ? "CHECK!" : "Normal");
    mvprintw(5, 48, "Engine Path: %s", app_settings.engine_active ? "ACTIVE" : "OFF");
    mvprintw(7, 48, "Move Log:");
    draw_pgn(8, 48, 8, 30);

    mvprintw(17, 48, "Engine Output:");
    char short_info[31];
    strncpy(short_info, last_engine_info, 30);
    short_info[30] = '\0';
    mvprintw(18, 48, "%-30s", short_info);

    mvprintw(20, 2, "Controls: [Arrows] Move Cursor | [Space/Enter] Select/Move Piece");
    mvprintw(21, 2, "[U] Takeback Move | [S] Settings Menu | [R] Restart Game | [Q] Quit GUI");
    attroff(COLOR_PAIR(CP_SIDEBAR));
}

void draw_settings_overlay() {
    int sy = 3, sx = 5;
    for (int i = 0; i < 18; i++) mvprintw(sy + i, sx, "                                                ");
    box(newwin(18, 48, sy, sx), 0, 0);
    mvprintw(sy + 1, sx + 4, "=== ENGINE SETTINGS ===");
    mvprintw(sy + 3, sx + 4, "Use [Up/Down] to navigate. [Enter] to edit.");
    mvprintw(sy + 4, sx + 4, "Press [S] or [Esc] to exit settings.");

    char *modes[] = {"Player vs Player", "Play as White (Engine Black)", "Play as Black (Engine White)"};
    mvprintw(sy + 6, sx + 4, "1. Engine Path: %s", app_settings.engine_path);
    mvprintw(sy + 8, sx + 4, "2. Max Depth  : %d", app_settings.depth);
    mvprintw(sy + 10, sx + 4, "3. Max Nodes  : %d", app_settings.nodes);
    mvprintw(sy + 12, sx + 4, "4. Search Time : %d ms", app_settings.movetime_ms);
    mvprintw(sy + 14, sx + 4, "5. Game Mode  : %s", modes[app_settings.engine_active]);
}

void edit_settings_item(int item) {
    echo(); curs_set(1); nodelay(stdscr, FALSE);
    mvprintw(22, 2, "%-75s", " ");
    if (item == 0) {
        mvprintw(22, 2, "Enter absolute Stockfish path: ");
        char temp[256];
        getnstr(temp, sizeof(temp) - 1);
        if (strlen(temp) > 0) {
            strcpy(app_settings.engine_path, temp);
            start_engine(app_settings.engine_path);
        }
    } else if (item == 1) {
        mvprintw(22, 2, "Enter Max Depth (0 to disable): ");
        char temp[32];
        getnstr(temp, sizeof(temp) - 1);
        app_settings.depth = atoi(temp);
    } else if (item == 2) {
        mvprintw(22, 2, "Enter Max Nodes (0 to disable): ");
        char temp[32];
        getnstr(temp, sizeof(temp) - 1);
        app_settings.nodes = atoi(temp);
    } else if (item == 3) {
        mvprintw(22, 2, "Enter engine thinking limit (ms): ");
        char temp[32];
        getnstr(temp, sizeof(temp) - 1);
        app_settings.movetime_ms = atoi(temp);
    } else if (item == 4) {
        app_settings.engine_active = (app_settings.engine_active + 1) % 3;
    }
    noecho(); curs_set(0); nodelay(stdscr, TRUE);
    mvprintw(22, 2, "%-75s", " ");
}

int main() {
    setlocale(LC_ALL, "");
    initscr();
    start_color();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    // Color Pair Schemes
    init_pair(CP_LIGHT_SQ, COLOR_BLACK, COLOR_WHITE);
    init_pair(CP_DARK_SQ, COLOR_WHITE, COLOR_BLUE);
    init_pair(CP_LIGHT_SEL, COLOR_BLACK, COLOR_MAGENTA);
    init_pair(CP_DARK_SEL, COLOR_WHITE, COLOR_MAGENTA);
    init_pair(CP_LIGHT_HL, COLOR_BLACK, COLOR_GREEN);
    init_pair(CP_DARK_HL, COLOR_WHITE, COLOR_GREEN);
    init_pair(CP_SIDEBAR, COLOR_WHITE, COLOR_BLACK);

    init_board(&game_state);
    start_engine(app_settings.engine_path);

    int cursor_r = 6, cursor_c = 4;
    int selected_sq = -1;
    int in_settings = 0;
    int settings_sel = 0;

    while (1) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q') break;

        // Auto trigger engine
        if (!engine_thinking && !in_settings) {
            int engine_turn = (app_settings.engine_active == 1 && game_state.turn == BLACK) ||
                              (app_settings.engine_active == 2 && game_state.turn == WHITE);
            if (engine_turn) {
                Move legal[256];
                if (generate_legal_moves(&game_state, legal) > 0) trigger_engine_search();
            }
        }

        update_engine_io();

        if (ch != ERR) {
            if (in_settings) {
                if (ch == 27 || ch == 's' || ch == 'S') in_settings = 0;
                else if (ch == KEY_UP) settings_sel = (settings_sel - 1 + 5) % 5;
                else if (ch == KEY_DOWN) settings_sel = (settings_sel + 1) % 5;
                else if (ch == '\n' || ch == KEY_ENTER) edit_settings_item(settings_sel);
            } else {
                if (ch == KEY_UP && cursor_r > 0) cursor_r--;
                else if (ch == KEY_DOWN && cursor_r < 7) cursor_r++;
                else if (ch == KEY_LEFT && cursor_c > 0) cursor_c--;
                else if (ch == KEY_RIGHT && cursor_c < 7) cursor_c++;
                else if (ch == 's' || ch == 'S') in_settings = 1;
                else if (ch == 'u' || ch == 'U') {
                    int rollback = app_settings.engine_active > 0 ? 2 : 1;
                    for (int k = 0; k < rollback; k++) {
                        if (state_history_count > 0 && move_history_count > 0) {
                            game_state = state_history[--state_history_count];
                            move_history_count--;
                        }
                    }
                    selected_sq = -1;
                } else if (ch == 'r' || ch == 'R') {
                    init_board(&game_state);
                    move_history_count = 0;
                    state_history_count = 0;
                    selected_sq = -1;
                } else if (ch == ' ' || ch == '\n' || ch == KEY_ENTER) {
                    int sq_idx = cursor_r * 8 + cursor_c;
                    if (selected_sq == -1) {
                        if (game_state.board[sq_idx] != EMPTY && COLOR(game_state.board[sq_idx]) == game_state.turn) {
                            selected_sq = sq_idx;
                        }
                    } else {
                        if (sq_idx == selected_sq) selected_sq = -1;
                        else {
                            Move legal[256];
                            int l_count = generate_legal_moves(&game_state, legal), valid = 0;
                            Move chosen;
                            for (int i = 0; i < l_count; i++) {
                                if (legal[i].from == selected_sq && legal[i].to == sq_idx) {
                                    chosen = legal[i]; valid = 1; break;
                                }
                            }
                            if (valid) {
                                char san[16];
                                get_san(&game_state, chosen, san);
                                state_history[state_history_count++] = game_state;
                                apply_move(&game_state, chosen);
                                move_history[move_history_count].m = chosen;
                                strcpy(move_history[move_history_count].san, san);
                                move_history_count++;
                                selected_sq = -1;
                            } else {
                                if (game_state.board[sq_idx] != EMPTY && COLOR(game_state.board[sq_idx]) == game_state.turn) {
                                    selected_sq = sq_idx;
                                } else {
                                    selected_sq = -1;
                                }
                            }
                        }
                    }
                }
            }
        }

        erase();
        draw_board(cursor_r, cursor_c, selected_sq);
        draw_sidebar();
        if (in_settings) draw_settings_overlay();
        
        // Show outcome notifications
        Move out_test[256];
        if (generate_legal_moves(&game_state, out_test) == 0) {
            if (is_in_check(&game_state, game_state.turn)) {
                mvprintw(12, 10, "  CHECKMATE! %s Wins  ", game_state.turn == WHITE ? "Black" : "White");
            } else {
                mvprintw(12, 10, "     DRAW (STALEMATE)    ");
            }
        }
        
        refresh();
        usleep(10000); // 10ms frame delay to keep CPU low
    }

    stop_engine();
    endwin();
    return 0;
}
