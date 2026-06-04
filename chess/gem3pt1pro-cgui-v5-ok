#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <fcntl.h>
#include <ctype.h>

#define MAX_MOVES 256
#define HIST_SIZE 512

// --- Data Structures ---
typedef struct {
    int from;
    int to;
    char promo;
    int is_capture;
    int is_ep;
    int is_castle;
} Move;

typedef struct {
    char board[64];
    int turn; // 0 = White, 1 = Black
    int ep; // En passant square index, -1 if none
    int cwk, cwq, cbk, cbq; // Castling rights
    int halfmove, fullmove;
} State;

// --- Global Variables ---
State state;
State history[HIST_SIZE];
char pgn_history[HIST_SIZE][16];
char uci_history[HIST_SIZE][8];
int hist_count = 0;

int cx = 4, cy = 6; // Cursor position
int selected_sq = -1;

int eng_in[2], eng_out[2];
pid_t eng_pid = 0;
char eng_buf[4096];
int eng_buf_len = 0;
int engine_active = 0;
char status_msg[256] = "Welcome to Mac Terminal Chess!";

struct termios orig_termios;

// --- Chess Logic Definitions ---
int dx[] = {-1, 1, 0, 0, -1, -1, 1, 1};
int dy[] = {0, 0, -1, 1, -1, 1, -1, 1};
int kx[] = {-2, -1, 1, 2, -2, -1, 1, 2};
int ky[] = {-1, -2, -2, -1, 1, 2, 2, 1};

// --- Terminal & UI Functions ---
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    printf("\033[?25h\033[0m\n"); // Show cursor, reset color
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    printf("\033[?25l"); // Hide cursor
}

int get_key() {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return 0;
    if (c == '\033') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return c;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return c;
        if (seq[0] == '[') {
            if (seq[1] == 'A') return 1001; // Up
            if (seq[1] == 'B') return 1002; // Down
            if (seq[1] == 'C') return 1003; // Right
            if (seq[1] == 'D') return 1004; // Left
        }
        return c;
    }
    return c;
}

const char* get_piece_unicode(char p) {
    switch (p) {
        case 'P': return "♙"; case 'N': return "♘"; case 'B': return "♗";
        case 'R': return "♖"; case 'Q': return "♕"; case 'K': return "♔";
        case 'p': return "♟"; case 'n': return "♞"; case 'b': return "♝";
        case 'r': return "♜"; case 'q': return "♛"; case 'k': return "♚";
        default: return " ";
    }
}

void draw_board() {
    printf("\033[H\033[J\r\n");
    printf("  \033[1mTerminal Chess GUI\033[0m - Status: %s\r\n\r\n", status_msg);
    for (int y = 0; y < 8; y++) {
        printf(" %d ", 8 - y);
        for (int x = 0; x < 8; x++) {
            int sq = y * 8 + x;
            int bg = ((x + y) % 2 == 0) ? 222 : 94; // Light/Dark squares
            if (sq == selected_sq) bg = 226; // Yellow (Selected)
            else if (x == cx && y == cy) bg = 46; // Green (Cursor)

            printf("\033[48;5;%dm\033[30m %s \033[0m", bg, get_piece_unicode(state.board[sq]));
        }
        printf("\r\n");
    }
    printf("    a  b  c  d  e  f  g  h\r\n\r\n");
    printf(" Move History (PGN):\r\n ");
    for (int i = 0; i < hist_count; i++) {
        if (i % 2 == 0) printf("%d. ", (i / 2) + 1);
        printf("%s ", pgn_history[i]);
        if (i % 2 == 1 && i != hist_count - 1) printf("\r\n ");
    }
    printf("\r\n\r\n \033[90m[Arrows]: Move Cursor | [Space/Enter]: Select/Move | [u]: Undo | [q]: Quit\033[0m\r\n");
    fflush(stdout);
}

