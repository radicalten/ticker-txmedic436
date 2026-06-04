#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/select.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>

#define KEY_UP    256
#define KEY_DOWN  257
#define KEY_RIGHT 258
#define KEY_LEFT  259
#define KEY_ENTER 10
#define KEY_SPACE 32

typedef struct {
    int fr, fc;
    int tr, tc;
    char promo;       // 'q', 'r', 'b', 'n' or 0
    int is_castle;    // 1 = King-side, 2 = Queen-side
    int is_ep;        // En Passant flag
} Move;

typedef struct {
    char board[8][8];
    int side;         // 0 = White, 1 = Black
    int castle_wk, castle_wq, castle_bk, castle_bq;
    int ep_r, ep_c;   // En Passant square coordinates (-1 if none)
    int halfmove;
    int fullmove;
    Move last_move;
    char san[16];     // SAN/PGN notation of the move that led to this state
} GameState;

// Configuration settings
enum { TC_TIME, TC_DEPTH, TC_NODES };
int game_mode = 1;    // 0 = Human vs Human, 1 = Human vs Engine (White), 2 = Human vs Engine (Black)
int tc_type = TC_TIME;
int tc_value = 2000;  // Default: 2000 milliseconds
char engine_path[256] = "stockfish";

// Game State History
GameState history[1024];
int history_count = 0;

// Cursor and Interface State
int cur_r = 6, cur_c = 4; // Cursor starts at e2
int sel_r = -1, sel_c = -1;
struct termios orig_termios;

// Engine pipe descriptors
int engine_pid = -1;
int engine_in = -1;
int engine_out = -1;
char engine_buffer[8192];
int engine_buf_pos = 0;

// Function declarations
int get_legal_moves(GameState *state, Move *legal_moves);
int is_square_attacked(GameState *state, int tr, int tc, int attacker_side);
void apply_move(GameState *next, const GameState *prev, Move m);
void draw_screen();
void disableRawMode();
void enableRawMode();

int is_in_bounds(int r, int c) {
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

char get_piece_char(char p) {
    char lower = (p >= 'a') ? p : p + ('a' - 'A');
    if (lower == 'p') return '\0';
    if (lower == 'n') return 'N';
    if (lower == 'b') return 'B';
    if (lower == 'r') return 'R';
    if (lower == 'q') return 'Q';
    if (lower == 'k') return 'K';
    return '\0';
}

int is_in_check(GameState *state, int side) {
    char king_char = (side == 0) ? 'K' : 'k';
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            if (state->board[r][c] == king_char) {
                return is_square_attacked(state, r, c, 1 - side);
            }
        }
    }
    return 0;
}

int is_square_attacked(GameState *state, int tr, int tc, int attacker_side) {
    // Knight attacks
    int k_dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int k_dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    char target_knight = (attacker_side == 0) ? 'N' : 'n';
    for (int i = 0; i < 8; i++) {
        int r = tr + k_dr[i], c = tc + k_dc[i];
        if (is_in_bounds(r, c) && state->board[r][c] == target_knight) return 1;
    }

    // Pawn attacks
    int p_dir = (attacker_side == 0) ? 1 : -1;
    char target_pawn = (attacker_side == 0) ? 'P' : 'p';
    if (is_in_bounds(tr + p_dir, tc - 1) && state->board[tr + p_dir][tc - 1] == target_pawn) return 1;
    if (is_in_bounds(tr + p_dir, tc + 1) && state->board[tr + p_dir][tc + 1] == target_pawn) return 1;

    // King attacks
    int kg_dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int kg_dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    char target_king = (attacker_side == 0) ? 'K' : 'k';
    for (int i = 0; i < 8; i++) {
        int r = tr + kg_dr[i], c = tc + kg_dc[i];
        if (is_in_bounds(r, c) && state->board[r][c] == target_king) return 1;
    }

    // Orthogonal sliding (Rooks/Queens)
    int orth_dr[] = {-1, 1, 0, 0};
    int orth_dc[] = {0, 0, -1, 1};
    for (int i = 0; i < 4; i++) {
        int r = tr + orth_dr[i], c = tc + orth_dc[i];
        while (is_in_bounds(r, c)) {
            char p = state->board[r][c];
            if (p != '.') {
                if (attacker_side == 0 && (p == 'R' || p == 'Q')) return 1;
                if (attacker_side == 1 && (p == 'r' || p == 'q')) return 1;
                break;
            }
            r += orth_dr[i]; c += orth_dc[i];
        }
    }

    // Diagonal sliding (Bishops/Queens)
    int diag_dr[] = {-1, -1, 1, 1};
    int diag_dc[] = {-1, 1, -1, 1};
    for (int i = 0; i < 4; i++) {
        int r = tr + diag_dr[i], c = tc + diag_dc[i];
        while (is_in_bounds(r, c)) {
            char p = state->board[r][c];
            if (p != '.') {
                if (attacker_side == 0 && (p == 'B' || p == 'Q')) return 1;
                if (attacker_side == 1 && (p == 'b' || p == 'q')) return 1;
                break;
            }
            r += diag_dr[i]; c += diag_dc[i];
        }
    }

    return 0;
}

