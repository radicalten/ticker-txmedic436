#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <signal.h>
#include <ctype.h>
#include <time.h>

// --- Constants & Macros ---
#define SQ(r, c) (((r) << 4) | (c))
#define ROW(sq) ((sq) >> 4)
#define COL(sq) ((sq) & 7)
#define VALID(sq) (!((sq) & 0x88))

enum { EMPTY, wP, wN, wB, wR, wQ, wK, bP, bN, bB, bR, bQ, bK };
#define IS_WHITE(p) ((p) >= wP && (p) <= wK)
#define IS_BLACK(p) ((p) >= bP && (p) <= bK)
#define COLOR(p) (IS_WHITE(p) ? 0 : 1)

const char *PIECE_UNICODE[] = {
    " ", "♙", "♘", "♗", "♖", "♕", "♔", "♟", "♞", "♝", "♜", "♛", "♚"
};
const char PIECE_FEN[] = ".PNBRQKpnbrqk";

// --- Data Structures ---
typedef struct {
    int from, to, promo, captured, ep, castle;
    char san[16];
} Move;

typedef struct {
    int board[128];
    int turn; // 0: White, 1: Black
    int castle; // 1:WK, 2:WQ, 4:BK, 8:BQ
    int ep;
    int halfmove, fullmove;
    Move history[2048];
    int ply;
} State;

// --- Global Variables ---
State state;
int to_engine[2], from_engine[2];
pid_t engine_pid = -1;
int tc_mode = 0; // 0: Depth, 1: Nodes, 2: Time
int tc_val = 10;
int engine_color = 1; // 1: Black plays as engine
int game_over = 0;

// --- Terminal UI ---
struct termios orig_termios;
void disableRawMode() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); printf("\033[?25h"); }
void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

// --- Chess Logic ---
void init_board(State *s) {
    const char *fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    int r = 7, c = 0;
    for (int i = 0; i < 128; i++) s->board[i] = EMPTY;
    for (const char *p = fen; *p; p++) {
        if (*p == '/') { r--; c = 0; }
        else if (isdigit(*p)) c += *p - '0';
        else if (*p == ' ') break;
        else {
            for (int i = 1; i <= 12; i++) {
                if (PIECE_FEN[i] == *p) { s->board[SQ(r, c++)] = i; break; }
            }
        }
    }
    s->turn = 0; s->castle = 15; s->ep = -1; s->halfmove = 0; s->fullmove = 1; s->ply = 0;
}

int find_king(State *s, int color) {
    int king = color == 0 ? wK : bK;
    for (int i = 0; i < 128; i++) if (VALID(i) && s->board[i] == king) return i;
    return -1;
}

int is_attacked(State *s, int sq, int by_color) {
    int pdir = by_color == 0 ? -16 : 16;
    int pawn = by_color == 0 ? wP : bP;
    if (VALID(sq + pdir - 1) && s->board[sq + pdir - 1] == pawn) return 1;
    if (VALID(sq + pdir + 1) && s->board[sq + pdir + 1] == pawn) return 1;

    int n_offsets[] = {-33, -31, -18, -14, 14, 18, 31, 33};
    int knight = by_color == 0 ? wN : bN;
    for (int i = 0; i < 8; i++) if (VALID(sq + n_offsets[i]) && s->board[sq + n_offsets[i]] == knight) return 1;

    int k_offsets[] = {-17, -16, -15, -1, 1, 15, 16, 17};
    int king = by_color == 0 ? wK : bK;
    for (int i = 0; i < 8; i++) if (VALID(sq + k_offsets[i]) && s->board[sq + k_offsets[i]] == king) return 1;

    int b_dirs[] = {-17, -15, 15, 17};
    int r_dirs[] = {-16, -1, 1, 16};
    int bishop = by_color == 0 ? wB : bB;
    int rook = by_color == 0 ? wR : bR;
    int queen = by_color == 0 ? wQ : bQ;
    for (int i = 0; i < 4; i++) {
        for (int d = 1; d < 8; d++) { int t = sq + b_dirs[i]*d; if(!VALID(t))break; int p=s->board[t]; if(p!=EMPTY){ if(p==bishop||p==queen)return 1; break; } }
        for (int d = 1; d < 8; d++) { int t = sq + r_dirs[i]*d; if(!VALID(t))break; int p=s->board[t]; if(p!=EMPTY){ if(p==rook||p==queen)return 1; break; } }
    }
    return 0;
}

