#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <ctype.h>

#define MAX_HISTORY 2048

// --- Core Data Structures ---

typedef struct {
    int from;
    int to;
    uint8_t promotion;
} Move;

typedef struct {
    Move moves[256];
    int count;
} MoveList;

typedef struct {
    uint8_t board[64];
    uint8_t turn;       // 8 = White, 16 = Black
    uint8_t castling;   // bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ
    int ep_square;      // ep capture square (-1 or 0..63)
    int halfmove;
    int fullmove;
} BoardState;

typedef struct {
    int mode;           // 0 = Depth, 1 = Nodes, 2 = MoveTime
    int value;
} TimeControl;

typedef struct {
    int in_pipe[2];     // GUI writes to in_pipe[1]
    int out_pipe[2];    // GUI reads from out_pipe[0]
    pid_t pid;
    int active;
    char path[256];
} Engine;

// --- Global State ---
struct termios orig_termios;
BoardState history[MAX_HISTORY];
char san_history[MAX_HISTORY][16];
Move move_history[MAX_HISTORY];
int history_count = 0;

// --- Terminal Control ---

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h\033[0m"); // Restore cursor, reset colors
    fflush(stdout);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
    fflush(stdout);
}

// --- Engine Communication ---

void engine_stop(Engine *eng) {
    if (eng->active) {
        kill(eng->pid, SIGTERM);
        waitpid(eng->pid, NULL, 0);
        close(eng->in_pipe[1]);
        close(eng->out_pipe[0]);
        eng->active = 0;
    }
}

int engine_start(Engine *eng, const char *path) {
    engine_stop(eng);
    if (pipe(eng->in_pipe) < 0 || pipe(eng->out_pipe) < 0) return 0;

    eng->pid = fork();
    if (eng->pid < 0) return 0;

    if (eng->pid == 0) {
        dup2(eng->in_pipe[0], STDIN_FILENO);
        dup2(eng->out_pipe[1], STDOUT_FILENO);
        close(eng->in_pipe[1]);
        close(eng->out_pipe[0]);

        execlp(path, path, (char *)NULL);
        exit(1);
    }

    close(eng->in_pipe[0]);
    close(eng->out_pipe[1]);

    int flags = fcntl(eng->out_pipe[0], F_GETFL, 0);
    fcntl(eng->out_pipe[0], F_SETFL, flags | O_NONBLOCK);

    write(eng->in_pipe[1], "uci\n", 4);

    struct pollfd pfd;
    pfd.fd = eng->out_pipe[0];
    pfd.events = POLLIN;

    int ready = 0;
    char line[1024];
    while (poll(&pfd, 1, 300) > 0) {
        char c;
        static int pos = 0;
        while (read(eng->out_pipe[0], &c, 1) > 0) {
            if (c == '\n') {
                line[pos] = '\0';
                pos = 0;
                if (strstr(line, "uciok")) ready = 1;
            } else if (pos < 1022) {
                line[pos++] = c;
            }
        }
        if (ready) break;
    }

    if (!ready) {
        kill(eng->pid, SIGKILL);
        waitpid(eng->pid, NULL, 0);
        close(eng->in_pipe[1]);
        close(eng->out_pipe[0]);
        return 0;
    }

    strncpy(eng->path, path, sizeof(eng->path));
    eng->active = 1;
    return 1;
}

void auto_detect_engine(Engine *eng) {
    const char *paths[] = {
        "stockfish",
        "/opt/homebrew/bin/stockfish",
        "/usr/local/bin/stockfish",
        "/usr/bin/stockfish",
        "./stockfish"
    };
    for (int i = 0; i < 5; i++) {
        if (engine_start(eng, paths[i])) return;
    }
    eng->active = 0;
}

int get_engine_line(int fd, char *line, int max_len) {
    static char buf[4096];
    static int pos = 0;
    char c;
    while (read(fd, &c, 1) > 0) {
        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                buf[pos] = '\0';
                strncpy(line, buf, max_len);
                pos = 0;
                return 1;
            }
        } else if (pos < (int)sizeof(buf) - 2) {
            buf[pos++] = c;
        }
    }
    return 0;
}

// --- Chess Logic Core ---

void init_board(BoardState *state) {
    memset(state->board, 0, 64);
    // Setup Black pieces
    state->board[0] = 20; state->board[1] = 18; state->board[2] = 19; state->board[3] = 21;
    state->board[4] = 22; state->board[5] = 19; state->board[6] = 18; state->board[7] = 20;
    for (int i = 8; i < 16; i++) state->board[i] = 17;

    // Setup White pieces
    state->board[56] = 12; state->board[57] = 10; state->board[58] = 11; state->board[59] = 13;
    state->board[60] = 14; state->board[61] = 11; state->board[62] = 10; state->board[63] = 12;
    for (int i = 48; i < 56; i++) state->board[i] = 9;

    state->turn = 8;
    state->castling = 15;
    state->ep_square = -1;
    state->halfmove = 0;
    state->fullmove = 1;
}

