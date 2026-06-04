#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <ctype.h>

/* --- Constants & Macros --- */
#define EMPTY 0
#define PAWN 1
#define KNIGHT 2
#define BISHOP 3
#define ROOK 4
#define QUEEN 5
#define KING 6

#define WHITE 8
#define BLACK 16
#define COLOR_MASK 24
#define PIECE_MASK 7

#define SQ(x, y) ((y) * 8 + (x))
#define FILE(sq) ((sq) % 8)
#define RANK(sq) ((sq) / 8)

/* Castling Rights Bits */
#define CASTLE_WK 1
#define CASTLE_WQ 2
#define CASTLE_BK 4
#define CASTLE_BQ 8

/* --- Data Structures --- */
typedef struct {
    int src;
    int dst;
    int promote; // Piece type to promote to, 0 if none
} Move;

typedef struct {
    int board[64];
    int turn;
    int castling;
    int ep_square; // -1 if none
    int halfmove;
    int fullmove;
} State;

typedef struct {
    State state;
    char uci_move[6];
    char san_move[16];
} History;

/* --- Globals --- */
State current_state;
History game_history[1024];
int ply = 0;

int cursor_x = 4, cursor_y = 4;
int selected_sq = -1;
bool is_running = true;

char engine_path[256];
int tc_type = 1; // 1: movetime, 2: depth, 3: nodes
int tc_val = 1000;

int eng_in_fd, eng_out_fd;
pid_t engine_pid;

struct termios orig_termios;

/* --- Terminal Setup --- */
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\033[?25h"); // Show cursor
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    printf("\033[?25l"); // Hide cursor
}

/* --- Chess Logic --- */
void init_board(State *s) {
    int initial[64] = {
        BLACK|ROOK, BLACK|KNIGHT, BLACK|BISHOP, BLACK|QUEEN, BLACK|KING, BLACK|BISHOP, BLACK|KNIGHT, BLACK|ROOK,
        BLACK|PAWN, BLACK|PAWN,   BLACK|PAWN,   BLACK|PAWN,  BLACK|PAWN, BLACK|PAWN,   BLACK|PAWN,   BLACK|PAWN,
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY,
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY,
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY,
        EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY, EMPTY,
        WHITE|PAWN, WHITE|PAWN,   WHITE|PAWN,   WHITE|PAWN,  WHITE|PAWN, WHITE|PAWN,   WHITE|PAWN,   WHITE|PAWN,
        WHITE|ROOK, WHITE|KNIGHT, WHITE|BISHOP, WHITE|QUEEN, WHITE|KING, WHITE|BISHOP, WHITE|KNIGHT, WHITE|ROOK
    };
    memcpy(s->board, initial, sizeof(initial));
    s->turn = WHITE;
    s->castling = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
    s->ep_square = -1;
    s->halfmove = 0;
    s->fullmove = 1;
}

bool is_attacked(State *s, int sq, int attacker_color) {
    int x = FILE(sq), y = RANK(sq);
    int p_dir = (attacker_color == WHITE) ? 1 : -1;
    
    // Pawn
    if (y + p_dir >= 0 && y + p_dir < 8) {
        if (x > 0 && s->board[SQ(x - 1, y + p_dir)] == (attacker_color | PAWN)) return true;
        if (x < 7 && s->board[SQ(x + 1, y + p_dir)] == (attacker_color | PAWN)) return true;
    }
    
    // Knight
    int nx[] = {-2,-1,1,2,-2,-1,1,2};
    int ny[] = {-1,-2,-2,-1,1,2,2,1};
    for (int i=0; i<8; i++) {
        int cx = x + nx[i], cy = y + ny[i];
        if (cx>=0 && cx<8 && cy>=0 && cy<8 && s->board[SQ(cx,cy)] == (attacker_color | KNIGHT)) return true;
    }
    
    // King
    int kx[] = {-1,0,1,-1,1,-1,0,1};
    int ky[] = {-1,-1,-1,0,0,1,1,1};
    for (int i=0; i<8; i++) {
        int cx = x + kx[i], cy = y + ky[i];
        if (cx>=0 && cx<8 && cy>=0 && cy<8 && s->board[SQ(cx,cy)] == (attacker_color | KING)) return true;
    }
    
    // Sliders (Bishop, Rook, Queen)
    int dirs[8][2] = {{-1,-1},{1,-1},{-1,1},{1,1}, {-1,0},{1,0},{0,-1},{0,1}};
    for (int i=0; i<8; i++) {
        int cx = x, cy = y;
        bool is_diag = (i < 4);
        while (1) {
            cx += dirs[i][0]; cy += dirs[i][1];
            if (cx<0 || cx>7 || cy<0 || cy>7) break;
            int p = s->board[SQ(cx,cy)];
            if (p != EMPTY) {
                if ((p & COLOR_MASK) == attacker_color) {
                    int pt = p & PIECE_MASK;
                    if (pt == QUEEN || (is_diag && pt == BISHOP) || (!is_diag && pt == ROOK)) return true;
                }
                break;
            }
        }
    }
    return false;
}

