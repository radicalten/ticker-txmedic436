#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define WHITE  1
#define BLACK -1

#define WK 1
#define WQ 2
#define BK 4
#define BQ 8

#define MV_CAPTURE 1
#define MV_EP      2
#define MV_CASTLE  4
#define MV_DBL     8
#define MV_PROMO   16

#define MAX_MOVES   256
#define MAX_HISTORY 2048
#define MAX_LINE    4096
#define MAX_ENGINE_ARGV 32

#define BG_LIGHT   223
#define BG_DARK    137
#define BG_LAST     45
#define BG_SELECTED 27
#define BG_TARGET   34
#define BG_CAPTURE 178
#define BG_CHECK   160

typedef enum { CTRL_DEPTH = 0, CTRL_NODES = 1, CTRL_TIME = 2 } ControlMode;

typedef struct {
    char b[64];
    int side;
    int castling;
    int ep;
    int halfmove;
    int fullmove;
} Position;

typedef struct {
    int from, to;
    int promo;   // uppercase piece letter: Q R B N, or 0
    int flags;
} Move;

typedef struct {
    Position pos_before;
    Move move;
    char san[32];
    char pgn[48];
} Hist;

typedef struct {
    Position pos;

    Move legal[MAX_MOVES];
    int legal_count;

    Hist hist[MAX_HISTORY];
    int hist_count;

    int cursor;
    int selected;

    int game_over;
    int in_check;
    int check_sq;
    int last_from;
    int last_to;

    int promo_mode;
    Move promo_choices[4];
    int promo_count;
    int promo_index;

    ControlMode ctrl_mode;
    int depth_limit;
    int nodes_limit;
    int time_limit; // milliseconds

    char notice[256];

    int engine_enabled;
    int engine_side; // WHITE / BLACK
    char *engine_cmd_raw;
    char *engine_argv[MAX_ENGINE_ARGV];
    int engine_argc;

    pid_t engine_pid;
    int engine_in;
    int engine_out;

    int engine_searching;
    int engine_cancelled;
    int uci_ok;
    int ready_ok;
    char engine_line[MAX_LINE];
    int engine_line_len;

    int terminal_active;
} App;

static App *g_app = NULL;
static volatile sig_atomic_t g_quit = 0;
static struct termios g_saved_termios;

static void cleanup_all(void);

static void on_signal(int sig) {
    (void)sig;
    g_quit = 1;
}

static long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
}

static int sq(int file, int rank) { return rank * 8 + file; }
static int file_of(int s) { return s & 7; }
static int rank_of(int s) { return s >> 3; }
static int on_board(int file, int rank) { return file >= 0 && file < 8 && rank >= 0 && rank < 8; }

static int piece_color(char p) {
    if (p == '.') return 0;
    return isupper((unsigned char)p) ? WHITE : BLACK;
}

static char piece_type(char p) {
    return (char)toupper((unsigned char)p);
}

static const char *side_name(int side) {
    return side == WHITE ? "White" : "Black";
}

static const char *control_name(ControlMode m) {
    switch (m) {
        case CTRL_DEPTH: return "depth";
        case CTRL_NODES: return "nodes";
        case CTRL_TIME:  return "time";
        default: return "?";
    }
}

static const char *piece_glyph(char p) {
    switch (p) {
        case 'P': return "♙";
        case 'N': return "♘";
        case 'B': return "♗";
        case 'R': return "♖";
        case 'Q': return "♕";
        case 'K': return "♔";
        case 'p': return "♟";
        case 'n': return "♞";
        case 'b': return "♝";
        case 'r': return "♜";
        case 'q': return "♛";
        case 'k': return "♚";
        default:  return " ";
    }
}

static int sq_from_alg(const char *s) {
    if (!s || strlen(s) < 2) return -1;
    if (s[0] < 'a' || s[0] > 'h') return -1;
    if (s[1] < '1' || s[1] > '8') return -1;
    return sq(s[0] - 'a', 8 - (s[1] - '0'));
}

static void sq_to_alg(int s, char out[3]) {
    out[0] = (char)('a' + file_of(s));
    out[1] = (char)('8' - rank_of(s));
    out[2] = '\0';
}

static void set_notice(App *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->notice, sizeof a->notice, fmt, ap);
    va_end(ap);
}

static void clear_notice(App *a) {
    a->notice[0] = '\0';
}

static void append_char(char *buf, size_t *len, size_t max, char c) {
    if (*len + 1 < max) {
        buf[(*len)++] = c;
        buf[*len] = '\0';
    }
}

static void append_str(char *buf, size_t *len, size_t max, const char *s) {
    while (*s && *len + 1 < max) {
        buf[(*len)++] = *s++;
        buf[*len] = '\0';
    }
}

static int find_king(const Position *p, int side) {
    char k = (side == WHITE) ? 'K' : 'k';
    for (int i = 0; i < 64; i++) {
        if (p->b[i] == k) return i;
    }
    return -1;
}

static int square_attacked(const Position *p, int target, int byside) {
    int tf = file_of(target);
    int tr = rank_of(target);

    if (byside == WHITE) {
        int r = tr + 1;
        if (r < 8) {
            if (tf > 0 && p->b[sq(tf - 1, r)] == 'P') return 1;
            if (tf < 7 && p->b[sq(tf + 1, r)] == 'P') return 1;
        }
    } else {
        int r = tr - 1;
        if (r >= 0) {
            if (tf > 0 && p->b[sq(tf - 1, r)] == 'p') return 1;
            if (tf < 7 && p->b[sq(tf + 1, r)] == 'p') return 1;
        }
    }

    static const int knight_delta[8][2] = {
        {1, 2}, {2, 1}, {2, -1}, {1, -2},
        {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}
    };
    for (int i = 0; i < 8; i++) {
        int f = tf + knight_delta[i][0];
        int r = tr + knight_delta[i][1];
        if (!on_board(f, r)) continue;
        char pc = p->b[sq(f, r)];
        if (byside == WHITE && pc == 'N') return 1;
        if (byside == BLACK && pc == 'n') return 1;
    }

    static const int bishop_delta[4][2] = {
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
    };
    for (int d = 0; d < 4; d++) {
        int f = tf + bishop_delta[d][0];
        int r = tr + bishop_delta[d][1];
        while (on_board(f, r)) {
            char pc = p->b[sq(f, r)];
            if (pc != '.') {
                if (byside == WHITE && (pc == 'B' || pc == 'Q')) return 1;
                if (byside == BLACK && (pc == 'b' || pc == 'q')) return 1;
                break;
            }
            f += bishop_delta[d][0];
            r += bishop_delta[d][1];
        }
    }

    static const int rook_delta[4][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1}
    };
    for (int d = 0; d < 4; d++) {
        int f = tf + rook_delta[d][0];
        int r = tr + rook_delta[d][1];
        while (on_board(f, r)) {
            char pc = p->b[sq(f, r)];
            if (pc != '.') {
                if (byside == WHITE && (pc == 'R' || pc == 'Q')) return 1;
                if (byside == BLACK && (pc == 'r' || pc == 'q')) return 1;
                break;
            }
            f += rook_delta[d][0];
            r += rook_delta[d][1];
        }
    }

    for (int df = -1; df <= 1; df++) {
        for (int dr = -1; dr <= 1; dr++) {
            if (df == 0 && dr == 0) continue;
            int f = tf + df;
            int r = tr + dr;
            if (!on_board(f, r)) continue;
            char pc = p->b[sq(f, r)];
            if (byside == WHITE && pc == 'K') return 1;
            if (byside == BLACK && pc == 'k') return 1;
        }
    }

    return 0;
}

