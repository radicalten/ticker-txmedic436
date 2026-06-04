/*
termchess.c — single-file ANSI-terminal chess GUI (no ncurses, no deps)
- Runs in macOS Terminal (ANSI escape sequences + termios raw mode)
- Keyboard-driven piece selection/movement with cursor
- Legal-move enforcement + highlighting:
    * cursor square
    * selected piece square
    * legal destinations for selected piece
    * last move (from/to)
    * king square highlighted when in check
- UCI engine support (Stockfish etc):
    * launches engine as child process
    * sends "position startpos moves ..." and "go depth|nodes|movetime"
    * parses "bestmove ..."
- Undo (takeback)
- Recent moves shown as PGN/SAN (with check/mate suffix)
- Change time control mode: depth / nodes / time (movetime)

Build (macOS):
  clang -O2 -std=c11 -Wall -Wextra -pedantic termchess.c -o termchess

Run:
  ./termchess                 (two-player local)
  ./termchess stockfish       (engine vs human; engine defaults to Black)
  ./termchess /path/to/engine

Keys:
  Arrow keys / WASD : move cursor
  Enter / Space     : select piece / confirm move
  Esc               : cancel selection
  u                 : undo one ply
  U                 : undo two plies
  r                 : reset to start position
  e                 : cycle engine control: Off -> Black -> White -> Off (if engine loaded)
  t                 : cycle time control mode (depth/nodes/time)
  + / -             : adjust current time control value
  T                 : set time control value (typed number + Enter)
  q                 : quit

Notes:
- Uses SAN for the move list (PGN move text). It’s “good enough” for typical play:
  includes captures, disambiguation, promotions, check/mate, castling.
*/

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

enum { WHITE = 0, BLACK = 1 };

enum Piece {
  EMPTY = 0,
  WP, WN, WB, WR, WQ, WK,
  BP, BN, BB, BR, BQ, BK
};

static inline int piece_color(int p) {
  if (p >= WP && p <= WK) return WHITE;
  if (p >= BP && p <= BK) return BLACK;
  return -1;
}
static inline int piece_type(int p) {
  if (p == EMPTY) return 0;
  int t = p % 6;
  return t ? t : 6; // 1..6 = pawn..king
}
static inline bool is_white(int p) { return p >= WP && p <= WK; }
static inline bool is_black(int p) { return p >= BP && p <= BK; }

static inline int file_of(int sq) { return sq & 7; }
static inline int rank_of(int sq) { return sq >> 3; }
static inline int sq_of(int f, int r) { return (r << 3) | f; }

static inline char file_char(int sq) { return (char)('a' + file_of(sq)); }
static inline char rank_char(int sq) { return (char)('1' + rank_of(sq)); }

static void sq_to_alg(int sq, char out[3]) {
  out[0] = file_char(sq);
  out[1] = rank_char(sq);
  out[2] = 0;
}

/* ------------------------------ Terminal raw mode ------------------------------ */

static struct termios g_orig_termios;
static bool g_raw_enabled = false;

static void term_disable_raw(void) {
  if (g_raw_enabled) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
    g_raw_enabled = false;
  }
  // leave alt screen, show cursor, reset attrs
  write(STDOUT_FILENO, "\x1b[0m\x1b[?25h\x1b[?1049l", 20);
}

static void term_enable_raw(void) {
  if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) {
    perror("tcgetattr");
    exit(1);
  }
  atexit(term_disable_raw);

  struct termios raw = g_orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    perror("tcsetattr");
    exit(1);
  }
  g_raw_enabled = true;

  // alt screen + hide cursor
  write(STDOUT_FILENO, "\x1b[?1049h\x1b[?25l", 16);
}

enum Key {
  KEY_NONE = 0,
  KEY_ESC = 27,
  KEY_ENTER = 13,
  KEY_BACKSPACE = 127,

  KEY_UP = 1000,
  KEY_DOWN,
  KEY_LEFT,
  KEY_RIGHT,
};

static int read_key(void) {
  unsigned char c;
  int n = (int)read(STDIN_FILENO, &c, 1);
  if (n <= 0) return KEY_NONE;

  if (c == '\r') return KEY_ENTER;
  if (c == 27) { // ESC or escape sequence
    unsigned char seq[2];
    int n1 = (int)read(STDIN_FILENO, &seq[0], 1);
    int n2 = (int)read(STDIN_FILENO, &seq[1], 1);
    if (n1 == 1 && n2 == 1 && seq[0] == '[') {
      switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
      }
    }
    return KEY_ESC;
  }
  return (int)c;
}

/* ------------------------------ Chess core ------------------------------ */

enum MoveFlags {
  MF_CAPTURE = 1 << 0,
  MF_EP      = 1 << 1,
  MF_CASTLE  = 1 << 2,
  MF_PROMO   = 1 << 3
};

typedef struct {
  uint8_t from, to;
  uint8_t prom;      // promoted piece code or 0
  uint8_t flags;
  uint8_t captured;  // captured piece code or 0
  uint8_t prev_castle;
  int8_t  prev_ep;   // -1 or 0..63
  uint16_t prev_halfmove;
  uint16_t prev_fullmove;
  char uci[6];       // "e2e4" or "e7e8q"
  char san[16];      // SAN for move list
} Move;

typedef struct {
  int board[64];
  int side;          // WHITE/BLACK to move
  uint8_t castle;    // bits: 1=WK,2=WQ,4=BK,8=BQ
  int8_t ep;         // en passant target square or -1
  uint16_t halfmove;
  uint16_t fullmove;
} State;