void add_sliding_moves(GameState *state, int r, int c, int dr, int dc, Move *moves, int *count) {
    char piece = state->board[r][c];
    int side = (piece >= 'a') ? 1 : 0;
    int tr = r + dr, tc = c + dc;
    while (is_in_bounds(tr, tc)) {
        char target = state->board[tr][tc];
        if (target == '.') {
            moves[*count] = (Move){r, c, tr, tc, 0, 0, 0};
            (*count)++;
        } else {
            int target_side = (target >= 'a') ? 1 : 0;
            if (target_side != side) {
                moves[*count] = (Move){r, c, tr, tc, 0, 0, 0};
                (*count)++;
            }
            break;
        }
        tr += dr; tc += dc;
    }
}

void generate_pawn_moves(GameState *state, int r, int c, Move *moves, int *count) {
    char piece = state->board[r][c];
    int side = (piece == 'p') ? 1 : 0;
    int dir = (side == 0) ? -1 : 1;
    int start_rank = (side == 0) ? 6 : 1;
    int promo_rank = (side == 0) ? 0 : 7;

    // Single step
    int tr = r + dir, tc = c;
    if (is_in_bounds(tr, tc) && state->board[tr][tc] == '.') {
        if (tr == promo_rank) {
            char p_pieces[] = {'q', 'r', 'b', 'n'};
            for (int i = 0; i < 4; i++) {
                moves[*count] = (Move){r, c, tr, tc, p_pieces[i], 0, 0}; (*count)++;
            }
        } else {
            moves[*count] = (Move){r, c, tr, tc, 0, 0, 0}; (*count)++;
            // Double step
            if (r == start_rank && state->board[r + 2 * dir][tc] == '.') {
                moves[*count] = (Move){r, c, r + 2 * dir, tc, 0, 0, 0}; (*count)++;
            }
        }
    }

    // Captures
    int dc[] = {-1, 1};
    for (int i = 0; i < 2; i++) {
        tc = c + dc[i]; tr = r + dir;
        if (is_in_bounds(tr, tc)) {
            char target = state->board[tr][tc];
            if (target != '.') {
                int target_side = (target >= 'a') ? 1 : 0;
                if (target_side != side) {
                    if (tr == promo_rank) {
                        char p_pieces[] = {'q', 'r', 'b', 'n'};
                        for (int k = 0; k < 4; k++) {
                            moves[*count] = (Move){r, c, tr, tc, p_pieces[k], 0, 0}; (*count)++;
                        }
                    } else {
                        moves[*count] = (Move){r, c, tr, tc, 0, 0, 0}; (*count)++;
                    }
                }
            } else if (state->ep_r == tr && state->ep_c == tc) {
                moves[*count] = (Move){r, c, tr, tc, 0, 0, 1}; (*count)++;
            }
        }
    }
}