void make_move(State *s, Move *m) {
    m->captured = s->board[m->to];
    m->ep = s->ep;
    m->castle = s->castle;
    
    s->board[m->to] = s->board[m->from];
    s->board[m->from] = EMPTY;
    
    if (s->board[m->to] == wP && ROW(m->to) == 7) s->board[m->to] = m->promo ? m->promo : wQ;
    if (s->board[m->to] == bP && ROW(m->to) == 0) s->board[m->to] = m->promo ? m->promo : bQ;
    
    if (s->board[m->to] == wP && m->to - m->from == 32) s->ep = m->from + 16;
    else if (s->board[m->to] == bP && m->from - m->to == 32) s->ep = m->from - 16;
    else s->ep = -1;

    if (m->to == s->ep && (s->board[m->to] == wP || s->board[m->to] == bP)) {
        s->board[m->to + (s->turn == 0 ? -16 : 16)] = EMPTY;
    }

    if (s->board[m->to] == wK) { s->castle &= ~3; if (m->to == SQ(0,6)) { s->board[SQ(0,5)] = wR; s->board[SQ(0,7)] = EMPTY; } if (m->to == SQ(0,2)) { s->board[SQ(0,3)] = wR; s->board[SQ(0,0)] = EMPTY; } }
    if (s->board[m->to] == bK) { s->castle &= ~12; if (m->to == SQ(7,6)) { s->board[SQ(7,5)] = bR; s->board[SQ(7,7)] = EMPTY; } if (m->to == SQ(7,2)) { s->board[SQ(7,3)] = bR; s->board[SQ(7,0)] = EMPTY; } }
    
    if (m->from == SQ(0,0) || m->to == SQ(0,0)) s->castle &= ~2;
    if (m->from == SQ(0,7) || m->to == SQ(0,7)) s->castle &= ~1;
    if (m->from == SQ(7,0) || m->to == SQ(7,0)) s->castle &= ~8;
    if (m->from == SQ(7,7) || m->to == SQ(7,7)) s->castle &= ~4;

    s->turn ^= 1;
    if (s->turn == 0) s->fullmove++;
}

void unmake_move(State *s, Move *m) {
    s->turn ^= 1;
    if (s->turn == 1) s->fullmove--;
    
    s->board[m->from] = s->board[m->to];
    s->board[m->to] = m->captured;
    
    if (m->promo) s->board[m->from] = (s->turn == 0 ? wP : bP);
    
    if (m->to == m->ep && (s->board[m->from] == wP || s->board[m->from] == bP)) {
        s->board[m->to] = EMPTY;
        s->board[m->to + (s->turn == 0 ? -16 : 16)] = (s->turn == 0 ? bP : wP);
    }

    if (s->board[m->from] == wK) { if (m->to == SQ(0,6)) { s->board[SQ(0,7)] = wR; s->board[SQ(0,5)] = EMPTY; } if (m->to == SQ(0,2)) { s->board[SQ(0,0)] = wR; s->board[SQ(0,3)] = EMPTY; } }
    if (s->board[m->from] == bK) { if (m->to == SQ(7,6)) { s->board[SQ(7,7)] = bR; s->board[SQ(7,5)] = EMPTY; } if (m->to == SQ(7,2)) { s->board[SQ(7,0)] = bR; s->board[SQ(7,3)] = EMPTY; } }

    s->ep = m->ep;
    s->castle = m->castle;
}