void make_move(State *s, Move m) {
    int p = s->board[m.src];
    int pt = p & PIECE_MASK;
    int captured = s->board[m.dst];
    
    // En Passant capture
    if (pt == PAWN && m.dst == s->ep_square) {
        s->board[SQ(FILE(m.dst), RANK(m.src))] = EMPTY;
    }
    
    // Castling
    if (pt == KING && abs(FILE(m.dst) - FILE(m.src)) == 2) {
        if (FILE(m.dst) == 6) { // Kingside
            s->board[SQ(5, RANK(m.src))] = s->board[SQ(7, RANK(m.src))];
            s->board[SQ(7, RANK(m.src))] = EMPTY;
        } else { // Queenside
            s->board[SQ(3, RANK(m.src))] = s->board[SQ(0, RANK(m.src))];
            s->board[SQ(0, RANK(m.src))] = EMPTY;
        }
    }
    
    // Move piece
    s->board[m.dst] = p;
    s->board[m.src] = EMPTY;
    if (m.promote) s->board[m.dst] = (p & COLOR_MASK) | m.promote;
    
    // Update State
    s->ep_square = -1;
    if (pt == PAWN && abs(RANK(m.dst) - RANK(m.src)) == 2) {
        s->ep_square = SQ(FILE(m.src), (RANK(m.src) + RANK(m.dst)) / 2);
    }
    
    // Castling Rights
    if (pt == KING) {
        if (s->turn == WHITE) s->castling &= ~(CASTLE_WK | CASTLE_WQ);
        else s->castling &= ~(CASTLE_BK | CASTLE_BQ);
    }
    if (pt == ROOK || (captured & PIECE_MASK) == ROOK) {
        if (m.src == SQ(7,7) || m.dst == SQ(7,7)) s->castling &= ~CASTLE_WK;
        if (m.src == SQ(0,7) || m.dst == SQ(0,7)) s->castling &= ~CASTLE_WQ;
        if (m.src == SQ(7,0) || m.dst == SQ(7,0)) s->castling &= ~CASTLE_BK;
        if (m.src == SQ(0,0) || m.dst == SQ(0,0)) s->castling &= ~CASTLE_BQ;
    }
    
    if (s->turn == BLACK) s->fullmove++;
    s->turn = (s->turn == WHITE) ? BLACK : WHITE;
}