void generate_knight_moves(GameState *state, int r, int c, Move *moves, int *count) {
    int dr[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int dc[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    int side = (state->board[r][c] >= 'a') ? 1 : 0;
    for (int i = 0; i < 8; i++) {
        int tr = r + dr[i], tc = c + dc[i];
        if (is_in_bounds(tr, tc)) {
            char target = state->board[tr][tc];
            int target_side = (target >= 'a') ? 1 : 0;
            if (target == '.' || target_side != side) {
                moves[*count] = (Move){r, c, tr, tc, 0, 0, 0}; (*count)++;
            }
        }
    }
}

void generate_king_moves(GameState *state, int r, int c, Move *moves, int *count) {
    int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
    int dc[] = {-1, 0, 1, -1, 1, -1, 0, 1};
    int side = (state->board[r][c] >= 'a') ? 1 : 0;
    for (int i = 0; i < 8; i++) {
        int tr = r + dr[i], tc = c + dc[i];
        if (is_in_bounds(tr, tc)) {
            char target = state->board[tr][tc];
            int target_side = (target >= 'a') ? 1 : 0;
            if (target == '.' || target_side != side) {
                moves[*count] = (Move){r, c, tr, tc, 0, 0, 0}; (*count)++;
            }
        }
    }

    // Castling
    if (side == 0) { // White
        if (state->castle_wk && state->board[7][4] == 'K' && state->board[7][5] == '.' && state->board[7][6] == '.' && state->board[7][7] == 'R') {
            if (!is_square_attacked(state, 7, 4, 1) && !is_square_attacked(state, 7, 5, 1) && !is_square_attacked(state, 7, 6, 1)) {
                moves[*count] = (Move){7, 4, 7, 6, 0, 1, 0}; (*count)++;
            }
        }
        if (state->castle_wq && state->board[7][4] == 'K' && state->board[7][3] == '.' && state->board[7][2] == '.' && state->board[7][1] == '.' && state->board[7][0] == 'R') {
            if (!is_square_attacked(state, 7, 4, 1) && !is_square_attacked(state, 7, 3, 1) && !is_square_attacked(state, 7, 2, 1)) {
                moves[*count] = (Move){7, 4, 7, 2, 0, 2, 0}; (*count)++;
            }
        }
    } else { // Black
        if (state->castle_bk && state->board[0][4] == 'k' && state->board[0][5] == '.' && state->board[0][6] == '.' && state->board[0][7] == 'r') {
            if (!is_square_attacked(state, 0, 4, 0) && !is_square_attacked(state, 0, 5, 0) && !is_square_attacked(state, 0, 6, 0)) {
                moves[*count] = (Move){0, 4, 0, 6, 0, 1, 0}; (*count)++;
            }
        }
        if (state->castle_bq && state->board[0][4] == 'k' && state->board[0][3] == '.' && state->board[0][2] == '.' && state->board[0][1] == '.' && state->board[0][0] == 'r') {
            if (!is_square_attacked(state, 0, 4, 0) && !is_square_attacked(state, 0, 3, 0) && !is_square_attacked(state, 0, 2, 0)) {
                moves[*count] = (Move){0, 4, 0, 2, 0, 2, 0}; (*count)++;
            }
        }
    }
}

int get_legal_moves(GameState *state, Move *legal_moves) {
    Move pseudo[256];
    int p_count = 0;
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            char p = state->board[r][c];
            if (p == '.') continue;
            int piece_side = (p >= 'a') ? 1 : 0;
            if (piece_side != state->side) continue;

            char lower = (p >= 'a') ? p : p + ('a' - 'A');
            if (lower == 'p') generate_pawn_moves(state, r, c, pseudo, &p_count);
            else if (lower == 'n') generate_knight_moves(state, r, c, pseudo, &p_count);
            else if (lower == 'k') generate_king_moves(state, r, c, pseudo, &p_count);
            else if (lower == 'r') {
                add_sliding_moves(state, r, c, -1, 0, pseudo, &p_count);
                add_sliding_moves(state, r, c, 1, 0, pseudo, &p_count);
                add_sliding_moves(state, r, c, 0, -1, pseudo, &p_count);
                add_sliding_moves(state, r, c, 0, 1, pseudo, &p_count);
            } else if (lower == 'b') {
                add_sliding_moves(state, r, c, -1, -1, pseudo, &p_count);
                add_sliding_moves(state, r, c, -1, 1, pseudo, &p_count);
                add_sliding_moves(state, r, c, 1, -1, pseudo, &p_count);
                add_sliding_moves(state, r, c, 1, 1, pseudo, &p_count);
            } else if (lower == 'q') {
                add_sliding_moves(state, r, c, -1, 0, pseudo, &p_count);
                add_sliding_moves(state, r, c, 1, 0, pseudo, &p_count);
                add_sliding_moves(state, r, c, 0, -1, pseudo, &p_count);
                add_sliding_moves(state, r, c, 0, 1, pseudo, &p_count);
                add_sliding_moves(state, r, c, -1, -1, pseudo, &p_count);
                add_sliding_moves(state, r, c, -1, 1, pseudo, &p_count);
                add_sliding_moves(state, r, c, 1, -1, pseudo, &p_count);
                add_sliding_moves(state, r, c, 1, 1, pseudo, &p_count);
            }
        }
    }

    int l_count = 0;
    for (int i = 0; i < p_count; i++) {
        GameState next;
        apply_move(&next, state, pseudo[i]);
        if (!is_in_check(&next, state->side)) {
            legal_moves[l_count++] = pseudo[i];
        }
    }
    return l_count;
}

