#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <signal.h>
#include <termios.h>

/* Chess Rule Engine Definitions */
#define COLOR_MASK 0x18
#define TYPE_MASK  0x07
#define WHITE      8
#define BLACK      16

#define PAWN       1
#define KNIGHT     2
#define BISHOP     3
#define ROOK       4
#define QUEEN      5
#define KING       6

#define CASTLE_WK  1
#define CASTLE_WQ  2
#define CASTLE_BK  4
#define CASTLE_BQ  8

typedef struct {
    int from;
    int to;
    int promo; // 0 or piece type (QUEEN, ROOK, etc.)
} Move;

typedef struct {
    uint8_t board[64];
    uint8_t active_color;
    uint8_t castling;
    int ep_square; // -1 or 0..63
    int halfmove;
    int fullmove;
} BoardState;

typedef struct {
    Move move;
    uint8_t piece_moved;
    uint8_t piece_captured;
    int prev_ep;
    uint8_t prev_castling;
    int prev_halfmove;
    char san[16];
} MoveHistory;

/* Global state for application */
struct termios orig_termios;
int engine_in = -1;
int engine_out = -1;
pid_t engine_pid = -1;
int engine_color = 0; // 0 if local game

uint8_t init_board[64] = {
    BLACK|ROOK, BLACK|KNIGHT, BLACK|BISHOP, BLACK|QUEEN, BLACK|KING, BLACK|BISHOP, BLACK|KNIGHT, BLACK|ROOK,
    BLACK|PAWN, BLACK|PAWN,   BLACK|PAWN,   BLACK|PAWN,  BLACK|PAWN, BLACK|PAWN,   BLACK|PAWN,   BLACK|PAWN,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    WHITE|PAWN, WHITE|PAWN,   WHITE|PAWN,   WHITE|PAWN,  WHITE|PAWN, WHITE|PAWN,   WHITE|PAWN,   WHITE|PAWN,
    WHITE|ROOK, WHITE|KNIGHT, WHITE|BISHOP, WHITE|QUEEN, WHITE|KING, WHITE|BISHOP, WHITE|KNIGHT, WHITE|ROOK
};

/* Forward declarations */
void generate_legal_moves(BoardState *state, Move *moves, int *count);
int is_square_attacked(BoardState *state, int sq, int attacker_color);
void make_move(BoardState *state, Move move, MoveHistory *hist);
void unmake_move(BoardState *state, Move move, MoveHistory *hist);

/* Terminal Raw Mode Utilities */
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

/* Process cleanup */
void cleanup() {
    disable_raw_mode();
    if (engine_pid > 0) {
        write(engine_in, "quit\n", 5);
        kill(engine_pid, SIGTERM);
    }
}

/* Spawns the external UCI engine process */
void start_engine(const char *path) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) {
        perror("pipe failed");
        exit(1);
    }
    engine_pid = fork();
    if (engine_pid < 0) {
        perror("fork failed");
        exit(1);
    }
    if (engine_pid == 0) { // Child
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execlp(path, path, (char *)NULL);
        perror("Failed to execute UCI engine");
        exit(1);
    } else { // Parent
        close(in_pipe[0]);
        close(out_pipe[1]);
        engine_in = in_pipe[1];
        engine_out = out_pipe[0];

        // Initialize UCI communication protocol
        write(engine_in, "uci\n", 4);
        char buf[1024];
        int idx = 0;
        while (read(engine_out, &buf[idx], 1) > 0) {
            if (buf[idx] == '\n') {
                buf[idx] = '\0';
                if (strncmp(buf, "uciok", 5) == 0) break;
                idx = 0;
            } else if (idx < 1023) idx++;
        }
        write(engine_in, "isready\n", 8);
        idx = 0;
        while (read(engine_out, &buf[idx], 1) > 0) {
            if (buf[idx] == '\n') {
                buf[idx] = '\0';
                if (strncmp(buf, "readyok", 7) == 0) break;
                idx = 0;
            } else if (idx < 1023) idx++;
        }
        write(engine_in, "ucinewgame\n", 11);
    }
}

