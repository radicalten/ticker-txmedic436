/*
  termchess.c - single-file terminal chess GUI (macOS Terminal) + UCI engine support
  No external dependencies (no ncurses). Uses ANSI escape sequences + termios.

  Build:
    clang -O2 -std=c11 -Wall -Wextra termchess.c -o termchess

  Run:
    ./termchess [--engine-white|--engine-black] [engine_path_or_name]

  Keys:
    Arrows move cursor
    Space/Enter select / move
    Esc/Backspace cancel selection
    u undo 1 ply
    U undo 2 plies
    e request engine move now
    q quit (prints PGN on exit)
*/

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ------------------------- Terminal raw mode ------------------------- */

static struct termios g_orig_termios;
static int g_raw_enabled = 0;

static void term_write(const char *s) {
  write(STDOUT_FILENO, s, (size_t)strlen(s));
}

static void term_printf(const char *fmt, ...) {
  char buf[8192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  term_write(buf);
}

static void term_disable_raw(void) {
  if (!g_raw_enabled) return;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
  g_raw_enabled = 0;
  term_write("\x1b[0m\x1b[?25h"); /* reset + show cursor */
}

static void term_enable_raw(void) {
  if (g_raw_enabled) return;
  if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1) {
    perror("tcgetattr");
    exit(1);
  }
  atexit(term_disable_raw);

  struct termios raw = g_orig_termios;
  raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= (tcflag_t)~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1; /* 100ms read timeout */

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    perror("tcsetattr");
    exit(1);
  }
  g_raw_enabled = 1;
  term_write("\x1b[?25l"); /* hide cursor */
}

static volatile sig_atomic_t g_should_quit = 0;

static void on_sigint(int sig) {
  (void)sig;
  g_should_quit = 1;
}

/* ------------------------- Chess core ------------------------- */

enum {
  EMPTY=0,
  PAWN=1, KNIGHT=2, BISHOP=3, ROOK=4, QUEEN=5, KING=6
};

static inline int sgn(int x){ return (x>0)-(x<0); }

static inline int piece_color(int p) { return sgn(p); } /* 1 white, -1 black, 0 empty */
static inline int piece_type(int p) { return p<0 ? -p : p; }

static inline int sq_file(int sq){ return sq & 7; }
static inline int sq_rank(int sq){ return sq >> 3; } /* 0..7 means rank 1..8 */

static inline int in_bounds(int f,int r){ return (unsigned)f<8u && (unsigned)r<8u; }
static inline int fr_to_sq(int f,int r){ return (r<<3) | f; }

static void sq_to_alg(int sq, char out[3]) {
  out[0] = (char)('a' + sq_file(sq));
  out[1] = (char)('1' + sq_rank(sq));
  out[2] = '\0';
}

typedef struct {
  uint8_t from, to;
  uint8_t promo; /* 0 none, else KNIGHT/BISHOP/ROOK/QUEEN */
  uint8_t flags;
} Move;

enum {
  MF_CAPTURE = 1<<0,
  MF_EP      = 1<<1,
  MF_CASTLE  = 1<<2,
  MF_PROMO   = 1<<3
};

typedef struct {
  Move m[256];
  int n;
} MoveList;

typedef struct {
  int b[64];
  int side;      /* 1 white to move, -1 black to move */
  int castling;  /* bits: 1 K,2 Q,4 k,8 q */
  int ep;        /* en-passant target square index or -1 */
  int halfmove;
  int fullmove;
} Position;

typedef struct {
  int captured;
  int castling;
  int ep;
  int halfmove;
  int fullmove;
  int movedPiece;
  int rookFrom, rookTo;
  int epCapturedSq;
} Undo;

#define MAX_PLY 2048

typedef struct {
  Position pos;
  Undo undos[MAX_PLY];
  Move moves[MAX_PLY];
  char san[MAX_PLY][32];
  char uci[MAX_PLY][8];
  int ply;
} Game;

/* ------------------------- UCI Engine ------------------------- */

typedef struct {
  pid_t pid;
  FILE *to;   /* write to engine stdin */
  FILE *from; /* read engine stdout */
  int alive;
} Engine;

static void engine_stop(Engine *e) {
  if (!e || !e->alive) return;
  fprintf(e->to, "quit\n");
  fflush(e->to);
  fclose(e->to);
  fclose(e->from);
  kill(e->pid, SIGTERM);
  int status=0;
  waitpid(e->pid, &status, 0);
  e->alive = 0;
}

static int engine_start(Engine *e, const char *path) {
  memset(e, 0, sizeof(*e));

  int inpipe[2], outpipe[2];
  if (pipe(inpipe) != 0) return -1;
  if (pipe(outpipe) != 0) return -1;

  pid_t pid = fork();
  if (pid < 0) return -1;

  if (pid == 0) {
    /* child */
    dup2(inpipe[0], STDIN_FILENO);
    dup2(outpipe[1], STDOUT_FILENO);
    dup2(outpipe[1], STDERR_FILENO);
    close(inpipe[0]); close(inpipe[1]);
    close(outpipe[0]); close(outpipe[1]);

    char *const argv[] = { (char*)path, NULL };
    execvp(path, argv);
    _exit(127);
  }

  /* parent */
  close(inpipe[0]);
  close(outpipe[1]);

  e->pid = pid;
  e->to = fdopen(inpipe[1], "w");
  e->from = fdopen(outpipe[0], "r");
  if (!e->to || !e->from) return -1;

  setvbuf(e->to, NULL, _IONBF, 0);
  setvbuf(e->from, NULL, _IOLBF, 0);
  e->alive = 1;
  return 0;
}

static int engine_wait_for(Engine *e, const char *token, int maxLines) {
  char line[4096];
  for (int i=0; i<maxLines; i++) {
    if (!fgets(line, sizeof(line), e->from)) return -1;
    if (strstr(line, token)) return 0;
  }
  return -1;
}