static void state_set_startpos(State *s) {
  static const int start[64] = {
    WR, WN, WB, WQ, WK, WB, WN, WR,
    WP, WP, WP, WP, WP, WP, WP, WP,
    EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,
    EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,
    EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,
    EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,EMPTY,
    BP, BP, BP, BP, BP, BP, BP, BP,
    BR, BN, BB, BQ, BK, BB, BN, BR
  };
  // NOTE: above is a1..h8? Actually it lays a1 as WR, but standard start has white pieces on rank 1.
  // Our indexing is a1=0..h1=7, a8=56..h8=63.
  // So we must place white on ranks 1-2 and black on ranks 7-8. The array above wrongly puts black on ranks 7-8 but reversed.
  // Let's fill explicitly:

  for (int i = 0; i < 64; i++) s->board[i] = EMPTY;

  // White pieces
  s->board[sq_of(0,0)] = WR; s->board[sq_of(1,0)] = WN; s->board[sq_of(2,0)] = WB; s->board[sq_of(3,0)] = WQ;
  s->board[sq_of(4,0)] = WK; s->board[sq_of(5,0)] = WB; s->board[sq_of(6,0)] = WN; s->board[sq_of(7,0)] = WR;
  for (int f=0; f<8; f++) s->board[sq_of(f,1)] = WP;

  // Black pieces
  s->board[sq_of(0,7)] = BR; s->board[sq_of(1,7)] = BN; s->board[sq_of(2,7)] = BB; s->board[sq_of(3,7)] = BQ;
  s->board[sq_of(4,7)] = BK; s->board[sq_of(5,7)] = BB; s->board[sq_of(6,7)] = BN; s->board[sq_of(7,7)] = BR;
  for (int f=0; f<8; f++) s->board[sq_of(f,6)] = BP;

  s->side = WHITE;
  s->castle = 1|2|4|8;
  s->ep = -1;
  s->halfmove = 0;
  s->fullmove = 1;
  (void)start;
}

static int find_king(const State *s, int side) {
  int k = (side == WHITE) ? WK : BK;
  for (int i = 0; i < 64; i++) if (s->board[i] == k) return i;
  return -1;
}

static bool on_board(int sq) { return sq >= 0 && sq < 64; }

static bool is_attacked_by(const State *s, int sq, int by_side) {
  // Pawn attacks
  if (by_side == WHITE) {
    int a = sq - 7, b = sq - 9;
    if (file_of(sq) > 0 && on_board(a) && s->board[a] == WP) return true;
    if (file_of(sq) < 7 && on_board(b) && s->board[b] == WP) return true;
  } else {
    int a = sq + 7, b = sq + 9;
    if (file_of(sq) < 7 && on_board(a) && s->board[a] == BP) return true;
    if (file_of(sq) > 0 && on_board(b) && s->board[b] == BP) return true;
  }

  // Knight attacks
  static const int kn[8] = { 17, 15, 10, 6, -6, -10, -15, -17 };
  int n = (by_side == WHITE) ? WN : BN;
  for (int i=0;i<8;i++) {
    int t = sq + kn[i];
    if (!on_board(t)) continue;
    int df = abs(file_of(t) - file_of(sq));
    int dr = abs(rank_of(t) - rank_of(sq));
    if (!((df==1 && dr==2) || (df==2 && dr==1))) continue;
    if (s->board[t] == n) return true;
  }

  // King attacks
  int k = (by_side == WHITE) ? WK : BK;
  for (int dr=-1; dr<=1; dr++) for (int df=-1; df<=1; df++) {
    if (!df && !dr) continue;
    int f = file_of(sq) + df;
    int r = rank_of(sq) + dr;
    if (f<0||f>7||r<0||r>7) continue;
    if (s->board[sq_of(f,r)] == k) return true;
  }

  // Sliding attacks: bishop/rook/queen
  int b = (by_side == WHITE) ? WB : BB;
  int r = (by_side == WHITE) ? WR : BR;
  int q = (by_side == WHITE) ? WQ : BQ;

  // Diagonals
  static const int diag_dirs[4] = { 9, 7, -7, -9 };
  for (int d=0; d<4; d++) {
    int cur = sq;
    while (1) {
      int next = cur + diag_dirs[d];
      if (!on_board(next)) break;
      if (abs(file_of(next) - file_of(cur)) != 1) break;
      cur = next;
      int p = s->board[cur];
      if (p != EMPTY) {
        if (p == b || p == q) return true;
        break;
      }
    }
  }

  // Orthogonals
  static const int ortho_dirs[4] = { 8, -8, 1, -1 };
  for (int d=0; d<4; d++) {
    int cur = sq;
    while (1) {
      int next = cur + ortho_dirs[d];
      if (!on_board(next)) break;
      if ((ortho_dirs[d] == 1 || ortho_dirs[d] == -1) && rank_of(next) != rank_of(cur)) break;
      cur = next;
      int p = s->board[cur];
      if (p != EMPTY) {
        if (p == r || p == q) return true;
        break;
      }
    }
  }

  return false;
}

static bool in_check(const State *s, int side) {
  int ksq = find_king(s, side);
  if (ksq < 0) return false;
  return is_attacked_by(s, ksq, 1 - side);
}

static void move_to_uci(const Move *m, char out[6]) {
  out[0] = file_char(m->from);
  out[1] = rank_char(m->from);
  out[2] = file_char(m->to);
  out[3] = rank_char(m->to);
  if (m->flags & MF_PROMO) {
    char pc = 'q';
    int t = piece_type(m->prom);
    if (t == 2) pc = 'n';
    else if (t == 3) pc = 'b';
    else if (t == 4) pc = 'r';
    else pc = 'q';
    out[4] = pc;
    out[5] = 0;
  } else {
    out[4] = 0;
  }
}