int is_square_attacked(const BoardState *state, int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;

    // Knight attacks
    int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_r[i], nc = c + kn_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            uint8_t p = state->board[nr * 8 + nc];
            if ((p & 7) == 2 && (p & attacker_color)) return 1;
        }
    }

    // Slider attacks (Bishops & Queens)
    int b_r[] = {-1, -1, 1, 1};
    int b_c[] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += b_r[i]; nc += b_c[i];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            uint8_t p = state->board[nr * 8 + nc];
            if (p != 0) {
                if ((p & attacker_color) && ((p & 7) == 3 || (p & 7) == 5)) return 1;
                break;
            }
        }
    }

    // Slider attacks (Rooks & Queens)
    int r_r[] = {-1, 1, 0, 0};
    int r_c[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int nr = r, nc = c;
        while (1) {
            nr += r_r[i]; nc += r_c[i];
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            uint8_t p = state->board[nr * 8 + nc];
            if (p != 0) {
                if ((p & attacker_color) && ((p & 7) == 4 || (p & 7) == 5)) return 1;
                break;
            }
        }
    }

    // Pawn attacks
    if (attacker_color == 8) { // White Pawn attack from below
        int pr = r + 1;
        if (pr < 8) {
            if (c - 1 >= 0 && (state->board[pr * 8 + c - 1] & 7) == 1 && (state->board[pr * 8 + c - 1] & 8)) return 1;
            if (c + 1 < 8 && (state->board[pr * 8 + c + 1] & 7) == 1 && (state->board[pr * 8 + c + 1] & 8)) return 1;
        }
    } else { // Black Pawn attack from above
        int pr = r - 1;
        if (pr >= 0) {
            if (c - 1 >= 0 && (state->board[pr * 8 + c - 1] & 7) == 1 && (state->board[pr * 8 + c - 1] & 16)) return 1;
            if (c + 1 < 8 && (state->board[pr * 8 + c + 1] & 7) == 1 && (state->board[pr * 8 + c + 1] & 16)) return 1;
        }
    }

    // King attacks
    int k_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int k_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_r[i], nc = c + k_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            uint8_t p = state->board[nr * 8 + nc];
            if ((p & 7) == 6 && (p & attacker_color)) return 1;
        }
    }

    return 0;
}

int is_in_check(const BoardState *state, int color) {
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if ((state->board[i] & 7) == 6 && (state->board[i] & color)) {
            king_sq = i;
            break;
        }
    }
    if (king_sq == -1) return 0;
    return is_square_attacked(state, king_sq, color == 8 ? 16 : 8);
}