void generate_san(State *s, Move *m) {
    char *p = m->san;
    int piece = s->board[m->from];
    int is_capture = m->captured != EMPTY || m->to == s->ep;
    
    if (piece == wK || piece == bK) {
        if (m->to - m->from == 2) { strcpy(m->san, "O-O"); return; }
        if (m->from - m->to == 2) { strcpy(m->san, "O-O-O"); return; }
    }

    if (piece != wP && piece != bP) *p++ = PIECE_FEN[piece];
    else if (is_capture) *p++ = 'a' + COL(m->from);

    if (is_capture) *p++ = 'x';
    *p++ = 'a' + COL(m->to);
    *p++ = '1' + ROW(m->to);
    if (m->promo) { *p++ = '='; *p++ = PIECE_FEN[m->promo]; }
    
    make_move(s, m);
    int king_sq = find_king(s, s->turn);
    if (is_attacked(s, king_sq, s->turn ^ 1)) {
        Move moves[256]; int count = 0;
        // Simple check for mate (generate moves to see if any exist)
        // For brevity, just append '+'
        *p++ = '+'; 
    }
    unmake_move(s, m);
    *p = '\0';
}

void add_move(State *s, int from, int to, int promo, Move *moves, int *count) {
    Move m = {from, to, promo, 0, 0, 0};
    generate_san(s, &m);
    make_move(s, &m);
    int king_sq = find_king(s, s->turn ^ 1);
    if (!is_attacked(s, king_sq, s->turn)) moves[(*count)++] = m;
    unmake_move(s, &m);
}

int generate_legal_moves(State *s, Move *moves) {
    int count = 0;
    for (int sq = 0; sq < 128; sq++) {
        if (!VALID(sq) || s->board[sq] == EMPTY || COLOR(s->board[sq]) != s->turn) continue;
        int p = s->board[sq];
        if (p == wP) {
            if (VALID(sq+16) && s->board[sq+16]==EMPTY) { add_move(s, sq, sq+16, 0, moves, &count); if (ROW(sq)==1 && s->board[sq+32]==EMPTY) add_move(s, sq, sq+32, 0, moves, &count); }
            if (VALID(sq+15) && (IS_BLACK(s->board[sq+15]) || sq+15==s->ep)) { for(int pr=wQ; pr<=wN; pr++) add_move(s, sq, sq+15, ROW(sq)==6?pr:0, moves, &count); }
            if (VALID(sq+17) && (IS_BLACK(s->board[sq+17]) || sq+17==s->ep)) { for(int pr=wQ; pr<=wN; pr++) add_move(s, sq, sq+17, ROW(sq)==6?pr:0, moves, &count); }
        } else if (p == bP) {
            if (VALID(sq-16) && s->board[sq-16]==EMPTY) { add_move(s, sq, sq-16, 0, moves, &count); if (ROW(sq)==6 && s->board[sq-32]==EMPTY) add_move(s, sq, sq-32, 0, moves, &count); }
            if (VALID(sq-15) && (IS_WHITE(s->board[sq-15]) || sq-15==s->ep)) { for(int pr=bQ; pr<=bN; pr++) add_move(s, sq, sq-15, ROW(sq)==1?pr:0, moves, &count); }
            if (VALID(sq-17) && (IS_WHITE(s->board[sq-17]) || sq-17==s->ep)) { for(int pr=bQ; pr<=bN; pr++) add_move(s, sq, sq-17, ROW(sq)==1?pr:0, moves, &count); }
        } else if (p == wN || p == bN) {
            int offs[] = {-33, -31, -18, -14, 14, 18, 31, 33};
            for (int i=0; i<8; i++) if (VALID(sq+offs[i]) && (s->board[sq+offs[i]]==EMPTY || COLOR(s->board[sq+offs[i]])!=s->turn)) add_move(s, sq, sq+offs[i], 0, moves, &count);
        } else if (p == wK || p == bK) {
            int offs[] = {-17, -16, -15, -1, 1, 15, 16, 17};
            for (int i=0; i<8; i++) if (VALID(sq+offs[i]) && (s->board[sq+offs[i]]==EMPTY || COLOR(s->board[sq+offs[i]])!=s->turn)) add_move(s, sq, sq+offs[i], 0, moves, &count);
            if (p == wK && sq == SQ(0,4)) {
                if ((s->castle&1) && s->board[SQ(0,5)]==EMPTY && s->board[SQ(0,6)]==EMPTY && !is_attacked(s, SQ(0,4), 1) && !is_attacked(s, SQ(0,5), 1) && !is_attacked(s, SQ(0,6), 1)) add_move(s, sq, SQ(0,6), 0, moves, &count);
                if ((s->castle&2) && s->board[SQ(0,3)]==EMPTY && s->board[SQ(0,2)]==EMPTY && s->board[SQ(0,1)]==EMPTY && !is_attacked(s, SQ(0,4), 1) && !is_attacked(s, SQ(0,3), 1) && !is_attacked(s, SQ(0,2), 1)) add_move(s, sq, SQ(0,2), 0, moves, &count);
            }
            if (p == bK && sq == SQ(7,4)) {
                if ((s->castle&4) && s->board[SQ(7,5)]==EMPTY && s->board[SQ(7,6)]==EMPTY && !is_attacked(s, SQ(7,4), 0) && !is_attacked(s, SQ(7,5), 0) && !is_attacked(s, SQ(7,6), 0)) add_move(s, sq, SQ(7,6), 0, moves, &count);
                if ((s->castle&8) && s->board[SQ(7,3)]==EMPTY && s->board[SQ(7,2)]==EMPTY && s->board[SQ(7,1)]==EMPTY && !is_attacked(s, SQ(7,4), 0) && !is_attacked(s, SQ(7,3), 0) && !is_attacked(s, SQ(7,2), 0)) add_move(s, sq, SQ(7,2), 0, moves, &count);
            }
        } else {
            int b_dirs[] = {-17, -15, 15, 17};
            int r_dirs[] = {-16, -1, 1, 16};
            int is_bishop = (p == wB || p == bB || p == wQ || p == bQ);
            int is_rook = (p == wR || p == bR || p == wQ || p == bQ);
            if (is_bishop) for(int i=0;i<4;i++) for(int d=1;d<8;d++){int t=sq+b_dirs[i]*d; if(!VALID(t))break; if(s->board[t]!=EMPTY){if(COLOR(s->board[t])!=s->turn)add_move(s,sq,t,0,moves,&count); break;} add_move(s,sq,t,0,moves,&count);}
            if (is_rook) for(int i=0;i<4;i++) for(int d=1;d<8;d++){int t=sq+r_dirs[i]*d; if(!VALID(t))break; if(s->board[t]!=EMPTY){if(COLOR(s->board[t])!=s->turn)add_move(s,sq,t,0,moves,&count); break;} add_move(s,sq,t,0,moves,&count);}
        }
    }
    return count;
}