int generate_legal_moves(State *s, Move *moves) {
    int count = 0;
    int my_color = s->turn;
    int opp_color = (my_color == WHITE) ? BLACK : WHITE;
    
    for (int src = 0; src < 64; src++) {
        int p = s->board[src];
        if (p == EMPTY || (p & COLOR_MASK) != my_color) continue;
        int pt = p & PIECE_MASK;
        int x = FILE(src), y = RANK(src);
        
        Move pseudo[64];
        int p_count = 0;
        
        if (pt == PAWN) {
            int dir = (my_color == WHITE) ? -1 : 1;
            int start_rank = (my_color == WHITE) ? 6 : 1;
            int prom_rank = (my_color == WHITE) ? 0 : 7;
            
            // Forward 1
            if (s->board[SQ(x, y + dir)] == EMPTY) {
                pseudo[p_count++] = (Move){src, SQ(x, y + dir), 0};
                // Forward 2
                if (y == start_rank && s->board[SQ(x, y + 2*dir)] == EMPTY)
                    pseudo[p_count++] = (Move){src, SQ(x, y + 2*dir), 0};
            }
            // Captures
            for (int dx = -1; dx <= 1; dx += 2) {
                if (x + dx >= 0 && x + dx < 8) {
                    int dst = SQ(x + dx, y + dir);
                    if ((s->board[dst] != EMPTY && (s->board[dst] & COLOR_MASK) == opp_color) || dst == s->ep_square) {
                        pseudo[p_count++] = (Move){src, dst, 0};
                    }
                }
            }
            
            // Handle promotions
            for (int i=0; i<p_count; i++) {
                if (RANK(pseudo[i].dst) == prom_rank) {
                    pseudo[i].promote = QUEEN;
                    Move base = pseudo[i];
                    pseudo[p_count++] = (Move){base.src, base.dst, ROOK};
                    pseudo[p_count++] = (Move){base.src, base.dst, BISHOP};
                    pseudo[p_count++] = (Move){base.src, base.dst, KNIGHT};
                }
            }
        } 
        else if (pt == KNIGHT) {
            int nx[] = {-2,-1,1,2,-2,-1,1,2};
            int ny[] = {-1,-2,-2,-1,1,2,2,1};
            for (int i=0; i<8; i++) {
                int cx = x + nx[i], cy = y + ny[i];
                if (cx>=0 && cx<8 && cy>=0 && cy<8 && (s->board[SQ(cx,cy)] == EMPTY || (s->board[SQ(cx,cy)] & COLOR_MASK) == opp_color))
                    pseudo[p_count++] = (Move){src, SQ(cx, cy), 0};
            }
        }
        else if (pt == KING) {
            int kx[] = {-1,0,1,-1,1,-1,0,1};
            int ky[] = {-1,-1,-1,0,0,1,1,1};
            for (int i=0; i<8; i++) {
                int cx = x + kx[i], cy = y + ky[i];
                if (cx>=0 && cx<8 && cy>=0 && cy<8 && (s->board[SQ(cx,cy)] == EMPTY || (s->board[SQ(cx,cy)] & COLOR_MASK) == opp_color))
                    pseudo[p_count++] = (Move){src, SQ(cx, cy), 0};
            }
            // Castling
            if (!is_attacked(s, src, opp_color)) {
                if (my_color == WHITE) {
                    if ((s->castling & CASTLE_WK) && s->board[SQ(5,7)]==EMPTY && s->board[SQ(6,7)]==EMPTY && !is_attacked(s, SQ(5,7), opp_color) && !is_attacked(s, SQ(6,7), opp_color))
                        pseudo[p_count++] = (Move){src, SQ(6,7), 0};
                    if ((s->castling & CASTLE_WQ) && s->board[SQ(1,7)]==EMPTY && s->board[SQ(2,7)]==EMPTY && s->board[SQ(3,7)]==EMPTY && !is_attacked(s, SQ(3,7), opp_color) && !is_attacked(s, SQ(2,7), opp_color))
                        pseudo[p_count++] = (Move){src, SQ(2,7), 0};
                } else {
                    if ((s->castling & CASTLE_BK) && s->board[SQ(5,0)]==EMPTY && s->board[SQ(6,0)]==EMPTY && !is_attacked(s, SQ(5,0), opp_color) && !is_attacked(s, SQ(6,0), opp_color))
                        pseudo[p_count++] = (Move){src, SQ(6,0), 0};
                    if ((s->castling & CASTLE_BQ) && s->board[SQ(1,0)]==EMPTY && s->board[SQ(2,0)]==EMPTY && s->board[SQ(3,0)]==EMPTY && !is_attacked(s, SQ(3,0), opp_color) && !is_attacked(s, SQ(2,0), opp_color))
                        pseudo[p_count++] = (Move){src, SQ(2,0), 0};
                }
            }
        }
        else { // Sliders
            int dirs[8][2] = {{-1,-1},{1,-1},{-1,1},{1,1}, {-1,0},{1,0},{0,-1},{0,1}};
            int start = (pt == ROOK) ? 4 : 0;
            int end = (pt == BISHOP) ? 4 : 8;
            for (int i=start; i<end; i++) {
                int cx = x, cy = y;
                while (1) {
                    cx += dirs[i][0]; cy += dirs[i][1];
                    if (cx<0 || cx>7 || cy<0 || cy>7) break;
                    int target = s->board[SQ(cx, cy)];
                    if (target == EMPTY) pseudo[p_count++] = (Move){src, SQ(cx, cy), 0};
                    else {
                        if ((target & COLOR_MASK) == opp_color) pseudo[p_count++] = (Move){src, SQ(cx, cy), 0};
                        break;
                    }
                }
            }
        }
        
        // Validate pseudo-legal moves
        for (int i=0; i<p_count; i++) {
            State cpy = *s;
            make_move(&cpy, pseudo[i]);
            
            // Find our king
            int king_sq = -1;
            for (int j=0; j<64; j++) {
                if (cpy.board[j] == (my_color | KING)) { king_sq = j; break; }
            }
            if (!is_attacked(&cpy, king_sq, opp_color)) {
                if (moves) moves[count] = pseudo[i];
                count++;
            }
        }
    }
    return count;
}

