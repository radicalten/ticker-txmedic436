#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>

#define MAX_MOVES 1024

// --- Core Chess Representation (0x88 Board) ---
typedef struct {
    int from;
    int to;
    char promo; // 'q', 'r', 'b', 'n' or 0
} Move;

typedef struct {
    char board[128];
    int turn;        // 0: White, 1: Black
    int castling[4]; // 0: WK, 1: WQ, 2: BK, 3: BQ
    int ep_square;   // 0x88 square index, -1 if none
} Position;

typedef struct {
    Position history[MAX_MOVES];
    Move moves[MAX_MOVES];
    char san_history[MAX_MOVES][16];
    int move_count;
    int current_ply;
} Game;

typedef struct {
    char engine_path[512];
    int limit_type;   // 0: movetime (ms), 1: depth, 2: nodes
    int limit_val;    // limit setting
    int player_color; // 0: White, 1: Black, 2: Human vs Human, 3: Engine vs Engine
} Config;

// --- Global Engine & Terminal State ---
int to_engine[2];
int from_engine[2];
pid_t engine_pid = -1;
int engine_is_thinking = 0;
char engine_buf[4096];
int engine_buf_len = 0;
struct termios orig_termios;
Config config;

// --- Helper Utilities ---
int to_0x88(int file, int rank) { return rank * 16 + file; }
int file_exists(const char *path) { return access(path, F_OK) == 0; }

// --- Chess Move Rules & Validation ---
int is_attacked(Position *pos, int sq, int attacker_side) {
    char p_pawn   = (attacker_side == 0) ? 'P' : 'p';
    char p_knight = (attacker_side == 0) ? 'N' : 'n';
    char p_bishop = (attacker_side == 0) ? 'B' : 'b';
    char p_rook   = (attacker_side == 0) ? 'R' : 'r';
    char p_queen  = (attacker_side == 0) ? 'Q' : 'q';
    char p_king   = (attacker_side == 0) ? 'K' : 'k';

    // Knight attacks
    int knight_offs[] = {33, 31, 18, 14, -33, -31, -18, -14};
    for (int i = 0; i < 8; i++) {
        int target = sq + knight_offs[i];
        if (!(target & 0x88) && pos->board[target] == p_knight) return 1;
    }

    // King attacks
    int king_offs[] = {17, 16, 15, 1, -17, -16, -15, -1};
    for (int i = 0; i < 8; i++) {
        int target = sq + king_offs[i];
        if (!(target & 0x88) && pos->board[target] == p_king) return 1;
    }

    // Pawn attacks
    if (attacker_side == 0) { // White attacking Black
        int targets[] = {sq - 17, sq - 15};
        for (int i = 0; i < 2; i++) {
            if (!(targets[i] & 0x88) && pos->board[targets[i]] == 'P') return 1;
        }
    } else { // Black attacking White
        int targets[] = {sq + 17, sq + 15};
        for (int i = 0; i < 2; i++) {
            if (!(targets[i] & 0x88) && pos->board[targets[i]] == 'p') return 1;
        }
    }

    // Rook & Queen attacks (Orthogonals)
    int rook_offs[] = {16, -16, 1, -1};
    for (int i = 0; i < 4; i++) {
        int target = sq + rook_offs[i];
        while (!(target & 0x88)) {
            char p = pos->board[target];
            if (p != '.') {
                if (p == p_rook || p == p_queen) return 1;
                break;
            }
            target += rook_offs[i];
        }
    }

    // Bishop & Queen attacks (Diagonals)
    int bishop_offs[] = {17, -17, 15, -15};
    for (int i = 0; i < 4; i++) {
        int target = sq + bishop_offs[i];
        while (!(target & 0x88)) {
            char p = pos->board[target];
            if (p != '.') {
                if (p == p_bishop || p == p_queen) return 1;
                break;
            }
            target += bishop_offs[i];
        }
    }
    return 0;
}

int find_king(Position *pos, int side) {
    char k = (side == 0) ? 'K' : 'k';
    for (int sq = 0; sq < 128; sq++) {
        if (!(sq & 0x88) && pos->board[sq] == k) return sq;
    }
    return -1;
}

int in_check(Position *pos, int side) {
    int king_sq = find_king(pos, side);
    if (king_sq == -1) return 0;
    return is_attacked(pos, king_sq, 1 - side);
}