/* Communicates with UCI Engine to resolve an output move */
void get_engine_move(BoardState *state, MoveHistory *history, int history_count, Move *out_move) {
    char cmd[8192] = "position startpos";
    if (history_count > 0) {
        strcat(cmd, " moves");
        for (int i = 0; i < history_count; i++) {
            char move_str[10];
            int f = history[i].move.from;
            int t = history[i].move.to;
            int ptype = history[i].piece_moved & TYPE_MASK;
            int tr = t / 8;
            if (ptype == PAWN && (tr == 0 || tr == 7)) {
                sprintf(move_str, " %c%d%c%dq", 'a' + (f % 8), 8 - (f / 8), 'a' + (t % 8), 8 - (t / 8));
            } else {
                sprintf(move_str, " %c%d%c%d", 'a' + (f % 8), 8 - (f / 8), 'a' + (t % 8), 8 - (t / 8));
            }
            strcat(cmd, move_str);
        }
    }
    strcat(cmd, "\n");
    write(engine_in, cmd, strlen(cmd));
    write(engine_in, "go movetime 1000\n", 17); // Engine limit: 1 second per move

    char line[1024];
    int idx = 0;
    char ch;
    while (read(engine_out, &ch, 1) > 0) {
        if (ch == '\n') {
            line[idx] = '\0';
            if (strncmp(line, "bestmove", 8) == 0) {
                char move_str[16];
                sscanf(line, "bestmove %s", move_str);
                int f_col = move_str[0] - 'a';
                int f_row = '8' - move_str[1];
                int t_col = move_str[2] - 'a';
                int t_row = '8' - move_str[3];

                out_move->from = f_row * 8 + f_col;
                out_move->to = t_row * 8 + t_col;
                out_move->promo = 0;
                if (strlen(move_str) == 5) {
                    char p = move_str[4];
                    if (p == 'q') out_move->promo = QUEEN;
                    else if (p == 'r') out_move->promo = ROOK;
                    else if (p == 'b') out_move->promo = BISHOP;
                    else if (p == 'n') out_move->promo = KNIGHT;
                }
                break;
            }
            idx = 0;
        } else if (idx < 1023) {
            line[idx++] = ch;
        }
    }
}

/* Helper to detect if active color's king is in check */
int king_in_check(BoardState *state, int color) {
    int king_sq = -1;
    for (int s = 0; s < 64; s++) {
        if (state->board[s] == (color | KING)) {
            king_sq = s;
            break;
        }
    }
    if (king_sq == -1) return 0;
    return is_square_attacked(state, king_sq, color ^ COLOR_MASK);
}

