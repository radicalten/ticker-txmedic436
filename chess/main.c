#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* --- CHESS LOGIC STRUCTURES --- */
typedef struct {
    int board[64];      /* 0=Empty, 1-6=White (P,N,B,R,Q,K), 7-12=Black (P,N,B,R,Q,K) */
    int turn;           /* 0 = White, 1 = Black */
    int castling;       /* Bitmask: 1=WK, 2=WQ, 4=BK, 8=BQ */
    int ep_square;      /* -1 or square index */
    int halfmove;
    int fullmove;
} GameState;

typedef struct {
    int from;
    int to;
    int promo;          /* 0 if none, else piece index */
} Move;

typedef enum { TC_DEPTH, TC_NODES, TC_TIME } TimeControlMode;

/* --- GLOBAL STATES --- */
GameState current_state;
GameState history[1024];
Move move_history[1024];
char pgn_history[1024][16];
int history_count = 0;

Move last_move = {-1, -1, 0};
int cursor_sq = 12;     /* Starts on e2 */
int selected_sq = -1;   /* -1 if no square is selected */

/* Engine connection settings */
int vs_engine = 1;      /* 1 = VS Engine, 0 = 2-Player Local */
int player_color = 0;   /* Player plays White by default */
char engine_path[256] = "stockfish";
char engine_error[256] = "";
int to_engine[2], from_engine[2];
pid_t engine_pid = -1;
int engine_thinking = 0;
char engine_buffer[16384];
int engine_buf_len = 0;

/* Time Control values */
TimeControlMode tc_mode = TC_DEPTH;
int tc_depth = 10;
int tc_nodes = 100000;
int tc_time = 1000;     /* milliseconds */

/* Unicode pieces: Solid symbols are colored appropriately for maximum contrast */
const char* piece_symbols[] = {
    " ",
    "♟", "♞", "♝", "♜", "♛", "♚", /* White pieces */
    "♟", "♞", "♝", "♜", "♛", "♚"  /* Black pieces */
};

char side_panel[16][128];
struct termios orig_termios;

/* --- TERMINAL RAW MODE FUNCTIONS --- */
void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    printf("\x1b[?25h\x1b[0m\n"); /* Show cursor and reset style */
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
    printf("\x1b[?25l"); /* Hide cursor */
}