static int is_in_check(const Position *p, int side) {
    int ksq = find_king(p, side);
    if (ksq < 0) return 0;
    return square_attacked(p, ksq, -side);
}

static void clear_castling_rights_for_rook_square(Position *p, int square, char rook_piece) {
    if (rook_piece == 'R') {
        if (square == sq(0, 7)) p->castling &= ~WQ;
        if (square == sq(7, 7)) p->castling &= ~WK;
    } else if (rook_piece == 'r') {
        if (square == sq(0, 0)) p->castling &= ~BQ;
        if (square == sq(7, 0)) p->castling &= ~BK;
    }
}

static void push_move(Move *m, int *count, int max, int from, int to, int promo, int flags) {
    if (*count >= max) return;
    m[*count].from = from;
    m[*count].to = to;
    m[*count].promo = promo;
    m[*count].flags = flags;
    (*count)++;
}

static int generate_pseudo_moves(const Position *p, Move *out, int max) {
    int n = 0;
    int side = p->side;

    for (int from = 0; from < 64; from++) {
        char pc = p->b[from];
        if (pc == '.' || piece_color(pc) != side) continue;

        int f = file_of(from);
        int r = rank_of(from);
        char t = piece_type(pc);

        if (t == 'P') {
            int dir = (side == WHITE) ? -1 : 1;
            int start_rank = (side == WHITE) ? 6 : 1;
            int promo_rank = (side == WHITE) ? 0 : 7;

            int nr = r + dir;
            if (on_board(f, nr) && p->b[sq(f, nr)] == '.') {
                int to = sq(f, nr);
                if (nr == promo_rank) {
                    push_move(out, &n, max, from, to, 'Q', MV_PROMO);
                    push_move(out, &n, max, from, to, 'R', MV_PROMO);
                    push_move(out, &n, max, from, to, 'B', MV_PROMO);
                    push_move(out, &n, max, from, to, 'N', MV_PROMO);
                } else {
                    push_move(out, &n, max, from, to, 0, 0);
                }

                if (r == start_rank) {
                    int nr2 = r + 2 * dir;
                    if (on_board(f, nr2) && p->b[sq(f, nr2)] == '.') {
                        push_move(out, &n, max, from, sq(f, nr2), 0, MV_DBL);
                    }
                }
            }

            for (int df = -1; df <= 1; df += 2) {
                int nf = f + df;
                int nr2 = r + dir;
                if (!on_board(nf, nr2)) continue;
                int to = sq(nf, nr2);
                char target = p->b[to];
                if (target != '.' && piece_color(target) == -side) {
                    if (nr2 == promo_rank) {
                        push_move(out, &n, max, from, to, 'Q', MV_CAPTURE | MV_PROMO);
                        push_move(out, &n, max, from, to, 'R', MV_CAPTURE | MV_PROMO);
                        push_move(out, &n, max, from, to, 'B', MV_CAPTURE | MV_PROMO);
                        push_move(out, &n, max, from, to, 'N', MV_CAPTURE | MV_PROMO);
                    } else {
                        push_move(out, &n, max, from, to, 0, MV_CAPTURE);
                    }
                } else if (to == p->ep) {
                    push_move(out, &n, max, from, to, 0, MV_CAPTURE | MV_EP);
                }
            }
        } else if (t == 'N') {
            static const int d[8][2] = {
                {1, 2}, {2, 1}, {2, -1}, {1, -2},
                {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}
            };
            for (int i = 0; i < 8; i++) {
                int nf = f + d[i][0];
                int nr = r + d[i][1];
                if (!on_board(nf, nr)) continue;
                int to = sq(nf, nr);
                char target = p->b[to];
                if (target == '.') push_move(out, &n, max, from, to, 0, 0);
                else if (piece_color(target) == -side) push_move(out, &n, max, from, to, 0, MV_CAPTURE);
            }
        } else if (t == 'B' || t == 'R' || t == 'Q') {
            static const int bd[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
            static const int rd[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
            const int (*dirs)[2] = NULL;
            int dc = 0;

            if (t == 'B') { dirs = bd; dc = 4; }
            else if (t == 'R') { dirs = rd; dc = 4; }
            else { 
                static const int qd[8][2] = {{1,1},{1,-1},{-1,1},{-1,-1},{1,0},{-1,0},{0,1},{0,-1}};
                dirs = qd; dc = 8;
            }

            for (int i = 0; i < dc; i++) {
                int nf = f + dirs[i][0];
                int nr = r + dirs[i][1];
                while (on_board(nf, nr)) {
                    int to = sq(nf, nr);
                    char target = p->b[to];
                    if (target == '.') {
                        push_move(out, &n, max, from, to, 0, 0);
                    } else {
                        if (piece_color(target) == -side) push_move(out, &n, max, from, to, 0, MV_CAPTURE);
                        break;
                    }
                    nf += dirs[i][0];
                    nr += dirs[i][1];
                }
            }
        } else if (t == 'K') {
            for (int df = -1; df <= 1; df++) {
                for (int dr = -1; dr <= 1; dr++) {
                    if (df == 0 && dr == 0) continue;
                    int nf = f + df;
                    int nr = r + dr;
                    if (!on_board(nf, nr)) continue;
                    int to = sq(nf, nr);
                    char target = p->b[to];
                    if (target == '.') push_move(out, &n, max, from, to, 0, 0);
                    else if (piece_color(target) == -side) push_move(out, &n, max, from, to, 0, MV_CAPTURE);
                }
            }

            // Castling
            if (side == WHITE && from == sq(4,7) && p->b[sq(4,7)] == 'K') {
                if ((p->castling & WK) &&
                    p->b[sq(5,7)] == '.' && p->b[sq(6,7)] == '.' &&
                    p->b[sq(7,7)] == 'R' &&
                    !square_attacked(p, sq(4,7), BLACK) &&
                    !square_attacked(p, sq(5,7), BLACK) &&
                    !square_attacked(p, sq(6,7), BLACK)) {
                    push_move(out, &n, max, from, sq(6,7), 0, MV_CASTLE);
                }
                if ((p->castling & WQ) &&
                    p->b[sq(1,7)] == '.' && p->b[sq(2,7)] == '.' && p->b[sq(3,7)] == '.' &&
                    p->b[sq(0,7)] == 'R' &&
                    !square_attacked(p, sq(4,7), BLACK) &&
                    !square_attacked(p, sq(3,7), BLACK) &&
                    !square_attacked(p, sq(2,7), BLACK)) {
                    push_move(out, &n, max, from, sq(2,7), 0, MV_CASTLE);
                }
            } else if (side == BLACK && from == sq(4,0) && p->b[sq(4,0)] == 'k') {
                if ((p->castling & BK) &&
                    p->b[sq(5,0)] == '.' && p->b[sq(6,0)] == '.' &&
                    p->b[sq(7,0)] == 'r' &&
                    !square_attacked(p, sq(4,0), WHITE) &&
                    !square_attacked(p, sq(5,0), WHITE) &&
                    !square_attacked(p, sq(6,0), WHITE)) {
                    push_move(out, &n, max, from, sq(6,0), 0, MV_CASTLE);
                }
                if ((p->castling & BQ) &&
                    p->b[sq(1,0)] == '.' && p->b[sq(2,0)] == '.' && p->b[sq(3,0)] == '.' &&
                    p->b[sq(0,0)] == 'r' &&
                    !square_attacked(p, sq(4,0), WHITE) &&
                    !square_attacked(p, sq(3,0), WHITE) &&
                    !square_attacked(p, sq(2,0), WHITE)) {
                    push_move(out, &n, max, from, sq(2,0), 0, MV_CASTLE);
                }
            }
        }
    }

    return n;
}

static void make_move(Position *p, Move m) {
    char moving = p->b[m.from];
    int side = piece_color(moving);
    char captured = (m.flags & MV_EP) ? (side == WHITE ? 'p' : 'P') : p->b[m.to];

    if (piece_type(moving) == 'P' || captured != '.') p->halfmove = 0;
    else p->halfmove++;

    if (piece_type(moving) == 'K') {
        if (side == WHITE) p->castling &= ~(WK | WQ);
        else p->castling &= ~(BK | BQ);
    }

    clear_castling_rights_for_rook_square(p, m.from, moving);
    if (captured == 'R' || captured == 'r') clear_castling_rights_for_rook_square(p, m.to, captured);

    p->b[m.from] = '.';

    if (m.flags & MV_EP) {
        int cap_sq = m.to + ((side == WHITE) ? 8 : -8);
        p->b[cap_sq] = '.';
    }

    if (m.flags & MV_CASTLE) {
        if (side == WHITE) {
            if (m.to == sq(6, 7)) {
                p->b[sq(7, 7)] = '.';
                p->b[sq(5, 7)] = 'R';
            } else {
                p->b[sq(0, 7)] = '.';
                p->b[sq(3, 7)] = 'R';
            }
        } else {
            if (m.to == sq(6, 0)) {
                p->b[sq(7, 0)] = '.';
                p->b[sq(5, 0)] = 'r';
            } else {
                p->b[sq(0, 0)] = '.';
                p->b[sq(3, 0)] = 'r';
            }
        }
    }

    if (m.flags & MV_PROMO) {
        p->b[m.to] = (side == WHITE) ? (char)m.promo : (char)tolower((unsigned char)m.promo);
    } else {
        p->b[m.to] = moving;
    }

    p->ep = -1;
    if (m.flags & MV_DBL) {
        p->ep = m.to + ((side == WHITE) ? 8 : -8);
    }

    if (side == BLACK) p->fullmove++;
    p->side = -side;
}

static int generate_legal_moves(const Position *p, Move *out, int max) {
    Move pseudo[MAX_MOVES];
    int pn = generate_pseudo_moves(p, pseudo, MAX_MOVES);
    int n = 0;

    for (int i = 0; i < pn; i++) {
        Position q = *p;
        make_move(&q, pseudo[i]);
        if (!is_in_check(&q, -q.side)) {
            if (n < max) out[n++] = pseudo[i];
        }
    }

    return n;
}

static void position_to_fen(const Position *p, char *out, size_t outsz) {
    char *o = out;
    size_t rem = outsz;
    out[0] = '\0';

    for (int r = 0; r < 8; r++) {
        int empty = 0;
        for (int f = 0; f < 8; f++) {
            char pc = p->b[sq(f, r)];
            if (pc == '.') {
                empty++;
            } else {
                if (empty) {
                    if (rem > 1) {
                        *o++ = (char)('0' + empty);
                        *o = '\0';
                        rem--;
                    }
                    empty = 0;
                }
                if (rem > 1) {
                    *o++ = pc;
                    *o = '\0';
                    rem--;
                }
            }
        }
        if (empty) {
            if (rem > 1) {
                *o++ = (char)('0' + empty);
                *o = '\0';
                rem--;
            }
        }
        if (r != 7 && rem > 1) {
            *o++ = '/';
            *o = '\0';
            rem--;
        }
    }

    if (rem > 1) { *o++ = ' '; *o = '\0'; rem--; }
    if (rem > 1) { *o++ = (p->side == WHITE ? 'w' : 'b'); *o = '\0'; rem--; }
    if (rem > 1) { *o++ = ' '; *o = '\0'; rem--; }

    char castle[8];
    int idx = 0;
    if (p->castling & WK) castle[idx++] = 'K';
    if (p->castling & WQ) castle[idx++] = 'Q';
    if (p->castling & BK) castle[idx++] = 'k';
    if (p->castling & BQ) castle[idx++] = 'q';
    if (idx == 0) castle[idx++] = '-';
    castle[idx] = '\0';

    for (int i = 0; castle[i] && rem > 1; i++) {
        *o++ = castle[i];
        *o = '\0';
        rem--;
    }

    if (rem > 1) { *o++ = ' '; *o = '\0'; rem--; }

    if (p->ep >= 0) {
        char ep[3];
        sq_to_alg(p->ep, ep);
        for (int i = 0; ep[i] && rem > 1; i++) {
            *o++ = ep[i];
            *o = '\0';
            rem--;
        }
    } else {
        if (rem > 1) { *o++ = '-'; *o = '\0'; rem--; }
    }

    char tmp[64];
    snprintf(tmp, sizeof tmp, " %d %d", p->halfmove, p->fullmove);
    append_str(out, &(size_t){strlen(out)}, outsz, "");
    strncat(out, tmp, outsz - strlen(out) - 1);
}

static void build_san(const Position *before, const Move *legal_moves, int legal_count, Move m, char *out, size_t outsz) {
    char base[32];
    base[0] = '\0';

    char moving = before->b[m.from];
    int side = piece_color(moving);
    int capture = (m.flags & (MV_CAPTURE | MV_EP)) != 0;

    if (m.flags & MV_CASTLE) {
        snprintf(base, sizeof base, "%s", (m.to > m.from) ? "O-O" : "O-O-O");
    } else {
        size_t len = 0;
        if (piece_type(moving) != 'P') {
            append_char(base, &len, sizeof base, piece_type(moving));

            int other = 0, same_file = 0, same_rank = 0;
            for (int i = 0; i < legal_count; i++) {
                Move x = legal_moves[i];
                if (x.from == m.from || x.to != m.to) continue;
                if (piece_color(before->b[x.from]) != side) continue;
                if (piece_type(before->b[x.from]) != piece_type(moving)) continue;
                other = 1;
                if (file_of(x.from) == file_of(m.from)) same_file = 1;
                if (rank_of(x.from) == rank_of(m.from)) same_rank = 1;
            }
            if (other) {
                if (same_file && same_rank) {
                    append_char(base, &len, sizeof base, (char)('a' + file_of(m.from)));
                    append_char(base, &len, sizeof base, (char)('8' - rank_of(m.from)));
                } else if (same_file) {
                    append_char(base, &len, sizeof base, (char)('8' - rank_of(m.from)));
                } else {
                    append_char(base, &len, sizeof base, (char)('a' + file_of(m.from)));
                }
            }
            if (capture) append_char(base, &len, sizeof base, 'x');
        } else {
            if (capture) {
                append_char(base, &len, sizeof base, (char)('a' + file_of(m.from)));
                append_char(base, &len, sizeof base, 'x');
            }
        }

        char dst[3];
        sq_to_alg(m.to, dst);
        append_str(base, &len, sizeof base, dst);

        if (m.flags & MV_PROMO) {
            append_char(base, &len, sizeof base, '=');
            append_char(base, &len, sizeof base, (char)toupper((unsigned char)m.promo));
        }
    }

    Position after = *before;
    make_move(&after, m);
    int give_check = is_in_check(&after, after.side);
    int mate = 0;
    if (give_check) {
        Move tmp[MAX_MOVES];
        int n = generate_legal_moves(&after, tmp, MAX_MOVES);
        mate = (n == 0);
    }

    if (mate) snprintf(out, outsz, "%s#", base);
    else if (give_check) snprintf(out, outsz, "%s+", base);
    else snprintf(out, outsz, "%s", base);
}

static int tokenize_engine_cmd(App *a) {
    if (!a->engine_cmd_raw) return 0;
    int argc = 0;
    char *p = a->engine_cmd_raw;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (argc >= MAX_ENGINE_ARGV - 1) break;
        a->engine_argv[argc++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) {
            *p = '\0';
            p++;
        }
    }
    a->engine_argv[argc] = NULL;
    a->engine_argc = argc;
    return argc;
}

static int write_full(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += w;
        n -= (size_t)w;
    }
    return 0;
}