void build_san(State *s, Move m, char *san) {
    int pt = s->board[m.src] & PIECE_MASK;
    int is_capture = (s->board[m.dst] != EMPTY || (pt == PAWN && m.dst == s->ep_square));
    
    if (pt == KING && abs(FILE(m.dst) - FILE(m.src)) == 2) {
        strcpy(san, (FILE(m.dst) == 6) ? "O-O" : "O-O-O");
    } else {
        char piece_char = " PNBRQK"[pt];
        int idx = 0;
        
        if (pt != PAWN) {
            san[idx++] = piece_char;
            
            // Disambiguation
            Move legals[256];
            int num_legals = generate_legal_moves(s, legals);
            bool file_amb = false, rank_amb = false, amb = false;
            
            for (int i=0; i<num_legals; i++) {
                if (legals[i].src != m.src && legals[i].dst == m.dst && (s->board[legals[i].src] & PIECE_MASK) == pt) {
                    amb = true;
                    if (FILE(legals[i].src) == FILE(m.src)) file_amb = true;
                    if (RANK(legals[i].src) == RANK(m.src)) rank_amb = true;
                }
            }
            if (amb) {
                if (!file_amb) san[idx++] = 'a' + FILE(m.src);
                else if (!rank_amb) san[idx++] = '8' - RANK(m.src);
                else {
                    san[idx++] = 'a' + FILE(m.src);
                    san[idx++] = '8' - RANK(m.src);
                }
            }
        } else if (is_capture) {
            san[idx++] = 'a' + FILE(m.src);
        }
        
        if (is_capture) san[idx++] = 'x';
        san[idx++] = 'a' + FILE(m.dst);
        san[idx++] = '8' - RANK(m.dst);
        
        if (m.promote) {
            san[idx++] = '=';
            san[idx++] = " PNBRQK"[m.promote];
        }
        san[idx] = '\0';
    }
    
    // Check/Mate
    State next = *s;
    make_move(&next, m);
    int num_responses = generate_legal_moves(&next, NULL);
    
    int king_sq = -1;
    for (int j=0; j<64; j++) {
        if (next.board[j] == (next.turn | KING)) { king_sq = j; break; }
    }
    
    if (is_attacked(&next, king_sq, s->turn)) {
        strcat(san, (num_responses == 0) ? "#" : "+");
    }
}

void m_to_uci(Move m, char *uci) {
    uci[0] = 'a' + FILE(m.src); uci[1] = '8' - RANK(m.src);
    uci[2] = 'a' + FILE(m.dst); uci[3] = '8' - RANK(m.dst);
    if (m.promote) uci[4] = tolower(" PNBRQK"[m.promote]);
    else uci[4] = '\0';
    uci[5] = '\0';
}

/* --- UI Functions --- */
void clear_screen() {
    printf("\033[2J\033[H");
}