void generate_moves_for_piece(const BoardState *state, int sq, MoveList *ml) {
    uint8_t p = state->board[sq];
    if (p == 0) return;
    int color = p & 24;
    if (color != state->turn) return;

    int type = p & 7;
    int r = sq / 8, c = sq % 8;

    if (type == 1) { // Pawn
        int dir = (color == 8) ? -1 : 1;
        int start_row = (color == 8) ? 6 : 1;
        int promo_row = (color == 8) ? 0 : 7;

        // Push 1
        int nr = r + dir;
        if (nr >= 0 && nr < 8 && state->board[nr * 8 + c] == 0) {
            if (nr == promo_row) {
                for (int pr = 2; pr <= 5; pr++) {
                    Move mv = {sq, nr * 8 + c, pr};
                    ml->moves[ml->count++] = mv;
                }
            } else {
                Move mv = {sq, nr * 8 + c, 0};
                ml->moves[ml->count++] = mv;
            }

            // Push 2
            int nnr = r + 2 * dir;
            if (r == start_row && state->board[nnr * 8 + c] == 0) {
                Move mv = {sq, nnr * 8 + c, 0};
                ml->moves[ml->count++] = mv;
            }
        }

        // Capture
        int dc[] = {-1, 1};
        for (int i = 0; i < 2; i++) {
            int nc = c + dc[i];
            int n_row = r + dir;
            if (n_row >= 0 && n_row < 8 && nc >= 0 && nc < 8) {
                int to = n_row * 8 + nc;
                uint8_t target = state->board[to];
                if ((target != 0 && (target & 24) != color) || to == state->ep_square) {
                    if (n_row == promo_row) {
                        for (int pr = 2; pr <= 5; pr++) {
                            Move mv = {sq, to, pr};
                            ml->moves[ml->count++] = mv;
                        }
                    } else {
                        Move mv = {sq, to, 0};
                        ml->moves[ml->count++] = mv;
                    }
                }
            }
        }
    } else if (type == 2) { // Knight
        int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
        int kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
        for (int i = 0; i < 8; i++) {
            int nr = r + kn_r[i], nc = c + kn_c[i];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int to = nr * 8 + nc;
                if (state->board[to] == 0 || (state->board[to] & 24) != color) {
                    Move mv = {sq, to, 0};
                    ml->moves[ml->count++] = mv;
                }
            }
        }
    } else if (type == 3 || type == 5) { // Bishop / Queen diagonal
        int b_r[] = {-1, -1, 1, 1};
        int b_c[] = {-1, 1, -1, 1};
        for (int i = 0; i < 4; i++) {
            int nr = r, nc = c;
            while (1) {
                nr += b_r[i]; nc += b_c[i];
                if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
                int to = nr * 8 + nc;
                if (state->board[to] == 0) {
                    Move mv = {sq, to, 0};
                    ml->moves[ml->count++] = mv;
                } else {
                    if ((state->board[to] & 24) != color) {
                        Move mv = {sq, to, 0};
                        ml->moves[ml->count++] = mv;
                    }
                    break;
                }
            }
        }
    }

    if (type == 4 || type == 5) { // Rook / Queen orthogonal
        int r_r[] = {-1, 1, 0, 0};
        int r_c[] = {0, 0, -1, 1};
        for (int i = 0; i < 4; i++) {
            int nr = r, nc = c;
            while (1) {
                nr += r_r[i]; nc += r_c[i];
                if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
                int to = nr * 8 + nc;
                if (state->board[to] == 0) {
                    Move mv = {sq, to, 0};
                    ml->moves[ml->count++] = mv;
                } else {
                    if ((state->board[to] & 24) != color) {
                        Move mv = {sq, to, 0};
                        ml->moves[ml->count++] = mv;
                    }
                    break;
                }
            }
        }
    } else if (type == 6) { // King
        int k_r[] = {-1, -1, -1, 0, 0, 1, 1, 1};
        int k_c[] = {-1, 0, 1, -1, 1, -1, 0, 1};
        for (int i = 0; i < 8; i++) {
            int nr = r + k_r[i], nc = c + k_c[i];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int to = nr * 8 + nc;
                if (state->board[to] == 0 || (state->board[to] & 24) != color) {
                    Move mv = {sq, to, 0};
                    ml->moves[ml->count++] = mv;
                }
            }
        }

        // Castling moves
        int enemy = (color == 8) ? 16 : 8;
        if (color == 8) {
            if ((state->castling & 1) && state->board[61] == 0 && state->board[62] == 0) {
                if (!is_square_attacked(state, 60, enemy) && !is_square_attacked(state, 61, enemy) && !is_square_attacked(state, 62, enemy)) {
                    Move mv = {60, 62, 0};
                    ml->moves[ml->count++] = mv;
                }
            }
            if ((state->castling & 2) && state->board[59] == 0 && state->board[58] == 0 && state->board[57] == 0) {
                if (!is_square_attacked(state, 60, enemy) && !is_square_attacked(state, 59, enemy) && !is_square_attacked(state, 58, enemy)) {
                    Move mv = {60, 58, 0};
                    ml->moves[ml->count++] = mv;
                }
            }
        } else {
            if ((state->castling & 4) && state->board[5] == 0 && state->board[6] == 0) {
                if (!is_square_attacked(state, 4, enemy) && !is_square_attacked(state, 5, enemy) && !is_square_attacked(state, 6, enemy)) {
                    Move mv = {4, 6, 0};
                    ml->moves[ml->count++] = mv;
                }
            }
            if ((state->castling & 8) && state->board[3] == 0 && state->board[2] == 0 && state->board[1] == 0) {
                if (!is_square_attacked(state, 4, enemy) && !is_square_attacked(state, 3, enemy) && !is_square_attacked(state, 2, enemy)) {
                    Move mv = {4, 2, 0};
                    ml->moves[ml->count++] = mv;
                }
            }
        }
    }
}

void generate_legal_moves(const BoardState *state, MoveList *ml);