/* --- UCI ENGINE PROCESS PIPING --- */
void start_engine() {
    if (pipe(to_engine) < 0 || pipe(from_engine) < 0) {
        strcpy(engine_error, "Failed to create pipes.");
        vs_engine = 0;
        return;
    }
    engine_pid = fork();
    if (engine_pid == 0) {
        dup2(to_engine[0], STDIN_FILENO);
        dup2(from_engine[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDERR_FILENO);
        close(to_engine[0]); close(to_engine[1]);
        close(from_engine[0]); close(from_engine[1]);
        execlp(engine_path, engine_path, NULL);
        exit(1); /* If execlp fails */
    }
    close(to_engine[0]);
    close(from_engine[1]);
    fcntl(from_engine[0], F_SETFL, O_NONBLOCK);
    
    /* Initialize Engine */
    write(to_engine[1], "uci\n", 4);
    write(to_engine[1], "isready\n", 8);
}

void send_to_engine(const char *cmd) {
    if (vs_engine && engine_pid > 0) {
        write(to_engine[1], cmd, strlen(cmd));
    }
}

/* --- CHESS RULE ENGINE (Move Gen & Legality) --- */
void init_game(GameState *state) {
    memset(state, 0, sizeof(GameState));
    int initial_pieces[8] = {4, 2, 3, 5, 6, 3, 2, 4};
    for (int i = 0; i < 8; i++) {
        state->board[i] = initial_pieces[i];
        state->board[i + 8] = 1;
        state->board[i + 48] = 7;
        state->board[i + 56] = initial_pieces[i] + 6;
    }
    state->turn = 0;
    state->castling = 1 | 2 | 4 | 8;
    state->ep_square = -1;
    state->halfmove = 0;
    state->fullmove = 1;
}

int is_square_attacked(const GameState *state, int sq, int attacker_color) {
    int r = sq / 8, c = sq % 8;
    int opp_pawn = (attacker_color == 0) ? 1 : 7;
    int opp_knight = (attacker_color == 0) ? 2 : 8;
    int opp_bishop = (attacker_color == 0) ? 3 : 9;
    int opp_rook = (attacker_color == 0) ? 4 : 10;
    int opp_queen = (attacker_color == 0) ? 5 : 11;
    int opp_king = (attacker_color == 0) ? 6 : 12;

    /* Pawn attacks */
    if (attacker_color == 0) {
        if (r - 1 >= 0) {
            if (c - 1 >= 0 && state->board[(r - 1) * 8 + (c - 1)] == opp_pawn) return 1;
            if (c + 1 < 8 && state->board[(r - 1) * 8 + (c + 1)] == opp_pawn) return 1;
        }
    } else {
        if (r + 1 < 8) {
            if (c - 1 >= 0 && state->board[(r + 1) * 8 + (c - 1)] == opp_pawn) return 1;
            if (c + 1 < 8 && state->board[(r + 1) * 8 + (c + 1)] == opp_pawn) return 1;
        }
    }

    /* Knight attacks */
    int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
    int kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
    for (int i = 0; i < 8; i++) {
        int nr = r + kn_r[i], nc = c + kn_c[i];
        if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
            if (state->board[nr * 8 + nc] == opp_knight) return 1;
        }
    }

    /* Sliding attacks (Rook/Bishop/Queen) */
    int dirs[8][2] = {
        {-1, 0}, {1, 0}, {0, -1}, {0, 1},
        {-1, -1}, {-1, 1}, {1, -1}, {1, 1}
    };
    for (int d = 0; d < 8; d++) {
        int dr = dirs[d][0], dc = dirs[d][1];
        int nr = r, nc = c;
        while (1) {
            nr += dr; nc += dc;
            if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
            int p = state->board[nr * 8 + nc];
            if (p != 0) {
                if (d < 4) {
                    if (p == opp_rook || p == opp_queen) return 1;
                } else {
                    if (p == opp_bishop || p == opp_queen) return 1;
                }
                break;
            }
        }
    }

    /* King attacks */
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            int nr = r + dr, nc = c + dc;
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                if (state->board[nr * 8 + nc] == opp_king) return 1;
            }
        }
    }
    return 0;
}

int find_king(const GameState *state, int color) {
    int king_piece = (color == 0) ? 6 : 12;
    for (int i = 0; i < 64; i++) {
        if (state->board[i] == king_piece) return i;
    }
    return -1;
}

int is_in_check(const GameState *state, int color) {
    int ksq = find_king(state, color);
    if (ksq == -1) return 0;
    return is_square_attacked(state, ksq, 1 - color);
}