void make_move(const Position *src, Position *dst, Move move) {
    *dst = *src;
    char p = dst->board[move.from];
    dst->board[move.from] = '.';

    if (move.promo) {
        dst->board[move.to] = (dst->turn == 0) ? toupper(move.promo) : tolower(move.promo);
    } else {
        dst->board[move.to] = p;
    }

    // En Passant capturing
    if ((p == 'P' || p == 'p') && move.to == dst->ep_square) {
        if (dst->turn == 0) dst->board[move.to - 16] = '.';
        else dst->board[move.to + 16] = '.';
    }

    // Ep Target Setting
    dst->ep_square = -1;
    if (p == 'P' && move.to - move.from == 32) dst->ep_square = move.from + 16;
    else if (p == 'p' && move.from - move.to == 32) dst->ep_square = move.from - 16;

    // Castling updates
    if (p == 'K') {
        dst->castling[0] = dst->castling[1] = 0;
        if (move.to - move.from == 2) {
            dst->board[to_0x88(7, 0)] = '.';
            dst->board[to_0x88(5, 0)] = 'R';
        } else if (move.from - move.to == 2) {
            dst->board[to_0x88(0, 0)] = '.';
            dst->board[to_0x88(3, 0)] = 'R';
        }
    } else if (p == 'k') {
        dst->castling[2] = dst->castling[3] = 0;
        if (move.to - move.from == 2) {
            dst->board[to_0x88(7, 7)] = '.';
            dst->board[to_0x88(5, 7)] = 'r';
        } else if (move.from - move.to == 2) {
            dst->board[to_0x88(0, 7)] = '.';
            dst->board[to_0x88(3, 7)] = 'r';
        }
    }

    // Lose castling rights if rook leaves or is taken
    if (move.from == to_0x88(7, 0) || move.to == to_0x88(7, 0)) dst->castling[0] = 0;
    if (move.from == to_0x88(0, 0) || move.to == to_0x88(0, 0)) dst->castling[1] = 0;
    if (move.from == to_0x88(7, 7) || move.to == to_0x88(7, 7)) dst->castling[2] = 0;
    if (move.from == to_0x88(0, 7) || move.to == to_0x88(0, 7)) dst->castling[3] = 0;

    dst->turn = 1 - dst->turn;
}