// --- Chess Engine/Logic Functions ---
void init_state() {
    const char* start = "rnbqkbnrpppppppp................................PPPPPPPPRNBQKBNR";
    memcpy(state.board, start, 64);
    state.turn = 0; state.ep = -1;
    state.cwk = 1; state.cwq = 1; state.cbk = 1; state.cbq = 1;
    state.halfmove = 0; state.fullmove = 1;
}

int is_attacked(State* s, int sq, int by_black) {
    int sx = sq % 8, sy = sq / 8;
    // Pawns
    int pd = by_black ? -1 : 1;
    char pwn = by_black ? 'p' : 'P';
    if (sy - pd >= 0 && sy - pd < 8) {
        if (sx > 0 && s->board[(sy - pd) * 8 + sx - 1] == pwn) return 1;
        if (sx < 7 && s->board[(sy - pd) * 8 + sx + 1] == pwn) return 1;
    }
    // Knights
    char kn = by_black ? 'n' : 'N';
    for (int i = 0; i < 8; i++) {
        int nx = sx + kx[i], ny = sy + ky[i];
        if (nx >= 0 && nx < 8 && ny >= 0 && ny < 8 && s->board[ny * 8 + nx] == kn) return 1;
    }
    // Kings
    char kg = by_black ? 'k' : 'K';
    for (int i = 0; i < 8; i++) {
        int nx = sx + dx[i], ny = sy + dy[i];
        if (nx >= 0 && nx < 8 && ny >= 0 && ny < 8 && s->board[ny * 8 + nx] == kg) return 1;
    }
    // Sliders
    char rq = by_black ? 'q' : 'Q', rr = by_black ? 'r' : 'R', rb = by_black ? 'b' : 'B';
    for (int i = 0; i < 8; i++) {
        for (int d = 1; d < 8; d++) {
            int nx = sx + dx[i] * d, ny = sy + dy[i] * d;
            if (nx < 0 || nx > 7 || ny < 0 || ny > 7) break;
            char p = s->board[ny * 8 + nx];
            if (p != '.') {
                if (p == rq || (i < 4 && p == rr) || (i >= 4 && p == rb)) return 1;
                break;
            }
        }
    }
    return 0;
}