void draw_ui() {
    clear_screen();
    printf("\n  C-Terminal Chess\n\n");
    
    for (int y = 0; y < 8; y++) {
        printf(" %d ", 8 - y);
        for (int x = 0; x < 8; x++) {
            int sq = SQ(x, y);
            bool is_cursor = (x == cursor_x && y == cursor_y);
            bool is_selected = (sq == selected_sq);
            
            // Background Colors
            if (is_cursor) printf("\033[43m"); // Yellow
            else if (is_selected) printf("\033[42m"); // Green
            else if ((x + y) % 2 == 0) printf("\033[47m"); // White/Light
            else printf("\033[44m"); // Blue/Dark
            
            // Piece
            int p = current_state.board[sq];
            if (p == EMPTY) {
                printf("   \033[0m");
            } else {
                int color = p & COLOR_MASK;
                int pt = p & PIECE_MASK;
                const char *pieces[] = {"", "♟", "♞", "♝", "♜", "♛", "♚"};
                
                if (color == WHITE) printf("\033[97m"); // Bright White Text
                else printf("\033[30m"); // Black Text
                
                printf(" %s \033[0m", pieces[pt]);
            }
        }
        printf("\n");
    }
    printf("    a  b  c  d  e  f  g  h\n\n");
    
    printf(" Arrows: Move | Space: Select/Move | U: Undo | Q: Quit\n\n");
    
    // Print PGN
    printf(" --- PGN ---\n");
    for (int i = 0; i < ply; i++) {
        if (i % 2 == 0) printf(" %d. ", (i / 2) + 1);
        printf("%s ", game_history[i].san_move);
        if (i % 2 == 1) printf("\n");
    }
    if (ply % 2 != 0) printf("\n");
    
    // Game Over Check
    Move legals[256];
    int n = generate_legal_moves(&current_state, legals);
    if (n == 0) {
        int king_sq = -1;
        for (int j=0; j<64; j++) if (current_state.board[j] == (current_state.turn | KING)) king_sq = j;
        if (is_attacked(&current_state, king_sq, (current_state.turn == WHITE) ? BLACK : WHITE)) {
            printf("\n CHECKMATE! %s wins.\n", (current_state.turn == WHITE) ? "Black" : "White");
        } else {
            printf("\n STALEMATE!\n");
        }
    }
    
    fflush(stdout);
}

/* --- Engine Interaction --- */
void start_engine() {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) {
        perror("pipe"); exit(1);
    }
    
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[1]); close(out_pipe[0]);
        execlp(engine_path, engine_path, NULL);
        perror("execlp"); exit(1);
    }
    
    close(in_pipe[0]); close(out_pipe[1]);
    eng_in_fd = in_pipe[1];
    eng_out_fd = out_pipe[0];
    
    // Set non-blocking read for engine
    int flags = fcntl(eng_out_fd, F_GETFL, 0);
    fcntl(eng_out_fd, F_SETFL, flags | O_NONBLOCK);
    
    dprintf(eng_in_fd, "uci\n");
    
    // Wait for uciok
    char buf[1024];
    int read_idx = 0;
    while (1) {
        int n = read(eng_out_fd, buf + read_idx, 1);
        if (n > 0) {
            read_idx++;
            buf[read_idx] = '\0';
            if (strstr(buf, "uciok\n")) break;
        }
    }
    
    dprintf(eng_in_fd, "isready\n");
    read_idx = 0;
    while (1) {
        int n = read(eng_out_fd, buf + read_idx, 1);
        if (n > 0) {
            read_idx++;
            buf[read_idx] = '\0';
            if (strstr(buf, "readyok\n")) break;
        }
    }
}

void send_engine_position() {
    dprintf(eng_in_fd, "position startpos moves ");
    for (int i = 0; i < ply; i++) {
        dprintf(eng_in_fd, "%s ", game_history[i].uci_move);
    }
    dprintf(eng_in_fd, "\n");
    
    if (tc_type == 1) dprintf(eng_in_fd, "go movetime %d\n", tc_val);
    else if (tc_type == 2) dprintf(eng_in_fd, "go depth %d\n", tc_val);
    else dprintf(eng_in_fd, "go nodes %d\n", tc_val);
}

void apply_user_move(Move m) {
    game_history[ply].state = current_state;
    build_san(&current_state, m, game_history[ply].san_move);
    m_to_uci(m, game_history[ply].uci_move);
    
    make_move(&current_state, m);
    ply++;
    
    if (current_state.turn == BLACK) { // Engine's turn
        send_engine_position();
    }
}