int generate_pseudo_moves(Position *pos, Move *moves) {
    int count = 0;
    int side = pos->turn;
    char p_pawn   = (side == 0) ? 'P' : 'p';
    char p_knight = (side == 0) ? 'N' : 'n';
    char p_bishop = (side == 0) ? 'B' : 'b';
    char p_rook   = (side == 0) ? 'R' : 'r';
    char p_queen  = (side == 0) ? 'Q' : 'q';
    char p_king   = (side == 0) ? 'K' : 'k';

    for (int sq = 0; sq < 128; sq++) {
        if (sq & 0x88) continue;
        char piece = pos->board[sq];
        if (piece == '.') continue;

        int is_white = (piece >= 'A' && piece <= 'Z');
        if ((side == 0 && !is_white) || (side == 1 && is_white)) continue;

        if (piece == p_pawn) {
            int rank = sq >> 4;
            if (side == 0) { // White pawn
                int to = sq + 16;
                if (!(to & 0x88) && pos->board[to] == '.') {
                    if (rank == 6) {
                        char promos[] = {'q', 'r', 'b', 'n'};
                        for (int p = 0; p < 4; p++) moves[count++] = (Move){sq, to, promos[p]};
                    } else {
                        moves[count++] = (Move){sq, to, 0};
                        if (rank == 1 && pos->board[sq + 32] == '.') moves[count++] = (Move){sq, sq + 32, 0};
                    }
                }
                int caps[] = {sq + 15, sq + 17};
                for (int c = 0; c < 2; c++) {
                    int to_c = caps[c];
                    if (!(to_c & 0x88)) {
                        char target_p = pos->board[to_c];
                        if ((target_p != '.' && (target_p >= 'a' && target_p <= 'z')) || to_c == pos->ep_square) {
                            if (rank == 6) {
                                char promos[] = {'q', 'r', 'b', 'n'};
                                for (int p = 0; p < 4; p++) moves[count++] = (Move){sq, to_c, promos[p]};
                            } else {
                                moves[count++] = (Move){sq, to_c, 0};
                            }
                        }
                    }
                }
            } else { // Black pawn
                int to = sq - 16;
                if (!(to & 0x88) && pos->board[to] == '.') {
                    if (rank == 1) {
                        char promos[] = {'q', 'r', 'b', 'n'};
                        for (int p = 0; p < 4; p++) moves[count++] = (Move){sq, to, promos[p]};
                    } else {
                        moves[count++] = (Move){sq, to, 0};
                        if (rank == 6 && pos->board[sq - 32] == '.') moves[count++] = (Move){sq, sq - 32, 0};
                    }
                }
                int caps[] = {sq - 15, sq - 17};
                for (int c = 0; c < 2; c++) {
                    int to_c = caps[c];
                    if (!(to_c & 0x88)) {
                        char target_p = pos->board[to_c];
                        if ((target_p != '.' && (target_p >= 'A' && target_p <= 'Z')) || to_c == pos->ep_square) {
                            if (rank == 1) {
                                char promos[] = {'q', 'r', 'b', 'n'};
                                for (int p = 0; p < 4; p++) moves[count++] = (Move){sq, to_c, promos[p]};
                            } else {
                                moves[count++] = (Move){sq, to_c, 0};
                            }
                        }
                    }
                }
            }
        } else if (piece == p_knight) {
            int knight_offs[] = {33, 31, 18, 14, -33, -31, -18, -14};
            for (int i = 0; i < 8; i++) {
                int to = sq + knight_offs[i];
                if (!(to & 0x88)) {
                    char target_p = pos->board[to];
                    if (target_p == '.' || ((side == 0) ? (target_p >= 'a' && target_p <= 'z') : (target_p >= 'A' && target_p <= 'Z'))) {
                        moves[count++] = (Move){sq, to, 0};
                    }
                }
            }
        } else if (piece == p_king) {
            int king_offs[] = {17, 16, 15, 1, -17, -16, -15, -1};
            for (int i = 0; i < 8; i++) {
                int to = sq + king_offs[i];
                if (!(to & 0x88)) {
                    char target_p = pos->board[to];
                    if (target_p == '.' || ((side == 0) ? (target_p >= 'a' && target_p <= 'z') : (target_p >= 'A' && target_p <= 'Z'))) {
                        moves[count++] = (Move){sq, to, 0};
                    }
                }
            }
            // Castling
            if (side == 0) {
                if (pos->castling[0] && pos->board[to_0x88(5, 0)] == '.' && pos->board[to_0x88(6, 0)] == '.' &&
                    !is_attacked(pos, to_0x88(4, 0), 1) && !is_attacked(pos, to_0x88(5, 0), 1) && !is_attacked(pos, to_0x88(6, 0), 1)) {
                    moves[count++] = (Move){to_0x88(4, 0), to_0x88(6, 0), 0};
                }
                if (pos->castling[1] && pos->board[to_0x88(3, 0)] == '.' && pos->board[to_0x88(2, 0)] == '.' && pos->board[to_0x88(1, 0)] == '.' &&
                    !is_attacked(pos, to_0x88(4, 0), 1) && !is_attacked(pos, to_0x88(3, 0), 1) && !is_attacked(pos, to_0x88(2, 0), 1)) {
                    moves[count++] = (Move){to_0x88(4, 0), to_0x88(2, 0), 0};
                }
            } else {
                if (pos->castling[2] && pos->board[to_0x88(5, 7)] == '.' && pos->board[to_0x88(6, 7)] == '.' &&
                    !is_attacked(pos, to_0x88(4, 7), 0) && !is_attacked(pos, to_0x88(5, 7), 0) && !is_attacked(pos, to_0x88(6, 7), 0)) {
                    moves[count++] = (Move){to_0x88(4, 7), to_0x88(6, 7), 0};
                }
                if (pos->castling[3] && pos->board[to_0x88(3, 7)] == '.' && pos->board[to_0x88(2, 7)] == '.' && pos->board[to_0x88(1, 7)] == '.' &&
                    !is_attacked(pos, to_0x88(4, 7), 0) && !is_attacked(pos, to_0x88(3, 7), 0) && !is_attacked(pos, to_0x88(2, 7), 0)) {
                    moves[count++] = (Move){to_0x88(4, 7), to_0x88(2, 7), 0};
                }
            }
        } else { // Sliders (Rook, Bishop, Queen)
            int slide_offs[8];
            int off_count = 0;
            if (piece == p_bishop || piece == p_queen) {
                slide_offs[off_count++] = 17; slide_offs[off_count++] = -17;
                slide_offs[off_count++] = 15; slide_offs[off_count++] = -15;
            }
            if (piece == p_rook || piece == p_queen) {
                slide_offs[off_count++] = 16; slide_offs[off_count++] = -16;
                slide_offs[off_count++] = 1;  slide_offs[off_count++] = -1;
            }
            for (int i = 0; i < off_count; i++) {
                int to = sq + slide_offs[i];
                while (!(to & 0x88)) {
                    char target_p = pos->board[to];
                    if (target_p == '.') {
                        moves[count++] = (Move){sq, to, 0};
                    } else {
                        int is_enemy = (side == 0) ? (target_p >= 'a' && target_p <= 'z') : (target_p >= 'A' && target_p <= 'Z');
                        if (is_enemy) moves[count++] = (Move){sq, to, 0};
                        break;
                    }
                    to += slide_offs[i];
                }
            }
        }
    }
    return count;
}

int generate_legal_moves(Position *pos, Move *legal_moves) {
    Move pseudo[256];
    int pseudo_count = generate_pseudo_moves(pos, pseudo);
    int count = 0;
    for (int i = 0; i < pseudo_count; i++) {
        Position temp;
        make_move(pos, &temp, pseudo[i]);
        if (!in_check(&temp, pos->turn)) {
            legal_moves[count++] = pseudo[i];
        }
    }
    return count;
}