// --- UCI Engine Logic ---
void start_engine() {
    pipe(to_engine); pipe(from_engine);
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        close(to_engine[0]); close(to_engine[1]);
        close(from_engine[0]); close(from_engine[1]);
        execlp("stockfish", "stockfish", NULL);
        exit(1);
    }
    close(to_engine[0]); close(from_engine[1]);
    fcntl(from_engine[0], F_SETFL, O_NONBLOCK);
    dprintf(to_engine[1], "uci\nisready\n");
}

void get_fen(State *s, char *fen) {
    char *p = fen;
    for (int r = 7; r >= 0; r--) {
        int empty = 0;
        for (int c = 0; c < 8; c++) {
            int piece = s->board[SQ(r, c)];
            if (piece == EMPTY) empty++;
            else { if (empty > 0) { p += sprintf(p, "%d", empty); empty = 0; } *p++ = PIECE_FEN[piece]; }
        }
        if (empty > 0) p += sprintf(p, "%d", empty);
        if (r > 0) *p++ = '/';
    }
    p += sprintf(p, " %c ", s->turn == 0 ? 'w' : 'b');
    if (s->castle == 0) *p++ = '-';
    else { if (s->castle & 1) *p++ = 'K'; if (s->castle & 2) *p++ = 'Q'; if (s->castle & 4) *p++ = 'k'; if (s->castle & 8) *p++ = 'q'; }
    if (s->ep == -1) p += sprintf(p, " -");
    else p += sprintf(p, " %c%c", 'a' + COL(s->ep), '1' + ROW(s->ep));
    sprintf(p, " %d %d", s->halfmove, s->fullmove);
}