/* Standard Algebraic Notation (SAN) Converter */
void to_san(BoardState *state, Move move, char *buf) {
    uint8_t piece = state->board[move.from];
    uint8_t ptype = piece & TYPE_MASK;
    uint8_t pcolor = piece & COLOR_MASK;

    if (ptype == KING) {
        if (move.from == 60 && move.to == 62 && pcolor == WHITE) { strcpy(buf, "O-O"); return; }
        if (move.from == 60 && move.to == 58 && pcolor == WHITE) { strcpy(buf, "O-O-O"); return; }
        if (move.from == 4 && move.to == 6 && pcolor == BLACK) { strcpy(buf, "O-O"); return; }
        if (move.from == 4 && move.to == 2 && pcolor == BLACK) { strcpy(buf, "O-O-O"); return; }
    }

    int ptr = 0;
    if (ptype != PAWN) {
        char p_chars[] = "  NBRQK";
        buf[ptr++] = p_chars[ptype];

        // Disambiguate matching pieces
        Move legal_moves[256];
        int count = 0;
        generate_legal_moves(state, legal_moves, &count);
        int dup_file = 0, dup_rank = 0, dup_exists = 0;
        for (int i = 0; i < count; i++) {
            if (legal_moves[i].from != move.from && legal_moves[i].to == move.to) {
                if (state->board[legal_moves[i].from] == piece) {
                    dup_exists = 1;
                    if ((legal_moves[i].from % 8) == (move.from % 8)) dup_rank = 1;
                    else dup_file = 1;
                }
            }
        }
        if (dup_exists) {
            if (!dup_file) buf[ptr++] = 'a' + (move.from % 8);
            else if (!dup_rank) buf[ptr++] = '8' - (move.from / 8);
            else {
                buf[ptr++] = 'a' + (move.from % 8);
                buf[ptr++] = '8' - (move.from / 8);
            }
        }
    } else {
        if (state->board[move.to] != 0 || move.to == state->ep_square) {
            buf[ptr++] = 'a' + (move.from % 8);
        }
    }

    if (state->board[move.to] != 0 || (ptype == PAWN && move.to == state->ep_square)) {
        buf[ptr++] = 'x';
    }

    buf[ptr++] = 'a' + (move.to % 8);
    buf[ptr++] = '8' - (move.to / 8);

    if (ptype == PAWN && (move.to / 8 == 0 || move.to / 8 == 7)) {
        buf[ptr++] = '=';
        buf[ptr++] = move.promo ? ("  NBRQK"[move.promo]) : 'Q';
    }

    BoardState temp = *state;
    MoveHistory th;
    make_move(&temp, move, &th);
    if (king_in_check(&temp, pcolor ^ COLOR_MASK)) {
        Move op_moves[256];
        int op_count = 0;
        generate_legal_moves(&temp, op_moves, &op_count);
        if (op_count == 0) buf[ptr++] = '#';
        else buf[ptr++] = '+';
    }
    buf[ptr] = '\0';
}

/* Chess Move Engine Calculations */
int is_square_attacked(BoardState *state, int sq, int attacker_color) {
    int r = sq / 8;
    int c = sq % 8;

    int pawn_offset = (attacker_color == WHITE) ? 1 : -1;
    int pawn_cols[2] = {c - 1, c + 1};
    for (int i = 0; i < 2; i++) {
        int pc = pawn_cols[i];
        int pr = r + pawn_offset;
        if (pr >= 0 && pr < 8 && pc >= 0 && pc < 8) {
            if (state->board[pr * 8 + pc] == (attacker_color | PAWN)) return 1;
        }
    }

    int knight_offsets[8][2] = {{-2,-1}, {-2,1}, {-1,-2}, {-1,2}, {1,-2}, {1,2}, {2,-1}, {2,1}};
    for (int i = 0; i < 8; i++) {
        int nr = r + knight_offsets[i][0];
        int nc = c + knight_offsets[i][1];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == (attacker_color | KNIGHT)) return 1;
        }
    }

    int king_offsets[8][2] = {{-1,-1}, {-1,0}, {-1,1}, {0,-1}, {0,1}, {1,-1}, {1,0}, {1,1}};
    for (int i = 0; i < 8; i++) {
        int kr = r + king_offsets[i][0];
        int kc = c + king_offsets[i][1];
        if (kr >= 0 && kr < 8 && kc >= 0 && kc < 8) {
            if (state->board[kr * 8 + kc] == (attacker_color | KING)) return 1;
        }
    }

    int rook_dirs[4][2] = {{-1,0}, {1,0}, {0,-1}, {0,1}};
    for (int i = 0; i < 4; i++) {
        int step = 1;
        while (1) {
            int rr = r + rook_dirs[i][0] * step;
            int rc = c + rook_dirs[i][1] * step;
            if (rr < 0 || rr >= 8 || rc < 0 || rc >= 8) break;
            uint8_t p = state->board[rr * 8 + rc];
            if (p != 0) {
                if (p == (attacker_color | ROOK) || p == (attacker_color | QUEEN)) return 1;
                break;
            }
            step++;
        }
    }

    int bishop_dirs[4][2] = {{-1,-1}, {-1,1}, {1,-1}, {1,1}};
    for (int i = 0; i < 4; i++) {
        int step = 1;
        while (1) {
            int br = r + bishop_dirs[i][0] * step;
            int bc = c + bishop_dirs[i][1] * step;
            if (br < 0 || br >= 8 || bc < 0 || bc >= 8) break;
            uint8_t p = state->board[br * 8 + bc];
            if (p != 0) {
                if (p == (attacker_color | BISHOP) || p == (attacker_color | QUEEN)) return 1;
                break;
            }
            step++;
        }
    }
    return 0;
}