// --- SAN Move Formatting (PGN compliant) ---
void get_move_san(Position *pos, Move move, char *san) {
    Move legal[256];
    int legal_count = generate_legal_moves(pos, legal);
    char p = pos->board[move.from];
    char p_upper = toupper(p);

    // Castling detection
    if (p_upper == 'K' && abs((move.to & 7) - (move.from & 7)) == 2) {
        if ((move.to & 7) == 6) strcpy(san, "O-O");
        else strcpy(san, "O-O-O");
        Position next;
        make_move(pos, &next, move);
        if (in_check(&next, next.turn)) {
            Move next_legal[256];
            if (generate_legal_moves(&next, next_legal) == 0) strcat(san, "#");
            else strcat(san, "+");
        }
        return;
    }

    int idx = 0;
    if (p_upper != 'P') {
        san[idx++] = p_upper;
        int file_conflict = 0, rank_conflict = 0, conflict_count = 0;
        for (int i = 0; i < legal_count; i++) {
            if (legal[i].from != move.from && legal[i].to == move.to && toupper(pos->board[legal[i].from]) == p_upper) {
                conflict_count++;
                if ((legal[i].from & 7) == (move.from & 7)) file_conflict = 1;
                if ((legal[i].from >> 4) == (move.from >> 4)) rank_conflict = 1;
            }
        }
        if (conflict_count > 0) {
            if (!file_conflict) san[idx++] = 'a' + (move.from & 7);
            else if (!rank_conflict) san[idx++] = '1' + (move.from >> 4);
            else {
                san[idx++] = 'a' + (move.from & 7);
                san[idx++] = '1' + (move.from >> 4);
            }
        }
    } else {
        if ((move.to & 7) != (move.from & 7)) {
            san[idx++] = 'a' + (move.from & 7);
        }
    }

    int is_cap = (pos->board[move.to] != '.' || (p_upper == 'P' && move.to == pos->ep_square));
    if (is_cap) san[idx++] = 'x';

    san[idx++] = 'a' + (move.to & 7);
    san[idx++] = '1' + (move.to >> 4);

    if (move.promo) {
        san[idx++] = '=';
        san[idx++] = toupper(move.promo);
    }
    san[idx] = '\0';

    Position next;
    make_move(pos, &next, move);
    if (in_check(&next, next.turn)) {
        Move next_legal[256];
        if (generate_legal_moves(&next, next_legal) == 0) strcat(san, "#");
        else strcat(san, "+");
    }
}

// --- UCI Engine Process Communication ---
void send_to_engine(const char *cmd) {
    if (engine_pid != -1) {
        write(to_engine[1], cmd, strlen(cmd));
    }
}

void start_engine(const char *path) {
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) return;
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[1]);
        close(from_engine[0]);
        execl(path, path, (char *)NULL);
        exit(1);
    }
    close(to_engine[0]);
    close(from_engine[1]);
    fcntl(from_engine[0], F_SETFL, O_NONBLOCK);

    send_to_engine("uci\n");
    send_to_engine("isready\n");
    send_to_engine("ucinewgame\n");
}

void move_to_uci(Move m, char *buf) {
    if (m.promo) {
        sprintf(buf, "%c%c%c%c%c", 'a' + (m.from & 7), '1' + (m.from >> 4), 'a' + (m.to & 7), '1' + (m.to >> 4), m.promo);
    } else {
        sprintf(buf, "%c%c%c%c", 'a' + (m.from & 7), '1' + (m.from >> 4), 'a' + (m.to & 7), '1' + (m.to >> 4));
    }
}

Move uci_to_move(const char *uci) {
    Move m = {0};
    m.from = to_0x88(uci[0] - 'a', uci[1] - '1');
    m.to = to_0x88(uci[2] - 'a', uci[3] - '1');
    if (strlen(uci) >= 5) m.promo = uci[4];
    return m;
}

void trigger_engine_move(Game *game) {
    if (engine_pid == -1) return;
    char cmd[8192];
    strcpy(cmd, "position startpos");
    if (game->current_ply > 0) {
        strcat(cmd, " moves");
        for (int i = 0; i < game->current_ply; i++) {
            char uci[8];
            move_to_uci(game->moves[i], uci);
            strcat(cmd, " ");
            strcat(cmd, uci);
        }
    }
    strcat(cmd, "\n");
    send_to_engine(cmd);

    if (config.limit_type == 0) sprintf(cmd, "go movetime %d\n", config.limit_val);
    else if (config.limit_type == 1) sprintf(cmd, "go depth %d\n", config.limit_val);
    else sprintf(cmd, "go nodes %d\n", config.limit_val);
    send_to_engine(cmd);
}