int generate_moves_for_square(const GameState *state, int sq, Move *moves) {
    int count = 0;
    int p = state->board[sq];
    if (p == 0) return 0;
    int color = (p <= 6) ? 0 : 1;
    if (color != state->turn) return 0;
    int r = sq / 8, c = sq % 8;

    if (p == 1) { /* White Pawn */
        if (r + 1 < 8 && state->board[(r + 1) * 8 + c] == 0) {
            if (r + 1 == 7) {
                for (int pr = 2; pr <= 5; pr++) moves[count++] = (Move){sq, (r + 1) * 8 + c, pr};
            } else {
                moves[count++] = (Move){sq, (r + 1) * 8 + c, 0};
                if (r == 1 && state->board[(r + 2) * 8 + c] == 0) {
                    moves[count++] = (Move){sq, (r + 2) * 8 + c, 0};
                }
            }
        }
        int caps[2] = {c - 1, c + 1};
        for (int i = 0; i < 2; i++) {
            int nc = caps[i];
            if (nc >= 0 && nc < 8) {
                int target = (r + 1) * 8 + nc;
                int tp = state->board[target];
                if ((tp >= 7) || (target == state->ep_square)) {
                    if (r + 1 == 7) {
                        for (int pr = 2; pr <= 5; pr++) moves[count++] = (Move){sq, target, pr};
                    } else {
                        moves[count++] = (Move){sq, target, 0};
                    }
                }
            }
        }
    } else if (p == 7) { /* Black Pawn */
        if (r - 1 >= 0 && state->board[(r - 1) * 8 + c] == 0) {
            if (r - 1 == 0) {
                for (int pr = 8; pr <= 11; pr++) moves[count++] = (Move){sq, (r - 1) * 8 + c, pr};
            } else {
                moves[count++] = (Move){sq, (r - 1) * 8 + c, 0};
                if (r == 6 && state->board[(r - 2) * 8 + c] == 0) {
                    moves[count++] = (Move){sq, (r - 2) * 8 + c, 0};
                }
            }
        }
        int caps[2] = {c - 1, c + 1};
        for (int i = 0; i < 2; i++) {
            int nc = caps[i];
            if (nc >= 0 && nc < 8) {
                int target = (r - 1) * 8 + nc;
                int tp = state->board[target];
                if ((tp != 0 && tp <= 6) || (target == state->ep_square)) {
                    if (r - 1 == 0) {
                        for (int pr = 8; pr <= 11; pr++) moves[count++] = (Move){sq, target, pr};
                    } else {
                        moves[count++] = (Move){sq, target, 0};
                    }
                }
            }
        }
    } else if (p == 2 || p == 8) { /* Knight */
        int kn_r[] = {-2, -2, -1, -1, 1, 1, 2, 2};
        int kn_c[] = {-1, 1, -2, 2, -2, 2, -1, 1};
        for (int i = 0; i < 8; i++) {
            int nr = r + kn_r[i], nc = c + kn_c[i];
            if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                int target = nr * 8 + nc;
                int tp = state->board[target];
                if (tp == 0 || ((color == 0) ? (tp >= 7) : (tp <= 6))) {
                    moves[count++] = (Move){sq, target, 0};
                }
            }
        }
    } else if (p == 3 || p == 9 || p == 4 || p == 10 || p == 5 || p == 11) { /* Sliders */
        int start_d = (p == 3 || p == 9) ? 4 : 0;
        int end_d = (p == 4 || p == 10) ? 4 : 8;
        int dirs[8][2] = {
            {-1, 0}, {1, 0}, {0, -1}, {0, 1},
            {-1, -1}, {-1, 1}, {1, -1}, {1, 1}
        };
        for (int d = start_d; d < end_d; d++) {
            int dr = dirs[d][0], dc = dirs[d][1];
            int nr = r, nc = c;
            while (1) {
                nr += dr; nc += dc;
                if (nr < 0 || nr >= 8 || nc < 0 || nc >= 8) break;
                int target = nr * 8 + nc;
                int tp = state->board[target];
                if (tp == 0) {
                    moves[count++] = (Move){sq, target, 0};
                } else {
                    if ((color == 0 && tp >= 7) || (color == 1 && tp <= 6)) {
                        moves[count++] = (Move){sq, target, 0};
                    }
                    break;
                }
            }
        }
    } else if (p == 6 || p == 12) { /* King */
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (dr == 0 && dc == 0) continue;
                int nr = r + dr, nc = c + dc;
                if (nr >= 0 && nr < 8 && nc >= 0 && nc < 8) {
                    int target = nr * 8 + nc;
                    int tp = state->board[target];
                    if (tp == 0 || ((color == 0 && tp >= 7) || (color == 1 && tp <= 6))) {
                        moves[count++] = (Move){sq, target, 0};
                    }
                }
            }
        }
        /* Castling */
        if (color == 0) {
            if ((state->castling & 1) && state->board[5] == 0 && state->board[6] == 0) {
                if (!is_square_attacked(state, 4, 1) && !is_square_attacked(state, 5, 1) && !is_square_attacked(state, 6, 1)) {
                    moves[count++] = (Move){4, 6, 0};
                }
            }
            if ((state->castling & 2) && state->board[1] == 0 && state->board[2] == 0 && state->board[3] == 0) {
                if (!is_square_attacked(state, 4, 1) && !is_square_attacked(state, 3, 1) && !is_square_attacked(state, 2, 1)) {
                    moves[count++] = (Move){4, 2, 0};
                }
            }
        } else {
            if ((state->castling & 4) && state->board[61] == 0 && state->board[62] == 0) {
                if (!is_square_attacked(state, 60, 0) && !is_square_attacked(state, 61, 0) && !is_square_attacked(state, 62, 0)) {
                    moves[count++] = (Move){60, 62, 0};
                }
            }
            if ((state->castling & 8) && state->board[57] == 0 && state->board[58] == 0 && state->board[59] == 0) {
                if (!is_square_attacked(state, 60, 0) && !is_square_attacked(state, 59, 0) && !is_square_attacked(state, 58, 0)) {
                    moves[count++] = (Move){60, 58, 0};
                }
            }
        }
    }
    return count;
}