void make_move(BoardState *state, Move move, MoveHistory *hist) {
    hist->move = move;
    hist->piece_moved = state->board[move.from];
    hist->piece_captured = state->board[move.to];
    hist->prev_ep = state->ep_square;
    hist->prev_castling = state->castling;
    hist->prev_halfmove = state->halfmove;

    uint8_t piece = state->board[move.from];
    uint8_t ptype = piece & TYPE_MASK;
    uint8_t pcolor = piece & COLOR_MASK;

    if (ptype == PAWN && move.to == state->ep_square) {
        int ep_pawn_sq = (pcolor == WHITE) ? move.to + 8 : move.to - 8;
        hist->piece_captured = state->board[ep_pawn_sq];
        state->board[ep_pawn_sq] = 0;
    }

    state->board[move.to] = piece;
    state->board[move.from] = 0;

    if (ptype == PAWN && (move.to / 8 == 0 || move.to / 8 == 7)) {
        state->board[move.to] = pcolor | (move.promo ? move.promo : QUEEN);
    }

    if (ptype == KING) {
        if (move.from == 60 && move.to == 62 && pcolor == WHITE) { state->board[61] = WHITE|ROOK; state->board[63] = 0; }
        else if (move.from == 60 && move.to == 58 && pcolor == WHITE) { state->board[59] = WHITE|ROOK; state->board[56] = 0; }
        else if (move.from == 4 && move.to == 6 && pcolor == BLACK) { state->board[5] = BLACK|ROOK; state->board[7] = 0; }
        else if (move.from == 4 && move.to == 2 && pcolor == BLACK) { state->board[3] = BLACK|ROOK; state->board[0] = 0; }
    }

    state->ep_square = -1;
    if (ptype == PAWN) {
        if (move.from - move.to == 16) state->ep_square = move.to + 8;
        else if (move.to - move.from == 16) state->ep_square = move.to - 8;
    }

    if (ptype == KING) {
        if (pcolor == WHITE) state->castling &= ~(CASTLE_WK | CASTLE_WQ);
        else state->castling &= ~(CASTLE_BK | CASTLE_BQ);
    }
    if (move.from == 56 || move.to == 56) state->castling &= ~CASTLE_WQ;
    if (move.from == 63 || move.to == 63) state->castling &= ~CASTLE_WK;
    if (move.from == 0 || move.to == 0) state->castling &= ~CASTLE_BQ;
    if (move.from == 7 || move.to == 7) state->castling &= ~CASTLE_BK;

    if (ptype == PAWN || hist->piece_captured != 0) state->halfmove = 0;
    else state->halfmove++;

    if (state->active_color == BLACK) state->fullmove++;
    state->active_color ^= COLOR_MASK;
}