static int engine_send(Engine *e, const char *fmt, ...) {
  if (!e || !e->alive) return -1;
  char buf[4096];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fputs(buf, e->to);
  fputc('\n', e->to);
  fflush(e->to);
  return 0;
}

static int engine_init_uci(Engine *e) {
  if (engine_send(e, "uci") != 0) return -1;
  if (engine_wait_for(e, "uciok", 10000) != 0) return -1;
  if (engine_send(e, "isready") != 0) return -1;
  if (engine_wait_for(e, "readyok", 10000) != 0) return -1;
  engine_send(e, "ucinewgame");
  engine_send(e, "isready");
  if (engine_wait_for(e, "readyok", 10000) != 0) return -1;
  return 0;
}

/* ------------------------- Move generation helpers ------------------------- */

static int find_king(const Position *p, int side) {
  int k = (side>0) ? KING : -KING;
  for (int i=0;i<64;i++) if (p->b[i]==k) return i;
  return -1;
}

static bool is_square_attacked(const Position *p, int sq, int attackerSide) {
  /* attackerSide: 1 white, -1 black */
  int f = sq_file(sq), r = sq_rank(sq);

  /* pawns */
  if (attackerSide == 1) {
    /* white pawns attack from below: squares (sq-7), (sq-9) */
    int s1f = f-1, s1r = r-1;
    int s2f = f+1, s2r = r-1;
    if (in_bounds(s1f,s1r) && p->b[fr_to_sq(s1f,s1r)] == PAWN) return true;
    if (in_bounds(s2f,s2r) && p->b[fr_to_sq(s2f,s2r)] == PAWN) return true;
  } else {
    /* black pawns attack from above: squares (sq+7), (sq+9) */
    int s1f = f-1, s1r = r+1;
    int s2f = f+1, s2r = r+1;
    if (in_bounds(s1f,s1r) && p->b[fr_to_sq(s1f,s1r)] == -PAWN) return true;
    if (in_bounds(s2f,s2r) && p->b[fr_to_sq(s2f,s2r)] == -PAWN) return true;
  }

  /* knights */
  static const int kofs[8][2] = {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
  for (int i=0;i<8;i++){
    int nf=f+kofs[i][0], nr=r+kofs[i][1];
    if (!in_bounds(nf,nr)) continue;
    int psq = fr_to_sq(nf,nr);
    int pc = p->b[psq];
    if (pc == attackerSide*KNIGHT) return true;
  }

  /* bishops/queens diagonals */
  static const int dofs[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
  for (int d=0; d<4; d++){
    int nf=f+dofs[d][0], nr=r+dofs[d][1];
    while (in_bounds(nf,nr)){
      int psq = fr_to_sq(nf,nr);
      int pc = p->b[psq];
      if (pc!=EMPTY){
        if (pc == attackerSide*BISHOP || pc == attackerSide*QUEEN) return true;
        break;
      }
      nf+=dofs[d][0]; nr+=dofs[d][1];
    }
  }

  /* rooks/queens orthogonals */
  static const int rofs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
  for (int d=0; d<4; d++){
    int nf=f+rofs[d][0], nr=r+rofs[d][1];
    while (in_bounds(nf,nr)){
      int psq = fr_to_sq(nf,nr);
      int pc = p->b[psq];
      if (pc!=EMPTY){
        if (pc == attackerSide*ROOK || pc == attackerSide*QUEEN) return true;
        break;
      }
      nf+=rofs[d][0]; nr+=rofs[d][1];
    }
  }

  /* king */
  for (int df=-1; df<=1; df++){
    for (int dr=-1; dr<=1; dr++){
      if (!df && !dr) continue;
      int nf=f+df, nr=r+dr;
      if (!in_bounds(nf,nr)) continue;
      int psq = fr_to_sq(nf,nr);
      if (p->b[psq] == attackerSide*KING) return true;
    }
  }

  return false;
}

static bool is_in_check(const Position *p, int side) {
  int ksq = find_king(p, side);
  if (ksq<0) return false;
  return is_square_attacked(p, ksq, -side);
}

static void add_move(MoveList *ml, int from, int to, int flags, int promo) {
  Move m;
  m.from = (uint8_t)from;
  m.to = (uint8_t)to;
  m.flags = (uint8_t)flags;
  m.promo = (uint8_t)promo;
  ml->m[ml->n++] = m;
}

static void gen_pseudo(const Position *p, MoveList *ml) {
  ml->n = 0;
  int side = p->side;

  for (int sq=0;sq<64;sq++){
    int pc = p->b[sq];
    if (pc==EMPTY || piece_color(pc)!=side) continue;
    int pt = piece_type(pc);
    int f = sq_file(sq), r = sq_rank(sq);

    if (pt == PAWN) {
      int dir = (side==1)? +1 : -1;
      int r1 = r + dir;
      if (in_bounds(f,r1)) {
        int to = fr_to_sq(f,r1);
        if (p->b[to]==EMPTY) {
          /* promotion? */
          if ((side==1 && r1==7) || (side==-1 && r1==0)) {
            add_move(ml, sq, to, MF_PROMO, QUEEN);
            add_move(ml, sq, to, MF_PROMO, ROOK);
            add_move(ml, sq, to, MF_PROMO, BISHOP);
            add_move(ml, sq, to, MF_PROMO, KNIGHT);
          } else {
            add_move(ml, sq, to, 0, 0);
          }

          /* double push */
          if ((side==1 && r==1) || (side==-1 && r==6)) {
            int r2 = r + 2*dir;
            int to2 = fr_to_sq(f,r2);
            if (p->b[to2]==EMPTY) {
              add_move(ml, sq, to2, 0, 0);
            }
          }
        }
      }

      /* captures */
      for (int df=-1; df<=1; df+=2) {
        int nf = f + df;
        int nr = r + dir;
        if (!in_bounds(nf,nr)) continue;
        int to = fr_to_sq(nf,nr);
        int target = p->b[to];
        bool isCap = (target!=EMPTY && piece_color(target)==-side);
        bool isEp = (p->ep == to);
        if (isCap || isEp) {
          int flags = MF_CAPTURE | (isEp?MF_EP:0);
          if ((side==1 && nr==7) || (side==-1 && nr==0)) {
            add_move(ml, sq, to, flags|MF_PROMO, QUEEN);
            add_move(ml, sq, to, flags|MF_PROMO, ROOK);
            add_move(ml, sq, to, flags|MF_PROMO, BISHOP);
            add_move(ml, sq, to, flags|MF_PROMO, KNIGHT);
          } else {
            add_move(ml, sq, to, flags, 0);
          }
        }
      }
    }
    else if (pt == KNIGHT) {
      static const int ofs[8][2] = {{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
      for (int i=0;i<8;i++){
        int nf=f+ofs[i][0], nr=r+ofs[i][1];
        if (!in_bounds(nf,nr)) continue;
        int to = fr_to_sq(nf,nr);
        int t = p->b[to];
        if (t==EMPTY) add_move(ml, sq, to, 0, 0);
        else if (piece_color(t)==-side) add_move(ml, sq, to, MF_CAPTURE, 0);
      }
    }
    else if (pt == BISHOP || pt==ROOK || pt==QUEEN) {
      static const int bishopDirs[4][2] = {{1,1},{1,-1},{-1,1},{-1,-1}};
      static const int rookDirs[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
      const int (*dirs)[2] = NULL;
      int dn = 0;

      int tmpDirs[8][2];
      if (pt==BISHOP) {
        dirs = bishopDirs; dn = 4;
      } else if (pt==ROOK) {
        dirs = rookDirs; dn = 4;
      } else {
        for (int i=0;i<4;i++){ tmpDirs[i][0]=bishopDirs[i][0]; tmpDirs[i][1]=bishopDirs[i][1]; }
        for (int i=0;i<4;i++){ tmpDirs[4+i][0]=rookDirs[i][0]; tmpDirs[4+i][1]=rookDirs[i][1]; }
        dirs = tmpDirs; dn = 8;
      }

      for (int d=0; d<dn; d++){
        int nf=f+dirs[d][0], nr=r+dirs[d][1];
        while (in_bounds(nf,nr)) {
          int to = fr_to_sq(nf,nr);
          int t = p->b[to];
          if (t==EMPTY) add_move(ml, sq, to, 0, 0);
          else {
            if (piece_color(t)==-side) add_move(ml, sq, to, MF_CAPTURE, 0);
            break;
          }
          nf+=dirs[d][0]; nr+=dirs[d][1];
        }
      }
    }
    else if (pt == KING) {
      for (int df=-1; df<=1; df++){
        for (int dr=-1; dr<=1; dr++){
          if (!df && !dr) continue;
          int nf=f+df, nr=r+dr;
          if (!in_bounds(nf,nr)) continue;
          int to = fr_to_sq(nf,nr);
          int t = p->b[to];
          if (t==EMPTY) add_move(ml, sq, to, 0, 0);
          else if (piece_color(t)==-side) add_move(ml, sq, to, MF_CAPTURE, 0);
        }
      }

      /* castling */
      if (side==1 && sq==fr_to_sq(4,0)) {
        /* king side */
        if ((p->castling & 1) &&
            p->b[fr_to_sq(5,0)]==EMPTY && p->b[fr_to_sq(6,0)]==EMPTY &&
            !is_square_attacked(p, fr_to_sq(4,0), -1) &&
            !is_square_attacked(p, fr_to_sq(5,0), -1) &&
            !is_square_attacked(p, fr_to_sq(6,0), -1)) {
          add_move(ml, sq, fr_to_sq(6,0), MF_CASTLE, 0);
        }
        /* queen side */
        if ((p->castling & 2) &&
            p->b[fr_to_sq(3,0)]==EMPTY && p->b[fr_to_sq(2,0)]==EMPTY && p->b[fr_to_sq(1,0)]==EMPTY &&
            !is_square_attacked(p, fr_to_sq(4,0), -1) &&
            !is_square_attacked(p, fr_to_sq(3,0), -1) &&
            !is_square_attacked(p, fr_to_sq(2,0), -1)) {
          add_move(ml, sq, fr_to_sq(2,0), MF_CASTLE, 0);
        }
      }
      if (side==-1 && sq==fr_to_sq(4,7)) {
        if ((p->castling & 4) &&
            p->b[fr_to_sq(5,7)]==EMPTY && p->b[fr_to_sq(6,7)]==EMPTY &&
            !is_square_attacked(p, fr_to_sq(4,7), +1) &&
            !is_square_attacked(p, fr_to_sq(5,7), +1) &&
            !is_square_attacked(p, fr_to_sq(6,7), +1)) {
          add_move(ml, sq, fr_to_sq(6,7), MF_CASTLE, 0);
        }
        if ((p->castling & 8) &&
            p->b[fr_to_sq(3,7)]==EMPTY && p->b[fr_to_sq(2,7)]==EMPTY && p->b[fr_to_sq(1,7)]==EMPTY &&
            !is_square_attacked(p, fr_to_sq(4,7), +1) &&
            !is_square_attacked(p, fr_to_sq(3,7), +1) &&
            !is_square_attacked(p, fr_to_sq(2,7), +1)) {
          add_move(ml, sq, fr_to_sq(2,7), MF_CASTLE, 0);
        }
      }
    }
  }
}

static void make_move(Position *p, Move m, Undo *u) {
  memset(u, 0, sizeof(*u));
  u->captured = EMPTY;
  u->castling = p->castling;
  u->ep = p->ep;
  u->halfmove = p->halfmove;
  u->fullmove = p->fullmove;
  u->movedPiece = p->b[m.from];
  u->rookFrom = u->rookTo = -1;
  u->epCapturedSq = -1;

  int side = p->side;
  int moved = p->b[m.from];
  int from = m.from, to = m.to;

  /* halfmove clock */
  int pt = piece_type(moved);
  if (pt == PAWN || (m.flags & MF_CAPTURE)) p->halfmove = 0;
  else p->halfmove++;

  /* clear ep by default */
  p->ep = -1;

  /* capture (incl ep) */
  if (m.flags & MF_EP) {
    int capSq = to + (side==1 ? -8 : +8);
    u->epCapturedSq = capSq;
    u->captured = p->b[capSq];
    p->b[capSq] = EMPTY;
  } else if (m.flags & MF_CAPTURE) {
    u->captured = p->b[to];
  }

  /* move piece */
  p->b[to] = moved;
  p->b[from] = EMPTY;

  /* promotion */
  if (m.flags & MF_PROMO) {
    int promoPiece = (side==1) ? (int)m.promo : -(int)m.promo;
    p->b[to] = promoPiece;
  }

  /* castling rook move */
  if (m.flags & MF_CASTLE) {
    if (side==1) {
      if (to == fr_to_sq(6,0)) { /* O-O */
        u->rookFrom = fr_to_sq(7,0);
        u->rookTo   = fr_to_sq(5,0);
      } else { /* O-O-O */
        u->rookFrom = fr_to_sq(0,0);
        u->rookTo   = fr_to_sq(3,0);
      }
    } else {
      if (to == fr_to_sq(6,7)) {
        u->rookFrom = fr_to_sq(7,7);
        u->rookTo   = fr_to_sq(5,7);
      } else {
        u->rookFrom = fr_to_sq(0,7);
        u->rookTo   = fr_to_sq(3,7);
      }
    }
    p->b[u->rookTo] = p->b[u->rookFrom];
    p->b[u->rookFrom] = EMPTY;
  }

  /* update castling rights */
  if (piece_type(moved) == KING) {
    if (side==1) p->castling &= ~3;
    else p->castling &= ~12;
  }
  if (piece_type(moved) == ROOK) {
    if (from == fr_to_sq(0,0)) p->castling &= ~2;
    if (from == fr_to_sq(7,0)) p->castling &= ~1;
    if (from == fr_to_sq(0,7)) p->castling &= ~8;
    if (from == fr_to_sq(7,7)) p->castling &= ~4;
  }
  /* if rook captured on corner, update castling too */
  if ((m.flags & MF_CAPTURE) && !(m.flags & MF_EP)) {
    if (to == fr_to_sq(0,0)) p->castling &= ~2;
    if (to == fr_to_sq(7,0)) p->castling &= ~1;
    if (to == fr_to_sq(0,7)) p->castling &= ~8;
    if (to == fr_to_sq(7,7)) p->castling &= ~4;
  }

  /* set en-passant target after double pawn push */
  if (piece_type(moved) == PAWN) {
    int fr = sq_rank(from), tr = sq_rank(to);
    if (abs(tr - fr) == 2) {
      int epSq = to + (side==1 ? -8 : +8);
      p->ep = epSq;
    }
  }

  /* toggle side and fullmove */
  p->side = -p->side;
  if (p->side == 1) p->fullmove++;
}

static void unmake_move(Position *p, Move m, const Undo *u) {
  /* restore bookkeeping first */
  p->side = -p->side;
  p->castling = u->castling;
  p->ep = u->ep;
  p->halfmove = u->halfmove;
  p->fullmove = u->fullmove;

  int from = m.from, to = m.to;
  int side = p->side;

  /* undo castling rook */
  if (m.flags & MF_CASTLE) {
    p->b[u->rookFrom] = p->b[u->rookTo];
    p->b[u->rookTo] = EMPTY;
  }

  /* move piece back */
  p->b[from] = u->movedPiece;
  p->b[to] = EMPTY;

  /* restore captured */
  if (m.flags & MF_EP) {
    /* captured pawn returns to epCapturedSq */
    if (u->epCapturedSq >= 0) p->b[u->epCapturedSq] = u->captured;
  } else if (m.flags & MF_CAPTURE) {
    p->b[to] = u->captured;
  }

  (void)side;
}

static void gen_legal(Position *p, MoveList *out) {
  MoveList pseudo;
  gen_pseudo(p, &pseudo);
  out->n = 0;
  for (int i=0;i<pseudo.n;i++){
    Move m = pseudo.m[i];
    Undo u;
    make_move(p, m, &u);
    /* mover is -p->side after make */
    if (!is_in_check(p, -p->side)) out->m[out->n++] = m;
    unmake_move(p, m, &u);
  }
}

/* find legal move matching from,to,promo */
static bool find_legal_move(Position *p, int from, int to, int promo, Move *out) {
  MoveList ml;
  gen_legal(p, &ml);
  for (int i=0;i<ml.n;i++){
    Move m = ml.m[i];
    if ((int)m.from==from && (int)m.to==to) {
      if ((m.flags & MF_PROMO) == 0) {
        if (promo==0) { *out=m; return true; }
      } else {
        if (promo==0 && m.promo==QUEEN) { *out=m; return true; } /* default queen */
        if (promo!=0 && (int)m.promo==promo) { *out=m; return true; }
      }
    }
  }
  return false;
}

/* ------------------------- SAN/PGN ------------------------- */

static char piece_letter(int pt) {
  switch (pt) {
    case KNIGHT: return 'N';
    case BISHOP: return 'B';
    case ROOK:   return 'R';
    case QUEEN:  return 'Q';
    case KING:   return 'K';
    default:     return '\0'; /* pawn */
  }
}

static void move_to_uci_str(Move m, char out[8]) {
  char a[3], b[3];
  sq_to_alg(m.from, a);
  sq_to_alg(m.to, b);
  if (m.flags & MF_PROMO) {
    char pc = 'q';
    if (m.promo==KNIGHT) pc='n';
    else if (m.promo==BISHOP) pc='b';
    else if (m.promo==ROOK) pc='r';
    else pc='q';
    snprintf(out, 8, "%s%s%c", a, b, pc);
  } else {
    snprintf(out, 8, "%s%s", a, b);
  }
}

/* Determine if move gives check or mate (after move). */
static void check_suffix(const Position *before, Move m, char *suffixOut /*2 chars + null*/) {
  Position tmp = *before;
  Undo u;
  make_move(&tmp, m, &u);

  int opp = tmp.side; /* side to move after move is opponent */
  bool check = is_in_check(&tmp, opp);

  if (!check) {
    suffixOut[0]='\0';
    return;
  }

  MoveList ml;
  gen_legal(&tmp, &ml);
  if (ml.n == 0) { suffixOut[0]='#'; suffixOut[1]='\0'; }
  else { suffixOut[0]='+'; suffixOut[1]='\0'; }
}

/* SAN disambiguation helper */
static void san_disambiguation(const Position *p, Move m, char *out /* up to 3 chars */) {
  out[0]='\0';
  int moved = p->b[m.from];
  int pt = piece_type(moved);
  if (pt==PAWN || pt==KING) return;
  /* if another same piece can go to same 'to', disambiguate */
  Position tmp = *p; /* gen_legal expects non-const */
  MoveList ml;
  gen_legal(&tmp, &ml);

  int fromF = sq_file(m.from), fromR = sq_rank(m.from);

  bool anyOther = false;
  bool fileConflict = false;
  bool rankConflict = false;

  for (int i=0;i<ml.n;i++){
    Move x = ml.m[i];
    if (x.to != m.to || x.from == m.from) continue;
    int pc = p->b[x.from];
    if (pc==EMPTY) continue;
    if (piece_color(pc)!=piece_color(moved)) continue;
    if (piece_type(pc)!=pt) continue;
    anyOther = true;
    if (sq_file(x.from)==fromF) fileConflict = true;
    if (sq_rank(x.from)==fromR) rankConflict = true;
  }

  if (!anyOther) return;

  /* SAN: if file is enough use file, else rank, else both */
  char buf[4]={0};
  if (!fileConflict) {
    buf[0] = (char)('a'+fromF);
    buf[1] = '\0';
  } else if (!rankConflict) {
    buf[0] = (char)('1'+fromR);
    buf[1] = '\0';
  } else {
    buf[0] = (char)('a'+fromF);
    buf[1] = (char)('1'+fromR);
    buf[2] = '\0';
  }
  strcpy(out, buf);
}

static void san_for_move(const Position *p, Move m, char out[32]) {
  out[0]='\0';
  int moved = p->b[m.from];
  int pt = piece_type(moved);

  /* castling */
  if (m.flags & MF_CASTLE) {
    if (sq_file(m.to)==6) strcpy(out, "O-O");
    else strcpy(out, "O-O-O");
    char suf[2]; check_suffix(p, m, suf);
    strcat(out, suf);
    return;
  }

  char dst[3]; sq_to_alg(m.to, dst);
  char suf[2]; check_suffix(p, m, suf);

  bool capture = (m.flags & MF_CAPTURE) != 0;
  char disamb[4]; san_disambiguation(p, m, disamb);

  if (pt == PAWN) {
    if (capture) {
      char file = (char)('a' + sq_file(m.from));
      snprintf(out, 32, "%cx%s", file, dst);
    } else {
      snprintf(out, 32, "%s", dst);
    }
    if (m.flags & MF_PROMO) {
      char pl = piece_letter((int)m.promo);
      char promo[4];
      snprintf(promo, sizeof(promo), "=%c", pl?pl:'Q');
      strcat(out, promo);
    }
    strcat(out, suf);
    return;
  }

  char pl = piece_letter(pt);
  if (!pl) pl='?';

  char cap = capture ? 'x' : '\0';

  if (cap) snprintf(out, 32, "%c%sx%s", pl, disamb, dst);
  else     snprintf(out, 32, "%c%s%s",  pl, disamb, dst);

  if (m.flags & MF_PROMO) {
    char pr = piece_letter((int)m.promo);
    char promo[4];
    snprintf(promo, sizeof(promo), "=%c", pr?pr:'Q');
    strcat(out, promo);
  }

  strcat(out, suf);
}

static void print_pgn(const Game *g) {
  /* basic PGN mainline (no tags). */
  for (int i=0;i<g->ply;i++){
    if (i%2==0) {
      int moveNo = (i/2)+1;
      printf("%d. %s", moveNo, g->san[i]);
    } else {
      printf(" %s", g->san[i]);
    }
    if (i+1<g->ply) printf(" ");
  }
  printf("\n");
}

/* ------------------------- Initialization ------------------------- */

static void set_startpos(Position *p) {
  memset(p, 0, sizeof(*p));
  for (int i=0;i<64;i++) p->b[i]=EMPTY;

  /* pawns */
  for (int f=0;f<8;f++){
    p->b[fr_to_sq(f,1)] = PAWN;
    p->b[fr_to_sq(f,6)] = -PAWN;
  }
  /* rooks */
  p->b[fr_to_sq(0,0)] = ROOK;  p->b[fr_to_sq(7,0)] = ROOK;
  p->b[fr_to_sq(0,7)] = -ROOK; p->b[fr_to_sq(7,7)] = -ROOK;
  /* knights */
  p->b[fr_to_sq(1,0)] = KNIGHT; p->b[fr_to_sq(6,0)] = KNIGHT;
  p->b[fr_to_sq(1,7)] = -KNIGHT; p->b[fr_to_sq(6,7)] = -KNIGHT;
  /* bishops */
  p->b[fr_to_sq(2,0)] = BISHOP; p->b[fr_to_sq(5,0)] = BISHOP;
  p->b[fr_to_sq(2,7)] = -BISHOP; p->b[fr_to_sq(5,7)] = -BISHOP;
  /* queens */
  p->b[fr_to_sq(3,0)] = QUEEN;
  p->b[fr_to_sq(3,7)] = -QUEEN;
  /* kings */
  p->b[fr_to_sq(4,0)] = KING;
  p->b[fr_to_sq(4,7)] = -KING;

  p->side = 1;
  p->castling = 1|2|4|8;
  p->ep = -1;
  p->halfmove = 0;
  p->fullmove = 1;
}

static void game_reset(Game *g) {
  memset(g, 0, sizeof(*g));
  set_startpos(&g->pos);
  g->ply = 0;
}

/* ------------------------- UI ------------------------- */

static char piece_char(int pc) {
  int pt = piece_type(pc);
  char c = '.';
  switch (pt) {
    case PAWN: c='P'; break;
    case KNIGHT: c='N'; break;
    case BISHOP: c='B'; break;
    case ROOK: c='R'; break;
    case QUEEN: c='Q'; break;
    case KING: c='K'; break;
    default: c='.'; break;
  }
  if (pc<0) c = (char)tolower((unsigned char)c);
  return c;
}

static void build_hint_squares(Position *p, int selectedSq, bool hint[64]) {
  for (int i=0;i<64;i++) hint[i]=false;
  if (selectedSq<0) return;
  MoveList ml;
  gen_legal(p, &ml);
  for (int i=0;i<ml.n;i++){
    if ((int)ml.m[i].from == selectedSq) hint[ml.m[i].to] = true;
  }
}

static void draw(const Game *g, int cursorSq, int selectedSq, const char *status) {
  Position pos = g->pos; /* local copy for hints generation (gen_legal mutates) */
  bool hint[64];
  build_hint_squares(&pos, selectedSq, hint);

  term_write("\x1b[H\x1b[2J"); /* home + clear */
  term_printf("termchess  |  arrows:move  space/enter:select/move  u:undo  U:undo2  e:engine  q:quit\n");
  term_printf("Side to move: %s   Castling:%c%c%c%c   EP:%s   Half:%d Full:%d\n",
              g->pos.side==1?"White":"Black",
              (g->pos.castling&1)?'K':'-',
              (g->pos.castling&2)?'Q':'-',
              (g->pos.castling&4)?'k':'-',
              (g->pos.castling&8)?'q':'-',
              (g->pos.ep>=0? (char[3]){(char)('a'+sq_file(g->pos.ep)), (char)('1'+sq_rank(g->pos.ep)), 0} : "-"),
              g->pos.halfmove, g->pos.fullmove);

  /* board + moves on right */
  const int boardX = 1;
  const int movesX = 35;

  for (int r=7;r>=0;r--){
    term_printf("\x1b[%d;%dH", 4+(7-r), boardX);
    term_printf("%d ", r+1);
    for (int f=0;f<8;f++){
      int sq = fr_to_sq(f,r);
      bool dark = ((f+r)&1);
      int bg = dark ? 48 : 47; /* black/white background */
      int fg = 30; /* black text */
      int pc = g->pos.b[sq];
      char pcch = piece_char(pc);

      /* piece colors */
      if (pc>0) fg = 34;        /* blue for white */
      else if (pc<0) fg = 31;   /* red for black */
      else fg = 30;

      /* highlighting priority */
      if (sq == cursorSq) {
        /* cursor: invert-ish */
        term_printf("\x1b[%d;%dm", 7, 0); /* reverse */
        term_printf(" %c ", pcch=='.'?' ':pcch);
        term_write("\x1b[0m");
        continue;
      }
      if (sq == selectedSq) {
        /* selected: yellow background */
        term_printf("\x1b[43;%dm", fg);
        term_printf(" %c ", pcch=='.'?' ':pcch);
        term_write("\x1b[0m");
        continue;
      }
      if (hint[sq]) {
        /* legal destination hint: green background */
        term_printf("\x1b[42;%dm", fg);
        term_printf(" %c ", pcch=='.'?'·':pcch);
        term_write("\x1b[0m");
        continue;
      }

      term_printf("\x1b[%d;%dm", bg, fg);
      term_printf(" %c ", pcch=='.'?' ':pcch);
      term_write("\x1b[0m");
    }

    /* right side: PGN-ish move list lines */
    term_printf("\x1b[%d;%dH", 4+(7-r), movesX);
    /* Print a wrapped view: show last ~16 plies */
    int start = g->ply - 16;
    if (start < 0) start = 0;
    int line = 0;
    /* Each rank line prints one "line" of moves: keep simple */
    int targetLine = 7-r;
    int idx = start;
    char linebuf[256]; linebuf[0]=0;
    int curLine = 0;
    while (idx < g->ply && curLine <= targetLine) {
      /* build one line up to ~40 chars */
      linebuf[0]=0;
      size_t used=0;
      while (idx < g->ply) {
        char token[64];
        if (idx%2==0) {
          int moveNo = idx/2+1;
          snprintf(token, sizeof(token), "%d.%s ", moveNo, g->san[idx]);
        } else {
          snprintf(token, sizeof(token), "%s ", g->san[idx]);
        }
        size_t len = strlen(token);
        if (used + len > 40 && used>0) break;
        strncat(linebuf, token, sizeof(linebuf)-strlen(linebuf)-1);
        used += len;
        idx++;
      }
      if (curLine == targetLine) {
        term_printf("%s", linebuf);
      }
      curLine++;
      line++;
    }
  }

  term_printf("\x1b[%d;%dH", 13, 1);
  term_write("    a  b  c  d  e  f  g  h\n");

  term_printf("\x1b[%d;%dH", 15, 1);
  term_printf("Status: %s\x1b[0m\n", status?status:"");
  term_write("\x1b[0m");
  fflush(stdout);
}

/* ------------------------- Input ------------------------- */

typedef enum {
  KEY_NONE=0,
  KEY_UP=1001, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
  KEY_ENTER=13,
  KEY_ESC=27,
  KEY_BACKSPACE=127
} Key;

static int read_key(void) {
  unsigned char c;
  int n = (int)read(STDIN_FILENO, &c, 1);
  if (n<=0) return KEY_NONE;

  if (c == 27) {
    /* escape sequence */
    unsigned char seq[2];
    int n1 = (int)read(STDIN_FILENO, &seq[0], 1);
    int n2 = (int)read(STDIN_FILENO, &seq[1], 1);
    if (n1==1 && n2==1 && seq[0]=='[') {
      switch (seq[1]) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        default: return KEY_ESC;
      }
    }
    return KEY_ESC;
  }

  if (c=='\r' || c=='\n') return KEY_ENTER;
  return (int)c;
}

/* ------------------------- Playing moves ------------------------- */

static bool game_push_move(Game *g, Move m) {
  if (g->ply >= MAX_PLY) return false;

  /* compute SAN before making move */
  char san[32];
  san_for_move(&g->pos, m, san);

  Undo u;
  make_move(&g->pos, m, &u);

  g->undos[g->ply] = u;
  g->moves[g->ply] = m;
  strncpy(g->san[g->ply], san, sizeof(g->san[g->ply])-1);
  g->san[g->ply][sizeof(g->san[g->ply])-1] = 0;
  move_to_uci_str(m, g->uci[g->ply]);

  g->ply++;
  return true;
}

static bool game_undo(Game *g, int plies) {
  while (plies-- > 0) {
    if (g->ply <= 0) return false;
    g->ply--;
    Move m = g->moves[g->ply];
    Undo u = g->undos[g->ply];
    unmake_move(&g->pos, m, &u);
  }
  return true;
}

static bool parse_uci_coords(const char *s, int *from, int *to, int *promo) {
  if (!s || strlen(s) < 4) return false;
  int ff = s[0]-'a', fr = s[1]-'1';
  int tf = s[2]-'a', tr = s[3]-'1';
  if (!in_bounds(ff,fr) || !in_bounds(tf,tr)) return false;
  *from = fr_to_sq(ff,fr);
  *to = fr_to_sq(tf,tr);
  *promo = 0;
  if (strlen(s) >= 5) {
    char c = (char)tolower((unsigned char)s[4]);
    if (c=='q') *promo=QUEEN;
    else if (c=='r') *promo=ROOK;
    else if (c=='b') *promo=BISHOP;
    else if (c=='n') *promo=KNIGHT;
  }
  return true;
}

/* Use legal-move matching to interpret UCI (handles castles/ep correctly). */
static bool find_move_by_uci(Position *p, const char *uci, Move *out) {
  int from,to,promo;
  if (!parse_uci_coords(uci, &from,&to,&promo)) return false;
  return find_legal_move(p, from,to,promo, out);
}

/* ------------------------- Engine move request ------------------------- */

static int engine_bestmove(Engine *e, const Game *g, int movetime_ms, char bestmoveOut[16]) {
  /* Build "position startpos moves ..." */
  char poscmd[8192];
  strcpy(poscmd, "position startpos");
  if (g->ply > 0) strcat(poscmd, " moves ");
  for (int i=0;i<g->ply;i++){
    strcat(poscmd, g->uci[i]);
    if (i+1<g->ply) strcat(poscmd, " ");
  }

  engine_send(e, "%s", poscmd);
  engine_send(e, "go movetime %d", movetime_ms);

  char line[4096];
  while (fgets(line, sizeof(line), e->from)) {
    /* look for "bestmove ..." */
    if (strncmp(line, "bestmove ", 9)==0) {
      char mv[32]={0};
      sscanf(line+9, "%31s", mv);
      strncpy(bestmoveOut, mv, 15);
      bestmoveOut[15]=0;
      return 0;
    }
    /* ignore info lines */
  }
  return -1;
}

/* ------------------------- Main ------------------------- */

static void usage(const char *argv0) {
  fprintf(stderr,
    "Usage:\n"
    "  %s                      (2-player local)\n"
    "  %s stockfish            (engine plays Black)\n"
    "  %s --engine-white stockfish\n"
    "  %s --engine-black stockfish\n",
    argv0, argv0, argv0, argv0
  );
}

int main(int argc, char **argv) {
  signal(SIGINT, on_sigint);

  Engine eng;
  memset(&eng, 0, sizeof(eng));
  int engineSide = 0; /* 0 none, 1 white, -1 black */
  const char *enginePath = NULL;

  for (int i=1;i<argc;i++){
    if (strcmp(argv[i], "--engine-white")==0) engineSide = 1;
    else if (strcmp(argv[i], "--engine-black")==0) engineSide = -1;
    else if (strcmp(argv[i], "--help")==0 || strcmp(argv[i], "-h")==0) {
      usage(argv[0]);
      return 0;
    } else {
      enginePath = argv[i];
    }
  }
  if (enginePath && engineSide==0) engineSide = -1; /* default black */

  Game g;
  game_reset(&g);

  if (enginePath) {
    if (engine_start(&eng, enginePath) != 0) {
      fprintf(stderr, "Failed to start engine: %s\n", enginePath);
      return 1;
    }
    if (engine_init_uci(&eng) != 0) {
      fprintf(stderr, "Failed UCI init with engine.\n");
      engine_stop(&eng);
      return 1;
    }
  }

  term_enable_raw();

  int cursorSq = fr_to_sq(4,1); /* e2 */
  int selectedSq = -1;
  int pendingPromo = 0; /* 0 none; else piece type */
  char status[256] = "Ready.";

  /* If engine plays White, make it move first */
  if (eng.alive && engineSide == g.pos.side) {
    strcpy(status, "Engine thinking...");
    draw(&g, cursorSq, selectedSq, status);
    char bm[16];
    if (engine_bestmove(&eng, &g, 250, bm) == 0 && strcmp(bm, "0000")!=0) {
      Move m;
      if (find_move_by_uci(&g.pos, bm, &m)) {
        game_push_move(&g, m);
        snprintf(status, sizeof(status), "Engine played %s", g.san[g.ply-1]);
      } else {
        snprintf(status, sizeof(status), "Engine bestmove not legal? %s", bm);
      }
    } else {
      snprintf(status, sizeof(status), "Engine has no move.");
    }
  }

  while (!g_should_quit) {
    draw(&g, cursorSq, selectedSq, status);

    int k = read_key();
    if (k == KEY_NONE) continue;

    if (k == 'q' || k == 'Q') {
      break;
    }

    if (pendingPromo) {
      int promo = 0;
      if (k=='q' || k=='Q') promo = QUEEN;
      else if (k=='r' || k=='R') promo = ROOK;
      else if (k=='b' || k=='B') promo = BISHOP;
      else if (k=='n' || k=='N') promo = KNIGHT;
      else promo = QUEEN;

      pendingPromo = promo;
      /* next move attempt will consume it; keep UI consistent */
    }

    if (k == KEY_UP) {
      int f=sq_file(cursorSq), r=sq_rank(cursorSq);
      if (r<7) cursorSq = fr_to_sq(f,r+1);
      continue;
    }
    if (k == KEY_DOWN) {
      int f=sq_file(cursorSq), r=sq_rank(cursorSq);
      if (r>0) cursorSq = fr_to_sq(f,r-1);
      continue;
    }
    if (k == KEY_LEFT) {
      int f=sq_file(cursorSq), r=sq_rank(cursorSq);
      if (f>0) cursorSq = fr_to_sq(f-1,r);
      continue;
    }
    if (k == KEY_RIGHT) {
      int f=sq_file(cursorSq), r=sq_rank(cursorSq);
      if (f<7) cursorSq = fr_to_sq(f+1,r);
      continue;
    }

    if (k == KEY_ESC || k == KEY_BACKSPACE) {
      selectedSq = -1;
      pendingPromo = 0;
      strcpy(status, "Selection cleared.");
      continue;
    }

    if (k=='u') {
      if (game_undo(&g, 1)) strcpy(status, "Undid 1 ply.");
      else strcpy(status, "Nothing to undo.");
      selectedSq = -1;
      pendingPromo = 0;
      continue;
    }
    if (k=='U') {
      if (game_undo(&g, 2)) strcpy(status, "Undid 2 plies.");
      else strcpy(status, "Not enough moves to undo 2 plies.");
      selectedSq = -1;
      pendingPromo = 0;
      continue;
    }

    if (k=='e' || k=='E') {
      if (!eng.alive) {
        strcpy(status, "No engine running.");
        continue;
      }
      strcpy(status, "Engine thinking...");
      draw(&g, cursorSq, selectedSq, status);
      char bm[16];
      if (engine_bestmove(&eng, &g, 250, bm) == 0 && strcmp(bm, "0000")!=0) {
        Move m;
        if (find_move_by_uci(&g.pos, bm, &m)) {
          game_push_move(&g, m);
          snprintf(status, sizeof(status), "Engine played %s", g.san[g.ply-1]);
          selectedSq = -1;
          pendingPromo = 0;
        } else {
          snprintf(status, sizeof(status), "Engine bestmove not legal? %s", bm);
        }
      } else {
        snprintf(status, sizeof(status), "Engine has no move.");
      }
      continue;
    }

    if (k == KEY_ENTER || k == ' ') {
      if (selectedSq < 0) {
        int pc = g.pos.b[cursorSq];
        if (pc!=EMPTY && piece_color(pc)==g.pos.side) {
          selectedSq = cursorSq;
          pendingPromo = 0;
          char alg[3]; sq_to_alg(selectedSq, alg);
          snprintf(status, sizeof(status), "Selected %c on %s. Choose destination.", piece_char(pc), alg);
        } else {
          strcpy(status, "Select a piece of the side to move.");
        }
      } else {
        int from = selectedSq;
        int to = cursorSq;

        int promo = pendingPromo; /* 0 => auto queen if needed */
        Move m;
        if (find_legal_move(&g.pos, from, to, promo, &m)) {
          /* if promotion and promo not specified: prompt once */
          if ((m.flags & MF_PROMO) && promo==0) {
            strcpy(status, "Promotion: press q r b n (default q), then press Space/Enter again.");
            pendingPromo = QUEEN; /* default choice */
            continue;
          }

          if (game_push_move(&g, m)) {
            snprintf(status, sizeof(status), "Played %s", g.san[g.ply-1]);
          } else {
            strcpy(status, "Move history full.");
          }

          selectedSq = -1;
          pendingPromo = 0;

          /* auto engine move if configured for side-to-move */
          if (eng.alive && engineSide == g.pos.side) {
            strcpy(status, "Engine thinking...");
            draw(&g, cursorSq, selectedSq, status);
            char bm[16];
            if (engine_bestmove(&eng, &g, 250, bm) == 0 && strcmp(bm, "0000")!=0) {
              Move em;
              if (find_move_by_uci(&g.pos, bm, &em)) {
                game_push_move(&g, em);
                snprintf(status, sizeof(status), "Engine played %s", g.san[g.ply-1]);
              } else {
                snprintf(status, sizeof(status), "Engine bestmove not legal? %s", bm);
              }
            } else {
              snprintf(status, sizeof(status), "Engine has no move.");
            }
          }
        } else {
          strcpy(status, "Illegal move.");
        }
      }
      continue;
    }

    /* any other key: ignore */
  }

  term_disable_raw();
  term_write("\x1b[2J\x1b[H");
  if (eng.alive) engine_stop(&eng);

  printf("PGN:\n");
  print_pgn(&g);
  return 0;
}
