// chess_tui.c - single-file terminal chess TUI with UCI engine support (macOS Terminal)
// No external dependencies (no ncurses). Uses ANSI escape codes + termios.
//
// Build: clang -O2 -std=c11 -Wall -Wextra -pedantic chess_tui.c -o chess_tui
// Run : ./chess_tui [path_to_uci_engine]
//
// Keys:
//   Arrows / hjkl : move cursor
//   Space/Enter   : select / move
//   Esc           : cancel selection
//   u             : undo 1 ply
//   U             : undo 2 plies (useful vs engine)
//   r             : reset
//   q             : quit
//   Promotion     : q/r/b/n
//
// Engine (optional):
//   If engine path is provided, engine plays Black (you play White).
//   Uses: "uci", "isready", "ucinewgame", "position startpos moves ...", "go movetime 500"
//
// Notes:
//   - Implements legal move generation: castling, en passant, promotion, check legality.
//   - Produces SAN (PGN move text) with basic disambiguation, check, mate.
//   - Terminal must support ANSI escapes (macOS Terminal does).
//
// Limitations:
//   - No draw adjudication (50-move, repetition) beyond stalemate/mate detection.
//   - Minimal UCI options handling.
//   - SAN is intended to be correct for standard moves; edge cases are handled but not exhaustively tested.

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

enum { WHITE = 0, BLACK = 1 };

enum {
    CASTLE_WK = 1 << 0,
    CASTLE_WQ = 1 << 1,
    CASTLE_BK = 1 << 2,
    CASTLE_BQ = 1 << 3
};

enum {
    MF_CAPTURE   = 1 << 0,
    MF_EP        = 1 << 1,
    MF_CASTLE    = 1 << 2,
    MF_PROMO     = 1 << 3
};

typedef struct {
    char b[64];        // pieces: P N B R Q K (white), p n b r q k (black), '.' empty
    int side;          // WHITE/BLACK
    int castling;      // bitmask
    int ep;            // en-passant target square index or -1
    int halfmove;      // not fully used for draw here
    int fullmove;
} State;

typedef struct {
    uint8_t from, to;
    char prom;   // 'q','r','b','n' (lowercase), or 0
    uint8_t flags;
} Move;

#define MAX_MOVES 256
#define MAX_PLY   1024

static struct termios g_orig_termios;
static int g_raw = 0;

static void die(const char *msg) {
    write(STDERR_FILENO, msg, strlen(msg));
    write(STDERR_FILENO, "\n", 1);
    exit(1);
}

static void disable_raw_mode(void) {
    if (g_raw) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_raw = 0;
        // show cursor
        write(STDOUT_FILENO, "\x1b[?25h", 6);
    }
}

static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) die("tcgetattr failed");
    atexit(disable_raw_mode);

    struct termios raw = g_orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; // 100ms

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr failed");
    g_raw = 1;
    // hide cursor
    write(STDOUT_FILENO, "\x1b[?25l", 6);
}

static void on_sigint(int sig) {
    (void)sig;
    disable_raw_mode();
    _exit(130);
}

static inline int file_of(int sq) { return sq & 7; }
static inline int rank_of(int sq) { return sq >> 3; }
static inline int sq_of(int f, int r) { return (r << 3) | f; }

static void sq_to_alg(int sq, char out[3]) {
    out[0] = (char)('a' + file_of(sq));
    out[1] = (char)('1' + rank_of(sq));
    out[2] = 0;
}

static int alg_to_sq(const char a, const char b) {
    if (a < 'a' || a > 'h' || b < '1' || b > '8') return -1;
    return sq_of(a - 'a', b - '1');
}

static inline bool is_white_piece(char p) { return (p >= 'A' && p <= 'Z'); }
static inline bool is_black_piece(char p) { return (p >= 'a' && p <= 'z'); }
static inline int piece_color(char p) {
    if (is_white_piece(p)) return WHITE;
    if (is_black_piece(p)) return BLACK;
    return -1;
}
static inline char piece_lower(char p) { return (char)tolower((unsigned char)p); }

static void state_set_startpos(State *s) {
    const char *start =
        "RNBQKBNR"
        "PPPPPPPP"
        "........"
        "........"
        "........"
        "........"
        "pppppppp"
        "rnbqkbnr";
    // Our internal ranks: rank 0 is "1" (White back rank).
    // Above string is rank 0..7? It's currently white then black; flip to match our coords.
    // Let's build properly by explicit placement.
    for (int i = 0; i < 64; i++) s->b[i] = '.';

    // White pieces
    const char *wb = "RNBQKBNR";
    for (int f = 0; f < 8; f++) {
        s->b[sq_of(f, 0)] = wb[f];
        s->b[sq_of(f, 1)] = 'P';
        s->b[sq_of(f, 6)] = 'p';
        s->b[sq_of(f, 7)] = (char)tolower(wb[f]);
    }

    (void)start;
    s->side = WHITE;
    s->castling = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
    s->ep = -1;
    s->halfmove = 0;
    s->fullmove = 1;
}

static int find_king(const State *s, int color) {
    char k = (color == WHITE) ? 'K' : 'k';
    for (int i = 0; i < 64; i++) if (s->b[i] == k) return i;
    return -1;
}

static bool on_board(int f, int r) {
    return (f >= 0 && f < 8 && r >= 0 && r < 8);
}