void apply_move(const GameState *prev, GameState *next, Move mv) {
    *next = *prev;
    int p = next->board[mv.from];

    /* Castling updates */
    if (p == 6) next->castling &= ~(1 | 2);
    if (p == 12) next->castling &= ~(4 | 8);
    if (mv.from == 0 || mv.to == 0) next->castling &= ~2;
    if (mv.from == 7 || mv.to == 7) next->castling &= ~1;
    if (mv.from == 56 || mv.to == 56) next->castling &= ~8;
    if (mv.from == 63 || mv.to == 63) next->castling &= ~4;

    /* Execute Castling rook moves */
    if (p == 6 && mv.from == 4) {
        if (mv.to == 6) { next->board[5] = 4; next->board[7] = 0; }
        else if (mv.to == 2) { next->board[3] = 4; next->board[0] = 0; }
    }
    if (p == 12 && mv.from == 60) {
        if (mv.to == 62) { next->board[61] = 10; next->board[63] = 0; }
        else if (mv.to == 58) { next->board[59] = 10; next->board[56] = 0; }
    }

    /* En Passant execution */
    if ((p == 1 || p == 7) && mv.to == prev->ep_square) {
        if (p == 1) next->board[mv.to - 8] = 0;
        else next->board[mv.to + 8] = 0;
    }

    /* Update ep square register */
    next->ep_square = -1;
    if (p == 1 && mv.to - mv.from == 16) next->ep_square = mv.from + 8;
    else if (p == 7 && mv.from - mv.to == 16) next->ep_square = mv.from - 8;

    /* Move the actual piece */
    next->board[mv.to] = p;
    next->board[mv.from] = 0;

    /* Apply Promotion */
    if (mv.promo != 0) {
        next->board[mv.to] = mv.promo;
    }

    next->turn = 1 - prev->turn;
    if (p == 1 || p == 7 || prev->board[mv.to] != 0) next->halfmove = 0;
    else next->halfmove++;
    if (prev->turn == 1) next->fullmove++;
}

int is_move_legal(const GameState *state, Move mv) {
    GameState temp;
    apply_move(state, &temp, mv);
    return !is_in_check(&temp, state->turn);
}

int generate_legal_moves(const GameState *state, Move *moves) {
    int count = 0;
    for (int sq = 0; sq < 64; sq++) {
        if (state->board[sq] != 0) {
            int color = (state->board[sq] <= 6) ? 0 : 1;
            if (color == state->turn) {
                Move pseudo[128];
                int p_count = generate_moves_for_square(state, sq, pseudo);
                for (int i = 0; i < p_count; i++) {
                    if (is_move_legal(state, pseudo[i])) {
                        moves[count++] = pseudo[i];
                    }
                }
            }
        }
    }
    return count;
}

/* --- INTERACTIVE TRANSLATORS & PGN --- */
int parse_uci_move(const char *str, Move *mv) {
    if (strlen(str) < 4) return 0;
    int f_col = str[0] - 'a', f_row = str[1] - '1';
    int t_col = str[2] - 'a', t_row = str[3] - '1';
    if (f_col < 0 || f_col > 7 || f_row < 0 || f_row > 7 || t_col < 0 || t_col > 7 || t_row < 0 || t_row > 7) return 0;
    mv->from = f_row * 8 + f_col;
    mv->to = t_row * 8 + t_col;
    mv->promo = 0;
    if (str[4] != '\0' && str[4] != '\r' && str[4] != '\n' && str[4] != ' ') {
        char p = str[4];
        if (t_row == 7) {
            if (p == 'q') mv->promo = 5; else if (p == 'r') mv->promo = 4;
            else if (p == 'b') mv->promo = 3; else if (p == 'n') mv->promo = 2;
        } else if (t_row == 0) {
            if (p == 'q') mv->promo = 11; else if (p == 'r') mv->promo = 10;
            else if (p == 'b') mv->promo = 9; else if (p == 'n') mv->promo = 8;
        }
    }
    return 1;
}

void sprint_uci_move(char *str, Move mv) {
    sprintf(str, "%c%c%c%c", 'a' + (mv.from % 8), '1' + (mv.from / 8), 'a' + (mv.to % 8), '1' + (mv.to / 8));
    if (mv.promo != 0) {
        char p = 'q';
        int pr = mv.promo;
        if (pr == 5 || pr == 11) p = 'q'; else if (pr == 4 || pr == 10) p = 'r';
        else if (pr == 3 || pr == 9) p = 'b'; else if (pr == 2 || pr == 8) p = 'n';
        sprintf(str + 4, "%c", p);
    }
}

