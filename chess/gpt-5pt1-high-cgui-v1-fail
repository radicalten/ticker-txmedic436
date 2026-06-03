// uci_tui_chess.c - single-file terminal chess UI that speaks UCI (macOS-friendly)
// Build: cc -O2 -Wall -Wextra -std=c11 uci_tui_chess.c -o chess
// Run:   ./chess [engine] [engine_args...]
//
// Notes:
// - Text UI (ASCII board) in terminal.
// - Uses UCI move format: e2e4, g1f3, e7e8q, etc.
// - Pseudo-legal move checking (doesn't fully check "king in check").
// - Maintains position from startpos by applying moves; sends "position startpos moves ..."
// - Works well with Stockfish.

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

// -------------------- Utilities --------------------

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void rstrip(char *s) {
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || isspace((unsigned char)s[n-1])))
        s[--n] = 0;
}

static char *lskip_ws(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static void str_tolower_inplace(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

// -------------------- UCI Engine (bidirectional pipes) --------------------

typedef struct {
    pid_t pid;
    FILE *to_engine;   // parent's write -> engine stdin
    FILE *from_engine; // parent's read  <- engine stdout+stderr
} UCIEngine;

static void uci_sendf(UCIEngine *e, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(e->to_engine, fmt, ap);
    va_end(ap);
    fputc('\n', e->to_engine);
    fflush(e->to_engine);
}

static bool uci_readline(UCIEngine *e, char *buf, size_t bufsz) {
    if (!fgets(buf, (int)bufsz, e->from_engine)) return false;
    rstrip(buf);
    return true;
}

static void uci_drain_until(UCIEngine *e, const char *prefix, bool print_info) {
    char line[4096];
    while (uci_readline(e, line, sizeof(line))) {
        if (print_info) {
            if (starts_with(line, "info ")) {
                fprintf(stderr, "%s\n", line);
            }
        }
        if (starts_with(line, prefix)) return;
    }
    die("Engine terminated while waiting for: %s", prefix);
}

static void uci_isready(UCIEngine *e) {
    uci_sendf(e, "isready");
    uci_drain_until(e, "readyok", false);
}

static void uci_handshake(UCIEngine *e) {
    uci_sendf(e, "uci");
    uci_drain_until(e, "uciok", false);
    uci_isready(e);
}

static void uci_ucinewgame(UCIEngine *e) {
    uci_sendf(e, "ucinewgame");
    uci_isready(e);
}

static void uci_start(UCIEngine *e, int argc, char **argv) {
    int to_child[2];
    int from_child[2];

    if (pipe(to_child) != 0) die("pipe to_child failed: %s", strerror(errno));
    if (pipe(from_child) != 0) die("pipe from_child failed: %s", strerror(errno));

    pid_t pid = fork();
    if (pid < 0) die("fork failed: %s", strerror(errno));

    if (pid == 0) {
        // child
        if (dup2(to_child[0], STDIN_FILENO) < 0) _exit(127);
        if (dup2(from_child[1], STDOUT_FILENO) < 0) _exit(127);
        if (dup2(from_child[1], STDERR_FILENO) < 0) _exit(127);

        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);

        execvp(argv[0], argv);
        _exit(127);
    }

    // parent
    e->pid = pid;

    close(to_child[0]);
    close(from_child[1]);

    e->to_engine = fdopen(to_child[1], "w");
    e->from_engine = fdopen(from_child[0], "r");
    if (!e->to_engine || !e->from_engine) die("fdopen failed");

    setvbuf(e->to_engine, NULL, _IOLBF, 0);

    // Perform UCI handshake
    uci_handshake(e);
    uci_ucinewgame(e);
}

static void uci_stop(UCIEngine *e) {
    if (!e || e->pid <= 0) return;
    // Try polite quit
    if (e->to_engine) {
        uci_sendf(e, "quit");
    }
    if (e->to_engine) fclose(e->to_engine);
    if (e->from_engine) fclose(e->from_engine);

    int status = 0;
    waitpid(e->pid, &status, 0);
    e->pid = -1;
}

// -------------------- Chess position (startpos only) --------------------

enum {
    CASTLE_WK = 1 << 0,
    CASTLE_WQ = 1 << 1,
    CASTLE_BK = 1 << 2,
    CASTLE_BQ = 1 << 3
};

typedef struct {
    char b[64];         // a1=0 ... h8=63
    bool white_to_move;
    unsigned castle;    // bits above
    int ep;             // en-passant target square index or -1
    int fullmove;       // starts at 1
} Position;

typedef struct {
    int from, to;       // 0..63
    char promo;         // 'q','r','b','n' or 0
} Move;

static int sq_of(char file, char rank) {
    int f = file - 'a';
    int r = rank - '1';
    if (f < 0 || f > 7 || r < 0 || r > 7) return -1;
    return r * 8 + f;
}

static void sq_to_str(int sq, char out[3]) {
    int f = sq % 8;
    int r = sq / 8;
    out[0] = (char)('a' + f);
    out[1] = (char)('1' + r);
    out[2] = 0;
}

static bool is_white_piece(char p) { return (p >= 'A' && p <= 'Z'); }
static bool is_black_piece(char p) { return (p >= 'a' && p <= 'z'); }
static bool is_empty(char p) { return p == '.'; }

static void pos_set_start(Position *p) {
    static const char *start =
        "RNBQKBNR"
        "PPPPPPPP"
        "........"
        "........"
        "........"
        "........"
        "pppppppp"
        "rnbqkbnr";
    // The above is ranks 1..8? We want a1..h8.
    // Let's build explicitly to avoid confusion:
    // rank1: RNBQKBNR (a1..h1)
    // rank2: PPPPPPPP
    // rank3: ........
    // rank4: ........
    // rank5: ........
    // rank6: ........
    // rank7: pppppppp
    // rank8: rnbqkbnr
    // That matches a1..h8 in row-major (rank1 first).
    memcpy(p->b, start, 64);
    p->white_to_move = true;
    p->castle = CASTLE_WK | CASTLE_WQ | CASTLE_BK | CASTLE_BQ;
    p->ep = -1;
    p->fullmove = 1;
}

static void print_castle(unsigned c, char out[5]) {
    int k = 0;
    if (c & CASTLE_WK) out[k++] = 'K';
    if (c & CASTLE_WQ) out[k++] = 'Q';
    if (c & CASTLE_BK) out[k++] = 'k';
    if (c & CASTLE_BQ) out[k++] = 'q';
    if (k == 0) out[k++] = '-';
    out[k] = 0;
}

static void pos_print(const Position *p, bool flipped) {
    char cbuf[8], epbuf[3];
    print_castle(p->castle, cbuf);
    if (p->ep >= 0) sq_to_str(p->ep, epbuf); else strcpy(epbuf, "-");

    printf("\n");
    printf("Side: %s | Castling: %s | EP: %s | Fullmove: %d\n",
           p->white_to_move ? "White" : "Black", cbuf, epbuf, p->fullmove);

    // files label
    if (!flipped) {
        printf("    a b c d e f g h\n");
        for (int r = 7; r >= 0; r--) {
            printf(" %d  ", r + 1);
            for (int f = 0; f < 8; f++) {
                char pc = p->b[r * 8 + f];
                printf("%c", pc);
                if (f != 7) printf(" ");
            }
            printf("  %d\n", r + 1);
        }
        printf("    a b c d e f g h\n");
    } else {
        printf("    h g f e d c b a\n");
        for (int r = 0; r < 8; r++) {
            printf(" %d  ", r + 1);
            for (int f = 7; f >= 0; f--) {
                char pc = p->b[r * 8 + f];
                printf("%c", pc);
                if (f != 0) printf(" ");
            }
            printf("  %d\n", r + 1);
        }
        printf("    h g f e d c b a\n");
    }
    printf("\n");
}

static bool parse_uci_move(const char *s, Move *m) {
    // Accept "e2e4" or "e7e8q"
    if (!s) return false;
    size_t n = strlen(s);
    if (n < 4) return false;

    char f1 = (char)tolower((unsigned char)s[0]);
    char r1 = s[1];
    char f2 = (char)tolower((unsigned char)s[2]);
    char r2 = s[3];

    int from = sq_of(f1, r1);
    int to   = sq_of(f2, r2);
    if (from < 0 || to < 0) return false;

    m->from = from;
    m->to = to;
    m->promo = 0;

    if (n >= 5) {
        char pr = (char)tolower((unsigned char)s[4]);
        if (pr=='q' || pr=='r' || pr=='b' || pr=='n') m->promo = pr;
        else return false;
    }
    return true;
}

static bool in_bounds(int f, int r) {
    return f >= 0 && f < 8 && r >= 0 && r < 8;
}

static bool clear_ray(const Position *p, int from, int to, int df, int dr) {
    int f1 = from % 8, r1 = from / 8;
    int f2 = to % 8, r2 = to / 8;

    int f = f1 + df, r = r1 + dr;
    while (in_bounds(f, r)) {
        int sq = r * 8 + f;
        if (sq == to) return true;
        if (!is_empty(p->b[sq])) return false;
        f += df; r += dr;
    }
    return false;
}

static bool is_pseudo_legal(const Position *p, const Move *m) {
    if (m->from < 0 || m->from > 63 || m->to < 0 || m->to > 63) return false;
    if (m->from == m->to) return false;

    char pc = p->b[m->from];
    char dst = p->b[m->to];
    if (is_empty(pc)) return false;

    bool white = p->white_to_move;
    if (white && !is_white_piece(pc)) return false;
    if (!white && !is_black_piece(pc)) return false;

    if (white && is_white_piece(dst)) return false;
    if (!white && is_black_piece(dst)) return false;

    int f1 = m->from % 8, r1 = m->from / 8;
    int f2 = m->to % 8,   r2 = m->to / 8;
    int df = f2 - f1;
    int dr = r2 - r1;

    char up = (char)toupper((unsigned char)pc);

    if (up == 'P') {
        int dir = white ? 1 : -1;
        int start_rank = white ? 1 : 6;
        int promo_rank = white ? 7 : 0;

        bool is_capture = (df == 1 || df == -1) && (dr == dir);
        bool is_push1   = (df == 0) && (dr == dir);
        bool is_push2   = (df == 0) && (dr == 2*dir) && (r1 == start_rank);

        // Promotion requirement for pawn reaching last rank
        if (r2 == promo_rank) {
            if (!(m->promo=='q' || m->promo=='r' || m->promo=='b' || m->promo=='n')) {
                return false; // require explicit promotion
            }
        } else {
            if (m->promo) return false;
        }

        if (is_push1) {
            if (!is_empty(dst)) return false;
            return true;
        }
        if (is_push2) {
            int mid = (r1 + dir) * 8 + f1;
            if (!is_empty(dst)) return false;
            if (!is_empty(p->b[mid])) return false;
            return true;
        }
        if (is_capture) {
            if (!is_empty(dst)) {
                // normal capture
                return true;
            }
            // en passant capture: to square empty but equals ep target
            if (p->ep == m->to) return true;
            return false;
        }
        return false;
    }

    if (m->promo) return false; // only pawns promote

    if (up == 'N') {
        int adf = df < 0 ? -df : df;
        int adr = dr < 0 ? -dr : dr;
        return (adf == 1 && adr == 2) || (adf == 2 && adr == 1);
    }

    if (up == 'B') {
        int adf = df < 0 ? -df : df;
        int adr = dr < 0 ? -dr : dr;
        if (adf != adr) return false;
        int sgnf = (df > 0) ? 1 : -1;
        int sgnr = (dr > 0) ? 1 : -1;
        return clear_ray(p, m->from, m->to, sgnf, sgnr);
    }

    if (up == 'R') {
        if (df != 0 && dr != 0) return false;
        int sgnf = (df == 0) ? 0 : (df > 0 ? 1 : -1);
        int sgnr = (dr == 0) ? 0 : (dr > 0 ? 1 : -1);
        return clear_ray(p, m->from, m->to, sgnf, sgnr);
    }

    if (up == 'Q') {
        int adf = df < 0 ? -df : df;
        int adr = dr < 0 ? -dr : dr;
        if (df == 0 || dr == 0) {
            int sgnf = (df == 0) ? 0 : (df > 0 ? 1 : -1);
            int sgnr = (dr == 0) ? 0 : (dr > 0 ? 1 : -1);
            return clear_ray(p, m->from, m->to, sgnf, sgnr);
        }
        if (adf == adr) {
            int sgnf = (df > 0) ? 1 : -1;
            int sgnr = (dr > 0) ? 1 : -1;
            return clear_ray(p, m->from, m->to, sgnf, sgnr);
        }
        return false;
    }

    if (up == 'K') {
        int adf = df < 0 ? -df : df;
        int adr = dr < 0 ? -dr : dr;
        if (adf <= 1 && adr <= 1) return true;

        // Castling (ignore check conditions; only basic path empty + rights + correct squares)
        if (white && m->from == sq_of('e','1') && r2 == 0) {
            if (m->to == sq_of('g','1')) { // O-O
                if (!(p->castle & CASTLE_WK)) return false;
                if (!is_empty(p->b[sq_of('f','1')]) || !is_empty(p->b[sq_of('g','1')])) return false;
                return true;
            }
            if (m->to == sq_of('c','1')) { // O-O-O
                if (!(p->castle & CASTLE_WQ)) return false;
                if (!is_empty(p->b[sq_of('d','1')]) || !is_empty(p->b[sq_of('c','1')]) || !is_empty(p->b[sq_of('b','1')])) return false;
                return true;
            }
        }
        if (!white && m->from == sq_of('e','8') && r2 == 7) {
            if (m->to == sq_of('g','8')) {
                if (!(p->castle & CASTLE_BK)) return false;
                if (!is_empty(p->b[sq_of('f','8')]) || !is_empty(p->b[sq_of('g','8')])) return false;
                return true;
            }
            if (m->to == sq_of('c','8')) {
                if (!(p->castle & CASTLE_BQ)) return false;
                if (!is_empty(p->b[sq_of('d','8')]) || !is_empty(p->b[sq_of('c','8')]) || !is_empty(p->b[sq_of('b','8')])) return false;
                return true;
            }
        }
        return false;
    }

    return false;
}

static void update_castle_rights_for_move(Position *p, int from, int to, char moved, char captured) {
    (void)captured;
    // Remove rights if king moves
    char up = (char)toupper((unsigned char)moved);
    if (up == 'K') {
        if (is_white_piece(moved)) p->castle &= ~(CASTLE_WK | CASTLE_WQ);
        else p->castle &= ~(CASTLE_BK | CASTLE_BQ);
    }

    // Remove rights if rook moves from original squares
    if (up == 'R') {
        if (from == sq_of('h','1')) p->castle &= ~CASTLE_WK;
        if (from == sq_of('a','1')) p->castle &= ~CASTLE_WQ;
        if (from == sq_of('h','8')) p->castle &= ~CASTLE_BK;
        if (from == sq_of('a','8')) p->castle &= ~CASTLE_BQ;
    }

    // Remove rights if rook is captured on original squares
    if (to == sq_of('h','1') && captured == 'R') p->castle &= ~CASTLE_WK;
    if (to == sq_of('a','1') && captured == 'R') p->castle &= ~CASTLE_WQ;
    if (to == sq_of('h','8') && captured == 'r') p->castle &= ~CASTLE_BK;
    if (to == sq_of('a','8') && captured == 'r') p->castle &= ~CASTLE_BQ;
}

static void apply_move(Position *p, const Move *m) {
    char moved = p->b[m->from];
    char captured = p->b[m->to];

    // Reset EP by default; set again on double pawn push
    int old_ep = p->ep;
    p->ep = -1;

    update_castle_rights_for_move(p, m->from, m->to, moved, captured);

    // En passant capture
    char up = (char)toupper((unsigned char)moved);
    if (up == 'P') {
        int f1 = m->from % 8, r1 = m->from / 8;
        int f2 = m->to % 8,   r2 = m->to / 8;

        // If moving diagonally to empty square and matches old ep -> capture pawn behind
        if (captured == '.' && f1 != f2 && m->to == old_ep) {
            int dir = is_white_piece(moved) ? -1 : 1; // captured pawn is one rank behind target
            int cap_sq = (r2 + dir) * 8 + f2;
            p->b[cap_sq] = '.';
        }

        // Double push sets EP target square
        if (f1 == f2 && (r2 - r1 == 2 || r2 - r1 == -2)) {
            int midr = (r1 + r2) / 2;
            p->ep = midr * 8 + f1;
        }
    }

    // Castling rook move
    if (up == 'K') {
        if (m->from == sq_of('e','1') && m->to == sq_of('g','1')) {
            // white O-O: rook h1 -> f1
            p->b[sq_of('h','1')] = '.';
            p->b[sq_of('f','1')] = 'R';
        } else if (m->from == sq_of('e','1') && m->to == sq_of('c','1')) {
            // white O-O-O: rook a1 -> d1
            p->b[sq_of('a','1')] = '.';
            p->b[sq_of('d','1')] = 'R';
        } else if (m->from == sq_of('e','8') && m->to == sq_of('g','8')) {
            // black O-O
            p->b[sq_of('h','8')] = '.';
            p->b[sq_of('f','8')] = 'r';
        } else if (m->from == sq_of('e','8') && m->to == sq_of('c','8')) {
            // black O-O-O
            p->b[sq_of('a','8')] = '.';
            p->b[sq_of('d','8')] = 'r';
        }
    }

    // Move piece
    p->b[m->from] = '.';
    p->b[m->to] = moved;

    // Promotion
    if (up == 'P' && m->promo) {
        char pr = m->promo;
        if (is_white_piece(moved)) pr = (char)toupper((unsigned char)pr);
        p->b[m->to] = pr;
    }

    // Side to move / fullmove
    bool was_white = p->white_to_move;
    p->white_to_move = !p->white_to_move;
    if (!was_white) p->fullmove++;
}

// -------------------- Game driver --------------------

#define MAX_PLIES 2048

typedef struct {
    Position hist_pos[MAX_PLIES];
    int hist_len;

    char moves[MAX_PLIES][8]; // UCI strings, e.g. "e2e4", "e7e8q"
    int move_len;
} Game;

static void game_reset(Game *g, Position *p) {
    g->hist_len = 0;
    g->move_len = 0;
    pos_set_start(p);
}

static void game_push_undo(Game *g, const Position *p) {
    if (g->hist_len >= MAX_PLIES) die("History overflow");
    g->hist_pos[g->hist_len++] = *p;
}

static bool game_undo(Game *g, Position *p) {
    if (g->hist_len <= 0 || g->move_len <= 0) return false;
    *p = g->hist_pos[--g->hist_len];
    g->move_len--;
    return true;
}

static void game_add_move(Game *g, const char *uci) {
    if (g->move_len >= MAX_PLIES) die("Move list overflow");
    snprintf(g->moves[g->move_len], sizeof(g->moves[g->move_len]), "%s", uci);
    g->move_len++;
}

static void uci_send_position(UCIEngine *e, const Game *g) {
    // position startpos moves ...
    // Build in chunks (avoid huge single snprintf).
    uci_sendf(e, "position startpos%s", (g->move_len == 0 ? "" : " moves"));
    if (g->move_len > 0) {
        // Some engines accept continuing on same line, but safest is one line.
        // We'll actually send the full "position ..." line in one go:
        // Rebuild properly:
        // (Do it with a dynamic buffer.)
        size_t cap = 32 + (size_t)g->move_len * 8;
        char *buf = (char*)malloc(cap);
        if (!buf) die("malloc failed");

        size_t n = 0;
        n += (size_t)snprintf(buf + n, cap - n, "position startpos moves");
        for (int i = 0; i < g->move_len; i++) {
            n += (size_t)snprintf(buf + n, cap - n, " %s", g->moves[i]);
        }
        uci_sendf(e, "%s", buf);
        free(buf);
    }
}

static bool uci_go_bestmove(UCIEngine *e, const Game *g, int depth, int movetime_ms, bool print_info,
                           char out_bestmove[16]) {
    // Send position + go command, then read until bestmove.
    uci_send_position(e, g);
    if (movetime_ms > 0) uci_sendf(e, "go movetime %d", movetime_ms);
    else uci_sendf(e, "go depth %d", depth);

    char line[4096];
    while (uci_readline(e, line, sizeof(line))) {
        if (print_info && starts_with(line, "info ")) {
            fprintf(stderr, "%s\n", line);
        }
        if (starts_with(line, "bestmove ")) {
            const char *bm = line + strlen("bestmove ");
            while (*bm && isspace((unsigned char)*bm)) bm++;
            // bm might be "e2e4" or "0000" or "(none)"
            snprintf(out_bestmove, 16, "%s", bm);
            // truncate at space
            for (int i = 0; out_bestmove[i]; i++) {
                if (isspace((unsigned char)out_bestmove[i])) { out_bestmove[i] = 0; break; }
            }
            return true;
        }
    }
    return false;
}

static void print_help(void) {
    printf(
        "Commands:\n"
        "  e2e4 / g1f3 / e7e8q    Make a move (UCI format; promotion required)\n"
        "  go                     Let engine play side-to-move once\n"
        "  engine w|b|off         Engine controls White, Black, or Off\n"
        "  depth N                Set engine depth (default 12)\n"
        "  time MS                Set engine movetime in ms (0 disables, uses depth)\n"
        "  undo                   Undo one ply\n"
        "  new                    New game\n"
        "  flip                   Flip board view\n"
        "  info on|off             Print engine 'info' lines during search\n"
        "  help                   Show this help\n"
        "  quit                   Exit\n"
    );
}

static bool looks_like_uci_move(const char *s) {
    // Simple check: a-h 1-8 a-h 1-8 [qrbn]?
    size_t n = strlen(s);
    if (n != 4 && n != 5) return false;
    char a = (char)tolower((unsigned char)s[0]);
    char b = s[1];
    char c = (char)tolower((unsigned char)s[2]);
    char d = s[3];
    if (!(a >= 'a' && a <= 'h')) return false;
    if (!(b >= '1' && b <= '8')) return false;
    if (!(c >= 'a' && c <= 'h')) return false;
    if (!(d >= '1' && d <= '8')) return false;
    if (n == 5) {
        char e = (char)tolower((unsigned char)s[4]);
        if (!(e=='q' || e=='r' || e=='b' || e=='n')) return false;
    }
    return true;
}

int main(int argc, char **argv) {
    // Engine argv: default "stockfish"
    char *default_engine_argv[] = { (char*)"stockfish", NULL };
    char **engine_argv = (argc >= 2) ? &argv[1] : default_engine_argv;

    UCIEngine eng = {0};
    uci_start(&eng, (argc >= 2) ? (argc - 1) : 1, engine_argv);

    Game g = {0};
    Position pos;
    game_reset(&g, &pos);

    bool flipped = false;
    bool print_info = false;

    // Engine control: 'w', 'b', or 0 (off)
    char engine_side = 'b'; // default: play vs engine as White

    int depth = 12;
    int movetime_ms = 0;

    printf("Terminal UCI Chess (single-file C)\n");
    printf("Engine: %s\n", engine_argv[0]);
    printf("Type 'help' for commands.\n");

    char input[256];

    for (;;) {
        pos_print(&pos, flipped);

        bool engine_turn = (engine_side == (pos.white_to_move ? 'w' : 'b'));
        if (engine_side && engine_turn) {
            printf("Engine thinking (%s)...\n", pos.white_to_move ? "White" : "Black");
            char bm[16] = {0};
            if (!uci_go_bestmove(&eng, &g, depth, movetime_ms, print_info, bm)) {
                printf("Engine failed to provide bestmove.\n");
                break;
            }
            if (strcmp(bm, "0000") == 0 || strcmp(bm, "(none)") == 0) {
                printf("Engine reports no move (game over).\n");
                break;
            }

            Move m;
            if (!parse_uci_move(bm, &m) || !is_pseudo_legal(&pos, &m)) {
                printf("Engine produced an unexpected/illegal move for our position: %s\n", bm);
                break;
            }

            game_push_undo(&g, &pos);
            apply_move(&pos, &m);
            game_add_move(&g, bm);

            continue; // next ply
        }

        printf("%s> ", pos.white_to_move ? "White" : "Black");
        if (!fgets(input, sizeof(input), stdin)) break;
        rstrip(input);

        char *cmd = lskip_ws(input);
        if (!*cmd) continue;

        // normalize a copy for command parsing, but keep original for move strings
        char cmd_lc[256];
        snprintf(cmd_lc, sizeof(cmd_lc), "%s", cmd);
        str_tolower_inplace(cmd_lc);

        if (strcmp(cmd_lc, "quit") == 0 || strcmp(cmd_lc, "exit") == 0) {
            break;
        } else if (strcmp(cmd_lc, "help") == 0) {
            print_help();
            continue;
        } else if (strcmp(cmd_lc, "flip") == 0) {
            flipped = !flipped;
            continue;
        } else if (strcmp(cmd_lc, "undo") == 0) {
            if (!game_undo(&g, &pos)) printf("Nothing to undo.\n");
            continue;
        } else if (strcmp(cmd_lc, "new") == 0) {
            game_reset(&g, &pos);
            uci_ucinewgame(&eng);
            continue;
        } else if (starts_with(cmd_lc, "depth ")) {
            int n = atoi(cmd_lc + 6);
            if (n < 1) printf("Bad depth.\n");
            else { depth = n; printf("Depth set to %d\n", depth); }
            continue;
        } else if (starts_with(cmd_lc, "time ")) {
            int n = atoi(cmd_lc + 5);
            if (n < 0) printf("Bad time.\n");
            else { movetime_ms = n; printf("Movetime set to %d ms (0=disabled)\n", movetime_ms); }
            continue;
        } else if (starts_with(cmd_lc, "info ")) {
            char *arg = cmd_lc + 5;
            arg = lskip_ws(arg);
            if (strcmp(arg, "on") == 0) print_info = true;
            else if (strcmp(arg, "off") == 0) print_info = false;
            else printf("Usage: info on|off\n");
            continue;
        } else if (starts_with(cmd_lc, "engine ")) {
            char *arg = cmd_lc + 7;
            arg = lskip_ws(arg);
            if (strcmp(arg, "w") == 0 || strcmp(arg, "white") == 0) engine_side = 'w';
            else if (strcmp(arg, "b") == 0 || strcmp(arg, "black") == 0) engine_side = 'b';
            else if (strcmp(arg, "off") == 0) engine_side = 0;
            else printf("Usage: engine w|b|off\n");
            continue;
        } else if (strcmp(cmd_lc, "go") == 0) {
            char bm[16] = {0};
            if (!uci_go_bestmove(&eng, &g, depth, movetime_ms, print_info, bm)) {
                printf("Engine failed to provide bestmove.\n");
                continue;
            }
            if (strcmp(bm, "0000") == 0 || strcmp(bm, "(none)") == 0) {
                printf("Engine reports no move (game over).\n");
                continue;
            }
            Move m;
            if (!parse_uci_move(bm, &m) || !is_pseudo_legal(&pos, &m)) {
                printf("Engine produced an unexpected/illegal move for our position: %s\n", bm);
                continue;
            }
            game_push_undo(&g, &pos);
            apply_move(&pos, &m);
            game_add_move(&g, bm);
            continue;
        }

        // Otherwise treat as move
        if (!looks_like_uci_move(cmd)) {
            printf("Unknown command or bad move format. Type 'help'.\n");
            continue;
        }

        Move m;
        if (!parse_uci_move(cmd, &m)) {
            printf("Bad move format.\n");
            continue;
        }
        if (!is_pseudo_legal(&pos, &m)) {
            printf("Illegal (pseudo-legal) move for this side/position.\n");
            continue;
        }

        // Apply
        game_push_undo(&g, &pos);
        apply_move(&pos, &m);

        // Store normalized UCI move (lowercase promotion)
        char store[8] = {0};
        snprintf(store, sizeof(store), "%c%c%c%c%c",
                 (char)tolower((unsigned char)cmd[0]), cmd[1],
                 (char)tolower((unsigned char)cmd[2]), cmd[3],
                 (m.promo ? (char)tolower((unsigned char)m.promo) : '\0'));
        if (!m.promo) store[4] = 0;
        game_add_move(&g, store);
    }

    uci_stop(&eng);
    printf("Bye.\n");
    return 0;
}