void unmake_move(BoardState *state, Move move, MoveHistory *hist) {
    state->active_color ^= COLOR_MASK;

    uint8_t ptype = hist->piece_moved & TYPE_MASK;
    uint8_t pcolor = hist->piece_moved & COLOR_MASK;

    state->board[move.from] = hist->piece_moved;
    state->board[move.to] = hist->piece_captured;

    if (ptype == PAWN && move.to == hist->prev_ep) {
        int ep_pawn_sq = (pcolor == WHITE) ? move.to + 8 : move.to - 8;
        state->board[ep_pawn_sq] = hist->piece_captured;
        state->board[move.to] = 0;
    }

    if (ptype == KING) {
        if (move.from == 60 && move.to == 62 && pcolor == WHITE) { state->board[63] = WHITE|ROOK; state->board[61] = 0; }
        else if (move.from == 60 && move.to == 58 && pcolor == WHITE) { state->board[56] = WHITE|ROOK; state->board[59] = 0; }
        else if (move.from == 4 && move.to == 6 && pcolor == BLACK) { state->board[7] = BLACK|ROOK; state->board[5] = 0; }
        else if (move.from == 4 && move.to == 2 && pcolor == BLACK) { state->board[0] = BLACK|ROOK; state->board[3] = 0; }
    }

    state->ep_square = hist->prev_ep;
    state->castling = hist->prev_castling;
    state->halfmove = hist->prev_halfmove;
    if (state->active_color == BLACK) state->fullmove--;
}