void get_pgn_move_string(const GameState *prev, Move mv, char *pgn) {
    int p = prev->board[mv.from];
    int p_type = (p > 6) ? p - 6 : p;
    if (p_type == 6) {
        if (mv.from == 4 && mv.to == 6) { strcpy(pgn, "O-O"); return; }
        if (mv.from == 4 && mv.to == 2) { strcpy(pgn, "O-O-O"); return; }
        if (mv.from == 60 && mv.to == 62) { strcpy(pgn, "O-O"); return; }
        if (mv.from == 60 && mv.to == 58) { strcpy(pgn, "O-O-O"); return; }
    }
    char piece_chars[] = {'\0', '\0', 'N', 'B', 'R', 'Q', 'K'};
    char p_char = piece_chars[p_type];
    char dest[4]; sprintf(dest, "%c%c", 'a' + (mv.to % 8), '1' + (mv.to / 8));
    int is_cap = (prev->board[mv.to] != 0) || (p_type == 1 && mv.to == prev->ep_square);
    
    char move_str[32] = "";
    if (p_type == 1) {
        if (is_cap) sprintf(move_str, "%cx%s", 'a' + (mv.from % 8), dest);
        else sprintf(move_str, "%s", dest);
        if (mv.promo != 0) {
            int pr = (mv.promo > 6) ? mv.promo - 6 : mv.promo;
            sprintf(move_str + strlen(move_str), "=%c", piece_chars[pr]);
        }
    } else {
        int ambig_col = 0, ambig_row = 0, need_disambig = 0;
        Move legals[256];
        int l_count = generate_legal_moves(prev, legals);
        for (int i = 0; i < l_count; i++) {
            Move m = legals[i];
            if (m.from != mv.from && m.to == mv.to && prev->board[m.from] == p) {
                need_disambig = 1;
                if (m.from % 8 == mv.from % 8) ambig_col = 1;
                if (m.from / 8 == mv.from / 8) ambig_row = 1;
            }
        }
        char disambig[3] = "";
        if (need_disambig) {
            if (!ambig_col) sprintf(disambig, "%c", 'a' + (mv.from % 8));
            else if (!ambig_row) sprintf(disambig, "%c", '1' + (mv.from / 8));
            else sprintf(disambig, "%c%c", 'a' + (mv.from % 8), '1' + (mv.from / 8));
        }
        if (is_cap) sprintf(move_str, "%c%s%sx%s", p_char, disambig, "", dest);
        else sprintf(move_str, "%c%s%s", p_char, disambig, dest);
    }
    GameState post; apply_move(prev, &post, mv);
    if (is_in_check(&post, post.turn)) {
        Move post_legals[256];
        if (generate_legal_moves(&post, post_legals) == 0) strcat(move_str, "#");
        else strcat(move_str, "+");
    }
    strcpy(pgn, move_str);
}

/* --- SIDEBAR PANEL RENDERER --- */
void prepare_side_panel() {
    memset(side_panel, 0, sizeof(side_panel));
    sprintf(side_panel[0], "  \x1b[1;36mCHESS ENGINE TERMINAL GUI\x1b[0m");
    sprintf(side_panel[1], "  =============================");
    sprintf(side_panel[2], "  Active Side: %s", current_state.turn == 0 ? "\x1b[1;37mWHITE\x1b[0m" : "\x1b[1;33mBLACK\x1b[0m");
    
    if (vs_engine) {
        sprintf(side_panel[3], "  Mode:        \x1b[1;32mVS Engine\x1b[0m (Engine: %s)", engine_path);
        sprintf(side_panel[4], "  Status:      %s", engine_thinking ? "\x1b[5;31mThinking...\x1b[0m" : "Idle");
    } else {
        sprintf(side_panel[3], "  Mode:        \x1b[1;35m2-Player (Local)\x1b[0m");
        sprintf(side_panel[4], "  Status:      Active");
    }
    if (strlen(engine_error) > 0) {
        sprintf(side_panel[5], "  \x1b[1;31mError: %s\x1b[0m", engine_error);
    } else {
        sprintf(side_panel[5], "  ");
    }

    sprintf(side_panel[6], "  \x1b[1mTIME CONTROL CONFIGS\x1b[0m");
    sprintf(side_panel[7], "  %s [1] Depth   : %-4d (Current limit)", tc_mode == TC_DEPTH ? " \x1b[1;32m>\x1b[0m" : "  ", tc_depth);
    sprintf(side_panel[8], "  %s [2] Nodes   : %-7d", tc_mode == TC_NODES ? " \x1b[1;32m>\x1b[0m" : "  ", tc_nodes);
    sprintf(side_panel[9], "  %s [3] Movetime: %-4d ms", tc_mode == TC_TIME ? " \x1b[1;32m>\x1b[0m" : "  ", tc_time);
    sprintf(side_panel[10], "  (Use '+' / '-' keys to adjust configuration value)");
    
    sprintf(side_panel[11], "  \x1b[1mCONTROLS KEYBOARD MANUAL\x1b[0m");
    sprintf(side_panel[12], "  - Arrow Keys / WASD / HJKL : Select grid cursor");
    sprintf(side_panel[13], "  - Space / Enter            : Pick piece / Place move");
    sprintf(side_panel[14], "  - 'u' : Undo move          - 't' : Toggle control mode");
    sprintf(side_panel[15], "  - 'm' : Swap engine on/off - 'q' : Terminate chess program");
}