static int engine_sendf(App *a, const char *fmt, ...) {
    if (a->engine_in < 0) return -1;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    if (write_full(a->engine_in, buf, (size_t)n) < 0) {
        set_notice(a, "Engine write failed");
        a->engine_enabled = 0;
        return -1;
    }
    return 0;
}

static int spawn_engine(App *a) {
    if (!a->engine_enabled || a->engine_argc == 0) return -1;

    int pin[2], pout[2];
    if (pipe(pin) < 0) return -1;
    if (pipe(pout) < 0) {
        close(pin[0]); close(pin[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pin[0]); close(pin[1]);
        close(pout[0]); close(pout[1]);
        return -1;
    }

    if (pid == 0) {
        dup2(pin[0], STDIN_FILENO);
        dup2(pout[1], STDOUT_FILENO);

        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            dup2(dn, STDERR_FILENO);
            if (dn > STDERR_FILENO) close(dn);
        }

        close(pin[0]); close(pin[1]);
        close(pout[0]); close(pout[1]);

        execvp(a->engine_argv[0], a->engine_argv);
        _exit(127);
    }

    close(pin[0]);
    close(pout[1]);

    a->engine_pid = pid;
    a->engine_in = pin[1];
    a->engine_out = pout[0];
    a->engine_searching = 0;
    a->engine_cancelled = 0;
    a->uci_ok = 0;
    a->ready_ok = 0;
    a->engine_line_len = 0;

    int flags = fcntl(a->engine_out, F_GETFL, 0);
    if (flags >= 0) fcntl(a->engine_out, F_SETFL, flags | O_NONBLOCK);

    return 0;
}