static bool square_attacked(const State *s, int sq, int by_color) {
    int f = file_of(sq), r = rank_of(sq);

    // Pawns
    if (by_color == WHITE) {
        int r2 = r - 1;
        if (r2 >= 0) {
            if (f - 1 >= 0 && s->b[sq_of(f - 1, r2)] == 'P') return true;
            if (f + 1 < 8 && s->b[sq_of(f + 1, r2)] == 'P') return true;
        }
    } else {
        int r2 = r + 1;
        if (r2 < 8) {
            if (f - 1 >= 0 && s->b[sq_of(f - 1, r2)] == 'p') return true;
            if (f + 1 < 8 && s->b[sq_of(f + 1, r2)] == 'p') return true;
        }
    }

    // Knights
    static const int kdf[8] = {+1,+2,+2,+1,-1,-2,-2,-1};
    static const int kdr[8] = {+2,+1,-1,-2,-2,-1,+1,+2};
    for (int i = 0; i < 8; i++) {
        int nf = f + kdf[i], nr = r + kdr[i];
        if (!on_board(nf, nr)) continue;
        char p = s->b[sq_of(nf, nr)];
        if (by_color == WHITE && p == 'N') return true;
        if (by_color == BLACK && p == 'n') return true;
    }

    // Bishops / Queens (diagonals)
    static const int bdir[4][2] = {{+1,+1},{+1,-1},{-1,+1},{-1,-1}};
    for (int d = 0; d < 4; d++) {
        int nf = f + bdir[d][0], nr = r + bdir[d][1];
        while (on_board(nf, nr)) {
            char p = s->b[sq_of(nf, nr)];
            if (p != '.') {
                if (by_color == WHITE && (p == 'B' || p == 'Q')) return true;
                if (by_color == BLACK && (p == 'b' || p == 'q')) return true;
                break;
            }
            nf += bdir[d][0]; nr += bdir[d][1];
        }
    }

    // Rooks / Queens (orthogonal)
    static const int rdir[4][2] = {{+1,0},{-1,0},{0,+1},{0,-1}};
    for (int d = 0; d < 4; d++) {
        int nf = f + rdir[d][0], nr = r + rdir[d][1];
        while (on_board(nf, nr)) {
            char p = s->b[sq_of(nf, nr)];
            if (p != '.') {
                if (by_color == WHITE && (p == 'R' || p == 'Q')) return true;
                if (by_color == BLACK && (p == 'r' || p == 'q')) return true;
                break;
            }
            nf += rdir[d][0]; nr += rdir[d][1];
        }
    }

    // King
    for (int df = -1; df <= 1; df++) for (int dr = -1; dr <= 1; dr++) {
        if (df == 0 && dr == 0) continue;
        int nf = f + df, nr = r + dr;
        if (!on_board(nf, nr)) continue;
        char p = s->b[sq_of(nf, nr)];
        if (by_color == WHITE && p == 'K') return true;
        if (by_color == BLACK && p == 'k') return true;
    }

    return false;
}

static bool in_check(const State *s, int color) {
    int ksq = find_king(s, color);
    if (ksq < 0) return false;
    return square_attacked(s, ksq, color ^ 1);
}

static void add_move(Move *out, int *n, int from, int to, uint8_t flags, char prom) {
    Move m;
    m.from = (uint8_t)from;
    m.to = (uint8_t)to;
    m.flags = flags;
    m.prom = prom;
    out[(*n)++] = m;
}