/* --- IN-PLACE BOARD DRAWING --- */
void draw_ui() {
    prepare_side_panel();
    printf("\x1b[H"); /* Reset cursor to top-left (no flashing screen) */
    printf("\n");

    for (int r = 7; r >= 0; r--) {
        for (int sub = 0; sub < 2; sub++) {
            if (sub == 0) printf("  %d ", r + 1);
            else printf("    ");

            for (int c = 0; c < 8; c++) {
                int sq = r * 8 + c;
                int piece = current_state.board[sq];
                int is_dark = ((r + c) % 2 == 0);
                
                /* Colors using cream and olive green palette */
                int bg = is_dark ? 65 : 230;
                if (sq == cursor_sq) bg = 214; /* Orange cursor */
                else if (sq == selected_sq) bg = 220; /* Gold selector */
                else if (last_move.from != -1 && (sq == last_move.from || sq == last_move.to)) {
                    bg = 153; /* Soft blue for last played path */
                }

                int fg = (piece != 0 && piece <= 6) ? 231 : 16; /* High contrast piece colors */
                printf("\x1b[38;5;%dm\x1b[48;5;%dm", fg, bg);

                if (sub == 0) {
                    printf("    ");
                } else {
                    if (piece == 0) printf("    ");
                    else printf(" %s  ", piece_symbols[piece]);
                }
                printf("\x1b[0m");
            }

            /* Draw Side-panel Line by Line */
            int panel_line = (7 - r) * 2 + sub;
            if (panel_line < 16) {
                printf("%s\x1b[K\n", side_panel[panel_line]);
            } else {
                printf("\x1b[K\n");
            }
        }
    }
    printf("      a   b   c   d   e   f   g   h\n\n");

    /* PGN Real-time Output Window */
    printf("  \x1b[1mREAL-TIME PGN MOVES RECORD:\x1b[0m\n  ");
    int print_count = 0;
    for (int i = 0; i < history_count; i += 2) {
        printf("%d. %s ", (i / 2) + 1, pgn_history[i]);
        if (i + 1 < history_count) {
            printf("%s   ", pgn_history[i + 1]);
        }
        print_count++;
        if (print_count % 6 == 0 && (i + 2 < history_count)) printf("\n  ");
    }
    printf("\x1b[K\n");

    /* Check status banner */
    if (is_in_check(&current_state, current_state.turn)) {
        printf("  \x1b[1;31m[CHECK DETECTED]\x1b[K\r");
    } else {
        printf("\x1b[K\r");
    }
    fflush(stdout);
}

/* --- PLAY / ENGINE HANDLER CONTROLS --- */
void trigger_engine_move() {
    if (!vs_engine || current_state.turn == player_color) return;

    char cmd[16384] = "position startpos moves";
    for (int i = 0; i < history_count; i++) {
        char mv_str[10]; sprint_uci_move(mv_str, move_history[i]);
        strcat(cmd, " "); strcat(cmd, mv_str);
    }
    strcat(cmd, "\n");
    send_to_engine(cmd);

    char go_cmd[128];
    if (tc_mode == TC_DEPTH) sprintf(go_cmd, "go depth %d\n", tc_depth);
    else if (tc_mode == TC_NODES) sprintf(go_cmd, "go nodes %d\n", tc_nodes);
    else sprintf(go_cmd, "go movetime %d\n", tc_time);

    send_to_engine(go_cmd);
    engine_thinking = 1;
}