void apply_move(GameState *next, const GameState *prev, Move m) {
    memcpy(next, prev, sizeof(GameState));
    char p = prev->board[m.fr][m.fc];
    next->board[m.fr][m.fc] = '.';
    next->board[m.tr][m.tc] = p;

    // Handle Promotion
    if (m.promo) {
        char pr = m.promo;
        if (prev->side == 0) { // White
            if (pr >= 'a') pr = pr - 'a' + 'A';
        } else { // Black
            if (pr < 'a') pr = pr - 'A' + 'a';
        }
        next->board[m.tr][m.tc] = pr;
    }

    // Castling Rook execution
    if (m.is_castle == 1) { // King-side
        if (prev->side == 0) { next->board[7][5] = 'R'; next->board[7][7] = '.'; }
        else { next->board[0][5] = 'r'; next->board[0][7] = '.'; }
    } else if (m.is_castle == 2) { // Queen-side
        if (prev->side == 0) { next->board[7][3] = 'R'; next->board[7][0] = '.'; }
        else { next->board[0][3] = 'r'; next->board[0][0] = '.'; }
    }

    // En Passant Capture cleanup
    if (m.is_ep) {
        next->board[prev->ep_r][m.tc] = '.'; 
    }

    // Double step Tracking for Ep
    char lower = (p >= 'a') ? p : p + ('a' - 'A');
    if (lower == 'p' && abs(m.tr - m.fr) == 2) {
        next->ep_r = m.fr + (m.tr - m.fr) / 2;
        next->ep_c = m.fc;
    } else {
        next->ep_r = -1;
        next->ep_c = -1;
    }

    // Castling Rights Updates
    if (p == 'K') { next->castle_wk = 0; next->castle_wq = 0; }
    else if (p == 'k') { next->castle_bk = 0; next->castle_bq = 0; }

    if (m.fr == 7 && m.fc == 7) next->castle_wk = 0;
    if (m.fr == 7 && m.fc == 0) next->castle_wq = 0;
    if (m.fr == 0 && m.fc == 7) next->castle_bk = 0;
    if (m.fr == 0 && m.fc == 0) next->castle_bq = 0;

    if (m.tr == 7 && m.tc == 7) next->castle_wk = 0;
    if (m.tr == 7 && m.tc == 0) next->castle_wq = 0;
    if (m.tr == 0 && m.tc == 7) next->castle_bk = 0;
    if (m.tr == 0 && m.tc == 0) next->castle_bq = 0;

    next->last_move = m;
    next->side = 1 - prev->side;
}

void format_uci_move(Move m, char *out) {
    if (m.promo) {
        sprintf(out, "%c%c%c%c%c", 'a' + m.fc, '8' - m.fr, 'a' + m.tc, '8' - m.tr, m.promo);
    } else {
        sprintf(out, "%c%c%c%c", 'a' + m.fc, '8' - m.fr, 'a' + m.tc, '8' - m.tr);
    }
}

void generate_san(GameState *prev, Move m, char *san_out) {
    if (m.is_castle == 1) {
        strcpy(san_out, "O-O");
        return;
    }
    if (m.is_castle == 2) {
        strcpy(san_out, "O-O-O");
        return;
    }

    char p = prev->board[m.fr][m.fc];
    char p_char = get_piece_char(p);
    int is_capture = (prev->board[m.tr][m.tc] != '.' || m.is_ep);
    
    char disambig[3] = {0};
    if (p_char != '\0') {
        Move all_legal[256];
        int num_legal = get_legal_moves(prev, all_legal);
        int file_conflict = 0, rank_conflict = 0;
        for (int i = 0; i < num_legal; i++) {
            Move o = all_legal[i];
            if (o.tr == m.tr && o.tc == m.tc && (o.fr != m.fr || o.fc != m.fc)) {
                if (prev->board[o.fr][o.fc] == p) {
                    if (o.fc != m.fc) file_conflict = 1;
                    else rank_conflict = 1;
                }
            }
        }
        int d_idx = 0;
        if (file_conflict) disambig[d_idx++] = 'a' + m.fc;
        if (rank_conflict) disambig[d_idx++] = '8' - m.fr;
    }

    char promo_str[4] = {0};
    if (m.promo) {
        sprintf(promo_str, "=%c", m.promo - 'a' + 'A');
    }

    char base[16];
    if (p_char == '\0') { // Pawn
        if (is_capture) {
            sprintf(base, "%cx%c%c%s", 'a' + m.fc, 'a' + m.tc, '8' - m.tr, promo_str);
        } else {
            sprintf(base, "%c%c%s", 'a' + m.tc, '8' - m.tr, promo_str);
        }
    } else {
        sprintf(base, "%c%s%s%c%c", p_char, disambig, is_capture ? "x" : "", 'a' + m.tc, '8' - m.tr);
    }

    GameState temp;
    apply_move(&temp, prev, m);
    int in_check = is_in_check(&temp, temp.side);
    if (in_check) {
        Move sub_legal[256];
        if (get_legal_moves(&temp, sub_legal) == 0) {
            sprintf(san_out, "%s#", base);
        } else {
            sprintf(san_out, "%s+", base);
        }
    } else {
        strcpy(san_out, base);
    }
}