int generate_pseudo_moves(State* s, Move* moves) {
    int count = 0;
    for (int i = 0; i < 64; i++) {
        char p = s->board[i];
        if (p == '.') continue;
        int is_black = islower(p) ? 1 : 0;
        if (is_black != s->turn) continue;
        int sx = i % 8, sy = i / 8;
        char up = toupper(p);

        if (up == 'P') {
            int dir = is_black ? 1 : -1;
            int start_rank = is_black ? 1 : 6;
            int promo_rank = is_black ? 7 : 0;
            // Push
            if (s->board[(sy + dir) * 8 + sx] == '.') {
                Move m = {i, (sy + dir) * 8 + sx, 0, 0, 0, 0};
                if (sy + dir == promo_rank) {
                    char promos[] = {'q', 'r', 'b', 'n'};
                    for (int j = 0; j < 4; j++) { m.promo = promos[j]; moves[count++] = m; }
                } else {
                    moves[count++] = m;
                    // Double push
                    if (sy == start_rank && s->board[(sy + dir * 2) * 8 + sx] == '.') {
                        m.to = (sy + dir * 2) * 8 + sx; moves[count++] = m;
                    }
                }
            }
            // Captures
            for (int dx = -1; dx <= 1; dx += 2) {
                if (sx + dx >= 0 && sx + dx < 8) {
                    int t = (sy + dir) * 8 + sx + dx;
                    if ((s->board[t] != '.' && (islower(s->board[t]) ? 1 : 0) != is_black) || t == s->ep) {
                        Move m = {i, t, 0, 1, t == s->ep, 0};
                        if (sy + dir == promo_rank) {
                            char promos[] = {'q', 'r', 'b', 'n'};
                            for (int j = 0; j < 4; j++) { m.promo = promos[j]; moves[count++] = m; }
                        } else moves[count++] = m;
                    }
                }
            }
        } else if (up == 'N') {
            for (int d = 0; d < 8; d++) {
                int nx = sx + kx[d], ny = sy + ky[d];
                if (nx >= 0 && nx < 8 && ny >= 0 && ny < 8) {
                    int t = ny * 8 + nx;
                    if (s->board[t] == '.' || (islower(s->board[t]) ? 1 : 0) != is_black)
                        moves[count++] = (Move){i, t, 0, s->board[t] != '.', 0, 0};
                }
            }
        } else { // Sliders & King
            int start = (up == 'R' || up == 'K') ? 0 : 4;
            int end = (up == 'B') ? 8 : (up == 'K' ? 0 : 8);
            if (up == 'K') end = 8;
            int max_d = (up == 'K') ? 1 : 7;
            for (int d = start; d < end; d++) {
                for (int dist = 1; dist <= max_d; dist++) {
                    int nx = sx + dx[d] * dist, ny = sy + dy[d] * dist;
                    if (nx < 0 || nx > 7 || ny < 0 || ny > 7) break;
                    int t = ny * 8 + nx;
                    if (s->board[t] == '.') {
                        moves[count++] = (Move){i, t, 0, 0, 0, 0};
                    } else {
                        if ((islower(s->board[t]) ? 1 : 0) != is_black)
                            moves[count++] = (Move){i, t, 0, 1, 0, 0};
                        break;
                    }
                }
            }
            // Castling
            if (up == 'K' && !is_attacked(s, i, !is_black)) {
                if (!is_black) {
                    if (s->cwk && s->board[61] == '.' && s->board[62] == '.' && !is_attacked(s, 61, 1) && !is_attacked(s, 62, 1))
                        moves[count++] = (Move){60, 62, 0, 0, 0, 1};
                    if (s->cwq && s->board[59] == '.' && s->board[58] == '.' && s->board[57] == '.' && !is_attacked(s, 59, 1) && !is_attacked(s, 58, 1))
                        moves[count++] = (Move){60, 58, 0, 0, 0, 1};
                } else {
                    if (s->cbk && s->board[5] == '.' && s->board[6] == '.' && !is_attacked(s, 5, 0) && !is_attacked(s, 6, 0))
                        moves[count++] = (Move){4, 6, 0, 0, 0, 1};
                    if (s->cbq && s->board[3] == '.' && s->board[2] == '.' && s->board[1] == '.' && !is_attacked(s, 3, 0) && !is_attacked(s, 2, 0))
                        moves[count++] = (Move){4, 2, 0, 0, 0, 1};
                }
            }
        }
    }
    return count;
}

void apply_move(State* s, Move m) {
    char p = s->board[m.from];
    s->board[m.to] = m.promo ? (s->turn ? m.promo : toupper(m.promo)) : p;
    s->board[m.from] = '.';
    if (m.is_ep) s->board[m.to + (s->turn ? -8 : 8)] = '.';
    if (m.is_castle) {
        if (m.to == 62) { s->board[61] = 'R'; s->board[63] = '.'; } // wk
        else if (m.to == 58) { s->board[59] = 'R'; s->board[56] = '.'; } // wq
        else if (m.to == 6) { s->board[5] = 'r'; s->board[7] = '.'; } // bk
        else if (m.to == 2) { s->board[3] = 'r'; s->board[0] = '.'; } // bq
    }
    // Update castling rights
    if (m.from == 60) { s->cwk = 0; s->cwq = 0; }
    if (m.from == 4) { s->cbk = 0; s->cbq = 0; }
    if (m.from == 63 || m.to == 63) s->cwk = 0;
    if (m.from == 56 || m.to == 56) s->cwq = 0;
    if (m.from == 7 || m.to == 7) s->cbk = 0;
    if (m.from == 0 || m.to == 0) s->cbq = 0;

    s->ep = (toupper(p) == 'P' && abs(m.from - m.to) == 16) ? (m.from + m.to) / 2 : -1;
    s->turn ^= 1;
}