static void cleanup_engine(App *a) {
    if (!a) return;

    if (a->engine_in >= 0) {
        write_full(a->engine_in, "quit\n", 5);
        close(a->engine_in);
        a->engine_in = -1;
    }

    if (a->engine_out >= 0) {
        close(a->engine_out);
        a->engine_out = -1;
    }

    if (a->engine_pid > 0) {
        int st = 0;
        pid_t r = waitpid(a->engine_pid, &st, WNOHANG);
        if (r == 0) {
            kill(a->engine_pid, SIGTERM);
            for (int i = 0; i < 20; i++) {
                usleep(50000);
                r = waitpid(a->engine_pid, &st, WNOHANG);
                if (r != 0) break;
            }
            if (r == 0) {
                kill(a->engine_pid, SIGKILL);
                waitpid(a->engine_pid, &st, 0);
            }
        }
        a->engine_pid = 0;
    }
}

static void engine_process_line(App *a, const char *line) {
    if (strcmp(line, "uciok") == 0) {
        a->uci_ok = 1;
        return;
    }
    if (strcmp(line, "readyok") == 0) {
        a->ready_ok = 1;
        return;
    }
    if (strncmp(line, "bestmove ", 9) == 0) {
        if (!a->engine_searching) return;

        char mv[16];
        const char *p = line + 9;
        int i = 0;
        while (*p && !isspace((unsigned char)*p) && i < (int)sizeof mv - 1) mv[i++] = *p++;
        mv[i] = '\0';

        if (a->engine_cancelled) {
            a->engine_searching = 0;
            a->engine_cancelled = 0;
            return;
        }

        Move legal[MAX_MOVES];
        int lc = generate_legal_moves(&a->pos, legal, MAX_MOVES);

        int from = -1, to = -1, promo = 0;
        if (strlen(mv) >= 4) {
            char s1[3] = { mv[0], mv[1], 0 };
            char s2[3] = { mv[2], mv[3], 0 };
            from = sq_from_alg(s1);
            to = sq_from_alg(s2);
            if (strlen(mv) >= 5) promo = (char)toupper((unsigned char)mv[4]);
        }

        if (from < 0 || to < 0) {
            set_notice(a, "Engine sent invalid bestmove: %s", mv);
            a->engine_searching = 0;
            return;
        }

        int found = 0;
        Move chosen = {0};

        for (int i = 0; i < lc; i++) {
            if (legal[i].from == from && legal[i].to == to) {
                if ((legal[i].flags & MV_PROMO) && promo != 0) {
                    if (legal[i].promo == promo) {
                        chosen = legal[i];
                        found = 1;
                        break;
                    }
                } else if ((legal[i].flags & MV_PROMO) && promo == 0) {
                    if (legal[i].promo == 'Q') {
                        chosen = legal[i];
                        found = 1;
                    }
                } else {
                    chosen = legal[i];
                    found = 1;
                    break;
                }
            }
        }

        if (!found) {
            set_notice(a, "Engine played illegal move: %s", mv);
            a->engine_searching = 0;
            return;
        }

        // Commit as engine move
        Position before = a->pos;
        char san[32];
        build_san(&before, a->legal, a->legal_count, chosen, san, sizeof san);

        if (a->hist_count >= MAX_HISTORY) {
            memmove(a->hist, a->hist + 1, sizeof(a->hist[0]) * (MAX_HISTORY - 1));
            a->hist_count = MAX_HISTORY - 1;
        }

        Hist *h = &a->hist[a->hist_count++];
        h->pos_before = before;
        h->move = chosen;
        snprintf(h->san, sizeof h->san, "%s", san);
        if (before.side == WHITE) snprintf(h->pgn, sizeof h->pgn, "%d. %s", before.fullmove, san);
        else snprintf(h->pgn, sizeof h->pgn, "%d... %s", before.fullmove, san);

        make_move(&a->pos, chosen);
        a->selected = -1;
        a->promo_mode = 0;
        clear_notice(a);

        // Refresh state
        a->legal_count = generate_legal_moves(&a->pos, a->legal, MAX_MOVES);
        a->in_check = is_in_check(&a->pos, a->pos.side);
        a->check_sq = a->in_check ? find_king(&a->pos, a->pos.side) : -1;
        a->game_over = (a->legal_count == 0);
        if (a->hist_count > 0) {
            a->last_from = a->hist[a->hist_count - 1].move.from;
            a->last_to = a->hist[a->hist_count - 1].move.to;
        } else {
            a->last_from = a->last_to = -1;
        }

        a->engine_searching = 0;
        a->engine_cancelled = 0;
        return;
    }
}