void generate_legal_moves(BoardState *state, Move *moves, int *count) {
    *count = 0;
    Move pseudo[256];
    int p_count = 0;
    int active = state->active_color;

    for (int sq = 0; sq < 64; sq++) {
        uint8_t p = state->board[sq];
        if (p == 0 || (p & COLOR_MASK) != active) continue;

        int r = sq / 8, c = sq % 8;
        int type = p & TYPE_MASK;

        if (type == PAWN) {
            int dir = (active == WHITE) ? -1 : 1;
            int tr = r + dir;
            if (tr >= 0 && tr < 8 && state->board[tr*8 + c] == 0) {
                if (tr == 0 || tr == 7) {
                    pseudo[p_count++] = (Move){sq, tr*8 + c, QUEEN};
                    pseudo[p_count++] = (Move){sq, tr*8 + c, ROOK};
                    pseudo[p_count++] = (Move){sq, tr*8 + c, BISHOP};
                    pseudo[p_count++] = (Move){sq, tr*8 + c, KNIGHT};
                } else {
                    pseudo[p_count++] = (Move){sq, tr*8 + c, 0};
                    int r_start = (active == WHITE) ? 6 : 1;
                    if (r == r_start && state->board[(r + 2*dir)*8 + c] == 0) {
                        pseudo[p_count++] = (Move){sq, (r + 2*dir)*8 + c, 0};
                    }
                }
            }
            int cols[2] = {c - 1, c + 1};
            for (int i = 0; i < 2; i++) {
                int tc = cols[i];
                if (tc >= 0 && tc < 8 && tr >= 0 && tr < 8) {
                    int dest = tr * 8 + tc;
                    if (state->board[dest] != 0 && (state->board[dest] & COLOR_MASK) != active) {
                        if (tr == 0 || tr == 7) {
                            pseudo[p_count++] = (Move){sq, dest, QUEEN};
                            pseudo[p_count++] = (Move){sq, dest, ROOK};
                            pseudo[p_count++] = (Move){sq, dest, BISHOP};
                            pseudo[p_count++] = (Move){sq, dest, KNIGHT};
                        } else {
                            pseudo[p_count++] = (Move){sq, dest, 0};
                        }
                    } else if (dest == state->ep_square) {
                        pseudo[p_count++] = (Move){sq, dest, 0};
                    }
                }
            }
        } else if (type == KNIGHT) {
            int offsets[8][2] = {{-2,-1}, {-2,1}, {-1,-2}, {-1,2}, {1,-2}, {1,2}, {2,-1}, {2,1}};
            for (int i = 0; i < 8; i++) {
                int tr = r + offsets[i][0], tc = c + offsets[i][1];
                if (tr >= 0 && tr < 8 && tc >= 0 && tc < 8) {
                    int dest = tr * 8 + tc;
                    if (state->board[dest] == 0 || (state->board[dest] & COLOR_MASK) != active) {
                        pseudo[p_count++] = (Move){sq, dest, 0};
                    }
                }
            }
        } else if (type == KING) {
            int offsets[8][2] = {{-1,-1}, {-1,0}, {-1,1}, {0,-1}, {0,1}, {1,-1}, {1,0}, {1,1}};
            for (int i = 0; i < 8; i++) {
                int tr = r + offsets[i][0], tc = c + offsets[i][1];
                if (tr >= 0 && tr < 8 && tc >= 0 && tc < 8) {
                    int dest = tr * 8 + tc;
                    if (state->board[dest] == 0 || (state->board[dest] & COLOR_MASK) != active) {
                        pseudo[p_count++] = (Move){sq, dest, 0};
                    }
                }
            }
        } else {
            int dirs[8][2], d_count = 0;
            if (type == BISHOP || type == QUEEN) {
                int b_dirs[4][2] = {{-1,-1}, {-1,1}, {1,-1}, {1,1}};
                for (int i=0; i<4; i++) { dirs[d_count][0] = b_dirs[i][0]; dirs[d_count][1] = b_dirs[i][1]; d_count++; }
            }
            if (type == ROOK || type == QUEEN) {
                int r_dirs[4][2] = {{-1,0}, {1,0}, {0,-1}, {0,1}};
                for (int i=0; i<4; i++) { dirs[d_count][0] = r_dirs[i][0]; dirs[d_count][1] = r_dirs[i][1]; d_count++; }
            }
            for (int d = 0; d < d_count; d++) {
                int step = 1;
                while (1) {
                    int tr = r + dirs[d][0] * step, tc = c + dirs[d][1] * step;
                    if (tr < 0 || tr >= 8 || tc < 0 || tc >= 8) break;
                    int dest = tr * 8 + tc;
                    if (state->board[dest] == 0) pseudo[p_count++] = (Move){sq, dest, 0};
                    else {
                        if ((state->board[dest] & COLOR_MASK) != active) pseudo[p_count++] = (Move){sq, dest, 0};
                        break;
                    }
                    step++;
                }
            }
        }
    }

    MoveHistory tmp;
    for (int i = 0; i < p_count; i++) {
        make_move(state, pseudo[i], &tmp);
        if (!king_in_check(state, active)) {
            moves[(*count)++] = pseudo[i];
        }
        unmake_move(state, pseudo[i], &tmp);
    }

    // Castling Validations
    if (active == WHITE) {
        if ((state->castling & CASTLE_WK) && state->board[61] == 0 && state->board[62] == 0 &&
            !is_square_attacked(state, 60, BLACK) && !is_square_attacked(state, 61, BLACK) && !is_square_attacked(state, 62, BLACK)) {
            moves[(*count)++] = (Move){60, 62, 0};
        }
        if ((state->castling & CASTLE_WQ) && state->board[57] == 0 && state->board[58] == 0 && state->board[59] == 0 &&
            !is_square_attacked(state, 60, BLACK) && !is_square_attacked(state, 59, BLACK) && !is_square_attacked(state, 58, BLACK)) {
            moves[(*count)++] = (Move){60, 58, 0};
        }
    } else {
        if ((state->castling & CASTLE_BK) && state->board[5] == 0 && state->board[6] == 0 &&
            !is_square_attacked(state, 4, WHITE) && !is_square_attacked(state, 5, WHITE) && !is_square_attacked(state, 6, WHITE)) {
            moves[(*count)++] = (Move){4, 6, 0};
        }
        if ((state->castling & CASTLE_BQ) && state->board[1] == 0 && state->board[2] == 0 && state->board[3] == 0 &&
            !is_square_attacked(state, 4, WHITE) && !is_square_attacked(state, 3, WHITE) && !is_square_attacked(state, 2, WHITE)) {
            moves[(*count)++] = (Move){4, 2, 0};
        }
    }
}