static void apply_move(State *s, Move *m) {
  // Fill captured if not already set (pseudo-gen sets it, but keep safe)
  m->captured = 0;
  int p = s->board[m->from];
  int target = s->board[m->to];

  m->prev_castle = s->castle;
  m->prev_ep = s->ep;
  m->prev_halfmove = s->halfmove;
  m->prev_fullmove = s->fullmove;

  // reset ep by default
  s->ep = -1;

  // halfmove clock
  bool pawn_move = (piece_type(p) == 1);
  bool is_capture = (target != EMPTY);

  // En passant capture
  if (m->flags & MF_EP) {
    is_capture = true;
    int cap_sq = (s->side == WHITE) ? (m->to - 8) : (m->to + 8);
    m->captured = (uint8_t)s->board[cap_sq];
    s->board[cap_sq] = EMPTY;
  } else if (target != EMPTY) {
    m->captured = (uint8_t)target;
  }

  // Move piece
  s->board[m->to] = p;
  s->board[m->from] = EMPTY;

  // Promotion
  if (m->flags & MF_PROMO) {
    s->board[m->to] = m->prom;
  }

  // Castling rook move
  if (m->flags & MF_CASTLE) {
    // King moved e1->g1, e1->c1, e8->g8, e8->c8
    if (m->to == sq_of(6,0)) { // white O-O
      s->board[sq_of(5,0)] = WR;
      s->board[sq_of(7,0)] = EMPTY;
    } else if (m->to == sq_of(2,0)) { // white O-O-O
      s->board[sq_of(3,0)] = WR;
      s->board[sq_of(0,0)] = EMPTY;
    } else if (m->to == sq_of(6,7)) { // black O-O
      s->board[sq_of(5,7)] = BR;
      s->board[sq_of(7,7)] = EMPTY;
    } else if (m->to == sq_of(2,7)) { // black O-O-O
      s->board[sq_of(3,7)] = BR;
      s->board[sq_of(0,7)] = EMPTY;
    }
  }

  // Set ep square on pawn double
  if (piece_type(p) == 1) {
    int dr = rank_of(m->to) - rank_of(m->from);
    if (dr == 2 || dr == -2) {
      s->ep = (int8_t)((m->from + m->to) / 2);
    }
  }

  // Update castling rights
  // King moved
  if (p == WK) s->castle &= ~(1|2);
  if (p == BK) s->castle &= ~(4|8);
  // Rook moved
  if (p == WR && m->from == sq_of(0,0)) s->castle &= ~2;
  if (p == WR && m->from == sq_of(7,0)) s->castle &= ~1;
  if (p == BR && m->from == sq_of(0,7)) s->castle &= ~8;
  if (p == BR && m->from == sq_of(7,7)) s->castle &= ~4;
  // Rook captured
  if (m->captured == WR && m->to == sq_of(0,0)) s->castle &= ~2;
  if (m->captured == WR && m->to == sq_of(7,0)) s->castle &= ~1;
  if (m->captured == BR && m->to == sq_of(0,7)) s->castle &= ~8;
  if (m->captured == BR && m->to == sq_of(7,7)) s->castle &= ~4;

  if (pawn_move || is_capture) s->halfmove = 0;
  else s->halfmove++;

  if (s->side == BLACK) s->fullmove++;

  s->side = 1 - s->side;

  // keep UCI string handy
  move_to_uci(m, m->uci);
}

static void undo_move(State *s, const Move *m) {
  // Restore turn
  s->side = 1 - s->side;

  // Restore counters and rights
  s->castle = m->prev_castle;
  s->ep = m->prev_ep;
  s->halfmove = m->prev_halfmove;
  s->fullmove = m->prev_fullmove;

  // Undo castling rook move first (king will be moved back below)
  if (m->flags & MF_CASTLE) {
    if (m->to == sq_of(6,0)) {
      s->board[sq_of(7,0)] = WR;
      s->board[sq_of(5,0)] = EMPTY;
    } else if (m->to == sq_of(2,0)) {
      s->board[sq_of(0,0)] = WR;
      s->board[sq_of(3,0)] = EMPTY;
    } else if (m->to == sq_of(6,7)) {
      s->board[sq_of(7,7)] = BR;
      s->board[sq_of(5,7)] = EMPTY;
    } else if (m->to == sq_of(2,7)) {
      s->board[sq_of(0,7)] = BR;
      s->board[sq_of(3,7)] = EMPTY;
    }
  }

  // Move piece back (handle promotion by restoring pawn)
  int moved = s->board[m->to];
  if (m->flags & MF_PROMO) {
    moved = (s->side == WHITE) ? WP : BP;
  }
  s->board[m->from] = moved;
  s->board[m->to] = EMPTY;

  // Restore captured piece
  if (m->flags & MF_EP) {
    int cap_sq = (s->side == WHITE) ? (m->to - 8) : (m->to + 8);
    s->board[cap_sq] = m->captured;
  } else {
    s->board[m->to] = m->captured;
  }
}

static void push_move(Move *list, int *n, int from, int to, int flags, int prom) {
  Move m;
  memset(&m, 0, sizeof(m));
  m.from = (uint8_t)from;
  m.to = (uint8_t)to;
  m.flags = (uint8_t)flags;
  m.prom = (uint8_t)prom;
  list[(*n)++] = m;
}

