#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <termios.h>
#include <signal.h>

// Piece types
enum { EMPTY = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };
enum { WHITE = 1, BLACK = -1 };

// Keyboard codes
enum {
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_ESC
};

// State representation
typedef struct {
    int board_type[64];
    int board_color[64];
    int active_color;
    int castling_rights; // Bitmask: 1=W_OO, 2=W_OOO, 4=B_OO, 8=B_OOO
    int ep_square;       // ep target square index, or -1
    int halfmove_clock;
    int fullmove_number;
} GameState;

typedef struct {
    int from;
    int to;
    int promo;
    int piece;
    char san[16];
} MoveRecord;

// Global settings and state
GameState current_state;
#define MAX_HISTORY 2048
GameState history[MAX_HISTORY];
int history_count = 0;

MoveRecord move_records[MAX_HISTORY];
int move_record_count = 0;

int cursor_square = 60; // E1
int selected_square = -1;

bool engine_mode = true; 
bool engine_available = false;
bool thinking = false;
pid_t engine_pid = -1;
int to_engine[2], from_engine[2];

// Time control configurations
int tc_type = 0; // 0 = Time (ms), 1 = Depth (plies), 2 = Nodes
int tc_value[3] = {5000, 10, 100000}; // Default limits

// Interactive promotion tracking
bool promoting_state = false;
int promo_from = -1;
int promo_to = -1;

// Base layout setup
const int initial_board_type[64] = {
    ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK,
    PAWN, PAWN, PAWN, PAWN, PAWN, PAWN, PAWN, PAWN,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    PAWN, PAWN, PAWN, PAWN, PAWN, PAWN, PAWN, PAWN,
    ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK
};
const int initial_board_color[64] = {
    BLACK, BLACK, BLACK, BLACK, BLACK, BLACK, BLACK, BLACK,
    BLACK, BLACK, BLACK, BLACK, BLACK, BLACK, BLACK, BLACK,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    WHITE, WHITE, WHITE, WHITE, WHITE, WHITE, WHITE, WHITE,
    WHITE, WHITE, WHITE, WHITE, WHITE, WHITE, WHITE, WHITE
};

// Forward Declarations
void draw_screen();
bool is_legal_move(int from, int to, int promo);
void apply_move_to_state(GameState* s, int from, int to, int promo);
bool is_king_in_check_state(GameState* s, int color);
void get_pseudolegal_moves_state(GameState* s, int sq, int* moves, int* count);
bool is_square_attacked_state(GameState* s, int sq, int attacker_color);
void send_to_engine(const char* cmd);
void trigger_engine_move();
void cleanup();

// Terminal Raw Mode Logic
struct termios orig_termios;
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\x1b[?25h\x1b[H\x1b[2J"); // Show cursor, clear screen
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(cleanup);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int read_key() {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        if (c == '\x1b') {
            char seq[2];
            struct timeval tv = {0, 50000};
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
                if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                    if (seq[0] == '[') {
                        switch (seq[1]) {
                            case 'A': return KEY_UP;
                            case 'B': return KEY_DOWN;
                            case 'C': return KEY_RIGHT;
                            case 'D': return KEY_LEFT;
                        }
                    }
                }
            }
            return KEY_ESC;
        }
        return c;
    }
    return 0;
}

// Chess Logic implementation
void init_game() {
    for (int i = 0; i < 64; i++) {
        current_state.board_type[i] = initial_board_type[i];
        current_state.board_color[i] = initial_board_color[i];
    }
    current_state.active_color = WHITE;
    current_state.castling_rights = 15;
    current_state.ep_square = -1;
    current_state.halfmove_clock = 0;
    current_state.fullmove_number = 1;

    history_count = 0;
    move_record_count = 0;
    selected_square = -1;
    cursor_square = 60;
    promoting_state = false;
}

bool is_legal_move(int from, int to, int promo) {
    if (from < 0 || from >= 64 || to < 0 || to >= 64) return false;
    if (current_state.board_color[from] != current_state.active_color) return false;

    int moves[64];
    int count = 0;
    get_pseudolegal_moves_state(&current_state, from, moves, &count);

    bool found = false;
    for (int i = 0; i < count; i++) {
        if (moves[i] == to) {
            found = true;
            break;
        }
    }
    if (!found) return false;

    GameState temp = current_state;
    apply_move_to_state(&temp, from, to, promo);
    if (is_king_in_check_state(&temp, current_state.active_color)) {
        return false;
    }
    return true;
}