void make_move(const BoardState *prev, BoardState *next, Move mv) {
    *next = *prev;

    uint8_t piece = prev->board[mv.from];
    int type = piece & 7;

    next->board[mv.to] = piece;
    next->board[mv.from] = 0;

    // Ep capture logic
    if (type == 1 && mv.to == prev->ep_square) {
        if (prev->turn == 8) {
            next->board[mv.to + 8] = 0;
        } else {
            next->board[mv.to - 8] = 0;
        }
    }

    // Ep update logic
    if (type == 1 && abs(mv.from - mv.to) == 16) {
        next->ep_square = (prev->turn == 8) ? mv.from - 8 : mv.from + 8;
    } else {
        next->ep_square = -1;
    }

    // Promotion logic
    if (type == 1 && mv.promotion != 0) {
        next->board[mv.to] = mv.promotion | prev->turn;
    }

    // Castling rook move execution
    if (type == 6) {
        if (mv.from == 60 && mv.to == 62) { next->board[61] = next->board[63]; next->board[63] = 0; }
        else if (mv.from == 60 && mv.to == 58) { next->board[59] = next->board[56]; next->board[56] = 0; }
        else if (mv.from == 4 && mv.to == 6) { next->board[5] = next->board[7]; next->board[7] = 0; }
        else if (mv.from == 4 && mv.to == 2) { next->board[3] = next->board[0]; next->board[0] = 0; }
    }

    // Update castling rights
    if (mv.from == 60) next->castling &= ~3;
    if (mv.from == 4) next->castling &= ~12;
    if (mv.from == 56 || mv.to == 56) next->castling &= ~2;
    if (mv.from == 63 || mv.to == 63) next->castling &= ~1;
    if (mv.from == 0 || mv.to == 0) next->castling &= ~8;
    if (mv.from == 7 || mv.to == 7) next->castling &= ~4;

    next->turn = (prev->turn == 8) ? 16 : 8;

    if (type == 1 || prev->board[mv.to] != 0) {
        next->halfmove = 0;
    } else {
        next->halfmove++;
    }

    if (prev->turn == 16) {
        next->fullmove++;
    }
}

void generate_legal_moves(const BoardState *state, MoveList *ml) {
    ml->count = 0;
    MoveList pseudo;
    pseudo.count = 0;
    for (int i = 0; i < 64; i++) {
        if (state->board[i] != 0 && (state->board[i] & 24) == state->turn) {
            generate_moves_for_piece(state, i, &pseudo);
        }
    }
    for (int i = 0; i < pseudo.count; i++) {
        BoardState temp;
        make_move(state, &temp, pseudo.moves[i]);
        if (!is_in_check(&temp, state->turn)) {
            ml->moves[ml->count++] = pseudo.moves[i];
        }
    }
}

// --- FEN & SAN String Parsers ---

void get_fen(const BoardState *state, char *fen) {
    int len = 0;
    for (int r = 0; r < 8; r++) {
        int empty = 0;
        for (int c = 0; c < 8; c++) {
            uint8_t p = state->board[r * 8 + c];
            if (p == 0) {
                empty++;
            } else {
                if (empty > 0) {
                    len += sprintf(fen + len, "%d", empty);
                    empty = 0;
                }
                char ch = " pnbrqk"[p & 7];
                if (p & 8) ch = ch - 'a' + 'A';
                len += sprintf(fen + len, "%c", ch);
            }
        }
        if (empty > 0) len += sprintf(fen + len, "%d", empty);
        if (r < 7) len += sprintf(fen + len, "/");
    }

    len += sprintf(fen + len, " %c ", (state->turn == 8) ? 'w' : 'b');

    int castl = 0;
    if (state->castling & 1) { len += sprintf(fen + len, "K"); castl = 1; }
    if (state->castling & 2) { len += sprintf(fen + len, "Q"); castl = 1; }
    if (state->castling & 4) { len += sprintf(fen + len, "k"); castl = 1; }
    if (state->castling & 8) { len += sprintf(fen + len, "q"); castl = 1; }
    if (!castl) len += sprintf(fen + len, "-");

    if (state->ep_square != -1) {
        len += sprintf(fen + len, " %c%c", 'a' + (state->ep_square % 8), '8' - (state->ep_square / 8));
    } else {
        len += sprintf(fen + len, " -");
    }

    len += sprintf(fen + len, " %d %d", state->halfmove, state->fullmove);
}

Move parse_move_str(const BoardState *state, const char *str) {
    Move mv = {-1, -1, 0};
    if (strlen(str) < 4) return mv;
    int f_col = str[0] - 'a';
    int f_row = '8' - str[1];
    int t_col = str[2] - 'a';
    int t_row = '8' - str[3];

    if (f_col < 0 || f_col > 7 || f_row < 0 || f_row > 7 || t_col < 0 || t_col > 7 || t_row < 0 || t_row > 7) {
        return mv;
    }

    mv.from = f_row * 8 + f_col;
    mv.to = t_row * 8 + t_col;

    if (str[4] != '\0' && str[4] != ' ' && str[4] != '\n' && str[4] != '\r') {
        char p = str[4];
        if (p == 'q') mv.promotion = 5;
        else if (p == 'r') mv.promotion = 4;
        else if (p == 'b') mv.promotion = 3;
        else if (p == 'n') mv.promotion = 2;
    }
    return mv;
}