int read_key() {
    char c;
    int nread = read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return 0;
    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) == 0) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) == 0) return '\x1b';
        if (seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 'k'; /* Arrow Up */
                case 'B': return 'j'; /* Arrow Down */
                case 'C': return 'l'; /* Arrow Right */
                case 'D': return 'h'; /* Arrow Left */
            }
        }
        return '\x1b';
    }
    return c;
}

void handle_key(int key) {
    if (key == 0) return;
    int r = cursor_sq / 8, c = cursor_sq % 8;

    if (key == 'k' || key == 'w') r = (r + 1) % 8;
    else if (key == 'j' || key == 's') r = (r + 7) % 8;
    else if (key == 'h' || key == 'a') c = (c + 7) % 8;
    else if (key == 'l' || key == 'd') c = (c + 1) % 8;
    cursor_sq = r * 8 + c;

    if (key == ' ' || key == '\r' || key == '\n') {
        if (selected_sq == -1) {
            int p = current_state.board[cursor_sq];
            if (p != 0) {
                int col = (p <= 6) ? 0 : 1;
                if (col == current_state.turn) {
                    if (!vs_engine || current_state.turn == player_color) {
                        selected_sq = cursor_sq;
                    }
                }
            }
        } else {
            if (cursor_sq == selected_sq) {
                selected_sq = -1;
            } else {
                /* Validate and try executing player move selection */
                Move legals[256];
                int l_count = generate_legal_moves(&current_state, legals);
                int matched_idx = -1;
                for (int i = 0; i < l_count; i++) {
                    if (legals[i].from == selected_sq && legals[i].to == cursor_sq) {
                        if (legals[i].promo != 0) {
                            if (legals[i].promo == 5 || legals[i].promo == 11) { /* Default to Queen promo */
                                matched_idx = i; break;
                            }
                        } else {
                            matched_idx = i; break;
                        }
                    }
                }
                if (matched_idx != -1) {
                    Move mv = legals[matched_idx];
                    char pgn[16]; get_pgn_move_string(&current_state, mv, pgn);
                    strcpy(pgn_history[history_count], pgn);

                    history[history_count] = current_state;
                    move_history[history_count] = mv;
                    history_count++;

                    apply_move(&current_state, &current_state, mv);
                    last_move = mv;
                    selected_sq = -1;

                    if (vs_engine) {
                        trigger_engine_move();
                    }
                } else {
                    /* Change selection if clicking another teammate piece */
                    int target_p = current_state.board[cursor_sq];
                    if (target_p != 0) {
                        int target_color = (target_p <= 6) ? 0 : 1;
                        if (target_color == current_state.turn) {
                            selected_sq = cursor_sq;
                        }
                    }
                }
            }
        }
    } else if (key == 'u' || key == 'U') {
        /* Undo implementation */
        if (!engine_thinking) {
            if (vs_engine) {
                if (history_count >= 2) {
                    history_count -= 2;
                    current_state = history[history_count];
                    last_move = (history_count > 0) ? move_history[history_count - 1] : (Move){-1, -1, 0};
                } else if (history_count == 1) {
                    history_count -= 1;
                    current_state = history[history_count];
                    last_move = (Move){-1, -1, 0};
                }
            } else {
                if (history_count >= 1) {
                    history_count--;
                    current_state = history[history_count];
                    last_move = (history_count > 0) ? move_history[history_count - 1] : (Move){-1, -1, 0};
                }
            }
            selected_sq = -1;
        }
    } else if (key == 't' || key == 'T') {
        tc_mode = (tc_mode + 1) % 3;
    } else if (key == '+' || key == '=') {
        if (tc_mode == TC_DEPTH && tc_depth < 30) tc_depth++;
        else if (tc_mode == TC_NODES && tc_nodes < 10000000) tc_nodes += 10000;
        else if (tc_mode == TC_TIME && tc_time < 60000) tc_time += 100;
    } else if (key == '-' || key == '_') {
        if (tc_mode == TC_DEPTH && tc_depth > 1) tc_depth--;
        else if (tc_mode == TC_NODES && tc_nodes > 10000) tc_nodes -= 10000;
        else if (tc_mode == TC_TIME && tc_time > 100) tc_time -= 100;
    } else if (key == 'm' || key == 'M') {
        if (vs_engine) {
            vs_engine = 0;
            engine_thinking = 0;
        } else {
            vs_engine = 1;
            if (engine_pid <= 0) start_engine();
            if (current_state.turn != player_color) {
                trigger_engine_move();
            }
        }
        selected_sq = -1;
    }
}