void apply_move_to_state(GameState* s, int from, int to, int promo) {
    int piece = s->board_type[from];
    int color = s->board_color[from];
    int target = s->board_type[to];

    if (piece == PAWN && to == s->ep_square) {
        int ep_pawn_sq = (s->active_color == WHITE) ? (to + 8) : (to - 8);
        s->board_type[ep_pawn_sq] = 0;
        s->board_color[ep_pawn_sq] = 0;
    }

    s->board_type[to] = (piece == PAWN && (to / 8 == 0 || to / 8 == 7)) ? promo : piece;
    s->board_color[to] = color;
    s->board_type[from] = 0;
    s->board_color[from] = 0;

    if (piece == KING) {
        if (to - from == 2) {
            s->board_type[from + 1] = ROOK;
            s->board_color[from + 1] = color;
            s->board_type[from + 3] = 0;
            s->board_color[from + 3] = 0;
        } else if (to - from == -2) {
            s->board_type[from - 1] = ROOK;
            s->board_color[from - 1] = color;
            s->board_type[from - 4] = 0;
            s->board_color[from - 4] = 0;
        }
    }

    if (piece == PAWN && abs(to - from) == 16) {
        s->ep_square = (color == WHITE) ? (from - 8) : (from + 8);
    } else {
        s->ep_square = -1;
    }

    if (piece == KING) {
        if (color == WHITE) s->castling_rights &= ~3;
        if (color == BLACK) s->castling_rights &= ~12;
    }
    if (piece == ROOK) {
        if (from == 56) s->castling_rights &= ~2;
        if (from == 63) s->castling_rights &= ~1;
        if (from == 0) s->castling_rights &= ~8;
        if (from == 7) s->castling_rights &= ~4;
    }
    if (target == ROOK) {
        if (to == 56) s->castling_rights &= ~2;
        if (to == 63) s->castling_rights &= ~1;
        if (to == 0) s->castling_rights &= ~8;
        if (to == 7) s->castling_rights &= ~4;
    }

    if (piece == PAWN || target != 0) {
        s->halfmove_clock = 0;
    } else {
        s->halfmove_clock++;
    }

    if (s->active_color == BLACK) {
        s->fullmove_number++;
    }
    s->active_color = -s->active_color;
}

void get_pseudolegal_moves_state(GameState* s, int sq, int* moves, int* count) {
    int piece = s->board_type[sq];
    int color = s->board_color[sq];
    if (piece == 0) return;

    int r = sq / 8, c = sq % 8;
    if (piece == KNIGHT) {
        int dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
        int dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
        for (int i = 0; i < 8; i++) {
            int nr = r + dr[i], nc = c + dc[i];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int nsq = nr * 8 + nc;
                if (s->board_color[nsq] != color) moves[(*count)++] = nsq;
            }
        }
    } else if (piece == BISHOP || piece == QUEEN) {
        int dr[] = {-1, -1, 1, 1};
        int dc[] = {-1, 1, -1, 1};
        for (int d = 0; d < 4; d++) {
            for (int i = 1; i < 8; i++) {
                int nr = r + dr[d]*i, nc = c + dc[d]*i;
                if (nr < 0 || nr > 7 || nc < 0 || nc > 7) break;
                int nsq = nr * 8 + nc;
                if (s->board_color[nsq] == 0) {
                    moves[(*count)++] = nsq;
                } else {
                    if (s->board_color[nsq] == -color) moves[(*count)++] = nsq;
                    break;
                }
            }
        }
    }
    if (piece == ROOK || piece == QUEEN) {
        int dr[] = {-1, 1, 0, 0};
        int dc[] = {0, 0, -1, 1};
        for (int d = 0; d < 4; d++) {
            for (int i = 1; i < 8; i++) {
                int nr = r + dr[d]*i, nc = c + dc[d]*i;
                if (nr < 0 || nr > 7 || nc < 0 || nc > 7) break;
                int nsq = nr * 8 + nc;
                if (s->board_color[nsq] == 0) {
                    moves[(*count)++] = nsq;
                } else {
                    if (s->board_color[nsq] == -color) moves[(*count)++] = nsq;
                    break;
                }
            }
        }
    } else if (piece == KING) {
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                int nr = r + dr, nc = c + dc;
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                    int nsq = nr * 8 + nc;
                    if (s->board_color[nsq] != color) moves[(*count)++] = nsq;
                }
            }
        }
        if (color == WHITE && sq == 60) {
            if ((s->castling_rights & 1) && s->board_color[61] == 0 && s->board_color[62] == 0 &&
                !is_square_attacked_state(s, 60, BLACK) && !is_square_attacked_state(s, 61, BLACK) && !is_square_attacked_state(s, 62, BLACK)) {
                moves[(*count)++] = 62;
            }
            if ((s->castling_rights & 2) && s->board_color[59] == 0 && s->board_color[58] == 0 && s->board_color[57] == 0 &&
                !is_square_attacked_state(s, 60, BLACK) && !is_square_attacked_state(s, 59, BLACK) && !is_square_attacked_state(s, 58, BLACK)) {
                moves[(*count)++] = 58;
            }
        } else if (color == BLACK && sq == 4) {
            if ((s->castling_rights & 4) && s->board_color[5] == 0 && s->board_color[6] == 0 &&
                !is_square_attacked_state(s, 4, WHITE) && !is_square_attacked_state(s, 5, WHITE) && !is_square_attacked_state(s, 6, WHITE)) {
                moves[(*count)++] = 6;
            }
            if ((s->castling_rights & 8) && s->board_color[3] == 0 && s->board_color[2] == 0 && s->board_color[1] == 0 &&
                !is_square_attacked_state(s, 4, WHITE) && !is_square_attacked_state(s, 3, WHITE) && !is_square_attacked_state(s, 2, WHITE)) {
                moves[(*count)++] = 2;
            }
        }
    } else if (piece == PAWN) {
        int dir = (color == WHITE) ? -1 : 1;
        int start_row = (color == WHITE) ? 6 : 1;
        int nr = r + dir;
        if (nr >= 0 && nr < 8) {
            int nsq = nr * 8 + c;
            if (s->board_color[nsq] == 0) {
                moves[(*count)++] = nsq;
                if (r == start_row) {
                    int nsq2 = (r + 2 * dir) * 8 + c;
                    if (s->board_color[nsq2] == 0) moves[(*count)++] = nsq2;
                }
            }
        }
        int dcs[] = {-1, 1};
        for (int i = 0; i < 2; i++) {
            int nc = c + dcs[i];
            if (nc >= 0 && nc < 8) {
                int nsq = nr * 8 + nc;
                if (s->board_color[nsq] == -color) {
                    moves[(*count)++] = nsq;
                } else if (nsq == s->ep_square) {
                    moves[(*count)++] = nsq;
                }
            }
        }
    }
}