static void gen_pseudo(const State *s, Move *out, int *n, int from_filter) {
  *n = 0;
  int side = s->side;

  for (int from = 0; from < 64; from++) {
    if (from_filter >= 0 && from != from_filter) continue;
    int p = s->board[from];
    if (p == EMPTY) continue;
    if (piece_color(p) != side) continue;

    int t = piece_type(p);
    int f = file_of(from), r = rank_of(from);

    if (t == 1) { // pawn
      int dir = (side == WHITE) ? 1 : -1;
      int start_rank = (side == WHITE) ? 1 : 6;
      int promo_rank = (side == WHITE) ? 7 : 0;

      int one = sq_of(f, r + dir);
      if (r + dir >= 0 && r + dir <= 7 && s->board[one] == EMPTY) {
        // promotion?
        if (r + dir == promo_rank) {
          int q = (side == WHITE) ? WQ : BQ;
          int r0= (side == WHITE) ? WR : BR;
          int b0= (side == WHITE) ? WB : BB;
          int n0= (side == WHITE) ? WN : BN;
          push_move(out, n, from, one, MF_PROMO, q);
          push_move(out, n, from, one, MF_PROMO, r0);
          push_move(out, n, from, one, MF_PROMO, b0);
          push_move(out, n, from, one, MF_PROMO, n0);
        } else {
          push_move(out, n, from, one, 0, 0);
          // double
          if (r == start_rank) {
            int two = sq_of(f, r + 2*dir);
            if (s->board[two] == EMPTY) push_move(out, n, from, two, 0, 0);
          }
        }
      }
      // captures
      for (int df = -1; df <= 1; df += 2) {
        int nf = f + df;
        int nr = r + dir;
        if (nf < 0 || nf > 7 || nr < 0 || nr > 7) continue;
        int to = sq_of(nf, nr);
        int tp = s->board[to];

        bool ep = (s->ep == to);
        if (ep || (tp != EMPTY && piece_color(tp) != side)) {
          int flags = MF_CAPTURE | (ep ? MF_EP : 0);
          if (nr == promo_rank) {
            int q = (side == WHITE) ? WQ : BQ;
            int r0= (side == WHITE) ? WR : BR;
            int b0= (side == WHITE) ? WB : BB;
            int n0= (side == WHITE) ? WN : BN;
            push_move(out, n, from, to, flags | MF_PROMO, q);
            push_move(out, n, from, to, flags | MF_PROMO, r0);
            push_move(out, n, from, to, flags | MF_PROMO, b0);
            push_move(out, n, from, to, flags | MF_PROMO, n0);
          } else {
            push_move(out, n, from, to, flags, 0);
          }
        }
      }
    } else if (t == 2) { // knight
      static const int kn[8] = { 17, 15, 10, 6, -6, -10, -15, -17 };
      for (int i=0;i<8;i++) {
        int to = from + kn[i];
        if (!on_board(to)) continue;
        int df = abs(file_of(to) - f);
        int dr = abs(rank_of(to) - r);
        if (!((df==1 && dr==2) || (df==2 && dr==1))) continue;
        int tp = s->board[to];
        if (tp == EMPTY) push_move(out, n, from, to, 0, 0);
        else if (piece_color(tp) != side) push_move(out, n, from, to, MF_CAPTURE, 0);
      }
    } else if (t == 3 || t == 4 || t == 5) { // bishop/rook/queen
      static const int diag[4] = { 9, 7, -7, -9 };
      static const int ortho[4]= { 8, -8, 1, -1 };

      const int *dirs = NULL;
      int nd = 0;
      int tmp[8];

      if (t == 3) { // bishop
        for (int i=0;i<4;i++) tmp[i]=diag[i];
        dirs = tmp; nd = 4;
      } else if (t == 4) { // rook
        for (int i=0;i<4;i++) tmp[i]=ortho[i];
        dirs = tmp; nd = 4;
      } else { // queen
        for (int i=0;i<4;i++) tmp[i]=diag[i];
        for (int i=0;i<4;i++) tmp[4+i]=ortho[i];
        dirs = tmp; nd = 8;
      }

      for (int d=0; d<nd; d++) {
        int cur = from;
        while (1) {
          int next = cur + dirs[d];
          if (!on_board(next)) break;
          if ((dirs[d] == 1 || dirs[d] == -1) && rank_of(next) != rank_of(cur)) break;
          if ((dirs[d] == 9 || dirs[d] == -9 || dirs[d] == 7 || dirs[d] == -7) &&
              abs(file_of(next) - file_of(cur)) != 1) break;

          cur = next;
          int tp = s->board[cur];
          if (tp == EMPTY) push_move(out, n, from, cur, 0, 0);
          else {
            if (piece_color(tp) != side) push_move(out, n, from, cur, MF_CAPTURE, 0);
            break;
          }
        }
      }
    } else if (t == 6) { // king
      for (int dr=-1; dr<=1; dr++) for (int df=-1; df<=1; df++) {
        if (!df && !dr) continue;
        int nf = f + df, nr = r + dr;
        if (nf<0||nf>7||nr<0||nr>7) continue;
        int to = sq_of(nf,nr);
        int tp = s->board[to];
        if (tp == EMPTY) push_move(out, n, from, to, 0, 0);
        else if (piece_color(tp) != side) push_move(out, n, from, to, MF_CAPTURE, 0);
      }

      // castling
      if (side == WHITE && from == sq_of(4,0)) {
        if ((s->castle & 1) && s->board[sq_of(5,0)]==EMPTY && s->board[sq_of(6,0)]==EMPTY) {
          if (!in_check(s, WHITE) &&
              !is_attacked_by(s, sq_of(5,0), BLACK) &&
              !is_attacked_by(s, sq_of(6,0), BLACK)) {
            push_move(out, n, from, sq_of(6,0), MF_CASTLE, 0);
          }
        }
        if ((s->castle & 2) && s->board[sq_of(3,0)]==EMPTY && s->board[sq_of(2,0)]==EMPTY && s->board[sq_of(1,0)]==EMPTY) {
          if (!in_check(s, WHITE) &&
              !is_attacked_by(s, sq_of(3,0), BLACK) &&
              !is_attacked_by(s, sq_of(2,0), BLACK)) {
            push_move(out, n, from, sq_of(2,0), MF_CASTLE, 0);
          }
        }
      } else if (side == BLACK && from == sq_of(4,7)) {
        if ((s->castle & 4) && s->board[sq_of(5,7)]==EMPTY && s->board[sq_of(6,7)]==EMPTY) {
          if (!in_check(s, BLACK) &&
              !is_attacked_by(s, sq_of(5,7), WHITE) &&
              !is_attacked_by(s, sq_of(6,7), WHITE)) {
            push_move(out, n, from, sq_of(6,7), MF_CASTLE, 0);
          }
        }
        if ((s->castle & 8) && s->board[sq_of(3,7)]==EMPTY && s->board[sq_of(2,7)]==EMPTY && s->board[sq_of(1,7)]==EMPTY) {
          if (!in_check(s, BLACK) &&
              !is_attacked_by(s, sq_of(3,7), WHITE) &&
              !is_attacked_by(s, sq_of(2,7), WHITE)) {
            push_move(out, n, from, sq_of(2,7), MF_CASTLE, 0);
          }
        }
      }
    }
  }
}

static int gen_legal(const State *s, Move *out, int max, int from_filter) {
  Move pseudo[256];
  int np = 0;
  gen_pseudo(s, pseudo, &np, from_filter);

  int n = 0;
  for (int i=0;i<np;i++) {
    State t = *s;
    Move m = pseudo[i];

    // Pre-mark captured for non-EP capture (for SAN correctness later when using this move)
    if (m.flags & MF_EP) {
      // will be captured from behind; leave to apply_move
    } else {
      int tp = t.board[m.to];
      if (tp != EMPTY) m.flags |= MF_CAPTURE;
    }

    apply_move(&t, &m);

    int mover = 1 - t.side;
    if (!in_check(&t, mover)) {
      if (n < max) out[n++] = m;
    }
  }
  return n;
}