static void engine_read_available(App *a) {
    if (a->engine_out < 0) return;

    char buf[512];
    for (;;) {
        ssize_t n = read(a->engine_out, buf, sizeof buf);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\r') continue;
                if (c == '\n') {
                    a->engine_line[a->engine_line_len] = '\0';
                    if (a->engine_line_len > 0) engine_process_line(a, a->engine_line);
                    a->engine_line_len = 0;
                } else {
                    if (a->engine_line_len < MAX_LINE - 1) {
                        a->engine_line[a->engine_line_len++] = c;
                    }
                }
            }
        } else if (n == 0) {
            // Engine exited
            close(a->engine_out);
            a->engine_out = -1;
            a->engine_enabled = 0;
            a->engine_searching = 0;
            a->engine_cancelled = 0;
            set_notice(a, "Engine disconnected");
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
            close(a->engine_out);
            a->engine_out = -1;
            a->engine_enabled = 0;
            a->engine_searching = 0;
            a->engine_cancelled = 0;
            set_notice(a, "Engine read error");
            return;
        }
    }
}

static int wait_for_engine_flag(App *a, int want_uciok, int want_readyok, int timeout_ms) {
    long long start = now_ms();
    while (now_ms() - start < timeout_ms) {
        engine_read_available(a);
        if (want_uciok && a->uci_ok) return 1;
        if (want_readyok && a->ready_ok) return 1;

        if (a->engine_out < 0) return 0;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(a->engine_out, &rfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        int rv = select(a->engine_out + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0 && errno != EINTR) return 0;
    }
    return 0;
}

static int engine_handshake(App *a) {
    if (!a->engine_enabled || a->engine_out < 0 || a->engine_in < 0) return 0;

    if (engine_sendf(a, "uci\n") < 0) return 0;
    if (!wait_for_engine_flag(a, 1, 0, 5000)) return 0;

    if (engine_sendf(a, "isready\n") < 0) return 0;
    if (!wait_for_engine_flag(a, 0, 1, 5000)) return 0;

    a->ready_ok = 0;
    a->uci_ok = 0;

    if (engine_sendf(a, "ucinewgame\n") < 0) return 0;
    if (engine_sendf(a, "isready\n") < 0) return 0;
    if (!wait_for_engine_flag(a, 0, 1, 5000)) return 0;
    a->ready_ok = 0;

    return 1;
}

static void refresh_legal(App *a) {
    a->legal_count = generate_legal_moves(&a->pos, a->legal, MAX_MOVES);
    a->in_check = is_in_check(&a->pos, a->pos.side);
    a->check_sq = a->in_check ? find_king(&a->pos, a->pos.side) : -1;
    a->game_over = (a->legal_count == 0);

    if (a->hist_count > 0) {
        a->last_from = a->hist[a->hist_count - 1].move.from;
        a->last_to = a->hist[a->hist_count - 1].move.to;
    } else {
        a->last_from = a->last_to = -1;
    }
}

static void init_position(Position *p) {
    const char *rows[8] = {
        "rnbqkbnr",
        "pppppppp",
        "........",
        "........",
        "........",
        "........",
        "PPPPPPPP",
        "RNBQKBNR"
    };

    for (int r = 0; r < 8; r++) {
        for (int f = 0; f < 8; f++) {
            p->b[sq(f, r)] = rows[r][f];
        }
    }

    p->side = WHITE;
    p->castling = WK | WQ | BK | BQ;
    p->ep = -1;
    p->halfmove = 0;
    p->fullmove = 1;
}

static int current_human_can_move(const App *a) {
    if (!a->engine_enabled) return 1;
    return a->pos.side != a->engine_side;
}

static void push_history(App *a, const Position *before, Move m, const char *san) {
    if (a->hist_count >= MAX_HISTORY) {
        memmove(a->hist, a->hist + 1, sizeof(a->hist[0]) * (MAX_HISTORY - 1));
        a->hist_count = MAX_HISTORY - 1;
    }

    Hist *h = &a->hist[a->hist_count++];
    h->pos_before = *before;
    h->move = m;
    snprintf(h->san, sizeof h->san, "%s", san);
    if (before->side == WHITE) snprintf(h->pgn, sizeof h->pgn, "%d. %s", before->fullmove, san);
    else snprintf(h->pgn, sizeof h->pgn, "%d... %s", before->fullmove, san);
}

static void commit_move(App *a, Move m) {
    Position before = a->pos;
    char san[32];
    build_san(&before, a->legal, a->legal_count, m, san, sizeof san);

    push_history(a, &before, m, san);

    make_move(&a->pos, m);
    a->selected = -1;
    a->promo_mode = 0;
    clear_notice(a);

    refresh_legal(a);

    if (a->game_over) {
        if (a->in_check) {
            set_notice(a, "Checkmate. %s wins.", side_name(-a->pos.side));
        } else {
            set_notice(a, "Stalemate.");
        }
    }

    if (a->engine_enabled && !a->game_over && a->pos.side == a->engine_side) {
        char fen[256];
        position_to_fen(&a->pos, fen, sizeof fen);
        if (engine_sendf(a, "position fen %s\n", fen) < 0) return;

        if (a->ctrl_mode == CTRL_DEPTH) {
            engine_sendf(a, "go depth %d\n", a->depth_limit);
        } else if (a->ctrl_mode == CTRL_NODES) {
            engine_sendf(a, "go nodes %d\n", a->nodes_limit);
        } else {
            engine_sendf(a, "go movetime %d\n", a->time_limit);
        }

        a->engine_searching = 1;
        a->engine_cancelled = 0;
        set_notice(a, "Engine thinking...");
    }
}

static void undo_move(App *a) {
    if (a->engine_searching) {
        engine_sendf(a, "stop\n");
        a->engine_cancelled = 1;
    }

    if (a->hist_count == 0) {
        set_notice(a, "Nothing to undo.");
        return;
    }

    a->hist_count--;
    a->pos = a->hist[a->hist_count].pos_before;
    a->selected = -1;
    a->promo_mode = 0;
    clear_notice(a);
    refresh_legal(a);

    if (a->engine_enabled && !a->game_over && a->pos.side == a->engine_side && !a->engine_searching && !a->engine_cancelled) {
        char fen[256];
        position_to_fen(&a->pos, fen, sizeof fen);
        engine_sendf(a, "position fen %s\n", fen);
        if (a->ctrl_mode == CTRL_DEPTH) engine_sendf(a, "go depth %d\n", a->depth_limit);
        else if (a->ctrl_mode == CTRL_NODES) engine_sendf(a, "go nodes %d\n", a->nodes_limit);
        else engine_sendf(a, "go movetime %d\n", a->time_limit);
        a->engine_searching = 1;
        set_notice(a, "Engine thinking...");
    }
}

static void reset_game(App *a) {
    if (a->engine_searching) {
        engine_sendf(a, "stop\n");
        a->engine_cancelled = 1;
    }

    init_position(&a->pos);
    a->hist_count = 0;
    a->selected = -1;
    a->promo_mode = 0;
    clear_notice(a);
    refresh_legal(a);

    if (a->engine_enabled && a->pos.side == a->engine_side && !a->engine_searching && !a->engine_cancelled) {
        char fen[256];
        position_to_fen(&a->pos, fen, sizeof fen);
        engine_sendf(a, "position fen %s\n", fen);
        if (a->ctrl_mode == CTRL_DEPTH) engine_sendf(a, "go depth %d\n", a->depth_limit);
        else if (a->ctrl_mode == CTRL_NODES) engine_sendf(a, "go nodes %d\n", a->nodes_limit);
        else engine_sendf(a, "go movetime %d\n", a->time_limit);
        a->engine_searching = 1;
        set_notice(a, "Engine thinking...");
    }

    set_notice(a, "New game.");
}

static void cycle_control_mode(App *a) {
    a->ctrl_mode = (ControlMode)((a->ctrl_mode + 1) % 3);
    clear_notice(a);
}

static void adjust_control(App *a, int delta) {
    if (a->ctrl_mode == CTRL_DEPTH) {
        a->depth_limit += delta;
        if (a->depth_limit < 1) a->depth_limit = 1;
        if (a->depth_limit > 99) a->depth_limit = 99;
    } else if (a->ctrl_mode == CTRL_NODES) {
        a->nodes_limit += delta * 1000;
        if (a->nodes_limit < 1000) a->nodes_limit = 1000;
        if (a->nodes_limit > 100000000) a->nodes_limit = 100000000;
    } else {
        a->time_limit += delta * 250;
        if (a->time_limit < 50) a->time_limit = 50;
        if (a->time_limit > 600000) a->time_limit = 600000;
    }
    clear_notice(a);
}

enum {
    KEY_NONE = -1,
    KEY_UP = 1001,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT
};

static int read_byte_timeout(int fd, unsigned char *c, int timeout_ms) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int rv = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (rv <= 0) return 0;

    ssize_t n = read(fd, c, 1);
    return n == 1;
}