void move_to_san(const BoardState *prev, Move mv, char *san) {
    BoardState temp;
    make_move(prev, &temp, mv);
    int type = prev->board[mv.from] & 7;

    if (type == 6) {
        if (mv.from == 60 && mv.to == 62) { strcpy(san, "O-O"); goto check_suffix; }
        if (mv.from == 60 && mv.to == 58) { strcpy(san, "O-O-O"); goto check_suffix; }
        if (mv.from == 4 && mv.to == 6) { strcpy(san, "O-O"); goto check_suffix; }
        if (mv.from == 4 && mv.to == 2) { strcpy(san, "O-O-O"); goto check_suffix; }
    }

    int len = 0;
    if (type == 1) {
        if (mv.to == prev->ep_square || prev->board[mv.to] != 0) {
            san[len++] = 'a' + (mv.from % 8);
            san[len++] = 'x';
        }
    } else {
        san[len++] = " PNBRQK"[type];
        MoveList ml;
        generate_legal_moves(prev, &ml);
        int duplicate_file = 0, duplicate_rank = 0, duplicates = 0;
        for (int i = 0; i < ml.count; i++) {
            Move other = ml.moves[i];
            if (other.from != mv.from && other.to == mv.to && (prev->board[other.from] & 7) == type && (prev->board[other.from] & 24) == (prev->board[mv.from] & 24)) {
                duplicates++;
                if (other.from % 8 == mv.from % 8) duplicate_file = 1;
                if (other.from / 8 == mv.from / 8) duplicate_rank = 1;
            }
        }
        if (duplicates > 0) {
            if (!duplicate_file) {
                san[len++] = 'a' + (mv.from % 8);
            } else if (!duplicate_rank) {
                san[len++] = '8' - (mv.from / 8);
            } else {
                san[len++] = 'a' + (mv.from % 8);
                san[len++] = '8' - (mv.from / 8);
            }
        }
        if (prev->board[mv.to] != 0) {
            san[len++] = 'x';
        }
    }

    san[len++] = 'a' + (mv.to % 8);
    san[len++] = '8' - (mv.to / 8);

    if (mv.promotion != 0) {
        san[len++] = '=';
        san[len++] = "  NBRQ"[mv.promotion];
    }
    san[len] = '\0';

check_suffix:;
    int opp_color = (prev->turn == 8) ? 16 : 8;
    if (is_in_check(&temp, opp_color)) {
        MoveList ml;
        generate_legal_moves(&temp, &ml);
        if (ml.count == 0) {
            strcat(san, "#");
        } else {
            strcat(san, "+");
        }
    }
}

void format_pgn(char *buf, int max_len) {
    int pos = 0;
    buf[0] = '\0';
    for (int i = 1; i <= history_count; i++) {
        if (i % 2 != 0) {
            pos += snprintf(buf + pos, max_len - pos, "%d. ", (i + 1) / 2);
        }
        pos += snprintf(buf + pos, max_len - pos, "%s ", san_history[i]);
        if (pos >= max_len - 12) break;
    }
}

// --- Display / GUI Functions ---

const char *get_piece_char(uint8_t p) {
    switch (p & 7) {
        case 1: return "♟";
        case 2: return "♞";
        case 3: return "♝";
        case 4: return "♜";
        case 5: return "♛";
        case 6: return "♚";
        default: return " ";
    }
}