bool is_square_attacked_state(GameState* s, int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;

    int k_dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int k_dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + k_dr[i], nc = c + k_dc[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            int target = nr * 8 + nc;
            if (s->board_type[target] == KNIGHT && s->board_color[target] == attacker_color) return true;
        }
    }

    int d_dr[] = {-1, -1, 1, 1};
    int d_dc[] = {-1, 1, -1, 1};
    for (int d = 0; d < 4; d++) {
        for (int i = 1; i < 8; i++) {
            int nr = r + d_dr[d]*i, nc = c + d_dc[d]*i;
            if (nr < 0 || nr > 7 || nc < 0 || nc > 7) break;
            int target = nr * 8 + nc;
            if (s->board_color[target] != 0) {
                if (s->board_color[target] == attacker_color &&
                    (s->board_type[target] == BISHOP || s->board_type[target] == QUEEN)) return true;
                break;
            }
        }
    }

    int s_dr[] = {-1, 1, 0, 0};
    int s_dc[] = {0, 0, -1, 1};
    for (int d = 0; d < 4; d++) {
        for (int i = 1; i < 8; i++) {
            int nr = r + s_dr[d]*i, nc = c + s_dc[d]*i;
            if (nr < 0 || nr > 7 || nc < 0 || nc > 7) break;
            int target = nr * 8 + nc;
            if (s->board_color[target] != 0) {
                if (s->board_color[target] == attacker_color &&
                    (s->board_type[target] == ROOK || s->board_type[target] == QUEEN)) return true;
                break;
            }
        }
    }

    int p_dir = (attacker_color == WHITE) ? 1 : -1;
    int p_row = r + p_dir;
    if (p_row >= 0 && p_row < 8) {
        int dcs[] = {-1, 1};
        for (int i = 0; i < 2; i++) {
            int p_col = c + dcs[i];
            if (p_col >= 0 && p_col < 8) {
                int target = p_row * 8 + p_col;
                if (s->board_type[target] == PAWN && s->board_color[target] == attacker_color) return true;
            }
        }
    }

    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = r + dr, nc = c + dc;
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int target = nr * 8 + nc;
                if (s->board_type[target] == KING && s->board_color[target] == attacker_color) return true;
            }
        }
    }
    return false;
}