static int read_key(void) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n != 1) return KEY_NONE;

    if (c != 27) return (int)c;

    unsigned char c2, c3;
    if (!read_byte_timeout(STDIN_FILENO, &c2, 20)) return 27;
    if (c2 != '[') return 27;
    if (!read_byte_timeout(STDIN_FILENO, &c3, 20)) return 27;

    switch (c3) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        default: return 27;
    }
}

static void enter_terminal(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "This program must run in a terminal.\n");
        exit(1);
    }

    if (tcgetattr(STDIN_FILENO, &g_saved_termios) < 0) {
        perror("tcgetattr");
        exit(1);
    }

    struct termios raw = g_saved_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_lflag |= ISIG;
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) < 0) {
        perror("tcsetattr");
        exit(1);
    }

    printf("\033[?1049h\033[?25l");
    fflush(stdout);
    g_app->terminal_active = 1;
}

static void leave_terminal(void) {
    if (!g_app || !g_app->terminal_active) return;
    printf("\033[0m\033[?25h\033[?1049l");
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
    g_app->terminal_active = 0;
}

static void render(App *a) {
    printf("\033[H\033[J");

    printf("Terminal Chess GUI  |  Unicode pieces  |  UCI engine support\n");
    printf("-----------------------------------------------------------\n\n");

    int selected_targets = 0;
    int king_sq = a->check_sq;
    int target_mask[64];
    int capture_mask[64];
    memset(target_mask, 0, sizeof target_mask);
    memset(capture_mask, 0, sizeof capture_mask);

    if (a->selected >= 0 && !a->promo_mode && !a->game_over) {
        for (int i = 0; i < a->legal_count; i++) {
            if (a->legal[i].from == a->selected) {
                target_mask[a->legal[i].to] = 1;
                if (a->legal[i].flags & (MV_CAPTURE | MV_EP)) capture_mask[a->legal[i].to] = 1;
            }
        }
        for (int i = 0; i < 64; i++) if (target_mask[i]) selected_targets++;
    }

    printf("    a  b  c  d  e  f  g  h\n");
    for (int r = 0; r < 8; r++) {
        printf(" %d ", 8 - r);
        for (int f = 0; f < 8; f++) {
            int s = sq(f, r);
            char pc = a->pos.b[s];

            int bg = ((r + f) & 1) ? BG_DARK : BG_LIGHT;

            if (s == a->last_from || s == a->last_to) bg = BG_LAST;
            if (a->selected >= 0 && s == a->selected) bg = BG_SELECTED;
            if (a->selected >= 0 && target_mask[s]) bg = capture_mask[s] ? BG_CAPTURE : BG_TARGET;
            if (a->in_check && s == king_sq) bg = BG_CHECK;

            int fg = (pc == '.') ? 0 : ((piece_color(pc) == WHITE) ? 15 : 16);

            int reverse = (s == a->cursor);

            printf("\033[0m");
            if (reverse) printf("\033[7m");
            printf("\033[38;5;%dm\033[48;5;%dm %s \033[0m", fg, bg, piece_glyph(pc));
        }
        printf(" %d\n", 8 - r);
    }
    printf("    a  b  c  d  e  f  g  h\n\n");

    if (a->game_over) {
        if (a->in_check) {
            printf("Status: Checkmate. %s wins.\n", side_name(-a->pos.side));
        } else {
            printf("Status: Stalemate.\n");
        }
    } else {
        printf("Status: %s to move%s\n", side_name(a->pos.side), a->in_check ? " (check)" : "");
    }

    char cur_alg[3], sel_alg[3];
    sq_to_alg(a->cursor, cur_alg);
    if (a->selected >= 0) sq_to_alg(a->selected, sel_alg);

    printf("Cursor: %s", cur_alg);
    if (a->selected >= 0) {
        char pc = a->pos.b[a->selected];
        printf("   Selected: %s %s   (%d legal targets)", sel_alg, piece_glyph(pc), selected_targets);
    }
    printf("\n");

    if (a->engine_enabled) {
        printf("Engine: %s  |  side: %s  |  %s\n",
               a->engine_argv[0],
               side_name(a->engine_side),
               a->engine_searching ? (a->engine_cancelled ? "stopping..." : "thinking...") : "idle");
    } else {
        printf("Engine: off\n");
    }

    printf("Search: depth=%d   nodes=%d   time=%d ms   | active: %s\n",
           a->depth_limit, a->nodes_limit, a->time_limit, control_name(a->ctrl_mode));

    if (a->notice[0]) {
        printf("Message: %s\n", a->notice);
    }

    printf("\nRecent PGN (last 5 plies):\n");
    if (a->hist_count == 0) {
        printf("  (none)\n");
    } else {
        int start = a->hist_count - 5;
        if (start < 0) start = 0;
        for (int i = start; i < a->hist_count; i++) {
            printf("  %s\n", a->hist[i].pgn);
        }
    }

    printf("\nControls:\n");
    printf("  arrows/hjkl = move cursor   Enter/Space = select/move   Esc = cancel\n");
    printf("  u = undo   r = reset   c = cycle search control   -/= = adjust active control\n");
    printf("  q = quit   (promotion: 1=Q 2=R 3=B 4=N, then Enter)\n");

    if (a->promo_mode) {
        static const char promo_piece[4] = {'Q','R','B','N'};
        printf("\nPromotion pending: current choice = %c   [1=Q 2=R 3=B 4=N, Enter confirms, Esc cancels]\n",
               promo_piece[a->promo_index]);
    }

    fflush(stdout);
}