/* ------------------------------ SAN (PGN move text) ------------------------------ */

static char piece_letter_san(int piece) {
  switch (piece_type(piece)) {
    case 2: return 'N';
    case 3: return 'B';
    case 4: return 'R';
    case 5: return 'Q';
    case 6: return 'K';
    default: return 0; // pawn
  }
}

static void san_for_move(const State *pre, const Move *m_in, char out[16]) {
  Move m = *m_in;

  int p = pre->board[m.from];
  int side = pre->side;

  // Determine capture properly (including EP)
  bool capture = false;
  if (m.flags & MF_EP) capture = true;
  else if (pre->board[m.to] != EMPTY) capture = true;

  // Castling
  if (m.flags & MF_CASTLE) {
    if (file_of(m.to) == 6) snprintf(out, 16, "O-O");
    else snprintf(out, 16, "O-O-O");
  } else {
    char buf[32] = {0};
    int idx = 0;

    char pl = piece_letter_san(p);
    if (pl) buf[idx++] = pl;

    // Disambiguation for pieces (not pawn, not king)
    if (pl && piece_type(p) != 6) {
      Move moves[256];
      int nm = gen_legal(pre, moves, 256, -1);

      bool amb = false;
      bool same_file = false;
      bool same_rank = false;

      for (int i=0;i<nm;i++) {
        if (moves[i].to != m.to) continue;
        if (moves[i].from == m.from) continue;
        int op = pre->board[moves[i].from];
        if (op == EMPTY) continue;
        if (piece_color(op) != side) continue;
        if (piece_type(op) != piece_type(p)) continue;
        amb = true;
        if (file_of(moves[i].from) == file_of(m.from)) same_file = true;
        if (rank_of(moves[i].from) == rank_of(m.from)) same_rank = true;
      }

      if (amb) {
        // Prefer file if unique, else rank if unique, else both.
        bool need_file = !same_file;
        bool need_rank = !same_rank;
        if (need_file) buf[idx++] = file_char(m.from);
        else if (need_rank) buf[idx++] = rank_char(m.from);
        else {
          buf[idx++] = file_char(m.from);
          buf[idx++] = rank_char(m.from);
        }
      }
    }

    // Pawn captures include from file
    if (!pl && capture) buf[idx++] = file_char(m.from);

    if (capture) buf[idx++] = 'x';

    buf[idx++] = file_char(m.to);
    buf[idx++] = rank_char(m.to);

    if (m.flags & MF_PROMO) {
      buf[idx++] = '=';
      char pr = piece_letter_san(m.prom);
      if (!pr) pr = 'Q';
      buf[idx++] = pr;
    }

    buf[idx] = 0;
    snprintf(out, 16, "%s", buf);
  }

  // Check / mate suffix (post position)
  State post = *pre;
  Move mm = m;
  apply_move(&post, &mm);

  bool chk = in_check(&post, post.side);
  if (chk) {
    Move tmp[256];
    int n = gen_legal(&post, tmp, 256, -1);
    if (n == 0) strncat(out, "#", 16 - strlen(out) - 1);
    else strncat(out, "+", 16 - strlen(out) - 1);
  }
}

/* ------------------------------ UCI Engine ------------------------------ */

typedef struct {
  pid_t pid;
  FILE *in;   // to engine stdin
  FILE *out;  // from engine stdout
  char name[128];
  bool ok;
} Engine;

static void engine_send(Engine *e, const char *fmt, ...) {
  if (!e || !e->in) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(e->in, fmt, ap);
  va_end(ap);
  fflush(e->in);
}

static bool engine_readline(Engine *e, char *buf, size_t cap) {
  if (!e || !e->out) return false;
  if (!fgets(buf, (int)cap, e->out)) return false;
  size_t n = strlen(buf);
  while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = 0;
  return true;
}

static bool engine_wait_for(Engine *e, const char *token, int max_lines) {
  char line[4096];
  for (int i=0;i<max_lines;i++) {
    if (!engine_readline(e, line, sizeof line)) return false;
    if (!strncmp(line, "id name ", 8)) {
      snprintf(e->name, sizeof e->name, "%s", line + 8);
    }
    if (strstr(line, token)) return true;
  }
  return false;
}

static bool engine_start(Engine *e, const char *path) {
  memset(e, 0, sizeof(*e));
  snprintf(e->name, sizeof e->name, "%s", path);

  int to_child[2], from_child[2];
  if (pipe(to_child) < 0) return false;
  if (pipe(from_child) < 0) return false;

  pid_t pid = fork();
  if (pid < 0) return false;

  if (pid == 0) {
    // child
    dup2(to_child[0], STDIN_FILENO);
    dup2(from_child[1], STDOUT_FILENO);
    dup2(from_child[1], STDERR_FILENO);
    close(to_child[0]); close(to_child[1]);
    close(from_child[0]); close(from_child[1]);

    execlp(path, path, (char*)NULL);
    _exit(127);
  }

  // parent
  close(to_child[0]);
  close(from_child[1]);

  e->pid = pid;
  e->in = fdopen(to_child[1], "w");
  e->out = fdopen(from_child[0], "r");
  if (!e->in || !e->out) return false;

  setvbuf(e->in, NULL, _IONBF, 0);
  setvbuf(e->out, NULL, _IONBF, 0);

  engine_send(e, "uci\n");
  if (!engine_wait_for(e, "uciok", 5000)) return false;

  engine_send(e, "isready\n");
  if (!engine_wait_for(e, "readyok", 5000)) return false;

  e->ok = true;
  return true;
}

static void engine_stop(Engine *e) {
  if (!e || !e->ok) return;
  engine_send(e, "quit\n");
  if (e->in) fclose(e->in);
  if (e->out) fclose(e->out);
  int status = 0;
  waitpid(e->pid, &status, 0);
  e->ok = false;
}

static void uci_position_startpos(Engine *e, const char (*moves)[6], int n_moves) {
  engine_send(e, "position startpos");
  if (n_moves > 0) engine_send(e, " moves");
  for (int i=0;i<n_moves;i++) engine_send(e, " %s", moves[i]);
  engine_send(e, "\n");
}