bool is_king_in_check_state(GameState* s, int color) {
    int king_sq = -1;
    for (int i = 0; i < 64; i++) {
        if (s->board_type[i] == KING && s->board_color[i] == color) {
            king_sq = i;
            break;
        }
    }
    if (king_sq == -1) return false;
    return is_square_attacked_state(s, king_sq, -color);
}

// Generate PGN Short Algebraic Notation (SAN)
void generate_san(int from, int to, int promo, char* san) {
    int piece = current_state.board_type[from];
    int color = current_state.board_color[from];
    int target = current_state.board_type[to];

    if (piece == KING) {
        if (to - from == 2) {
            strcpy(san, "O-O");
            return;
        }
        if (to - from == -2) {
            strcpy(san, "O-O-O");
            return;
        }
    }

    int len = 0;
    if (piece != PAWN) {
        char piece_chars[] = {'?', 'P', 'N', 'B', 'R', 'Q', 'K'};
        san[len++] = piece_chars[piece];

        bool dup_file = false, dup_rank = false, multi = false;
        for (int i = 0; i < 64; i++) {
            if (i == from) continue;
            if (current_state.board_type[i] == piece && current_state.board_color[i] == color) {
                if (is_legal_move(i, to, QUEEN)) {
                    multi = true;
                    if (i % 8 == from % 8) dup_file = true;
                    if (i / 8 == from / 8) dup_rank = true;
                }
            }
        }
        if (multi) {
            if (!dup_file) {
                san[len++] = 'a' + (from % 8);
            } else if (!dup_rank) {
                san[len++] = '8' - (from / 8);
            } else {
                san[len++] = 'a' + (from % 8);
                san[len++] = '8' - (from / 8);
            }
        }
    } else {
        if (target != 0 || to == current_state.ep_square) {
            san[len++] = 'a' + (from % 8);
        }
    }

    if (target != 0 || (piece == PAWN && to == current_state.ep_square)) {
        san[len++] = 'x';
    }

    san[len++] = 'a' + (to % 8);
    san[len++] = '8' - (to / 8);

    if (piece == PAWN && (to / 8 == 0 || to / 8 == 7)) {
        san[len++] = '=';
        char promo_chars[] = {'?', 'P', 'N', 'B', 'R', 'Q', 'K'};
        san[len++] = promo_chars[promo];
    }
    san[len] = '\0';

    GameState temp_state = current_state;
    apply_move_to_state(&temp_state, from, to, promo);
    if (is_king_in_check_state(&temp_state, temp_state.active_color)) {
        bool has_legal = false;
        for (int f = 0; f < 64; f++) {
            if (temp_state.board_color[f] == temp_state.active_color) {
                int moves[64];
                int count = 0;
                get_pseudolegal_moves_state(&temp_state, f, moves, &count);
                for (int m = 0; m < count; m++) {
                    GameState test_state = temp_state;
                    apply_move_to_state(&test_state, f, moves[m], QUEEN);
                    if (!is_king_in_check_state(&test_state, test_state.active_color)) {
                        has_legal = true;
                        break;
                    }
                }
            }
            if (has_legal) break;
        }
        if (!has_legal) {
            strcat(san, "#");
        } else {
            strcat(san, "+");
        }
    }
}

int get_game_status() {
    bool has_legal = false;
    int active_color = current_state.active_color;
    for (int f = 0; f < 64; f++) {
        if (current_state.board_color[f] == active_color) {
            int moves[64];
            int count = 0;
            get_pseudolegal_moves_state(&current_state, f, moves, &count);
            for (int m = 0; m < count; m++) {
                GameState temp = current_state;
                apply_move_to_state(&temp, f, moves[m], QUEEN);
                if (!is_king_in_check_state(&temp, active_color)) {
                    has_legal = true;
                    break;
                }
            }
        }
        if (has_legal) break;
    }
    if (!has_legal) {
        if (is_king_in_check_state(&current_state, active_color)) {
            return 1; // Checkmate
        } else {
            return 2; // Stalemate
        }
    }
    return 0; // Game in progress
}