static void gen_pseudo_moves(const State *s, Move *out, int *n) {
    *n = 0;
    int us = s->side;
    for (int from = 0; from < 64; from++) {
        char p = s->b[from];
        if (p == '.') continue;
        if (piece_color(p) != us) continue;

        int f = file_of(from), r = rank_of(from);
        char pl = piece_lower(p);

        if (pl == 'p') {
            int dir = (us == WHITE) ? +1 : -1;
            int start_rank = (us == WHITE) ? 1 : 6;
            int promo_rank = (us == WHITE) ? 7 : 0;

            int r1 = r + dir;
            if (r1 >= 0 && r1 < 8) {
                int to = sq_of(f, r1);
                if (s->b[to] == '.') {
                    // move forward
                    if (r1 == promo_rank) {
                        add_move(out, n, from, to, MF_PROMO, 'q');
                        add_move(out, n, from, to, MF_PROMO, 'r');
                        add_move(out, n, from, to, MF_PROMO, 'b');
                        add_move(out, n, from, to, MF_PROMO, 'n');
                    } else {
                        add_move(out, n, from, to, 0, 0);
                    }
                    // double
                    if (r == start_rank) {
                        int r2 = r + 2 * dir;
                        int to2 = sq_of(f, r2);
                        if (s->b[to2] == '.') {
                            add_move(out, n, from, to2, 0, 0);
                        }
                    }
                }
                // captures
                for (int df = -1; df <= 1; df += 2) {
                    int nf = f + df;
                    if (nf < 0 || nf >= 8) continue;
                    int cap = sq_of(nf, r1);
                    char tp = s->b[cap];

                    // normal capture
                    if (tp != '.' && piece_color(tp) == (us ^ 1)) {
                        uint8_t flags = MF_CAPTURE;
                        if (r1 == promo_rank) {
                            add_move(out, n, from, cap, (uint8_t)(flags | MF_PROMO), 'q');
                            add_move(out, n, from, cap, (uint8_t)(flags | MF_PROMO), 'r');
                            add_move(out, n, from, cap, (uint8_t)(flags | MF_PROMO), 'b');
                            add_move(out, n, from, cap, (uint8_t)(flags | MF_PROMO), 'n');
                        } else {
                            add_move(out, n, from, cap, flags, 0);
                        }
                    }

                    // en passant
                    if (s->ep == cap) {
                        add_move(out, n, from, cap, (uint8_t)(MF_CAPTURE | MF_EP), 0);
                    }
                }
            }
        } else if (pl == 'n') {
            static const int kdf[8] = {+1,+2,+2,+1,-1,-2,-2,-1};
            static const int kdr[8] = {+2,+1,-1,-2,-2,-1,+1,+2};
            for (int i = 0; i < 8; i++) {
                int nf = f + kdf[i], nr = r + kdr[i];
                if (!on_board(nf, nr)) continue;
                int to = sq_of(nf, nr);
                char tp = s->b[to];
                if (tp == '.' || piece_color(tp) == (us ^ 1)) {
                    uint8_t flags = (tp != '.') ? MF_CAPTURE : 0;
                    add_move(out, n, from, to, flags, 0);
                }
            }
        } else if (pl == 'b' || pl == 'r' || pl == 'q') {
            static const int bdir[4][2] = {{+1,+1},{+1,-1},{-1,+1},{-1,-1}};
            static const int rdir[4][2] = {{+1,0},{-1,0},{0,+1},{0,-1}};
            const int (*dirs)[2] = NULL;
            int ndirs = 0;

            int all[8][2];
            if (pl == 'b') {
                memcpy(all, bdir, sizeof(bdir));
                dirs = all; ndirs = 4;
            } else if (pl == 'r') {
                memcpy(all, rdir, sizeof(rdir));
                dirs = all; ndirs = 4;
            } else {
                memcpy(all, bdir, sizeof(bdir));
                memcpy(all + 4, rdir, sizeof(rdir));
                dirs = all; ndirs = 8;
            }

            for (int d = 0; d < ndirs; d++) {
                int nf = f + dirs[d][0], nr = r + dirs[d][1];
                while (on_board(nf, nr)) {
                    int to = sq_of(nf, nr);
                    char tp = s->b[to];
                    if (tp == '.') {
                        add_move(out, n, from, to, 0, 0);
                    } else {
                        if (piece_color(tp) == (us ^ 1)) {
                            add_move(out, n, from, to, MF_CAPTURE, 0);
                        }
                        break;
                    }
                    nf += dirs[d][0]; nr += dirs[d][1];
                }
            }
        } else if (pl == 'k') {
            for (int df = -1; df <= 1; df++) for (int dr = -1; dr <= 1; dr++) {
                if (df == 0 && dr == 0) continue;
                int nf = f + df, nr = r + dr;
                if (!on_board(nf, nr)) continue;
                int to = sq_of(nf, nr);
                char tp = s->b[to];
                if (tp == '.' || piece_color(tp) == (us ^ 1)) {
                    uint8_t flags = (tp != '.') ? MF_CAPTURE : 0;
                    add_move(out, n, from, to, flags, 0);
                }
            }

            // Castling (pseudo; legality checked later incl. attacked squares)
            if (us == WHITE) {
                if ((s->castling & CASTLE_WK) &&
                    s->b[sq_of(5,0)] == '.' && s->b[sq_of(6,0)] == '.') {
                    add_move(out, n, from, sq_of(6,0), MF_CASTLE, 0);
                }
                if ((s->castling & CASTLE_WQ) &&
                    s->b[sq_of(1,0)] == '.' && s->b[sq_of(2,0)] == '.' && s->b[sq_of(3,0)] == '.') {
                    add_move(out, n, from, sq_of(2,0), MF_CASTLE, 0);
                }
            } else {
                if ((s->castling & CASTLE_BK) &&
                    s->b[sq_of(5,7)] == '.' && s->b[sq_of(6,7)] == '.') {
                    add_move(out, n, from, sq_of(6,7), MF_CASTLE, 0);
                }
                if ((s->castling & CASTLE_BQ) &&
                    s->b[sq_of(1,7)] == '.' && s->b[sq_of(2,7)] == '.' && s->b[sq_of(3,7)] == '.') {
                    add_move(out, n, from, sq_of(2,7), MF_CASTLE, 0);
                }
            }
        }
    }
}

static void apply_move(State *s, const Move *m) {
    char p = s->b[m->from];
    char captured = s->b[m->to];

    // Reset en-passant by default
    int old_ep = s->ep;
    (void)old_ep;
    s->ep = -1;

    // halfmove clock
    if (piece_lower(p) == 'p' || (m->flags & MF_CAPTURE)) s->halfmove = 0;
    else s->halfmove++;

    // Move piece (special cases later)
    s->b[m->from] = '.';

    // En passant capture: remove pawn behind target square
    if (m->flags & MF_EP) {
        int dir = (s->side == WHITE) ? -1 : +1;
        int cap_sq = (int)m->to + 8 * dir;
        captured = s->b[cap_sq];
        s->b[cap_sq] = '.';
    }

    // Castling: move rook too
    if (m->flags & MF_CASTLE) {
        // King has already moved from -> to; we place king below after promo handling.
        int to = m->to;
        if (s->side == WHITE) {
            // e1->g1: rook h1->f1 ; e1->c1: rook a1->d1
            if (to == sq_of(6,0)) {
                s->b[sq_of(5,0)] = s->b[sq_of(7,0)];
                s->b[sq_of(7,0)] = '.';
            } else if (to == sq_of(2,0)) {
                s->b[sq_of(3,0)] = s->b[sq_of(0,0)];
                s->b[sq_of(0,0)] = '.';
            }
        } else {
            if (to == sq_of(6,7)) {
                s->b[sq_of(5,7)] = s->b[sq_of(7,7)];
                s->b[sq_of(7,7)] = '.';
            } else if (to == sq_of(2,7)) {
                s->b[sq_of(3,7)] = s->b[sq_of(0,7)];
                s->b[sq_of(0,7)] = '.';
            }
        }
    }

    // Promotion
    if (m->flags & MF_PROMO) {
        char prom = m->prom ? m->prom : 'q';
        if (s->side == WHITE) prom = (char)toupper((unsigned char)prom);
        s->b[m->to] = prom;
    } else {
        s->b[m->to] = p;
    }

    // Update castling rights
    // If king moves, lose both
    if (p == 'K') s->castling &= ~(CASTLE_WK | CASTLE_WQ);
    if (p == 'k') s->castling &= ~(CASTLE_BK | CASTLE_BQ);

    // If rook moves from original squares, lose that side
    if (p == 'R') {
        if (m->from == sq_of(0,0)) s->castling &= ~CASTLE_WQ;
        if (m->from == sq_of(7,0)) s->castling &= ~CASTLE_WK;
    }
    if (p == 'r') {
        if (m->from == sq_of(0,7)) s->castling &= ~CASTLE_BQ;
        if (m->from == sq_of(7,7)) s->castling &= ~CASTLE_BK;
    }

    // If rook is captured on original squares, lose that right
    if (captured == 'R') {
        if (m->to == sq_of(0,0)) s->castling &= ~CASTLE_WQ;
        if (m->to == sq_of(7,0)) s->castling &= ~CASTLE_WK;
    }
    if (captured == 'r') {
        if (m->to == sq_of(0,7)) s->castling &= ~CASTLE_BQ;
        if (m->to == sq_of(7,7)) s->castling &= ~CASTLE_BK;
    }

    // Set en passant target if pawn double move
    if (piece_lower(p) == 'p') {
        int rf = rank_of(m->from), rt = rank_of(m->to);
        if (abs(rt - rf) == 2) {
            int mid_rank = (rf + rt) / 2;
            s->ep = sq_of(file_of(m->from), mid_rank);
        }
    }

    // Switch side and fullmove
    s->side ^= 1;
    if (s->side == WHITE) s->fullmove++;
}