/* --- MAIN PROCESS LOOP --- */
int main(int argc, char **argv) {
    if (argc > 1) {
        strncpy(engine_path, argv[1], sizeof(engine_path) - 1);
    }

    init_game(&current_state);
    enable_raw_mode();
    start_engine();

    printf("\x1b[2J"); /* Direct screen clean on start */
    int quit = 0;

    while (!quit) {
        draw_ui();

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        int max_fd = STDIN_FILENO;
        if (vs_engine && engine_pid > 0) {
            FD_SET(from_engine[0], &fds);
            if (from_engine[0] > max_fd) max_fd = from_engine[0];
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 30000; /* Loop wait interval (30ms for responsiveness) */

        int act = select(max_fd + 1, &fds, NULL, NULL, &tv);
        if (act < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &fds)) {
            int key = read_key();
            if (key == 'q' || key == 'Q') {
                quit = 1;
            } else {
                handle_key(key);
            }
        }

        /* Read asynchronous engine moves from the pipe output */
        if (vs_engine && engine_pid > 0 && FD_ISSET(from_engine[0], &fds)) {
            char tmp[1024];
            int n = read(from_engine[0], tmp, sizeof(tmp) - 1);
            if (n > 0) {
                if (engine_buf_len + n < (int)sizeof(engine_buffer)) {
                    memcpy(engine_buffer + engine_buf_len, tmp, n);
                    engine_buf_len += n;
                    engine_buffer[engine_buf_len] = '\0';
                }
            }

            /* Scan engine lines for standard uci triggers */
            char line[2048];
            for (int i = 0; i < engine_buf_len; i++) {
                if (engine_buffer[i] == '\n') {
                    int len = i;
                    if (len > 0 && engine_buffer[len - 1] == '\r') len--;
                    if (len < (int)sizeof(line)) {
                        memcpy(line, engine_buffer, len);
                        line[len] = '\0';
                    }
                    memmove(engine_buffer, engine_buffer + i + 1, engine_buf_len - (i + 1));
                    engine_buf_len -= (i + 1);
                    i = -1;

                    if (strncmp(line, "bestmove", 8) == 0) {
                        char mv_str[16] = "";
                        sscanf(line, "bestmove %s", mv_str);
                        Move mv;
                        if (parse_uci_move(mv_str, &mv)) {
                            Move legals[256];
                            int l_count = generate_legal_moves(&current_state, legals);
                            int is_legal = 0;
                            for (int m = 0; m < l_count; m++) {
                                if (legals[m].from == mv.from && legals[m].to == mv.to) {
                                    mv = legals[m];
                                    is_legal = 1; break;
                                }
                            }
                            if (is_legal) {
                                char pgn[16]; get_pgn_move_string(&current_state, mv, pgn);
                                strcpy(pgn_history[history_count], pgn);

                                history[history_count] = current_state;
                                move_history[history_count] = mv;
                                history_count++;

                                apply_move(&current_state, &current_state, mv);
                                last_move = mv;
                            }
                        }
                        engine_thinking = 0;
                    }
                }
            }
        }

        /* Verify active status of child process */
        if (vs_engine && engine_pid > 0) {
            int status;
            if (waitpid(engine_pid, &status, WNOHANG) > 0) {
                vs_engine = 0;
                engine_pid = -1;
                strcpy(engine_error, "Engine exited. Changed to 2-Player.");
            }
        }

        /* Request Engine evaluation if it's the computer's turn */
        if (vs_engine && !engine_thinking && current_state.turn != player_color) {
            Move legals[256];
            if (generate_legal_moves(&current_state, legals) > 0) {
                trigger_engine_move();
            }
        }
    }

    if (engine_pid > 0) {
        //kill(engine_pid, SIGTERM);
        waitpid(engine_pid, NULL, 0);
    }
    return 0;
}