// Convert coordinate move to array indices (e.g. "e2e4" -> from=52, to=36)
bool parse_coordinate_move(const char* str, int* from, int* to, int* promo) {
    if (strlen(str) < 4) return false;
    int fc = str[0] - 'a';
    int fr = '8' - str[1];
    int tc = str[2] - 'a';
    int tr = '8' - str[3];
    if (fc < 0 || fc > 7 || fr < 0 || fr > 7 || tc < 0 || tc > 7 || tr < 0 || tr > 7) {
        return false;
    }
    *from = fr * 8 + fc;
    *to = tr * 8 + tc;
    *promo = QUEEN;
    if (strlen(str) >= 5) {
        char p = str[4];
        if (p == 'n' || p == 'N') *promo = KNIGHT;
        else if (p == 'b' || p == 'B') *promo = BISHOP;
        else if (p == 'r' || p == 'R') *promo = ROOK;
        else *promo = QUEEN;
    }
    return true;
}

void get_coord_move_str(MoveRecord rec, char* out) {
    int f = rec.from, t = rec.to;
    int fc = f % 8, fr = 8 - (f / 8);
    int tc = t % 8, tr = 8 - (t / 8);
    char pchar = '\0';
    if (rec.piece == PAWN && (t / 8 == 0 || t / 8 == 7)) {
        if (rec.promo == KNIGHT) pchar = 'n';
        else if (rec.promo == BISHOP) pchar = 'b';
        else if (rec.promo == ROOK) pchar = 'r';
        else pchar = 'q';
    }
    if (pchar) {
        sprintf(out, "%c%d%c%d%c", 'a' + fc, fr, 'a' + tc, tr, pchar);
    } else {
        sprintf(out, "%c%d%c%d", 'a' + fc, fr, 'a' + tc, tr);
    }
}

// Subprocess UCI Engine Communication
void start_engine(const char* path) {
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) return;
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[1]);
        close(from_engine[0]);
        execlp(path, path, NULL);
        exit(1);
    }
    close(to_engine[0]);
    close(from_engine[1]);
    fcntl(from_engine[0], F_SETFL, O_NONBLOCK);
}

void send_to_engine(const char* cmd) {
    if (engine_pid > 0) {
        write(to_engine[1], cmd, strlen(cmd));
    }
}

void verify_engine() {
    start_engine("stockfish");
    if (engine_pid > 0) {
        send_to_engine("uci\n");
        char line[1024];
        int line_len = 0;
        bool ok = false;
        struct timeval tv = {1, 0};
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(from_engine[0], &fds);
        int sel = select(from_engine[0] + 1, &fds, NULL, NULL, &tv);
        if (sel > 0) {
            char c;
            while (read(from_engine[0], &c, 1) == 1) {
                if (c == '\n' || c == '\r') {
                    line[line_len] = '\0';
                    if (strncmp(line, "uciok", 5) == 0) {
                        ok = true;
                        break;
                    }
                    line_len = 0;
                } else {
                    if (line_len < 1023) line[line_len++] = c;
                }
            }
        }
        if (ok) {
            engine_available = true;
            send_to_engine("isready\n");
        } else {
            kill(engine_pid, SIGKILL);
            engine_pid = -1;
            engine_available = false;
            engine_mode = false;
        }
    } else {
        engine_available = false;
        engine_mode = false;
    }
}

void trigger_engine_move() {
    if (!engine_available || !engine_mode) return;
    thinking = true;
    draw_screen();

    char cmd[16384] = "position startpos";
    if (move_record_count > 0) {
        strcat(cmd, " moves");
        for (int i = 0; i < move_record_count; i++) {
            char mstr[12];
            get_coord_move_str(move_records[i], mstr);
            strcat(cmd, " ");
            strcat(cmd, mstr);
        }
    }
    strcat(cmd, "\n");
    send_to_engine(cmd);

    char go_cmd[128];
    if (tc_type == 0) {
        sprintf(go_cmd, "go wtime %d btime %d\n", tc_value[0], tc_value[0]);
    } else if (tc_type == 1) {
        sprintf(go_cmd, "go depth %d\n", tc_value[1]);
    } else {
        sprintf(go_cmd, "go nodes %d\n", tc_value[2]);
    }
    send_to_engine(go_cmd);

    char line[4096];
    int line_len = 0;
    bool got_move = false;
    char bestmove_str[100] = "";

    while (!got_move) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(from_engine[0], &fds);
        struct timeval tv = {0, 100000};
        int sel = select(from_engine[0] + 1, &fds, NULL, NULL, &tv);
        if (sel > 0) {
            char c;
            while (read(from_engine[0], &c, 1) == 1) {
                if (c == '\n' || c == '\r') {
                    line[line_len] = '\0';
                    if (line_len > 0) {
                        if (strncmp(line, "bestmove ", 9) == 0) {
                            strcpy(bestmove_str, line + 9);
                            got_move = true;
                            break;
                        }
                    }
                    line_len = 0;
                } else {
                    if (line_len < 4095) line[line_len++] = c;
                }
            }
        }
    }

    thinking = false;
    if (got_move) {
        char mv[10] = "";
        sscanf(bestmove_str, "%s", mv);
        int from, to, promo;
        if (parse_coordinate_move(mv, &from, &to, &promo)) {
            char san[16];
            generate_san(from, to, promo, san);

            history[history_count++] = current_state;
            move_records[move_record_count++] = (MoveRecord){
                .from = from,
                .to = to,
                .promo = promo,
                .piece = current_state.board_type[from],
                .san = ""
            };
            strcpy(move_records[move_record_count - 1].san, san);
            apply_move_to_state(&current_state, from, to, promo);
        }
    }
}