static bool legal_castle_path_ok(const State *s, const Move *m) {
    // assumes pseudo castling move.
    int us = s->side;
    int from = m->from, to = m->to;
    // king must not be in check, and squares crossed must not be attacked.
    if (in_check(s, us)) return false;

    if (us == WHITE) {
        if (to == sq_of(6,0)) { // O-O: e1 f1 g1
            if (square_attacked(s, sq_of(5,0), BLACK)) return false;
            if (square_attacked(s, sq_of(6,0), BLACK)) return false;
            if (s->b[sq_of(7,0)] != 'R') return false;
            return true;
        } else if (to == sq_of(2,0)) { // O-O-O: e1 d1 c1
            if (square_attacked(s, sq_of(3,0), BLACK)) return false;
            if (square_attacked(s, sq_of(2,0), BLACK)) return false;
            if (s->b[sq_of(0,0)] != 'R') return false;
            return true;
        }
    } else {
        if (to == sq_of(6,7)) {
            if (square_attacked(s, sq_of(5,7), WHITE)) return false;
            if (square_attacked(s, sq_of(6,7), WHITE)) return false;
            if (s->b[sq_of(7,7)] != 'r') return false;
            return true;
        } else if (to == sq_of(2,7)) {
            if (square_attacked(s, sq_of(3,7), WHITE)) return false;
            if (square_attacked(s, sq_of(2,7), WHITE)) return false;
            if (s->b[sq_of(0,7)] != 'r') return false;
            return true;
        }
    }
    return false;
}

static void gen_legal_moves(const State *s, Move *out, int *n) {
    Move pm[MAX_MOVES];
    int pn = 0;
    gen_pseudo_moves(s, pm, &pn);

    *n = 0;
    for (int i = 0; i < pn; i++) {
        const Move *m = &pm[i];

        // extra castling legality
        if (m->flags & MF_CASTLE) {
            if (!legal_castle_path_ok(s, m)) continue;
        }

        State t = *s;
        apply_move(&t, m);

        // must not leave own king in check
        if (in_check(&t, s->side)) continue;

        out[(*n)++] = *m;
    }
}

static bool moves_equal_uci(const Move *m, int from, int to, char prom) {
    if ((int)m->from != from || (int)m->to != to) return false;
    char mp = m->prom;
    if ((m->flags & MF_PROMO) == 0) mp = 0;
    if (prom == 0) return mp == 0;
    return (char)tolower((unsigned char)mp) == (char)tolower((unsigned char)prom);
}

static void move_to_uci(const State *s_before, const Move *m, char out[6]) {
    (void)s_before;
    char a[3], b[3];
    sq_to_alg(m->from, a);
    sq_to_alg(m->to, b);
    out[0] = a[0]; out[1] = a[1];
    out[2] = b[0]; out[3] = b[1];
    int idx = 4;
    if (m->flags & MF_PROMO) {
        out[idx++] = (char)tolower((unsigned char)(m->prom ? m->prom : 'q'));
    }
    out[idx] = 0;
}

static char piece_san_letter(char p) {
    switch (piece_lower(p)) {
        case 'n': return 'N';
        case 'b': return 'B';
        case 'r': return 'R';
        case 'q': return 'Q';
        case 'k': return 'K';
        default: return 0; // pawns none
    }
}