void print_piece(char p) {
    switch (p) {
        case 'P': printf(" ♟ "); break;
        case 'N': printf(" ♞ "); break;
        case 'B': printf(" ♝ "); break;
        case 'R': printf(" ♜ "); break;
        case 'Q': printf(" ♛ "); break;
        case 'K': printf(" ♚ "); break;
        case 'p': printf(" ♟ "); break;
        case 'n': printf(" ♞ "); break;
        case 'b': printf(" ♝ "); break;
        case 'r': printf(" ♜ "); break;
        case 'q': printf(" ♛ "); break;
        case 'k': printf(" ♚ "); break;
        default:  printf("   "); break;
    }
}

int kill_engine() {
    if (engine_pid > 0) {
        kill(engine_pid, SIGKILL);
        close(engine_in);
        close(engine_out);
        engine_pid = -1;
        return 1;
    }
    return 0;
}

int start_engine_connection(const char *path) {
    kill_engine();
    int p_in[2], p_out[2];
    if (pipe(p_in) < 0 || pipe(p_out) < 0) return 0;
    engine_pid = fork();
    if (engine_pid < 0) return 0;
    if (engine_pid == 0) {
        dup2(p_in[0], STDIN_FILENO);
        dup2(p_out[1], STDOUT_FILENO);
        close(p_in[1]); close(p_out[0]);
        execlp(path, path, NULL);
        execl(path, path, NULL);
        exit(1);
    }
    close(p_in[0]); close(p_out[1]);
    engine_in = p_in[1];
    engine_out = p_out[0];
    fcntl(engine_out, F_SETFL, O_NONBLOCK);
    write(engine_in, "uci\nisready\n", 12);
    return 1;
}

int poll_engine_output(char *out_line, int max_len) {
    char tmp[1024];
    int n = read(engine_out, tmp, sizeof(tmp) - 1);
    if (n <= 0) return 0;
    tmp[n] = '\0';
    if (engine_buf_pos + n < (int)sizeof(engine_buffer)) {
        strcpy(engine_buffer + engine_buf_pos, tmp);
        engine_buf_pos += n;
    }
    char *nl = strchr(engine_buffer, '\n');
    if (nl) {
        *nl = '\0';
        strncpy(out_line, engine_buffer, max_len - 1);
        out_line[max_len - 1] = '\0';
        int len = strlen(out_line) + 1;
        memmove(engine_buffer, engine_buffer + len, engine_buf_pos - len + 1);
        engine_buf_pos -= len;
        return 1;
    }
    return 0;
}

void get_engine_move() {
    char position_cmd[8192] = "position startpos moves";
    for (int i = 1; i < history_count; i++) {
        char uci_m[16];
        format_uci_move(history[i].last_move, uci_m);
        strcat(position_cmd, " ");
        strcat(position_cmd, uci_m);
    }
    strcat(position_cmd, "\n");
    write(engine_in, position_cmd, strlen(position_cmd));

    char go_cmd[128];
    if (tc_type == TC_TIME) {
        sprintf(go_cmd, "go movetime %d\n", tc_value);
    } else if (tc_type == TC_DEPTH) {
        sprintf(go_cmd, "go depth %d\n", tc_value);
    } else {
        sprintf(go_cmd, "go nodes %d\n", tc_value);
    }
    write(engine_in, go_cmd, strlen(go_cmd));

    char line[1024];
    int received_move = 0;
    int dots = 0;
    while (!received_move) {
        while (poll_engine_output(line, sizeof(line))) {
            if (strncmp(line, "bestmove ", 9) == 0) {
                char move_str[16];
                sscanf(line, "bestmove %s", move_str);
                Move legal_moves[256];
                int num_moves = get_legal_moves(&history[history_count - 1], legal_moves);
                Move match = {0};
                int found = 0;
                for (int i = 0; i < num_moves; i++) {
                    char uci_m[16];
                    format_uci_move(legal_moves[i], uci_m);
                    if (strcmp(uci_m, move_str) == 0) {
                        match = legal_moves[i];
                        found = 1;
                        break;
                    }
                }
                if (found) {
                    char san[16];
                    generate_san(&history[history_count - 1], match, san);
                    apply_move(&history[history_count], &history[history_count - 1], match);
                    strcpy(history[history_count].san, san);
                    history_count++;
                }
                received_move = 1;
                break;
            }
        }
        if (received_move) break;
        draw_screen();
        printf("\x1b[10;45H\x1b[38;5;11mEngine is thinking");
        for (int d = 0; d < (dots % 4); d++) printf(".");
        printf("   \r");
        fflush(stdout);
        dots++;
        usleep(150000);
    }
}

void disableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enableRawMode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disableRawMode);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int read_key() {
    char c;
    int n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return 0;
    if (c == '\x1b') {
        char seq[3];
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
        int r1 = read(STDIN_FILENO, &seq[0], 1);
        int r2 = read(STDIN_FILENO, &seq[1], 1);
        fcntl(STDIN_FILENO, F_SETFL, flags);
        if (r1 <= 0) return '\x1b';
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return KEY_UP;
                case 'B': return KEY_DOWN;
                case 'C': return KEY_RIGHT;
                case 'D': return KEY_LEFT;
            }
        }
        return '\x1b';
    }
    if (c == '\r') return KEY_ENTER;
    return c;
}

void run_config_menu() {
    disableRawMode();
    printf("\x1b[H\x1b[2J");
    printf("=== CHESS TERMINAL GUI CONFIGURATION ===\n\n");
    printf("1. Set Game Mode\n");
    printf("2. Set Time Control / Limits\n");
    printf("3. Set Engine Executable Path\n");
    printf("4. Cancel\n\n");
    printf("Select option (1-4): ");
    fflush(stdout);
    int opt = 4;
    if (scanf("%d", &opt) != 1) opt = 4;
    int ch; while ((ch = getchar()) != '\n' && ch != EOF); // Flush stdin

    if (opt == 1) {
        printf("\nSelect Game Mode:\n");
        printf("  0: Human vs Human\n");
        printf("  1: Human vs Engine (You play White)\n");
        printf("  2: Human vs Engine (You play Black)\n");
        printf("Mode choice: ");
        fflush(stdout);
        int m;
        if (scanf("%d", &m) == 1 && m >= 0 && m <= 2) {
            game_mode = m;
        }
    } else if (opt == 2) {
        printf("\nSelect Time Control Type:\n");
        printf("  0: Time limit per move (milliseconds)\n");
        printf("  1: Search Depth limit (max plies)\n");
        printf("  2: Node Count limit (max positions searched)\n");
        printf("Type choice: ");
        fflush(stdout);
        int t;
        if (scanf("%d", &t) == 1 && t >= 0 && t <= 2) {
            tc_type = t;
            if (t == 0) printf("Enter time limit per move in ms (e.g. 2000): ");
            else if (t == 1) printf("Enter target depth (e.g. 10): ");
            else if (t == 2) printf("Enter max node count (e.g. 150000): ");
            fflush(stdout);
            int val;
            if (scanf("%d", &val) == 1 && val > 0) {
                tc_value = val;
            }
        }
    } else if (opt == 3) {
        printf("\nEnter engine path (e.g., /opt/homebrew/bin/stockfish or stockfish):\n");
        printf("Path: ");
        fflush(stdout);
        char new_path[256];
        if (fgets(new_path, sizeof(new_path), stdin)) {
            new_path[strcspn(new_path, "\n")] = 0;
            if (strlen(new_path) > 0) {
                strncpy(engine_path, new_path, sizeof(engine_path) - 1);
                start_engine_connection(engine_path);
            }
        }
    }
    enableRawMode();
}