enum TCMode { TC_DEPTH=0, TC_NODES=1, TC_TIME=2 };

static bool engine_bestmove(Engine *e, const char (*moves)[6], int n_moves,
                           enum TCMode mode, int value, char best[6]) {
  if (!e || !e->ok) return false;

  uci_position_startpos(e, moves, n_moves);

  if (mode == TC_DEPTH) engine_send(e, "go depth %d\n", value);
  else if (mode == TC_NODES) engine_send(e, "go nodes %d\n", value);
  else engine_send(e, "go movetime %d\n", value);

  char line[4096];
  while (engine_readline(e, line, sizeof line)) {
    if (!strncmp(line, "bestmove ", 9)) {
      const char *bm = line + 9;
      if (!strncmp(bm, "(none)", 6)) return false;
      // copy 4 or 5 chars
      size_t len = strlen(bm);
      if (len < 4) return false;
      best[0]=bm[0]; best[1]=bm[1]; best[2]=bm[2]; best[3]=bm[3];
      if (len >= 5 && isalpha((unsigned char)bm[4])) { best[4]=bm[4]; best[5]=0; }
      else { best[4]=0; }
      return true;
    }
  }
  return false;
}

static int alg_to_sq(char f, char r) {
  if (f < 'a' || f > 'h' || r < '1' || r > '8') return -1;
  return sq_of(f - 'a', r - '1');
}

static bool parse_uci_move(const State *s, const char uci[6], Move *outm) {
  (void)s;
  if (!uci || strlen(uci) < 4) return false;
  int from = alg_to_sq(uci[0], uci[1]);
  int to   = alg_to_sq(uci[2], uci[3]);
  if (from < 0 || to < 0) return false;

  memset(outm, 0, sizeof(*outm));
  outm->from = (uint8_t)from;
  outm->to   = (uint8_t)to;

  // promo char (if any)
  if (strlen(uci) >= 5) {
    char pc = (char)tolower((unsigned char)uci[4]);
    int prom = 0;
    int side = s->side;
    if (pc == 'q') prom = (side==WHITE)?WQ:BQ;
    else if (pc == 'r') prom = (side==WHITE)?WR:BR;
    else if (pc == 'b') prom = (side==WHITE)?WB:BB;
    else if (pc == 'n') prom = (side==WHITE)?WN:BN;
    if (prom) { outm->flags |= MF_PROMO; outm->prom = (uint8_t)prom; }
  }
  return true;
}

/* ------------------------------ UI / Rendering ------------------------------ */

static const char *piece_glyph(int p) {
  // ASCII fallback
  switch (p) {
    case WP: return "P"; case WN: return "N"; case WB: return "B"; case WR: return "R"; case WQ: return "Q"; case WK: return "K";
    case BP: return "p"; case BN: return "n"; case BB: return "b"; case BR: return "r"; case BQ: return "q"; case BK: return "k";
    default: return " ";
  }
}

static void ansi_bg256(int c) { printf("\x1b[48;5;%dm", c); }
static void ansi_fg256(int c) { printf("\x1b[38;5;%dm", c); }
static void ansi_reset(void) { printf("\x1b[0m"); }

static void clear_screen_home(void) {
  printf("\x1b[H\x1b[J");
}

static void draw_status_line(const char *msg) {
  ansi_reset();
  printf("%s\n", msg ? msg : "");
}

static void format_recent_pgn(const Move *hist, int ply, char *out, size_t cap) {
  // Show last ~12 plies in a compact way.
  // Build from a starting ply so we include move numbers.
  int start = ply - 12;
  if (start < 0) start = 0;

  out[0] = 0;
  size_t used = 0;

  for (int i = start; i < ply; i++) {
    int move_no = (i/2) + 1;
    bool white_move = ((i % 2) == 0);

    char tmp[64];
    if (white_move) snprintf(tmp, sizeof tmp, "%d. %s", move_no, hist[i].san);
    else snprintf(tmp, sizeof tmp, " %s", hist[i].san);

    size_t tl = strlen(tmp);
    if (used + tl + 2 >= cap) break;
    memcpy(out + used, tmp, tl);
    used += tl;

    if (white_move && i == ply-1) {
      // if last is white move, keep spacing tidy
    }
  }
  out[used] = 0;
}

static int prompt_number(const char *label, int cur, int y_line) {
  // y_line is informational; we just redraw prompt at bottom-ish.
  (void)y_line;
  char buf[32];
  snprintf(buf, sizeof buf, "%d", cur);
  size_t len = strlen(buf);

  while (1) {
    ansi_reset();
    printf("\n%s [%s] (Enter=ok, Esc=cancel): ", label, buf);
    fflush(stdout);

    int k = read_key();
    if (k == KEY_NONE) continue;
    if (k == KEY_ESC) return cur;
    if (k == KEY_ENTER) {
      int v = atoi(buf);
      if (v <= 0) v = cur;
      return v;
    }
    if (k == KEY_BACKSPACE) {
      if (len > 0) buf[--len] = 0;
      continue;
    }
    if (isdigit(k)) {
      if (len < sizeof(buf)-1) {
        buf[len++] = (char)k;
        buf[len] = 0;
      }
      continue;
    }
  }
}

static bool square_in_list(int sq, const Move *moves, int n) {
  for (int i=0;i<n;i++) if (moves[i].to == sq) return true;
  return false;
}

static int find_move_index(int from, int to, const Move *moves, int n, int prom_piece /*0=any*/) {
  for (int i=0;i<n;i++) {
    if (moves[i].from == from && moves[i].to == to) {
      if (!(moves[i].flags & MF_PROMO)) return i;
      if (prom_piece == 0) return i;
      if (moves[i].prom == prom_piece) return i;
    }
  }
  return -1;
}

static int choose_promotion_piece(int side) {
  // Returns a promoted piece code. Default Q if anything odd.
  while (1) {
    int k = read_key();
    if (k == KEY_NONE) continue;
    char c = (char)tolower(k);
    if (c == 'q') return (side==WHITE) ? WQ : BQ;
    if (c == 'r') return (side==WHITE) ? WR : BR;
    if (c == 'b') return (side==WHITE) ? WB : BB;
    if (c == 'n') return (side==WHITE) ? WN : BN;
    if (k == KEY_ESC) return (side==WHITE) ? WQ : BQ;
  }
}