void engine_play(State *s) {
    char fen[256]; get_fen(s, fen);
    dprintf(to_engine[1], "position fen %s\n", fen);
    if (tc_mode == 0) dprintf(to_engine[1], "go depth %d\n", tc_val);
    else if (tc_mode == 1) dprintf(to_engine[1], "go nodes %d\n", tc_val);
    else dprintf(to_engine[1], "go movetime %d\n", tc_val);

    char line[1024]; int i = 0;
    while (1) {
        char c;
        if (read(from_engine[0], &c, 1) == 1) {
            if (c == '\n') {
                line[i] = '\0'; i = 0;
                if (strncmp(line, "bestmove", 8) == 0) {
                    int from = SQ(line[10]-'1', line[9]-'a');
                    int to = SQ(line[12]-'1', line[11]-'a');
                    int promo = 0;
                    if (line[13] != ' ') {
                        if (line[13] == 'q') promo = s->turn == 0 ? wQ : bQ;
                        if (line[13] == 'r') promo = s->turn == 0 ? wR : bR;
                        if (line[13] == 'b') promo = s->turn == 0 ? wB : bB;
                        if (line[13] == 'n') promo = s->turn == 0 ? wN : bN;
                    }
                    Move moves[256]; int count = generate_legal_moves(s, moves);
                    for (int j = 0; j < count; j++) {
                        if (moves[j].from == from && moves[j].to == to && moves[j].promo == promo) {
                            s->history[s->ply++] = moves[j];
                            make_move(s, &moves[j]);
                            return;
                        }
                    }
                }
            } else if (i < 1023) line[i++] = c;
        } else usleep(10000);
    }
}

// --- UI & Input ---
void draw_board(State *s, int cx, int cy, int sel, Move *legal_moves, int legal_count, int last_from, int last_to) {
    printf("\033[H"); // Return to top left
    int king_sq = find_king(s, s->turn);
    int in_check = is_attacked(s, king_sq, s->turn ^ 1);
    
    for (int r = 7; r >= 0; r--) {
        printf("  %d ", r + 1);
        for (int c = 0; c < 8; c++) {
            int sq = SQ(r, c);
            int p = s->board[sq];
            int is_light = (r + c) % 2 != 0;
            int bg = is_light ? 250 : 240;

            if (sq == sel) bg = 220; // Yellow
            else if (sq == last_from || sq == last_to) bg = 117; // Blue
            else if (in_check && sq == king_sq) bg = 196; // Red
            else {
                for (int i = 0; i < legal_count; i++) {
                    if (legal_moves[i].to == sq) { bg = 114; break; } // Green
                }
            }

            printf("\033[48;5;%dm", bg);
            if (p == EMPTY) printf("   ");
            else {
                if (IS_WHITE(p)) printf("\033[38;5;255m");
                else printf("\033[38;5;0m");
                printf(" %s ", PIECE_UNICODE[p]);
            }
            printf("\033[0m");
        }
        printf("\n");
    }
    printf("    a  b  c  d  e  f  g  h\n\n");

    printf("Turn: %s | ", s->turn == 0 ? "White" : "Black");
    printf("Time Control: %s %d | ", tc_mode == 0 ? "Depth" : (tc_mode == 1 ? "Nodes" : "Time"), tc_val);
    if (game_over) printf("GAME OVER\n");
    else if (in_check) printf("CHECK!\n");
    else printf("\n");

    printf("\nPGN: ");
    for (int i = 0; i < s->ply; i++) {
        if (i % 2 == 0) printf("%d. ", i / 2 + 1);
        printf("%s ", s->history[i].san);
    }
    printf("\n\n[Arrows] Move  [Enter] Select/Place  [u] Undo  [t] Time Control  [q] Quit\n");
}