void undo_move() {
    if (history_count > 0) {
        if (engine_mode && history_count >= 2) {
            history_count -= 2;
            current_state = history[history_count];
            move_record_count -= 2;
        } else if (!engine_mode && history_count >= 1) {
            history_count -= 1;
            current_state = history[history_count];
            move_record_count -= 1;
        }
        selected_square = -1;
    }
}

// Screen Rendering and Formatting
const char* get_piece_symbol(int type) {
    switch(type) {
        case PAWN:   return "P";
        case KNIGHT: return "N";
        case BISHOP: return "B";
        case ROOK:   return "R";
        case QUEEN:  return "Q";
        case KING:   return "K";
    }
    return " ";
}

void set_square_color(int sq) {
    int r = sq / 8, c = sq % 8;
    bool is_light = (r + c) % 2 == 0;

    bool is_curr = (sq == cursor_square);
    bool is_sel = (sq == selected_square);
    bool is_target = false;
    if (selected_square != -1) {
        is_target = is_legal_move(selected_square, sq, QUEEN);
    }
    bool is_last_move = false;
    if (move_record_count > 0) {
        int lf = move_records[move_record_count - 1].from;
        int lt = move_records[move_record_count - 1].to;
        if (sq == lf || sq == lt) is_last_move = true;
    }

    if (is_curr) {
        printf("\x1b[48;5;220m\x1b[30m"); // Bright Yellow
    } else if (is_sel) {
        printf("\x1b[48;5;34m\x1b[38;5;15m"); // Emerald Green
    } else if (is_target) {
        if (is_light) {
            printf("\x1b[48;5;39m\x1b[38;5;15m"); // Bright Cyan
        } else {
            printf("\x1b[48;5;27m\x1b[38;5;15m"); // Royal Blue
        }
    } else if (is_last_move) {
        printf("\x1b[48;5;125m\x1b[38;5;15m"); // Pastel Red/Orange
    } else if (is_light) {
        printf("\x1b[48;5;253m\x1b[30m"); // Cream/Off-White
    } else {
        printf("\x1b[48;5;239m\x1b[38;5;15m"); // Charcoal/Grey
    }
}