int parse_engine_output(Game *game, Move *out_move) {
    char chunk[1024];
    ssize_t n = read(from_engine[0], chunk, sizeof(chunk) - 1);
    if (n <= 0) return 0;
    chunk[n] = '\0';

    if (engine_buf_len + n < (int)sizeof(engine_buf)) {
        memcpy(engine_buf + engine_buf_len, chunk, n);
        engine_buf_len += n;
        engine_buf[engine_buf_len] = '\0';
    } else {
        engine_buf_len = 0;
    }

    char *start = engine_buf;
    char *newline;
    int found = 0;
    while ((newline = strchr(start, '\n')) != NULL) {
        *newline = '\0';
        if (strncmp(start, "bestmove ", 9) == 0) {
            char best[32];
            sscanf(start, "bestmove %s", best);
            if (strcmp(best, "(none)") != 0 && strcmp(best, "NULL") != 0) {
                *out_move = uci_to_move(best);
                found = 1;
            }
        }
        start = newline + 1;
    }
    int consumed = start - engine_buf;
    if (consumed > 0) {
        memmove(engine_buf, start, engine_buf_len - consumed);
        engine_buf_len -= consumed;
        engine_buf[engine_buf_len] = '\0';
    }
    return found;
}

// --- Terminal Control (Raw Mode) ---
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\x1b[?25h\x1b[2J\x1b[H"); // Show cursor, clear screen
    fflush(stdout);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\x1b[?25l"); // Hide cursor
    fflush(stdout);
}

int get_key() {
    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;
    if (c == 27) {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return 27;
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) return 27;
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 1000; // UP
                case 'B': return 1001; // DOWN
                case 'C': return 1002; // RIGHT
                case 'D': return 1003; // LEFT
            }
        }
        return 27;
    }
    return c;
}

// --- Render Layout & Wrap PGN ---
void get_pgn_lines(Game *game, char lines[10][40], int max_lines) {
    char pgn[8192] = "";
    for (int i = 0; i < game->move_count; i++) {
        if (i % 2 == 0) {
            char turn_num[16];
            sprintf(turn_num, "%d. ", (i / 2) + 1);
            strcat(pgn, turn_num);
        }
        strcat(pgn, game->san_history[i]);
        strcat(pgn, " ");
    }
    for (int l = 0; l < max_lines; l++) lines[l][0] = '\0';

    int len = strlen(pgn);
    int current_line = 0, line_pos = 0, word_start = 0;
    for (int i = 0; i <= len; i++) {
        if (pgn[i] == ' ' || pgn[i] == '\0') {
            int word_len = i - word_start;
            if (line_pos + word_len + 1 < 36) {
                if (line_pos > 0) lines[current_line][line_pos++] = ' ';
                memcpy(&lines[current_line][line_pos], &pgn[word_start], word_len);
                line_pos += word_len;
                lines[current_line][line_pos] = '\0';
            } else {
                current_line++;
                if (current_line >= max_lines) break;
                line_pos = 0;
                memcpy(&lines[current_line][line_pos], &pgn[word_start], word_len);
                line_pos += word_len;
                lines[current_line][line_pos] = '\0';
            }
            word_start = i + 1;
        }
    }
}