void draw_screen() {
    printf("\x1b[H\x1b[2J"); // Clear Screen
    printf("\x1b[1;36m   CHESS TERMINAL GUI & ENGINE CLIENT\x1b[0m\n\n");

    GameState *state = &history[history_count - 1];

    // Compute legal move targets for selected piece highlighting
    int is_highlighted[8][8];
    memset(is_highlighted, 0, sizeof(is_highlighted));
    if (sel_r != -1 && sel_c != -1) {
        Move legal_moves[256];
        int num_legal = get_legal_moves(state, legal_moves);
        for (int i = 0; i < num_legal; i++) {
            if (legal_moves[i].fr == sel_r && legal_moves[i].fc == sel_c) {
                is_highlighted[legal_moves[i].tr][legal_moves[i].tc] = 1;
            }
        }
    }

    // Print Chessboard with Sidebar Layout
    for (int r = 0; r < 8; r++) {
        printf(" %d ", 8 - r); // Left rank coordinate labels
        for (int c = 0; c < 8; c++) {
            // Background grid coloring (Chesswood palette)
            int is_dark = (r + c) % 2 == 1;
            if (r == cur_r && c == cur_c) {
                printf("\x1b[48;5;198m"); // Cursor highlight (Bright Pink)
            } else if (r == sel_r && c == sel_c) {
                printf("\x1b[48;5;220m"); // Selected piece (Golden Yellow)
            } else if (is_highlighted[r][c]) {
                printf("\x1b[48;5;114m"); // Target highlight (Sage Green)
            } else if (is_dark) {
                printf("\x1b[48;5;95m");  // Dark Square (Mahogany Brown)
            } else {
                printf("\x1b[48;5;180m"); // Light Square (Desert Sand)
            }

            // Piece Text Coloring (Standard styling contrast)
            char p = state->board[r][c];
            if (p != '.') {
                if (p >= 'a') {
                    printf("\x1b[38;5;234m"); // Black pieces: Dark text
                } else {
                    printf("\x1b[38;5;15m");  // White pieces: White text
                }
            }
            print_piece(p);
            printf("\x1b[0m");
        }

        // Draw HUD Sidebar alongside ranks
        switch (r) {
            case 0: printf("   \x1b[1;34m=== INTERFACE STATS ===\x1b[0m"); break;
            case 1: 
                printf("   Active player : %s", (state->side == 0) ? "\x1b[1;15mWhite\x1b[0m" : "\x1b[1;238mBlack\x1b[0m"); 
                break;
            case 2: 
                printf("   Active mode   : ");
                if (game_mode == 0) printf("Human vs Human");
                else if (game_mode == 1) printf("Human vs Engine (Engine plays Black)");
                else printf("Human vs Engine (Engine plays White)");
                break;
            case 3:
                printf("   Limits        : ");
                if (tc_type == TC_TIME) printf("%d ms per move", tc_value);
                else if (tc_type == TC_DEPTH) printf("Depth depth: %d", tc_value);
                else printf("Nodes target: %d", tc_value);
                break;
            case 4:
                printf("   Engine Engine : \x1b[32m%s\x1b[0m", (engine_pid > 0) ? "Active Online" : "Disconnected (H-H only)");
                break;
            case 5: printf("   \x1b[1;34m=== PGN RECORD ===\x1b[0m"); break;
            case 6:
            case 7: {
                int start_m = (history_count > 16) ? (history_count / 2 - 4) * 2 + 1 : 1;
                printf("   ");
                for (int i = start_m; i < history_count; i++) {
                    if (i % 2 == 1) printf("%d. ", (i / 2) + 1);
                    printf("%s ", history[i].san);
                    if (i % 2 == 0) printf(" ");
                }
                break;
            }
        }
        printf("\n");
    }

    printf("     a  b  c  d  e  f  g  h\n\n"); // Files column labels
    printf("\x1b[1;33m[Arrow Keys/WASD]\x1b[0m Move Cursor | \x1b[1;33m[Space/Enter]\x1b[0m Selection | \x1b[1;33m[U]\x1b[0m Undo\n");
    printf("\x1b[1;33m[C]\x1b[0m Config Options  | \x1b[1;33m[Q]\x1b[0m Force Quit Application\n");
    fflush(stdout);
}

char get_human_promotion_choice() {
    printf("\x1b[11;45H\x1b[38;5;11mPromote Pawn to [Q]ueen, [R]ook, [B]ishop, [N]ight? ");
    fflush(stdout);
    while (1) {
        char c = read_key();
        if (c == 'q' || c == 'Q') return 'q';
        if (c == 'r' || c == 'R') return 'r';
        if (c == 'b' || c == 'B') return 'b';
        if (c == 'n' || c == 'N') return 'n';
    }
}