/* Screen Rendering Utilities */
void draw_board(BoardState *state, int cursor_sq, int selected_sq, Move *legal_moves, int legal_count, MoveHistory *history, int history_count, const char *status_msg) {
    printf("\033[H"); // Cursor position home
    printf("\n  \033[1;36m♟ CHESS GUI PORTABLE\033[0m\n\n");

    char targets[64] = {0};
    if (selected_sq != -1) {
        for (int i = 0; i < legal_count; i++) {
            if (legal_moves[i].from == selected_sq) {
                targets[legal_moves[i].to] = 1;
            }
        }
    }

    for (int r = 0; r < 8; r++) {
        printf("  %d ", 8 - r);
        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            uint8_t piece = state->board[sq];
            uint8_t ptype = piece & TYPE_MASK;
            uint8_t pcolor = piece & COLOR_MASK;

            int bg = ((r + c) % 2 == 0) ? 254 : 240; // Wood/Grey high-contrast checker
            if (sq == cursor_sq) bg = 220;           // Bright Gold/Yellow Cursor
            else if (sq == selected_sq) bg = 196;    // Bright Red Select
            else if (targets[sq]) bg = 41;           // Vibrant Green Destination Target

            char p_char = ' ';
            int fg = 0;
            if (ptype != 0) {
                char p_chars[] = "  NBRQK";
                p_char = (ptype == PAWN) ? 'P' : p_chars[ptype];
                if (pcolor == WHITE) fg = 15; // Crisp White Piece
                else {
                    fg = 16;                  // Shadow Black Piece
                    p_char += 32;             // Lowercase ASCII mapping
                }
            }

            if (ptype != 0) {
                if (pcolor == WHITE) printf("\033[48;5;%dm\033[1;38;5;%dm %c \033[0m", bg, fg, p_char);
                else printf("\033[48;5;%dm\033[38;5;%dm %c \033[0m", bg, fg, p_char);
            } else {
                if (targets[sq]) printf("\033[48;5;%dm * \033[0m", bg);
                else printf("\033[48;5;%dm   \033[0m", bg);
            }
        }
        printf("\n");
    }

    printf("     ");
    for (int c = 0; c < 8; c++) printf(" %c ", 'A' + c);
    printf("\n\n");

    printf(" Status: \033[1;33m%s\033[0m\n", status_msg);
    printf(" Active: %s\n", (state->active_color == WHITE) ? "WHITE" : "BLACK");

    // Print moves using standard PGN history formatting
    printf(" Moves:  ");
    int start = (history_count > 10) ? history_count - 10 : 0;
    if (start > 0) printf("... ");
    for (int i = start; i < history_count; i++) {
        if (i % 2 == 0) printf("%d. %s ", (i / 2) + 1, history[i].san);
        else printf("%s  ", history[i].san);
    }
    printf("\n\n");

    printf(" Controls: \033[1m[Arrows/WASD]\033[0m Move | \033[1m[Space/Enter]\033[0m Select/Move\n");
    printf("           \033[1m[U]\033[0m Takeback (Undo)  | \033[1m[Q]\033[0m Quit Application\n");
}

/* Translates standard Arrow keystrokes */
int read_key() {
    char c;
    int nread = read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return 0;
    if (c == '\033') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) == 0) return '\033';
        if (read(STDIN_FILENO, &seq[1], 1) == 0) return '\033';
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 'W'; // UP
                case 'B': return 'S'; // DOWN
                case 'C': return 'D'; // RIGHT
                case 'D': return 'A'; // LEFT
            }
        }
        return '\033';
    }
    return c;
}