void draw_screen(Game *game, int cursor_f, int cursor_r, int selected_sq) {
    char side[16][80];
    for (int i = 0; i < 16; i++) side[i][0] = '\0';

    // Status / Metadata Formatting
    if (engine_pid == -1) sprintf(side[0], "  \x1b[1;31mENGINE DISCONNECTED\x1b[0m");
    else if (engine_is_thinking) sprintf(side[0], "  ENGINE: \x1b[1;32mTHINKING...\x1b[0m");
    else sprintf(side[0], "  ENGINE: \x1b[1;34mIDLE\x1b[0m");

    char short_path[25];
    if (strlen(config.engine_path) > 22) {
        sprintf(short_path, "...%s", config.engine_path + strlen(config.engine_path) - 19);
    } else strcpy(short_path, config.engine_path);
    sprintf(side[1], "  PATH:   %s", short_path);

    if (config.limit_type == 0) sprintf(side[2], "  LIMIT:  MoveTime %dms", config.limit_val);
    else if (config.limit_type == 1) sprintf(side[2], "  LIMIT:  Depth %d", config.limit_val);
    else sprintf(side[2], "  LIMIT:  Nodes %d", config.limit_val);

    sprintf(side[3], "  SIDE:   %s", (config.player_color == 0) ? "White" : (config.player_color == 1) ? "Black" :
            (config.player_color == 2) ? "Human vs Human" : "Engine vs Engine");

    // Game-Over Check State
    Position *curr = &game->history[game->current_ply];
    Move legal[256];
    int legal_count = generate_legal_moves(curr, legal);
    if (legal_count == 0) {
        if (in_check(curr, curr->turn)) {
            sprintf(side[4], "  \x1b[1;31m%s\x1b[0m", (curr->turn == 0) ? "MATE - BLACK WINS!" : "MATE - WHITE WINS!");
        } else {
            sprintf(side[4], "  \x1b[1;33mSTALEMATE!\x1b[0m");
        }
    } else if (in_check(curr, curr->turn)) {
        sprintf(side[4], "  \x1b[1;31mCHECK!\x1b[0m");
    }

    sprintf(side[5], "  PGN HISTORY:");
    char pgn_lines[10][40];
    get_pgn_lines(game, pgn_lines, 10);
    for (int i = 0; i < 10; i++) {
        if (pgn_lines[i][0] != '\0') sprintf(side[6 + i], "  %s", pgn_lines[i]);
    }

    // ANSI Frame Painting
    printf("\x1b[H"); // Refresh starting from console Home
    printf("\r\n   ========================================================================\r\n");
    printf("                       CHESS TERMINAL GUI (UCI Engine)\r\n");
    printf("   ========================================================================\r\n\r\n");

    for (int r = 7; r >= 0; r--) {
        for (int line = 0; line < 2; line++) {
            if (line == 0) printf(" %d  ", r + 1);
            else printf("    ");

            for (int f = 0; f < 8; f++) {
                int sq = to_0x88(f, r);
                int is_dark = (r + f) % 2 == 0;
                int is_cursor = (f == cursor_f && r == cursor_r);
                int is_selected = (sq == selected_sq);

                int is_legal_dest = 0;
                if (selected_sq != -1) {
                    for (int i = 0; i < legal_count; i++) {
                        if (legal[i].from == selected_sq && legal[i].to == sq) {
                            is_legal_dest = 1;
                            break;
                        }
                    }
                }

                // Square Coloring
                if (is_cursor) printf("\x1b[48;5;120m");       // Soft Green
                else if (is_selected) printf("\x1b[48;5;221m"); // Soft Yellow
                else if (is_legal_dest) printf("\x1b[48;5;75m"); // Soft Blue
                else if (is_dark) printf("\x1b[48;5;238m");     // Charcoal Dark Square
                else printf("\x1b[48;5;250m");                  // Muted Light Square

                char p = curr->board[sq];
                int is_white_piece = (p >= 'A' && p <= 'Z');
                if (p != '.') {
                    if (is_white_piece) printf("\x1b[38;5;231m\x1b[1m"); // Bright White Piece
                    else printf("\x1b[38;5;234m\x1b[1m");                 // Bold Charcoal Piece
                }

                if (line == 0) {
                    if (p != '.') printf("  %c ", p);
                    else printf("    ");
                } else printf("    ");
                printf("\x1b[0m");
            }
            int board_line_idx = (7 - r) * 2 + line;
            printf("%s\r\n", side[board_line_idx]);
        }
    }
    printf("       a   b   c   d   e   f   g   h\r\n\r\n");
    printf("   ------------------------------------------------------------------------\r\n");
    printf("   [ARROWS] Cursor  [SPACE/ENTER] Select/Move  [U] Undo  [C] Config  [Q] Quit\r\n");
    printf("   ------------------------------------------------------------------------\r\n");
    fflush(stdout);
}

char prompt_promotion() {
    printf("\x1b[19;1H\x1b[K  Promote to (q/r/b/n)? ");
    fflush(stdout);
    while (1) {
        int c = get_key();
        if (c == 'q' || c == 'r' || c == 'b' || c == 'n') return c;
        usleep(10000);
    }
}