void time_control_menu() {
    printf("\033[2J\033[H");
    printf("--- Time Control Settings ---\n");
    printf("1. Depth (Current: %d)\n", tc_mode == 0 ? tc_val : 10);
    printf("2. Nodes (Current: %d)\n", tc_mode == 1 ? tc_val : 100000);
    printf("3. Time in ms (Current: %d)\n", tc_mode == 2 ? tc_val : 1000);
    printf("Select mode (1-3): ");
    
    char c;
    while (read(STDIN_FILENO, &c, 1) != 1);
    if (c >= '1' && c <= '3') {
        tc_mode = c - '1';
        printf("\nEnter value: ");
        char buf[16]; int i = 0;
        while (1) {
            read(STDIN_FILENO, &c, 1);
            if (c == '\n' || c == '\r') break;
            if (isdigit(c) && i < 15) { buf[i++] = c; printf("%c", c); }
        }
        buf[i] = '\0';
        tc_val = atoi(buf);
    }
    printf("\033[2J");
}

int main() {
    init_board(&state);
    start_engine();
    enableRawMode();
    printf("\033[2J");

    int cx = 4, cy = 0; // Cursor file, rank
    int sel = -1;
    Move legal_moves[256]; int legal_count = 0;
    int last_from = -1, last_to = -1;

    while (1) {
        if (state.turn == engine_color && !game_over) {
            draw_board(&state, cx, cy, sel, legal_moves, legal_count, last_from, last_to);
            engine_play(&state);
            last_from = state.history[state.ply - 1].from;
            last_to = state.history[state.ply - 1].to;
            sel = -1; legal_count = 0;
        }

        Move all_moves[256];
        int all_count = generate_legal_moves(&state, all_moves);
        if (all_count == 0) game_over = 1;

        draw_board(&state, cx, cy, sel, legal_moves, legal_count, last_from, last_to);

        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) continue;

        if (c == 'q') break;
        else if (c == 'u') {
            if (state.ply > 0) {
                unmake_move(&state, &state.history[--state.ply]);
                if (state.turn == engine_color && state.ply > 0) {
                    unmake_move(&state, &state.history[--state.ply]);
                }
                sel = -1; legal_count = 0;
                last_from = state.ply > 0 ? state.history[state.ply - 1].from : -1;
                last_to = state.ply > 0 ? state.history[state.ply - 1].to : -1;
                game_over = 0;
            }
        } else if (c == 't') {
            time_control_menu();
        } else if (c == '\033') {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) != 1) continue;
            if (read(STDIN_FILENO, &seq[1], 1) != 1) continue;
            if (seq[0] == '[') {
                if (seq[1] == 'A' && cy < 7) cy++;
                if (seq[1] == 'B' && cy > 0) cy--;
                if (seq[1] == 'C' && cx < 7) cx++;
                if (seq[1] == 'D' && cx > 0) cx--;
            }
        } else if (c == '\n' || c == '\r' || c == ' ') {
            if (game_over || state.turn == engine_color) continue;
            int sq = SQ(cy, cx);
            if (sel == -1) {
                if (state.board[sq] != EMPTY && COLOR(state.board[sq]) == state.turn) {
                    sel = sq;
                    legal_count = 0;
                    for (int i = 0; i < all_count; i++) {
                        if (all_moves[i].from == sq) legal_moves[legal_count++] = all_moves[i];
                    }
                }
            } else {
                int moved = 0;
                for (int i = 0; i < legal_count; i++) {
                    if (legal_moves[i].to == sq) {
                        state.history[state.ply++] = legal_moves[i];
                        make_move(&state, &legal_moves[i]);
                        last_from = legal_moves[i].from;
                        last_to = legal_moves[i].to;
                        moved = 1;
                        break;
                    }
                }
                sel = -1; legal_count = 0;
                if (!moved && state.board[sq] != EMPTY && COLOR(state.board[sq]) == state.turn) {
                    sel = sq;
                    for (int i = 0; i < all_count; i++) {
                        if (all_moves[i].from == sq) legal_moves[legal_count++] = all_moves[i];
                    }
                }
            }
        }
    }
    return 0;
}