static void draw(const State *s,
                 const Move *hist, int ply,
                 int cursor, int selected,
                 const Move *sel_moves, int n_sel_moves,
                 int last_from, int last_to,
                 const Engine *eng, int eng_side,
                 enum TCMode tcmode, int tcval,
                 const char *status_msg) {

  clear_screen_home();

  // Header
  ansi_reset();
  const char *side_name = (s->side == WHITE) ? "White" : "Black";
  const char *mode_str = (tcmode==TC_DEPTH)?"depth":(tcmode==TC_NODES)?"nodes":"time(ms)";
  const char *eng_state =
    (eng && eng->ok) ? ((eng_side<0) ? "Engine: OFF" : (eng_side==WHITE ? "Engine: White" : "Engine: Black"))
                     : "Engine: (not loaded)";

  printf("termchess  |  To move: %s  |  %s  |  TC: %s=%d\n", side_name, eng_state, mode_str, tcval);
  if (eng && eng->ok) printf("UCI engine: %s\n", eng->name[0] ? eng->name : "(unknown)");
  else printf("UCI engine: (none)\n");

  // Move list (SAN)
  char pgn[1024];
  format_recent_pgn(hist, ply, pgn, sizeof pgn);
  printf("Moves: %s\n", pgn[0] ? pgn : "(none)");

  // Board drawing
  // Colors
  const int light = 230;
  const int dark  = 94;
  const int hi_cursor = 33;
  const int hi_sel    = 208;
  const int hi_legal  = 120;
  const int hi_last   = 222;
  const int hi_check  = 196;

  bool check = in_check(s, s->side);
  int king_sq = check ? find_king(s, s->side) : -1;

  printf("\n");
  for (int r = 7; r >= 0; r--) {
    ansi_reset();
    printf(" %d ", r+1);
    for (int f=0; f<8; f++) {
      int sq = sq_of(f,r);

      int bg = ((f+r)&1) ? dark : light;

      // layered highlights (priority)
      if (sq == king_sq) bg = hi_check;
      if (sq == last_from || sq == last_to) bg = hi_last;
      if (selected >= 0 && sq == selected) bg = hi_sel;
      if (selected >= 0 && square_in_list(sq, sel_moves, n_sel_moves)) bg = hi_legal;
      if (sq == cursor) bg = hi_cursor;

      ansi_bg256(bg);
      int p = s->board[sq];
      if (p == EMPTY) {
        printf("   ");
      } else {
        // text color
        if (is_white(p)) ansi_fg256(16); else ansi_fg256(231);
        printf(" %s ", piece_glyph(p));
      }
      ansi_reset();
    }
    printf("\n");
  }
  ansi_reset();
  printf("    a  b  c  d  e  f  g  h\n");

  printf("\n");
  draw_status_line(status_msg);
  fflush(stdout);
}

/* ------------------------------ Main program ------------------------------ */

static volatile sig_atomic_t g_quit = 0;
static void on_sigint(int sig) { (void)sig; g_quit = 1; }

