/*
 * Single-file Terminal Chess GUI with UCI Engine Support
 * 
 * Compile: gcc -O3 chess.c -o chess
 * Run:     ./chess [path_to_stockfish]
 *          (Defaults to "stockfish" if no path is provided)
 *
 * Controls:
 *   Arrow Keys : Move cursor
 *   Space/Enter: Select piece / Make move
 *   u          : Undo move
 *   t          : Cycle time control mode (Depth -> Time -> Nodes)
 *   + / -      : Adjust time control value
 *   q          : Quit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/wait.h>
#include <ctype.h>
#include <signal.h>

// --- Constants & Macros ---
#define EMPTY 0
#define W_P 1
#define W_N 2
#define W_B 3
#define W_R 4
#define W_Q 5
#define W_K 6
#define B_P -1
#define B_N -2
#define B_B -3
#define B_R -4
#define B_Q -5
#define B_K -6

#define COL_RESET   "\033[0m"
#define COL_LIGHT   "\033[48;5;250m"
#define COL_DARK    "\033[48;5;238m"
#define COL_SEL     "\033[48;5;220m"
#define COL_LEGAL   "\033[48;5;114m"
#define COL_LAST    "\033[48;5;117m"
#define COL_CHECK   "\033[48;5;196m"
#define COL_W_PIECE "\033[38;5;255m"
#define COL_B_PIECE "\033[38;5;0m"

// --- Global State ---
int board[128];
int side; // 0 = White, 1 = Black
int castle; // 1=WK, 2=WQ, 4=BK, 8=BQ
int ep; // En passant square
int hply; // History ply
int king_sq[2];

struct Hist {
    int move;
    int castle;
    int ep;
    int cap;
    int king_sq;
} hist[1024];

int pseudo_moves[256];
int num_pseudo;
int legal_moves[256];
int num_legal;
char san_history[1024][16];

// UCI Engine State
int to_engine[2], from_engine[2];
pid_t engine_pid;
char engine_path[256] = "stockfish";
int tc_mode = 0; // 0=depth, 1=time, 2=nodes
int tc_val[3] = {10, 1000, 100000};

// GUI State
int cursor_sq = 0;
int selected_sq = -1;
int game_over = 0;
struct termios orig_termios;

// --- Terminal Functions ---
void disable_raw_mode() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); }
void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int get_key() {
    int c = getchar();
    if (c == 27) {
        if (getchar() == '[') {
            c = getchar();
            if (c == 'A') return 'U';
            if (c == 'B') return 'D';
            if (c == 'C') return 'R';
            if (c == 'D') return 'L';
        }
    }
    return c;
}

// --- Chess Core ---
void init_board() {
    const char *fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    memset(board, 0, sizeof(board));
    int sq = 112; // a8
    for (int i = 0; fen[i] && fen[i] != ' '; i++) {
        if (fen[i] == '/') sq -= 24;
        else if (isdigit(fen[i])) sq += fen[i] - '0';
        else {
            int p = EMPTY;
            switch (tolower(fen[i])) {
                case 'p': p = B_P; break; case 'n': p = B_N; break;
                case 'b': p = B_B; break; case 'r': p = B_R; break;
                case 'q': p = B_Q; break; case 'k': p = B_K; break;
            }
            if (isupper(fen[i])) p = -p; // White pieces are positive
            board[sq++] = p;
        }
    }
    side = 0; castle = 15; ep = -1; hply = 0;
    king_sq[0] = 4; king_sq[1] = 116;
}

int is_attacked(int sq, int by_side) {
    // Pawns
    int p_dir = by_side ? -16 : 16;
    if (!(sq + p_dir + 1 & 0x88) && board[sq + p_dir + 1] == (by_side ? B_P : W_P)) return 1;
    if (!(sq + p_dir - 1 & 0x88) && board[sq + p_dir - 1] == (by_side ? B_P : W_P)) return 1;
    // Knights
    int knight_dir[8] = {-33, -31, -18, -14, 14, 18, 31, 33};
    for(int i=0; i<8; i++) { int t = sq + knight_dir[i]; if (!(t & 0x88) && board[t] == (by_side ? B_N : W_N)) return 1; }
    // Kings
    int king_dir[8] = {-17, -16, -15, -1, 1, 15, 16, 17};
    for(int i=0; i<8; i++) { int t = sq + king_dir[i]; if (!(t & 0x88) && board[t] == (by_side ? B_K : W_K)) return 1; }
    // Bishops/Queens
    int diag_dir[4] = {-17, -15, 15, 17};
    for(int i=0; i<4; i++) { int t = sq + diag_dir[i]; while(!(t & 0x88)) { int p = board[t]; if (p) { if (p == (by_side ? B_B : W_B) || p == (by_side ? B_Q : W_Q)) return 1; break; } t += diag_dir[i]; } }
    // Rooks/Queens
    int straight_dir[4] = {-16, -1, 1, 16};
    for(int i=0; i<4; i++) { int t = sq + straight_dir[i]; while(!(t & 0x88)) { int p = board[t]; if (p) { if (p == (by_side ? B_R : W_R) || p == (by_side ? B_Q : W_Q)) return 1; break; } t += straight_dir[i]; } }
    return 0;
}

void add_move(int from, int to, int promo, int flags) {
    pseudo_moves[num_pseudo++] = from | (to << 8) | (promo << 16) | (flags << 24);
}

void gen_pseudo_moves() {
    num_pseudo = 0;
    for (int sq = 0; sq < 128; sq++) {
        if (sq & 0x88) continue;
        int p = board[sq];
        if (p == EMPTY || (side == 0 && p < 0) || (side == 1 && p > 0)) continue;
        int type = abs(p);

        if (type == 1) { // Pawn
            int p_dir = side ? -16 : 16;
            int to = sq + p_dir;
            if (!(to & 0x88) && board[to] == EMPTY) {
                if ((to >> 4) == (side ? 0 : 7)) {
                    add_move(sq, to, 5, 0); add_move(sq, to, 4, 0); add_move(sq, to, 3, 0); add_move(sq, to, 2, 0);
                } else {
                    add_move(sq, to, 0, 0);
                    if ((sq >> 4) == (side ? 6 : 1)) {
                        int to2 = sq + 2 * p_dir;
                        if (board[to2] == EMPTY) add_move(sq, to2, 0, 4);
                    }
                }
            }
            int cap_dirs[2] = {p_dir - 1, p_dir + 1};
            for (int i = 0; i < 2; i++) {
                int c_to = sq + cap_dirs[i];
                if (c_to & 0x88) continue;
                int target = board[c_to];
                if ((target != EMPTY && ((side == 0 && target < 0) || (side == 1 && target > 0))) || c_to == ep) {
                    if ((c_to >> 4) == (side ? 0 : 7)) {
                        add_move(sq, c_to, 5, c_to == ep ? 1 : 0); add_move(sq, c_to, 4, c_to == ep ? 1 : 0);
                        add_move(sq, c_to, 3, c_to == ep ? 1 : 0); add_move(sq, c_to, 2, c_to == ep ? 1 : 0);
                    } else {
                        add_move(sq, c_to, 0, c_to == ep ? 1 : 0);
                    }
                }
            }
        } else if (type == 2) { // Knight
            int d[] = {-33, -31, -18, -14, 14, 18, 31, 33};
            for(int i=0; i<8; i++) { int t = sq + d[i]; if(!(t&0x88)) { int target=board[t]; if(target==EMPTY || (side==0&&target<0)||(side==1&&target>0)) add_move(sq,t,0,0); } }
        } else if (type == 6) { // King
            int d[] = {-17, -16, -15, -1, 1, 15, 16, 17};
            for(int i=0; i<8; i++) { int t = sq + d[i]; if(!(t&0x88)) { int target=board[t]; if(target==EMPTY || (side==0&&target<0)||(side==1&&target>0)) add_move(sq,t,0,0); } }
            if (side == 0 && sq == 4) {
                if ((castle & 1) && board[5]==EMPTY && board[6]==EMPTY && !is_attacked(4,1) && !is_attacked(5,1) && !is_attacked(6,1)) add_move(4, 6, 0, 2);
                if ((castle & 2) && board[3]==EMPTY && board[2]==EMPTY && board[1]==EMPTY && !is_attacked(4,1) && !is_attacked(3,1) && !is_attacked(2,1)) add_move(4, 2, 0, 2);
            }
            if (side == 1 && sq == 116) {
                if ((castle & 4) && board[117]==EMPTY && board[118]==EMPTY && !is_attacked(116,0) && !is_attacked(117,0) && !is_attacked(118,0)) add_move(116, 118, 0, 2);
                if ((castle & 8) && board[115]==EMPTY && board[114]==EMPTY && board[113]==EMPTY && !is_attacked(116,0) && !is_attacked(115,0) && !is_attacked(114,0)) add_move(116, 114, 0, 2);
            }
        } else { // Sliding
            int dirs[8]; int num_dirs = 0;
            if (type == 3 || type == 5) { int d[] = {-17, -15, 15, 17}; for(int i=0;i<4;i++) dirs[num_dirs++]=d[i]; }
            if (type == 4 || type == 5) { int d[] = {-16, 16, -1, 1}; for(int i=0;i<4;i++) dirs[num_dirs++]=d[i]; }
            for(int i=0; i<num_dirs; i++) {
                int t = sq + dirs[i];
                while(!(t & 0x88)) {
                    int target = board[t];
                    if (target != EMPTY) {
                        if ((side == 0 && target < 0) || (side == 1 && target > 0)) add_move(sq, t, 0, 0);
                        break;
                    }
                    add_move(sq, t, 0, 0);
                    t += dirs[i];
                }
            }
        }
    }
}

void make_move(int move) {
    int from = move & 0xFF, to = (move >> 8) & 0xFF, promo = (move >> 16) & 0xFF, flags = (move >> 24) & 0xFF;
    int piece = board[from], cap = board[to];
    hist[hply] = (struct Hist){move, castle, ep, cap, king_sq[side]};
    board[to] = piece; board[from] = EMPTY;
    if (flags & 1) board[to + (side ? 16 : -16)] = EMPTY; // EP
    if (flags & 2) { // Castle
        if (to == 6) { board[5] = board[7]; board[7] = EMPTY; }
        else if (to == 2) { board[3] = board[0]; board[0] = EMPTY; }
        else if (to == 118) { board[117] = board[119]; board[119] = EMPTY; }
        else if (to == 114) { board[115] = board[112]; board[112] = EMPTY; }
    }
    if (promo) board[to] = side ? -promo : promo;
    ep = -1;
    if (abs(piece) == 1 && abs(to - from) == 32) ep = (from + to) / 2;
    if (from == 0 || to == 0) castle &= ~2; if (from == 7 || to == 7) castle &= ~1;
    if (from == 112 || to == 112) castle &= ~8; if (from == 119 || to == 119) castle &= ~4;
    if (from == 4 || to == 4) castle &= ~3; if (from == 116 || to == 116) castle &= ~12;
    if (abs(piece) == 6) king_sq[side] = to;
    side ^= 1; hply++;
}

void unmake_move() {
    hply--; side ^= 1;
    int move = hist[hply].move, from = move & 0xFF, to = (move >> 8) & 0xFF, flags = (move >> 24) & 0xFF;
    int piece = board[to], cap = hist[hply].cap;
    castle = hist[hply].castle; ep = hist[hply].ep; king_sq[side] = hist[hply].king_sq;
    if ((move >> 16) & 0xFF) piece = side ? B_P : W_P;
    board[from] = piece; board[to] = cap;
    if (flags & 1) { board[to + (side ? 16 : -16)] = side ? W_P : B_P; board[to] = EMPTY; }
    if (flags & 2) {
        if (to == 6) { board[7] = board[5]; board[5] = EMPTY; }
        else if (to == 2) { board[0] = board[3]; board[3] = EMPTY; }
        else if (to == 118) { board[119] = board[117]; board[117] = EMPTY; }
        else if (to == 114) { board[112] = board[115]; board[115] = EMPTY; }
    }
}

void gen_legal_moves() {
    num_legal = 0; gen_pseudo_moves();
    for(int i=0; i<num_pseudo; i++) {
        make_move(pseudo_moves[i]);
        if(!is_attacked(king_sq[side^1], side)) legal_moves[num_legal++] = pseudo_moves[i];
        unmake_move();
    }
}

void gen_fen(char *fen) {
    char *p = fen;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int c = 0; c < 8; c++) {
            int sq = r * 16 + c, piece = board[sq];
            if (piece == EMPTY) empty++;
            else {
                if (empty) { *p++ = '0' + empty; empty = 0; }
                char pc = " PNBRQK"[abs(piece)];
                if (piece < 0) pc = tolower(pc);
                *p++ = pc;
            }
        }
        if (empty) *p++ = '0' + empty;
        if (r > 0) *p++ = '/';
    }
    sprintf(p, " %c %s%s%s%s %s 0 1", side ? 'b' : 'w',
        castle & 1 ? "K" : "", castle & 2 ? "Q" : "", castle & 4 ? "k" : "", castle & 8 ? "q" : "",
        ep == -1 ? "-" : (char[]){'a' + (ep & 7), '1' + (ep >> 4), 0});
}

void get_san(int move, char *san) {
    int from = move & 0xFF, to = (move >> 8) & 0xFF, promo = (move >> 16) & 0xFF, flags = (move >> 24) & 0xFF;
    int piece = board[from], cap = board[to];
    if (flags & 1) cap = 1;
    char *p = san;
    if (flags & 2) { strcpy(p, to > from ? "O-O" : "O-O-O"); p += strlen(p); }
    else {
        if (abs(piece) != 1) {
            *p++ = " PNBRQK"[abs(piece)];
            for(int i=0; i<num_legal; i++) {
                int m = legal_moves[i];
                if (m == move) continue;
                int f = m & 0xFF, t = (m >> 8) & 0xFF;
                if (t == to && board[f] == piece) {
                    if ((f & 7) != (from & 7)) *p++ = 'a' + (from & 7);
                    else if ((f >> 4) != (from >> 4)) *p++ = '1' + (from >> 4);
                    else { *p++ = 'a' + (from & 7); *p++ = '1' + (from >> 4); }
                    break;
                }
            }
        } else if (cap) *p++ = 'a' + (from & 7);
        if (cap) *p++ = 'x';
        *p++ = 'a' + (to & 7); *p++ = '1' + (to >> 4);
        if (promo) *p++ = " PNBRQK"[promo];
    }
    make_move(move);
    if (is_attacked(king_sq[side], side^1)) {
        gen_legal_moves();
        *p++ = num_legal == 0 ? '#' : '+';
    }
    unmake_move();
    *p = '\0';
}

// --- UCI Engine ---
int read_line(int fd, char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c;
        if (read(fd, &c, 1) <= 0) return i;
        if (c == '\n') { buf[i] = '\0'; return i; }
        buf[i++] = c;
    }
    buf[i] = '\0'; return i;
}

void uci_init() {
    pipe(to_engine); pipe(from_engine);
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO); dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[0]); close(to_engine[1]); close(from_engine[0]); close(from_engine[1]);
        execlp(engine_path, engine_path, NULL); exit(1);
    }
    close(to_engine[0]); close(from_engine[1]);
    write(to_engine[1], "uci\n", 4);
    char line[1024];
    while(read_line(from_engine[0], line, sizeof(line)) > 0) if (strstr(line, "uciok")) break;
    write(to_engine[1], "isready\n", 8);
    while(read_line(from_engine[0], line, sizeof(line)) > 0) if (strstr(line, "readyok")) break;
}

void uci_quit() {
    write(to_engine[1], "quit\n", 5);
    waitpid(engine_pid, NULL, 0);
}

int uci_get_bestmove(char *uci_move) {
    char fen[256], cmd[512];
    gen_fen(fen);
    sprintf(cmd, "position fen %s\n", fen);
    write(to_engine[1], cmd, strlen(cmd));
    if (tc_mode == 0) sprintf(cmd, "go depth %d\n", tc_val[0]);
    else if (tc_mode == 1) sprintf(cmd, "go movetime %d\n", tc_val[1]);
    else sprintf(cmd, "go nodes %d\n", tc_val[2]);
    write(to_engine[1], cmd, strlen(cmd));
    char line[1024];
    while (read_line(from_engine[0], line, sizeof(line)) > 0) {
        if (strncmp(line, "bestmove ", 9) == 0) { sscanf(line, "bestmove %s", uci_move); return 1; }
    }
    return 0;
}

void do_engine_move() {
    gen_legal_moves();
    if (num_legal == 0) { game_over = 1; return; }
    char uci_move[16];
    if (uci_get_bestmove(uci_move)) {
        int from = (uci_move[0]-'a') + (uci_move[1]-'1')*16;
        int to = (uci_move[2]-'a') + (uci_move[3]-'1')*16;
        int promo = 0;
        if (uci_move[4] == 'q') promo = 5; else if (uci_move[4] == 'r') promo = 4;
        else if (uci_move[4] == 'b') promo = 3; else if (uci_move[4] == 'n') promo = 2;
        int move = -1;
        for(int i=0; i<num_legal; i++) {
            int m = legal_moves[i];
            if ((m & 0xFF) == from && ((m >> 8) & 0xFF) == to) {
                if (promo == 0 || ((m >> 16) & 0xFF) == promo) { move = m; break; }
            }
        }
        if (move != -1) {
            char san[16]; get_san(move, san); strcpy(san_history[hply], san);
            make_move(move);
        }
    }
    gen_legal_moves();
    if (num_legal == 0) game_over = 1;
}

// --- GUI ---
char *piece_char(int p) {
    switch(p) {
        case W_K: return "♔"; case W_Q: return "♕"; case W_R: return "♖";
        case W_B: return "♗"; case W_N: return "♘"; case W_P: return "♙";
        case B_K: return "♚"; case B_Q: return "♛"; case B_R: return "♜";
        case B_B: return "♝"; case B_N: return "♞"; case B_P: return "♟";
        default: return " ";
    }
}

void draw() {
    printf("\033[H\033[2J");
    int last_from = hply > 0 ? hist[hply-1].move & 0xFF : -1;
    int last_to = hply > 0 ? (hist[hply-1].move >> 8) & 0xFF : -1;
    int in_check = is_attacked(king_sq[side], side^1);

    for (int r = 7; r >= 0; r--) {
        printf("  %d ", r + 1);
        for (int c = 0; c < 8; c++) {
            int sq = r * 16 + c;
            char *bg = (r + c) % 2 != 0 ? COL_LIGHT : COL_DARK;
            if (sq == cursor_sq) bg = COL_SEL;
            else if (sq == last_from || sq == last_to) bg = COL_LAST;
            else if (in_check && sq == king_sq[side]) bg = COL_CHECK;
            else if (selected_sq != -1) {
                for(int i=0; i<num_legal; i++) {
                    if ((legal_moves[i] & 0xFF) == selected_sq && ((legal_moves[i] >> 8) & 0xFF) == sq) { bg = COL_LEGAL; break; }
                }
            }
            int p = board[sq];
            char *fg = p > 0 ? COL_W_PIECE : (p < 0 ? COL_B_PIECE : "");
            printf("%s%s %s %s", bg, fg, piece_char(p), COL_RESET);
        }
        printf("\n");
    }
    printf("    a  b  c  d  e  f  g  h\n\n");

    printf("Turn: %s %s\n", side == 0 ? "White" : "Black", game_over ? "(GAME OVER)" : (in_check ? "(CHECK!)" : ""));
    printf("Engine: %s | ", engine_path);
    if (tc_mode == 0) printf("Depth: %d", tc_val[0]);
    else if (tc_mode == 1) printf("Time: %d ms", tc_val[1]);
    else printf("Nodes: %d", tc_val[2]);
    printf("  [t] cycle, [+/-] adjust\n");

    printf("Moves: ");
    int start = hply > 20 ? hply - 20 : 0;
    for(int i=start; i<hply; i++) {
        if (i % 2 == 0) printf("%d. ", i/2 + 1);
        printf("%s ", san_history[i]);
    }
    printf("\n\nControls: Arrows=Move, Space=Select/Place, u=Undo, q=Quit\n");
}

void handle_sigint(int sig) { disable_raw_mode(); uci_quit(); exit(0); }

int main(int argc, char **argv) {
    if (argc > 1) strncpy(engine_path, argv[1], sizeof(engine_path)-1);
    signal(SIGINT, handle_sigint);
    
    init_board();
    uci_init();
    enable_raw_mode();
    gen_legal_moves();
    draw();

    while (1) {
        int key = get_key();
        if (key == 'q') break;
        
        if (!game_over && side == 0) { // Player is White
            if (key == 'U' && !(cursor_sq + 16 & 0x88)) cursor_sq += 16;
            if (key == 'D' && !(cursor_sq - 16 & 0x88)) cursor_sq -= 16;
            if (key == 'R' && !(cursor_sq + 1 & 0x88)) cursor_sq += 1;
            if (key == 'L' && !(cursor_sq - 1 & 0x88)) cursor_sq -= 1;

            if (key == ' ' || key == '\n') {
                if (selected_sq == -1) {
                    if (board[cursor_sq] != EMPTY && board[cursor_sq] > 0) {
                        selected_sq = cursor_sq;
                    }
                } else {
                    int move = -1;
                    for(int i=0; i<num_legal; i++) {
                        if ((legal_moves[i] & 0xFF) == selected_sq && ((legal_moves[i] >> 8) & 0xFF) == cursor_sq) {
                            move = legal_moves[i]; break;
                        }
                    }
                    if (move != -1) {
                        if (((move >> 16) & 0xFF) != 0) { // Auto-promote to Queen
                            for(int i=0; i<num_legal; i++) {
                                if ((legal_moves[i] & 0xFFFF) == (move & 0xFFFF) && ((legal_moves[i] >> 16) & 0xFF) == 5) { move = legal_moves[i]; break; }
                            }
                        }
                        char san[16]; get_san(move, san); strcpy(san_history[hply], san);
                        make_move(move);
                        selected_sq = -1;
                        gen_legal_moves();
                        if (num_legal == 0) game_over = 1;
                    } else {
                        selected_sq = -1;
                    }
                }
            }
        }

        if (key == 'u') {
            if (hply > 0) {
                unmake_move();
                if (side == 0 && hply > 0) unmake_move(); // Undo engine move too
                game_over = 0; selected_sq = -1; gen_legal_moves();
            }
        }

        if (key == 't') tc_mode = (tc_mode + 1) % 3;
        if (key == '+' || key == '=') {
            if (tc_mode == 0) tc_val[0] += 2;
            else if (tc_mode == 1) tc_val[1] += 500;
            else tc_val[2] += 50000;
        }
        if (key == '-') {
            if (tc_mode == 0 && tc_val[0] > 2) tc_val[0] -= 2;
            else if (tc_mode == 1 && tc_val[1] > 500) tc_val[1] -= 500;
            else if (tc_mode == 2 && tc_val[2] > 50000) tc_val[2] -= 50000;
        }

        draw();

        if (!game_over && side == 1) { // Engine is Black
            do_engine_move();
            draw();
        }
    }

    uci_quit();
    return 0;
}