int generate_legal_moves(State* s, Move* legal_moves) {
    Move pseudo[MAX_MOVES];
    int p_count = generate_pseudo_moves(s, pseudo);
    int l_count = 0;
    for (int i = 0; i < p_count; i++) {
        State temp = *s;
        apply_move(&temp, pseudo[i]);
        int k_pos = -1;
        char kg = s->turn ? 'k' : 'K';
        for (int j = 0; j < 64; j++) if (temp.board[j] == kg) { k_pos = j; break; }
        if (!is_attacked(&temp, k_pos, !s->turn)) {
            legal_moves[l_count++] = pseudo[i];
        }
    }
    return l_count;
}

void move_to_uci(Move m, char* uci) {
    uci[0] = 'a' + (m.from % 8); uci[1] = '8' - (m.from / 8);
    uci[2] = 'a' + (m.to % 8);   uci[3] = '8' - (m.to / 8);
    if (m.promo) { uci[4] = m.promo; uci[5] = 0; }
    else uci[4] = 0;
}

void generate_san(State* s, Move m, Move* legals, int l_count, char* san) {
    if (m.is_castle) { strcpy(san, m.to % 8 == 6 ? "O-O" : "O-O-O"); }
    else {
        char p = toupper(s->board[m.from]);
        int idx = 0;
        if (p != 'P') san[idx++] = p;

        // Disambiguation
        int file_conflict = 0, rank_conflict = 0, exact_conflict = 0;
        for (int i = 0; i < l_count; i++) {
            if (legals[i].from != m.from && legals[i].to == m.to && s->board[legals[i].from] == s->board[m.from]) {
                if (legals[i].from % 8 == m.from % 8) file_conflict = 1;
                else rank_conflict = 1;
                exact_conflict = 1;
            }
        }
        if (exact_conflict) {
            if (!file_conflict) san[idx++] = 'a' + (m.from % 8);
            else if (!rank_conflict) san[idx++] = '8' - (m.from / 8);
            else { san[idx++] = 'a' + (m.from % 8); san[idx++] = '8' - (m.from / 8); }
        } else if (p == 'P' && m.is_capture) {
            san[idx++] = 'a' + (m.from % 8);
        }

        if (m.is_capture) san[idx++] = 'x';
        san[idx++] = 'a' + (m.to % 8);
        san[idx++] = '8' - (m.to / 8);
        if (m.promo) { san[idx++] = '='; san[idx++] = toupper(m.promo); }
        san[idx] = 0;
    }

    State temp = *s;
    apply_move(&temp, m);
    Move next_legals[MAX_MOVES];
    int next_l_count = generate_legal_moves(&temp, next_legals);
    int opp_k = -1;
    char okg = temp.turn ? 'k' : 'K';
    for (int j = 0; j < 64; j++) if (temp.board[j] == okg) { opp_k = j; break; }
    int check = is_attacked(&temp, opp_k, !temp.turn);
    if (check) {
        strcat(san, next_l_count == 0 ? "#" : "+");
    } else if (next_l_count == 0) {
        // Stalemate notation (not standard SAN, but good for UI)
    }
}

// --- UCI Engine Interaction ---
void init_engine(const char* path) {
    if (pipe(eng_in) == -1 || pipe(eng_out) == -1) return;
    eng_pid = fork();
    if (eng_pid == 0) {
        dup2(eng_in[0], STDIN_FILENO); dup2(eng_out[1], STDOUT_FILENO);
        close(eng_in[0]); close(eng_in[1]); close(eng_out[0]); close(eng_out[1]);
        execl(path, path, NULL);
        exit(1);
    }
    close(eng_in[0]); close(eng_out[1]);
    fcntl(eng_out[0], F_SETFL, O_NONBLOCK);
    engine_active = 1;
    write(eng_in[1], "uci\n", 4);
    strcpy(status_msg, "Engine Loaded. White to move.");
}