int main(int argc, char **argv) {
  signal(SIGINT, on_sigint);
  signal(SIGTERM, on_sigint);

  term_enable_raw();

  Engine eng;
  memset(&eng, 0, sizeof eng);
  bool have_engine = false;
  if (argc >= 2) {
    if (engine_start(&eng, argv[1])) have_engine = true;
  }

  // engine control: off (-1), white (0), black (1)
  int engine_side = have_engine ? BLACK : -1;

  State s;
  state_set_startpos(&s);

  Move hist[2048];
  char uci_hist[2048][6];
  int ply = 0;

  int cursor = sq_of(4,1);
  int selected = -1;
  Move sel_moves[256];
  int n_sel_moves = 0;

  int last_from = -1, last_to = -1;

  enum TCMode tcmode = TC_DEPTH;
  int tcval = 12;

  char status[256] = "Arrows/WASD move cursor. Enter selects/moves. t changes TC. e toggles engine. q quits.";

  while (!g_quit) {
    // detect terminal game end (mate/stalemate)
    Move all[256];
    int nall = gen_legal(&s, all, 256, -1);
    bool chk = in_check(&s, s.side);
    if (nall == 0) {
      if (chk) snprintf(status, sizeof status, "Checkmate. %s wins. (u=undo, r=reset, q=quit)",
                        (s.side==WHITE) ? "Black" : "White");
      else snprintf(status, sizeof status, "Stalemate. (u=undo, r=reset, q=quit)");
      engine_side = -1; // stop engine play after end
    } else if (chk) {
      snprintf(status, sizeof status, "Check! (%s to move)  (Enter selects, u undo, t TC, e engine, q quit)",
               (s.side==WHITE) ? "White" : "Black");
    }

    // if engine to move, ask engine and play
    if (have_engine && engine_side == s.side && nall > 0) {
      draw(&s, hist, ply, cursor, selected, sel_moves, n_sel_moves, last_from, last_to,
           &eng, engine_side, tcmode, tcval, "Engine thinking... (Ctrl-C quits)");
      char best[6];
      if (engine_bestmove(&eng, (const char (*)[6])uci_hist, ply, tcmode, tcval, best)) {
        Move em;
        if (parse_uci_move(&s, best, &em)) {
          // validate it is legal in this position
          Move legal[256];
          int nleg = gen_legal(&s, legal, 256, -1);

          int promo_piece = 0;
          if (strlen(best) >= 5) {
            char pc = (char)tolower((unsigned char)best[4]);
            if (pc=='q') promo_piece = (s.side==WHITE)?WQ:BQ;
            else if (pc=='r') promo_piece = (s.side==WHITE)?WR:BR;
            else if (pc=='b') promo_piece = (s.side==WHITE)?WB:BB;
            else if (pc=='n') promo_piece = (s.side==WHITE)?WN:BN;
          }

          int idx = find_move_index(em.from, em.to, legal, nleg, promo_piece);
          if (idx >= 0) {
            State pre = s;
            Move mv = legal[idx];
            san_for_move(&pre, &mv, mv.san);

            apply_move(&s, &mv);

            hist[ply] = mv;
            snprintf(uci_hist[ply], sizeof uci_hist[ply], "%s", mv.uci);
            ply++;

            last_from = mv.from;
            last_to = mv.to;

            selected = -1;
            n_sel_moves = 0;
            snprintf(status, sizeof status, "Engine played %s", mv.san);
            continue;
          } else {
            snprintf(status, sizeof status, "Engine produced illegal move: %s (ignored)", best);
            engine_side = -1;
          }
        }
      } else {
        snprintf(status, sizeof status, "Engine returned no move. (u=undo, r=reset, q=quit)");
        engine_side = -1;
      }
    }

    // draw current frame
    draw(&s, hist, ply, cursor, selected, sel_moves, n_sel_moves, last_from, last_to,
         have_engine ? &eng : NULL, engine_side, tcmode, tcval, status);

    // input
    int k = read_key();
    if (k == KEY_NONE) continue;

    // cursor movement
    if (k == KEY_UP || k == 'w') {
      int r = rank_of(cursor);
      if (r < 7) cursor += 8;
    } else if (k == KEY_DOWN || k == 's') {
      int r = rank_of(cursor);
      if (r > 0) cursor -= 8;
    } else if (k == KEY_LEFT || k == 'a') {
      int f = file_of(cursor);
      if (f > 0) cursor -= 1;
    } else if (k == KEY_RIGHT || k == 'd') {
      int f = file_of(cursor);
      if (f < 7) cursor += 1;
    } else if (k == KEY_ESC) {
      selected = -1;
      n_sel_moves = 0;
      snprintf(status, sizeof status, "Selection cancelled.");
    } else if (k == 'q') {
      break;
    } else if (k == 'r') {
      state_set_startpos(&s);
      ply = 0;
      selected = -1;
      n_sel_moves = 0;
      last_from = last_to = -1;
      snprintf(status, sizeof status, "Reset to start position.");
      if (have_engine) engine_send(&eng, "ucinewgame\nisready\n");
    } else if (k == 'u' || k == 'U') {
      int count = (k == 'U') ? 2 : 1;
      while (count-- > 0 && ply > 0) {
        ply--;
        undo_move(&s, &hist[ply]);
        last_from = last_to = -1;
      }
      selected = -1;
      n_sel_moves = 0;
      snprintf(status, sizeof status, "Undone.");
      if (have_engine) engine_send(&eng, "isready\n");
    } else if (k == 'e') {
      if (!have_engine) {
        snprintf(status, sizeof status, "No engine loaded. Run: ./termchess stockfish");
      } else {
        if (engine_side < 0) engine_side = BLACK;
        else if (engine_side == BLACK) engine_side = WHITE;
        else engine_side = -1;
        snprintf(status, sizeof status, "Engine control changed.");
      }
    } else if (k == 't') {
      tcmode = (enum TCMode)((tcmode + 1) % 3);
      snprintf(status, sizeof status, "Time control mode changed.");
    } else if (k == '+' || k == '=') {
      if (tcmode == TC_DEPTH) tcval += 1;
      else if (tcmode == TC_NODES) tcval += 100000;
      else tcval += 100;
      if (tcval < 1) tcval = 1;
      snprintf(status, sizeof status, "TC now %d.", tcval);
    } else if (k == '-') {
      if (tcmode == TC_DEPTH) tcval -= 1;
      else if (tcmode == TC_NODES) tcval -= 100000;
      else tcval -= 100;
      if (tcval < 1) tcval = 1;
      snprintf(status, sizeof status, "TC now %d.", tcval);
    } else if (k == 'T') {
      const char *label = (tcmode==TC_DEPTH) ? "Set depth" : (tcmode==TC_NODES) ? "Set nodes" : "Set movetime(ms)";
      // redraw prompt in-place by temporarily printing after current draw
      tcval = prompt_number(label, tcval, 0);
      if (tcval < 1) tcval = 1;
      snprintf(status, sizeof status, "TC set to %d.", tcval);
    } else if (k == KEY_ENTER || k == ' ') {
      // only allow human input if side not controlled by engine
      if (have_engine && engine_side == s.side) {
        snprintf(status, sizeof status, "Engine controls this side. Press e to change engine control.");
        continue;
      }

      if (selected < 0) {
        int p = s.board[cursor];
        if (p == EMPTY || piece_color(p) != s.side) {
          snprintf(status, sizeof status, "Select a %s piece to move.", (s.side==WHITE)?"White":"Black");
          continue;
        }
        selected = cursor;
        n_sel_moves = gen_legal(&s, sel_moves, 256, selected);
        snprintf(status, sizeof status, "Selected %c%c. Choose destination; legal moves highlighted. (Esc cancels)",
                 file_char(selected), rank_char(selected));
      } else {
        // attempt move selected -> cursor
        Move leg[256];
        int nleg = gen_legal(&s, leg, 256, -1);
        int idx = find_move_index(selected, cursor, leg, nleg, 0);
        if (idx < 0) {
          snprintf(status, sizeof status, "Illegal move.");
          continue;
        }
        Move mv = leg[idx];

        // if promotion, ask
        if (mv.flags & MF_PROMO) {
          draw(&s, hist, ply, cursor, selected, sel_moves, n_sel_moves, last_from, last_to,
               have_engine ? &eng : NULL, engine_side, tcmode, tcval,
               "Promote to: (q)ueen, (r)ook, (b)ishop, k(n)ight  (Esc=Q)");
          int prom = choose_promotion_piece(s.side);
          mv.prom = (uint8_t)prom;
        }

        State pre = s;
        san_for_move(&pre, &mv, mv.san);
        apply_move(&s, &mv);

        hist[ply] = mv;
        snprintf(uci_hist[ply], sizeof uci_hist[ply], "%s", mv.uci);
        ply++;

        last_from = mv.from;
        last_to = mv.to;

        selected = -1;
        n_sel_moves = 0;

        snprintf(status, sizeof status, "Played %s", mv.san);
      }
    } else {
      // ignore
    }
  }

  if (have_engine) engine_stop(&eng);
  return 0;
}