// --- Interactive Config Submenu ---
void run_config_menu() {
    int active = 1, selection = 0;
    char path_buf[512];
    strcpy(path_buf, config.engine_path);

    while (active) {
        printf("\x1b[H\x1b[2J");
        printf("\r\n    CHESS ENGINE CONFIGURATION MENU\r\n");
        printf("    ===============================\r\n\r\n");
        printf(" %s 1. Engine Path:  %s\r\n", (selection == 0) ? "->" : "  ", path_buf);
        printf(" %s 2. Limit Type:   %s\r\n", (selection == 1) ? "->" : "  ",
               (config.limit_type == 0) ? "Move Time" : (config.limit_type == 1) ? "Depth" : "Nodes");
        printf(" %s 3. Limit Value:  %d\r\n", (selection == 2) ? "->" : "  ", config.limit_val);
        printf(" %s 4. Player Color: %s\r\n", (selection == 3) ? "->" : "  ",
               (config.player_color == 0) ? "White" : (config.player_color == 1) ? "Black" :
               (config.player_color == 2) ? "Human vs Human" : "Engine vs Engine");
        printf(" %s 5. Save and Return\r\n", (selection == 4) ? "->" : "  ");
        printf("\r\n\r\n [Arrows: Navigate, Enter: Select/Edit, Esc: Back]\r\n");
        fflush(stdout);

        int key = -1;
        while (key == -1) {
            key = get_key();
            usleep(10000);
        }

        if (key == 1000) selection = (selection - 1 + 5) % 5;
        else if (key == 1001) selection = (selection + 1) % 5;
        else if (key == 27) active = 0;
        else if (key == '\r' || key == '\n') {
            if (selection == 0) {
                printf("\r\n Enter Absolute Path: ");
                fflush(stdout);
                int pos = 0;
                path_buf[0] = '\0';
                while (1) {
                    int c = get_key();
                    if (c == '\r' || c == '\n') break;
                    else if ((c == 127 || c == 8) && pos > 0) {
                        pos--;
                        path_buf[pos] = '\0';
                        printf("\b \b");
                        fflush(stdout);
                    } else if (c >= 32 && c < 127 && pos < 511) {
                        path_buf[pos++] = c;
                        path_buf[pos] = '\0';
                        putchar(c);
                        fflush(stdout);
                    }
                    usleep(10000);
                }
            } else if (selection == 1) {
                config.limit_type = (config.limit_type + 1) % 3;
                if (config.limit_type == 0 && config.limit_val > 10000) config.limit_val = 1000;
                if (config.limit_type == 1 && config.limit_val > 50) config.limit_val = 10;
                if (config.limit_type == 2 && config.limit_val < 1000) config.limit_val = 10000;
            } else if (selection == 2) {
                printf("\r\n Enter Value: ");
                fflush(stdout);
                char val_buf[32] = "";
                int pos = 0;
                while (1) {
                    int c = get_key();
                    if (c == '\r' || c == '\n') break;
                    else if ((c == 127 || c == 8) && pos > 0) {
                        pos--;
                        val_buf[pos] = '\0';
                        printf("\b \b");
                        fflush(stdout);
                    } else if (c >= '0' && c <= '9' && pos < 31) {
                        val_buf[pos++] = c;
                        val_buf[pos] = '\0';
                        putchar(c);
                        fflush(stdout);
                    }
                    usleep(10000);
                }
                if (pos > 0) config.limit_val = atoi(val_buf);
            } else if (selection == 3) {
                config.player_color = (config.player_color + 1) % 4;
            } else if (selection == 4) {
                if (strcmp(config.engine_path, path_buf) != 0) {
                    strcpy(config.engine_path, path_buf);
                    if (engine_pid != -1) {
                        kill(engine_pid, SIGKILL);
                        close(to_engine[1]);
                        close(from_engine[0]);
                        engine_pid = -1;
                    }
                    if (file_exists(config.engine_path)) {
                        start_engine(config.engine_path);
                    }
                }
                active = 0;
            }
        }
    }
}

// --- Undo Framework ---
void undo_move(Game *game) {
    if (game->current_ply > 0) {
        int plies = 1;
        if (config.player_color == 0 || config.player_color == 1) {
            if (game->current_ply >= 2) plies = 2;
        }
        if (engine_is_thinking) {
            send_to_engine("stop\n");
            engine_is_thinking = 0;
        }
        game->current_ply -= plies;
        game->move_count = game->current_ply;
    }
}