int main() {
    // Set up default initial game state configuration
    history[0] = (GameState){
        .board = {
            {'r', 'n', 'b', 'q', 'k', 'b', 'n', 'r'},
            {'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p'},
            {'.', '.', '.', '.', '.', '.', '.', '.'},
            {'.', '.', '.', '.', '.', '.', '.', '.'},
            {'.', '.', '.', '.', '.', '.', '.', '.'},
            {'.', '.', '.', '.', '.', '.', '.', '.'},
            {'P', 'P', 'P', 'P', 'P', 'P', 'P', 'P'},
            {'R', 'N', 'B', 'Q', 'K', 'B', 'N', 'R'}
        },
        .side = 0,
        .castle_wk = 1, .castle_wq = 1, .castle_bk = 1, .castle_bq = 1,
        .ep_r = -1, .ep_c = -1,
        .halfmove = 0, .fullmove = 1,
        .san = ""
    };
    history_count = 1;

    enableRawMode();
    start_engine_connection(engine_path);

    while (1) {
        GameState *state = &history[history_count - 1];

        // Evaluate terminal checkmate/stalemate game states
        Move legal_moves[256];
        int num_legal = get_legal_moves(state, legal_moves);
        if (num_legal == 0) {
            draw_screen();
            if (is_in_check(state, state->side)) {
                printf("\x1b[11;45H\x1b[1;31mCHECKMATE! %s Wins!\x1b[0m\n", (state->side == 0) ? "Black" : "White");
            } else {
                printf("\x1b[11;45H\x1b[1;33mSTALEMATE! Draw Game.\x1b[0m\n");
            }
            disableRawMode();
            kill_engine();
            return 0;
        }

        // Handle active engine searching routines
        int is_engine_turn = (game_mode == 1 && state->side == 1) || (game_mode == 2 && state->side == 0);
        if (is_engine_turn && engine_pid > 0) {
            get_engine_move();
            continue;
        }

        draw_screen();

        int k = read_key();
        if (k == 'q' || k == 'Q') {
            break;
        }
        if (k == 'c' || k == 'C') {
            run_config_menu();
            continue;
        }
        if (k == 'u' || k == 'U') {
            if (history_count > 1) {
                // If engine playing, take back 2 moves to remain human turn
                if (game_mode != 0 && history_count > 2) {
                    history_count -= 2;
                } else {
                    history_count--;
                }
                sel_r = -1; sel_c = -1;
            }
            continue;
        }

        // Process grid navigation inputs
        if (k == KEY_UP || k == 'w' || k == 'W') { if (cur_r > 0) cur_r--; }
        else if (k == KEY_DOWN || k == 's' || k == 'S') { if (cur_r < 7) cur_r++; }
        else if (k == KEY_LEFT || k == 'a' || k == 'A') { if (cur_c > 0) cur_c--; }
        else if (k == KEY_RIGHT || k == 'd' || k == 'D') { if (cur_c < 7) cur_c++; }
        else if (k == KEY_SPACE || k == KEY_ENTER) {
            if (sel_r == -1) {
                // Selecting source piece
                char target_p = state->board[cur_r][cur_c];
                if (target_p != '.') {
                    int p_side = (target_p >= 'a') ? 1 : 0;
                    if (p_side == state->side) {
                        sel_r = cur_r; sel_c = cur_c;
                    }
                }
            } else {
                // Target destination square chosen
                if (sel_r == cur_r && sel_c == cur_c) {
                    sel_r = -1; sel_c = -1; // Unselect
                } else {
                    // Search if move is within valid legal moveset
                    Move match = {0};
                    int is_valid = 0;
                    for (int i = 0; i < num_legal; i++) {
                        if (legal_moves[i].fr == sel_r && legal_moves[i].fc == sel_c &&
                            legal_moves[i].tr == cur_r && legal_moves[i].tc == cur_c) {
                            match = legal_moves[i];
                            is_valid = 1;
                            break;
                        }
                    }

                    if (is_valid) {
                        // Check if promotion required
                        char lower_p = (state->board[sel_r][sel_c] >= 'a') ? state->board[sel_r][sel_c] : state->board[sel_r][sel_c] + ('a' - 'A');
                        if (lower_p == 'p' && (cur_r == 0 || cur_r == 7)) {
                            match.promo = get_human_promotion_choice();
                        }

                        char san[16];
                        generate_san(state, match, san);
                        apply_move(&history[history_count], state, match);
                        strcpy(history[history_count].san, san);
                        history_count++;

                        sel_r = -1; sel_c = -1;
                    } else {
                        // Re-select if landing on another owned unit
                        char target_p = state->board[cur_r][cur_c];
                        if (target_p != '.') {
                            int p_side = (target_p >= 'a') ? 1 : 0;
                            if (p_side == state->side) {
                                sel_r = cur_r; sel_c = cur_c;
                            }
                        }
                    }
                }
            }
        }
    }

    disableRawMode();
    kill_engine();

    // Print PGN of game upon safe exit
    printf("\n=== Complete PGN Export ===\n");
    for (int i = 1; i < history_count; i++) {
        if (i % 2 == 1) printf("%d. ", (i / 2) + 1);
        printf("%s ", history[i].san);
        if (i % 2 == 0) printf("\n");
    }
    printf("\n\nGoodbye!\n");
    return 0;
}