void draw_interface(const BoardState *state, int cursor_sq, int selected_sq, TimeControl tc, int play_mode, int engine_thinking, Engine *eng) {
    printf("\033[H"); // Cursor Home - draws in place seamlessly!

    printf("\033[1;36m╔══════════════════════════════════════════════════════════════════════════════╗\033[0m\n");
    printf("\033[1;36m║                        ★  C-CHESS TERMINAL GUI  ★                            ║\033[0m\n");
    printf("\033[1;36m╚══════════════════════════════════════════════════════════════════════════════╝\033[0m\n");

    int white_in_check = is_in_check(state, 8);
    int black_in_check = is_in_check(state, 16);

    MoveList legal_moves;
    legal_moves.count = 0;
    if (selected_sq != -1) {
        generate_legal_moves(state, &legal_moves);
    }

    for (int r = 0; r < 8; r++) {
        printf("  %d ", 8 - r);

        for (int c = 0; c < 8; c++) {
            int sq = r * 8 + c;
            uint8_t p = state->board[sq];

            int is_dark = ((r + c) % 2 != 0);
            const char *bg = is_dark ? "\033[48;5;23m" : "\033[48;5;252m"; // Teal/Green & Soft Gray

            // Highlight King in Red if in check
            if ((p & 7) == 6) {
                if (((p & 8) && white_in_check) || ((p & 16) && black_in_check)) {
                    bg = "\033[48;5;124m"; // Red
                }
            }

            // Highlight last played move
            if (history_count > 0) {
                Move last = move_history[history_count];
                if (sq == last.from || sq == last.to) {
                    bg = "\033[48;5;67m"; // Sky-blue tint
                }
            }

            // Highlight selection
            if (sq == selected_sq) {
                bg = "\033[48;5;178m"; // Golden Yellow
            }

            // Highlight legal destinations
            if (selected_sq != -1) {
                for (int m = 0; m < legal_moves.count; m++) {
                    if (legal_moves.moves[m].from == selected_sq && legal_moves.moves[m].to == sq) {
                        bg = is_dark ? "\033[48;5;29m" : "\033[48;5;114m"; // Sage Green dots/shading
                    }
                }
            }

            // Cursor overlay
            if (sq == cursor_sq) {
                bg = "\033[48;5;201m"; // Vibrant Magenta
            }

            printf("%s", bg);
            if (p != 0) {
                const char *fg = (p & 8) ? "\033[38;5;231m\033[1m" : "\033[38;5;16m\033[1m"; // Crisp white or dark pieces
                printf(" %s%s%s  ", fg, get_piece_char(p), bg);
            } else {
                printf("    ");
            }
        }
        printf("\033[0m %d   ", 8 - r);

        // --- Vertical side pane ---
        printf("\033[1;37m│\033[0m ");
        switch (r) {
            case 0:
                printf("Mode: ");
                if (play_mode == 0) printf("\033[1;32mPlayer vs Player (PvP)\033[0m");
                else if (play_mode == 1) printf("\033[1;33mPlayer (White) vs Engine\033[0m");
                else if (play_mode == 2) printf("\033[1;33mEngine (White) vs Player (Black)\033[0m");
                else if (play_mode == 3) printf("\033[1;35mEngine vs Engine\033[0m");
                break;
            case 1:
                printf("Engine Status: ");
                if (!eng->active) printf("\033[1;31mNot Configured (PvP Only)\033[0m");
                else if (engine_thinking) printf("\033[5;1;33mThinking...\033[0m");
                else printf("\033[1;32mIdle\033[0m");
                break;
            case 2:
                printf("Time Control: \033[1;34m");
                if (tc.mode == 0) printf("Depth (%d plies)", tc.value);
                else if (tc.mode == 1) printf("Nodes (%d)", tc.value);
                else printf("MoveTime (%d ms)", tc.value);
                printf("\033[0m");
                break;
            case 3:
                printf("Turn: %s", (state->turn == 8) ? "\033[1;37mWhite ♙\033[0m" : "\033[1;30mBlack ♟\033[0m");
                break;
            case 4:
                printf("Cursor: \033[1;35m%c%d\033[0m", 'a' + (cursor_sq % 8), 8 - (cursor_sq / 8));
                if (selected_sq != -1) {
                    printf(" | Selected: \033[1;33m%c%d\033[0m", 'a' + (selected_sq % 8), 8 - (selected_sq / 8));
                } else {
                    printf(" | Selected: None");
                }
                break;
            case 5:
                printf("Last Move: ");
                if (history_count > 0) {
                    Move last = move_history[history_count];
                    printf("\033[1;32m%s\033[0m (%c%d → %c%d)", san_history[history_count], 'a' + (last.from % 8), 8 - (last.from / 8), 'a' + (last.to % 8), 8 - (last.to / 8));
                } else {
                    printf("None");
                }
                break;
            case 6:
                printf("Game Status: ");
                {
                    MoveList ml;
                    generate_legal_moves(state, &ml);
                    if (ml.count == 0) {
                        if (is_in_check(state, state->turn)) {
                            printf("\033[1;31mCHECKMATE (%s wins)\033[0m", (state->turn == 8) ? "Black" : "White");
                        } else {
                            printf("\033[1;33mSTALEMATE (Draw)\033[0m");
                        }
                    } else if (is_in_check(state, state->turn)) {
                        printf("\033[1;31mCHECK!\033[0m");
                    } else {
                        printf("\033[1;32mPlaying...\033[0m");
                    }
                }
                break;
            case 7:
                printf("Engine Path: \033[3m%s\033[0m", eng->active ? eng->path : "N/A");
                break;
        }
        printf("\n");
    }
    printf("     a    b    c    d    e    f    g    h\n\n");

    // --- Interactive Control Panel ---
    printf("\033[1;36m┌─────────────────────────────── CONTROLS ─────────────────────────────────────┐\033[0m\n");
    printf("│ \033[1mArrows/WASD\033[0m: Move cursor     │ \033[1mSpace/Enter\033[0m: Select/Place piece          │\n");
    printf("│ \033[1mu\033[0m: Undo last moves             │ \033[1mm\033[0m: Toggle Game Modes (PvP vs Engine)      │\n");
    printf("│ \033[1mc\033[0m: Cycle TC constraints Mode   │ \033[1m+/-\033[0m: Adjust TC configuration                │\n");
    printf("│ \033[1mp\033[0m: Manually assign Engine path │ \033[1mq\033[0m: Quit match immediately                 │\n");
    printf("\033[1;36m└──────────────────────────────────────────────────────────────────────────────┘\033[0m\n");

    // Display formatted live PGN log
    printf("\033[1mPGN Log:\033[0m ");
    char pgn[2048];
    format_pgn(pgn, sizeof(pgn));
    if (strlen(pgn) == 0) {
        printf("\033[3mNo moves recorded yet.\033[0m\n");
    } else {
        if (strlen(pgn) > 75) {
            printf("... %s\n", pgn + strlen(pgn) - 75);
        } else {
            printf("%s\n", pgn);
        }
    }
}