static void gen_san_for_move(const State *before, const Move *m, char san_out[32]) {
    // Apply move to get after-state
    State after = *before;
    char moving = before->b[m->from];
    char target = before->b[m->to];

    bool is_capture = (m->flags & MF_CAPTURE) != 0;
    bool is_ep = (m->flags & MF_EP) != 0;

    // Handle castling SAN
    if (m->flags & MF_CASTLE) {
        if (file_of(m->to) == 6) strcpy(san_out, "O-O");
        else strcpy(san_out, "O-O-O");
        apply_move(&after, m);
    } else {
        apply_move(&after, m);

        char buf[32];
        int pos = 0;

        char pl = piece_lower(moving);
        char piece = piece_san_letter(moving);

        if (pl == 'p') {
            if (is_capture) {
                buf[pos++] = (char)('a' + file_of(m->from));
                buf[pos++] = 'x';
            }
            // destination
            char alg[3]; sq_to_alg(m->to, alg);
            buf[pos++] = alg[0]; buf[pos++] = alg[1];

            if (m->flags & MF_PROMO) {
                buf[pos++] = '=';
                char pp = (char)toupper((unsigned char)(m->prom ? m->prom : 'q'));
                buf[pos++] = pp;
            }
        } else {
            buf[pos++] = piece;

            // Disambiguation: if another same piece can also go to destination
            Move leg[MAX_MOVES]; int ln = 0;
            gen_legal_moves(before, leg, &ln);

            bool ambiguous = false;
            bool same_file = false, same_rank = false;
            for (int i = 0; i < ln; i++) {
                if ((int)leg[i].to != (int)m->to) continue;
                if ((int)leg[i].from == (int)m->from) continue;
                char op = before->b[leg[i].from];
                if (piece_color(op) != before->side) continue;
                if (piece_lower(op) != pl) continue;
                // promotions don't matter here (not for piece types)
                ambiguous = true;
                if (file_of(leg[i].from) == file_of(m->from)) same_file = true;
                if (rank_of(leg[i].from) == rank_of(m->from)) same_rank = true;
            }
            if (ambiguous) {
                if (!same_file) {
                    buf[pos++] = (char)('a' + file_of(m->from));
                } else if (!same_rank) {
                    buf[pos++] = (char)('1' + rank_of(m->from));
                } else {
                    buf[pos++] = (char)('a' + file_of(m->from));
                    buf[pos++] = (char)('1' + rank_of(m->from));
                }
            }

            if (is_capture) buf[pos++] = 'x';

            char alg[3]; sq_to_alg(m->to, alg);
            buf[pos++] = alg[0]; buf[pos++] = alg[1];
        }

        // check / mate suffix
        bool gives_check = in_check(&after, after.side);
        if (gives_check) {
            // checkmate?
            Move reply[MAX_MOVES]; int rn = 0;
            gen_legal_moves(&after, reply, &rn);
            if (rn == 0) buf[pos++] = '#';
            else buf[pos++] = '+';
        } else {
            // stalemate doesn't get a suffix
        }

        buf[pos] = 0;
        strcpy(san_out, buf);
    }

    // For en-passant SAN: already represented as capture; no explicit "e.p." in SAN
    (void)target;
    (void)is_ep;
}

static const char *color_name(int c) { return c == WHITE ? "White" : "Black"; }

static bool any_legal_moves(const State *s) {
    Move m[MAX_MOVES]; int n = 0;
    gen_legal_moves(s, m, &n);
    return n > 0;
}

/* ---------------- UCI engine ---------------- */

typedef struct {
    pid_t pid;
    int in_fd;   // write to engine stdin
    int out_fd;  // read from engine stdout
    bool ok;
    char rbuf[8192];
    size_t rlen;
} Engine;

static void engine_close(Engine *e) {
    if (!e->ok) return;
    // try polite quit
    const char *q = "quit\n";
    (void)write(e->in_fd, q, strlen(q));
    close(e->in_fd);
    close(e->out_fd);
    int status = 0;
    waitpid(e->pid, &status, 0);
    e->ok = false;
}

static bool engine_write_line(Engine *e, const char *line) {
    if (!e->ok) return false;
    size_t len = strlen(line);
    ssize_t w = write(e->in_fd, line, len);
    return (w == (ssize_t)len);
}

static bool engine_read_line(Engine *e, char *out, size_t outsz, int timeout_ms) {
    // Read until '\n' in internal buffer, else select/read more
    if (!e->ok) return false;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        // check if we already have a full line
        for (size_t i = 0; i < e->rlen; i++) {
            if (e->rbuf[i] == '\n') {
                size_t linelen = i + 1;
                size_t copylen = (linelen < outsz - 1) ? linelen : (outsz - 1);
                memcpy(out, e->rbuf, copylen);
                out[copylen] = 0;

                // shift buffer
                memmove(e->rbuf, e->rbuf + linelen, e->rlen - linelen);
                e->rlen -= linelen;

                // trim CRLF
                size_t L = strlen(out);
                while (L > 0 && (out[L - 1] == '\n' || out[L - 1] == '\r')) out[--L] = 0;
                return true;
            }
        }

        // timeout?
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (timeout_ms >= 0 && elapsed_ms >= timeout_ms) return false;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(e->out_fd, &rfds);
        struct timeval tv;
        int remain = timeout_ms < 0 ? 250 : (int)(timeout_ms - elapsed_ms);
        if (remain < 0) remain = 0;
        tv.tv_sec = remain / 1000;
        tv.tv_usec = (remain % 1000) * 1000;

        int rc = select(e->out_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (rc == 0) continue;

        char tmp[1024];
        ssize_t r = read(e->out_fd, tmp, sizeof(tmp));
        if (r <= 0) return false;
        if (e->rlen + (size_t)r >= sizeof(e->rbuf)) {
            // drop buffer if overflow
            e->rlen = 0;
        }
        memcpy(e->rbuf + e->rlen, tmp, (size_t)r);
        e->rlen += (size_t)r;
    }
}

static bool engine_wait_token(Engine *e, const char *token, int timeout_ms) {
    char line[1024];
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000L + (now.tv_nsec - start.tv_nsec) / 1000000L;
        if (elapsed >= timeout_ms) return false;

        if (!engine_read_line(e, line, sizeof(line), (int)(timeout_ms - elapsed))) return false;
        if (strstr(line, token)) return true;
    }
}