int main(int argc, char **argv) {
    BoardState state;
    memcpy(state.board, init_board, 64);
    state.active_color = WHITE;
    state.castling = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
    state.ep_square = -1;
    state.halfmove = 0;
    state.fullmove = 1;

    MoveHistory history[2048];
    int history_count = 0;

    if (argc > 1) {
        printf("\nStarting engine connection to %s...\n", argv[1]);
        start_engine(argv[1]);
        printf("Select your color (w = White, b = Black): ");
        fflush(stdout);
        char choice;
        if (scanf(" %c", &choice) == 1 && (choice == 'b' || choice == 'B')) {
            engine_color = WHITE;
        } else {
            engine_color = BLACK;
        }
        // clear input stream buffer
        int c;
        while ((c = getchar()) != '\n' && c != EOF);
    }

    enable_raw_mode();
    atexit(cleanup);

    int cursor_sq = 44; // Starts on E3
    int selected_sq = -1;
    char status_msg[256] = "Match Started";

    printf("\033[2J"); // Direct Clear terminal

    while (1) {
        Move legal_moves[256];
        int legal_count = 0;
        generate_legal_moves(&state, legal_moves, &legal_count);

        if (legal_count == 0) {
            if (king_in_check(&state, state.active_color)) {
                sprintf(status_msg, "GAME OVER - %s victory by checkmate!", (state.active_color == WHITE) ? "BLACK" : "WHITE");
            } else {
                sprintf(status_msg, "GAME OVER - Draw by Stalemate!");
            }
        } else if (state.halfmove >= 100) {
            sprintf(status_msg, "GAME OVER - Draw by 50-move rule!");
        } else if (engine_pid != -1) {
            sprintf(status_msg, "Versus Stockfish Engine (Your Color: %s)", (engine_color == BLACK) ? "White" : "Black");
        } else {
            sprintf(status_msg, "Two Player Local Match");
        }

        draw_board(&state, cursor_sq, selected_sq, legal_moves, legal_count, history, history_count, status_msg);

        // Turn management for UCI engine
        if (engine_pid != -1 && state.active_color == engine_color && legal_count > 0) {
            draw_board(&state, cursor_sq, selected_sq, legal_moves, legal_count, history, history_count, "Stockfish engine is thinking...");
            Move eng_move;
            get_engine_move(&state, history, history_count, &eng_move);
            to_san(&state, eng_move, history[history_count].san);
            make_move(&state, eng_move, &history[history_count]);
            history_count++;
            selected_sq = -1;
            continue;
        }

        int key = read_key();
        if (key == 0) continue;
        if (key == 'q' || key == 'Q') break;

        // Navigation offsets
        int r = cursor_sq / 8;
        int c = cursor_sq % 8;

        if (key == 'w' || key == 'W') { if (r > 0) r--; }
        else if (key == 's' || key == 'S') { if (r < 7) r++; }
        else if (key == 'a' || key == 'A') { if (c > 0) c--; }
        else if (key == 'd' || key == 'D') { if (c < 7) c++; }
        else if (key == 'u' || key == 'U') { // Undo trigger
            if (engine_pid != -1) {
                if (history_count >= 2) {
                    unmake_move(&state, history[history_count - 1].move, &history[history_count - 1]);
                    unmake_move(&state, history[history_count - 2].move, &history[history_count - 2]);
                    history_count -= 2;
                }
            } else if (history_count >= 1) {
                unmake_move(&state, history[history_count - 1].move, &history[history_count - 1]);
                history_count--;
            }
            selected_sq = -1;
        } else if (key == ' ' || key == '\n') {
            if (selected_sq == -1) {
                uint8_t piece = state.board[cursor_sq];
                if (piece != 0 && (piece & COLOR_MASK) == state.active_color) {
                    selected_sq = cursor_sq;
                }
            } else {
                int is_legal = 0;
                Move chosen;
                for (int i = 0; i < legal_count; i++) {
                    if (legal_moves[i].from == selected_sq && legal_moves[i].to == cursor_sq) {
                        is_legal = 1;
                        chosen = legal_moves[i];
                        break;
                    }
                }
                if (is_legal) {
                    to_san(&state, chosen, history[history_count].san);
                    make_move(&state, chosen, &history[history_count]);
                    history_count++;
                    selected_sq = -1;
                } else {
                    // Update selection path dynamically
                    uint8_t piece = state.board[cursor_sq];
                    if (piece != 0 && (piece & COLOR_MASK) == state.active_color) {
                        selected_sq = cursor_sq;
                    } else {
                        selected_sq = -1;
                    }
                }
            }
        }
        cursor_sq = r * 8 + c;
    }

    return 0;
}