// --- Main Event Loop ---

int main() {
    printf("\033[2J"); // Clear Screen Initially

    Engine eng;
    eng.active = 0;
    auto_detect_engine(&eng);

    BoardState initial_state;
    init_board(&initial_state);
    history[0] = initial_state;
    history_count = 0;

    int cursor_sq = 60; // Start selection on White King's square
    int selected_sq = -1;
    TimeControl tc = {2, 1000}; // MoveTime (1000ms) by default
    int play_mode = 0; // PvP default
    int engine_thinking = 0;
    int running = 1;
    int draw_needed = 1;

    enable_raw_mode();

    while (running) {
        // Handle Engine Play Action
        if (eng.active && !engine_thinking) {
            int active_color = history[history_count].turn;
            int is_engine_turn = 0;
            if (play_mode == 1 && active_color == 16) is_engine_turn = 1;
            else if (play_mode == 2 && active_color == 8) is_engine_turn = 1;
            else if (play_mode == 3) is_engine_turn = 1;

            if (is_engine_turn) {
                MoveList ml;
                generate_legal_moves(&history[history_count], &ml);
                if (ml.count > 0) {
                    char fen[512];
                    get_fen(&history[history_count], fen);

                    char cmd[1024];
                    sprintf(cmd, "position fen %s\n", fen);
                    write(eng.in_pipe[1], cmd, strlen(cmd));

                    if (tc.mode == 0) {
                        sprintf(cmd, "go depth %d\n", tc.value);
                    } else if (tc.mode == 1) {
                        sprintf(cmd, "go nodes %d\n", tc.value);
                    } else {
                        sprintf(cmd, "go movetime %d\n", tc.value);
                    }
                    write(eng.in_pipe[1], cmd, strlen(cmd));

                    engine_thinking = 1;
                    draw_needed = 1;
                }
            }
        }

        if (draw_needed) {
            draw_interface(&history[history_count], cursor_sq, selected_sq, tc, play_mode, engine_thinking, &eng);
            fflush(stdout);
            draw_needed = 0;
        }

        struct pollfd fds[2];
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        fds[1].fd = eng.active ? eng.out_pipe[0] : -1;
        fds[1].events = POLLIN;

        int ret = poll(fds, eng.active ? 2 : 1, 30);
        if (ret > 0) {
            // Process Player Inputs
            if (fds[0].revents & POLLIN) {
                char c;
                if (read(STDIN_FILENO, &c, 1) > 0) {
                    if (c == '\033') { // Arrow keys escape parsing
                        char seq[3];
                        if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0) {
                            if (seq[0] == '[') {
                                int r = cursor_sq / 8;
                                int c_col = cursor_sq % 8;
                                switch (seq[1]) {
                                    case 'A': if (r > 0) r--; break; // Up
                                    case 'B': if (r < 7) r++; break; // Down
                                    case 'C': if (c_col < 7) c_col++; break; // Right
                                    case 'D': if (c_col > 0) c_col--; break; // Left
                                }
                                cursor_sq = r * 8 + c_col;
                                draw_needed = 1;
                            }
                        }
                    } else {
                        int r = cursor_sq / 8;
                        int c_col = cursor_sq % 8;
                        // WASD navigation
                        if (c == 'w' || c == 'W') { if (r > 0) r--; cursor_sq = r * 8 + c_col; draw_needed = 1; }
                        else if (c == 's' || c == 'S') { if (r < 7) r++; cursor_sq = r * 8 + c_col; draw_needed = 1; }
                        else if (c == 'd' || c == 'D') { if (c_col < 7) c_col++; cursor_sq = r * 8 + c_col; draw_needed = 1; }
                        else if (c == 'a' || c == 'A') { if (c_col > 0) c_col--; cursor_sq = r * 8 + c_col; draw_needed = 1; }
                        // Move execution
                        else if (c == ' ' || c == '\n' || c == '\r') {
                            if (!engine_thinking) {
                                if (selected_sq == -1) {
                                    uint8_t piece = history[history_count].board[cursor_sq];
                                    if (piece != 0 && (piece & 24) == history[history_count].turn) {
                                        selected_sq = cursor_sq;
                                    }
                                } else {
                                    MoveList ml;
                                    generate_legal_moves(&history[history_count], &ml);
                                    int valid = 0;
                                    Move target_mv;
                                    for (int m = 0; m < ml.count; m++) {
                                        if (ml.moves[m].from == selected_sq && ml.moves[m].to == cursor_sq) {
                                            valid = 1;
                                            target_mv = ml.moves[m];
                                            break;
                                        }
                                    }
                                    if (valid) {
                                        BoardState next;
                                        make_move(&history[history_count], &next, target_mv);
                                        move_to_san(&history[history_count], target_mv, san_history[history_count + 1]);
                                        history_count++;
                                        history[history_count] = next;
                                        move_history[history_count] = target_mv;
                                        selected_sq = -1;
                                    } else {
                                        uint8_t piece = history[history_count].board[cursor_sq];
                                        if (piece != 0 && (piece & 24) == history[history_count].turn) {
                                            selected_sq = cursor_sq;
                                        } else {
                                            selected_sq = -1;
                                        }
                                    }
                                }
                                draw_needed = 1;
                            }
                        }
                        // Move Undo (takeback)
                        else if (c == 'u' || c == 'U') {
                            if (!engine_thinking) {
                                if (play_mode != 0 && history_count >= 2) {
                                    history_count -= 2;
                                } else if (history_count >= 1) {
                                    history_count -= 1;
                                }
                                selected_sq = -1;
                                draw_needed = 1;
                            }
                        }
                        // Change Match Mode
                        else if (c == 'm' || c == 'M') {
                            play_mode = (play_mode + 1) % 4;
                            selected_sq = -1;
                            draw_needed = 1;
                        }
                        // Time Control constraints switching
                        else if (c == 'c' || c == 'C') {
                            tc.mode = (tc.mode + 1) % 3;
                            if (tc.mode == 0) tc.value = 10;
                            else if (tc.mode == 1) tc.value = 100000;
                            else tc.value = 1000;
                            draw_needed = 1;
                        }
                        // Adjust parameter metrics up (+)
                        else if (c == '+' || c == '=') {
                            if (tc.mode == 0) {
                                if (tc.value < 30) tc.value++;
                            } else if (tc.mode == 1) {
                                if (tc.value < 5000000) tc.value += 50000;
                            } else {
                                if (tc.value < 60000) tc.value += 500;
                            }
                            draw_needed = 1;
                        }
                        // Adjust parameter metrics down (-)
                        else if (c == '-') {
                            if (tc.mode == 0) {
                                if (tc.value > 1) tc.value--;
                            } else if (tc.mode == 1) {
                                if (tc.value > 10000) tc.value -= 50000;
                            } else {
                                if (tc.value > 100) tc.value -= 500;
                            }
                            draw_needed = 1;
                        }
                        // Configure Engine Custom Path
                        else if (c == 'p' || c == 'P') {
                            disable_raw_mode();
                            printf("\n\033[1;36m[Custom Engine Path Configuration]\033[0m\n");
                            printf("Path structure: ");
                            fflush(stdout);
                            char input_path[256];
                            if (fgets(input_path, sizeof(input_path), stdin)) {
                                input_path[strcspn(input_path, "\n")] = 0;
                                if (strlen(input_path) > 0) {
                                    if (engine_start(&eng, input_path)) {
                                        printf("\033[1;32mEngine connected successfully!\033[0m Pres key to return...");
                                    } else {
                                        printf("\033[1;31mConnection failed. Invalid engine executable path.\033[0m Press key to return...");
                                    }
                                }
                            }
                            fflush(stdout);
                            getchar();
                            enable_raw_mode();
                            printf("\033[2J"); // Complete redraw
                            draw_needed = 1;
                        }
                        // Quit game
                        else if (c == 'q' || c == 'Q') {
                            running = 0;
                        }
                    }
                }
            }

            // Process Engine Pipeline Actions
            if (eng.active && (fds[1].revents & POLLIN)) {
                char line[1024];
                while (get_engine_line(eng.out_pipe[0], line, sizeof(line))) {
                    if (strstr(line, "bestmove") == line) {
                        char mv_str[32] = {0};
                        sscanf(line, "bestmove %s", mv_str);
                        Move mv = parse_move_str(&history[history_count], mv_str);
                        if (mv.from != -1 && mv.to != -1) {
                            BoardState next;
                            make_move(&history[history_count], &next, mv);
                            move_to_san(&history[history_count], mv, san_history[history_count + 1]);
                            history_count++;
                            history[history_count] = next;
                            move_history[history_count] = mv;
                        }
                        engine_thinking = 0;
                        draw_needed = 1;
                    }
                }
            }
        }
    }

    disable_raw_mode();
    engine_stop(&eng);
    return 0;
}