void draw_sidebar_line(int line_idx) {
    printf("   \x1b[K");
    switch (line_idx) {
        case 0:  printf("\x1b[1;36m====================================================\x1b[0m"); break;
        case 1:  printf("\x1b[1;32m                 TERMINAL CHESS GUI                 \x1b[0m"); break;
        case 2:  printf("\x1b[1;36m====================================================\x1b[0m"); break;
        case 3:  printf("Mode:          %s", engine_mode ? "\x1b[1;33mPlayer vs Engine\x1b[0m" : "\x1b[1;35mPass & Play (Local)\x1b[0m"); break;
        case 4: {
            int status = get_game_status();
            if (status == 1) {
                printf("Active Turn:   \x1b[1;5;31mCHECKMATE! (%s wins)\x1b[0m",
                       current_state.active_color == WHITE ? "BLACK" : "WHITE");
            } else if (status == 2) {
                printf("Active Turn:   \x1b[1;33mSTALEMATE! (Draw)\x1b[0m");
            } else {
                printf("Active Turn:   %s", current_state.active_color == WHITE ? "\x1b[1;37mWHITE\x1b[0m" : "\x1b[1;31mBLACK\x1b[0m");
            }
            break;
        }
        case 5:  printf("Engine Status: %s", !engine_available ? "\x1b[1;30mOffline (Run with 'stockfish' in path)\x1b[0m" :
                                            (thinking ? "\x1b[5;1;32mThinking...\x1b[0m" : "\x1b[1;34mReady\x1b[0m")); break;
        case 6: {
            char limit_str[100];
            if (tc_type == 0) sprintf(limit_str, "Time Limit: %d ms", tc_value[0]);
            else if (tc_type == 1) sprintf(limit_str, "Depth Limit: %d plies", tc_value[1]);
            else sprintf(limit_str, "Node Limit: %d", tc_value[2]);
            printf("Time Control:  \x1b[1;32m%s\x1b[0m", limit_str);
            break;
        }
        case 7:  printf("Castling:      W: %s%s  B: %s%s",
                        (current_state.castling_rights & 1) ? "K" : "-",
                        (current_state.castling_rights & 2) ? "Q" : "-",
                        (current_state.castling_rights & 4) ? "k" : "-",
                        (current_state.castling_rights & 8) ? "q" : "-"); break;
        case 8:  printf("Halfmove/Full: %d / %d", current_state.halfmove_clock, current_state.fullmove_number); break;
        case 9:  printf("----------------------------------------------------"); break;
        case 10: {
            if (promoting_state) {
                printf("\x1b[1;5;31m[PROMOTION] Press: Q (Queen), R (Rook), B (Bishop), N (Knight)\x1b[0m");
            } else {
                printf("\x1b[1mGAME HISTORY (PGN):\x1b[0m");
            }
            break;
        }
        case 11: case 12: case 13: case 14: case 15:
        case 16: case 17: case 18: case 19: case 20: {
            int start_move = (move_record_count > 20) ? (move_record_count - 20) : 0;
            int current_idx = start_move + (line_idx - 11) * 2;
            if (current_idx < move_record_count) {
                int move_num = (current_idx / 2) + 1;
                char white_move[20] = "";
                char black_move[20] = "";
                strcpy(white_move, move_records[current_idx].san);
                if (current_idx + 1 < move_record_count) {
                    strcpy(black_move, move_records[current_idx + 1].san);
                }
                printf("  %d. %-12s %-12s", move_num, white_move, black_move);
            }
            break;
        }
        case 21: printf("----------------------------------------------------"); break;
        case 22: printf("\x1b[1mCONTROLS:\x1b[0m"); break;
        case 23: printf("  \x1b[1;33mArrow Keys\x1b[0m : Move Cursor"); break;
        case 24: printf("  \x1b[1;33mSpace/Enter\x1b[0m: Select Piece / Confirm Destination"); break;
        case 25: printf("  \x1b[1;33mU\x1b[0m           : Undo Move"); break;
        case 26: printf("  \x1b[1;33mR\x1b[0m           : Reset/New Game"); break;
        case 27: printf("  \x1b[1;33mM\x1b[0m           : Toggle Mode (Engine vs Human)"); break;
        case 28: printf("  \x1b[1;33mT\x1b[0m           : Cycle TC (Time -> Depth -> Nodes)"); break;
        case 29: printf("  \x1b[1;33m+/-\x1b[0m         : Adjust TC limits"); break;
        case 30: printf("  \x1b[1;33mQ\x1b[0m           : Quit Game"); break;
        case 31: printf("\x1b[1;36m====================================================\x1b[0m"); break;
        default: break;
    }
}

void draw_screen() {
    printf("\x1b[H"); // Cursor to (1,1)

    printf("\n       A     B     C     D     E     F     G     H\n");
    printf("    +-----+-----+-----+-----+-----+-----+-----+-----+\n");

    for (int row = 0; row < 8; row++) {
        int rank = 8 - row;
        for (int line = 0; row < 8 && line < 3; line++) {
            if (line == 1) {
                printf("  %d |", rank);
            } else {
                printf("    |");
            }

            for (int col = 0; col < 8; col++) {
                int sq = row * 8 + col;
                set_square_color(sq);
                if (line == 0 || line == 2) {
                    printf("     ");
                } else {
                    int p_type = current_state.board_type[sq];
                    int p_col = current_state.board_color[sq];
                    if (p_col == WHITE) {
                        printf("\x1b[1;97m"); // Bright White Piece
                    } else if (p_col == BLACK) {
                        printf("\x1b[1;31m"); // Bright Red Piece
                    }
                    if (p_type == 0) {
                        printf("  .  ");
                    } else {
                        printf("  %s  ", get_piece_symbol(p_type));
                    }
                }
                printf("\x1b[0m|");
            }

            if (line == 1) {
                printf(" %d", rank);
            } else {
                printf("   ");
            }
            draw_sidebar_line(row * 3 + line);
            printf("\n");
        }
        printf("    +-----+-----+-----+-----+-----+-----+-----+-----+\n");
    }
    printf("       A     B     C     D     E     F     G     H\n\n");
}