static bool engine_start(Engine *e, const char *path) {
    memset(e, 0, sizeof(*e));
    e->pid = -1;
    e->in_fd = -1;
    e->out_fd = -1;
    e->ok = false;

    int inpipe[2], outpipe[2];
    if (pipe(inpipe) < 0) return false;
    if (pipe(outpipe) < 0) { close(inpipe[0]); close(inpipe[1]); return false; }

    pid_t pid = fork();
    if (pid < 0) {
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);
        return false;
    }
    if (pid == 0) {
        // child: connect pipes
        dup2(inpipe[0], STDIN_FILENO);
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(outpipe[1], STDERR_FILENO);
        close(inpipe[0]); close(inpipe[1]);
        close(outpipe[0]); close(outpipe[1]);

        execl(path, path, (char*)NULL);
        _exit(127);
    }

    // parent
    close(inpipe[0]);
    close(outpipe[1]);

    e->pid = pid;
    e->in_fd = inpipe[1];
    e->out_fd = outpipe[0];
    e->ok = true;
    e->rlen = 0;

    // nonblocking read
    int flags = fcntl(e->out_fd, F_GETFL, 0);
    fcntl(e->out_fd, F_SETFL, flags | O_NONBLOCK);

    // UCI handshake
    engine_write_line(e, "uci\n");
    if (!engine_wait_token(e, "uciok", 3000)) return false;
    engine_write_line(e, "isready\n");
    if (!engine_wait_token(e, "readyok", 3000)) return false;
    engine_write_line(e, "ucinewgame\n");
    engine_write_line(e, "isready\n");
    if (!engine_wait_token(e, "readyok", 3000)) return false;

    return true;
}

static bool engine_bestmove(Engine *e, const char *pos_cmd, int movetime_ms, char bestmove_out[16]) {
    // pos_cmd already includes trailing '\n' or not? We'll ensure.
    if (!engine_write_line(e, pos_cmd)) return false;

    char go[64];
    snprintf(go, sizeof(go), "go movetime %d\n", movetime_ms);
    engine_write_line(e, go);

    char line[1024];
    // Wait for bestmove
    for (;;) {
        if (!engine_read_line(e, line, sizeof(line), 10000)) return false;
        if (strncmp(line, "bestmove ", 9) == 0) {
            const char *bm = line + 9;
            while (*bm == ' ') bm++;
            // copy first token
            size_t i = 0;
            while (bm[i] && !isspace((unsigned char)bm[i]) && i + 1 < 16) {
                bestmove_out[i] = bm[i];
                i++;
            }
            bestmove_out[i] = 0;
            return true;
        }
    }
}

/* ---------------- TUI ---------------- */

static void ansi_clear(void) { write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7); }

static void ansi_reset(void) { write(STDOUT_FILENO, "\x1b[0m", 4); }

static void ansi_set_style(const char *s) { write(STDOUT_FILENO, s, strlen(s)); }

static void term_beep(void) { write(STDOUT_FILENO, "\a", 1); }

static int read_key(void) {
    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0) return -1;

    if (c == 0x1b) {
        unsigned char seq[2];
        ssize_t n1 = read(STDIN_FILENO, &seq[0], 1);
        ssize_t n2 = read(STDIN_FILENO, &seq[1], 1);
        if (n1 == 1 && n2 == 1 && seq[0] == '[') {
            switch (seq[1]) {
                case 'A': return 1001; // up
                case 'B': return 1002; // down
                case 'C': return 1003; // right
                case 'D': return 1004; // left
            }
        }
        return 27; // ESC
    }

    if (c == '\r' || c == '\n') return 13;
    if (c == 127) return 127;
    return c;
}

static bool is_dest_in_list(int sq, const Move *list, int n) {
    for (int i = 0; i < n; i++) if ((int)list[i].to == sq) return true;
    return false;
}

static void draw(const State *s,
                 int cursor_sq,
                 int selected_sq,
                 const Move *sel_moves, int sel_n,
                 const char san_moves[MAX_PLY][32], int ply,
                 bool vs_engine,
                 const char *engine_path,
                 const char *status_line) {
    ansi_clear();

    // Header
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "Terminal Chess (C, no ncurses)  |  Side: %s%s%s\n",
             color_name(s->side),
             vs_engine ? "  |  Engine: " : "",
             vs_engine ? engine_path : "");
    write(STDOUT_FILENO, hdr, strlen(hdr));

    if (status_line && status_line[0]) {
        write(STDOUT_FILENO, status_line, strlen(status_line));
        write(STDOUT_FILENO, "\n", 1);
    } else {
        write(STDOUT_FILENO, "\n", 1);
    }

    // Board top labels
    write(STDOUT_FILENO, "    a  b  c  d  e  f  g  h\n", 27);

    for (int r = 7; r >= 0; r--) {
        char line[512];
        int pos = 0;
        pos += snprintf(line + pos, sizeof(line) - pos, " %d ", r + 1);
        write(STDOUT_FILENO, line, (size_t)pos);

        for (int f = 0; f < 8; f++) {
            int sq = sq_of(f, r);
            char p = s->b[sq];

            bool dark = ((f + r) & 1) != 0;
            bool cur = (sq == cursor_sq);
            bool sel = (sq == selected_sq);
            bool legal = (selected_sq >= 0 && is_dest_in_list(sq, sel_moves, sel_n));

            // background styles (simple ANSI)
            // dark squares: 48;5;237, light: 48;5;250
            // legal: green, selected: yellow, cursor: inverse
            if (legal) ansi_set_style("\x1b[48;5;22m");         // dark green
            else if (sel) ansi_set_style("\x1b[48;5;136m");     // yellow-brown
            else if (dark) ansi_set_style("\x1b[48;5;237m");
            else ansi_set_style("\x1b[48;5;250m");

            if (cur) ansi_set_style("\x1b[7m"); // reverse

            // piece color
            if (p == '.') {
                ansi_set_style("\x1b[38;5;244m");
                write(STDOUT_FILENO, " . ", 3);
            } else if (is_white_piece(p)) {
                ansi_set_style("\x1b[38;5;231m");
                char cell[4] = {' ', p, ' ', 0};
                write(STDOUT_FILENO, cell, 3);
            } else {
                ansi_set_style("\x1b[38;5;16m");
                char cell[4] = {' ', (char)toupper((unsigned char)p), ' ', 0};
                write(STDOUT_FILENO, cell, 3);
            }

            ansi_reset();
        }
        char tail[16];
        snprintf(tail, sizeof(tail), " %d\n", r + 1);
        write(STDOUT_FILENO, tail, strlen(tail));
    }

    write(STDOUT_FILENO, "    a  b  c  d  e  f  g  h\n\n", 29);

    // Status: check, mate, stalemate
    bool chk = in_check(s, s->side);
    bool any = any_legal_moves(s);
    char stl[256];
    if (chk && !any) snprintf(stl, sizeof(stl), "Status: Checkmate. %s wins.\n", color_name(s->side ^ 1));
    else if (!chk && !any) snprintf(stl, sizeof(stl), "Status: Stalemate.\n");
    else if (chk) snprintf(stl, sizeof(stl), "Status: Check.\n");
    else snprintf(stl, sizeof(stl), "Status: OK.\n");
    write(STDOUT_FILENO, stl, strlen(stl));

    write(STDOUT_FILENO,
          "Commands: arrows/hjkl move  Space/Enter select/move  Esc cancel  u undo  U undo2  r reset  q quit\n\n",
          112);

    // PGN SAN move list
    write(STDOUT_FILENO, "PGN (SAN):\n", 11);
    // print wrapped
    int col = 0;
    for (int i = 0; i < ply; i++) {
        char token[64];
        if ((i % 2) == 0) {
            snprintf(token, sizeof(token), "%d. %s", (i / 2) + 1, san_moves[i]);
        } else {
            snprintf(token, sizeof(token), "%s", san_moves[i]);
        }
        int len = (int)strlen(token);
        if (col + len + 2 > 78) {
            write(STDOUT_FILENO, "\n", 1);
            col = 0;
        }
        if (col != 0) { write(STDOUT_FILENO, "  ", 2); col += 2; }
        write(STDOUT_FILENO, token, (size_t)len);
        col += len;
    }
    write(STDOUT_FILENO, "\n", 1);
}