static int find_move_candidates(const App *a, int from, int to, Move out[4]) {
    int n = 0;
    for (int i = 0; i < a->legal_count; i++) {
        if (a->legal[i].from == from && a->legal[i].to == to) {
            if (n < 4) out[n++] = a->legal[i];
        }
    }
    return n;
}

static int handle_board_action(App *a) {
    if (a->game_over) return 0;
    if (!current_human_can_move(a)) return 0;
    if (a->promo_mode) return 0;

    if (a->selected < 0) {
        char pc = a->pos.b[a->cursor];
        if (pc != '.' && piece_color(pc) == a->pos.side) {
            a->selected = a->cursor;
            clear_notice(a);
            return 1;
        }
        return 0;
    }

    if (a->cursor == a->selected) {
        a->selected = -1;
        clear_notice(a);
        return 1;
    }

    char pc = a->pos.b[a->cursor];
    if (pc != '.' && piece_color(pc) == a->pos.side) {
        a->selected = a->cursor;
        clear_notice(a);
        return 1;
    }

    Move cand[4];
    int n = find_move_candidates(a, a->selected, a->cursor, cand);
    if (n == 0) {
        a->selected = -1;
        clear_notice(a);
        return 1;
    }

    if (n == 1) {
        commit_move(a, cand[0]);
        return 1;
    }

    // promotion choices
    a->promo_mode = 1;
    a->promo_count = n;
    a->promo_index = 0;
    for (int i = 0; i < n; i++) a->promo_choices[i] = cand[i];
    set_notice(a, "Promotion pending.");
    return 1;
}