/* --- Main Loop --- */
int main() {
    printf("Enter Engine PATH (e.g., stockfish): ");
    if (scanf("%255s", engine_path) != 1) return 1;
    
    printf("Time Control Mode (1=movetime ms, 2=depth, 3=nodes): ");
    if (scanf("%d", &tc_type) != 1) return 1;
    
    printf("Time Control Value: ");
    if (scanf("%d", &tc_val) != 1) return 1;
    
    start_engine();
    init_board(&current_state);
    enable_raw_mode();
    
    char eng_buf[4096];
    int eng_buf_len = 0;
    
    draw_ui();
    
    while (is_running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        
        if (current_state.turn == BLACK) {
            FD_SET(eng_out_fd, &readfds);
        }
        
        int max_fd = (eng_out_fd > STDIN_FILENO) ? eng_out_fd : STDIN_FILENO;
        select(max_fd + 1, &readfds, NULL, NULL, NULL);
        
        // Handle User Input
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            char c;
            if (read(STDIN_FILENO, &c, 1) == 1) {
                if (c == 'q' || c == 'Q') is_running = false;
                else if (c == 'u' || c == 'U') {
                    if (ply >= 2 && current_state.turn == WHITE) {
                        ply -= 2;
                        current_state = game_history[ply].state;
                        selected_sq = -1;
                        draw_ui();
                    }
                }
                else if (c == '\033') {
                    char seq[3];
                    if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                        if (seq[0] == '[') {
                            if (seq[1] == 'A' && cursor_y > 0) cursor_y--;
                            else if (seq[1] == 'B' && cursor_y < 7) cursor_y++;
                            else if (seq[1] == 'C' && cursor_x < 7) cursor_x++;
                            else if (seq[1] == 'D' && cursor_x > 0) cursor_x--;
                            draw_ui();
                        }
                    }
                }
                else if (c == ' ' || c == '\n' || c == '\r') {
                    if (current_state.turn == WHITE) {
                        int sq = SQ(cursor_x, cursor_y);
                        if (selected_sq == -1) {
                            if (current_state.board[sq] != EMPTY && (current_state.board[sq] & COLOR_MASK) == WHITE) {
                                selected_sq = sq;
                                draw_ui();
                            }
                        } else {
                            if (sq == selected_sq) {
                                selected_sq = -1; // Unselect
                                draw_ui();
                            } else {
                                // Try move
                                Move legals[256];
                                int n = generate_legal_moves(&current_state, legals);
                                bool valid = false;
                                Move chosen;
                                
                                for (int i=0; i<n; i++) {
                                    if (legals[i].src == selected_sq && legals[i].dst == sq) {
                                        // Default to Queen promotion for simplicity if no specific piece chosen
                                        if (legals[i].promote && legals[i].promote != QUEEN) continue; 
                                        valid = true;
                                        chosen = legals[i];
                                        break;
                                    }
                                }
                                
                                if (valid) {
                                    apply_user_move(chosen);
                                    selected_sq = -1;
                                } else {
                                    selected_sq = -1;
                                }
                                draw_ui();
                            }
                        }
                    }
                }
            }
        }
        
        // Handle Engine Output
        if (current_state.turn == BLACK && FD_ISSET(eng_out_fd, &readfds)) {
            int n = read(eng_out_fd, eng_buf + eng_buf_len, sizeof(eng_buf) - eng_buf_len - 1);
            if (n > 0) {
                eng_buf_len += n;
                eng_buf[eng_buf_len] = '\0';
                
                char *newline;
                while ((newline = strchr(eng_buf, '\n')) != NULL) {
                    *newline = '\0';
                    
                    if (strncmp(eng_buf, "bestmove ", 9) == 0) {
                        char uci_m[6];
                        sscanf(eng_buf + 9, "%5s", uci_m);
                        
                        // Parse UCI back to internal Move
                        Move eng_move;
                        eng_move.src = SQ(uci_m[0]-'a', 7 - (uci_m[1]-'1'));
                        eng_move.dst = SQ(uci_m[2]-'a', 7 - (uci_m[3]-'1'));
                        eng_move.promote = 0;
                        if (uci_m[4]) {
                            char p = uci_m[4];
                            if (p == 'q') eng_move.promote = QUEEN;
                            else if (p == 'r') eng_move.promote = ROOK;
                            else if (p == 'b') eng_move.promote = BISHOP;
                            else if (p == 'n') eng_move.promote = KNIGHT;
                        }
                        
                        // Ensure it's fully applied with SAN
                        game_history[ply].state = current_state;
                        build_san(&current_state, eng_move, game_history[ply].san_move);
                        strcpy(game_history[ply].uci_move, uci_m);
                        make_move(&current_state, eng_move);
                        ply++;
                        
                        draw_ui();
                    }
                    
                    int remaining = eng_buf_len - (newline - eng_buf) - 1;
                    memmove(eng_buf, newline + 1, remaining);
                    eng_buf_len = remaining;
                    eng_buf[eng_buf_len] = '\0';
                }
            }
        }
    }
    
    // Cleanup
    dprintf(eng_in_fd, "quit\n");
    close(eng_in_fd);
    close(eng_out_fd);
    waitpid(engine_pid, NULL, 0);
    
    clear_screen();
    return 0;
}