static char prompt_promotion_piece(void) {
    // expects raw mode
    const char *msg = "Promote to (q/r/b/n): ";
    write(STDOUT_FILENO, msg, strlen(msg));
    for (;;) {
        int k = read_key();
        if (k < 0) continue;
        char c = (char)tolower(k);
        if (c == 'q' || c == 'r' || c == 'b' || c == 'n') {
            write(STDOUT_FILENO, "\n", 1);
            return c;
        }
        if (k == 27) { write(STDOUT_FILENO, "\n", 1); return 'q'; }
    }
}

/* ---------------- main game ---------------- */

int main(int argc, char **argv) {
    signal(SIGINT, on_sigint);
    setvbuf(stdout, NULL, _IONBF, 0);
    enable_raw_mode();

    Engine eng;
    bool vs_engine = false;
    const char *engine_path = NULL;
    int engine_color = BLACK;

    if (argc >= 2) {
        engine_path = argv[1];
        if (!engine_start(&eng, engine_path)) {
            disable_raw_mode();
            fprintf(stderr, "Failed to start/handshake UCI engine: %s\n", engine_path);
            return 1;
        }
        vs_engine = true;
    } else {
        memset(&eng, 0, sizeof(eng));
        eng.ok = false;
        engine_path = "(none)";
    }

    State s;
    state_set_startpos(&s);

    State hist[MAX_PLY];
    char uci_hist[MAX_PLY][6];
    char san_hist[MAX_PLY][32];
    int ply = 0;

    int cursor = sq_of(4, 1); // e2
    int selected = -1;
    Move selected_moves[MAX_MOVES];
    int selected_n = 0;

    char status_line[256] = "";

    for (;;) {
        // If selection active, regenerate legal moves for that piece
        selected_n = 0;
        if (selected >= 0) {
            Move leg[MAX_MOVES]; int ln = 0;
            gen_legal_moves(&s, leg, &ln);
            for (int i = 0; i < ln; i++) {
                if ((int)leg[i].from == selected) {
                    selected_moves[selected_n++] = leg[i];
                }
            }
            if (selected_n == 0) selected = -1; // no legal moves
        }

        draw(&s, cursor, selected, selected_moves, selected_n,
             san_hist, ply, vs_engine, engine_path, status_line);
        status_line[0] = 0;

        // Game end? still allow undo/reset/quit
        bool game_over = false;
        bool chk = in_check(&s, s.side);
        bool any = any_legal_moves(&s);
        if (!any) game_over = true;

        // Engine move if it's engine's turn and game not over
        if (vs_engine && eng.ok && s.side == engine_color && !game_over) {
            // Build "position startpos moves ..."
            char pos[8192];
            size_t poslen = 0;
            poslen += (size_t)snprintf(pos + poslen, sizeof(pos) - poslen, "position startpos");
            if (ply > 0) {
                poslen += (size_t)snprintf(pos + poslen, sizeof(pos) - poslen, " moves");
                for (int i = 0; i < ply; i++) {
                    poslen += (size_t)snprintf(pos + poslen, sizeof(pos) - poslen, " %s", uci_hist[i]);
                    if (poslen + 16 >= sizeof(pos)) break;
                }
            }
            poslen += (size_t)snprintf(pos + poslen, sizeof(pos) - poslen, "\n");

            draw(&s, cursor, selected, selected_moves, selected_n,
                 san_hist, ply, vs_engine, engine_path, "Engine thinking...");
            char bm[16];
            if (!engine_bestmove(&eng, pos, 500, bm)) {
                snprintf(status_line, sizeof(status_line), "Engine error or timeout.");
                continue;
            }
            if (strcmp(bm, "(none)") == 0) {
                snprintf(status_line, sizeof(status_line), "Engine reports no move.");
                continue;
            }
            if (strlen(bm) < 4) {
                snprintf(status_line, sizeof(status_line), "Bad engine bestmove: %s", bm);
                continue;
            }
            int from = alg_to_sq(bm[0], bm[1]);
            int to   = alg_to_sq(bm[2], bm[3]);
            char prom = 0;
            if (strlen(bm) >= 5) prom = bm[4];

            Move leg[MAX_MOVES]; int ln = 0;
            gen_legal_moves(&s, leg, &ln);

            int found = -1;
            for (int i = 0; i < ln; i++) {
                if (moves_equal_uci(&leg[i], from, to, prom)) { found = i; break; }
            }
            if (found < 0) {
                snprintf(status_line, sizeof(status_line), "Engine played illegal move: %s", bm);
                continue;
            }

            // record history and SAN
            if (ply < MAX_PLY) {
                hist[ply] = s;
                char san[32];
                gen_san_for_move(&s, &leg[found], san);
                strncpy(san_hist[ply], san, sizeof(san_hist[ply]) - 1);
                san_hist[ply][sizeof(san_hist[ply]) - 1] = 0;

                // store UCI
                char uci[6];
                move_to_uci(&s, &leg[found], uci);
                strncpy(uci_hist[ply], uci, sizeof(uci_hist[ply]) - 1);
                uci_hist[ply][sizeof(uci_hist[ply]) - 1] = 0;

                apply_move(&s, &leg[found]);
                ply++;
                selected = -1;
            }
            continue;
        }

        // User input
        int k = read_key();
        if (k < 0) continue;

        if (k == 'q' || k == 'Q') break;

        if (k == 'r' || k == 'R') {
            state_set_startpos(&s);
            ply = 0;
            selected = -1;
            cursor = sq_of(4, 1);
            snprintf(status_line, sizeof(status_line), "Reset.");
            if (vs_engine && eng.ok) {
                engine_write_line(&eng, "ucinewgame\n");
                engine_write_line(&eng, "isready\n");
                engine_wait_token(&eng, "readyok", 3000);
            }
            continue;
        }

        if (k == 'u' || k == 'U') {
            int undo_count = (k == 'U') ? 2 : 1;
            while (undo_count-- > 0) {
                if (ply <= 0) { term_beep(); break; }
                ply--;
                s = hist[ply];
                selected = -1;
            }
            continue;
        }

        // movement keys
        if (k == 1001 || k == 'k') { // up
            int f = file_of(cursor), r = rank_of(cursor);
            if (r < 7) cursor = sq_of(f, r + 1);
            continue;
        }
        if (k == 1002 || k == 'j') { // down
            int f = file_of(cursor), r = rank_of(cursor);
            if (r > 0) cursor = sq_of(f, r - 1);
            continue;
        }
        if (k == 1003 || k == 'l') { // right
            int f = file_of(cursor), r = rank_of(cursor);
            if (f < 7) cursor = sq_of(f + 1, r);
            continue;
        }
        if (k == 1004 || k == 'h') { // left
            int f = file_of(cursor), r = rank_of(cursor);
            if (f > 0) cursor = sq_of(f - 1, r);
            continue;
        }

        if (k == 27) { // ESC
            selected = -1;
            continue;
        }

        bool select_or_move = (k == ' ' || k == 13);
        if (!select_or_move) continue;

        // If game over, don't allow new moves (but undo/reset ok)
        if (game_over) { term_beep(); continue; }

        // If vs engine and it's engine's side to move, ignore user move
        if (vs_engine && s.side == engine_color) { term_beep(); continue; }

        if (selected < 0) {
            char p = s.b[cursor];
            if (p == '.' || piece_color(p) != s.side) {
                term_beep();
                continue;
            }
            selected = cursor;
            continue;
        } else {
            // attempt move selected -> cursor
            if (cursor == selected) { selected = -1; continue; }

            Move leg[MAX_MOVES]; int ln = 0;
            gen_legal_moves(&s, leg, &ln);

            int found = -1;
            for (int i = 0; i < ln; i++) {
                if ((int)leg[i].from == selected && (int)leg[i].to == cursor) {
                    found = i;
                    break;
                }
            }
            if (found < 0) {
                term_beep();
                continue;
            }

            // If promotion, ask user choice
            Move m = leg[found];
            if (m.flags & MF_PROMO) {
                char pr = prompt_promotion_piece();
                m.prom = pr;
            }

            if (ply >= MAX_PLY) {
                snprintf(status_line, sizeof(status_line), "Move limit reached.");
                continue;
            }

            hist[ply] = s;

            char san[32];
            gen_san_for_move(&s, &m, san);
            strncpy(san_hist[ply], san, sizeof(san_hist[ply]) - 1);
            san_hist[ply][sizeof(san_hist[ply]) - 1] = 0;

            char uci[6];
            move_to_uci(&s, &m, uci);
            strncpy(uci_hist[ply], uci, sizeof(uci_hist[ply]) - 1);
            uci_hist[ply][sizeof(uci_hist[ply]) - 1] = 0;

            apply_move(&s, &m);
            ply++;
            selected = -1;
            continue;
        }
    }

    if (vs_engine) engine_close(&eng);
    disable_raw_mode();

    // Print final PGN line (SAN sequence)
    printf("\nFinal PGN (SAN):\n");
    for (int i = 0; i < ply; i++) {
        if ((i % 2) == 0) printf("%d. %s", (i / 2) + 1, san_hist[i]);
        else printf(" %s", san_hist[i]);
        if ((i % 2) == 1) printf(" ");
    }
    printf("\n");
    return 0;
}