void send_engine_move() {
    if (!engine_active) return;
    char cmd[4096] = "position startpos moves";
    for (int i = 0; i < hist_count; i++) {
        strcat(cmd, " "); strcat(cmd, uci_history[i]);
    }
    strcat(cmd, "\ngo movetime 1000\n");
    write(eng_in[1], cmd, strlen(cmd));
    strcpy(status_msg, "Engine thinking...");
}

void process_engine_output() {
    if (!engine_active) return;
    char buf[1024];
    int n = read(eng_out[0], buf, sizeof(buf) - 1);
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                eng_buf[eng_buf_len] = 0;
                if (strncmp(eng_buf, "bestmove ", 9) == 0) {
                    char uci[6];
                    sscanf(eng_buf + 9, "%5s", uci);
                    // Find and apply move
                    Move legals[MAX_MOVES];
                    int l_count = generate_legal_moves(&state, legals);
                    for (int j = 0; j < l_count; j++) {
                        char tmp_uci[8];
                        move_to_uci(legals[j], tmp_uci);
                        if (strcmp(tmp_uci, uci) == 0) {
                            history[hist_count] = state;
                            generate_san(&state, legals[j], legals, l_count, pgn_history[hist_count]);
                            strcpy(uci_history[hist_count], uci);
                            hist_count++;
                            apply_move(&state, legals[j]);
                            strcpy(status_msg, "Your turn.");
                            break;
                        }
                    }
                }
                eng_buf_len = 0;
            } else if (eng_buf_len < sizeof(eng_buf) - 1) {
                eng_buf[eng_buf_len++] = buf[i];
            }
        }
    }
}

// --- Main Loop ---
int main(int argc, char** argv) {
    if (argc > 1) init_engine(argv[1]);
    init_state();
    enable_raw_mode();

    while (1) {
        draw_board();

        if (engine_active && state.turn == 1) { // Black is engine
            process_engine_output();
            usleep(10000); // 10ms
            
            // Allow checking for quit or undo while engine thinks
            struct timeval tv = {0, 0};
            fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
                int c = get_key();
                if (c == 'q') break;
            }
            continue;
        }

        int c = get_key();
        if (c == 'q') break;
        if (c == 'u') {
            if (hist_count > 0) {
                hist_count--;
                state = history[hist_count];
                if (engine_active && hist_count > 0) { // Undo engine's move too
                    hist_count--;
                    state = history[hist_count];
                }
                selected_sq = -1;
                strcpy(status_msg, "Move undone.");
            }
            continue;
        }

        if (c == 1001 && cy > 0) cy--;
        if (c == 1002 && cy < 7) cy++;
        if (c == 1003 && cx < 7) cx++;
        if (c == 1004 && cx > 0) cx--;

        if (c == ' ' || c == '\r' || c == '\n') {
            int sq = cy * 8 + cx;
            if (selected_sq == -1) {
                if (state.board[sq] != '.' && (islower(state.board[sq]) ? 1 : 0) == state.turn) {
                    selected_sq = sq;
                }
            } else {
                if (sq == selected_sq) {
                    selected_sq = -1; // Deselect
                } else {
                    Move legals[MAX_MOVES];
                    int l_count = generate_legal_moves(&state, legals);
                    int moved = 0;
                    for (int i = 0; i < l_count; i++) {
                        if (legals[i].from == selected_sq && legals[i].to == sq) {
                            // Default auto-queen for terminal UI brevity
                            if (legals[i].promo && legals[i].promo != 'q') continue; 
                            
                            history[hist_count] = state;
                            generate_san(&state, legals[i], legals, l_count, pgn_history[hist_count]);
                            move_to_uci(legals[i], uci_history[hist_count]);
                            hist_count++;
                            
                            apply_move(&state, legals[i]);
                            selected_sq = -1;
                            moved = 1;
                            
                            if (engine_active) send_engine_move();
                            else strcpy(status_msg, state.turn == 0 ? "White's turn." : "Black's turn.");
                            
                            break;
                        }
                    }
                    if (!moved) strcpy(status_msg, "Illegal move.");
                }
            }
        }
    }
    return 0;
}