static int handle_key(App *a, int key) {
    if (key == KEY_NONE) return 0;

    if (a->promo_mode) {
        int changed = 0;
        if (key == 27) {
            a->promo_mode = 0;
            clear_notice(a);
            changed = 1;
        } else if (key == '\r' || key == '\n' || key == ' ') {
            commit_move(a, a->promo_choices[a->promo_index]);
            changed = 1;
        } else if (key == '1' || key == 'q' || key == 'Q') {
            a->promo_index = 0;
            changed = 1;
        } else if (key == '2' || key == 'r' || key == 'R') {
            if (a->promo_count > 1) a->promo_index = 1;
            changed = 1;
        } else if (key == '3' || key == 'b' || key == 'B') {
            if (a->promo_count > 2) a->promo_index = 2;
            changed = 1;
        } else if (key == '4' || key == 'n' || key == 'N') {
            if (a->promo_count > 3) a->promo_index = 3;
            changed = 1;
        }
        return changed;
    }

    switch (key) {
        case 'q':
        case 3: // Ctrl-C
            g_quit = 1;
            return 0;

        case 'u':
            undo_move(a);
            return 1;

        case 'r':
            reset_game(a);
            return 1;

        case 'c':
            cycle_control_mode(a);
            return 1;

        case '-':
            adjust_control(a, -1);
            return 1;

        case '=':
            adjust_control(a, +1);
            return 1;

        case 27:
            a->selected = -1;
            clear_notice(a);
            return 1;

        case KEY_LEFT:
        case 'h':
            if (file_of(a->cursor) > 0) a->cursor--;
            return 1;

        case KEY_RIGHT:
        case 'l':
            if (file_of(a->cursor) < 7) a->cursor++;
            return 1;

        case KEY_UP:
        case 'k':
            if (rank_of(a->cursor) > 0) a->cursor -= 8;
            return 1;

        case KEY_DOWN:
        case 'j':
            if (rank_of(a->cursor) < 7) a->cursor += 8;
            return 1;

        case '\r':
        case '\n':
        case ' ':
            return handle_board_action(a);

        default:
            return 0;
    }
}

static void init_app(App *a) {
    memset(a, 0, sizeof *a);

    a->cursor = sq(4, 6); // e2
    a->selected = -1;
    a->ctrl_mode = CTRL_DEPTH;
    a->depth_limit = 8;
    a->nodes_limit = 50000;
    a->time_limit = 1000;

    a->engine_side = BLACK;
    a->engine_in = -1;
    a->engine_out = -1;
    a->engine_pid = 0;

    a->terminal_active = 0;

    init_position(&a->pos);
    refresh_legal(a);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [--engine \"cmd\"] [--engine-side white|black]\n"
        "           [--depth N] [--nodes N] [--time MS]\n"
        "\n"
        "Examples:\n"
        "  %s\n"
        "  %s --engine \"stockfish\" --engine-side black\n",
        prog, prog, prog);
}

static int parse_side(const char *s) {
    if (!strcasecmp(s, "white")) return WHITE;
    if (!strcasecmp(s, "black")) return BLACK;
    return 0;
}

static int parse_args(App *a, int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--engine") && i + 1 < argc) {
            a->engine_cmd_raw = strdup(argv[++i]);
            if (!a->engine_cmd_raw) return 0;
        } else if (!strcmp(argv[i], "--engine-side") && i + 1 < argc) {
            int s = parse_side(argv[++i]);
            if (!s) return 0;
            a->engine_side = s;
        } else if (!strcmp(argv[i], "--depth") && i + 1 < argc) {
            a->depth_limit = atoi(argv[++i]);
            if (a->depth_limit < 1) a->depth_limit = 1;
        } else if (!strcmp(argv[i], "--nodes") && i + 1 < argc) {
            a->nodes_limit = atoi(argv[++i]);
            if (a->nodes_limit < 1000) a->nodes_limit = 1000;
        } else if (!strcmp(argv[i], "--time") && i + 1 < argc) {
            a->time_limit = atoi(argv[++i]);
            if (a->time_limit < 50) a->time_limit = 50;
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(argv[0]);
            exit(0);
        } else {
            return 0;
        }
    }

    if (a->engine_cmd_raw) {
        if (!tokenize_engine_cmd(a)) {
            free(a->engine_cmd_raw);
            a->engine_cmd_raw = NULL;
            return 0;
        }
        a->engine_enabled = 1;
    }

    return 1;
}

static void cleanup_terminal(void) {
    if (!g_app) return;
    if (g_app->terminal_active) {
        printf("\033[0m\033[?25h\033[?1049l");
        fflush(stdout);
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
        g_app->terminal_active = 0;
    }
}

static void cleanup_all(void) {
    if (!g_app) return;
    cleanup_engine(g_app);
    cleanup_terminal();
    if (g_app->engine_cmd_raw) {
        free(g_app->engine_cmd_raw);
        g_app->engine_cmd_raw = NULL;
    }
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);

    App app;
    g_app = &app;
    init_app(&app);

    if (!parse_args(&app, argc, argv)) {
        usage(argv[0]);
        cleanup_all();
        return 1;
    }

    if (app.engine_enabled) {
        if (spawn_engine(&app) != 0) {
            app.engine_enabled = 0;
            set_notice(&app, "Failed to spawn engine.");
        } else if (!engine_handshake(&app)) {
            cleanup_engine(&app);
            app.engine_enabled = 0;
            set_notice(&app, "Engine handshake failed.");
        }
    }

    atexit(cleanup_all);
    enter_terminal();
    refresh_legal(&app);

    int dirty = 1;

    while (!g_quit) {
        if (app.engine_enabled &&
            !app.engine_searching &&
            !app.engine_cancelled &&
            !app.game_over &&
            app.pos.side == app.engine_side) {
            char fen[256];
            position_to_fen(&app.pos, fen, sizeof fen);
            if (engine_sendf(&app, "position fen %s\n", fen) == 0) {
                if (app.ctrl_mode == CTRL_DEPTH) engine_sendf(&app, "go depth %d\n", app.depth_limit);
                else if (app.ctrl_mode == CTRL_NODES) engine_sendf(&app, "go nodes %d\n", app.nodes_limit);
                else engine_sendf(&app, "go movetime %d\n", app.time_limit);
                app.engine_searching = 1;
                set_notice(&app, "Engine thinking...");
                dirty = 1;
            }
        }

        if (dirty) {
            render(&app);
            dirty = 0;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;

        FD_SET(STDIN_FILENO, &rfds);
        maxfd = STDIN_FILENO;

        if (app.engine_out >= 0) {
            FD_SET(app.engine_out, &rfds);
            if (app.engine_out > maxfd) maxfd = app.engine_out;
        }

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;

        int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (rv == 0) continue;

        if (app.engine_out >= 0 && FD_ISSET(app.engine_out, &rfds)) {
            engine_read_available(&app);
            dirty = 1;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            int key = read_key();
            if (key != KEY_NONE) {
                if (handle_key(&app, key)) dirty = 1;
            }
        }

        if (app.engine_searching && app.engine_cancelled && !app.engine_enabled) {
            app.engine_searching = 0;
            app.engine_cancelled = 0;
        }

        if (app.game_over && app.engine_searching) {
            engine_sendf(&app, "stop\n");
            app.engine_cancelled = 1;
            dirty = 1;
        }

        if (g_quit) break;
    }

    cleanup_all();
    return 0;
}