// --- Main Engine Game Setup & Execution ---
int main() {
    signal(SIGPIPE, SIG_IGN);

    // Initial Config Setup
    config.limit_type = 0;   // movetime
    config.limit_val = 1000; // 1 second
    config.player_color = 0; // Human is White

    // Detect Stockfish via macOS brew pathways
    if (file_exists("/opt/homebrew/bin/stockfish")) {
        strcpy(config.engine_path, "/opt/homebrew/bin/stockfish");
    } else if (file_exists("/usr/local/bin/stockfish")) {
        strcpy(config.engine_path, "/usr/local/bin/stockfish");
    } else {
        strcpy(config.engine_path, "stockfish"); // fallback path search
    }

    if (file_exists(config.engine_path)) {
        start_engine(config.engine_path);
    }

    Game game;
    memset(&game, 0, sizeof(Game));

    // Fill Initial Chess Position Array
    Position *p = &game.history[0];
    memset(p->board, '.', 128);
    char back_row[] = "rnbqkbnr";
    char back_row_upper[] = "RNBQKBNR";
    for (int i = 0; i < 8; i++) {
        p->board[to_0x88(i, 0)] = back_row_upper[i];
        p->board[to_0x88(i, 1)] = 'P';
        p->board[to_0x88(i, 6)] = 'p';
        p->board[to_0x88(i, 7)] = back_row[i];
    }
    p->turn = 0;
    p->castling[0] = p->castling[1] = p->castling[2] = p->castling[3] = 1;
    p->ep_square = -1;

    enable_raw_mode();
    printf("\x1b[2J"); // Initial canvas clear

    int cursor_f = 4, cursor_r = 1;
    int selected_sq = -1;
    int force_redraw = 1;
    int running = 1;

    while (running) {
        int key = get_key();
        if (key != -1) {
            if (key == 'q' || key == 'Q') {
                running = 0;
            } else if (key == 'u' || key == 'U') {
                undo_move(&game);
                selected_sq = -1;
                force_redraw = 1;
            } else if (key == 'c' || key == 'C') {
                run_config_menu();
                force_redraw = 1;
            } else if (key == 1000) { // UP
                if (cursor_r < 7) { cursor_r++; force_redraw = 1; }
            } else if (key == 1001) { // DOWN
                if (cursor_r > 0) { cursor_r--; force_redraw = 1; }
            } else if (key == 1002) { // RIGHT
                if (cursor_f < 7) { cursor_f++; force_redraw = 1; }
            } else if (key == 1003) { // LEFT
                if (cursor_f > 0) { cursor_f--; force_redraw = 1; }
            } else if (key == ' ' || key == '\r' || key == '\n') {
                int clicked_sq = to_0x88(cursor_f, cursor_r);
                Position *curr_pos = &game.history[game.current_ply];
                char piece = curr_pos->board[clicked_sq];

                int is_engine_turn = (config.player_color == 3) ||
                                     (config.player_color == 0 && curr_pos->turn == 1) ||
                                     (config.player_color == 1 && curr_pos->turn == 0);

                if (!is_engine_turn && !engine_is_thinking) {
                    if (selected_sq == -1) {
                        if (piece != '.') {
                            int is_white = (piece >= 'A' && piece <= 'Z');
                            if ((curr_pos->turn == 0 && is_white) || (curr_pos->turn == 1 && !is_white)) {
                                selected_sq = clicked_sq;
                                force_redraw = 1;
                            }
                        }
                    } else {
                        Move legal[256];
                        int legal_count = generate_legal_moves(curr_pos, legal);
                        int matched = -1;
                        for (int i = 0; i < legal_count; i++) {
                            if (legal[i].from == selected_sq && legal[i].to == clicked_sq) {
                                matched = i;
                                break;
                            }
                        }
                        if (matched != -1) {
                            Move m = legal[matched];
                            int req_promo = 0;
                            for (int i = 0; i < legal_count; i++) {
                                if (legal[i].from == selected_sq && legal[i].to == clicked_sq && legal[i].promo != 0) {
                                    req_promo = 1;
                                    break;
                                }
                            }
                            if (req_promo) m.promo = prompt_promotion();

                            get_move_san(curr_pos, m, game.san_history[game.current_ply]);
                            Position next;
                            make_move(curr_pos, &next, m);

                            game.moves[game.current_ply] = m;
                            game.current_ply++;
                            game.history[game.current_ply] = next;
                            game.move_count = game.current_ply;

                            selected_sq = -1;
                            force_redraw = 1;
                        } else {
                            if (piece != '.') {
                                int is_white = (piece >= 'A' && piece <= 'Z');
                                if ((curr_pos->turn == 0 && is_white) || (curr_pos->turn == 1 && !is_white)) {
                                    selected_sq = clicked_sq;
                                } else selected_sq = -1;
                            } else selected_sq = -1;
                            force_redraw = 1;
                        }
                    }
                }
            }
        }

        // Logic check: Is it currently the Engine's Turn?
        Position *curr_pos = &game.history[game.current_ply];
        int is_engine_turn = (config.player_color == 3) ||
                             (config.player_color == 0 && curr_pos->turn == 1) ||
                             (config.player_color == 1 && curr_pos->turn == 0);

        Move legal[256];
        int legal_count = generate_legal_moves(curr_pos, legal);

        if (is_engine_turn && !engine_is_thinking && legal_count > 0 && engine_pid != -1) {
            engine_is_thinking = 1;
            force_redraw = 1;
            trigger_engine_move(&game);
        }

        // Read and parse non-blocking output from engine
        if (engine_is_thinking) {
            Move engine_m;
            if (parse_engine_output(&game, &engine_m)) {
                // Keep promotion selection consistent
                int match_found = 0;
                for (int i = 0; i < legal_count; i++) {
                    if (legal[i].from == engine_m.from && legal[i].to == engine_m.to) {
                        engine_m.promo = legal[i].promo;
                        match_found = 1;
                        break;
                    }
                }
                if (match_found) {
                    get_move_san(curr_pos, engine_m, game.san_history[game.current_ply]);
                    Position next;
                    make_move(curr_pos, &next, engine_m);

                    game.moves[game.current_ply] = engine_m;
                    game.current_ply++;
                    game.history[game.current_ply] = next;
                    game.move_count = game.current_ply;
                }
                engine_is_thinking = 0;
                force_redraw = 1;
            }
        }

        if (force_redraw) {
            draw_screen(&game, cursor_f, cursor_r, selected_sq);
            force_redraw = 0;
        }

        usleep(10000); // Muffle execution cycles (~100 FPS loop threshold)
    }

    if (engine_pid != -1) {
        kill(engine_pid, SIGKILL);
        waitpid(engine_pid, NULL, 0);
    }
    return 0;
}