void adjust_tc(int direction) {
    if (tc_type == 0) {
        tc_value[0] += direction * 1000;
        if (tc_value[0] < 1000) tc_value[0] = 1000;
    } else if (tc_type == 1) {
        tc_value[1] += direction * 1;
        if (tc_value[1] < 1) tc_value[1] = 1;
        if (tc_value[1] > 100) tc_value[1] = 100;
    } else if (tc_type == 2) {
        tc_value[2] += direction * 10000;
        if (tc_value[2] < 10000) tc_value[2] = 10000;
    }
}

void cleanup() {
    if (engine_pid > 0) {
        send_to_engine("quit\n");
        kill(engine_pid, SIGTERM);
    }
    disable_raw_mode();
}

int main() {
    printf("\x1b[2J"); // Clear Screen Once
    enable_raw_mode();
    printf("\x1b[?25l"); // Hide terminal blinking cursor

    init_game();
    verify_engine();

    while (1) {
        draw_screen();

        if (engine_mode && current_state.active_color == BLACK && !promoting_state) {
            trigger_engine_move();
            continue;
        }

        int k = read_key();
        if (k == 0) continue;

        if (promoting_state) {
            int choice = 0;
            if (k == 'q' || k == 'Q') choice = QUEEN;
            else if (k == 'r' || k == 'R') choice = ROOK;
            else if (k == 'b' || k == 'B') choice = BISHOP;
            else if (k == 'n' || k == 'N') choice = KNIGHT;

            if (choice != 0) {
                char san[16];
                generate_san(promo_from, promo_to, choice, san);

                history[history_count++] = current_state;
                move_records[move_record_count++] = (MoveRecord){
                    .from = promo_from,
                    .to = promo_to,
                    .promo = choice,
                    .piece = PAWN,
                    .san = ""
                };
                strcpy(move_records[move_record_count - 1].san, san);
                apply_move_to_state(&current_state, promo_from, promo_to, choice);

                promoting_state = false;
                promo_from = -1;
                promo_to = -1;
            }
            continue;
        }

        if (k == 'q' || k == 'Q') {
            break;
        } else if (k == KEY_UP) {
            if (cursor_square >= 8) cursor_square -= 8;
        } else if (k == KEY_DOWN) {
            if (cursor_square < 56) cursor_square += 8;
        } else if (k == KEY_LEFT) {
            if (cursor_square % 8 > 0) cursor_square -= 1;
        } else if (k == KEY_RIGHT) {
            if (cursor_square % 8 < 7) cursor_square += 1;
        } else if (k == 'u' || k == 'U') {
            undo_move();
        } else if (k == 'r' || k == 'R') {
            init_game();
        } else if (k == 'm' || k == 'M') {
            if (engine_available) {
                engine_mode = !engine_mode;
                selected_square = -1;
            }
        } else if (k == 't' || k == 'T') {
            tc_type = (tc_type + 1) % 3;
        } else if (k == '+' || k == '=') {
            adjust_tc(1);
        } else if (k == '-' || k == '_') {
            adjust_tc(-1);
        } else if (k == ' ' || k == '\n' || k == '\r') {
            if (selected_square == -1) {
                if (current_state.board_color[cursor_square] == current_state.active_color) {
                    selected_square = cursor_square;
                }
            } else {
                if (cursor_square == selected_square) {
                    selected_square = -1;
                } else if (current_state.board_color[cursor_square] == current_state.active_color) {
                    selected_square = cursor_square; // Retarget active selection
                } else {
                    if (is_legal_move(selected_square, cursor_square, QUEEN)) {
                        int piece = current_state.board_type[selected_square];
                        if (piece == PAWN && (cursor_square / 8 == 0 || cursor_square / 8 == 7)) {
                            promoting_state = true;
                            promo_from = selected_square;
                            promo_to = cursor_square;
                            selected_square = -1;
                        } else {
                            char san[16];
                            generate_san(selected_square, cursor_square, QUEEN, san);

                            history[history_count++] = current_state;
                            move_records[move_record_count++] = (MoveRecord){
                                .from = selected_square,
                                .to = cursor_square,
                                .promo = QUEEN,
                                .piece = piece,
                                .san = ""
                            };
                            strcpy(move_records[move_record_count - 1].san, san);
                            apply_move_to_state(&current_state, selected_square, cursor_square, QUEEN);
                            selected_square = -1;
                        }
                    }
                }
            }
        }
    }

    cleanup();
    return 0;
}
