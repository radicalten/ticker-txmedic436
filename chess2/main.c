/* Cfish: Single-File Amalgamated Version */
/* Original code by Syzygy1 (GPv3 License) */

/* --- System Headers --- */
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <float.h>
//#include <immintrin.h>
//#include <intrin.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
//#include <nmmintrin.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
//#include <windows.h>
//#include <xmmintrin.h>


/* ==========================================
   FILE: types.h
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2017 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef TYPES_H
#define TYPES_H


// When compiling with provided Makefile (e.g. for Linux and OSX),
// configuration is done automatically. To get started type 'make help'.
//
// When Makefile is not used (e.g. with Microsoft Visual Studio) some
// switches need to be set manually:
//
// -DNDEBUG      | Disable debugging mode. Always use this for release.
//
// -DNO_PREFETCH | Disable use of prefetch asm-instruction. You may need
//               | this to run on some very old machines.
//
// -DUSE_POPCNT  | Add runtime support for use of popcnt asm-instruction.
//               | Works only in 64-bit mode and requires hardware with
//               | popcnt support.
//
// -DUSE_PEXT    | Add runtime support for use of pext asm-instruction.
//               | Works only in 64-bit mode and requires hardware with
//               | pext support.

#ifndef NDEBUG
#endif
#ifdef _WIN32
#endif

#define INLINE static inline __attribute__((always_inline))
#define NOINLINE __attribute__((noinline))

// Declaring pure functions as pure seems not to help. (Investigate later.)
//#define PURE __attribute__((pure))
#define PURE

#if defined __has_attribute
#if __has_attribute(minsize)
#define SMALL __attribute__((minsize))
#elif __has_attribute(optimize)
#define SMALL __attribute__((optimize("Os")))
#endif
#endif

#ifndef SMALL
#define SMALL
#endif

// Predefined macros hell:
//
// __GNUC__           Compiler is gcc, Clang or Intel on Linux
// __INTEL_COMPILER   Compiler is Intel
// _MSC_VER           Compiler is MSVC or Intel on Windows
// _WIN32             Building on Windows (any)
// _WIN64             Building on Windows 64 bit

#if defined(_WIN64) && defined(_MSC_VER) // No Makefile used
#  define IS_64BIT
#endif

#if defined(USE_POPCNT) && (defined(__INTEL_COMPILER) || defined(_MSC_VER))
#endif

#if !defined(NO_PREFETCH) && (defined(__INTEL_COMPILER) || defined(_MSC_VER))
#endif

#if defined(USE_PEXT)
#  define pext(b, m) _pext_u64(b, m)
#else
#  define pext(b, m) (0)
#endif

#ifdef USE_POPCNT
#define HasPopCnt 1
#else
#define HasPopCnt 0
#endif

#ifdef USE_PEXT
#define HasPext 1
#else
#define HasPext 0
#endif

#ifdef IS_64BIT
#define Is64Bit 1
#else
#define Is64Bit 0
#endif

#ifdef NUMA
#define HasNuma 1
#else
#define HasNuma 0
#endif

typedef uint64_t Key;
typedef uint64_t Bitboard;

enum { MAX_MOVES = 256, MAX_PLY = 246 };

// A move needs 16 bits to be stored
//
// bit  0- 5: destination square (from 0 to 63)
// bit  6-11: origin square (from 0 to 63)
// bit 12-13: promotion piece type - 2 (from KNIGHT-2 to QUEEN-2)
// bit 14-15: special move flag: promotion (1), en passant (2), castling (3)
// NOTE: EN-PASSANT bit is set only when a pawn can be captured
//
// Null move (MOVE_NULL) is encoded as a2a2.

enum { MOVE_NONE = 0, MOVE_NULL = 65 };

enum { NORMAL, PROMOTION, ENPASSANT, CASTLING };

enum { WHITE = false, BLACK = true };

enum { KING_SIDE, QUEEN_SIDE };

enum {
  NO_CASTLING = 0, WHITE_OO = 1, WHITE_OOO = 2,
  BLACK_OO = 4, BLACK_OOO = 8, ANY_CASTLING = 15
};

INLINE int make_castling_right(int c, int s)
{
  return c == WHITE ? s == QUEEN_SIDE ? WHITE_OOO : WHITE_OO
                    : s == QUEEN_SIDE ? BLACK_OOO : BLACK_OO;
}

enum { PHASE_ENDGAME = 0, PHASE_MIDGAME = 128 };
enum { MG, EG };

enum {
  SCALE_FACTOR_DRAW = 0, SCALE_FACTOR_NORMAL = 64,
  SCALE_FACTOR_MAX = 128, SCALE_FACTOR_NONE = 255
};

enum { BOUND_NONE, BOUND_UPPER, BOUND_LOWER, BOUND_EXACT };

enum {
  VALUE_ZERO = 0, VALUE_DRAW = 0,
  VALUE_KNOWN_WIN = 10000, VALUE_MATE = 32000,
  VALUE_INFINITE = 32001, VALUE_NONE = 32002
};

#ifdef LONG_MATES
enum { MAX_MATE_PLY = 600 };
#else
enum { MAX_MATE_PLY = MAX_PLY };
#endif

enum {
  VALUE_TB_WIN_IN_MAX_PLY  =  VALUE_MATE - 2 * MAX_PLY,
  VALUE_TB_LOSS_IN_MAX_PLY = -VALUE_MATE + 2 * MAX_PLY,
  VALUE_MATE_IN_MAX_PLY    =  VALUE_MATE -     MAX_PLY,
  VALUE_MATED_IN_MAX_PLY   = -VALUE_MATE +     MAX_PLY
};

enum {
  PawnValueMg   = 126,   PawnValueEg   = 208,
  KnightValueMg = 781,   KnightValueEg = 854,
  BishopValueMg = 825,   BishopValueEg = 915,
  RookValueMg   = 1276,  RookValueEg   = 1380,
  QueenValueMg  = 2538,  QueenValueEg  = 2682,

  MidgameLimit  = 15258, EndgameLimit = 3915
};

enum { PAWN = 1, KNIGHT, BISHOP, ROOK, QUEEN, KING };

enum {
  W_PAWN = 1, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
  B_PAWN = 9, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING
};

enum {
  DEPTH_QS_CHECKS     =  0,
  DEPTH_QS_NO_CHECKS  = -1,
  DEPTH_QS_RECAPTURES = -5,
  DEPTH_NONE = -6,
  DEPTH_OFFSET = -7
};

enum {
  SQ_A1, SQ_B1, SQ_C1, SQ_D1, SQ_E1, SQ_F1, SQ_G1, SQ_H1,
  SQ_A2, SQ_B2, SQ_C2, SQ_D2, SQ_E2, SQ_F2, SQ_G2, SQ_H2,
  SQ_A3, SQ_B3, SQ_C3, SQ_D3, SQ_E3, SQ_F3, SQ_G3, SQ_H3,
  SQ_A4, SQ_B4, SQ_C4, SQ_D4, SQ_E4, SQ_F4, SQ_G4, SQ_H4,
  SQ_A5, SQ_B5, SQ_C5, SQ_D5, SQ_E5, SQ_F5, SQ_G5, SQ_H5,
  SQ_A6, SQ_B6, SQ_C6, SQ_D6, SQ_E6, SQ_F6, SQ_G6, SQ_H6,
  SQ_A7, SQ_B7, SQ_C7, SQ_D7, SQ_E7, SQ_F7, SQ_G7, SQ_H7,
  SQ_A8, SQ_B8, SQ_C8, SQ_D8, SQ_E8, SQ_F8, SQ_G8, SQ_H8,
  SQ_NONE
};

enum {
  NORTH = 8, EAST = 1, SOUTH = -8, WEST = -1,
  NORTH_EAST = NORTH + EAST, SOUTH_EAST = SOUTH + EAST,
  NORTH_WEST = NORTH + WEST, SOUTH_WEST = SOUTH + WEST,
};

enum { FILE_A, FILE_B, FILE_C, FILE_D, FILE_E, FILE_F, FILE_G, FILE_H };

enum { RANK_1, RANK_2, RANK_3, RANK_4, RANK_5, RANK_6, RANK_7, RANK_8 };

typedef uint32_t Move;
typedef int32_t Phase;
typedef int32_t Value;
typedef bool Color;
typedef uint32_t Piece;
typedef uint32_t PieceType;
typedef int32_t Depth;
typedef uint32_t Square;
typedef uint32_t File;
typedef uint32_t Rank;

// Score type stores a middlegame and an endgame value in a single integer.
// The endgame value goes in the upper 16 bits, the middlegame value in
// the lower 16 bits.

typedef uint32_t Score;

enum { SCORE_ZERO };

#define make_score(mg,eg) ((((unsigned)(eg))<<16) + (mg))

// Casting an out-of-range value to int16_t is implementation-defined, but
// we assume the implementation does the right thing.
INLINE Value eg_value(Score s)
{
  return (int16_t)((s + 0x8000) >> 16);
}

INLINE Value mg_value(Score s)
{
  return (int16_t)s;
}

/// Division of a Score must be handled separately for each tEerm
INLINE Score score_divide(Score s, int i)
{
  return make_score(mg_value(s) / i, eg_value(s) / i);
}

extern Value PieceValue[2][16];

extern uint32_t NonPawnPieceValue[16];

#define SQUARE_FLIP(sq) ((sq) ^ 0x38)

#define mate_in(ply) ((Value)(VALUE_MATE - (ply)))
#define mated_in(ply) ((Value)(-VALUE_MATE + (ply)))
#define make_square(f,r) ((Square)(((r) << 3) + (f)))
#define make_piece(c,pt) ((Piece)(((c) << 3) + (pt)))
#define type_of_p(p) ((p) & 7)
#define color_of(p) ((p) >> 3)
// since Square is now unsigned, no need to test for s >= SQ_A1
#define square_is_ok(s) ((Square)(s) <= SQ_H8)
#define file_of(s) ((s) & 7)
#define rank_of(s) ((s) >> 3)
#define relative_square(c,s) ((Square)((s) ^ ((c) * 56)))
#define relative_rank(c,r) ((r) ^ ((c) * 7))
#define relative_rank_s(c,s) relative_rank(c,rank_of(s))
#define pawn_push(c) ((c) == WHITE ? 8 : -8)
#define from_sq(m) ((Square)((m)>>6) & 0x3f)
#define to_sq(m) ((Square)((m) & 0x3f))
#define from_to(m) ((m) & 0xfff)
#define type_of_m(m) ((m) >> 14)
#define promotion_type(m) ((((m)>>12) & 3) + KNIGHT)
#define make_move(from,to) ((Move)((to) | ((from) << 6)))
#define reverse_move(m) (make_move(to_sq(m), from_sq(m)))
#define make_promotion(from,to,pt) ((Move)((to) | ((from)<<6) | (PROMOTION<<14) | (((pt)-KNIGHT)<<12)))
#define make_enpassant(from,to) ((Move)((to) | ((from)<<6) | (ENPASSANT<<14)))
#define make_castling(from,to) ((Move)((to) | ((from)<<6) | (CASTLING<<14)))
#define move_is_ok(m) (from_sq(m) != to_sq(m))

INLINE bool opposite_colors(Square s1, Square s2)
{
  Square s = s1 ^ s2;
  return ((s >> 3) ^ s) & 1;
}

INLINE Key make_key(uint64_t seed)
{
  return seed * 6364136223846793005ULL + 1442695040888963407ULL;
}

typedef struct Position Position;
typedef struct LimitsType LimitsType;
typedef struct RootMove RootMove;
typedef struct RootMoves RootMoves;
typedef struct PawnEntry PawnEntry;
typedef struct MaterialEntry MaterialEntry;

enum { MAX_LPH = 4 };

typedef Move CounterMoveStat[16][64];
typedef int16_t PieceToHistory[16][64];
typedef PieceToHistory CounterMoveHistoryStat[2][2][16][64];
typedef int16_t ButterflyHistory[2][4096];
typedef int16_t CapturePieceToHistory[16][64][8];
typedef int16_t LowPlyHistory[MAX_LPH][4096];

struct ExtMove {
  Move move;
  int value;
};

typedef struct ExtMove ExtMove;

struct PSQT {
  Score psq[16][64];
};

extern struct PSQT psqt;

#undef max
#undef min

#define MAX(T) INLINE T max_##T(T a, T b) { return a > b ? a : b; }
MAX(int)
MAX(uint64_t)
MAX(unsigned)
MAX(int64_t)
MAX(int16_t)
MAX(uint8_t)
MAX(double)
MAX(size_t)
MAX(long)
#undef MAX

#define MIN(T) INLINE T min_##T(T a, T b) { return a < b ? a : b; }
MIN(int)
MIN(uint64_t)
MIN(unsigned)
MIN(int64_t)
MIN(int16_t)
MIN(uint8_t)
MIN(double)
MIN(size_t)
MIN(long)
#undef MIN

#define CLAMP(T) INLINE T clamp_##T(T a, T b, T c) { return a < b ? b : a > c ? c : a; }
CLAMP(int)
CLAMP(uint64_t)
CLAMP(unsigned)
CLAMP(int64_t)
CLAMP(int16_t)
CLAMP(uint8_t)
CLAMP(double)
CLAMP(size_t)
CLAMP(long)
#undef CLAMP

#ifndef __APPLE__
#define TEMPLATE(F,a,...) _Generic((a), \
    int: F##_int,              \
    uint64_t: F##_uint64_t,    \
    unsigned: F##_unsigned,    \
    int64_t: F##_int64_t,      \
    int16_t: F##_int16_t,      \
    uint8_t: F##_uint8_t,      \
    double: F##_double         \
) (a,__VA_ARGS__)
#else
#define TEMPLATE(F,a,...) _Generic((a), \
    int: F##_int,              \
    uint64_t: F##_uint64_t,    \
    unsigned: F##_unsigned,    \
    int64_t: F##_int64_t,      \
    int16_t: F##_int16_t,      \
    uint8_t: F##_uint8_t,      \
    size_t: F##_size_t,        \
    long: F##_long,            \
    double: F##_double         \
) (a,__VA_ARGS__)
#endif

#define max(a,b) TEMPLATE(max,a,b)
#define min(a,b) TEMPLATE(min,a,b)
#define clamp(a,b,c) TEMPLATE(clamp,a,b,c)

#ifdef NDEBUG
#define assume(x) do { if (!(x)) __builtin_unreachable(); } while (0)
#else
#define assume(x) assert(x)
#endif

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#ifdef NNUE

struct DirtyPiece {
  int dirtyNum;
  Piece pc[3];
  Square from[3];
  Square to[3];
};

typedef struct DirtyPiece DirtyPiece;

#endif

#endif

/* ==========================================
   FILE: misc.h
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef MISC_H
#define MISC_H

#ifndef _WIN32
#endif


void print_engine_info(bool to_uci);
void print_compiler_info(void);

// prefetch() preloads the given address in L1/L2 cache. This is
// a non-blocking function that doesn't stall the CPU waiting for data
// to be loaded from memory, which can be quite slow.

INLINE void prefetch(void *addr)
{
#ifndef NO_PREFETCH

#if defined(__INTEL_COMPILER)
// This hack prevents prefetches from being optimized away by
// Intel compiler. Both MSVC and gcc seem not be affected by this.
  __asm__ ("");
#endif

#if defined(__INTEL_COMPILER) || defined(_MSC_VER)
  _mm_prefetch((char *)addr, _MM_HINT_T0);
#else
  __builtin_prefetch(addr);
#endif
#else
  (void)addr;
#endif
}

INLINE void prefetch2(void *addr)
{
  prefetch(addr);
  prefetch((uint8_t *)addr + 64);
}

typedef int64_t TimePoint; // A value in milliseconds

INLINE TimePoint now(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return 1000 * (uint64_t)tv.tv_sec + (uint64_t)tv.tv_usec / 1000;
}

#ifdef _WIN32
bool large_pages_supported(void);
extern size_t largePageMinimum;

typedef HANDLE FD;
#define FD_ERR INVALID_HANDLE_VALUE
typedef HANDLE map_t;
typedef struct {
  void *ptr;
} alloc_t;

void flockfile(FILE *F);
void funlockfile(FILE *F);

#else /* Unix */

typedef int FD;
#define FD_ERR -1
typedef size_t map_t;
typedef struct {
  void *ptr;
  size_t size;
} alloc_t;

#endif

ssize_t getline(char **lineptr, size_t *n, FILE *stream);

FD open_file(const char *name);
void close_file(FD fd);
size_t file_size(FD fd);
const void *map_file(FD fd, map_t *map);
void unmap_file(const void *data, map_t map);
void *allocate_memory(size_t size, bool lp, alloc_t *alloc);
void free_memory(alloc_t *alloc);

struct PRNG
{
  uint64_t s;
};

typedef struct PRNG PRNG;

void prng_init(PRNG *rng, uint64_t seed);
uint64_t prng_rand(PRNG *rng);
uint64_t prng_sparse_rand(PRNG *rng);

INLINE uint64_t mul_hi64(uint64_t a, uint64_t b)
{
#if defined(__GNUC__) && defined(IS_64BIT)
  __extension__ typedef unsigned __int128 uint128;
  return ((uint128)a * (uint128)b) >> 64;
#else
  uint64_t aL = (uint32_t)a, aH = a >> 32;
  uint64_t bL = (uint32_t)b, bH = b >> 32;
  uint64_t c1 = (aL * bL) >> 32;
  uint64_t c2 = aH * bL + c1;
  uint64_t c3 = aL * bH + (uint32_t)c2;
  return aH * bH + (c2 >> 32) + (c3 >> 32);
#endif
}

INLINE bool is_little_endian(void)
{
  int num = 1;
  return *(uint8_t *)&num == 1;
}

INLINE uint32_t from_le_u32(uint32_t v)
{
  return is_little_endian() ? v : __builtin_bswap32(v);
}

INLINE uint16_t from_le_u16(uint16_t v)
{
  return is_little_endian() ? v : __builtin_bswap16(v);
}

INLINE uint64_t from_be_u64(uint64_t v)
{
  return is_little_endian() ? __builtin_bswap64(v) : v;
}

INLINE uint32_t from_be_u32(uint32_t v)
{
  return is_little_endian() ? __builtin_bswap32(v) : v;
}

INLINE uint16_t from_be_u16(uint16_t v)
{
  return is_little_endian() ? __builtin_bswap16(v) : v;
}

INLINE uint32_t read_le_u32(const void *p)
{
  return from_le_u32(*(uint32_t *)p);
}

INLINE uint16_t read_le_u16(const void *p)
{
  return from_le_u16(*(uint16_t *)p);
}

INLINE uint32_t readu_le_u32(const void *p)
{
  const uint8_t *q = p;
  return q[0] | (q[1] << 8) | (q[2] << 16) | (q[3] << 24);
}

INLINE uint16_t readu_le_u16(const void *p)
{
  const uint8_t *q = p;
  return q[0] | (q[1] << 8);
}

#endif

/* ==========================================
   FILE: bitboard.h
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2018 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef BITBOARD_H
#define BITBOARD_H



void bitbases_init(void);
bool bitbases_probe(Square wksq, Square wpsq, Square bksq, Color us);

void bitboards_init(void);
void print_pretty(Bitboard b);

#define AllSquares (~0ULL)
#define DarkSquares  0xAA55AA55AA55AA55ULL
#define LightSquares (~DarkSquares)

#define FileABB 0x0101010101010101ULL
#define FileBBB (FileABB << 1)
#define FileCBB (FileABB << 2)
#define FileDBB (FileABB << 3)
#define FileEBB (FileABB << 4)
#define FileFBB (FileABB << 5)
#define FileGBB (FileABB << 6)
#define FileHBB (FileABB << 7)

#define Rank1BB 0xFFULL
#define Rank2BB (Rank1BB << (8 * 1))
#define Rank3BB (Rank1BB << (8 * 2))
#define Rank4BB (Rank1BB << (8 * 3))
#define Rank5BB (Rank1BB << (8 * 4))
#define Rank6BB (Rank1BB << (8 * 5))
#define Rank7BB (Rank1BB << (8 * 6))
#define Rank8BB (Rank1BB << (8 * 7))

#define QueenSide   (FileABB | FileBBB | FileCBB | FileDBB)
#define CenterFiles (FileCBB | FileDBB | FileEBB | FileFBB)
#define KingSide    (FileEBB | FileFBB | FileGBB | FileHBB)
#define Center      ((FileDBB | FileEBB) & (Rank4BB | Rank5BB))

extern uint8_t SquareDistance[64][64];

extern Bitboard SquareBB[64];
extern Bitboard FileBB[8];
extern Bitboard RankBB[8];
extern Bitboard ForwardRanksBB[2][8];
extern Bitboard BetweenBB[64][64];
extern Bitboard LineBB[64][64];
extern Bitboard DistanceRingBB[64][8];
extern Bitboard ForwardFileBB[2][64];
extern Bitboard PassedPawnSpan[2][64];
extern Bitboard PawnAttackSpan[2][64];
extern Bitboard PseudoAttacks[8][64];
extern Bitboard PawnAttacks[2][64];


INLINE __attribute__((pure)) Bitboard sq_bb(Square s)
{
  return SquareBB[s];
}

#if __x86_64__
INLINE Bitboard inv_sq(Bitboard b, Square s)
{
  __asm__("btcq %1, %0" : "+r" (b) : "r" ((uint64_t)s) : "cc");
  return b;
}
#else
INLINE Bitboard inv_sq(Bitboard b, Square s)
{
  return b ^ sq_bb(s);
}
#endif

INLINE bool more_than_one(Bitboard b)
{
  return b & (b - 1);
}


// rank_bb() and file_bb() return a bitboard representing all the squares on
// the given file or rank.

INLINE Bitboard rank_bb(Rank r)
{
  return RankBB[r];
}

INLINE Bitboard rank_bb_s(Square s)
{
  return RankBB[rank_of(s)];
}

INLINE Bitboard file_bb(File f)
{
  return FileBB[f];
}

INLINE Bitboard file_bb_s(Square s)
{
  return FileBB[file_of(s)];
}


// shift_bb() moves a bitboard one step along direction Direction.
INLINE Bitboard shift_bb(int Direction, Bitboard b)
{
  return  Direction == NORTH       ?  b << 8
        : Direction == SOUTH       ?  b >> 8
        : Direction == NORTH+NORTH ?  b << 16
        : Direction == SOUTH+SOUTH ?  b >> 16
        : Direction == EAST        ? (b & ~FileHBB) << 1
        : Direction == WEST        ? (b & ~FileABB) >> 1
        : Direction == NORTH_EAST  ? (b & ~FileHBB) << 9
        : Direction == SOUTH_EAST  ? (b & ~FileHBB) >> 7
        : Direction == NORTH_WEST  ? (b & ~FileABB) << 7
        : Direction == SOUTH_WEST  ? (b & ~FileABB) >> 9
        : 0;
}


// pawn_attacks_bb() returns the squares attacked by pawns of the given color
// from the squares in the given bitboard.

INLINE Bitboard pawn_attacks_bb(Bitboard b, const Color C)
{
  return C == WHITE ? shift_bb(NORTH_WEST, b) | shift_bb(NORTH_EAST, b)
                    : shift_bb(SOUTH_WEST, b) | shift_bb(SOUTH_EAST, b);
}


// pawn_double_attacks_bb() returns the pawn attacks for the given color
// from the squares in the given bitboard.

INLINE Bitboard pawn_double_attacks_bb(Bitboard b, const Color C)
{
  return C == WHITE ? shift_bb(NORTH_WEST, b) & shift_bb(NORTH_EAST, b)
                    : shift_bb(SOUTH_WEST, b) & shift_bb(SOUTH_EAST, b);
}


// adjacent_files_bb() returns a bitboard representing all the squares
// on the adjacent files of the given one.

INLINE Bitboard adjacent_files_bb(unsigned f)
{
  return shift_bb(EAST, FileBB[f]) | shift_bb(WEST, FileBB[f]);
}


// between_bb() returns a bitboard representing all the squares between
// the two given ones. For instance, between_bb(SQ_C4, SQ_F7) returns a
// bitboard with the bits for square d5 and e6 set. If s1 and s2 are not
// on the same rank, file or diagonal, 0 is returned.

INLINE Bitboard between_bb(Square s1, Square s2)
{
  return BetweenBB[s1][s2];
}


// forward_ranks_bb() returns a bitboard representing all the squares on
// all the ranks in front of the given one, from the point of view of the
// given color. For instance, forward_ranks_bb(BLACK, RANK_3) will return
// the squares on ranks 1 and 2.

INLINE Bitboard forward_ranks_bb(Color c, unsigned r)
{
  return ForwardRanksBB[c][r];
}


// forward_file_bb() returns a bitboard representing all the squares
// along the line in front of the given one, from the point of view of
// the given color:
//     ForwardFileBB[c][s] = forward_ranks_bb(c, rank_of(s)) & file_bb(s)

INLINE Bitboard forward_file_bb(Color c, Square s)
{
  return ForwardFileBB[c][s];
}


// pawn_attack_span() returns a bitboard representing all the squares
// that can be attacked by a pawn of the given color when it moves along
// its file, starting from the given square:
//     PawnAttackSpan[c][s] = forward_ranks_bb(c, rank_of(s)) & adjacent_files_bb(s);

INLINE Bitboard pawn_attack_span(Color c, Square s)
{
  return PawnAttackSpan[c][s];
}


// passed_pawn_span() returns a bitboard mask which can be used to test
// if a pawn of the given color and on the given square is a passed pawn:
//     PassedPawnSpan[c][s] = pawn_attack_span(c, s) | forward_bb(c, s)

INLINE Bitboard passed_pawn_span(Color c, Square s)
{
  return PassedPawnSpan[c][s];
}


// aligned() returns true if square s is on the line determined by move m.

INLINE uint64_t aligned(Move m, Square s)
{
  return ((Bitboard *)LineBB)[m & 4095] & sq_bb(s);
}


// distance() functions return the distance between x and y, defined as
// the number of steps for a king in x to reach y. Works with squares,
// ranks, files.

INLINE int distance(Square x, Square y)
{
  return SquareDistance[x][y];
}

INLINE unsigned distance_f(Square x, Square y)
{
  unsigned f1 = file_of(x), f2 = file_of(y);
  return f1 < f2 ? f2 - f1 : f1 - f2;
}

INLINE unsigned distance_r(Square x, Square y)
{
  unsigned r1 = rank_of(x), r2 = rank_of(y);
  return r1 < r2 ? r2 - r1 : r1 - r2;
}

#define attacks_bb_queen(s, occupied) (attacks_bb_bishop((s), (occupied)) | attacks_bb_rook((s), (occupied)))

#if defined(MAGIC_FANCY)
#elif defined(MAGIC_PLAIN)
#elif defined(MAGIC_BLACK)
#elif defined(BMI2_FANCY)
#elif defined(BMI2_PLAIN)
#elif defined(AVX2_BITBOARD)
#endif

INLINE Bitboard attacks_bb(int pt, Square s, Bitboard occupied)
{
  assert(pt != PAWN);

  switch (pt) {
  case BISHOP:
      return attacks_bb_bishop(s, occupied);
  case ROOK:
      return attacks_bb_rook(s, occupied);
  case QUEEN:
      return attacks_bb_queen(s, occupied);
  default:
      return PseudoAttacks[pt][s];
  }
}


// popcount() counts the number of non-zero bits in a bitboard.

INLINE int popcount(Bitboard b)
{
#ifndef USE_POPCNT

  extern uint8_t PopCnt16[1 << 16];
  union { Bitboard bb; uint16_t u[4]; } v = { b };
  return PopCnt16[v.u[0]] + PopCnt16[v.u[1]] + PopCnt16[v.u[2]] + PopCnt16[v.u[3]];

#elif defined(_MSC_VER) || defined(__INTEL_COMPILER)

  return (int)_mm_popcnt_u64(b);

#else // Assumed gcc or compatible compiler

  return __builtin_popcountll(b);

#endif
}


// lsb() and msb() return the least/most significant bit in a non-zero
// bitboard.

#if defined(__GNUC__)

INLINE int lsb(Bitboard b)
{
  assert(b);
  return __builtin_ctzll(b);
}

INLINE int msb(Bitboard b)
{
  assert(b);
  return 63 ^ __builtin_clzll(b);
}

#elif defined(_MSC_VER)

#if defined(_WIN64)

INLINE Square lsb(Bitboard b)
{
  assert(b);
  unsigned long idx;
  _BitScanForward64(&idx, b);
  return (Square) idx;
}

INLINE Square msb(Bitboard b)
{
  assert(b);
  unsigned long idx;
  _BitScanReverse64(&idx, b);
  return (Square) idx;
}

#else

INLINE Square lsb(Bitboard b)
{
  assert(b);
  unsigned long idx;
  if ((uint32_t)b) {
    _BitScanForward(&idx, (uint32_t)b);
    return idx;
  } else {
    _BitScanForward(&idx, (uint32_t)(b >> 32));
    return idx + 32;
  }
}

INLINE Square msb(Bitboard b)
{
  assert(b);
  unsigned long idx;
  if (b >> 32) {
    _BitScanReverse(&idx, (uint32_t)(b >> 32));
    return idx + 32;
  } else {
    _BitScanReverse(&idx, (uint32_t)b);
    return idx;
  }
}

#endif

#else

#error "Compiler not supported."

#endif


// pop_lsb() finds and clears the least significant bit in a non-zero
// bitboard.

INLINE Square pop_lsb(Bitboard* b)
{
  const Square s = lsb(*b);
  *b &= *b - 1;
  return s;
}


// frontmost_sq() and backmost_sq() return the square corresponding to the
// most/least advanced bit relative to the given color.

INLINE Square frontmost_sq(Color c, Bitboard b)
{
  return c == WHITE ? msb(b) : lsb(b);
}

INLINE Square  backmost_sq(Color c, Bitboard b)
{
  return c == WHITE ? lsb(b) : msb(b);
}

#endif

/* ==========================================
   FILE: position.h
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2017 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef POSITION_H
#define POSITION_H

#ifndef _WIN32
#endif


#ifdef NNUE
#endif

extern const char PieceToChar[];
extern Key matKey[16];

struct Zob {
  Key psq[16][64];
  Key enpassant[8];
  Key castling[16];
  Key side, noPawns;
};

extern struct Zob zob;

void psqt_init(void);
void zob_init(void);

// Stack struct stores information needed to restore a Position struct to
// its previous state when we retract a move.

struct Stack {
  // Copied when making a move
#ifndef NNUE_PURE
  Key pawnKey;
#endif
  Key materialKey;
#ifndef NNUE_PURE
  Score psq;
#endif
  union {
    uint16_t nonPawnMaterial[2];
    uint32_t nonPawn;
  };
  union {
    struct {
      uint8_t pliesFromNull;
      uint8_t rule50;
    };
    uint16_t plyCounters;
  };
  uint8_t castlingRights;

  // Not copied when making a move
  uint8_t capturedPiece;
  uint8_t epSquare;
  Key key;
  Bitboard checkersBB;

  // Original search stack data
  Move* pv;
  PieceToHistory *history;
  Move currentMove;
  Move excludedMove;
  Move killers[2];
  Value staticEval;
  Value statScore;
  int moveCount;
  bool ttPv;
  bool ttHit;
  uint8_t ply;

  // MovePicker data
  uint8_t stage;
  uint8_t recaptureSquare;
  uint8_t mp_ply;
  Move countermove;
  Depth depth;
  Move ttMove;
  Value threshold;
  Move mpKillers[2];
  ExtMove *cur, *endMoves, *endBadCaptures;

  // CheckInfo data
  Bitboard blockersForKing[2];
  union {
    struct {
      Bitboard pinnersForKing[2];
    };
    struct {
      Bitboard dummy;           // pinnersForKing[WHITE]
      Bitboard checkSquares[7]; // element 0 is pinnersForKing[BLACK]
    };
  };
  Square ksq;

#ifdef NNUE
  // NNUE data
  Accumulator accumulator;
  DirtyPiece dirtyPiece;
#endif
};

typedef struct Stack Stack;

#define StateCopySize offsetof(Stack, capturedPiece)
#define StateSize offsetof(Stack, pv)
#define SStackBegin(st) (&st.pv)
#define SStackSize (offsetof(Stack, countermove) - offsetof(Stack, pv))


// Position struct stores information regarding the board representation as
// pieces, side to move, hash keys, castling info, etc. The search uses
// the functions do_move() and undo_move() on a Position struct to traverse
// the search tree.

struct Position {
  Stack *st;
  // Board / game representation.
  Bitboard byTypeBB[7]; // no reason to allocate 8 here
  Bitboard byColorBB[2];
  Color sideToMove;
  uint8_t chess960;
  uint8_t board[64];
  uint8_t pieceCount[16];
  uint8_t castlingRightsMask[64];
  uint8_t castlingRookSquare[16];
  Bitboard castlingPath[16];
  Key rootKeyFlip;
  uint16_t gamePly;
  bool hasRepeated;

  ExtMove *moveList;

  // Relevant mainly to the search of the root position.
  RootMoves *rootMoves;
  Stack *stack;
  uint64_t nodes;
  uint64_t tbHits;
  uint64_t ttHitAverage;
  int pvIdx, pvLast;
  int selDepth, nmpMinPly;
  Color nmpColor;
  Depth rootDepth;
  Depth completedDepth;
  Score contempt;
  int failedHighCnt;

  // Pointers to thread-specific tables.
  CounterMoveStat *counterMoves;
  ButterflyHistory *mainHistory;
  LowPlyHistory *lowPlyHistory;
  CapturePieceToHistory *captureHistory;
  PawnEntry *pawnTable;
  MaterialEntry *materialTable;
  CounterMoveHistoryStat *counterMoveHistory;

  // Thread-control data.
  uint64_t bestMoveChanges;
  atomic_bool resetCalls;
  int callsCnt;
  int action;
  int threadIdx;
#ifndef _WIN32
  pthread_t nativeThread;
  pthread_mutex_t mutex;
  pthread_cond_t sleepCondition;
#else
  HANDLE nativeThread;
  HANDLE startEvent, stopEvent;
#endif
  void *stackAllocation;
};

// FEN string input/output
void pos_set(Position *pos, char *fen, int isChess960);
void pos_fen(const Position *pos, char *fen);
void print_pos(Position *pos);

//PURE Bitboard attackers_to_occ(const Position *pos, Square s, Bitboard occupied);
PURE Bitboard slider_blockers(const Position *pos, Bitboard sliders, Square s,
    Bitboard *pinners);

PURE bool is_legal(const Position *pos, Move m);
PURE bool is_pseudo_legal(const Position *pos, Move m);
PURE bool gives_check_special(const Position *pos, Stack *st, Move m);

// Doing and undoing moves
void do_move(Position *pos, Move m, int givesCheck);
void undo_move(Position *pos, Move m);
void do_null_move(Position *pos);
INLINE void undo_null_move(Position *pos);

// Static exchange evaluation
PURE bool see_test(const Position *pos, Move m, int value);

PURE Key key_after(const Position *pos, Move m);
PURE bool is_draw(const Position *pos);
PURE bool has_game_cycle(const Position *pos, int ply);

// Position representation
#define pieces() (pos->byTypeBB[0])
#define pieces_p(p) (pos->byTypeBB[p])
#define pieces_pp(p1,p2) (pos->byTypeBB[p1] | pos->byTypeBB[p2])
#define pieces_c(c) (pos->byColorBB[c])
#define pieces_cp(c,p) (pieces_p(p) & pieces_c(c))
#define pieces_cpp(c,p1,p2) (pieces_pp(p1,p2) & pieces_c(c))
#define piece_on(s) (pos->board[s])
#define ep_square() (pos->st->epSquare)
#define is_empty(s) (!piece_on(s))
#define piece_count(c,p) (pos->pieceCount[make_piece(c,p)])
#define square_of(c,p) lsb(pieces_cp(c,p))
#define loop_through_pieces(c,p,s) \
  for (Bitboard bb_pieces = pieces_cp(c,p); \
      bb_pieces && (s = pop_lsb(&bb_pieces), true);)
#define piece_count_mk(c, p) (((material_key()) >> (20 * (c) + 4 * (p) + 4)) & 15)

// Castling
#define can_castle_cr(cr) (pos->st->castlingRights & (cr))
#define can_castle_c(c) can_castle_cr((WHITE_OO | WHITE_OOO) << (2 * (c)))
#define can_castle_any() (pos->st->castlingRights)
#define castling_impeded(cr) (pieces() & pos->castlingPath[cr])
#define castling_rook_square(cr) (pos->castlingRookSquare[cr])

// Checking
#define checkers() (pos->st->checkersBB)

// Attacks to/from a given square
#define attackers_to(s) attackers_to_occ(pos,s,pieces())
#define attacks_from_pawn(s,c) (PawnAttacks[c][s])
#define attacks_from_knight(s) (PseudoAttacks[KNIGHT][s])
#define attacks_from_bishop(s) attacks_bb_bishop(s, pieces())
#define attacks_from_rook(s) attacks_bb_rook(s, pieces())
#define attacks_from_queen(s) (attacks_from_bishop(s)|attacks_from_rook(s))
#define attacks_from_king(s) (PseudoAttacks[KING][s])
#define attacks_from(pc,s) attacks_bb(pc,s,pieces())

// Properties of moves
#define moved_piece(m) (piece_on(from_sq(m)))
#define captured_piece() (pos->st->capturedPiece)

// Accessing hash keys
#define raw_key() (pos->st->key)
#define key() (pos->st->rule50 < 14 ? pos->st->key : pos->st->key ^ make_key((pos->st->rule50 - 14) / 8))
#define material_key() (pos->st->materialKey)
#define pawn_key() (pos->st->pawnKey)

// Other properties of the position
#define stm() (pos->sideToMove)
#define game_ply() (pos->gamePly)
#define is_chess960() (pos->chess960)
#define nodes_searched() (pos->nodes)
#define rule50_count() (pos->st->rule50)
#define psq_score() (pos->st->psq)
#define non_pawn_material_c(c) (pos->st->nonPawnMaterial[c])
#define non_pawn_material() (non_pawn_material_c(WHITE) + non_pawn_material_c(BLACK))
#define pawns_only() (!pos->st->nonPawn)

INLINE Bitboard blockers_for_king(const Position *pos, Color c)
{
  return pos->st->blockersForKing[c];
}

INLINE bool pawn_passed(const Position *pos, Color c, Square s)
{
  return !(pieces_cp(!c, PAWN) & passed_pawn_span(c, s));
}

INLINE bool opposite_bishops(const Position *pos)
{
  return   piece_count(WHITE, BISHOP) == 1
        && piece_count(BLACK, BISHOP) == 1
        && (pieces_p(BISHOP) & DarkSquares)
        && (pieces_p(BISHOP) & ~DarkSquares);
}

INLINE bool is_capture_or_promotion(const Position *pos, Move m)
{
  assert(move_is_ok(m));
  return type_of_m(m) != NORMAL ? type_of_m(m) != CASTLING : !is_empty(to_sq(m));
}

INLINE bool is_capture(const Position *pos, Move m)
{
  // Castling is encoded as "king captures the rook"
  assert(move_is_ok(m));
  return (!is_empty(to_sq(m)) && type_of_m(m) != CASTLING) || type_of_m(m) == ENPASSANT;
}

INLINE bool gives_check(const Position *pos, Stack *st, Move m)
{
  return  type_of_m(m) == NORMAL && !(blockers_for_king(pos, !stm()) & pieces_c(stm()))
        ? (bool)(st->checkSquares[type_of_p(moved_piece(m))] & sq_bb(to_sq(m)))
        : gives_check_special(pos, st, m);
}

void pos_set_check_info(Position *pos);

// undo_null_move is used to undo a null move.

INLINE void undo_null_move(Position *pos)
{
  assert(!checkers());

  pos->st--;
  pos->sideToMove = !pos->sideToMove;
}

// Inlining this seems to slow down.
#if 0
// slider_blockers() returns a bitboard of all pieces that are blocking
// attacks on the square 's' from 'sliders'. A piece blocks a slider if
// removing that piece from the board would result in a position where
// square 's' is attacked. Both pinned pieces and discovered check
// candidates are slider blockers and are calculated by calling this
// function.

INLINE Bitboard slider_blockers(const Position *pos, Bitboard sliders, Square s,
    Bitboard *pinners)
{
  Bitboard result = 0, snipers;
  *pinners = 0;

  // Snipers are sliders that attack square 's'when a piece removed.
  snipers = (  (PseudoAttacks[ROOK  ][s] & pieces_pp(QUEEN, ROOK))
             | (PseudoAttacks[BISHOP][s] & pieces_pp(QUEEN, BISHOP))) & sliders;

  while (snipers) {
    Square sniperSq = pop_lsb(&snipers);
    Bitboard b = between_bb(s, sniperSq) & pieces();

    if (!more_than_one(b)) {
      result |= b;
      if (b & pieces_c(color_of(piece_on(s))))
        *pinners |= sq_bb(sniperSq);
    }
  }
  return result;
}
#endif

// attackers_to() computes a bitboard of all pieces which attack a given
// square. Slider attacks use the occupied bitboard to indicate occupancy.

INLINE Bitboard attackers_to_occ(const Position *pos, Square s,
    Bitboard occupied)
{
  return  (attacks_from_pawn(s, BLACK)    & pieces_cp(WHITE, PAWN))
        | (attacks_from_pawn(s, WHITE)    & pieces_cp(BLACK, PAWN))
        | (attacks_from_knight(s)         & pieces_p(KNIGHT))
        | (attacks_bb_rook(s, occupied)   & pieces_pp(ROOK,   QUEEN))
        | (attacks_bb_bishop(s, occupied) & pieces_pp(BISHOP, QUEEN))
        | (attacks_from_king(s)           & pieces_p(KING));
}

#endif

/* ==========================================
   FILE: movegen.h
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef MOVEGEN_H
#define MOVEGEN_H


#define GEN_CAPTURES     0
#define GEN_QUIETS       1
#define GEN_QUIET_CHECKS 2
#define GEN_EVASIONS     3
#define GEN_NON_EVASIONS 4
#define GEN_LEGAL        5

ExtMove *generate_captures(const Position *pos, ExtMove *list);
ExtMove *generate_quiets(const Position *pos, ExtMove *list);
ExtMove *generate_quiet_checks(const Position *pos, ExtMove *list);
ExtMove *generate_evasions(const Position *pos, ExtMove *list);
ExtMove *generate_non_evasions(const Position *pos, ExtMove *list);
ExtMove *generate_legal(const Position *pos, ExtMove *list);

#endif


/* ==========================================
   FILE: movepick.h
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2018 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef MOVEPICK_H
#define MOVEPICK_H



#define stats_clear(s) memset(s, 0, sizeof(*s))

static const int CounterMovePruneThreshold = 0;

INLINE void cms_update(PieceToHistory cms, Piece pc, Square to, int v)
{
  cms[pc][to] += v - cms[pc][to] * abs(v) / 29952;
}

INLINE void history_update(ButterflyHistory history, Color c, Move m, int v)
{
  m &= 4095;
  history[c][m] += v - history[c][m] * abs(v) / 13365;
}

INLINE void cpth_update(CapturePieceToHistory history, Piece pc, Square to,
                        int captured, int v)
{
  history[pc][to][captured] += v - history[pc][to][captured] * abs(v) / 10692;
}

INLINE void lph_update(LowPlyHistory history, int ply, Move m, int v)
{
  m &= 4095;
  history[ply][m] += v - history[ply][m] * abs(v) / 10692;
}

enum {
  ST_MAIN_SEARCH, ST_CAPTURES_INIT, ST_GOOD_CAPTURES, ST_KILLERS, ST_KILLERS_2,
  ST_QUIET_INIT, ST_QUIET, ST_BAD_CAPTURES,

  ST_EVASION, ST_EVASIONS_INIT, ST_ALL_EVASIONS,

  ST_QSEARCH, ST_QCAPTURES_INIT, ST_QCAPTURES, ST_QCHECKS,

  ST_PROBCUT, ST_PROBCUT_INIT, ST_PROBCUT_2
};

Move next_move(const Position *pos, bool skipQuiets);

// Initialisation of move picker data.

INLINE void mp_init(const Position *pos, Move ttm, Depth d, int ply)
{
  assert(d > 0);

  Stack *st = pos->st;

  st->depth = d;
  st->mp_ply = ply;

  Square prevSq = to_sq((st-1)->currentMove);
  st->countermove = (*pos->counterMoves)[piece_on(prevSq)][prevSq];
  st->mpKillers[0] = st->killers[0];
  st->mpKillers[1] = st->killers[1];

  st->ttMove = ttm;
  st->stage = checkers() ? ST_EVASION : ST_MAIN_SEARCH;
  if (!ttm || !is_pseudo_legal(pos, ttm))
    st->stage++;
}

INLINE void mp_init_q(const Position *pos, Move ttm, Depth d, Square s)
{
  assert(d <= 0);

  Stack *st = pos->st;

  st->ttMove = ttm;
  st->stage = checkers() ? ST_EVASION : ST_QSEARCH;
  if (!(   ttm
        && (checkers() || d > DEPTH_QS_RECAPTURES || to_sq(ttm) == s)
        && is_pseudo_legal(pos, ttm)))
    st->stage++;

  st->depth = d;
  st->recaptureSquare = s;
}

INLINE void mp_init_pc(const Position *pos, Move ttm, Value th)
{
  assert(!checkers());

  Stack *st = pos->st;

  st->threshold = th;

  st->ttMove = ttm;
  st->stage = ST_PROBCUT;

  // In ProbCut we generate captures with SEE higher than the given
  // threshold.
  if (!(ttm && is_pseudo_legal(pos, ttm) && is_capture(pos, ttm)
            && see_test(pos, ttm, th)))
    st->stage++;
}

#endif

/* ==========================================
   FILE: pawns.h
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef PAWNS_H
#define PAWNS_H

#ifndef NNUE_PURE


// Number of entries in the pawn hash table. Must be a power of 2.
#define PAWN_ENTRIES 16384

// PawnEntry contains various information about a pawn structure. A lookup
// to the pawn hash table (performed by calling the probe function) returns
// a pointer to an Entry object.

struct PawnEntry {
  Key key;
  Bitboard passedPawns[2];
  Bitboard pawnAttacks[2];
  Bitboard pawnAttacksSpan[2];
  Score kingSafety[2];
  Score score;
  uint8_t kingSquares[2];
  uint8_t castlingRights[2];
  uint8_t semiopenFiles[2];
  uint8_t pawnsOnSquares[2][2]; // [color][light/dark squares]
  uint8_t blockedCount;
  uint8_t passedCount;
  uint8_t openFiles;
};

typedef struct PawnEntry PawnEntry;
typedef PawnEntry PawnTable[PAWN_ENTRIES];

Score do_king_safety_white(PawnEntry *pe, const Position *pos, Square ksq);
Score do_king_safety_black(PawnEntry *pe, const Position *pos, Square ksq);

Value shelter_storm_white(const Position *pos, Square ksq);
Value shelter_storm_black(const Position *pos, Square ksq);

void pawn_entry_fill(const Position *pos, PawnEntry *e, Key k);

INLINE PawnEntry *pawn_probe(const Position *pos)
{
  Key key = pawn_key();
  PawnEntry *e = &pos->pawnTable[key & (PAWN_ENTRIES - 1)];

  if (unlikely(e->key != key))
    pawn_entry_fill(pos, e, key);

  return e;
}

INLINE bool is_on_semiopen_file(const PawnEntry *pe, Color c, Square s)
{
  return pe->semiopenFiles[c] & (1 << file_of(s));
}

INLINE int pawns_on_same_color_squares(PawnEntry *pe, Color c, Square s)
{
  return pe->pawnsOnSquares[c][!!(DarkSquares & sq_bb(s))];
}

INLINE Score king_safety_white(PawnEntry *pe, const Position *pos, Square ksq)
{
  if (   pe->kingSquares[WHITE] == ksq
      && pe->castlingRights[WHITE] == can_castle_c(WHITE))
    return pe->kingSafety[WHITE];
  else
    return pe->kingSafety[WHITE] = do_king_safety_white(pe, pos, ksq);
}

INLINE Score king_safety_black(PawnEntry *pe, const Position *pos, Square ksq)
{
  if (   pe->kingSquares[BLACK] == ksq
      && pe->castlingRights[BLACK] == can_castle_c(BLACK))
    return pe->kingSafety[BLACK];
  else
    return pe->kingSafety[BLACK] = do_king_safety_black(pe, pos, ksq);
}

#endif
#endif

/* ==========================================
   FILE: material.h
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2018 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef MATERIAL_H
#define MATERIAL_H


// MaterialEntry contains various information about a material
// configuration. It contains a material imbalance evaluation, a function
// pointer to a special endgame evaluation function (which in most cases
// is NULL, meaning that the standard evaluation function will be used),
// and scale factors.
//
// The scale factors are used to scale the evaluation score up or down.
// For instance, in KRB vs KR endgames, the score is scaled down by a
// factor of 4, which will result in scores of absolute value less than
// one pawn.

struct MaterialEntry {
  Key key;
  int16_t gamePhase;
  Score score;
  uint8_t eval_func;
  uint8_t eval_func_side;
  uint8_t scal_func[2];
  uint8_t factor[2];
};

typedef struct MaterialEntry MaterialEntry;

typedef MaterialEntry MaterialTable[8192];

void material_entry_fill(const Position *pos, MaterialEntry *e, Key key);

INLINE MaterialEntry *material_probe(const Position *pos)
{
  Key key = material_key();
  MaterialEntry *e = &pos->materialTable[key >> (64-13)];

  if (unlikely(e->key != key))
    material_entry_fill(pos, e, key);

  return e;
}

INLINE Score material_imbalance(MaterialEntry *me)
{
  return me->score;
}

INLINE bool material_specialized_eval_exists(MaterialEntry *me)
{
  return me->eval_func != 0;
}

INLINE Value material_evaluate(MaterialEntry *me, const Position *pos)
{
  return endgame_funcs[me->eval_func](pos, me->eval_func_side);
}

// scale_factor takes a position and a color as input and returns a scale factor
// for the given color. We have to provide the position in addition to the color
// because the scale factor may also be a function which should be applied to
// the position. For instance, in KBP vs K endgames, the scaling function looks
// for rook pawns and wrong-colored bishops.
INLINE int material_scale_factor(MaterialEntry *me, const Position *pos,
    Color c)
{
  int sf = SCALE_FACTOR_NONE;
  if (me->scal_func[c])
    sf = endgame_funcs[me->scal_func[c]](pos, c);
  return sf != SCALE_FACTOR_NONE ? sf : me->factor[c];
}

#endif

/* ==========================================
   FILE: endgame.h
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef ENDGAME_H
#define ENDGAME_H


typedef Value (EgFunc)(const Position *, Color);

#define NUM_EVAL 9
#define NUM_SCALING 6

extern EgFunc *endgame_funcs[NUM_EVAL + NUM_SCALING + 6];
extern Key endgame_keys[NUM_EVAL + NUM_SCALING][2];

void endgames_init(void);

#endif

/* ==========================================
   FILE: evaluate.h
   ========================================== */

#ifndef EVALUATE_H
#define EVALUATE_H


#define DefaultEvalFile "nn-62ef826d1a6d.nnue"

enum { Tempo = 28 };

#ifdef NNUE
enum { EVAL_HYBRID, EVAL_PURE, EVAL_CLASSICAL };
#ifndef NNUE_PURE
extern int useNNUE;
#else
#define useNNUE EVAL_PURE
#endif
#endif

Value evaluate(const Position *pos);

#endif

/* ==========================================
   FILE: thread.h
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef THREAD_H
#define THREAD_H

#ifndef _WIN32
#else
#endif


#define MAX_THREADS 512

#ifndef _WIN32
#define LOCK_T pthread_mutex_t
#define LOCK_INIT(x) pthread_mutex_init(&(x), NULL)
#define LOCK_DESTROY(x) pthread_mutex_destroy(&(x))
#define LOCK(x) pthread_mutex_lock(&(x))
#define UNLOCK(x) pthread_mutex_unlock(&(x))
#else
#define LOCK_T HANDLE
#define LOCK_INIT(x) do { x = CreateMutex(NULL, FALSE, NULL); } while (0)
#define LOCK_DESTROY(x) CloseHandle(x)
#define LOCK(x) WaitForSingleObject(x, INFINITE)
#define UNLOCK(x) ReleaseMutex(x)
#endif

enum {
  THREAD_SLEEP, THREAD_SEARCH, THREAD_TT_CLEAR, THREAD_EXIT, THREAD_RESUME
};

void thread_search(Position *pos);
void thread_wake_up(Position *pos, int action);
void thread_wait_until_sleeping(Position *pos);
void thread_wait(Position *pos, atomic_bool *b);


// MainThread struct seems to exist mostly for easy move.

struct MainThread {
  double previousTimeReduction;
  Value previousScore;
  Value iterValue[4];
};

typedef struct MainThread MainThread;

extern MainThread mainThread;

void mainthread_search(void);


// ThreadPool struct handles all the threads-related stuff like init,
// starting, parking and, most importantly, launching a thread. All the
// access to threads data is done through this class.

struct ThreadPool {
  Position *pos[MAX_THREADS];
  int numThreads;
#ifndef _WIN32
  pthread_mutex_t mutex;
  pthread_cond_t sleepCondition;
  bool initializing;
#else
  HANDLE event;
#endif
  bool searching, sleeping, stopOnPonderhit;
  atomic_bool ponder, stop, increaseDepth;
  LOCK_T lock;
};

typedef struct ThreadPool ThreadPool;

void threads_init(void);
void threads_exit(void);
void threads_start_thinking(Position *pos, LimitsType *);
void threads_set_number(int num);
uint64_t threads_nodes_searched(void);
uint64_t threads_tb_hits(void);

extern ThreadPool Threads;

INLINE Position *threads_main(void)
{
  return Threads.pos[0];
}

extern CounterMoveHistoryStat **cmhTables;
extern int numCmhTables;

#endif

/* ==========================================
   FILE: timeman.h
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef TIMEMAN_H
#define TIMEMAN_H


// The TimeManagement class computes the optimal time to think depending on
// the maximum available time, the game move number and other parameters.

struct TimeManagement {
  TimePoint startTime;
  int optimumTime;
  int maximumTime;
  int64_t availableNodes;
  int tempoNNUE;
};

extern struct TimeManagement Time;

void time_init(Color us, int ply);

#define time_optimum() Time.optimumTime
#define time_maximum() Time.maximumTime

INLINE TimePoint time_elapsed(void)
{
  return Limits.npmsec ? (int64_t)threads_nodes_searched()
                       : now() - Time.startTime;
}

#endif

/* ==========================================
   FILE: tt.h
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef TT_H
#define TT_H


// TTEntry struct is the 10 bytes transposition table entry, defined as below:
//
// key        16 bit
// depth       8 bit
// generation  5 bit
// pv node     1 bit
// bound type  2 bit
// move       16 bit
// value      16 bit
// eval value 16 bit

struct TTEntry {
  uint16_t key16;
  uint8_t  depth8;
  uint8_t  genBound8;
  uint16_t move16;
  int16_t  value16;
  int16_t  eval16;
};

typedef struct TTEntry TTEntry;

// A TranspositionTable consists of a power of 2 number of clusters and
// each cluster consists of ClusterSize number of TTEntry. Each non-empty
// entry contains information of exactly one position. The size of a
// cluster should divide the size of a cache line size, to ensure that
// clusters never cross cache lines. This ensures best cache performance,
// as the cacheline is prefetched, as soon as possible.

enum { CacheLineSize = 64, ClusterSize = 3 };

struct Cluster {
  TTEntry entry[ClusterSize];
  char padding[2]; // Align to a divisor of the cache line size
};

typedef struct Cluster Cluster;

struct TranspositionTable {
  size_t clusterCount;
  Cluster *table;
  alloc_t alloc;
  uint8_t generation8; // Size must be not bigger than TTEntry::genBound8
};

typedef struct TranspositionTable TranspositionTable;

extern TranspositionTable TT;

INLINE void tte_save(TTEntry *tte, Key k, Value v, bool pv, int b, Depth d,
    Move m, Value ev)
{
  // Preserve any existing move for the same position
  if (m || (uint16_t)k != tte->key16)
    tte->move16 = (uint16_t)m;

  // Don't overwrite more valuable entries
  if (  (uint16_t)k != tte->key16
      || d - DEPTH_OFFSET > tte->depth8 - 4
      || b == BOUND_EXACT)
  {
    assert(d > DEPTH_OFFSET && d < 256 + DEPTH_OFFSET);

    tte->key16     = (uint16_t)k;
    tte->depth8    = (uint8_t)(d - DEPTH_OFFSET);
    tte->genBound8 = (uint8_t)(TT.generation8 | ((uint8_t)pv << 2) | b);
    tte->value16   = (int16_t)v;
    tte->eval16    = (int16_t)ev;
  }
}

INLINE Move tte_move(TTEntry *tte)
{
  return tte->move16;
}

INLINE Value tte_value(TTEntry *tte)
{
  return tte->value16;
}

INLINE Value tte_eval(TTEntry *tte)
{
  return tte->eval16;
}

INLINE Depth tte_depth(TTEntry *tte)
{
  return tte->depth8 + DEPTH_OFFSET;
}

INLINE bool tte_is_pv(TTEntry *tte)
{
  return tte->genBound8 & 0x4;
}

INLINE int tte_bound(TTEntry *tte)
{
  return tte->genBound8 & 0x3;
}

void tt_free(void);

INLINE void tt_new_search(void)
{
  TT.generation8 += 8; // Lower 3 bits are used by PvNode and Bound
}

INLINE TTEntry *tt_first_entry(Key key)
{
  return &TT.table[mul_hi64(key, TT.clusterCount)].entry[0];
}

TTEntry *tt_probe(Key key, bool *found);
int tt_hashfull(void);
void tt_allocate(size_t mbSize);
void tt_clear(void);
void tt_clear_worker(int idx);

#endif

/* ==========================================
   FILE: uci.h
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef UCI_H
#define UCI_H



struct Option;
typedef struct Option Option;

typedef void (*OnChange)(Option *);

enum {
  OPT_TYPE_CHECK, OPT_TYPE_SPIN, OPT_TYPE_BUTTON, OPT_TYPE_STRING,
  OPT_TYPE_COMBO, OPT_TYPE_DISABLED
};

enum {
  OPT_CONTEMPT,
  OPT_ANALYSIS_CONTEMPT,
  OPT_THREADS,
  OPT_HASH,
  OPT_CLEAR_HASH,
  OPT_PONDER,
  OPT_MULTI_PV,
  OPT_SKILL_LEVEL,
  OPT_MOVE_OVERHEAD,
  OPT_SLOW_MOVER,
  OPT_NODES_TIME,
  OPT_ANALYSE_MODE,
  OPT_CHESS960,
  OPT_SYZ_PATH,
  OPT_SYZ_PROBE_DEPTH,
  OPT_SYZ_50_MOVE,
  OPT_SYZ_PROBE_LIMIT,
  OPT_SYZ_USE_DTM,
  OPT_BOOK_FILE,
  OPT_BOOK_FILE2,
  OPT_BOOK_BEST_MOVE,
  OPT_BOOK_DEPTH,
#ifdef NNUE
  OPT_EVAL_FILE,
#ifndef NNUE_PURE
  OPT_USE_NNUE,
#endif
#endif
  OPT_LARGE_PAGES,
  OPT_NUMA
};

struct Option {
  char *name;
  int type;
  int def, minVal, maxVal;
  char *defString;
  OnChange onChange;
  int value;
  char *valString;
};

void options_init(void);
void options_free(void);
void print_options(void);
int option_value(int opt);
const char *option_string_value(int opt);
const char *option_default_string_value(int opt);
void option_set_value(int opt, int value);
bool option_set_by_name(char *name, char *value);

void setoption(char *str);
void position(Position *pos, char *str);

void uci_loop(int argc, char* argv[]);
char *uci_value(char *str, Value v);
char *uci_square(char *str, Square s);
char *uci_move(char *str, Move m, int chess960);
void print_pv(Position *pos, Depth depth, Value alpha, Value beta);
Move uci_to_move(const Position *pos, char *str);

#endif

/* ==========================================
   FILE: tbprobe.h
   ========================================== */

#ifndef TBPROBE_H
#define TBPROBE_H


extern int TB_MaxCardinality;
extern int TB_MaxCardinalityDTM;

void TB_init(char *path);
void TB_free(void);
void TB_release(void);
int TB_probe_wdl(Position *pos, int *success);
int TB_probe_dtz(Position *pos, int *success);
Value TB_probe_dtm(Position *pos, int wdl, int *success);
bool TB_root_probe_wdl(Position *pos, RootMoves *rm);
bool TB_root_probe_dtz(Position *pos, RootMoves *rm);
bool TB_root_probe_dtm(Position *pos, RootMoves *rm);
void TB_expand_mate(Position *pos, RootMove *move);

#endif

/* ==========================================
   FILE: misc.c
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef _WIN32
#else
#endif


// Version number. If Version is left empty, then compile date in the format
// DD-MM-YY and show in engine_info.
char Version[] = "";

#ifndef _WIN32
pthread_mutex_t ioMutex = PTHREAD_MUTEX_INITIALIZER;
#else
HANDLE ioMutex;
#endif

static char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
static char date[] = __DATE__;

// print engine_info() prints the full name of the current Stockfish version.
// This will be either "Stockfish <Tag> DD-MM-YY" (where DD-MM-YY is the
// date when the program was compiled) or "Stockfish <Version>", depending
// on whether Version is empty.

void print_engine_info(bool to_uci)
{
  char my_date[64];

  printf("Cfish %s", Version);

  if (strlen(Version) == 0) {
    int day, month, year;

    strcpy(my_date, date);
    char *str = strtok(my_date, " "); // month
    for (month = 1; strncmp(str, &months[3 * month - 3], 3) != 0; month++);
    str = strtok(NULL, " "); // day
    day = atoi(str);
    str = strtok(NULL, " "); // year
    year = atoi(str);

    printf("%02d%02d%02d", day, month, year % 100);
  }

  printf(
#ifdef IS_64BIT
         " 64"
#endif
#ifdef USE_AVX512
         " AVX512"
#elif USE_PEXT
         " BMI2"
#elif USE_AVX2
         " AVX2"
#elif USE_NEON
         " NEON"
#elif USE_POPCNT
         " POPCNT"
#endif
#ifdef USE_VNNI
         "-VNNI"
#endif
#ifdef NUMA
         " NUMA"
#endif
         "%s\n", to_uci ? "\nid author The Stockfish developers"
                      : " by Syzygy based on Stockfish");
  fflush(stdout);
}

// print compiler_info() prints a string trying to describe the compiler

void print_compiler_info(void)
{
#define stringify2(x) #x
#define stringify(x) stringify2(x)
#define make_version_string(major, minor, patch) stringify(major) "." stringify(minor) "." stringify(patch)

  printf("\nCompiled by "

#ifdef __clang__
         "clang " make_version_string(__clang_major__, __clang_minor__,
                                      __clang_patchlevel__)
#elif __INTEL_COMPILER
         "Intel compiler (version " stringify(__INTEL_COMPILER)
         " update " stringify(__INTEL_COMPILER_UPDATE) ")"
#elif _MSC_VER
         "MSVC (version " stringify(_MSC_FULL_VER) "." stringify(_MSC_BUILD) ")"
#elif __GNUC__
         "gcc (GNUC) "
         make_version_string(__GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__)
#else
         "Unknown compiler (unknown version)"
#endif

#ifdef __APPLE__
         " on Apple"
#elif __CYGWIN__
         " on Cygwin"
#elif __MINGW64__
         " on MinGW64"
#elif __MINGW32__
         " on MinGW32"
#elif __ANDROID__
         " on Android"
#elif __linux__
         " on Linux"
#elif _WIN64
         " on Microsoft Windows 64-bit"
#elif _WIN32
         " on Microsoft Windows 32-bit"
#else
         " on unknown system"
#endif

         "\nCompilation settings include: "
#ifdef IS_64BIT
         "64bit"
#else
         "32bit"
#endif
#ifdef USE_VNNI
         " VNNI"
#endif
#ifdef USE_AVX512
         " AVX512"
#endif
#ifdef USE_PEXT
         " BMI2"
#endif
#ifdef USE_AVX2
         " AVX2"
#endif
#ifdef USE_AVX
         " AVX"
#endif
#ifdef USE_SSE41
         " SSE41"
#endif
#ifdef USE_SSSE3
         " SSSE3"
#endif
#ifdef USE_SSE2
         " SSE2"
#endif
#ifdef USE_POPCNT
         " POPCNT"
#endif
#ifdef USE_MMX
         " MMX"
#endif
#ifdef USE_NEON
         " NEON"
#endif
#ifdef NNUE_SPARSE
         " sparse"
#endif
#ifndef NDEBUG
         " DEBUG"
#endif
         "\n__VERSION__ macro expands to: "
#ifdef __VERSION__
         __VERSION__
#else
         "(undefined macro)"
#endif
         "\n\n");
}

// xorshift64star Pseudo-Random Number Generator
// This class is based on original code written and dedicated
// to the public domain by Sebastiano Vigna (2014).
// It has the following characteristics:
//
//  -  Outputs 64-bit numbers
//  -  Passes Dieharder and SmallCrush test batteries
//  -  Does not require warm-up, no zeroland to escape
//  -  Internal state is a single 64-bit integer
//  -  Period is 2^64 - 1
//  -  Speed: 1.60 ns/call (Core i7 @3.40GHz)
//
// For further analysis see
//   <http://vigna.di.unimi.it/ftp/papers/xorshift.pdf>

void prng_init(PRNG *rng, uint64_t seed)
{
  rng->s = seed;
}

uint64_t prng_rand(PRNG *rng)
{
  uint64_t s = rng->s;

  s ^= s >> 12;
  s ^= s << 25;
  s ^= s >> 27;
  rng->s = s;

  return s * 2685821657736338717LL;
}

uint64_t prng_sparse_rand(PRNG *rng)
{
  uint64_t r1 = prng_rand(rng);
  uint64_t r2 = prng_rand(rng);
  uint64_t r3 = prng_rand(rng);
  return r1 & r2 & r3;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
  if (*n == 0)
    *lineptr = malloc(*n = 100);

  int c = 0;
  size_t i = 0;
  while ((c = getc(stream)) != EOF) {
    (*lineptr)[i++] = c;
    if (i == *n)
      *lineptr = realloc(*lineptr, *n += 100);
    if (c == '\n') break;
  }
  (*lineptr)[i] = 0;
  return i;
}

#ifdef _WIN32
typedef SIZE_T (WINAPI *GLPM)(void);
size_t largePageMinimum;

bool large_pages_supported(void)
{
  GLPM impGetLargePageMinimum =
    (GLPM)(void (*)(void))GetProcAddress(GetModuleHandle("kernel32.dll"),
        "GetLargePageMinimum");
  if (!impGetLargePageMinimum)
    return 0;

  if ((largePageMinimum = impGetLargePageMinimum()) == 0)
    return 0;

  LUID privLuid;
  if (!LookupPrivilegeValue(NULL, SE_LOCK_MEMORY_NAME, &privLuid))
    return 0;

  HANDLE token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token))
    return 0;

  TOKEN_PRIVILEGES tokenPrivs;
  tokenPrivs.PrivilegeCount = 1;
  tokenPrivs.Privileges[0].Luid = privLuid;
  tokenPrivs.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (!AdjustTokenPrivileges(token, FALSE, &tokenPrivs, 0, NULL, NULL))
    return 0;

  return 1;
}

// The following two functions were taken from mingw_lock.c

void __cdecl _lock(int locknum);
void __cdecl _unlock(int locknum);
#define _STREAM_LOCKS 16
#define _IOLOCKED 0x8000
typedef struct {
  FILE f;
  CRITICAL_SECTION lock;
} _FILEX;

void flockfile(FILE *F)
{
  if ((F >= (&__iob_func()[0])) && (F <= (&__iob_func()[_IOB_ENTRIES-1]))) {
    _lock(_STREAM_LOCKS + (int)(F - (&__iob_func()[0])));
    F->_flag |= _IOLOCKED;
  } else
    EnterCriticalSection(&(((_FILEX *)F)->lock));
}

void funlockfile(FILE *F)
{
  if ((F >= (&__iob_func()[0])) && (F <= (&__iob_func()[_IOB_ENTRIES-1]))) {
    F->_flag &= ~_IOLOCKED;
    _unlock(_STREAM_LOCKS + (int)(F - (&__iob_func()[0])));
  } else
    LeaveCriticalSection(&(((_FILEX *)F)->lock));
}
#endif

FD open_file(const char *name)
{
#ifndef _WIN32
  return open(name, O_RDONLY);

#else
  return CreateFile(name, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
      FILE_FLAG_RANDOM_ACCESS, NULL);

#endif
}

void close_file(FD fd)
{
#ifndef _WIN32
  close(fd);

#else
  CloseHandle(fd);

#endif
}

size_t file_size(FD fd)
{
#ifndef _WIN32
  struct stat statbuf;
  fstat(fd, &statbuf);
  return statbuf.st_size;

#else
  DWORD sizeLow, sizeHigh;
  sizeLow = GetFileSize(fd, &sizeHigh);
  return ((uint64_t)sizeHigh << 32) | sizeLow;

#endif
}

const void *map_file(FD fd, map_t *map)
{
#ifndef _WIN32
  *map = file_size(fd);
  void *data = mmap(NULL, *map, PROT_READ, MAP_SHARED, fd, 0);
#ifdef MADV_RANDOM
  madvise(data, *map, MADV_RANDOM);
#endif
  return data == MAP_FAILED ? NULL : data;

#else
  DWORD sizeLow, sizeHigh;
  sizeLow = GetFileSize(fd, &sizeHigh);
  *map = CreateFileMapping(fd, NULL, PAGE_READONLY, sizeHigh, sizeLow, NULL);
  if (*map == NULL)
    return NULL;
  return MapViewOfFile(*map, FILE_MAP_READ, 0, 0, 0);

#endif
}

void unmap_file(const void *data, map_t map)
{
  if (!data) return;

#ifndef _WIN32
  munmap((void *)data, map);

#else
  UnmapViewOfFile(data);
  CloseHandle(map);

#endif
}

void *allocate_memory(size_t size, bool lp, alloc_t *alloc)
{
  void *ptr = NULL;

#ifdef _WIN32
  if (lp) {
    size_t pageSize = largePageMinimum;
    size_t lpSize = (size + pageSize - 1) & ~(pageSize - 1);
    ptr = VirtualAlloc(NULL, lpSize,
        MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);
  } else
    ptr = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  alloc->ptr = ptr;
  return ptr;

#else /* Unix */
  size_t alignment = lp ? 1ULL << 21 : 1;
  size_t allocSize = size + alignment - 1;

#if defined(__APPLE__) && defined(VM_FLAGS_SUPERPAGE_SIZE_2MB)
  if (lp)
    ptr = mmap(NULL, allocSize, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, VM_FLAGS_SUPERPAGE_SIZE_2MB, 0);
  else
    ptr = mmap(NULL, allocSize, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

#else
  ptr = mmap(NULL, allocSize, PROT_READ | PROT_WRITE,
      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#if defined(__linux__) && defined(MADV_HUGEPAGE)
  // Advise the kernel to allocate large pages.
  if (lp)
    madvise(ptr, allocSize, MADV_HUGEPAGE);
#endif

#endif

  alloc->ptr = ptr;
  alloc->size = allocSize;
  return (void *)(((uintptr_t)ptr + alignment - 1) & ~(alignment - 1));

#endif
}

void free_memory(alloc_t *alloc)
{
#ifdef _WIN32
  VirtualFree(alloc->ptr, 0, MEM_RELEASE);
#else
  munmap(alloc->ptr, alloc->size);
#endif
}

/* ==========================================
   FILE: bitboard.c
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#ifndef USE_POPCNT
uint8_t PopCnt16[1 << 16];
#endif
uint8_t SquareDistance[64][64];

#ifndef AVX2_BITBOARD
static int RookDirs[] = { NORTH, EAST, SOUTH, WEST };
static int BishopDirs[] = { NORTH_EAST, SOUTH_EAST, SOUTH_WEST, NORTH_WEST };

static Bitboard sliding_attack(int dirs[], Square sq, Bitboard occupied)
{
  Bitboard attack = 0;

  for (int i = 0; i < 4; i++)
    for (Square s = sq + dirs[i];
         square_is_ok(s) && distance(s, s - dirs[i]) == 1; s += dirs[i])
    {
      attack |= sq_bb(s);
      if (occupied & sq_bb(s))
        break;
    }

  return attack;
}
#endif

#if defined(MAGIC_FANCY)
#elif defined(MAGIC_PLAIN)
#elif defined(MAGIC_BLACK)
#elif defined(BMI2_FANCY)
#elif defined(BMI2_PLAIN)
#elif defined(AVX2_BITBOARD)
#endif

Bitboard SquareBB[64];
Bitboard FileBB[8];
Bitboard RankBB[8];
Bitboard ForwardRanksBB[2][8];
Bitboard BetweenBB[64][64];
Bitboard LineBB[64][64];
Bitboard DistanceRingBB[64][8];
Bitboard ForwardFileBB[2][64];
Bitboard PassedPawnSpan[2][64];
Bitboard PawnAttackSpan[2][64];
Bitboard PseudoAttacks[8][64];
Bitboard PawnAttacks[2][64];

#ifndef PEDANTIC
Bitboard EPMask[16];
Bitboard CastlingPath[64];
int CastlingRightsMask[64];
Square CastlingRookSquare[16];
Key CastlingHash[16];
Bitboard CastlingBits[16];
Score CastlingPSQ[16];
Square CastlingRookFrom[16];
Square CastlingRookTo[16];
#endif

#ifndef USE_POPCNT
// popcount16() counts the non-zero bits using SWAR-Popcount algorithm.

INLINE unsigned popcount16(unsigned u)
{
  u -= (u >> 1) & 0x5555U;
  u = ((u >> 2) & 0x3333U) + (u & 0x3333U);
  u = ((u >> 4) + u) & 0x0F0FU;
  return (u * 0x0101U) >> 8;
}
#endif


// Bitboards::pretty() returns an ASCII representation of a bitboard suitable
// to be printed to standard output. Useful for debugging.

void print_pretty(Bitboard b)
{
  printf("+---+---+---+---+---+---+---+---+\n");

  for (int r = 7; r >= 0; r--) {
    for (int f = 0; f <= 7; f++)
      printf((b & sq_bb(8 * r + f)) ? "| X " : "|   ");

    printf("| %d\n+---+---+---+---+---+---+---+---+\n", 1 + r);
  }
  printf("  a   b   c   d   e   f   g   h\n");
}


// bitboards_init() initializes various bitboard tables. It is called at
// startup and relies on global objects to be already zero-initialized.

void bitboards_init(void)
{
#ifndef USE_POPCNT
  for (unsigned i = 0; i < (1 << 16); ++i)
    PopCnt16[i] = popcount16(i);
#endif

  for (Square s = 0; s < 64; s++)
    SquareBB[s] = 1ULL << s;

  for (int f = 0; f < 8; f++)
    FileBB[f] = f > FILE_A ? FileBB[f - 1] << 1 : FileABB;

  for (int r = 0; r < 8; r++)
    RankBB[r] = r > RANK_1 ? RankBB[r - 1] << 8 : Rank1BB;

  for (int r = 0; r < 7; r++)
    ForwardRanksBB[WHITE][r] = ~(ForwardRanksBB[BLACK][r + 1] = ForwardRanksBB[BLACK][r] | RankBB[r]);

  for (int c = 0; c < 2; c++)
    for (Square s = 0; s < 64; s++) {
      ForwardFileBB[c][s]  = ForwardRanksBB[c][rank_of(s)] & FileBB[file_of(s)];
      PawnAttackSpan[c][s] = ForwardRanksBB[c][rank_of(s)] & adjacent_files_bb(file_of(s));
      PassedPawnSpan[c][s] = ForwardFileBB[c][s] | PawnAttackSpan[c][s];
    }

  for (Square s1 = 0; s1 < 64; s1++)
    for (Square s2 = 0; s2 < 64; s2++)
      if (s1 != s2) {
        SquareDistance[s1][s2] = max(distance_f(s1, s2), distance_r(s1, s2));
        DistanceRingBB[s1][SquareDistance[s1][s2]] |= sq_bb(s2);
      }

#ifndef PEDANTIC
  for (Square s = SQ_A4; s <= SQ_H5; s++)
    EPMask[s - SQ_A4] =  ((sq_bb(s) >> 1) & ~FileHBB)
                       | ((sq_bb(s) << 1) & ~FileABB);
#endif

  int steps[][5] = {
    {0}, { 7, 9 }, { 6, 10, 15, 17 }, {0}, {0}, {0}, { 1, 7, 8, 9 }
  };

  for (int c = 0; c < 2; c++)
    for (int pt = PAWN; pt <= KING; pt++)
      for (int s = 0; s < 64; s++)
        for (int i = 0; steps[pt][i]; i++) {
          Square to = s + (Square)(c == WHITE ? steps[pt][i] : -steps[pt][i]);

          if (square_is_ok(to) && distance(s, to) < 3) {
            if (pt == PAWN)
              PawnAttacks[c][s] |= sq_bb(to);
            else
              PseudoAttacks[pt][s] |= sq_bb(to);
          }
        }

  init_sliding_attacks();

  for (Square s1 = 0; s1 < 64; s1++) {
    PseudoAttacks[QUEEN][s1] = PseudoAttacks[BISHOP][s1] = attacks_bb_bishop(s1, 0);
    PseudoAttacks[QUEEN][s1] |= PseudoAttacks[ROOK][s1] = attacks_bb_rook(s1, 0);

    for (Square s2 = 0; s2 < 64; s2++) {
      BetweenBB[s1][s2] = sq_bb(s2);
      for (int pt = BISHOP; pt <= ROOK; pt++) {

        if (!(PseudoAttacks[pt][s1] & sq_bb(s2)))
          continue;

        LineBB[s1][s2] = (attacks_bb(pt, s1, 0) & attacks_bb(pt, s2, 0)) | sq_bb(s1) | sq_bb(s2);
        BetweenBB[s1][s2] |= attacks_bb(pt, s1, sq_bb(s2)) & attacks_bb(pt, s2, sq_bb(s1));
      }
    }
  }
}

/* ==========================================
   FILE: position.c
   ========================================== */



static void set_castling_right(Position *pos, Color c, Square rfrom);
static void set_state(Position *pos, Stack *st);

#ifndef NDEBUG
static int pos_is_ok(Position *pos, int *failedStep);
static int check_pos(Position *pos);
#else
#define check_pos(p) do {} while (0)
#endif

struct Zob zob;

Key matKey[16] = {
  0ULL,
  0x5ced000000000101ULL,
  0xe173000000001001ULL,
  0xd64d000000010001ULL,
  0xab88000000100001ULL,
  0x680b000001000001ULL,
  0x0000000000000001ULL,
  0ULL,
  0ULL,
  0xf219000010000001ULL,
  0xbb14000100000001ULL,
  0x58df001000000001ULL,
  0xa15f010000000001ULL,
  0x7c94100000000001ULL,
  0x0000000000000001ULL,
  0ULL
};

const char PieceToChar[] = " PNBRQK  pnbrqk";

int failed_step;

INLINE void put_piece(Position *pos, Color c, Piece piece, Square s)
{
  pos->board[s] = piece;
  pos->byTypeBB[0] |= sq_bb(s);
  pos->byTypeBB[type_of_p(piece)] |= sq_bb(s);
  pos->byColorBB[c] |= sq_bb(s);
}

INLINE void remove_piece(Position *pos, Color c, Piece piece, Square s)
{
  pos->byTypeBB[0] ^= sq_bb(s);
  pos->byTypeBB[type_of_p(piece)] ^= sq_bb(s);
  pos->byColorBB[c] ^= sq_bb(s);
  /* board[s] = 0;  Not needed, overwritten by the capturing one */
}

INLINE void move_piece(Position *pos, Color c, Piece piece, Square from,
    Square to)
{
  Bitboard fromToBB = sq_bb(from) ^ sq_bb(to);
  pos->byTypeBB[0] ^= fromToBB;
  pos->byTypeBB[type_of_p(piece)] ^= fromToBB;
  pos->byColorBB[c] ^= fromToBB;
  pos->board[from] = 0;
  pos->board[to] = piece;
}


// Calculate CheckInfo data.

INLINE void set_check_info(Position *pos)
{
  Stack *st = pos->st;

  st->blockersForKing[WHITE] = slider_blockers(pos, pieces_c(BLACK), square_of(WHITE, KING), &st->pinnersForKing[WHITE]);
  st->blockersForKing[BLACK] = slider_blockers(pos, pieces_c(WHITE), square_of(BLACK, KING), &st->pinnersForKing[BLACK]);

  Color them = !stm();
  st->ksq = square_of(them, KING);

  st->checkSquares[PAWN]   = attacks_from_pawn(st->ksq, them);
  st->checkSquares[KNIGHT] = attacks_from_knight(st->ksq);
  st->checkSquares[BISHOP] = attacks_from_bishop(st->ksq);
  st->checkSquares[ROOK]   = attacks_from_rook(st->ksq);
  st->checkSquares[QUEEN]  = st->checkSquares[BISHOP] | st->checkSquares[ROOK];
  st->checkSquares[KING]   = 0;
}


// print_pos() prints an ASCII representation of the position to stdout.

void print_pos(Position *pos)
{
  char fen[128];
  pos_fen(pos, fen);

  flockfile(stdout);
  printf("\n +---+---+---+---+---+---+---+---+\n");

  for (int r = 7; r >= 0; r--) {
    for (int f = 0; f <= 7; f++)
      printf(" | %c", PieceToChar[pos->board[8 * r + f]]);

    printf(" | %d\n +---+---+---+---+---+---+---+---+\n", r + 1);
  }

  printf("   a   b   c   d   e   f   g   h\n\nFen: %s\nKey: %16"PRIX64"\nCheckers: ", fen, key());

  char buf[16];
  for (Bitboard b = checkers(); b; )
    printf("%s ", uci_square(buf, pop_lsb(&b)));

  if (popcount(pieces()) <= TB_MaxCardinality && !can_castle_cr(ANY_CASTLING)) {
    int s1, s2;
    int wdl = TB_probe_wdl(pos, &s1);
    int dtz = TB_probe_dtz(pos, &s2);
    printf("\nTablebases WDL: %4d (%d)\nTablebases DTZ: %4d (%d)", wdl, s1, dtz, s2);
    if (s1 && wdl != 0) {
      Value dtm = TB_probe_dtm(pos, wdl, &s1);
      printf("\nTablebases DTM: %s (%d)", uci_value(buf, dtm), s1);
    }
  }
  printf("\n");
  fflush(stdout);
  funlockfile(stdout);
}

INLINE Key H1(Key h)
{
  return h & 0x1fff;
}

INLINE Key H2(Key h)
{
  return (h >> 16) & 0x1fff;
}

static Key cuckoo[8192];
static uint16_t cuckooMove[8192];

// zob_init() initializes at startup the various arrays used to compute
// hash keys.

void zob_init(void) {

  PRNG rng;
  prng_init(&rng, 1070372);

  for (int c = 0; c < 2; c++)
    for (int pt = PAWN; pt <= KING; pt++)
      for (Square s = 0; s < 64; s++)
        zob.psq[make_piece(c, pt)][s] = prng_rand(&rng);

  for (int f = 0; f < 8; f++)
    zob.enpassant[f] = prng_rand(&rng);

  for (int cr = 0; cr < 16; cr++)
    zob.castling[cr] = prng_rand(&rng);

  zob.side = prng_rand(&rng);
  zob.noPawns = prng_rand(&rng);

  // Prepare the cuckoo tables
  int count = 0;
  for (int c = 0; c < 2; c++)
    for (int pt = PAWN; pt <= KING; pt++) {
      int pc = make_piece(c, pt);
      for (Square s1 = 0; s1 < 64; s1++)
        for (Square s2 = s1 + 1; s2 < 64; s2++)
          if (PseudoAttacks[pt][s1] & sq_bb(s2)) {
//            Move move = between_bb(s1, s2) ? make_move(s1, s2)
//                                           : make_move(SQ_C3, SQ_D5);
            Move move = make_move(s1, s2);
            Key key = zob.psq[pc][s1] ^ zob.psq[pc][s2] ^ zob.side;
            uint32_t i = H1(key);
            while (true) {
              Key tmpKey = cuckoo[i];
              cuckoo[i] = key;
              key = tmpKey;
              Move tmpMove = cuckooMove[i];
              cuckooMove[i] = move;
              move = tmpMove;
              if (!move) break;
              i = (i == H1(key)) ? H2(key) : H1(key);
            }
            count++;
          }
    }
  assert(count == 3668);
}


// pos_set() initializes the position object with the given FEN string.
// This function is not very robust - make sure that input FENs are correct,
// this is assumed to be the responsibility of the GUI.

void pos_set(Position *pos, char *fen, int isChess960)
{
  unsigned char col, row, token;
  Square sq = SQ_A8;

  Stack *st = pos->st;
  memset(pos, 0, offsetof(Position, moveList));
  pos->st = st;
  memset(st, 0, StateSize);
  for (int i = 0; i < 16; i++)
    pos->pieceCount[i] = 0;

  // Piece placement
  while ((token = *fen++) && token != ' ') {
    if (token >= '0' && token <= '9')
      sq += token - '0'; // Advance the given number of files
    else if (token == '/')
      sq -= 16;
    else {
      for (int piece = 0; piece < 16; piece++)
        if (PieceToChar[piece] == token) {
          put_piece(pos, color_of(piece), piece, sq++);
          pos->pieceCount[piece]++;
          break;
        }
    }
  }

  // Active color
  token = *fen++;
  pos->sideToMove = token == 'w' ? WHITE : BLACK;
  token = *fen++;

  // Castling availability. Compatible with 3 standards: Normal FEN
  // standard, Shredder-FEN that uses the letters of the columns on which
  // the rooks began the game instead of KQkq and also X-FEN standard
  // that, in case of Chess960, // if an inner rook is associated with
  // the castling right, the castling tag is replaced by the file letter
  // of the involved rook, as for the Shredder-FEN.
  while ((token = *fen++) && !isspace(token)) {
    Square rsq;
    int c = islower(token) ? BLACK : WHITE;
    Piece rook = make_piece(c, ROOK);

    token = toupper(token);

    if (token == 'K')
      for (rsq = relative_square(c, SQ_H1); piece_on(rsq) != rook; --rsq);
    else if (token == 'Q')
      for (rsq = relative_square(c, SQ_A1); piece_on(rsq) != rook; ++rsq);
    else if (token >= 'A' && token <= 'H')
      rsq = make_square(token - 'A', relative_rank(c, RANK_1));
    else
      continue;

    set_castling_right(pos, c, rsq);
  }

  // En passant square. Ignore if no pawn capture is possible.
  if (   ((col = *fen++) && (col >= 'a' && col <= 'h'))
      && ((row = *fen++) && (row == (stm() == WHITE ? '6' : '3'))))
  {
    st->epSquare = make_square(col - 'a', row - '1');

    // We assume a legal FEN, i.e. if epSquare is present, then the previous
    // move was a legal double pawn push.
    if (!(attackers_to(st->epSquare) & pieces_cp(stm(), PAWN)))
      st->epSquare = 0;
  }
  else
    st->epSquare = 0;

  // Halfmove clock and fullmove number
  st->rule50 = strtol(fen, &fen, 10);
  pos->gamePly = strtol(fen, NULL, 10);

  // Convert from fullmove starting from 1 to ply starting from 0,
  // handle also common incorrect FEN with fullmove = 0.
  pos->gamePly = max(2 * (pos->gamePly - 1), 0) + (stm() == BLACK);

  pos->chess960 = isChess960;
  set_state(pos, st);

  assert(pos_is_ok(pos, &failed_step));
}


// set_castling_right() is a helper function used to set castling rights
// given the corresponding color and the rook starting square.

static void set_castling_right(Position *pos, Color c, Square rfrom)
{
  Square kfrom = square_of(c, KING);
  int cs = kfrom < rfrom ? KING_SIDE : QUEEN_SIDE;
  int cr = (WHITE_OO << ((cs == QUEEN_SIDE) + 2 * c));

  Square kto = relative_square(c, cs == KING_SIDE ? SQ_G1 : SQ_C1);
  Square rto = relative_square(c, cs == KING_SIDE ? SQ_F1 : SQ_D1);

  pos->st->castlingRights |= cr;

  pos->castlingRightsMask[kfrom] |= cr;
  pos->castlingRightsMask[rfrom] |= cr;
  pos->castlingRookSquare[cr] = rfrom;

  for (Square s = min(rfrom, rto); s <= max(rfrom, rto); s++)
    if (s != kfrom && s != rfrom)
      pos->castlingPath[cr] |= sq_bb(s);

  for (Square s = min(kfrom, kto); s <= max(kfrom, kto); s++)
    if (s != kfrom && s != rfrom)
      pos->castlingPath[cr] |= sq_bb(s);
}


// set_state() computes the hash keys of the position, and other data
// that once computed is updated incrementally as moves are made. The
// function is only used when a new position is set up, and to verify
// the correctness of the Stack data when running in debug mode.

static void set_state(Position *pos, Stack *st)
{
  st->key = st->materialKey = 0;
#ifndef NNUE_PURE
  st->pawnKey = zob.noPawns;
  st->psq = 0;
#endif
  st->nonPawn = 0;

  st->checkersBB = attackers_to(square_of(stm(), KING)) & pieces_c(!stm());

  set_check_info(pos);

  for (Bitboard b = pieces(); b; ) {
    Square s = pop_lsb(&b);
    Piece pc = piece_on(s);
    st->key ^= zob.psq[pc][s];
#ifndef NNUE_PURE
    st->psq += psqt.psq[pc][s];
#endif
  }

// emulate a bug in Stockfish
//  if (st->epSquare != 0)
    st->key ^= zob.enpassant[file_of(st->epSquare)];

  if (stm() == BLACK)
    st->key ^= zob.side;

  st->key ^= zob.castling[st->castlingRights];

#ifndef NNUE_PURE
  for (Bitboard b = pieces_p(PAWN); b; ) {
    Square s = pop_lsb(&b);
    st->pawnKey ^= zob.psq[piece_on(s)][s];
  }
#endif

  for (PieceType pt = PAWN; pt <= KING; pt++) {
    st->materialKey += piece_count(WHITE, pt) * matKey[8 * WHITE + pt];
    st->materialKey += piece_count(BLACK, pt) * matKey[8 * BLACK + pt];
  }

  for (PieceType pt = KNIGHT; pt <= QUEEN; pt++)
    for (int c = 0; c < 2; c++)
      st->nonPawn += piece_count(c, pt) * NonPawnPieceValue[make_piece(c, pt)];
}


// pos_fen() returns a FEN representation of the position. In case of
// Chess960 the Shredder-FEN notation is used. This is used for copying
// the root position to search threads.

void pos_fen(const Position *pos, char *str)
{
  int cnt;

  for (int r = 7; r >= 0; r--) {
    for (int f = 0; f < 8; f++) {
      for (cnt = 0; f < 8 && !piece_on(8 * r + f); f++)
        cnt++;
      if (cnt) *str++ = '0' + cnt;
      if (f < 8) *str++ = PieceToChar[piece_on(8 * r + f)];
    }
    if (r > 0) *str++ = '/';
  }

  *str++ = ' ';
  *str++ = stm() == WHITE ? 'w' : 'b';
  *str++ = ' ';

  int cr = pos->st->castlingRights;

  if (!is_chess960()) {
    if (cr & WHITE_OO) *str++ = 'K';
    if (cr & WHITE_OOO) *str++ = 'Q';
    if (cr & BLACK_OO) *str++ = 'k';
    if (cr & BLACK_OOO) *str++ = 'q';
  } else {
    if (cr & WHITE_OO) *str++ = 'A' + file_of(castling_rook_square(make_castling_right(WHITE, KING_SIDE)));
    if (cr & WHITE_OOO) *str++ = 'A' + file_of(castling_rook_square(make_castling_right(WHITE, QUEEN_SIDE)));
    if (cr & BLACK_OO) *str++ = 'a' + file_of(castling_rook_square(make_castling_right(BLACK, KING_SIDE)));
    if (cr & BLACK_OOO) *str++ = 'a' + file_of(castling_rook_square(make_castling_right(BLACK, QUEEN_SIDE)));
  }
  if (!cr)
      *str++ = '-';

  *str++ = ' ';
  if (ep_square() != 0) {
    *str++ = 'a' + file_of(ep_square());
    *str++ = '1' + rank_of(ep_square());
  } else {
    *str++ = '-';
  }

  sprintf(str, " %d %d", rule50_count(), 1 + (game_ply()-(stm() == BLACK)) / 2);
}


// Turning slider_blockers() into an inline function was slower, even
// though it should only add a single slightly optimised copy to evaluate().
#if 1
// slider_blockers() returns a bitboard of all pieces that are blocking
// attacks on the square 's' from 'sliders'. A piece blocks a slider if
// removing that piece from the board would result in a position where
// square 's' is attacked. Both pinned pieces and discovered check
// candidates are slider blockers and are calculated by calling this
// function.

Bitboard slider_blockers(const Position *pos, Bitboard sliders, Square s,
    Bitboard *pinners)
{
  Bitboard blockers = 0, snipers;
  *pinners = 0;

  // Snipers are sliders that attack square 's' when a piece and other
  // snipers are removed.
  snipers = (  (PseudoAttacks[ROOK  ][s] & pieces_pp(QUEEN, ROOK))
             | (PseudoAttacks[BISHOP][s] & pieces_pp(QUEEN, BISHOP))) & sliders;
  Bitboard occupancy = pieces() ^ snipers;

  while (snipers) {
    Square sniperSq = pop_lsb(&snipers);
    Bitboard b = between_bb(s, sniperSq) & occupancy;

    if (b && !more_than_one(b)) {
      blockers |= b;
      if (b & pieces_c(color_of(piece_on(s))))
        *pinners |= sq_bb(sniperSq);
    }
  }
  return blockers;
}
#endif


#if 0
// attackers_to() computes a bitboard of all pieces which attack a given
// square. Slider attacks use the occupied bitboard to indicate occupancy.

Bitboard attackers_to_occ(const Position *pos, Square s, Bitboard occupied)
{
  return  (attacks_from_pawn(s, BLACK)    & pieces_cp(WHITE, PAWN))
        | (attacks_from_pawn(s, WHITE)    & pieces_cp(BLACK, PAWN))
        | (attacks_from_knight(s)         & pieces_p(KNIGHT))
        | (attacks_bb_rook(s, occupied)   & pieces_pp(ROOK,   QUEEN))
        | (attacks_bb_bishop(s, occupied) & pieces_pp(BISHOP, QUEEN))
        | (attacks_from_king(s)           & pieces_p(KING));
}
#endif


// is_legal() tests whether a pseudo-legal move is legal

bool is_legal(const Position *pos, Move m)
{
  assert(move_is_ok(m));

  Color us = stm();
  Square from = from_sq(m);
  Square to = to_sq(m);

  assert(color_of(moved_piece(m)) == us);
  assert(piece_on(square_of(us, KING)) == make_piece(us, KING));

  // En passant captures are a tricky special case. Because they are rather
  // uncommon, we do it simply by testing whether the king is attacked after
  // the move is made.
  if (unlikely(type_of_m(m) == ENPASSANT)) {
    Square ksq = square_of(us, KING);
    Square capsq = to ^ 8;
    Bitboard occupied = pieces() ^ sq_bb(from) ^ sq_bb(capsq) ^ sq_bb(to);

    assert(to == ep_square());
    assert(moved_piece(m) == make_piece(us, PAWN));
    assert(piece_on(capsq) == make_piece(!us, PAWN));
    assert(piece_on(to) == 0);

    return   !(attacks_bb_rook  (ksq, occupied) & pieces_cpp(!us, QUEEN, ROOK))
          && !(attacks_bb_bishop(ksq, occupied) & pieces_cpp(!us, QUEEN, BISHOP));
  }

  // Check legality of castling moves.
  if (unlikely(type_of_m(m) == CASTLING)) {
    // to > from works both for standard chess and for Chess960.
    to = relative_square(us, to > from ? SQ_G1 : SQ_C1);
    int step = to > from ? WEST : EAST;

    for (Square s = to; s != from; s += step)
      if (attackers_to(s) & pieces_c(!us))
        return false;

    // For Chess960, verify that moving the castling rook does not discover
    // some hidden checker, e.g. on SQ_A1 when castling rook is on SQ_B1.
    return !is_chess960() || !(blockers_for_king(pos, us) & sq_bb(to_sq(m)));
  }

  // If the moving piece is a king, check whether the destination
  // square is attacked by the opponent. Castling moves are checked
  // for legality during move generation.
  if (pieces_p(KING) & sq_bb(from))
    return !(attackers_to_occ(pos, to, pieces() ^ sq_bb(from)) & pieces_c(!us));

  // A non-king move is legal if and only if it is not pinned or it
  // is moving along the ray towards or away from the king.
  return   !(blockers_for_king(pos, us) & sq_bb(from))
        ||  aligned(m, square_of(us, KING));
}


// is_pseudo_legal() takes a random move and tests whether the move is
// pseudo legal. It is used to validate moves from TT that can be corrupted
// due to SMP concurrent access or hash position key aliasing.

#if 0
bool is_pseudo_legal_old(Position *pos, Move m)
{
  Color us = stm();
  Square from = from_sq(m);
  Square to = to_sq(m);
  Piece pc = moved_piece(m);

  // Use a slower but simpler function for uncommon cases
  if (type_of_m(m) != NORMAL) {
    ExtMove list[MAX_MOVES];
    ExtMove *last = generate_legal(pos, list);
    for (ExtMove *p = list; p < last; p++)
      if (p->move == m)
        return true;
    return false;
  }

  // Is not a promotion, so promotion piece must be empty
  if (promotion_type(m) - KNIGHT != 0)
    return false;

  // If the 'from' square is not occupied by a piece belonging to the side to
  // move, the move is obviously not legal.
  if (pc == 0 || color_of(pc) != us)
    return false;

  // The destination square cannot be occupied by a friendly piece
  if (pieces_c(us) & sq_bb(to))
    return false;

  // Handle the special case of a pawn move
  if (type_of_p(pc) == PAWN) {
    // We have already handled promotion moves, so destination
    // cannot be on the 8th/1st rank.
    if (!((to + 0x08) & 0x30))
      return false;

    if (   !(attacks_from_pawn(from, us) & pieces_c(!us) & sq_bb(to)) // Not a capture
        && !((from + pawn_push(us) == to) && is_empty(to))       // Not a single push
        && !( (from + 2 * pawn_push(us) == to)              // Not a double push
           && (rank_of(from) == relative_rank(us, RANK_2))
           && is_empty(to)
           && is_empty(to - pawn_push(us))))
      return false;
  }
  else if (!(attacks_from(pc, from) & sq_bb(to)))
    return false;

  // Evasions generator already takes care to avoid some kind of illegal moves
  // and legal() relies on this. We therefore have to take care that the same
  // kind of moves are filtered out here.
  if (checkers()) {
    if (type_of_p(pc) != KING) {
      // Double check? In this case a king move is required
      if (more_than_one(checkers()))
        return false;

      // Our move must be a blocking evasion or a capture of the checking piece
      if (!((between_bb(lsb(checkers()), square_of(us, KING)) | checkers()) & sq_bb(to)))
        return false;
    }
    // In case of king moves under check we have to remove king so as to catch
    // invalid moves like b1a1 when opposite queen is on c1.
    else if (attackers_to_occ(pos, to, pieces() ^ sq_bb(from)) & pieces_c(!us))
      return false;
  }

  return true;
}
#endif

bool is_pseudo_legal(const Position *pos, Move m)
{
  Color us = stm();
  Square from = from_sq(m);

  if (!(pieces_c(us) & sq_bb(from)))
    return false;

  if (unlikely(type_of_m(m) == CASTLING)) {
    if (checkers()) return false;
    ExtMove list[MAX_MOVES];
    ExtMove *end = generate_quiets(pos, list);
    for (ExtMove *p = list; p < end; p++)
      if (p->move == m) return true;
    return false;
  }

  Square to = to_sq(m);
  if (pieces_c(us) & sq_bb(to))
    return false;

  PieceType pt = type_of_p(piece_on(from));
  if (pt != PAWN) {
    if (type_of_m(m) != NORMAL)
      return false;
    switch (pt) {
    case KNIGHT:
      if (!(attacks_from_knight(from) & sq_bb(to)))
        return false;
      break;
    case BISHOP:
      if (!(attacks_from_bishop(from) & sq_bb(to)))
        return false;
      break;
    case ROOK:
      if (!(attacks_from_rook(from) & sq_bb(to)))
        return false;
      break;
    case QUEEN:
      if (!(attacks_from_queen(from) & sq_bb(to)))
        return false;
      break;
    case KING:
      if (!(attacks_from_king(from) & sq_bb(to)))
        return false;
      // is_legal() does not remove the "from" square from the "occupied"
      // bitboard when checking that the king is not in check on the "to"
      // square. So we need to be careful here.
      if (   checkers()
          && (attackers_to_occ(pos, to, pieces() ^ sq_bb(from)) & pieces_c(!us)))
        return false;
      return true;
    default:
      assume(false);
      break;
    }
  } else {
    if (likely(type_of_m(m) == NORMAL)) {
      if (!((to + 0x08) & 0x30))
        return false;
      if (   !(attacks_from_pawn(from, us) & pieces_c(!us) & sq_bb(to))
          && !((from + pawn_push(us) == to) && is_empty(to))
          && !(   from + 2 * pawn_push(us) == to
               && rank_of(from) == relative_rank(us, RANK_2)
               && is_empty(to) && is_empty(to - pawn_push(us))))
        return false;
    }
    else if (likely(type_of_m(m) == PROMOTION)) {
      // No need to test for pawn to 8th rank.
      if (   !(attacks_from_pawn(from, us) & pieces_c(!us) & sq_bb(to))
          && !((from + pawn_push(us) == to) && is_empty(to)))
        return false;
    }
    else
      return to == ep_square() && (attacks_from_pawn(from, us) & sq_bb(to));
  }
  if (checkers()) {
    // Again we need to be a bit careful.
    if (more_than_one(checkers()))
      return false;
    if (!(between_bb(square_of(us, KING), lsb(checkers())) & sq_bb(to)))
      return false;
  }
  return true;
}

#if 0
int is_pseudo_legal(Position *pos, Move m)
{
  int r1 = is_pseudo_legal_old(pos, m);
  int r2 = is_pseudo_legal_new(pos, m);
  if (r1 != r2) {
    printf("old: %d, new: %d\n", r1, r2);
    printf("old: %d\n", is_pseudo_legal_old(pos, m));
    printf("new: %d\n", is_pseudo_legal_new(pos, m));
exit(1);
  }
  return r1;
}
#endif


// gives_check_special() is invoked by gives_check() if there are
// discovered check candidates or the move is of a special type

bool gives_check_special(const Position *pos, Stack *st, Move m)
{
  assert(move_is_ok(m));
  assert(color_of(moved_piece(m)) == stm());

  Square from = from_sq(m);
  Square to = to_sq(m);

  if ((blockers_for_king(pos, !stm()) & sq_bb(from)) && !aligned(m, st->ksq))
    return true;

  switch (type_of_m(m)) {
  case NORMAL:
    return st->checkSquares[type_of_p(piece_on(from))] & sq_bb(to);

  case PROMOTION:
    return attacks_bb(promotion_type(m), to, pieces() ^ sq_bb(from)) & sq_bb(st->ksq);

  case ENPASSANT:
  {
    if (st->checkSquares[PAWN] & sq_bb(to))
      return true;
    Square capsq = make_square(file_of(to), rank_of(from));
//    Bitboard b = pieces() ^ sq_bb(from) ^ sq_bb(capsq) ^ sq_bb(to);
    Bitboard b = inv_sq(inv_sq(inv_sq(pieces(), from), to), capsq);
    return  (attacks_bb_rook  (st->ksq, b) & pieces_cpp(stm(), QUEEN, ROOK))
          ||(attacks_bb_bishop(st->ksq, b) & pieces_cpp(stm(), QUEEN, BISHOP));
  }
  case CASTLING:
  {
    // Castling is encoded as 'King captures the rook'
    Square rto = relative_square(stm(), to > from ? SQ_F1 : SQ_D1);
    return   (PseudoAttacks[ROOK][rto] & sq_bb(st->ksq))
          && (attacks_bb_rook(rto, pieces() ^ sq_bb(from)) & sq_bb(st->ksq));
  }
  default:
    assume(false);
    return false;
  }
}


// do_move() makes a move. The move is assumed to be legal.

void do_move(Position *pos, Move m, int givesCheck)
{
  assert(move_is_ok(m));

  Key key = pos->st->key ^ zob.side;

  // Copy some fields of the old state to our new Stack object except the
  // ones which are going to be recalculated from scratch anyway and then
  // switch our state pointer to point to the new (ready to be updated)
  // state.
  Stack *st = ++pos->st;
  memcpy(st, st - 1, (StateCopySize + 7) & ~7);

  // Increment ply counters. Note that rule50 will be reset to zero later
  // on in case of a capture or a pawn move.
  st->plyCounters += 0x101; // Increment both rule50 and pliesFromNull

#ifdef NNUE
  st->accumulator.state[WHITE] = ACC_EMPTY;
  st->accumulator.state[BLACK] = ACC_EMPTY;
  DirtyPiece *dp = &(st->dirtyPiece);
  dp->dirtyNum = 1;
#endif

  Color us = stm();
  Color them = !us;
  Square from = from_sq(m);
  Square to = to_sq(m);
  Piece piece = piece_on(from);
  Piece captured =  type_of_m(m) == ENPASSANT
                  ? make_piece(them, PAWN) : piece_on(to);

  assert(color_of(piece) == us);
  assert(   is_empty(to)
         || color_of(piece_on(to)) == (type_of_m(m) != CASTLING ? them : us));
  assert(type_of_p(captured) != KING);

  if (unlikely(type_of_m(m) == CASTLING)) {
    assert(piece == make_piece(us, KING));
    assert(captured == make_piece(us, ROOK));

    Square rfrom, rto;

    int kingSide = to > from;
    rfrom = to; // Castling is encoded as "king captures friendly rook"
    rto = relative_square(us, kingSide ? SQ_F1 : SQ_D1);
    to = relative_square(us, kingSide ? SQ_G1 : SQ_C1);

#ifdef NNUE
    dp->dirtyNum = 2;
    dp->pc[1] = captured;
    dp->from[1] = rfrom;
    dp->to[1] = rto;
#endif

    // Remove both pieces first since squares could overlap in Chess960
    remove_piece(pos, us, piece, from);
    remove_piece(pos, us, captured, rfrom);
    pos->board[from] = pos->board[rfrom] = 0;
    put_piece(pos, us, piece, to);
    put_piece(pos, us, captured, rto);

#ifndef NNUE_PURE
    st->psq += psqt.psq[captured][rto] - psqt.psq[captured][rfrom];
#endif
    key ^= zob.psq[captured][rfrom] ^ zob.psq[captured][rto];
    captured = 0;
  }

  else if (captured) {
    Square capsq = to;

    // If the captured piece is a pawn, update pawn hash key. Otherwise,
    // update non-pawn material.
    if (type_of_p(captured) == PAWN) {
      if (unlikely(type_of_m(m) == ENPASSANT)) {
        capsq ^= 8;

        assert(piece == make_piece(us, PAWN));
        assert(to == (st-1)->epSquare);
        assert(relative_rank_s(us, to) == RANK_6);
        assert(is_empty(to));
        assert(piece_on(capsq) == make_piece(them, PAWN));

        pos->board[capsq] = 0; // Not done by remove_piece()
      }

#ifndef NNUE_PURE
      st->pawnKey ^= zob.psq[captured][capsq];
#endif
    } else
      st->nonPawn -= NonPawnPieceValue[captured];

#ifdef NNUE
    dp->dirtyNum = 2; // captured piece goes off the board
    dp->pc[1] = captured;
    dp->from[1] = capsq;
    dp->to[1] = SQ_NONE;
#endif

    // Update board
    remove_piece(pos, them, captured, capsq);
    pos->pieceCount[captured]--;

    // Update material hash key and prefetch access to materialTable
    key ^= zob.psq[captured][capsq];
    st->materialKey -= matKey[captured];
#ifndef NNUE_PURE
    prefetch(&pos->materialTable[st->materialKey >> (64 - 13)]);

    // Update incremental scores
    st->psq -= psqt.psq[captured][capsq];
#endif

    // Reset ply counters
    st->plyCounters = 0;
  }

  // Set captured piece
  st->capturedPiece = captured;

  // Update hash key
  key ^= zob.psq[piece][from] ^ zob.psq[piece][to];

  // Reset en passant square
  if (unlikely((st-1)->epSquare != 0))
    key ^= zob.enpassant[file_of((st-1)->epSquare)];
  st->epSquare = 0;

  // Update castling rights if needed
  if (    st->castlingRights
      && (pos->castlingRightsMask[from] | pos->castlingRightsMask[to]))
  {
//    uint32_t cr = pos->castlingRightsMask[from] | pos->castlingRightsMask[to];
//    key ^= zob.castling[st->castlingRights & cr];
//    st->castlingRights &= ~cr;
    key ^= zob.castling[st->castlingRights];
    st->castlingRights &= ~(pos->castlingRightsMask[from] | pos->castlingRightsMask[to]);
    key ^= zob.castling[st->castlingRights];
  }

#ifdef NNUE
    dp->pc[0] = piece;
    dp->from[0] = from;
    dp->to[0] = to;
#endif

  // Move the piece. The tricky Chess960 castling is handled earlier.
  if (likely(type_of_m(m) != CASTLING))
    move_piece(pos, us, piece, from, to);

  // If the moving piece is a pawn do some special extra work
  if (type_of_p(piece) == PAWN) {
    // Set en-passant square if the moved pawn can be captured
    if (   (to ^ from) == 16
        && (attacks_from_pawn(to ^ 8, us) & pieces_cp(them, PAWN)))
    {
      st->epSquare = to ^ 8;
      key ^= zob.enpassant[file_of(st->epSquare)];
    }
    else if (type_of_m(m) == PROMOTION) {
      Piece promotion = make_piece(us, promotion_type(m));

      assert(relative_rank_s(us, to) == RANK_8);
      assert(type_of_p(promotion) >= KNIGHT && type_of_p(promotion) <= QUEEN);

      remove_piece(pos, us, piece, to);
      pos->pieceCount[piece]--;
      put_piece(pos, us, promotion, to);
      pos->pieceCount[promotion]++;

#ifdef NNUE
      dp->to[0] = SQ_NONE;   // pawn to SQ_NONE, promoted piece from SQ_NONE
      dp->pc[dp->dirtyNum] = promotion;
      dp->from[dp->dirtyNum] = SQ_NONE;
      dp->to[dp->dirtyNum] = to;
      dp->dirtyNum++;
#endif

      // Update hash keys
      key ^= zob.psq[piece][to] ^ zob.psq[promotion][to];
#ifndef NNUE_PURE
      st->pawnKey ^= zob.psq[piece][to];
#endif
      st->materialKey += matKey[promotion] - matKey[piece];

#ifndef NNUE_PURE
      // Update incremental score
      st->psq += psqt.psq[promotion][to] - psqt.psq[piece][to];
#endif

      // Update material
      st->nonPawn += NonPawnPieceValue[promotion];
    }

#ifndef NNUE_PURE
    // Update pawn hash key and prefetch access to pawnsTable
    st->pawnKey ^= zob.psq[piece][from] ^ zob.psq[piece][to];
    prefetch2(&pos->pawnTable[st->pawnKey & (PAWN_ENTRIES -1)]);
#endif

    // Reset ply counters.
    st->plyCounters = 0;
  }

#ifndef NNUE_PURE
  // Update incremental scores
  st->psq += psqt.psq[piece][to] - psqt.psq[piece][from];
#endif

  // Update the key with the final value
  st->key = key;

  // Calculate checkers bitboard (if move gives check)
#if 1
  st->checkersBB =  givesCheck
                  ? attackers_to(square_of(them, KING)) & pieces_c(us) : 0;
#else
  st->checkersBB = 0;
  if (givesCheck) {
    if (type_of_m(m) != NORMAL || ((st-1)->blockersForKing[them] & sq_bb(from)))
      st->checkersBB = attackers_to(square_of(them, KING)) & pieces_c(us);
    else
      st->checkersBB = (st-1)->checkSquares[piece & 7] & sq_bb(to);
  }
#endif

  pos->sideToMove = !pos->sideToMove;
  pos->nodes++;

  set_check_info(pos);

  assert(pos_is_ok(pos, &failed_step));
}


// undo_move() unmakes a move. When it returns, the position should
// be restored to exactly the same state as before the move was made.

void undo_move(Position *pos, Move m)
{
  assert(move_is_ok(m));

  pos->sideToMove = !pos->sideToMove;

  Color us = stm();
  Square from = from_sq(m);
  Square to = to_sq(m);
  Piece pc = piece_on(to);

  assert(is_empty(from) || type_of_m(m) == CASTLING);
  assert(type_of_p(pos->st->capturedPiece) != KING);

  if (unlikely(type_of_m(m) == PROMOTION)) {
    assert(relative_rank_s(us, to) == RANK_8);
    assert(type_of_p(pc) == promotion_type(m));
    assert(type_of_p(pc) >= KNIGHT && type_of_p(pc) <= QUEEN);

    remove_piece(pos, us, pc, to);
    pos->pieceCount[pc]--;
    pc = make_piece(us, PAWN);
    put_piece(pos, us, pc, to);
    pos->pieceCount[pc]++;
  }

  if (unlikely(type_of_m(m) == CASTLING)) {
    Square rfrom, rto;
    int kingSide = to > from;
    rfrom = to; // Castling is encoded as "king captures friendly rook"
    rto = relative_square(us, kingSide ? SQ_F1 : SQ_D1);
    to = relative_square(us, kingSide ? SQ_G1 : SQ_C1);
    Piece king = make_piece(us, KING);
    Piece rook = make_piece(us, ROOK);

    // Remove both pieces first since squares could overlap in Chess960
    remove_piece(pos, us, king, to);
    remove_piece(pos, us, rook, rto);
    pos->board[to] = pos->board[rto] = 0;
    put_piece(pos, us, king, from);
    put_piece(pos, us, rook, rfrom);
  } else {
    move_piece(pos, us, pc, to, from); // Put the piece back at the source square

    if (pos->st->capturedPiece) {
      Square capsq = to;

      if (unlikely(type_of_m(m) == ENPASSANT)) {
        capsq ^= 8;

        assert(type_of_p(pc) == PAWN);
        assert(to == (pos->st-1)->epSquare);
        assert(relative_rank_s(us, to) == RANK_6);
        assert(is_empty(capsq));
        assert(pos->st->capturedPiece == make_piece(!us, PAWN));
      }

      put_piece(pos, !us, pos->st->capturedPiece, capsq); // Restore the captured piece
      pos->pieceCount[pos->st->capturedPiece]++;
    }
  }

  // Finally, point our state pointer back to the previous state
  pos->st--;

  assert(pos_is_ok(pos, &failed_step));
}


// do_null_move() is used to do a null move

void do_null_move(Position *pos)
{
  assert(!checkers());

  Stack *st = ++pos->st;
  memcpy(st, st - 1, (StateSize + 7) & ~7);
#ifdef NNUE
  st->accumulator.state[WHITE] = ACC_EMPTY;
  st->accumulator.state[BLACK] = ACC_EMPTY;
  st->dirtyPiece.dirtyNum = 0;
  st->dirtyPiece.pc[0] = 0;
#endif

  if (unlikely(st->epSquare)) {
    st->key ^= zob.enpassant[file_of(st->epSquare)];
    st->epSquare = 0;
  }

  st->key ^= zob.side;
  prefetch(tt_first_entry(st->key));

  st->rule50++;
  st->pliesFromNull = 0;

  pos->sideToMove = !pos->sideToMove;

  set_check_info(pos);

  assert(pos_is_ok(pos, &failed_step));
}

// See position.h for undo_null_move()


// key_after() computes the new hash key after the given move. Needed
// for speculative prefetch. It does not recognize special moves like
// castling, en-passant and promotions.

Key key_after(const Position *pos, Move m)
{
  Square from = from_sq(m);
  Square to = to_sq(m);
  Piece pc = piece_on(from);
  Piece captured = piece_on(to);
  Key k = pos->st->key ^ zob.side;

  if (captured)
    k ^= zob.psq[captured][to];

  return k ^ zob.psq[pc][to] ^ zob.psq[pc][from];
}


// Test whether SEE >= value.
bool see_test(const Position *pos, Move m, int value)
{
  if (unlikely(type_of_m(m) != NORMAL))
    return 0 >= value;

  Square from = from_sq(m), to = to_sq(m);
  Bitboard occ;

  int swap = PieceValue[MG][piece_on(to)] - value;
  if (swap < 0)
    return false;

  swap = PieceValue[MG][piece_on(from)] - swap;
  if (swap <= 0)
    return true;

  occ = pieces() ^ sq_bb(from) ^ sq_bb(to);
  Color stm = color_of(piece_on(from));
  Bitboard attackers = attackers_to_occ(pos, to, occ), stmAttackers;
  bool res = true;

  while (true) {
    stm = !stm;
    attackers &= occ;
    if (!(stmAttackers = attackers & pieces_c(stm))) break;
    if (    (stmAttackers & blockers_for_king(pos, stm))
        && (pos->st->pinnersForKing[stm] & occ))
      stmAttackers &= ~blockers_for_king(pos, stm);
    if (!stmAttackers) break;
    res = !res;
    Bitboard bb;
    if ((bb = stmAttackers & pieces_p(PAWN))) {
      if ((swap = PawnValueMg - swap) < res) break;
      occ ^= bb & -bb;
      attackers |= attacks_bb_bishop(to, occ) & pieces_pp(BISHOP, QUEEN);
    }
    else if ((bb = stmAttackers & pieces_p(KNIGHT))) {
      if ((swap = KnightValueMg - swap) < res) break;
      occ ^= bb & -bb;
    }
    else if ((bb = stmAttackers & pieces_p(BISHOP))) {
      if ((swap = BishopValueMg - swap) < res) break;
      occ ^= bb & -bb;
      attackers |= attacks_bb_bishop(to, occ) & pieces_pp(BISHOP, QUEEN);
    }
    else if ((bb = stmAttackers & pieces_p(ROOK))) {
      if ((swap = RookValueMg - swap) < res) break;
      occ ^= bb & -bb;
      attackers |= attacks_bb_rook(to, occ) & pieces_pp(ROOK, QUEEN);
    }
    else if ((bb = stmAttackers & pieces_p(QUEEN))) {
      if ((swap = QueenValueMg - swap) < res) break;
      occ ^= bb & -bb;
      attackers |=  (attacks_bb_bishop(to, occ) & pieces_pp(BISHOP, QUEEN))
                  | (attacks_bb_rook(to, occ) & pieces_pp(ROOK, QUEEN));
    }
    else // KING
      return (attackers & ~pieces_c(stm)) ? !res : res;
  }

  return res;
}


// is_draw() tests whether the position is drawn by 50-move rule or by
// repetition. It does not detect stalemates.

SMALL
bool is_draw(const Position *pos)
{
  Stack *st = pos->st;

  if (unlikely(st->rule50 > 99)) {
    if (!checkers())
      return true;
    return generate_legal(pos, (st-1)->endMoves) != (st-1)->endMoves;
  }

  // st->pliesFromNull is reset both on null moves and on zeroing moves.
  int e = st->pliesFromNull - 4;
  if (e >= 0) {
    Stack *stp = st - 2;
    for (int i = 0; i <= e; i += 2) {
      stp -= 2;
      if (stp->key == st->key)
        return true; // Draw at first repetition
    }
  }

  return false;
}


// has_game_cycle() tests if the position has a move which draws by
// repetition or an earlier position has a move that directly reaches
// the current position.

bool has_game_cycle(const Position *pos, int ply)
{
  unsigned int j;

  int end = pos->st->pliesFromNull;

  Key originalKey = pos->st->key;
  Stack *stp = pos->st - 1;

  for (int i = 3; i <= end; i += 2) {
    stp -= 2;

    Key moveKey = originalKey ^ stp->key;
    if (   (j = H1(moveKey), cuckoo[j] == moveKey)
        || (j = H2(moveKey), cuckoo[j] == moveKey))
    {
      Move m = cuckooMove[j];
      if (!((((Bitboard *)BetweenBB)[m] ^ sq_bb(to_sq(m))) & pieces())) {
        if (   ply > i
            || color_of(piece_on(is_empty(from_sq(m)) ? to_sq(m) : from_sq(m))) == stm())
          return true;
      }
    }
  }
  return false;
}


void pos_set_check_info(Position *pos)
{
  set_check_info(pos);
}

// pos_is_ok() performs some consistency checks for the position object.
// This is meant to be helpful when debugging.

#ifndef NDEBUG
static int pos_is_ok(Position *pos, int *failedStep)
{
  int Fast = 1; // Quick (default) or full check?

  enum { Default, King, Bitboards, StackOK, Lists, Castling };

  for (int step = Default; step <= (Fast ? Default : Castling); step++) {
    if (failedStep)
      *failedStep = step;

    if (step == Default)
      if (   (stm() != WHITE && stm() != BLACK)
          || piece_on(square_of(WHITE, KING)) != W_KING
          || piece_on(square_of(BLACK, KING)) != B_KING
          || ( ep_square() && relative_rank_s(stm(), ep_square()) != RANK_6))
        return 0;

#if 0
    if (step == King)
      if (   std::count(board, board + SQUARE_NB, W_KING) != 1
          || std::count(board, board + SQUARE_NB, B_KING) != 1
          || attackers_to(square_of(!stm(), KING)) & pieces_c(stm()))
        return 0;
#endif

    if (step == Bitboards) {
      if (  (pieces_c(WHITE) & pieces_c(BLACK))
          ||(pieces_c(WHITE) | pieces_c(BLACK)) != pieces())
        return 0;

      for (int p1 = PAWN; p1 <= KING; p1++)
        for (int p2 = PAWN; p2 <= KING; p2++)
          if (p1 != p2 && (pieces_p(p1) & pieces_p(p2)))
            return 0;
    }

    if (step == StackOK) {
      Stack si = *(pos->st);
      set_state(pos, &si);
      if (memcmp(&si, pos->st, StateSize))
        return 0;
    }

    if (step == Lists)
      for (int c = 0; c < 2; c++)
        for (int pt = PAWN; pt <= KING; pt++)
          if (piece_count(c, pt) != popcount(pieces_cp(c, pt)))
            return 0;

    if (step == Castling)
      for (int c = 0; c < 2; c++)
        for (int s = 0; s < 2; s++) {
          int cr = make_castling_right(c, s);
          if (!can_castle_cr(cr))
            continue;

          if (   piece_on(pos->castlingRookSquare[cr]) != make_piece(c, ROOK)
              || pos->castlingRightsMask[pos->castlingRookSquare[cr]] != cr
              || (pos->castlingRightsMask[square_of(c, KING)] & cr) != cr)
            return 0;
        }
  }

  return 1;
}
#endif

/* ==========================================
   FILE: movegen.c
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/



enum { CAPTURES, QUIETS, QUIET_CHECKS, EVASIONS, NON_EVASIONS, LEGAL };


INLINE ExtMove *make_promotions(ExtMove *list, Square to, Square ksq,
    const int Type, const int D)
{
  if (Type == CAPTURES || Type == EVASIONS || Type == NON_EVASIONS) {
    (list++)->move = make_promotion(to - D, to, QUEEN);
    if (attacks_from_knight(to) & sq_bb(ksq))
      (list++)->move = make_promotion(to - D, to, KNIGHT);
  }

  if (Type == QUIETS || Type == EVASIONS || Type == NON_EVASIONS) {
    (list++)->move = make_promotion(to - D, to, ROOK);
    (list++)->move = make_promotion(to - D, to, BISHOP);
    if (!(attacks_from_knight(to) & sq_bb(ksq)))
      (list++)->move = make_promotion(to - D, to, KNIGHT);
  }

  return list;
}


INLINE ExtMove *generate_pawn_moves(const Position *pos, ExtMove *list,
    Bitboard target, const Color Us, const int Type)
{
  // Compute our parametrized parameters at compile time, named according to
  // the point of view of white side.
  const Color    Them     = Us == WHITE ? BLACK      : WHITE;
  const Bitboard TRank8BB = Us == WHITE ? Rank8BB    : Rank1BB;
  const Bitboard TRank7BB = Us == WHITE ? Rank7BB    : Rank2BB;
  const Bitboard TRank3BB = Us == WHITE ? Rank3BB    : Rank6BB;
  const int      Up       = Us == WHITE ? NORTH      : SOUTH;
  const int      Right    = Us == WHITE ? NORTH_EAST : SOUTH_WEST;
  const int      Left     = Us == WHITE ? NORTH_WEST : SOUTH_EAST;

  const Bitboard emptySquares =  Type == QUIETS || Type == QUIET_CHECKS
                               ? target : ~pieces();
  const Bitboard enemies =  Type == EVASIONS ? checkers()
                          : Type == CAPTURES ? target : pieces_c(Them);

  Bitboard pawnsOn7    = pieces_cp(Us, PAWN) &  TRank7BB;
  Bitboard pawnsNotOn7 = pieces_cp(Us, PAWN) & ~TRank7BB;

  // Single and double pawn pushes, no promotions
  if (Type != CAPTURES) {
    Bitboard b1 = shift_bb(Up, pawnsNotOn7)   & emptySquares;
    Bitboard b2 = shift_bb(Up, b1 & TRank3BB) & emptySquares;

    if (Type == EVASIONS) { // Consider only blocking squares
      b1 &= target;
      b2 &= target;
    }

    if (Type == QUIET_CHECKS) {
      Stack *st = pos->st;

      // A quiet check is either a direct check or a discovered check.
      Bitboard dcCandidatePawns = blockers_for_king(pos, Them) & ~file_bb_s(st->ksq);
      b1 &= attacks_from_pawn(st->ksq, Them) | shift_bb(Up, dcCandidatePawns);
      b2 &= attacks_from_pawn(st->ksq, Them) | shift_bb(Up+Up, dcCandidatePawns);
    }

    while (b1) {
      Square to = pop_lsb(&b1);
      (list++)->move = make_move(to - Up, to);
    }

    while (b2) {
      Square to = pop_lsb(&b2);
      (list++)->move = make_move(to - Up - Up, to);
    }
  }

  // Promotions and underpromotions
  if (pawnsOn7 && (Type != EVASIONS || (target & TRank8BB))) {
    Bitboard b1 = shift_bb(Right, pawnsOn7) & enemies;
    Bitboard b2 = shift_bb(Left , pawnsOn7) & enemies;
    Bitboard b3 = shift_bb(Up   , pawnsOn7) & emptySquares;

    if (Type == EVASIONS)
      b3 &= target;

    while (b1)
      list = make_promotions(list, pop_lsb(&b1), pos->st->ksq, Type, Right);

    while (b2)
      list = make_promotions(list, pop_lsb(&b2), pos->st->ksq, Type, Left);

    while (b3)
      list = make_promotions(list, pop_lsb(&b3), pos->st->ksq, Type, Up);
  }

  // Standard and en-passant captures
  if (Type == CAPTURES || Type == EVASIONS || Type == NON_EVASIONS) {
    Bitboard b1 = shift_bb(Right, pawnsNotOn7) & enemies;
    Bitboard b2 = shift_bb(Left , pawnsNotOn7) & enemies;

    while (b1) {
      Square to = pop_lsb(&b1);
      (list++)->move = make_move(to - Right, to);
    }

    while (b2) {
      Square to = pop_lsb(&b2);
      (list++)->move = make_move(to - Left, to);
    }

    if (ep_square() != 0) {
      assert(rank_of(ep_square()) == relative_rank(Us, RANK_6));

      // An en passant capture can be an evasion only if the checking piece
      // is the double pushed pawn and so is in the target. Otherwise this
      // is a discovered check and we are forced to do otherwise.
      if (Type == EVASIONS && (target & sq_bb(ep_square() + Up)))
        return list;

      b1 = pawnsNotOn7 & attacks_from_pawn(ep_square(), Them);

      assert(b1);

      while (b1)
        (list++)->move = make_enpassant(pop_lsb(&b1), ep_square());
    }
  }

  return list;
}


INLINE ExtMove *generate_moves(const Position *pos, ExtMove *list,
    Bitboard target, const Color Us, const int Pt, const bool Checks)
{
  assert(Pt != KING && Pt != PAWN);

  Bitboard bb = pieces_cp(Us, Pt);

  while (bb) {
    Square from = pop_lsb(&bb);
    Bitboard b = attacks_bb(Pt, from, pieces()) & target;

    if (Checks && (Pt == QUEEN || !(blockers_for_king(pos, !Us) & sq_bb(from))))
      b &= pos->st->checkSquares[Pt];

    while (b)
      (list++)->move = make_move(from, pop_lsb(&b));
  }

  return list;
}


INLINE ExtMove *generate_all(const Position *pos, ExtMove *list, const Color Us,
  const int Type)
{
  const bool Checks = Type == QUIET_CHECKS;
  const Square ksq = square_of(Us, KING);
  Bitboard target;

  if (Type == EVASIONS && more_than_one(checkers()))
    goto kingMoves; // Double check, only a king move can save the day

  target =  Type == EVASIONS     ? between_bb(ksq, lsb(checkers()))
          : Type == NON_EVASIONS ? ~pieces_c(Us)
          : Type == CAPTURES     ? pieces_c(!Us) : ~pieces();

  list = generate_pawn_moves(pos, list, target, Us, Type);
  list = generate_moves(pos, list, target, Us, KNIGHT, Checks);
  list = generate_moves(pos, list, target, Us, BISHOP, Checks);
  list = generate_moves(pos, list, target, Us,   ROOK, Checks);
  list = generate_moves(pos, list, target, Us,  QUEEN, Checks);

kingMoves:
  if (!Checks || blockers_for_king(pos, !Us) & sq_bb(ksq)) {
    Bitboard b = attacks_from(KING, ksq) & (Type == EVASIONS ? ~pieces_c(Us) : target);
    if (Checks)
      b &= ~PseudoAttacks[QUEEN][square_of(!Us, KING)];

    while (b)
      (list++)->move = make_move(ksq, pop_lsb(&b));

    if ((Type == QUIETS || Type == NON_EVASIONS) && can_castle_c(Us)) {
      const int OO = make_castling_right(Us, KING_SIDE);
      if (!castling_impeded(OO) && can_castle_cr(OO))
        (list++)->move = make_castling(ksq, castling_rook_square(OO));

      const int OOO = make_castling_right(Us, QUEEN_SIDE);
      if (!castling_impeded(OOO) && can_castle_cr(OOO))
        (list++)->move = make_castling(ksq, castling_rook_square(OOO));
    }
  }

  return list;
}


// generate_captures() generates all pseudo-legal captures plus queen and
// checking knight promotions.
//
// generate_quiets() generates all pseudo-legal non-captures and
// underpromotions (except checking knight promotions).
//
// generate_evasions() generates all pseudo-legal check evasions
//
// generate_quiet_checks() generates all pseudo-legal non-captures giving
// check, except castling
//
// generate_non_evasions() generates all pseudo-legal captures and
// non-captures.

INLINE ExtMove *generate(const Position *pos, ExtMove *list, const int Type)
{
  assert(Type != LEGAL);
  assert((Type == EVASIONS) == (bool)checkers());

  Color us = stm();

  return us == WHITE ? generate_all(pos, list, WHITE, Type)
                     : generate_all(pos, list, BLACK, Type);
}

// "template" instantiations

NOINLINE ExtMove *generate_captures(const Position *pos, ExtMove *list)
{
  return generate(pos, list, CAPTURES);
}

NOINLINE ExtMove *generate_quiets(const Position *pos, ExtMove *list)
{
  return generate(pos, list, QUIETS);
}

NOINLINE ExtMove *generate_evasions(const Position *pos, ExtMove *list)
{
  return generate(pos, list, EVASIONS);
}

NOINLINE ExtMove *generate_quiet_checks(const Position *pos, ExtMove *list)
{
  return generate(pos, list, QUIET_CHECKS);
}

NOINLINE ExtMove *generate_non_evasions(const Position *pos, ExtMove *list)
{
  return generate(pos, list, NON_EVASIONS);
}


// generate_legal() generates all the legal moves in the given position
NOINLINE ExtMove *generate_legal(const Position *pos, ExtMove *list)
{
  Color us = stm();
  Bitboard pinned = blockers_for_king(pos, us) & pieces_c(us);
  Square ksq = square_of(us, KING);
  ExtMove *cur = list;

  list = checkers() ? generate_evasions(pos, list)
                    : generate_non_evasions(pos, list);
  while (cur != list)
    if (  (  (pinned && pinned & sq_bb(from_sq(cur->move)))
           || from_sq(cur->move) == ksq
           || type_of_m(cur->move) == ENPASSANT)
        && !is_legal(pos, cur->move))
      cur->move = (--list)->move;
    else
      ++cur;

  return list;
}

/* ==========================================
   FILE: movepick.c
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2018 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/



// An insertion sort which sorts moves in descending order up to and
// including a given limit. The order of moves smaller than the limit is
// left unspecified.

INLINE void partial_insertion_sort(ExtMove *begin, ExtMove *end, int limit)
{
  for (ExtMove *sortedEnd = begin, *p = begin + 1; p < end; p++)
    if (p->value >= limit) {
      ExtMove tmp = *p, *q;
      *p = *++sortedEnd;
      for (q = sortedEnd; q != begin && (q-1)->value < tmp.value; q--)
        *q = *(q-1);
      *q = tmp;
    }
}


// pick_best() finds the best move in the range (begin, end).

static Move pick_best(ExtMove *begin, ExtMove *end)
{
  ExtMove *p, *q;

  for (p = begin, q = begin + 1; q < end; q++)
    if (q->value > p->value)
      p = q;
  Move m = p->move;
  int v = p->value;
  *p = *begin;
  begin->value = v;

  return m;
}


// score() assigns a numerical value to each move in a move list. The moves with
// highest values will be picked first.

static void score_captures(const Position *pos)
{
  Stack *st = pos->st;
  CapturePieceToHistory *history = pos->captureHistory;

  // Winning and equal captures in the main search are ordered by MVV,
  // preferring captures near our with a good history.

  for (ExtMove *m = st->cur; m < st->endMoves; m++)
    m->value =  PieceValue[MG][piece_on(to_sq(m->move))] * 6
              + (*history)[moved_piece(m->move)][to_sq(m->move)][type_of_p(piece_on(to_sq(m->move)))];
}

SMALL
static void score_quiets(const Position *pos)
{
  Stack *st = pos->st;
  ButterflyHistory *history = pos->mainHistory;
  LowPlyHistory *lph = pos->lowPlyHistory;

  PieceToHistory *cmh = (st-1)->history;
  PieceToHistory *fmh = (st-2)->history;
  PieceToHistory *fmh2 = (st-4)->history;
  PieceToHistory *fmh3 = (st-6)->history;

  Color c = stm();

  for (ExtMove *m = st->cur; m < st->endMoves; m++) {
    uint32_t move = m->move & 4095;
    Square to = move & 63;
    Square from = move >> 6;
    m->value =      (*history)[c][move]
              + 2 * (*cmh)[piece_on(from)][to]
              +     (*fmh)[piece_on(from)][to]
              +     (*fmh2)[piece_on(from)][to]
              +     (*fmh3)[piece_on(from)][to]
              + (st->mp_ply < MAX_LPH ? min(4, st->depth / 3) * (*lph)[st->mp_ply][move] : 0);
  }
}

static void score_evasions(const Position *pos)
{
  Stack *st = pos->st;
  // Try captures ordered by MVV/LVA, then non-captures ordered by
  // stats heuristics.

  ButterflyHistory *history = pos->mainHistory;
  PieceToHistory *cmh = (st-1)->history;
  Color c = stm();

  for (ExtMove *m = st->cur; m < st->endMoves; m++)
    if (is_capture(pos, m->move))
      m->value =  PieceValue[MG][piece_on(to_sq(m->move))]
                - type_of_p(moved_piece(m->move));
    else
      m->value =      (*history)[c][from_to(m->move)]
                + 2 * (*cmh)[moved_piece(m->move)][to_sq(m->move)]
                - (1 << 28);
}


// next_move() returns the next pseudo-legal move to be searched.

Move next_move(const Position *pos, bool skipQuiets)
{
  Stack *st = pos->st;
  Move move;

  switch (st->stage) {

  case ST_MAIN_SEARCH: case ST_EVASION: case ST_QSEARCH: case ST_PROBCUT:
    st->endMoves = (st-1)->endMoves;
    st->stage++;
    return st->ttMove;

  case ST_CAPTURES_INIT:
    st->endBadCaptures = st->cur = (st-1)->endMoves;
    st->endMoves = generate_captures(pos, st->cur);
    score_captures(pos);
    st->stage++;
    /* fallthrough */

  case ST_GOOD_CAPTURES:
    while (st->cur < st->endMoves) {
      move = pick_best(st->cur++, st->endMoves);
      if (move != st->ttMove) {
        if (see_test(pos, move, -69 * (st->cur-1)->value / 1024))
          return move;

        // Losing capture, move it to the beginning of the array.
        (st->endBadCaptures++)->move = move;
      }
    }
    st->stage++;

    // First killer move.
    move = st->mpKillers[0];
    if (move && move != st->ttMove && is_pseudo_legal(pos, move)
             && !is_capture(pos, move))
      return move;
    /* fallthrough */

  case ST_KILLERS:
    st->stage++;
    move = st->mpKillers[1]; // Second killer move.
    if (move && move != st->ttMove && is_pseudo_legal(pos, move)
             && !is_capture(pos, move))
      return move;
    /* fallthrough */

  case ST_KILLERS_2:
    st->stage++;
    move = st->countermove;
    if (move && move != st->ttMove && move != st->mpKillers[0]
             && move != st->mpKillers[1] && is_pseudo_legal(pos, move)
             && !is_capture(pos, move))
      return move;
    /* fallthrough */

  case ST_QUIET_INIT:
    if (!skipQuiets) {
      st->cur = st->endBadCaptures;
      st->endMoves = generate_quiets(pos, st->cur);
      score_quiets(pos);
      partial_insertion_sort(st->cur, st->endMoves, -3000 * st->depth);
    }
    st->stage++;
    /* fallthrough */

  case ST_QUIET:
    if (!skipQuiets)
      while (st->cur < st->endMoves) {
        move = (st->cur++)->move;
        if (   move != st->ttMove && move != st->mpKillers[0]
            && move != st->mpKillers[1] && move != st->countermove)
          return move;
      }
    st->stage++;
    st->cur = (st-1)->endMoves; // Return to bad captures.
    /* fallthrough */

  case ST_BAD_CAPTURES:
    if (st->cur < st->endBadCaptures)
      return (st->cur++)->move;
    break;

  case ST_EVASIONS_INIT:
    st->cur = (st-1)->endMoves;
    st->endMoves = generate_evasions(pos, st->cur);
    score_evasions(pos);
    st->stage++;

  case ST_ALL_EVASIONS:
    while (st->cur < st->endMoves) {
      move = pick_best(st->cur++, st->endMoves);
      if (move != st->ttMove)
        return move;
    }
    break;

  case ST_QCAPTURES_INIT:
    st->cur = (st-1)->endMoves;
    st->endMoves = generate_captures(pos, st->cur);
    score_captures(pos);
    st->stage++;

  case ST_QCAPTURES:
    while (st->cur < st->endMoves) {
      move = pick_best(st->cur++, st->endMoves);
      if (move != st->ttMove && (st->depth > DEPTH_QS_RECAPTURES
              || to_sq(move) == st->recaptureSquare))
        return move;
    }
    if (st->depth <= DEPTH_QS_NO_CHECKS)
      break;
    st->cur = (st-1)->endMoves;
    st->endMoves = generate_quiet_checks(pos, st->cur);
    st->stage++;
    /* fallthrough */

  case ST_QCHECKS:
    while (st->cur < st->endMoves) {
      move = (st->cur++)->move;
      if (move != st->ttMove)
        return move;
    }
    break;

  case ST_PROBCUT_INIT:
    st->cur = (st-1)->endMoves;
    st->endMoves = generate_captures(pos, st->cur);
    score_captures(pos);
    st->stage++;
    /* fallthrough */

  case ST_PROBCUT_2:
    while (st->cur < st->endMoves) {
      move = pick_best(st->cur++, st->endMoves);
      if (move != st->ttMove && see_test(pos, move, st->threshold))
        return move;
    }
    break;

  default:
    assume(false);

  }

  return 0;
}

/* ==========================================
   FILE: pawns.c
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2018 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef NNUE_PURE



#define V(v) ((Value)(v))
#define S(mg, eg) make_score(mg, eg)

// Pawn penalties
static const Score Backward      = S( 9, 22);
static const Score Doubled       = S(13, 51);
static const Score DoubledEarly  = S(20,  7);
static const Score Isolated      = S( 3, 15);
static const Score WeakLever     = S( 4, 58);
static const Score WeakUnopposed = S(13, 24);

// Bonus for blocked pawns at 5th or 6th rank
static const Score BlockedPawn[2] = { S(-17, -6), S(-9, 2) };

static const Score BlockedStorm[8] = {
  S(0, 0), S(0, 0), S(75, 78), S(-8, 16), S(-6, 10), S(-6, 6), S(0,2)
};

// Connected pawn bonus
static const int Connected[8] = { 0, 5, 7, 11, 23, 48, 87 };

#undef V
#define V(mg) S(mg,0)
// Strength of pawn shelter for our king by [distance from edge][rank].
// RANK_1 = 0 is used for files where we have no pawn, or pawn is behind
// our king.
static const Score ShelterStrength[4][8] = {
  { V( -5), V( 82), V( 92), V( 54), V( 36), V( 22), V(  28) },
  { V(-44), V( 63), V( 33), V(-50), V(-30), V(-12), V( -62) },
  { V(-11), V( 77), V( 22), V( -6), V( 31), V(  8), V( -45) },
  { V(-39), V(-12), V(-29), V(-50), V(-43), V(-68), V(-164) }
};

// Danger of enemry pawns moving toward our king by [distance from edge][rank].
// RANK_1 = 0 is used for files where the enemy has no pawn or where their
// pawn is behind our king. Note that UnblockedStorm[0][1-2] accommodates
// opponent pawn on edge, likely blocked by our king.
static const Score UnblockedStorm[4][8] = {
  { V( 87), V(-288), V(-168), V( 96), V( 47), V( 44), V( 46) },
  { V( 42), V( -25), V( 120), V( 45), V( 34), V( -9), V( 24) },
  { V( -8), V(  51), V( 167), V( 35), V( -4), V(-16), V(-12) },
  { V(-17), V( -13), V( 100), V(  4), V(  9), V(-16), V(-31) }
};

// KingOnFile[semi-open Us][semi-open Them] contains bonuses/penalties
// for king when the king is on a semi-open or open file.
static const Score KingOnFile[2][2] = {
  { S(-21,10), S(-7, 1) }, { S(0, -3), S(9, -4) }
};

#undef S
#undef V

INLINE Score pawn_evaluate(const Position *pos, PawnEntry *e, const Color Us)
{
  const Color Them  = Us == WHITE ? BLACK : WHITE;
  const int   Up    = Us == WHITE ? NORTH : SOUTH;
  const int   Down  = Us == WHITE ? SOUTH : NORTH;

  Bitboard neighbours, stoppers, doubled, support, phalanx, opposed;
  Bitboard lever, leverPush, blocked;
  Square s;
  bool backward, passed;
  Score score = SCORE_ZERO;

  Bitboard ourPawns   = pieces_cp(Us, PAWN);
  Bitboard theirPawns = pieces_p(PAWN) ^ ourPawns;

  Bitboard doubleAttackThem = pawn_double_attacks_bb(theirPawns, Them);

  e->passedPawns[Us] = 0;
  e->semiopenFiles[Us] = 0xFF;
  e->kingSquares[Us] = SQ_NONE;
  e->pawnAttacks[Us] = e->pawnAttacksSpan[Us] = pawn_attacks_bb(ourPawns, Us);
  e->pawnsOnSquares[Us][BLACK] = popcount(ourPawns & DarkSquares);
  e->pawnsOnSquares[Us][WHITE] = popcount(ourPawns & LightSquares);
  e->blockedCount += popcount(  shift_bb(Up, ourPawns)
                              & (theirPawns | doubleAttackThem));

  // Loop through all pawns of the current color and score each pawn
  loop_through_pieces(Us, PAWN, s) {
    assert(piece_on(s) == make_piece(Us, PAWN));

    int f = file_of(s);
    int r = relative_rank_s(Us, s);
    e->semiopenFiles[Us] &= ~(1 << f);

    // Flag the pawn
    opposed    = theirPawns & forward_file_bb(Us, s);
    blocked    = theirPawns & sq_bb(s + Up);
    stoppers   = theirPawns & passed_pawn_span(Us, s);
    lever      = theirPawns & PawnAttacks[Us][s];
    leverPush  = theirPawns & PawnAttacks[Us][s + Up];
    doubled    = ourPawns   & sq_bb(s - Up);
    neighbours = ourPawns   & adjacent_files_bb(f);
    phalanx    = neighbours & rank_bb_s(s);
    support    = neighbours & rank_bb_s(s - Up);

    if (doubled) {
      // Additional doubled penalty if none of their pawns is fixed
      if (!(ourPawns & shift_bb(Down, theirPawns | pawn_attacks_bb(theirPawns, Them))))
        score -= DoubledEarly;
    }

    // A pawn is backward when it is behind all pawns of the same color on
    // the adjacent files and cannot safely advance.
    backward =  !(neighbours & forward_ranks_bb(Them, rank_of(s + Up)))
              && (leverPush | blocked);

    // Compute additional span if pawn is neither backward nor blocked
    if (!backward && !blocked)
      e->pawnAttacksSpan[Us] |= pawn_attack_span(Us, s);

    // A pawn is passed if one of the three following conditions is true:
    // (a) there are no stoppers except some levers
    // (b) the only stoppers are the leverPush, but we outnumber them
    // (c) there is only one front stopper which can be levered
    //     (Refined in evaluation_passed())
    passed =  !(stoppers ^ lever)
            || (   !(stoppers ^ leverPush)
                && popcount(phalanx) >= popcount(leverPush))
            || (   stoppers == blocked && r >= RANK_5
                && (shift_bb(Up, support) & ~(theirPawns | doubleAttackThem)));

    passed &= !(forward_file_bb(Us, s) & ourPawns);

    // Passed pawns will be properly scored later in evaluation when we have
    // full attack info.
    if (passed)
      e->passedPawns[Us] |= sq_bb(s);

    // Score this pawn
    if (support | phalanx) {
      int v =  Connected[r] * (2 + !!phalanx - !!opposed)
             + 22 * popcount(support);
      score += make_score(v, v * (r - 2) / 4);
    }

    else if (!neighbours) {
      if (    opposed
          && (ourPawns & forward_file_bb(Them, s))
          && !(theirPawns & adjacent_files_bb(f)))
        score -= Doubled;
      else
        score -= Isolated + (!opposed ? WeakUnopposed : 0);
    }

    else if (backward)
      score -= Backward + (!opposed && ((s+1) & 0x06) ? WeakUnopposed : 0);

    if (!support)
      score -=  (doubled ? Doubled : 0)
              + (more_than_one(lever) ? WeakLever : 0);

    if (blocked && r >= RANK_5)
      score += BlockedPawn[r - RANK_5];
  }

  return score;
}


// pawns_probe() looks up the current position's pawns configuration in
// the pawns hash table.

void pawn_entry_fill(const Position *pos, PawnEntry *e, Key key)
{
  e->key = key;
  e->blockedCount = 0;
  e->score = pawn_evaluate(pos, e, WHITE) - pawn_evaluate(pos, e, BLACK);
  e->openFiles = popcount(e->semiopenFiles[WHITE] & e->semiopenFiles[BLACK]);
  e->passedCount = popcount(e->passedPawns[WHITE] | e->passedPawns[BLACK]);
}


// evaluate_shelter() calculates the shelter bonus and the storm penalty
// for a king, by looking at the king file and the two closest files.

INLINE Score evaluate_shelter(const PawnEntry *pe, const Position *pos,
    Square ksq, const Color Us)
{
  const Color Them = Us == WHITE ? BLACK : WHITE;
  
  Bitboard b =  pieces_p(PAWN) & ~forward_ranks_bb(Them, rank_of(ksq));
  Bitboard ourPawns = b & pieces_c(Us) & ~pe->pawnAttacks[Them];
  Bitboard theirPawns = b & pieces_c(Them);
  Score bonus = make_score(5, 5);

  File center = clamp(file_of(ksq), FILE_B, FILE_G);

  for (File f = center - 1; f <= center + 1; f++) {
    b = ourPawns & file_bb(f);
    int ourRank = b ? relative_rank_s(Us, backmost_sq(Us, b)) : 0;

    b = theirPawns & file_bb(f);
    int theirRank = b ? relative_rank_s(Us, frontmost_sq(Them, b)) : 0;

    int d = min(f, FILE_H - f);
    bonus += ShelterStrength[d][ourRank];

    if (ourRank && (ourRank == theirRank - 1)) {
      bonus -= BlockedStorm[theirRank];
    } else
      bonus -= UnblockedStorm[d][theirRank];
  }

  bonus -= KingOnFile[is_on_semiopen_file(pe, Us, ksq)][is_on_semiopen_file(pe, Them, ksq)];

  return bonus;
}


// do_king_safety() calculates a bonus for king safety. It is called only
// when king square changes, which is about 20% of total king_safety() calls.

INLINE Score do_king_safety(PawnEntry *pe, const Position *pos, Square ksq,
    const Color Us)
{
  pe->kingSquares[Us] = ksq;
  pe->castlingRights[Us] = can_castle_c(Us);

  int minPawnDist;

  Bitboard pawns = pieces_cp(Us, PAWN);
  if (!pawns)
    minPawnDist = 6;
  else if (pawns & PseudoAttacks[KING][ksq])
    minPawnDist = 1;
  else for (minPawnDist = 1;
            minPawnDist < 6 && !(DistanceRingBB[ksq][minPawnDist] & pawns);
            minPawnDist++);

  Score shelter = evaluate_shelter(pe, pos, ksq, Us);

  // If we can castle use the bonus after the castling if it is bigger
  if (can_castle_cr(make_castling_right(Us, KING_SIDE))) {
    Score s = evaluate_shelter(pe, pos, relative_square(Us, SQ_G1), Us);
    if (mg_value(s) > mg_value(shelter))
      shelter = s;
  }

  if (can_castle_cr(make_castling_right(Us, QUEEN_SIDE))) {
    Score s = evaluate_shelter(pe, pos, relative_square(Us, SQ_C1), Us);
    if (mg_value(s) > mg_value(shelter))
      shelter = s;
  }

  return shelter - make_score(0, 16 * minPawnDist);
}

// "template" instantiation:
NOINLINE Score do_king_safety_white(PawnEntry *pe, const Position *pos,
    Square ksq)
{
  return do_king_safety(pe, pos, ksq, WHITE);
}

NOINLINE Score do_king_safety_black(PawnEntry *pe, const Position *pos,
    Square ksq)
{
  return do_king_safety(pe, pos, ksq, BLACK);
}

#else

typedef int make_iso_compilers_happy;

#endif

/* ==========================================
   FILE: material.c
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2018 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef NNUE_PURE



// Polynomial material imbalance parameters.
#define S(mg,eg) make_score(mg,eg)

static const Score QuadraticOurs[][8] = {
  { S(1419, 1455) },
  { S( 101,   28), S( 37,  39) },
  { S(  57,   64), S(249, 187), S(-49, -62) },
  { S(   0,    0), S(118, 137), S( 10,  27), S(  0,   0) },
  { S( -63,  -68), S( -5,   3), S(100,  81), S(132, 118), S(-246, -244) },
  { S(-210, -211), S( 37,  14), S(147, 141), S(161, 105), S(-158, -174), S(-9, -31) }
};

static const Score QuadraticTheirs[][8] = {
  { 0 },
  { S(  33,  30) },
  { S(  46,  18), S(106,  84) },
  { S(  75,  35), S( 59,  44), S( 60,  15) },
  { S(  26,  35), S(  6,  22), S( 38,  39), S(-12,  -2) },
  { S(  97,  93), S(100, 163), S(-58, -91), S(112, 192), S(276, 225) }
};

#undef S

// Helper used to detect a given material distribution.
INLINE bool is_KXK(const Position *pos, int us)
{
  return  !more_than_one(pieces_c(!us))
        && non_pawn_material_c(us) >= RookValueMg;
}

INLINE bool is_KBPsK(const Position *pos, int us)
{
  return   non_pawn_material_c(us) == BishopValueMg
        && pieces_cp(us, PAWN);
}

INLINE bool is_KQKRPs(const Position *pos, int us) {
  return  !piece_count(us, PAWN)
        && non_pawn_material_c(us) == QueenValueMg
        && piece_count(!us, ROOK) == 1
        && pieces_cp(!us, PAWN);
}

// imbalance() calculates the imbalance by comparing the piece count of each
// piece type for both colors.
static Score imbalance(int us, int pieceCount[][8])
{
  int *pc_us = pieceCount[us];
  int *pc_them = pieceCount[!us];
  Score bonus = SCORE_ZERO;

  // Second-degree polynomial material imbalance, by Tord Romstad
  for (int pt1 = 0; pt1 <= QUEEN; pt1++) {
    if (!pc_us[pt1])
      continue;

    int v = 0;

    for (int pt2 = 0; pt2 <= pt1; pt2++)
      v +=  QuadraticOurs[pt1][pt2] * pc_us[pt2]
          + QuadraticTheirs[pt1][pt2] * pc_them[pt2];

    bonus += pc_us[pt1] * v;
  }

  return bonus;
}

typedef int PieceCountType[2][8];

// material_probe() looks up the current position's material configuration
// in the material hash table. It returns a pointer to the MaterialEntry
// if the position is found. Otherwise a new Entry is computed and stored
// there, so we don't have to recompute all when the same material
// configuration occurs again.

void material_entry_fill(const Position *pos, MaterialEntry *e, Key key)
{
  memset(e, 0, sizeof(MaterialEntry));
  e->key = key;
  e->factor[WHITE] = e->factor[BLACK] = (uint8_t)SCALE_FACTOR_NORMAL;

  Value npm_w = non_pawn_material_c(WHITE);
  Value npm_b = non_pawn_material_c(BLACK);
  Value npm = clamp(npm_w + npm_b, EndgameLimit, MidgameLimit);
  e->gamePhase = ((npm - EndgameLimit) * PHASE_MIDGAME) / (MidgameLimit - EndgameLimit);

  // Look for a specialized evaluation function.
  for (int i = 0; i < NUM_EVAL; i++)
    for (int c = 0; c < 2; c++)
      if (endgame_keys[i][c] == key) {
        e->eval_func = 1 + i;
        e->eval_func_side = c;
        return;
      }

  for (int c = 0; c < 2; c++)
    if (is_KXK(pos, c)) {
      e->eval_func = 10; // EvaluateKXK
      e->eval_func_side = c;
      return;
    }

  // Look for a specialized scaling function.
  for (int i = 0; i < NUM_SCALING; i++)
    for (int c = 0; c < 2; c++)
      if (endgame_keys[NUM_EVAL + i][c] == key) {
        e->scal_func[c] = 11 + i;
        return;
      }

  // We did not find any specialized scaling function, so fall back on
  // generic ones that refer to more than one material distribution. Note
  // that in this case we do not return after setting the function.
  for (int c = 0; c < 2; c++) {
    if (is_KBPsK(pos, c))
      e->scal_func[c] = 17; // ScaleKBPsK

    else if (is_KQKRPs(pos, c))
      e->scal_func[c] = 18; // ScaleKQKRPs
  }

  if (npm_w + npm_b == 0 && pieces_p(PAWN)) { // Only pawns on the board.
    if (!pieces_cp(BLACK, PAWN)) {
      assert(piece_count(WHITE, PAWN) >= 2);

      e->scal_func[WHITE] = 19; // ScaleKPsK
    }
    else if (!pieces_cp(WHITE, PAWN)) {
      assert(piece_count(BLACK, PAWN) >= 2);

      e->scal_func[BLACK] = 19; // ScaleKPsK
    }
    else if (popcount(pieces_p(PAWN)) == 2) { // Each side has one pawn.
      // This is a special case because we set scaling functions
      // for both colors instead of only one.
      e->scal_func[WHITE] = 20; // ScaleKPKP
      e->scal_func[BLACK] = 20; // ScaleKPKP
    }
  }

  // Zero or just one pawn makes it difficult to win, even with a small
  // material advantage. This catches some trivial draws like KK, KBK and
  // KNK and gives a drawish scale factor for cases such as KRKBP and
  // KmmKm (except for KBBKN).
  if (!piece_count(WHITE, PAWN) && npm_w - npm_b <= BishopValueMg)
    e->factor[WHITE] = (uint8_t)(npm_w <  RookValueMg   ? SCALE_FACTOR_DRAW :
                                 npm_b <= BishopValueMg ? 4 : 14);

  if (!piece_count(BLACK, PAWN) && npm_b - npm_w <= BishopValueMg)
    e->factor[BLACK] = (uint8_t)(npm_b <  RookValueMg   ? SCALE_FACTOR_DRAW :
                                 npm_w <= BishopValueMg ? 4 : 14);

  // Evaluate the material imbalance. We use PIECE_TYPE_NONE as a place
  // holder for the bishop pair "extended piece", which allows us to be
  // more flexible in defining bishop pair bonuses.
#define pc(c,p) piece_count_mk(c,p)
  int PieceCount[2][8] = {
    { pc(0, BISHOP) > 1, pc(0, PAWN), pc(0, KNIGHT),
      pc(0, BISHOP)    , pc(0, ROOK), pc(0, QUEEN) },
    { pc(1, BISHOP) > 1, pc(1, PAWN), pc(1, KNIGHT),
      pc(1, BISHOP)    , pc(1, ROOK), pc(1, QUEEN) }
  };
#undef pc
  Score tmp = imbalance(WHITE, PieceCount) - imbalance(BLACK, PieceCount);
  e->score = make_score(mg_value(tmp) / 16, eg_value(tmp) / 16);
}

#else

typedef int make_iso_compilers_happy;

#endif

/* ==========================================
   FILE: endgame.c
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef NNUE_PURE



// Table used to drive the king towards the edge of the board
// in KX vs K and KQ vs KR endgames.
static int PushToEdges[64];

// Table used to drive the king towards a corner square of the
// right color in KBN vs K endgames.
static int PushToCorners[64];

// Tables used to drive a piece towards or away from another piece
static const int PushClose[8] = { 140, 120, 100, 80, 60, 40, 20, 0 };
static const int PushAway [8] = { -20, 0, 20, 40, 60, 80, 100, 120 };

#ifndef NDEBUG
static bool verify_material(const Position *pos, int c, Value npm, int pawnsCnt)
{
  return   non_pawn_material_c(c) == npm
        && piece_count(c, PAWN) == pawnsCnt;
}
#endif

// Map the square as if strongSide is white and strongSide's only pawn
// is on the left half of the board.
static Square normalize(const Position *pos, Color strongSide, Square sq)
{
  assert(piece_count(strongSide, PAWN) == 1);

  if (file_of(square_of(strongSide, PAWN)) >= FILE_E)
    sq ^= 0x07;

  if (strongSide == BLACK)
    sq ^= 0x38;

  return sq;
}


// Compute material key from an endgame code string.

static Key calc_key(const char *code, Color c)
{
  Key key = 0;
  int color = c << 3;

  for (; *code; code++)
    for (int i = 1;; i++)
      if (*code == PieceToChar[i]) {
        key += matKey[i ^ color];
        break;
      }

  return key;
}

static EgFunc EvaluateKPK, EvaluateKNNK, EvaluateKNNKP, EvaluateKBNK,
              EvaluateKRKP, EvaluateKRKB, EvaluateKRKN, EvaluateKQKP,
              EvaluateKQKR, EvaluateKXK;

static EgFunc ScaleKRPKR, ScaleKRPKB, ScaleKBPKB, ScaleKBPKN, ScaleKBPPKB,
              ScaleKRPPKRP, ScaleKBPsK, ScaleKQKRPs, ScaleKPKP, ScaleKPsK;

EgFunc *endgame_funcs[NUM_EVAL + NUM_SCALING + 6] = {
  NULL,
// Entries 1-10 are evaluation functions.
  &EvaluateKPK,    // 1
  &EvaluateKNNK,   // 2
  &EvaluateKNNKP,  // 3
  &EvaluateKBNK,   // 4
  &EvaluateKRKP,   // 5
  &EvaluateKRKB,   // 6
  &EvaluateKRKN,   // 7
  &EvaluateKQKP,   // 8
  &EvaluateKQKR,   // 9
  &EvaluateKXK,    // 10
// Entries 11-20 are scaling functions.
  &ScaleKRPKR,     // 11
  &ScaleKRPKB,     // 12
  &ScaleKBPKB,     // 13
  &ScaleKBPKN,     // 14
  &ScaleKBPPKB,    // 15
  &ScaleKRPPKRP,   // 16
  &ScaleKBPsK,     // 17
  &ScaleKQKRPs,    // 18
  &ScaleKPsK,      // 19
  &ScaleKPKP       // 20
};

Key endgame_keys[NUM_EVAL + NUM_SCALING][2];

static const char *endgame_codes[NUM_EVAL + NUM_SCALING] = {
  // Codes for evaluation functions 1-9.
  "KPk", "KNNk", "KNNkp", "KBNk", "KRkp", "KRkb", "KRkn", "KQkp", "KQkr",
  // Codes for scaling functions 11-17.
  "KRPkr", "KRPkb", "KBPkb", "KBPkn", "KBPPkb", "KRPPkrp"
};

void endgames_init(void)
{
  for (int i = 0; i < NUM_EVAL + NUM_SCALING; i++) {
    endgame_keys[i][WHITE] = calc_key(endgame_codes[i], WHITE);
    endgame_keys[i][BLACK] = calc_key(endgame_codes[i], BLACK);
  }

  for (int s = 0; s < 64; s++) {
    int f = file_of(s), r = rank_of(s);
    int fd = min(f, 7 - f), rd = min(r, 7 - r);
    PushToEdges[s] = 90 - (7 * fd * fd / 2 + 7 * rd * rd / 2);
    PushToCorners[s] = 420 * abs(7 - r - f);
  }
}


// Mate with KX vs K. This function is used to evaluate positions with
// king and plenty of material vs a lone king. It simply gives the
// attacking side a bonus for driving the defending king towards the edge
// of the board, and for keeping the distance between the two kings small.
static Value EvaluateKXK(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, weakSide, VALUE_ZERO, 0));
  assert(!checkers()); // Eval is never called when in check

  // Stalemate detection with lone king
  if (stm() == weakSide) {
    ExtMove list[MAX_MOVES];
    if (generate_legal(pos, list) == list)
      return VALUE_DRAW;
  }

  Square winnerKSq = square_of(strongSide, KING);
  Square loserKSq = square_of(weakSide, KING);

  Value result =  non_pawn_material_c(strongSide)
                + piece_count(strongSide, PAWN) * PawnValueEg
                + PushToEdges[loserKSq]
                + PushClose[distance(winnerKSq, loserKSq)];

  if (    pieces_pp(QUEEN, ROOK)
      || (pieces_p(BISHOP) && pieces_p(KNIGHT))
      || (   (pieces_p(BISHOP) & DarkSquares)
          && (pieces_p(BISHOP) & LightSquares)))
    result = min(result + VALUE_KNOWN_WIN, VALUE_TB_WIN_IN_MAX_PLY - 1);

  return strongSide == stm() ? result : -result;
}


// Mate with KBN vs K. This is similar to KX vs K, but we have to drive the
// defending king towards a corner square of the right color.
static Value EvaluateKBNK(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, KnightValueMg + BishopValueMg, 0));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 0));

  Square winnerKSq = square_of(strongSide, KING);
  Square loserKSq = square_of(weakSide, KING);
  Square bishopSq = lsb(pieces_p(BISHOP));

  // If our bishop does not attack A1/H8, we flip the enemy king square
  // to drive to opposite corners (A8/H1)
  if (opposite_colors(bishopSq, SQ_A1)) {
    winnerKSq = winnerKSq ^ 0x38;
    loserKSq  = loserKSq ^ 0x38;
  }

  Value result =  VALUE_KNOWN_WIN + 3520
                + PushClose[distance(winnerKSq, loserKSq)]
                + PushToCorners[loserKSq];

  return strongSide == stm() ? result : -result;
}


// KP vs K. This endgame is evaluated with the help of a bitbase.
static Value EvaluateKPK(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, VALUE_ZERO, 1));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 0));

  // Assume strongSide is white and the pawn is on files A-D
  Square wksq = normalize(pos, strongSide, square_of(strongSide, KING));
  Square bksq = normalize(pos, strongSide, square_of(weakSide, KING));
  Square psq  = normalize(pos, strongSide, lsb(pieces_p(PAWN)));

  Color us = strongSide == stm() ? WHITE : BLACK;

  if (!bitbases_probe(wksq, psq, bksq, us))
    return VALUE_DRAW;

  Value result = VALUE_KNOWN_WIN + PawnValueEg + (Value)(rank_of(psq));

  return strongSide == stm() ? result : -result;
}


// KR vs KP. This is a somewhat tricky endgame to evaluate precisely without
// a bitbase. The function below returns drawish scores when the pawn is
// far advanced with support of the king, while the attacking king is far
// away.
static Value EvaluateKRKP(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, RookValueMg, 0));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 1));

  Square wksq = relative_square(strongSide, square_of(strongSide, KING));
  Square bksq = relative_square(strongSide, square_of(weakSide, KING));
  Square rsq  = relative_square(strongSide, lsb(pieces_p(ROOK)));
  Square psq  = relative_square(strongSide, lsb(pieces_p(PAWN)));

  Square queeningSq = make_square(file_of(psq), RANK_1);
  Value result;

  // If the stronger side's king is in front of the pawn, it is a win.
  if (forward_file_bb(WHITE, wksq) & sq_bb(psq))
    result = RookValueEg - distance(wksq, psq);

  // If the weaker side's king is too far from the pawn and the rook,
  // it is a win.
  else if (   distance(bksq, psq) >= 3 + (stm() == weakSide)
           && distance(bksq, rsq) >= 3)
    result = RookValueEg - distance(wksq, psq);

  // If the pawn is far advanced and supported by the defending king,
  // the position is drawish.
  else if (   rank_of(bksq) <= RANK_3
           && distance(bksq, psq) == 1
           && rank_of(wksq) >= RANK_4
           && distance(wksq, psq) > 2 + (stm() == strongSide))
    result = (Value)(80) - 8 * distance(wksq, psq);

  else
    result =  (Value)(200) - 8 * (  distance(wksq, psq + SOUTH)
                                  - distance(bksq, psq + SOUTH)
                                  - distance(psq, queeningSq));

  return strongSide == stm() ? result : -result;
}


// KR vs KB. This is very simple, and always returns drawish scores.  The
// score is slightly bigger when the defending king is close to the edge.
static Value EvaluateKRKB(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, RookValueMg, 0));
  assert(verify_material(pos, weakSide, BishopValueMg, 0));

  Value result = (Value)PushToEdges[square_of(weakSide, KING)];
  return strongSide == stm() ? result : -result;
}


// KR vs KN. The attacking side has slightly better winning chances than
// in KR vs KB, particularly if the king and the knight are far apart.
static Value EvaluateKRKN(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, RookValueMg, 0));
  assert(verify_material(pos, weakSide, KnightValueMg, 0));

  Square bksq = square_of(weakSide, KING);
  Square bnsq = lsb(pieces_p(KNIGHT));
  Value result = (Value)PushToEdges[bksq] + PushAway[distance(bksq, bnsq)];
  return strongSide == stm() ? result : -result;
}


// KQ vs KP. In general, this is a win for the stronger side, but there are a
// few important exceptions. A pawn on 7th rank and on the A,C,F or H files
// with a king positioned next to it can be a draw, so in that case, we only
// use the distance between the kings.
static Value EvaluateKQKP(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, QueenValueMg, 0));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 1));

  Square winnerKSq = square_of(strongSide, KING);
  Square loserKSq = square_of(weakSide, KING);
  Square pawnSq = lsb(pieces_p(PAWN));

  Value result = (Value)PushClose[distance(winnerKSq, loserKSq)];

  if (   relative_rank_s(weakSide, pawnSq) != RANK_7
      || distance(loserKSq, pawnSq) != 1
      || ((FileBBB | FileDBB | FileEBB | FileGBB) & sq_bb(pawnSq)))
    result += QueenValueEg - PawnValueEg;

  return strongSide == stm() ? result : -result;
}


// KQ vs KR.  This is almost identical to KX vs K:  We give the attacking
// king a bonus for having the kings close together, and for forcing the
// defending king towards the edge. If we also take care to avoid null
// move for the defending side in the search, this is usually sufficient
// to win KQ vs KR.
static Value EvaluateKQKR(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, QueenValueMg, 0));
  assert(verify_material(pos, weakSide, RookValueMg, 0));

  Square winnerKSq = square_of(strongSide, KING);
  Square loserKSq = square_of(weakSide, KING);

  Value result =  QueenValueEg
                - RookValueEg
                + PushToEdges[loserKSq]
                + PushClose[distance(winnerKSq, loserKSq)];

  return strongSide == stm() ? result : -result;
}


// KNN vs KP. Simply push the opposing king to the corner.
static Value EvaluateKNNKP(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, 2 * KnightValueMg, 0));
  assert(verify_material(pos, weakSide, VALUE_ZERO, 1));

  Value result =      PawnValueEg
                +  2 * PushToEdges[square_of(weakSide, KING)]
                - 10 * relative_rank_s(weakSide, square_of(weakSide, PAWN));

  return strongSide == stm() ? result : -result;
}


// Some cases of trivial draws.
Value EvaluateKNNK(const Position *pos, Color strongSide)
{
  // Avoid compiler warnings about unused variables.
  (void)pos, (void)strongSide;

  return VALUE_DRAW;
}


// KB and one or more pawns vs K. It checks for draws with rook pawns
// and a bishop of the wrong color. If such a draw is detected,
// SCALE_FACTOR_DRAW is returned. If not, the return value is
// SCALE_FACTOR_NONE, i.e. no scaling will be used.
int ScaleKBPsK(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(non_pawn_material_c(strongSide) == BishopValueMg);
  assert(pieces_cp(strongSide, PAWN));

  // No assertions about the material of weakSide, because we want draws to
  // be detected even when the weaker side has some pawns.

  Bitboard pawns = pieces_cp(strongSide, PAWN);
  File pawnsFile = file_of(lsb(pawns));

  // All pawns are on a single rook file?
  if (    (pawnsFile == FILE_A || pawnsFile == FILE_H)
      && !(pawns & ~file_bb(pawnsFile))) {

    Square bishopSq = square_of(strongSide, BISHOP);
    Square queeningSq = relative_square(strongSide, make_square(pawnsFile, RANK_8));
    Square kingSq = square_of(weakSide, KING);

    if (   opposite_colors(queeningSq, bishopSq)
        && distance(queeningSq, kingSq) <= 1)
      return SCALE_FACTOR_DRAW;
  }

  // If all the pawns are on the same B or G file, then it's potentially a draw
  if (    (pawnsFile == FILE_B || pawnsFile == FILE_G)
      && !(pieces_p(PAWN) & ~file_bb(pawnsFile))
      && non_pawn_material_c(weakSide) == 0
      && piece_count(weakSide, PAWN))
  {
    // Get weakSide pawn that is closest to the home rank
    Square weakPawnSq = backmost_sq(weakSide, pieces_cp(weakSide, PAWN));

    Square strongKingSq = square_of(strongSide, KING);
    Square weakKingSq = square_of(weakSide, KING);
    Square bishopSq = square_of(strongSide, BISHOP);

    // There is potential for a draw if our pawn is blocked on the 7th rank,
    // the bishop cannot attack it or they only have one pawn left.
    if (   relative_rank_s(strongSide, weakPawnSq) == RANK_7
        && (pieces_cp(strongSide, PAWN) & sq_bb(weakPawnSq + pawn_push(weakSide)))
        && (opposite_colors(bishopSq, weakPawnSq) || piece_count(strongSide, PAWN) == 1)) {

      unsigned strongKingDist = distance(weakPawnSq, strongKingSq);
      unsigned weakKingDist = distance(weakPawnSq, weakKingSq);

      // It is a draw if the weak king is on its back two ranks, within 2
      // squares of the blocking pawn and the strong king is not closer.
      // (I think this rule fails only in practically unreachable
      // positions such as 5k1K/6p1/6P1/8/8/3B4/8/8 w and positions where
      // qsearch will immediately correct the problem such as
      // 8/4k1p1/6P1/1K6/3B4/8/8/8 w)
      if (   relative_rank_s(strongSide, weakKingSq) >= RANK_7
          && weakKingDist <= 2
          && weakKingDist <= strongKingDist)
        return SCALE_FACTOR_DRAW;
    }
  }

  return SCALE_FACTOR_NONE;
}


// KQ vs KR and one or more pawns. It tests for fortress draws with a rook
// on the third rank defended by a pawn.
static int ScaleKQKRPs(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, QueenValueMg, 0));
  assert(piece_count(weakSide, ROOK) == 1);
  assert(pieces_cp(weakSide, PAWN));

  Square kingSq = square_of(weakSide, KING);
  Square rsq = lsb(pieces_p(ROOK));

  if (    relative_rank_s(weakSide, kingSq) <= RANK_2
      &&  relative_rank_s(weakSide, square_of(strongSide, KING)) >= RANK_4
      &&  relative_rank_s(weakSide, rsq) == RANK_3
      && (  pieces_p(PAWN)
          & attacks_from_king(kingSq)
          & attacks_from_pawn(rsq, strongSide)))
    return SCALE_FACTOR_DRAW;

  return SCALE_FACTOR_NONE;
}


// KRP vs KR. This function knows a handful of the most important classes
// of drawn positions, but is far from perfect. It would probably be a
// good idea to add more knowledge in the future.
//
// It would also be nice to rewrite the actual code for this function,
// which is mostly copied from Glaurung 1.x, and is not very pretty.
static int ScaleKRPKR(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, RookValueMg, 1));
  assert(verify_material(pos, weakSide,   RookValueMg, 0));

  // Assume strongSide is white and the pawn is on files A-D.
  Square wksq = normalize(pos, strongSide, square_of(strongSide, KING));
  Square bksq = normalize(pos, strongSide, square_of(weakSide, KING));
  Square wrsq = normalize(pos, strongSide, square_of(strongSide, ROOK));
  Square wpsq = normalize(pos, strongSide, lsb(pieces_p(PAWN)));
  Square brsq = normalize(pos, strongSide, square_of(weakSide, ROOK));

  File f = file_of(wpsq);
  Rank r = rank_of(wpsq);
  Square queeningSq = make_square(f, RANK_8);
  signed tempo = (stm() == strongSide);

  // If the pawn is not too far advanced and the defending king defends
  // the queening square, use the third-rank defence.
  if (   r <= RANK_5
      && distance(bksq, queeningSq) <= 1
      && wksq <= SQ_H5
      && (rank_of(brsq) == RANK_6 || (r <= RANK_3 && rank_of(wrsq) != RANK_6)))
    return SCALE_FACTOR_DRAW;

  // The defending side saves a draw by checking from behind in case the
  // pawn has advanced to the 6th rank with the king behind.
  if (   r == RANK_6
      && distance(bksq, queeningSq) <= 1
      && rank_of(wksq) + tempo <= RANK_6
      && (rank_of(brsq) == RANK_1 || (!tempo && distance_f(brsq, wpsq) >= 3)))
    return SCALE_FACTOR_DRAW;

  if (   r >= RANK_6
      && bksq == queeningSq
      && rank_of(brsq) == RANK_1
      && (!tempo || distance(wksq, wpsq) >= 2))
    return SCALE_FACTOR_DRAW;

  // White pawn on a7 and rook on a8 is a draw if black's king is on g7
  // or h7 and the black rook is behind the pawn.
  if (   wpsq == SQ_A7
      && wrsq == SQ_A8
      && (bksq == SQ_H7 || bksq == SQ_G7)
      && file_of(brsq) == FILE_A
      && (rank_of(brsq) <= RANK_3 || file_of(wksq) >= FILE_D || rank_of(wksq) <= RANK_5))
    return SCALE_FACTOR_DRAW;

  // If the defending king blocks the pawn and the attacking king is too
  // far away, it is a draw.
  if (   r <= RANK_5
      && bksq == wpsq + NORTH
      && distance(wksq, wpsq) - tempo >= 2
      && distance(wksq, brsq) - tempo >= 2)
    return SCALE_FACTOR_DRAW;

  // Pawn on the 7th rank supported by the rook from behind usually wins
  // if the attacking king is closer to the queening square than the
  // defending king, and the defending king cannot gain tempi by
  // threatening the attacking rook.
  if (   r == RANK_7
      && f != FILE_A
      && file_of(wrsq) == f
      && wrsq != queeningSq
      && (distance(wksq, queeningSq) < distance(bksq, queeningSq) - 2 + tempo)
      && (distance(wksq, queeningSq) < distance(bksq, wrsq) + tempo))
    return SCALE_FACTOR_MAX - 2 * distance(wksq, queeningSq);

  // Similar to the above, but with the pawn further back
  if (   f != FILE_A
      && file_of(wrsq) == f
      && wrsq < wpsq
      && (distance(wksq, queeningSq) < distance(bksq, queeningSq) - 2 + tempo)
      && (distance(wksq, wpsq + NORTH) < distance(bksq, wpsq + NORTH) - 2 + tempo)
      && (  distance(bksq, wrsq) + tempo >= 3
          || (    distance(wksq, queeningSq) < distance(bksq, wrsq) + tempo
              && (distance(wksq, wpsq + NORTH) < distance(bksq, wrsq) + tempo))))
    return  SCALE_FACTOR_MAX
          - 8 * distance(wpsq, queeningSq)
          - 2 * distance(wksq, queeningSq);

  // If the pawn is not far advanced and the defending king is somewhere in
  // the pawn's path, it's probably a draw.
  if (r <= RANK_4 && bksq > wpsq) {
    if (file_of(bksq) == file_of(wpsq))
      return 10;
    if (   distance_f(bksq, wpsq) == 1
        && distance(wksq, bksq) > 2)
      return 24 - 2 * distance(wksq, bksq);
  }
  return SCALE_FACTOR_NONE;
}

static int ScaleKRPKB(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, RookValueMg, 1));
  assert(verify_material(pos, weakSide, BishopValueMg, 0));

  // Test for a rook pawn
  if (pieces_p(PAWN) & (FileABB | FileHBB)) {
    Square ksq = square_of(weakSide, KING);
    Square bsq = lsb(pieces_p(BISHOP));
    Square psq = lsb(pieces_p(PAWN));
    Rank rk = relative_rank_s(strongSide, psq);
    Square push = pawn_push(strongSide);

    // If the pawn is on the 5th rank and the pawn (currently) is on
    // the same color square as the bishop then there is a chance of
    // a fortress. Depending on the king position give a moderate
    // reduction or a stronger one if the defending king is near the
    // corner but not trapped there.
    if (rk == RANK_5 && !opposite_colors(bsq, psq)) {
      int d = distance(psq + 3 * push, ksq);

      if (d <= 2 && !(d == 0 && ksq == square_of(strongSide, KING) + 2 * push))
        return 24;
      else
        return 48;
    }

    // When the pawn has moved to the 6th rank we can be fairly sure
    // it is drawn if the bishop attacks the square in front of the
    // pawn from a reasonable distance and the defending king is near
    // the corner
    if (   rk == RANK_6
        && distance(psq + 2 * push, ksq) <= 1
        && (PseudoAttacks[BISHOP][bsq] & sq_bb(psq + push))
        && distance_f(bsq, psq) >= 2)
      return 8;
  }

  return SCALE_FACTOR_NONE;
}

// KRPP vs KRP. There is just a single rule: if the stronger side has no
// passed pawns and the defending king is actively placed, the position
// is drawish.
static int ScaleKRPPKRP(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, RookValueMg, 2));
  assert(verify_material(pos, weakSide,   RookValueMg, 1));

  Square wpsq1 = lsb(pieces_cp(strongSide, PAWN));
  Square wpsq2 = msb(pieces_cp(strongSide, PAWN));
  Square bksq = square_of(weakSide, KING);

  // Does the stronger side have a passed pawn?
  if (pawn_passed(pos, strongSide, wpsq1) || pawn_passed(pos, strongSide, wpsq2))
    return SCALE_FACTOR_NONE;

  Rank r = max(relative_rank_s(strongSide, wpsq1), relative_rank_s(strongSide, wpsq2));

  if (   distance_f(bksq, wpsq1) <= 1
      && distance_f(bksq, wpsq2) <= 1
      && relative_rank_s(strongSide, bksq) > r)
  {
    assert(r > RANK_1 && r < RANK_7);
    return 7 * r;
  }
  return SCALE_FACTOR_NONE;
}


// K and two or more pawns vs K. There is just a single rule here: If all
// pawns are on the same rook file and are blocked by the defending king,
// it is a draw.
static int ScaleKPsK(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(non_pawn_material_c(strongSide) == 0);
  assert(piece_count(strongSide, PAWN) >= 2);
  assert(verify_material(pos, weakSide, VALUE_ZERO, 0));

  Square ksq = square_of(weakSide, KING);
  Bitboard pawns = pieces_cp(strongSide, PAWN);

  // If all pawns are ahead of the king on a single rook file, it is a draw.
  if (   !(pawns & ~(FileABB | FileHBB))
      && !(pawns & ~passed_pawn_span(weakSide, ksq)))
    return SCALE_FACTOR_DRAW;

  return SCALE_FACTOR_NONE;
}


// KBP vs KB. There are two rules: if the defending king is somewhere
// along the path of the pawn, and the square of the king is not of the
// same color as the stronger side's bishop, it is a draw. If the two
// bishops have opposite color, it's almost always a draw.
static int ScaleKBPKB(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, BishopValueMg, 1));
  assert(verify_material(pos, weakSide,   BishopValueMg, 0));

  Square pawnSq = lsb(pieces_p(PAWN));
  Square strongBishopSq = square_of(strongSide, BISHOP);
  Square weakBishopSq = square_of(weakSide, BISHOP);
  Square weakKingSq = square_of(weakSide, KING);

  // Case 1: Defending king blocks the pawn, and cannot be driven away
  if (   file_of(weakKingSq) == file_of(pawnSq)
      && relative_rank_s(strongSide, pawnSq) < relative_rank_s(strongSide, weakKingSq)
      && (   opposite_colors(weakKingSq, strongBishopSq)
          || relative_rank_s(strongSide, weakKingSq) <= RANK_6))
    return SCALE_FACTOR_DRAW;

  // Case 2: Opposite colored bishops
  if (opposite_colors(strongBishopSq, weakBishopSq))
    return SCALE_FACTOR_DRAW;

  return SCALE_FACTOR_NONE;
}


// KBPP vs KB. It detects a few basic draws with opposite-colored bishops.
static int ScaleKBPPKB(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, BishopValueMg, 2));
  assert(verify_material(pos, weakSide,   BishopValueMg, 0));

  Square wbsq = square_of(strongSide, BISHOP);
  Square bbsq = square_of(weakSide, BISHOP);

  if (!opposite_colors(wbsq, bbsq))
    return SCALE_FACTOR_NONE;

  Square ksq = square_of(weakSide, KING);
  Square psq1 = lsb(pieces_cp(strongSide, PAWN));
  Square psq2 = msb(pieces_cp(strongSide, PAWN));
  int r1 = rank_of(psq1);
  int r2 = rank_of(psq2);
  Square blockSq1, blockSq2;

  if (relative_rank_s(strongSide, psq1) > relative_rank_s(strongSide, psq2)) {
    blockSq1 = psq1 + pawn_push(strongSide);
    blockSq2 = make_square(file_of(psq2), rank_of(psq1));
  } else {
    blockSq1 = psq2 + pawn_push(strongSide);
    blockSq2 = make_square(file_of(psq1), rank_of(psq2));
  }

  switch (distance_f(psq1, psq2)) {
  case 0:
    // Both pawns are on the same file. It is an easy draw if the defender
    // firmly controls some square in the frontmost pawn's path.
    if (   file_of(ksq) == file_of(blockSq1)
        && relative_rank_s(strongSide, ksq) >= relative_rank_s(strongSide, blockSq1)
        && opposite_colors(ksq, wbsq))
      return SCALE_FACTOR_DRAW;
    else
      return SCALE_FACTOR_NONE;

  case 1:
    // Pawns on adjacent files. It is a draw if the defender firmly controls
    // the square in front of the frontmost pawn's path, and the square
    // diagonally behind this square on the file of the other pawn.
    if (   ksq == blockSq1
        && opposite_colors(ksq, wbsq)
        && (   bbsq == blockSq2
            || (attacks_from_bishop(blockSq2) & pieces_cp(weakSide, BISHOP))
            || distance(r1, r2) >= 2))
      return SCALE_FACTOR_DRAW;

    else if (   ksq == blockSq2
             && opposite_colors(ksq, wbsq)
             && (   bbsq == blockSq1
                 || (attacks_from_bishop(blockSq1)
                                         & pieces_cp(weakSide, BISHOP))))
      return SCALE_FACTOR_DRAW;
    else
      return SCALE_FACTOR_NONE;

  default:
    // The pawns are not on the same file or adjacent files. No scaling.
    return SCALE_FACTOR_NONE;
  }
}


// KBP vs KN. There is a single rule: If the defending king is somewhere
// along the path of the pawn, and the square of the king is not of the
// same color as the stronger side's bishop, it is a draw.
static int ScaleKBPKN(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, BishopValueMg, 1));
  assert(verify_material(pos, weakSide, KnightValueMg, 0));

  Square pawnSq = lsb(pieces_p(PAWN));
  Square strongBishopSq = lsb(pieces_p(BISHOP));
  Square weakKingSq = square_of(weakSide, KING);

  if (   file_of(weakKingSq) == file_of(pawnSq)
      && relative_rank_s(strongSide, pawnSq) < relative_rank_s(strongSide, weakKingSq)
      && (   opposite_colors(weakKingSq, strongBishopSq)
          || relative_rank_s(strongSide, weakKingSq) <= RANK_6))
    return SCALE_FACTOR_DRAW;

  return SCALE_FACTOR_NONE;
}


// KP vs KP. This is done by removing the weakest side's pawn and probing
// the KP vs K bitbase: If the weakest side has a draw without the pawn,
// it probably has at least a draw with the pawn as well. The exception
// is when the stronger side's pawn is far advanced and not on a rook
// file; in this case it is often possible to win
// (e.g. 8/4k3/3p4/3P4/6K1/8/8/8 w - - 0 1).
static int ScaleKPKP(const Position *pos, Color strongSide)
{
  Color weakSide = !strongSide;

  assert(verify_material(pos, strongSide, VALUE_ZERO, 1));
  assert(verify_material(pos, weakSide,   VALUE_ZERO, 1));

  // Assume strongSide is white and the pawn is on files A-D
  Square wksq = normalize(pos, strongSide, square_of(strongSide, KING));
  Square bksq = normalize(pos, strongSide, square_of(weakSide, KING));
  Square psq  = normalize(pos, strongSide, square_of(strongSide, PAWN));

  Color us = strongSide == stm() ? WHITE : BLACK;

  // If the pawn has advanced to the fifth rank or further, and is not a
  // rook pawn, it is too dangerous to assume that it is at least a draw.
  if (rank_of(psq) >= RANK_5 && file_of(psq) != FILE_A)
    return SCALE_FACTOR_NONE;

  // Probe the KPK bitbase with the weakest side's pawn removed. If it is
  // a draw, it is probably at least a draw even with the pawn.
  return bitbases_probe(wksq, psq, bksq, us) ? SCALE_FACTOR_NONE : SCALE_FACTOR_DRAW;
}

#else

typedef int make_iso_compilers_happy;

#endif

/* ==========================================
   FILE: evaluate.c
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2018 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#ifdef NNUE
#endif

#ifndef NNUE_PURE

// Struct EvalInfo contains various information computed and collected
// by the evaluation functions.
struct EvalInfo {
  MaterialEntry *me;
  PawnEntry *pe;
  Bitboard mobilityArea[2];

  // attackedBy[color][piece type] is a bitboard representing all squares
  // attacked by a given color and piece type. A special "piece type" which
  // is also calculated is ALL_PIECES.
  Bitboard attackedBy[2][8];

  // attackedBy2[color] are the squares attacked by 2 pieces of a given
  // color, possibly via x-ray or by one pawn and one piece. Diagonal
  // x-ray through pawn or squares attacked by 2 pawns are not explicitly
  // added.
  Bitboard attackedBy2[2];

  // kingRing[color] are the squares adjacent to the king plus some other
  // very near squares, depending on king position.
  Bitboard kingRing[2];

  // kingAttackersCount[color] is the number of pieces of the given color
  // which attack a square in the kingRing of the enemy king.
  int kingAttackersCount[2];

  // kingAttackersWeight[color] is the sum of the "weights" of the pieces
  // of the given color which attack a square in the kingRing of the enemy
  // king. The weights of the individual piece types are given by the
  // elements in the KingAttackWeights array.
  int kingAttackersWeight[2];

  // kingAttacksCount[color] is the number of attacks by the given color to
  // squares directly adjacent to the enemy king. Pieces which attack more
  // than one square are counted multiple times. For instance, if there is
  // a white knight on g5 and black's king is on g8, this white knight adds
  // 2 to kingAttacksCount[WHITE].
  int kingAttacksCount[2];
};

typedef struct EvalInfo EvalInfo;

static const Bitboard KingFlank[8] = {
  QueenSide ^ FileDBB, QueenSide, QueenSide, CenterFiles,
  CenterFiles, KingSide, KingSide, KingSide ^ FileEBB
};

// Thresholds for lazy and space evaluation
enum {
  LazyThreshold1 =  1565,
  LazyThreshold2 =  1102,
  SpaceThreshold = 11551,
  NNUEThreshold1 =   682,
  NNUEThreshold2 =   176
};

// KingAttackWeights[PieceType] contains king attack weights by piece type
static const int KingAttackWeights[8] = { 0, 0, 81, 52, 44, 10 };

// Penalties for enemy's safe checks
static const int SafeCheck[][2] = {
  {0}, {0}, { 803, 1292 }, { 639, 974 }, { 1087, 1878 }, { 759, 1132 }
};

#define V(v) (Value)(v)
#define S(mg,eg) make_score(mg,eg)

// MobilityBonus[PieceType-2][attacked] contains bonuses for middle and
// end game, indexed by piece type and number of attacked squares in the
// mobility area.
static const Score MobilityBonus[4][32] = {
  // Knight
  { S(-62,-79), S(-53,-57), S(-12,-31), S( -3,-17), S(  3,  7), S( 12, 13),
    S( 21, 16), S( 28, 21), S( 37, 26) },
  // Bishop
  { S(-47,-59), S(-20,-25), S( 14, -8), S( 29, 12), S( 39, 21), S( 53, 40),
    S( 53, 56), S( 60, 58), S( 62, 65), S( 69, 72), S( 78, 78), S( 83, 87),
    S( 91, 88), S( 96, 98) },
  // Rook
  { S(-60,-82), S(-24,-15), S(  0, 17), S(  3, 43), S(  4, 72), S( 14,100),
    S( 20,102), S( 30,122), S( 41,133), S( 41,139), S( 41,153), S( 45,160),
    S( 57,165), S( 58,170), S( 67,175) },
  // Queen
  { S(-29,-49), S(-16,-29), S( -8, -8), S( -8, 17), S( 18, 39), S( 25, 54),
    S( 23, 59), S( 37, 73), S( 41, 76), S( 54, 95), S( 65, 95) ,S( 68,101),
    S( 69,124), S( 70,128), S( 70,132), S( 70,133) ,S( 71,136), S( 72,140),
    S( 74,147), S( 76,149), S( 90,153), S(104,169), S(105,171), S(106,171),
    S(112,178), S(114,185), S(114,187), S(119,221) }
};

// BishopsPawns[distance from edge] contains a file-dependent penalty for
// pawns on squares of the same color as our bishop.
static const Score BishopPawns[8] = {
  S(3, 8), S(3, 9), S(2, 8), S(3, 8), S(3, 8), S(2, 8), S(3, 9), S(3, 8)
};

static const Score RookOnClosedFile = S(10, 5);
static const Score RookOnOpenFile[2] = { S(19, 6), S(47, 26) };

// ThreatByMinor/ByRook[attacked PieceType] contains bonuses according to
// which piece type attacks which one. Attacks on lesser pieces which are
// pawn defended are not considered.
static const Score ThreatByMinor[8] = {
  S(0, 0), S(5, 32), S(55, 41), S(77, 56), S(89,119), S(79,162)
};

static const Score ThreatByRook[8] = {
  S(0, 0), S(3, 44), S(37, 68), S(42, 60), S( 0, 39), S(58, 43)
};

// PassedRank[mg/eg][Rank] contains midgame and endgame bonuses for passed
// pawns. We don't use a Score because we process the two components
// independently.
static const Value PassedRank[2][8] = {
  { V(0), V( 7), V(16), V(17), V(64), V(170), V(278) },
  { V(0), V(27), V(32), V(40), V(71), V(174), V(262) }
};

// PassedFile[File] contains a bonus according to the file of a passed pawn
static const Score PassedFile[8] = {
  S( 0,  0), S(11,  8), S(22, 16), S(33, 24),
  S(33, 24), S(22, 16), S(11,  8), S( 0,  0)
};

// Assorted bonuses and penalties used by evaluation
static const Score BishopKingProtector = S(  6,  9);
static const Score BishopOnKingRing    = S( 24,  0);
static const Score BishopOutpost       = S( 31, 24);
static const Score BishopXRayPawns     = S(  4,  5);
static const Score CorneredBishop      = S( 50, 50);
static const Score FlankAttacks        = S(  8,  0);
static const Score Hanging             = S( 69, 36);
static const Score KnightKingProtector = S(  8,  9);
static const Score KnightOnQueen       = S( 16, 11);
static const Score KnightOutpost       = S( 57, 38);
static const Score LongDiagonalBishop  = S( 45,  0);
static const Score MinorBehindPawn     = S( 18,  3);
static const Score PawnlessFlank       = S( 17, 95);
static const Score ReachableOutpost    = S( 31, 22);
static const Score RestrictedPiece     = S(  7,  7);
static const Score RookOnKingRing      = S( 16,  0);
static const Score SliderOnQueen       = S( 60, 18);
static const Score ThreatByKing        = S( 24, 89);
static const Score ThreatByPawnPush    = S( 48, 39);
static const Score ThreatBySafePawn    = S(173, 94);
static const Score TrappedRook         = S( 55, 13);
static const Score UncontestedOutpost  = S(  1, 10);
static const Score WeakQueen           = S( 56, 15);
static const Score WeakQueenProtection = S( 14,  0);

static const Value CorneredBishopV     = 50;

#undef S
#undef V

// eval_init() initializes king and attack bitboards for a given color
// adding pawn attacks. To be done at the beginning of the evaluation.

INLINE void evalinfo_init(const Position *pos, EvalInfo *ei, const Color Us)
{
  const Color Them = Us == WHITE ? BLACK : WHITE;
  const int   Down = Us == WHITE ? SOUTH : NORTH;
  const Bitboard LowRanks = Us == WHITE ? Rank2BB | Rank3BB
                                        : Rank7BB | Rank6BB;

  const Square ksq = square_of(Us, KING);

  Bitboard dblAttackByPawn = pawn_double_attacks_bb(pieces_cp(Us, PAWN), Us);

  // Find our pawns on the first two ranks, and those which are blocked
  Bitboard b = pieces_cp(Us, PAWN) & (shift_bb(Down, pieces()) | LowRanks);

  // Squares occupied by those pawns, by our king or queen, by blockers to
  // attacks on our king or controlled by enemy pawns are excluded from the
  // mobility area
  ei->mobilityArea[Us] = ~(b | pieces_cpp(Us, KING, QUEEN) | blockers_for_king(pos, Us) | ei->pe->pawnAttacks[Them]);

  // Initialise attackedBy[] for kings and pawns
  b = ei->attackedBy[Us][KING] = attacks_from_king(square_of(Us, KING));
  ei->attackedBy[Us][PAWN] = ei->pe->pawnAttacks[Us];
  ei->attackedBy[Us][0] = b | ei->attackedBy[Us][PAWN];
  ei->attackedBy2[Us] = (b & ei->attackedBy[Us][PAWN]) | dblAttackByPawn;

  // Init our king safety tables only if we are going to use them
  Square s = make_square(clamp(file_of(ksq), FILE_B, FILE_G),
                         clamp(rank_of(ksq), RANK_2, RANK_7));
  ei->kingRing[Us] = PseudoAttacks[KING][s] | sq_bb(s);

  ei->kingAttackersCount[Them] = popcount(ei->kingRing[Us] & ei->pe->pawnAttacks[Them]);
  ei->kingAttacksCount[Them] = ei->kingAttackersWeight[Them] = 0;

  // Remove from kingRing[] the squares defended by two pawns
  ei->kingRing[Us] &= ~dblAttackByPawn;
}


// evaluate_piece() assigns bonuses and penalties to the pieces of a given
// color and type.

INLINE Score evaluate_pieces(const Position *pos, EvalInfo *ei, Score *mobility,
    const Color Us, const int Pt)
{
  const Color Them  = Us == WHITE ? BLACK : WHITE;
  const int   Down  = Us == WHITE ? SOUTH : NORTH;
  const Bitboard OutpostRanks = Us == WHITE ? Rank4BB | Rank5BB | Rank6BB
                                            : Rank5BB | Rank4BB | Rank3BB;

  Bitboard b, bb;
  Square s;
  Score score = SCORE_ZERO;

  ei->attackedBy[Us][Pt] = 0;

  loop_through_pieces(Us, Pt, s) {
    // Find attacked squares, including x-ray attacks for bishops and rooks
    b = Pt == BISHOP ? attacks_bb_bishop(s, pieces() ^ pieces_p(QUEEN))
      : Pt == ROOK ? attacks_bb_rook(s,
                              pieces() ^ pieces_p(QUEEN) ^ pieces_cp(Us, ROOK))
                   : attacks_from(Pt, s);

    if (blockers_for_king(pos, Us) & sq_bb(s))
      b &= LineBB[square_of(Us, KING)][s];

    ei->attackedBy2[Us] |= ei->attackedBy[Us][0] & b;
    ei->attackedBy[Us][Pt] |= b;
    ei->attackedBy[Us][0] |= b;

    if (b & ei->kingRing[Them]) {
      ei->kingAttackersCount[Us]++;
      ei->kingAttackersWeight[Us] += KingAttackWeights[Pt];
      ei->kingAttacksCount[Us] += popcount(b & ei->attackedBy[Them][KING]);
    }
    else if (Pt == ROOK && (file_bb_s(s) & ei->kingRing[Them]))
      score += RookOnKingRing;
    else if (Pt == BISHOP && (attacks_bb_bishop(s, pieces_p(PAWN)) & ei->kingRing[Them]))
      score += BishopOnKingRing;

    int mob = popcount(b & ei->mobilityArea[Us]);

    mobility[Us] += MobilityBonus[Pt - 2][mob];

    if (Pt == BISHOP || Pt == KNIGHT) {
      // Bonus if the piece is on an outpost square or can reach one.
      // Bonus for knights (UncontestedOutpost) if few relevant targets.
      bb = OutpostRanks & (ei->attackedBy[Us][PAWN] | shift_bb(Down, pieces_p(PAWN)))
                        & ~ei->pe->pawnAttacksSpan[Them];
      Bitboard targets = pieces_c(Them) & ~pieces_p(PAWN);
      if (   Pt == KNIGHT
          && (bb & sq_bb(s) & ~CenterFiles) // on a side outpost
          && !(b & targets)                 // no relevant attacks
          && (!more_than_one(targets & (sq_bb(s) & QueenSide ? QueenSide : KingSide))))
        score += UncontestedOutpost * popcount(pieces_p(PAWN) & (sq_bb(s) & QueenSide ? QueenSide : KingSide));
      else if (bb & sq_bb(s))
        score += Pt == KNIGHT ? KnightOutpost : BishopOutpost;

      else if (Pt == KNIGHT && bb & b & ~pieces_c(Us))
        score += ReachableOutpost;

      // Knight and Bishop bonus for being right behind a pawn
      if (shift_bb(Down, pieces_p(PAWN)) & sq_bb(s))
        score += MinorBehindPawn;

      // Penalty if the minor is far from the king
      score -= (Pt == KNIGHT ? KnightKingProtector : BishopKingProtector) *
                  distance(s, square_of(Us, KING));

      if (Pt == BISHOP) {
        // Penalty according to number of pawns on the same color square as
        // the bishop, bigger when the center files are blocked with pawns
        // and smaller when the bishop is outside the pawn chain
        Bitboard blocked = pieces_cp(Us, PAWN) & shift_bb(Down, pieces());

        score -=  BishopPawns[file_of(s)]
                             *  pawns_on_same_color_squares(ei->pe, Us, s)
                             * (!(ei->attackedBy[Us][PAWN] & sq_bb(s))
                + popcount(blocked & CenterFiles));

        // Penalty for all enemy pawns x-rayed
        score -= BishopXRayPawns * popcount(PseudoAttacks[BISHOP][s] & pieces_cp(Them, PAWN));

        // Bonus for bishop on a long diagonal which can "see" both center
        // squares
        if (more_than_one(attacks_bb_bishop(s, pieces_p(PAWN)) & Center))
          score += LongDiagonalBishop;

        // An important Chess960 pattern: a cornered bishop blocked by a
        // friendly pawn diagonally in front of it is a very serious problem,
        // especially when that pawn is also blocked.
        if (   is_chess960()
            && (s == relative_square(Us, SQ_A1) || s == relative_square(Us, SQ_H1)))
        {
          Square d = pawn_push(Us) + (file_of(s) == FILE_A ? EAST : WEST);
          if (piece_on(s + d) == make_piece(Us, PAWN))
            score -= !is_empty(s + d + pawn_push(Us)) ? CorneredBishop * 4
                                                      : CorneredBishop * 3;
        }
      }
    }

    if (Pt == ROOK) {
      // Bonuses for rook on a (semi-)open or closed file
      if (is_on_semiopen_file(ei->pe, Us, s))
        score += RookOnOpenFile[is_on_semiopen_file(ei->pe, Them, s)];
      else {
        // If our pawn on this file is blocked, increase penalty
        if (pieces_cp(Us, PAWN) & shift_bb(Down, pieces()) & file_bb_s(s))
          score -= RookOnClosedFile;

        // Penalty when trapped by the king. Even more if the king cannot castle
        if (mob <= 3) {
          File kf = file_of(square_of(Us, KING));
          if ((kf < FILE_E) == (file_of(s) < kf))
            score -= TrappedRook * (1 + !can_castle_c(Us));
        }
      }
    }

    if (Pt == QUEEN) {
      // Penalty if any relative pin or discovered attack against the queen
      Bitboard pinners;
      if (slider_blockers(pos, pieces_cpp(Them, ROOK, BISHOP), s, &pinners))
          score -= WeakQueen;
    }
  }

  return score;
}

// evaluate_king() assigns bonuses and penalties to a king of a given color.

INLINE Score evaluate_king(const Position *pos, EvalInfo *ei, Score *mobility,
    const Color Us)
{
  const Color Them = Us == WHITE ? BLACK : WHITE;
  const Bitboard Camp =  Us == WHITE
                       ? AllSquares ^ Rank6BB ^ Rank7BB ^ Rank8BB
                       : AllSquares ^ Rank1BB ^ Rank2BB ^ Rank3BB;

  const Square ksq = square_of(Us, KING);
  Bitboard weak, b1, b2, b3, safe, unsafeChecks = 0;
  Bitboard rookChecks, queenChecks, bishopChecks, knightChecks;
  int kingDanger = 0;

  // King shelter and enemy pawns storm
  Score score = Us == WHITE ? king_safety_white(ei->pe, pos, ksq)
                            : king_safety_black(ei->pe, pos, ksq);

  // Attacked squares defended at most once by our queen or king
  weak =  ei->attackedBy[Them][0]
        & ~ei->attackedBy2[Us]
        & ( ~ei->attackedBy[Us][0]
           | ei->attackedBy[Us][KING] | ei->attackedBy[Us][QUEEN]);

  // Analyse the safe enemy's checks which are possible on next move
  safe  = ~pieces_c(Them);
  safe &= ~ei->attackedBy[Us][0] | (weak & ei->attackedBy2[Them]);

  b1 = attacks_bb_rook(ksq, pieces() ^ pieces_cp(Us, QUEEN));
  b2 = attacks_bb_bishop(ksq, pieces() ^ pieces_cp(Us, QUEEN));

  // Enemy rooks checks
  rookChecks = b1 & ei->attackedBy[Them][ROOK] & safe;
  if (rookChecks)
    kingDanger += SafeCheck[ROOK][more_than_one(rookChecks)];
  else
    unsafeChecks |= b1 & ei->attackedBy[Them][ROOK];

  queenChecks =  (b1 | b2) & ei->attackedBy[Them][QUEEN] & safe
               & ~(ei->attackedBy[Us][QUEEN] | rookChecks);
  if (queenChecks)
    kingDanger += SafeCheck[QUEEN][more_than_one(queenChecks)];

  // Enemy bishops checks: we count them only if they are from squares from
  // which we can't give a queen check, because queen checks are more valuable.
  bishopChecks =  b2 & ei->attackedBy[Them][BISHOP] & safe
                & ~queenChecks;
  if (bishopChecks)
    kingDanger += SafeCheck[BISHOP][more_than_one(bishopChecks)];
  else
    unsafeChecks |= b2 & ei->attackedBy[Them][BISHOP];

  // Enemy knights checks
  knightChecks = attacks_from_knight(ksq) & ei->attackedBy[Them][KNIGHT];
  if (knightChecks & safe)
    kingDanger += SafeCheck[KNIGHT][more_than_one(knightChecks & safe)];
  else
    unsafeChecks |= knightChecks;

  // Find the squares that opponent attacks in our king flank, the squares
  // which they attack twice in that flank, and the squares that we defend.
  b1 = ei->attackedBy[Them][0] & KingFlank[file_of(ksq)] & Camp;
  b2 = b1 & ei->attackedBy2[Them];
  b3 = ei->attackedBy[Us][0] & KingFlank[file_of(ksq)] & Camp;

  int kingFlankAttack = popcount(b1) + popcount(b2);
  int kingFlankDefense = popcount(b3);

  kingDanger +=  ei->kingAttackersCount[Them] * ei->kingAttackersWeight[Them]
               + 183 * popcount(ei->kingRing[Us] & weak)
               + 148 * popcount(unsafeChecks)
               +  98 * popcount(blockers_for_king(pos, Us))
               +  69 * ei->kingAttacksCount[Them]
               +   3 * kingFlankAttack * kingFlankAttack / 8
               +       mg_value(mobility[Them] - mobility[Us])
               - 873 * !pieces_cp(Them, QUEEN)
               - 100 * !!(ei->attackedBy[Us][KNIGHT] & ei->attackedBy[Us][KING])
               -   6 * mg_value(score) / 8
               -   4 * kingFlankDefense
               +  37;

  // Transform the kingDanger units into a Score, and subtract it from
  // the evaluation
  if (kingDanger > 100)
    score -= make_score(kingDanger * kingDanger / 4096, kingDanger / 16);

  // Penalty when our king is on a pawnless flank
  if (!(pieces_p(PAWN) & KingFlank[file_of(ksq)]))
    score -= PawnlessFlank;

  // Penalty if king flank is under attack, potentially moving toward the king
  score -= FlankAttacks * kingFlankAttack;

  return score;
}


// evaluate_threats() assigns bonuses according to the types of the
// attacking and the attacked pieces.

INLINE Score evaluate_threats(const Position *pos, EvalInfo *ei, const Color Us)
{
  const Color Them = Us == WHITE ? BLACK : WHITE;
  const int   Up   = Us == WHITE ? NORTH : SOUTH;
  const Bitboard TRank3BB = Us == WHITE ? Rank3BB : Rank6BB;

  enum { Minor, Rook };

  Bitboard b, weak, defended, nonPawnEnemies, stronglyProtected, safe;
  Score score = SCORE_ZERO;

  // Non-pawn enemies
  nonPawnEnemies = pieces_c(Them) & ~pieces_p(PAWN);

  // Squares strongly protected by the opponent, either because they attack the
  // square with a pawn or because they attack the square twice and we don't.
  stronglyProtected =  ei->attackedBy[Them][PAWN]
                     | (ei->attackedBy2[Them] & ~ei->attackedBy2[Us]);

  // Non-pawn enemies, strongly protected
  defended = nonPawnEnemies & stronglyProtected;

  // Enemies not strongly protected and under our attack
  weak = pieces_c(Them) & ~stronglyProtected & ei->attackedBy[Us][0];

  // Add a bonus according to the kind of attacking pieces
  if (defended | weak) {
    b = (defended | weak) & (ei->attackedBy[Us][KNIGHT] | ei->attackedBy[Us][BISHOP]);
    while (b)
      score += ThreatByMinor[piece_on(pop_lsb(&b)) - 8 * Them];

    b = weak & ei->attackedBy[Us][ROOK];
    while (b)
      score += ThreatByRook[piece_on(pop_lsb(&b)) - 8 * Them];

    if (weak & ei->attackedBy[Us][KING])
      score += ThreatByKing;

    b =  ~ei->attackedBy[Them][0]
       | (nonPawnEnemies & ei->attackedBy2[Us]);
    score += Hanging * popcount(weak & b);

    // Additional bonus if weak piece is only protected by a queen
    score += WeakQueenProtection * popcount(weak & ei->attackedBy[Them][QUEEN]);
  }

  // Bonus for restricting their piece moves
  b =   ei->attackedBy[Them][0]
     & ~stronglyProtected
     &  ei->attackedBy[Us][0];
  score += RestrictedPiece * popcount(b);

  // Protected or unattacked squares
  safe = ~ei->attackedBy[Them][0] | ei->attackedBy[Us][0];

  // Bonus for attacking enemy pieces with our relatively safe pawns
  b = pieces_cp(Us, PAWN) & safe;
  b = pawn_attacks_bb(b, Us) & nonPawnEnemies;
  score += ThreatBySafePawn * popcount(b);

  // Find the squares reachable by a single pawn push
  b  = shift_bb(Up, pieces_cp(Us, PAWN)) & ~pieces();
  b |= shift_bb(Up, b & TRank3BB) & ~pieces();

  // Keep only those squares which are relatively safe
  b &= ~ei->attackedBy[Them][PAWN] & safe;

  // Bonus for safe pawn threats on the next move
  b = pawn_attacks_bb(b, Us) & nonPawnEnemies;
  score += ThreatByPawnPush * popcount(b);

  // Bonus for impending threats against enemy queen
  if (piece_count(Them, QUEEN) == 1) {
    bool queenImbalance = !pieces_cp(Us, QUEEN);

    Square s = square_of(Them, QUEEN);
    safe =   ei->mobilityArea[Us]
          & ~pieces_cp(Us, PAWN)
          & ~stronglyProtected;

    b = ei->attackedBy[Us][KNIGHT] & attacks_from_knight(s);

    score += KnightOnQueen * popcount(b & safe) * (1 + queenImbalance);

    b =  (ei->attackedBy[Us][BISHOP] & attacks_from_bishop(s))
       | (ei->attackedBy[Us][ROOK  ] & attacks_from_rook(s));

    score += SliderOnQueen * popcount(b & safe & ei->attackedBy2[Us]) * (1 + queenImbalance);
  }

  return score;
}


// Helper function
INLINE int capped_distance(Square s1, Square s2)
{
  return min(distance(s1, s2), 5);
}

// evaluate_passed() evaluates the passed pawns and candidate passed
// pawns of the given color.

INLINE Score evaluate_passed(const Position *pos, EvalInfo *ei, const Color Us)
{
  const Color Them = Us == WHITE ? BLACK : WHITE;
  const int   Up   = Us == WHITE ? NORTH : SOUTH;
  const int   Down = Us == WHITE ? SOUTH : NORTH;

  Bitboard b, bb, squaresToQueen, unsafeSquares, blockedPassers, helpers;
  Score score = SCORE_ZERO;

  b = ei->pe->passedPawns[Us];

  blockedPassers = b & shift_bb(Down, pieces_cp(Them, PAWN));
  if (blockedPassers) {
    helpers =  shift_bb(Up, pieces_cp(Us, PAWN))
             & ~pieces_c(Them)
             & (~ei->attackedBy2[Them] | ei->attackedBy[Us][0]);

    // Remove blocked candidate passers that don't have help to pass
    b &=  ~blockedPassers
        | shift_bb(WEST, helpers)
        | shift_bb(EAST, helpers);
  }

  while (b) {
    Square s = pop_lsb(&b);

    assert(!(pieces_cp(Them, PAWN) & forward_file_bb(Us, s + Up)));

    int r = relative_rank_s(Us, s);

    Value mbonus = PassedRank[MG][r], ebonus = PassedRank[EG][r];

    if (r > RANK_3) {
      int w = 5 * r - 13;
      Square blockSq = s + Up;

      // Adjust bonus based on the king's proximity
      ebonus += ( (capped_distance(square_of(Them, KING), blockSq) * 19) / 4
                 - capped_distance(square_of(Us, KING), blockSq) * 2 ) * w;

      // If blockSq is not the queening square then consider also a second push
      if (r != RANK_7)
        ebonus -= capped_distance(square_of(Us, KING), blockSq + Up) * w;

      // If the pawn is free to advance, then increase the bonus
      if (is_empty(blockSq)) {
        // If there is a rook or queen attacking/defending the pawn from behind,
        // consider all the squaresToQueen. Otherwise consider only the squares
        // in the pawn's path attacked or occupied by the enemy.
        squaresToQueen = forward_file_bb(Us, s);
        unsafeSquares = passed_pawn_span(Us, s);

        bb = forward_file_bb(Them, s) & pieces_pp(ROOK, QUEEN);

        if (!(pieces_c(Them) & bb))
          unsafeSquares &= ei->attackedBy[Them][0] | pieces_c(Them);

        // If there are no enemy pieces or attacks on passed pawn span, assign
        // a big bonus. Otherwise, assign a smaller bonus if the path to queen
        // is not attacked and an even smaller bonus if it is attacked but
        // block square is not.
        int k =  !unsafeSquares                               ? 36
               : !(unsafeSquares & ~ei->attackedBy[Us][PAWN]) ? 30
               : !(unsafeSquares & squaresToQueen)            ? 17
               : !(unsafeSquares & sq_bb(blockSq))            ?  7 : 0;

        // Assign a larger bonus if the block square is defended
        if ((pieces_c(Us) & bb) || (ei->attackedBy[Us][0] & sq_bb(blockSq)))
          k += 5;

        mbonus += k * w, ebonus += k * w;
      }
    } // r > RANK_3

    score += make_score(mbonus, ebonus) - PassedFile[file_of(s)];
  }

  return score;
}


// evaluate_space() computes the space evaluation for a given side. The
// space evaluation is a simple bonus based on the number of safe squares
// available for minor pieces on the central four files on ranks 2--4. Safe
// squares one, two or three squares behind a friendly pawn are counted
// twice. Finally, the space bonus is multiplied by a weight. The aim is to
// improve play on game opening.

INLINE Score evaluate_space(const Position *pos, EvalInfo *ei, const Color Us)
{
  // Early exit if, for example, bot queens or 6 minor pieces have been
  // exchanged
  if (non_pawn_material() < SpaceThreshold)
    return SCORE_ZERO;

  const Color Them = Us == WHITE ? BLACK : WHITE;
  const int   Down = Us == WHITE ? SOUTH : NORTH;
  const Bitboard SpaceMask = Us == WHITE
    ? (FileCBB | FileDBB | FileEBB | FileFBB) & (Rank2BB | Rank3BB | Rank4BB)
    : (FileCBB | FileDBB | FileEBB | FileFBB) & (Rank7BB | Rank6BB | Rank5BB);

  // Find the available squares for our pieces inside the SpaceMask area
  Bitboard safe =   SpaceMask
                 & ~pieces_cp(Us, PAWN)
                 & ~ei->attackedBy[Them][PAWN];

  // Find all squares which are at most three squares behind some friendly pawn
  Bitboard behind = pieces_cp(Us, PAWN);
  behind |= shift_bb(Down, behind);
  behind |= shift_bb(Down + Down, behind);

  int bonus = popcount(safe) + popcount(behind & safe & ~ei->attackedBy[Them][0]);
  int weight = popcount(pieces_c(Us)) - 3 + min(ei->pe->blockedCount, 9);
  Score score = make_score(bonus * weight * weight / 16, 0);

  return score;
}


// evaluate_winnable() adusts the mg and eg score components based on the
// known attacking/defending status of the players.
// A single value is derived from the mg and eg values and returned.
INLINE Value evaluate_winnable(const Position *pos, EvalInfo *ei, Score score)
{
  int outflanking = distance_f(square_of(WHITE, KING), square_of(BLACK, KING))
          + rank_of(square_of(WHITE, KING)) - rank_of(square_of(BLACK, KING));

  bool pawnsOnBothFlanks =   (pieces_p(PAWN) & QueenSide)
                          && (pieces_p(PAWN) & KingSide);

  bool almostUnwinnable =   outflanking < 0
                         && !pawnsOnBothFlanks;

  bool infiltration =   rank_of(square_of(WHITE, KING)) > RANK_4
                     || rank_of(square_of(BLACK, KING)) < RANK_5;

  // Compute the complexity bonus for the attacking side
  int complexity =   9 * ei->pe->passedCount
                  + 12 * popcount(pieces_p(PAWN))
                  +  9 * outflanking
                  + 21 * pawnsOnBothFlanks
                  + 24 * infiltration
                  + 51 * !non_pawn_material()
                  - 43 * almostUnwinnable
                  - 110;

  Value mg = mg_value(score);
  Value eg = eg_value(score);

  // Now apply the bonus: note that we find the attacking side by extracting
  // the sign of the midgame or endgame values, and that we carefully cap the
  // bonus so that the midgame and endgame scores will never change sign after
  // the bonus.
  int u = ((mg > 0) - (mg < 0)) * clamp(complexity + 50, -abs(mg), 0);
  int v = ((eg > 0) - (eg < 0)) * max(complexity, -abs(eg));

  mg += u;
  eg += v;

  // Compute the scale factor for the winning side
  Color strongSide = eg > VALUE_DRAW ? WHITE : BLACK;
  int sf = material_scale_factor(ei->me, pos, strongSide);

  // If scale is not already specific, scale down via general heuristics
  if (sf == SCALE_FACTOR_NORMAL) {
    if (opposite_bishops(pos)) {
      if (   non_pawn_material_c(WHITE) == BishopValueMg
          && non_pawn_material_c(BLACK) == BishopValueMg)
        sf = 18 + 4 * popcount(ei->pe->passedPawns[strongSide]);
      else
        sf = 22 + 3 * popcount(pieces_c(strongSide));
    } else if (   non_pawn_material_c(WHITE) == RookValueMg
               && non_pawn_material_c(BLACK) == RookValueMg
               && piece_count(strongSide, PAWN) - piece_count(!strongSide, PAWN) <= 1
               && !(KingSide & pieces_cp(strongSide, PAWN)) != !(QueenSide & pieces_cp(strongSide, PAWN))
               && (attacks_from_king(square_of(!strongSide, KING)) & pieces_cp(!strongSide, PAWN)))
      sf = 36;
    else if (popcount(pieces_p(QUEEN)) == 1)
      sf = 37 + 3 * (pieces_cp(WHITE, QUEEN) ? piece_count(BLACK, BISHOP) + piece_count(BLACK, KNIGHT)
                                             : piece_count(WHITE, BISHOP) + piece_count(WHITE, KNIGHT));
    else
      sf = min(sf, 36 + 7 * piece_count(strongSide, PAWN)) - 4 * !pawnsOnBothFlanks;;

    sf -= 4 * !pawnsOnBothFlanks;
  }

  // Interpolate between the middlegame and the scaled endgame score
  v =  mg * ei->me->gamePhase
     + eg * (PHASE_MIDGAME - ei->me->gamePhase) * sf / SCALE_FACTOR_NORMAL;
  v /= PHASE_MIDGAME;

  return v;
}

// evaluate_classical() is the classical evaluation function. It returns
// a static evaluation of the position from the point of view of the side
// to move.

static Value evaluate_classical(const Position *pos)
{
  assert(!checkers());

  Score mobility[2] = { SCORE_ZERO, SCORE_ZERO };
  Value v;
  EvalInfo ei;

  // Probe the material hash table
  ei.me = material_probe(pos);

  // If we have a specialized evaluation function for the current material
  // configuration, call it and return.
  if (material_specialized_eval_exists(ei.me))
    return material_evaluate(ei.me, pos);

  // Initialize score by reading the incrementally updated scores included
  // in the position struct (material + piece square tables) and the
  // material imbalance. Score is computed internally from the white point
  // of view.
  Score score = psq_score() + material_imbalance(ei.me) + pos->contempt;

  // Probe the pawn hash table
  ei.pe = pawn_probe(pos);
  score += ei.pe->score;

  // Early exit if score is high
#define lazy_skip(v) (abs(mg_value(score) + eg_value(score)) / 2 > v + non_pawn_material() / 64)
  if (lazy_skip(LazyThreshold1))
    goto make_v;

  // Initialize attack and king safety bitboards.
  evalinfo_init(pos, &ei, WHITE);
  evalinfo_init(pos, &ei, BLACK);

  // Evaluate all pieces but king and pawns
  score +=  evaluate_pieces(pos, &ei, mobility, WHITE, KNIGHT)
          - evaluate_pieces(pos, &ei, mobility, BLACK, KNIGHT)
          + evaluate_pieces(pos, &ei, mobility, WHITE, BISHOP)
          - evaluate_pieces(pos, &ei, mobility, BLACK, BISHOP)
          + evaluate_pieces(pos, &ei, mobility, WHITE, ROOK)
          - evaluate_pieces(pos, &ei, mobility, BLACK, ROOK)
          + evaluate_pieces(pos, &ei, mobility, WHITE, QUEEN)
          - evaluate_pieces(pos, &ei, mobility, BLACK, QUEEN);

  score += mobility[WHITE] - mobility[BLACK];

  // Evaluate kings after all other pieces because we need full attack
  // information when computing the king safety evaluation.
  score +=  evaluate_king(pos, &ei, mobility, WHITE)
          - evaluate_king(pos, &ei, mobility, BLACK);

  // Evaluate passed pawns, we need full attack information including king
  score +=  evaluate_passed(pos, &ei, WHITE)
          - evaluate_passed(pos, &ei, BLACK);

  if (lazy_skip(LazyThreshold2))
    goto make_v;

  // Evaluate tactical threats, we need full attack information including king
  score +=  evaluate_threats(pos, &ei, WHITE)
          - evaluate_threats(pos, &ei, BLACK);

  // Evaluate space for both sides, only during opening
  score +=  evaluate_space(pos, &ei, WHITE)
          - evaluate_space(pos, &ei, BLACK);

make_v:
  // Derive single value from the mg and eg parts of the score
  v = evaluate_winnable(pos, &ei, score);

  // Evaluation grain
  v = (v / 16) * 16;

  // Side to move point of view
  v = (stm() == WHITE ? v : -v) + Tempo;

  return v;
}

#ifdef NNUE
int useNNUE;

// fix_FRC() corrects for cornered bishops to fix FRC with NNUE.
static Value fix_FRC(const Position *pos)
{
  if (!(pieces_p(BISHOP) & 0x8100000000000081ULL))
    return 0;

  Value v = 0;

  if (piece_on(SQ_A1) == W_BISHOP && piece_on(SQ_B2) == W_PAWN)
    v += !is_empty(SQ_B3) ? -CorneredBishopV * 4
                          : -CorneredBishopV * 3;
  if (piece_on(SQ_H1) == W_BISHOP && piece_on(SQ_G2) == W_PAWN)
    v += !is_empty(SQ_G3) ? -CorneredBishopV * 4
                          : -CorneredBishopV * 3;
  if (piece_on(SQ_A8) == B_BISHOP && piece_on(SQ_B7) == B_PAWN)
    v += !is_empty(SQ_B6) ? CorneredBishopV * 4
                          : CorneredBishopV * 3;
  if (piece_on(SQ_H8) == B_BISHOP && piece_on(SQ_G7) == B_PAWN)
    v += !is_empty(SQ_G6) ? CorneredBishopV * 4
                          : CorneredBishopV * 3;

  return stm() == WHITE ? v : -v;
}

#define adjusted_NNUE() \
  (nnue_evaluate(pos) * (580 + mat / 32 - 4 * rule50_count()) / 1024 \
   + Time.tempoNNUE + (is_chess960() ? fix_FRC(pos) : 0))

#endif

Value evaluate(const Position *pos)
{
  Value v;

#ifdef NNUE

  const int mat = non_pawn_material() + 4 * PawnValueMg * popcount(pieces_p(PAWN));
  if (useNNUE == EVAL_HYBRID) {
    Value psq = abs(eg_value(psq_score()));
    int r50 = 16 + rule50_count();
    bool largePsq = psq * 16 > (NNUEThreshold1 + non_pawn_material() / 64) * r50;
    bool classical = largePsq || (psq > PawnValueMg / 4 && !(pos->nodes & 0x0B));

    bool lowPieceEndgame =   non_pawn_material() == BishopValueMg
                          || (non_pawn_material() < 2 * RookValueMg
                              && popcount(pieces_p(PAWN)) < 2);
    v = classical || lowPieceEndgame ? evaluate_classical(pos)
                                     : adjusted_NNUE();

    if (   classical && largePsq && !lowPieceEndgame
        && (   abs(v) * 16 < NNUEThreshold2 * r50
            || (   opposite_bishops(pos)
                && abs(v) * 16 < (NNUEThreshold1 + non_pawn_material() / 64) * r50
                && !(pos->nodes & 0xB))))
      v = adjusted_NNUE();

  } else if (useNNUE == EVAL_PURE)
    v = adjusted_NNUE();
  else
    v = evaluate_classical(pos);

#else

  v = evaluate_classical(pos);

#endif

  // Damp down the evalation linearly when shuffling
  v = v * (100 - rule50_count()) / 100;

  return clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

#else /* NNUE_PURE */

Value evaluate(const Position *pos)
{
  Value v;
  int mat = non_pawn_material() + 4 * PawnValueMg * popcount(pieces_p(PAWN));

  v = adjusted_NNUE();
  v = v * (100 - rule50_count()) / 100;
  return clamp(v, VALUE_TB_LOSS_IN_MAX_PLY + 1, VALUE_TB_WIN_IN_MAX_PLY - 1);
}

#endif

/* ==========================================
   FILE: thread.c
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2017 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/



static void thread_idle_loop(Position *pos);

#ifndef _WIN32
#define THREAD_FUNC void *
#else
#define THREAD_FUNC DWORD WINAPI
#endif

// Global objects
ThreadPool Threads;
MainThread mainThread;
CounterMoveHistoryStat **cmhTables = NULL;
int numCmhTables = 0;

// thread_init() is where a search thread starts and initialises itself.

static THREAD_FUNC thread_init(void *arg)
{
  int idx = (intptr_t)arg;

  int node;
  if (settings.numaEnabled)
    node = bind_thread_to_numa_node(idx);
  else
    node = 0;
#ifdef PER_THREAD_CMH
  (void)node;
  int t = idx;
#else
  int t = node;
#endif
  if (t >= numCmhTables) {
    int old = numCmhTables;
    numCmhTables = t + 16;
    cmhTables = realloc(cmhTables,
        numCmhTables * sizeof(CounterMoveHistoryStat *));
    while (old < numCmhTables)
      cmhTables[old++] = NULL;
  }
  if (!cmhTables[t]) {
    if (settings.numaEnabled)
      cmhTables[t] = numa_alloc(sizeof(CounterMoveHistoryStat));
    else
      cmhTables[t] = calloc(1, sizeof(CounterMoveHistoryStat));
    for (int chk = 0; chk < 2; chk++)
      for (int c = 0; c < 2; c++)
        for (int j = 0; j < 16; j++)
          for (int k = 0; k < 64; k++)
            (*cmhTables[t])[chk][c][0][0][j][k] = CounterMovePruneThreshold - 1;
  }

  Position *pos;

  if (settings.numaEnabled) {
    pos = numa_alloc(sizeof(Position));
#ifndef NNUE_PURE
    pos->pawnTable = numa_alloc(PAWN_ENTRIES * sizeof(PawnEntry));
    pos->materialTable = numa_alloc(8192 * sizeof(MaterialEntry));
#endif
    pos->counterMoves = numa_alloc(sizeof(CounterMoveStat));
    pos->mainHistory = numa_alloc(sizeof(ButterflyHistory));
    pos->captureHistory = numa_alloc(sizeof(CapturePieceToHistory));
    pos->lowPlyHistory = numa_alloc(sizeof(LowPlyHistory));
    pos->rootMoves = numa_alloc(sizeof(RootMoves));
    pos->stackAllocation = numa_alloc(63 + (MAX_PLY + 110) * sizeof(Stack));
    pos->moveList = numa_alloc(10000 * sizeof(ExtMove));
  } else {
    pos = calloc(1, sizeof(Position));
#ifndef NNUE_PURE
    pos->pawnTable = calloc(PAWN_ENTRIES,  sizeof(PawnEntry));
    pos->materialTable = calloc(8192, sizeof(MaterialEntry));
#endif
    pos->counterMoves = calloc(1, sizeof(CounterMoveStat));
    pos->mainHistory = calloc(1, sizeof(ButterflyHistory));
    pos->captureHistory = calloc(1, sizeof(CapturePieceToHistory));
    pos->lowPlyHistory = calloc(1, sizeof(LowPlyHistory));
    pos->rootMoves = calloc(1, sizeof(RootMoves));
    pos->stackAllocation = calloc(63 + (MAX_PLY + 110), sizeof(Stack));
    pos->moveList = calloc(10000, sizeof(ExtMove));
  }
  pos->stack = (Stack *)(((uintptr_t)pos->stackAllocation + 0x3f) & ~0x3f);
  pos->threadIdx = idx;
  pos->counterMoveHistory = cmhTables[t];

  atomic_store(&pos->resetCalls, false);
  pos->selDepth = pos->callsCnt = 0;

#ifndef _WIN32  // linux

  pthread_mutex_init(&pos->mutex, NULL);
  pthread_cond_init(&pos->sleepCondition, NULL);

  Threads.pos[idx] = pos;

  pthread_mutex_lock(&Threads.mutex);
  Threads.initializing = false;
  pthread_cond_signal(&Threads.sleepCondition);
  pthread_mutex_unlock(&Threads.mutex);

#else // Windows

  pos->startEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
  pos->stopEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

  Threads.pos[idx] = pos;

  SetEvent(Threads.event);

#endif

  thread_idle_loop(pos);

  return 0;
}

// thread_create() launches a new thread.

static void thread_create(int idx)
{
#ifndef _WIN32

  pthread_t thread;

  Threads.initializing = true;
  pthread_mutex_lock(&Threads.mutex);
  pthread_create(&thread, NULL, thread_init, (void *)(intptr_t)idx);
  while (Threads.initializing)
    pthread_cond_wait(&Threads.sleepCondition, &Threads.mutex);
  pthread_mutex_unlock(&Threads.mutex);

#else

  HANDLE thread = CreateThread(NULL, 0, thread_init, (void *)(intptr_t)idx,
      0 , NULL);
  WaitForSingleObject(Threads.event, INFINITE);

#endif

  Threads.pos[idx]->nativeThread = thread;
}


// thread_destroy() waits for thread termination before returning.

static void thread_destroy(Position *pos)
{
#ifndef _WIN32
  pthread_mutex_lock(&pos->mutex);
  pos->action = THREAD_EXIT;
  pthread_cond_signal(&pos->sleepCondition);
  pthread_mutex_unlock(&pos->mutex);
  pthread_join(pos->nativeThread, NULL);
  pthread_cond_destroy(&pos->sleepCondition);
  pthread_mutex_destroy(&pos->mutex);
#else
  pos->action = THREAD_EXIT;
  SetEvent(pos->startEvent);
  WaitForSingleObject(pos->nativeThread, INFINITE);
  CloseHandle(pos->startEvent);
  CloseHandle(pos->stopEvent);
#endif

  if (settings.numaEnabled) {
#ifndef NNUE_PURE
    numa_free(pos->pawnTable, PAWN_ENTRIES * sizeof(PawnEntry));
    numa_free(pos->materialTable, 8192 * sizeof(MaterialEntry));
#endif
    numa_free(pos->counterMoves, sizeof(CounterMoveStat));
    numa_free(pos->mainHistory, sizeof(ButterflyHistory));
    numa_free(pos->captureHistory, sizeof(CapturePieceToHistory));
    numa_free(pos->lowPlyHistory, sizeof(LowPlyHistory));
    numa_free(pos->rootMoves, sizeof(RootMoves));
    numa_free(pos->stackAllocation, 63 + (MAX_PLY + 110) * sizeof(Stack));
    numa_free(pos->moveList, 10000 * sizeof(ExtMove));
    numa_free(pos, sizeof(Position));
  } else {
#ifndef NNUE_PURE
    free(pos->pawnTable);
    free(pos->materialTable);
#endif
    free(pos->counterMoves);
    free(pos->mainHistory);
    free(pos->captureHistory);
    free(pos->lowPlyHistory);
    free(pos->rootMoves);
    free(pos->stackAllocation);
    free(pos->moveList);
    free(pos);
  }
}


// thread_wait_for_search_finished() waits on sleep condition until
// not searching.

void thread_wait_until_sleeping(Position *pos)
{
#ifndef _WIN32

  pthread_mutex_lock(&pos->mutex);

  while (pos->action != THREAD_SLEEP)
    pthread_cond_wait(&pos->sleepCondition, &pos->mutex);

  pthread_mutex_unlock(&pos->mutex);

#else

  WaitForSingleObject(pos->stopEvent, INFINITE);

#endif

  if (pos->threadIdx == 0)
    Threads.searching = false;
}


// thread_wait() waits on sleep condition until condition is true.

void thread_wait(Position *pos, atomic_bool *condition)
{
#ifndef _WIN32

  pthread_mutex_lock(&pos->mutex);

  while (!atomic_load(condition))
    pthread_cond_wait(&pos->sleepCondition, &pos->mutex);

  pthread_mutex_unlock(&pos->mutex);

#else

  (void)condition;
  WaitForSingleObject(pos->startEvent, INFINITE);

#endif
}


void thread_wake_up(Position *pos, int action)
{
#ifndef _WIN32

  pthread_mutex_lock(&pos->mutex);

#endif

  if (action != THREAD_RESUME)
    pos->action = action;

#ifndef _WIN32

  pthread_cond_signal(&pos->sleepCondition);
  pthread_mutex_unlock(&pos->mutex);

#else

  SetEvent(pos->startEvent);

#endif
}


// thread_idle_loop() is where the thread is parked when it has no work to do.

static void thread_idle_loop(Position *pos)
{
  while (true) {
#ifndef _WIN32

    pthread_mutex_lock(&pos->mutex);

    while (pos->action == THREAD_SLEEP) {
      pthread_cond_signal(&pos->sleepCondition); // Wake up any waiting thread
      pthread_cond_wait(&pos->sleepCondition, &pos->mutex);
    }

    pthread_mutex_unlock(&pos->mutex);

#else

    WaitForSingleObject(pos->startEvent, INFINITE);

#endif

    if (pos->action == THREAD_EXIT) {

      break;

    } else if (pos->action == THREAD_TT_CLEAR) {

      tt_clear_worker(pos->threadIdx);

    } else {

      if (pos->threadIdx == 0)
        mainthread_search();
      else
        thread_search(pos);

    }

    pos->action = THREAD_SLEEP;

#ifdef _WIN32

    SetEvent(pos->stopEvent);

#endif
  }
}


// threads_init() creates and launches requested threads that will go
// immediately to sleep. We cannot use a constructor because Threads is a
// static object and we need a fully initialized engine at this point due to
// allocation of Endgames in the Thread constructor.

void threads_init(void)
{
#ifndef _WIN32

  pthread_mutex_init(&Threads.mutex, NULL);
  pthread_cond_init(&Threads.sleepCondition, NULL);

#else

  Threads.event = CreateEvent(NULL, FALSE, FALSE, NULL);

#endif

#ifdef NUMA

  numa_init();

#endif

  Threads.numThreads = 1;
  thread_create(0);
}


// threads_exit() terminates threads before the program exits. Cannot be
// done in destructor because threads must be terminated before deleting
// any static objects while still in main().

void threads_exit(void)
{
  threads_set_number(0);

#ifndef _WIN32

  pthread_cond_destroy(&Threads.sleepCondition);
  pthread_mutex_destroy(&Threads.mutex);

#else

  CloseHandle(Threads.event);

#endif

#ifdef NUMA

  numa_exit();

#endif
}


// threads_set_number() creates/destroys threads to match the requested
// number.

void threads_set_number(int num)
{
  while (Threads.numThreads < num)
    thread_create(Threads.numThreads++);

  while (Threads.numThreads > num)
    thread_destroy(Threads.pos[--Threads.numThreads]);

  search_init();

  if (num == 0 && numCmhTables > 0) {
    for (int i = 0; i < numCmhTables; i++)
      if (cmhTables[i]) {
        if (settings.numaEnabled)
          numa_free(cmhTables[i], sizeof(CounterMoveHistoryStat));
        else
          free(cmhTables[i]);
      }
    free(cmhTables);
    cmhTables = NULL;
    numCmhTables = 0;
  }

  if (num == 0)
    Threads.searching = false;
}


// threads_nodes_searched() returns the number of nodes searched.

uint64_t threads_nodes_searched(void)
{
  uint64_t nodes = 0;
  for (int idx = 0; idx < Threads.numThreads; idx++)
    nodes += Threads.pos[idx]->nodes;
  return nodes;
}


// threads_tb_hits() returns the number of TB hits.

uint64_t threads_tb_hits(void)
{
  uint64_t hits = 0;
  for (int idx = 0; idx < Threads.numThreads; idx++)
    hits += Threads.pos[idx]->tbHits;
  return hits;
}

/* ==========================================
   FILE: timeman.c
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/



struct TimeManagement Time; // Our global time management struct

// tm_init() is called at the beginning of the search and calculates the
// time bounds allowed for the current game ply. We currently support:
// 1) x basetime (+z increment)
// 2) x moves in y seconds (+z increment)

void time_init(Color us, int ply)
{
  int moveOverhead    = option_value(OPT_MOVE_OVERHEAD);
  int slowMover       = option_value(OPT_SLOW_MOVER);
  int npmsec          = option_value(OPT_NODES_TIME);

  // optScale is a percentage of available time to use for the current move.
  // maxScale is a multiplier applied to optimumTime.
  double optScale, maxScale;

  // If we have to play in 'nodes as time' mode, then convert from time
  // to nodes, and use resulting values in time management formulas.
  // WARNING: Given npms (nodes per millisecond) must be much lower then
  // the real engine speed to avoid time losses.
  if (npmsec) {
    if (!Time.availableNodes) // Only once at game start
      Time.availableNodes = npmsec * Limits.time[us]; // Time is in msec

    // Convert from millisecs to nodes
    Limits.time[us] = (int)Time.availableNodes;
    Limits.inc[us] *= npmsec;
    Limits.npmsec = npmsec;
  }

  Time.startTime = Limits.startTime;

  // Maximum move horizon of 50 moves
  int mtg = Limits.movestogo ? min(Limits.movestogo, 50) : 50;

  // Make sure that timeLeft > 0 since we may use it as a divisor
  TimePoint timeLeft = max(1, Limits.time[us] + Limits.inc[us] * (mtg - 1) - moveOverhead * (2 + mtg));

  // A user may scale time usage by setting UCI option "Slow Mover".
  // Default is 100 and changing this value will probably lose Elo.
  timeLeft = slowMover * timeLeft / 100;

  // x basetime (+z increment)
  // If there is a healthy increment, timeLeft can exceed actual available
  // game time for the current move, so also cap to 20% of available game time.
  if (Limits.movestogo == 0) {
    optScale = min(0.0084 + pow(ply + 3.0, 0.5) * 0.0042,
                    0.2 * Limits.time[us] / (double)timeLeft);
    maxScale = min(7.0, 4.0 + ply / 12.0);
  }
  // x moves in y seconds (+z increment)
  else {
    optScale = min((0.8 + ply / 120.0) / mtg,
                     0.8 * Limits.time[us] / (double)timeLeft);
    maxScale = min(6.3, 1.5 + 0.11 * mtg);
  }

  // Never use more than 80% of the available time for this move
  Time.optimumTime = optScale * timeLeft;
  Time.maximumTime = min(0.8 * Limits.time[us] - moveOverhead, maxScale * Time.optimumTime);

  if (use_time_management()) {
    int strength = log(max(1, (int)(Time.optimumTime * Threads.numThreads  / 10))) * 60;
    Time.tempoNNUE = clamp((strength + 264) / 24, 18, 30);
  } else
    Time.tempoNNUE = 28; // default for no time given

  if (option_value(OPT_PONDER))
    Time.optimumTime += Time.optimumTime / 4;
}

/* ==========================================
   FILE: tt.c
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _WIN32
#endif


TranspositionTable TT; // Our global transposition table

// tt_free() frees the allocated transposition table memory.

void tt_free(void)
{
  if (TT.table)
    free_memory(&TT.alloc);
  TT.table = NULL;
}


// tt_allocate() allocates the transposition table, measured in megabytes.

void tt_allocate(size_t mbSize)
{
  TT.clusterCount = mbSize * 1024 * 1024 / sizeof(Cluster);
  size_t size = TT.clusterCount * sizeof(Cluster);

  TT.table = NULL;
  if (settings.largePages) {
    TT.table = allocate_memory(size, true, &TT.alloc);
#if !defined(__linux__)
    if (!TT.table)
      printf("info string Unable to allocate large pages for the "
             "transposition table.\n");
    else
      printf("info string Transposition table allocated using large pages.\n");
    fflush(stdout);
#endif
  }
  if (!TT.table)
    TT.table = allocate_memory(size, false, &TT.alloc);
  if (!TT.table)
    goto failed;

  // Clear the TT table to page in the memory immediately. This avoids
  // an initial slow down during the first second or minutes of the search.
  tt_clear();
  return;

failed:
  fprintf(stderr, "Failed to allocate %"PRIu64"MB for "
                  "transposition table.\n", (uint64_t)mbSize);
  exit(EXIT_FAILURE);
}


// tt_clear() initialises the entire transposition table to zero.

void tt_clear(void)
{
  // We let search threads clear the table in parallel. In NUMA mode,
  // this has the beneficial effect of spreading the TT over all nodes.

  if (TT.table) {
    for (int idx = 0; idx < Threads.numThreads; idx++)
      thread_wake_up(Threads.pos[idx], THREAD_TT_CLEAR);
    for (int idx = 0; idx < Threads.numThreads; idx++)
      thread_wait_until_sleeping(Threads.pos[idx]);
  }
}

void tt_clear_worker(int idx)
{
  // Find out which part of the TT this thread should clear.
  // To each thread we assign a number of 2MB blocks.

  size_t total = TT.clusterCount * sizeof(Cluster);
  size_t slice = (total + Threads.numThreads - 1) / Threads.numThreads;
  size_t blocks = (slice + (2 * 1024 * 1024) - 1) / (2 * 1024 * 1024);
  size_t begin = idx * blocks * (2 * 1024 * 1024);
  size_t end = begin + blocks * (2 * 1024 * 1024);
  begin = min(begin, total);
  end = min(end, total);

  // Now clear that part
  memset((uint8_t *)TT.table + begin, 0, end - begin);
}


// tt_probe() looks up the current position in the transposition table.
// It returns true and a pointer to the TTEntry if the position is found.
// Otherwise, it returns false and a pointer to an empty or least valuable
// TTEntry to be replaced later. The replace value of an entry is
// calculated as its depth minus 8 times its relative age. TTEntry t1 is
// considered more valuable than TTEntry t2 if its replace value is greater
// than that of t2.

TTEntry *tt_probe(Key key, bool *found)
{
  TTEntry *tte = tt_first_entry(key);
  uint16_t key16 = key; // Use the low 16 bits as key inside the cluster

  for (int i = 0; i < ClusterSize; i++)
    if (tte[i].key16 == key16 || !tte[i].depth8) {
//      if ((tte[i].genBound8 & 0xF8) != TT.generation8 && tte[i].key16)
      tte[i].genBound8 = TT.generation8 | (tte[i].genBound8 & 0x7); // Refresh
      *found = tte[i].depth8;
      return &tte[i];
    }

  // Find an entry to be replaced according to the replacement strategy
  TTEntry *replace = tte;
  for (int i = 1; i < ClusterSize; i++)
    // Due to our packed storage format for generation and its cyclic
    // nature we add 263 (256 is the modulus plus 7 to keep the unrelated
    // lowest three bits from affecting the result) to calculate the entry
    // age correctly even after generation8 overflows into the next cycle.
    if ( replace->depth8 - ((263 + TT.generation8 - replace->genBound8) & 0xF8)
        >  tte[i].depth8 - ((263 + TT.generation8 -   tte[i].genBound8) & 0xF8))
      replace = &tte[i];

  *found = false;
  return replace;
}


// Returns an approximation of the hashtable occupation during a search. The
// hash is x permill full, as per UCI protocol.

int tt_hashfull(void)
{
  int cnt = 0;
  for (int i = 0; i < 1000 / ClusterSize; i++) {
    const TTEntry *tte = &TT.table[i].entry[0];
    for (int j = 0; j < ClusterSize; j++)
      cnt += tte[j].depth8 && (tte[j].genBound8 & 0xf8) == TT.generation8;
  }
  return cnt * 1000 / (ClusterSize * (1000 / ClusterSize));
}

/* ==========================================
   FILE: uci.c
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2020 The Stockfish developers

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/



extern void benchmark(Position *pos, char *str);

// FEN string of the initial position, normal chess
static const char StartFEN[] =
  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// position() is called when the engine receives the "position" UCI
// command. The function sets up the position described in the given FEN
// string ("fen") or the starting position ("startpos") and then makes
// the moves given in the following move list ("moves").

void position(Position *pos, char *str)
{
  char fen[128];
  char *moves;

  moves = strstr(str, "moves");
  if (moves) {
    if (moves > str) moves[-1] = 0;
    moves += 5;
  }

  if (strncmp(str, "fen", 3) == 0) {
    strncpy(fen, str + 4, 127);
    fen[127] = 0;
  } else if (strncmp(str, "startpos", 8) == 0)
    strcpy(fen, StartFEN);
  else
    return;

  pos->st = pos->stack + 100; // Start of circular buffer of 100 slots.
  pos_set(pos, fen, option_value(OPT_CHESS960));

  // Parse move list (if any).
  if (moves) {
    int ply = 0;

    for (moves = strtok(moves, " \t"); moves; moves = strtok(NULL, " \t")) {
      Move m = uci_to_move(pos, moves);
      if (!m) break;
      do_move(pos, m, gives_check(pos, pos->st, m));
      pos->gamePly++;
      // Roll over if we reach 100 plies.
      if (++ply == 100) {
        memcpy(pos->st - 100, pos->st, StateSize);
        pos->st -= 100;
        pos_set_check_info(pos);
        ply -= 100;
      }
    }

    // Make sure that is_draw() never tries to look back more than 99 ply.
    // This is enough, since 100 ply history means draw by 50-move rule.
    if (pos->st->pliesFromNull > 99)
      pos->st->pliesFromNull = 99;

    // Now move some of the game history at the end of the circular buffer
    // in front of that buffer.
    int k = (pos->st - (pos->stack + 100)) - max(7, pos->st->pliesFromNull);
    for (; k < 0; k++)
      memcpy(pos->stack + 100 + k, pos->stack + 200 + k, StateSize);
  }

  pos->rootKeyFlip = pos->st->key;
  (pos->st-1)->endMoves = pos->moveList;

  // Clear history position keys that have not yet repeated. This ensures
  // that is_draw() does not flag as a draw the first repetition of a
  // position coming before the root position. In addition, we set
  // pos->hasRepeated to indicate whether a position has repeated since
  // the last irreversible move.
  for (int k = 0; k <= pos->st->pliesFromNull; k++) {
    int l;
    for (l = k + 4; l <= pos->st->pliesFromNull; l += 2)
      if ((pos->st - k)->key == (pos->st - l)->key)
        break;
    if (l <= pos->st->pliesFromNull)
      pos->hasRepeated = true;
    else
      (pos->st - k)->key = 0;
  }
  pos->rootKeyFlip ^= pos->st->key;
  pos->st->key ^= pos->rootKeyFlip;
}


// setoption() is called when the engine receives the "setoption" UCI
// command. The function updates the UCI option ("name") to the given
// value ("value").

void setoption(char *str)
{
  char *name, *value;

  name = strstr(str, "name");
  if (!name) {
    name = "";
    goto error;
  }

  name += 4;
  while (isblank(*name))
    name++;

  value = strstr(name, "value");
  if (value) {
    char *p = value - 1;
    while (isblank(*p))
      p--;
    p[1] = 0;
    value += 5;
    while (isblank(*value))
      value++;
  }
  if (!value || strlen(value) == 0)
    value = "<empty>";

  if (option_set_by_name(name, value))
    return;

error:
  fprintf(stderr, "No such option: %s\n", name);
}


// go() is called when engine receives the "go" UCI command. The function sets
// the thinking time and other parameters from the input string, then starts
// the search.

static void go(Position *pos, char *str)
{
  char *token;
  bool ponderMode = false;

  process_delayed_settings();

  Limits = (struct LimitsType){ 0 };
  Limits.startTime = now(); // As early as possible!

  for (token = strtok(str, " \t"); token; token = strtok(NULL, " \t")) {
    if (strcmp(token, "searchmoves") == 0)
      while ((token = strtok(NULL, " \t")))
        Limits.searchmoves[Limits.numSearchmoves++] = uci_to_move(pos, token);
    else if (strcmp(token, "wtime") == 0)
      Limits.time[WHITE] = atoi(strtok(NULL, " \t"));
    else if (strcmp(token, "btime") == 0)
      Limits.time[BLACK] = atoi(strtok(NULL, " \t"));
    else if (strcmp(token, "winc") == 0)
      Limits.inc[WHITE] = atoi(strtok(NULL, " \t"));
    else if (strcmp(token, "binc") == 0)
      Limits.inc[BLACK] = atoi(strtok(NULL, " \t"));
    else if (strcmp(token, "movestogo") == 0)
      Limits.movestogo = atoi(strtok(NULL, " \t"));
    else if (strcmp(token, "depth") == 0)
      Limits.depth = atoi(strtok(NULL, " \t"));
    else if (strcmp(token, "nodes") == 0)
      Limits.nodes = strtoull(strtok(NULL, " \t"), NULL, 10);
    else if (strcmp(token, "movetime") == 0)
      Limits.movetime = atoi(strtok(NULL, " \t"));
    else if (strcmp(token, "mate") == 0)
      Limits.mate = atoi(strtok(NULL, " \t"));
    else if (strcmp(token, "infinite") == 0)
      Limits.infinite = true;
    else if (strcmp(token, "ponder") == 0)
      ponderMode = true;
    else if (strcmp(token, "perft") == 0) {
      char str_buf[64];
      sprintf(str_buf, "%d %d %d current perft", option_value(OPT_HASH),
                    option_value(OPT_THREADS), atoi(strtok(NULL, " \t")));
      benchmark(pos, str_buf);
      return;
    }
  }

  start_thinking(pos, ponderMode);
}


// uci_loop() waits for a command from stdin, parses it and calls the
// appropriate function. Also intercepts EOF from stdin to ensure
// gracefully exiting if the GUI dies unexpectedly. When called with some
// command line arguments, e.g. to run 'bench', once the command is
// executed the function returns immediately. In addition to the UCI ones,
// also some additional debug commands are supported.

void uci_loop(int argc, char **argv)
{
  Position pos;
  char fen[strlen(StartFEN) + 1];
  char str_buf[64];
  char *token;

  LOCK_INIT(Threads.lock);

  // Threads.searching is only read and set by the UI thread.
  // The UI thread uses it to know whether it must still call
  // thread_wait_until_sleeping() on the main search thread.
  // (This is important for our native Windows threading implementation.)
  Threads.searching = false;

  // Threads.sleeping is set by the main search thread if it has run
  // out of work but must wait for a "stop" or "ponderhit" command from
  // the GUI to arrive before being allowed to output "bestmove". The main
  // thread will then go to sleep and has to be waken up by the UI thread.
  // This variable must be accessed only after acquiring Threads.lock.
  Threads.sleeping = false;

  // Allocate 215 Stack slots.
  // Slots 100-200 form a circular buffer to be filled with game moves.
  // Slots 0-99 make room for prepending the part of game history relevant
  // for repetition detection.
  // Slots 201-214 may be used by TB root probing.
  pos.stackAllocation = malloc(63 + 215 * sizeof(Stack));
  pos.stack = (Stack *)(((uintptr_t)pos.stackAllocation + 0x3f) & ~0x3f);
  pos.moveList = malloc(1000 * sizeof(ExtMove));
  pos.st = pos.stack + 100;
  pos.st[-1].endMoves = pos.moveList;

  size_t buf_size = 1;
  for (int i = 1; i < argc; i++)
    buf_size += strlen(argv[i]) + 1;

  if (buf_size < 1024) buf_size = 1024;

  char *cmd = malloc(buf_size);

  cmd[0] = 0;
  for (int i = 1; i < argc; i++) {
    strcat(cmd, argv[i]);
    strcat(cmd, " ");
  }

  strcpy(fen, StartFEN);
  pos_set(&pos, fen, 0);
  pos.rootKeyFlip = pos.st->key;

  do {
    if (argc == 1 && !getline(&cmd, &buf_size, stdin))
      strcpy(cmd, "quit");

    if (cmd[strlen(cmd) - 1] == '\n')
      cmd[strlen(cmd) - 1] = 0;

    token = cmd;
    while (isblank(*token))
      token++;

    char *str = token;
    while (*str && !isblank(*str))
      str++;

    if (*str) {
      *str++ = 0;
      while (isblank(*str))
        str++;
    }

    // The GUI sends 'ponderhit' to tell us the player has played the
    // expected move. In case Threads.stopOnPonderhit is set we are waiting
    // for 'ponderhit' to stop the search (for instance because we have
    // already searched long enough), otherwise we should continue searching
    // but switch from pondering to normal search.
    if (strcmp(token, "quit") == 0 || strcmp(token, "stop") == 0) {
      if (Threads.searching) {
        Threads.stop = true;
        LOCK(Threads.lock);
        if (Threads.sleeping)
          thread_wake_up(threads_main(), THREAD_RESUME);
        Threads.sleeping = false;
        UNLOCK(Threads.lock);
      }
    }
    else if (strcmp(token, "ponderhit") == 0) {
      Threads.ponder = false; // Switch to normal search
      if (Threads.stopOnPonderhit)
        Threads.stop = true;
      LOCK(Threads.lock);
      if (Threads.sleeping) {
        Threads.stop = true;
        thread_wake_up(threads_main(), THREAD_RESUME);
        Threads.sleeping = false;
      }
      UNLOCK(Threads.lock);
    }
    else if (strcmp(token, "uci") == 0) {
      flockfile(stdout);
      printf("id name ");
      print_engine_info(true);
      printf("\n");
      print_options();
      printf("uciok\n");
      fflush(stdout);
      funlockfile(stdout);
    }
    else if (strcmp(token, "ucinewgame") == 0) {
      process_delayed_settings();
      search_clear();
    } else if (strcmp(token, "isready") == 0) {
      process_delayed_settings();
      printf("readyok\n");
      fflush(stdout);
    }
    else if (strcmp(token, "go") == 0)        go(&pos, str);
    else if (strcmp(token, "position") == 0)  position(&pos, str);
    else if (strcmp(token, "setoption") == 0) setoption(str);

    // Additional custom non-UCI commands, useful for debugging
    else if (strcmp(token, "bench") == 0)     benchmark(&pos, str);
    else if (strcmp(token, "d") == 0)         print_pos(&pos);
    else if (strcmp(token, "perft") == 0) {
      sprintf(str_buf, "%d %d %d current perft", option_value(OPT_HASH),
                    option_value(OPT_THREADS), atoi(str));
      benchmark(&pos, str_buf);
    }
    else if (strcmp(token, "compiler") == 0)  print_compiler_info();
    else if (strcmp(token, "export_net") == 0) nnue_export_net();
    else if (strncmp(token, "#", 1)) {
      printf("Unknown command: %s %s\n", token, str);
      fflush(stdout);
    }
  } while (argc == 1 && strcmp(token, "quit") != 0);

  if (Threads.searching)
    thread_wait_until_sleeping(threads_main());

  free(cmd);
  free(pos.stackAllocation);
  free(pos.moveList);

  LOCK_DESTROY(Threads.lock);
}


// uci_value() converts a Value to a string suitable for use with the UCI
// protocol specification:
//
// cp <x>    The score from the engine's point of view in centipawns.
// mate <y>  Mate in y moves, not plies. If the engine is getting mated
//           use negative values for y.

char *uci_value(char *str, Value v)
{
  if (abs(v) < VALUE_MATE_IN_MAX_PLY)
    sprintf(str, "cp %d", v * 100 / PawnValueEg);
  else
    sprintf(str, "mate %d",
                 (v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2);

  return str;
}


// uci_square() converts a Square to a string in algebraic notation
// (g1, a7, etc.)

char *uci_square(char *str, Square s)
{
  str[0] = 'a' + file_of(s);
  str[1] = '1' + rank_of(s);
  str[2] = 0;

  return str;
}


// uci_move() converts a Move to a string in coordinate notation (g1f3,
// a7a8q). The only special case is castling, where we print in the e1g1
// notation in normal chess mode, and in e1h1 notation in chess960 mode.
// Internally all castling moves are always encoded as 'king captures rook'.

char *uci_move(char *str, Move m, int chess960)
{
  char buf1[8], buf2[8];
  Square from = from_sq(m);
  Square to = to_sq(m);

  if (m == 0)
    return "(none)";

  if (m == MOVE_NULL)
    return "0000";

  if (type_of_m(m) == CASTLING && !chess960)
    to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));

  strcat(strcpy(str, uci_square(buf1, from)), uci_square(buf2, to));

  if (type_of_m(m) == PROMOTION) {
    str[strlen(str) + 1] = 0;
    str[strlen(str)] = " pnbrqk"[promotion_type(m)];
  }

  return str;
}


// uci_to_move() converts a string representing a move in coordinate
// notation (g1f3, a7a8q) to the corresponding legal Move, if any.

Move uci_to_move(const Position *pos, char *str)
{
  if (strlen(str) == 5) // Junior could send promotion piece in uppercase
    str[4] = tolower(str[4]);

  ExtMove list[MAX_MOVES];
  ExtMove *last = generate_legal(pos, list);

  char buf[16];

  for (ExtMove *m = list; m < last; m++)
    if (strcmp(str, uci_move(buf, m->move, pos->chess960)) == 0)
      return m->move;

  return 0;
}

/* ==========================================
   FILE: tbprobe.c
   ========================================== */

/*
  Copyright (c) 2013-2018 Ronald de Man
  This file may be redistributed and/or modified without restrictions.
*/



#define TB_PIECES 7
#define TB_HASHBITS  (TB_PIECES < 7 ?  11 : 12)
#define TB_MAX_PIECE (TB_PIECES < 7 ? 254 : 650)
#define TB_MAX_PAWN  (TB_PIECES < 7 ? 256 : 861)

#ifdef _WIN32
typedef HANDLE map_t;
#define SEP_CHAR ';'
#else
typedef size_t map_t;
#define SEP_CHAR ':'
#endif

int TB_MaxCardinality = 0, TB_MaxCardinalityDTM = 0;
extern int TB_CardinalityDTM;

static const char *tbSuffix[] = { ".rtbw", ".rtbm", ".rtbz" };
static uint32_t tbMagic[] = { 0x5d23e871, 0x88ac504b, 0xa50c66d7 };

enum { WDL, DTM, DTZ };
enum { PIECE_ENC, FILE_ENC, RANK_ENC };

struct PairsData {
  const uint8_t *indexTable;
  const uint16_t *sizeTable;
  const uint8_t *data;
  const uint16_t *offset;
  uint8_t *symLen;
  const uint8_t *symPat;
  uint8_t blockSize;
  uint8_t idxBits;
  uint8_t minLen;
  uint8_t constValue[2];
  uint64_t base[]; // must be base[1] in C++
};

struct EncInfo {
  struct PairsData *precomp;
  size_t factor[TB_PIECES];
  uint8_t pieces[TB_PIECES];
  uint8_t norm[TB_PIECES];
};

struct BaseEntry {
  Key key;
  const uint8_t *data[3];
  map_t mapping[3];
  atomic_bool ready[3];
  uint8_t num;
  bool symmetric, hasPawns, hasDtm, hasDtz;
  union {
    bool kk_enc;
    uint8_t pawns[2];
  };
  bool dtmLossOnly;
};

struct PieceEntry {
  struct BaseEntry be;
  struct EncInfo ei[5]; // 2 + 2 + 1
  uint16_t *dtmMap;
  uint16_t dtmMapIdx[2][2];
  const void *dtzMap;
  uint16_t dtzMapIdx[4];
  uint8_t dtzFlags;
};

struct PawnEntry {
  struct BaseEntry be;
  struct EncInfo ei[24]; // 4 * 2 + 6 * 2 + 4
  uint16_t *dtmMap;
  uint16_t dtmMapIdx[6][2][2];
  const void *dtzMap;
  uint16_t dtzMapIdx[4][4];
  uint8_t dtzFlags[4];
  bool dtmSwitched;
};

struct TbHashEntry {
  Key key;
  struct BaseEntry *ptr;
};

static LOCK_T tbMutex;
static int initialized = 0;
static int numPaths = 0;
static char *pathString = NULL;
static char **paths = NULL;

static int tbNumPiece, tbNumPawn;
static int numWdl, numDtm, numDtz;

static struct PieceEntry *pieceEntry;
static struct PawnEntry *pawnEntry;
static struct TbHashEntry tbHash[1 << TB_HASHBITS];

static void init_indices(void);

// Given a position, produce a text string of the form KQPvKRP, where
// "KQP" represents the white pieces if flip == false and the black pieces
// if flip == true.
static void prt_str(Position *pos, char *str, bool flip)
{
  Color color = !flip ? WHITE : BLACK;

  for (int pt = KING; pt >= PAWN; pt--)
    for (int i = popcount(pieces_cp(color, pt)); i > 0; i--)
      *str++ = PieceToChar[pt];
  *str++ = 'v';
  color = !color;
  for (int pt = KING; pt >= PAWN; pt--)
    for (int i = popcount(pieces_cp(color, pt)); i > 0; i--)
      *str++ = PieceToChar[pt];
  *str++ = 0;
}

// Produce a 64-bit material key corresponding to the material combination
// defined by pcs[16], where pcs[1], ..., pcs[6] are the number of white
// pawns, ..., kings and pcs[9], ..., pcs[14] are the number of black
// pawns, ..., kings.
static Key calc_key_from_pcs(int *pcs, bool flip)
{
  Key key = 0;

  int color = !flip ? 0 : 8;
  for (int i = W_PAWN; i <= B_KING; i++)
    key += matKey[i] * pcs[i ^ color];

  return key;
}

// Produce a 64-bit material key corresponding to the material combination
// piece[0], ..., piece[num - 1], where each value corresponds to a piece
// (1-6 for white pawn-king, 9-14 for black pawn-king).
static Key calc_key_from_pieces(uint8_t *piece, int num)
{
  Key key = 0;

  for (int i = 0; i < num; i++)
    if (piece[i])
      key += matKey[piece[i]];

  return key;
}

static FD open_tb(const char *str, const char *suffix)
{
  char name[256];

  for (int i = 0; i < numPaths; i++) {
    strcpy(name, paths[i]);
    strcat(name, "/");
    strcat(name, str);
    strcat(name, suffix);
    FD fd = open_file(name);
    if (fd != FD_ERR) return fd;
  }
  return FD_ERR;
}

static bool test_tb(const char *str, const char *suffix)
{
  FD fd = open_tb(str, suffix);
  if (fd != FD_ERR) {
    size_t size = file_size(fd);
    close_file(fd);
    if ((size & 63) != 16) {
      fprintf(stderr, "Incomplete tablebase file %s.%s\n", str, suffix);
      printf("info string Incomplete tablebase file %s.%s\n", str, suffix);
      fd = FD_ERR;
    }
  }
  return fd != FD_ERR;
}

static const void *map_tb(const char *name, const char *suffix, map_t *mapping)
{
  FD fd = open_tb(name, suffix);
  if (fd == FD_ERR)
    return NULL;

  const void *data = map_file(fd, mapping);
  if (data == NULL) {
    fprintf(stderr, "Could not map %s%s into memory.\n", name, suffix);
    exit(EXIT_FAILURE);
  }

  close_file(fd);

  return data;
}

static void add_to_hash(void *ptr, Key key)
{
  int idx;

  idx = key >> (64 - TB_HASHBITS);
  while (tbHash[idx].ptr)
    idx = (idx + 1) & ((1 << TB_HASHBITS) - 1);

  tbHash[idx].key = key;
  tbHash[idx].ptr = ptr;
}

#define pchr(i) PieceToChar[QUEEN - (i)]
#define Swap(a,b) {int tmp=a;a=b;b=tmp;}

static void init_tb(char *str)
{
  if (!test_tb(str, tbSuffix[WDL]))
    return;

  int pcs[16];
  for (int i = 0; i < 16; i++)
    pcs[i] = 0;
  int color = 0;
  for (char *s = str; *s; s++)
    if (*s == 'v')
      color = 8;
    else
      for (int i = PAWN; i <= KING; i++)
        if (*s == PieceToChar[i]) {
          pcs[i | color]++;
          break;
        }

  Key key = calc_key_from_pcs(pcs, false);
  Key key2 = calc_key_from_pcs(pcs, true);

  bool hasPawns = pcs[W_PAWN] || pcs[B_PAWN];

  struct BaseEntry *be = hasPawns ? &pawnEntry[tbNumPawn++].be
                                  : &pieceEntry[tbNumPiece++].be;
  be->hasPawns = hasPawns;
  be->key = key;
  be->symmetric = key == key2;
  be->num = 0;
  for (int i = 0; i < 16; i++)
    be->num += pcs[i];

  numWdl++;
  numDtm += be->hasDtm = test_tb(str, tbSuffix[DTM]);
  numDtz += be->hasDtz = test_tb(str, tbSuffix[DTZ]);

  TB_MaxCardinality = max(TB_MaxCardinality, be->num);
  if (be->hasDtm)
    TB_MaxCardinalityDTM = max(TB_MaxCardinalityDTM, be->num);

  for (int type = 0; type < 3; type++)
    atomic_init(&be->ready[type], false);

  if (!be->hasPawns) {
    int j = 0;
    for (int i = 0; i < 16; i++)
      if (pcs[i] == 1) j++;
    be->kk_enc = j == 2;
  } else {
    be->pawns[0] = pcs[W_PAWN];
    be->pawns[1] = pcs[B_PAWN];
    if (pcs[B_PAWN] && (!pcs[W_PAWN] || pcs[W_PAWN] > pcs[B_PAWN]))
      Swap(be->pawns[0], be->pawns[1]);
  }

  add_to_hash(be, key);
  if (key != key2)
    add_to_hash(be, key2);
}

#define PIECE(x) ((struct PieceEntry *)(x))
#define PAWN(x) ((struct PawnEntry *)(x))

INLINE int num_tables(struct BaseEntry *be, const int type)
{
  return be->hasPawns ? type == DTM ? 6 : 4 : 1;
}

INLINE struct EncInfo *first_ei(struct BaseEntry *be, const int type)
{
  return  be->hasPawns
        ? &PAWN(be)->ei[type == WDL ? 0 : type == DTM ? 8 : 20]
        : &PIECE(be)->ei[type == WDL ? 0 : type == DTM ? 2 : 4];
}

static void free_tb_entry(struct BaseEntry *be)
{
  for (int type = 0; type < 3; type++) {
    if (atomic_load_explicit(&be->ready[type], memory_order_relaxed)) {
      unmap_file(be->data[type], be->mapping[type]);
      int num = num_tables(be, type);
      struct EncInfo *ei = first_ei(be, type);
      for (int t = 0; t < num; t++) {
        free(ei[t].precomp);
        if (type != DTZ)
          free(ei[num + t].precomp);
      }
      atomic_store_explicit(&be->ready[type], false, memory_order_relaxed);
    }
  }
}

void TB_free(void)
{
  TB_init("");
  free(pieceEntry);
  free(pawnEntry);
}

void TB_release(void)
{
  for (int i = 0; i < tbNumPiece; i++)
    free_tb_entry((struct BaseEntry *)&pieceEntry[i]);
  for (int i = 0; i < tbNumPawn; i++)
    free_tb_entry((struct BaseEntry *)&pawnEntry[i]);
}

void TB_init(char *path)
{
  if (!initialized) {
    init_indices();
    initialized = 1;
  }

  // if pathString is set, we need to clean up first.
  if (pathString) {
    free(pathString);
    free(paths);

    TB_release();

    LOCK_DESTROY(tbMutex);

    pathString = NULL;
  }

  numWdl = numDtm = numDtz = 0;
  tbNumPiece = tbNumPawn = 0;
  TB_MaxCardinality = TB_MaxCardinalityDTM = 0;

  // if path is an empty string or equals "<empty>", we are done.
  const char *p = path;
  if (strlen(p) == 0 || !strcmp(p, "<empty>")) return;

  pathString = strdup(p);
  numPaths = 0;
  for (int i = 0;; i++) {
    if (pathString[i] != SEP_CHAR)
      numPaths++;
    while (pathString[i] && pathString[i] != SEP_CHAR)
      i++;
    if (!pathString[i]) break;
    pathString[i] = 0;
  }
  paths = malloc(numPaths * sizeof(*paths));
  for (int i = 0, j = 0; i < numPaths; i++) {
    while (!pathString[j]) j++;
    paths[i] = &pathString[j];
    while (pathString[j]) j++;
  }

  LOCK_INIT(tbMutex);

  if (!pieceEntry) {
    pieceEntry = malloc(TB_MAX_PIECE * sizeof(*pieceEntry));
    pawnEntry = malloc(TB_MAX_PAWN * sizeof(*pawnEntry));
    if (!pieceEntry || !pawnEntry) {
      fprintf(stderr, "Out of memory.\n");
      exit(EXIT_FAILURE);
    }
  }

  for (int i = 0; i < (1 << TB_HASHBITS); i++)
    tbHash[i] = (struct TbHashEntry){ 0 };

  char str[16];
  int i, j, k, l, m;

  for (i = 0; i < 5; i++) {
    sprintf(str, "K%cvK", pchr(i));
    init_tb(str);
  }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++) {
      sprintf(str, "K%cvK%c", pchr(i), pchr(j));
      init_tb(str);
    }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++) {
      sprintf(str, "K%c%cvK", pchr(i), pchr(j));
      init_tb(str);
    }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = 0; k < 5; k++) {
        sprintf(str, "K%c%cvK%c", pchr(i), pchr(j), pchr(k));
        init_tb(str);
      }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = j; k < 5; k++) {
        sprintf(str, "K%c%c%cvK", pchr(i), pchr(j), pchr(k));
        init_tb(str);
      }

  // 6- and 7-piece TBs make sense only with a 64-bit address space
  if (sizeof(size_t) < 8 || TB_PIECES < 6)
    goto finished;

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = i; k < 5; k++)
        for (l = (i == k) ? j : k; l < 5; l++) {
          sprintf(str, "K%c%cvK%c%c", pchr(i), pchr(j), pchr(k), pchr(l));
          init_tb(str);
        }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = j; k < 5; k++)
        for (l = 0; l < 5; l++) {
          sprintf(str, "K%c%c%cvK%c", pchr(i), pchr(j), pchr(k), pchr(l));
          init_tb(str);
        }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = j; k < 5; k++)
        for (l = k; l < 5; l++) {
          sprintf(str, "K%c%c%c%cvK", pchr(i), pchr(j), pchr(k), pchr(l));
          init_tb(str);
        }

  if (TB_PIECES < 7)
    goto finished;

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = j; k < 5; k++)
        for (l = k; l < 5; l++)
          for (m = l; m < 5; m++) {
            sprintf(str, "K%c%c%c%c%cvK", pchr(i), pchr(j), pchr(k), pchr(l), pchr(m));
            init_tb(str);
          }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = j; k < 5; k++)
        for (l = k; l < 5; l++)
          for (m = 0; m < 5; m++) {
            sprintf(str, "K%c%c%c%cvK%c", pchr(i), pchr(j), pchr(k), pchr(l), pchr(m));
            init_tb(str);
          }

  for (i = 0; i < 5; i++)
    for (j = i; j < 5; j++)
      for (k = j; k < 5; k++)
        for (l = 0; l < 5; l++)
          for (m = l; m < 5; m++) {
            sprintf(str, "K%c%c%cvK%c%c", pchr(i), pchr(j), pchr(k), pchr(l), pchr(m));
            init_tb(str);
          }

finished:
  printf("info string Found %d WDL, %d DTM and %d DTZ tablebase files.\n",
      numWdl, numDtm, numDtz);
  fflush(stdout);
}

static const int8_t OffDiag[] = {
  0,-1,-1,-1,-1,-1,-1,-1,
  1, 0,-1,-1,-1,-1,-1,-1,
  1, 1, 0,-1,-1,-1,-1,-1,
  1, 1, 1, 0,-1,-1,-1,-1,
  1, 1, 1, 1, 0,-1,-1,-1,
  1, 1, 1, 1, 1, 0,-1,-1,
  1, 1, 1, 1, 1, 1, 0,-1,
  1, 1, 1, 1, 1, 1, 1, 0
};

static const uint8_t Triangle[] = {
  6, 0, 1, 2, 2, 1, 0, 6,
  0, 7, 3, 4, 4, 3, 7, 0,
  1, 3, 8, 5, 5, 8, 3, 1,
  2, 4, 5, 9, 9, 5, 4, 2,
  2, 4, 5, 9, 9, 5, 4, 2,
  1, 3, 8, 5, 5, 8, 3, 1,
  0, 7, 3, 4, 4, 3, 7, 0,
  6, 0, 1, 2, 2, 1, 0, 6
};

static const uint8_t FlipDiag[] = {
   0,  8, 16, 24, 32, 40, 48, 56,
   1,  9, 17, 25, 33, 41, 49, 57,
   2, 10, 18, 26, 34, 42, 50, 58,
   3, 11, 19, 27, 35, 43, 51, 59,
   4, 12, 20, 28, 36, 44, 52, 60,
   5, 13, 21, 29, 37, 45, 53, 61,
   6, 14, 22, 30, 38, 46, 54, 62,
   7, 15, 23, 31, 39, 47, 55, 63
};

static const uint8_t Lower[] = {
  28,  0,  1,  2,  3,  4,  5,  6,
   0, 29,  7,  8,  9, 10, 11, 12,
   1,  7, 30, 13, 14, 15, 16, 17,
   2,  8, 13, 31, 18, 19, 20, 21,
   3,  9, 14, 18, 32, 22, 23, 24,
   4, 10, 15, 19, 22, 33, 25, 26,
   5, 11, 16, 20, 23, 25, 34, 27,
   6, 12, 17, 21, 24, 26, 27, 35
};

static const uint8_t Diag[] = {
   0,  0,  0,  0,  0,  0,  0,  8,
   0,  1,  0,  0,  0,  0,  9,  0,
   0,  0,  2,  0,  0, 10,  0,  0,
   0,  0,  0,  3, 11,  0,  0,  0,
   0,  0,  0, 12,  4,  0,  0,  0,
   0,  0, 13,  0,  0,  5,  0,  0,
   0, 14,  0,  0,  0,  0,  6,  0,
  15,  0,  0,  0,  0,  0,  0,  7
};

static const uint8_t Flap[2][64] = {
  {  0,  0,  0,  0,  0,  0,  0,  0,
     0,  6, 12, 18, 18, 12,  6,  0,
     1,  7, 13, 19, 19, 13,  7,  1,
     2,  8, 14, 20, 20, 14,  8,  2,
     3,  9, 15, 21, 21, 15,  9,  3,
     4, 10, 16, 22, 22, 16, 10,  4,
     5, 11, 17, 23, 23, 17, 11,  5,
     0,  0,  0,  0,  0,  0,  0,  0  },
  {  0,  0,  0,  0,  0,  0,  0,  0,
     0,  1,  2,  3,  3,  2,  1,  0,
     4,  5,  6,  7,  7,  6,  5,  4,
     8,  9, 10, 11, 11, 10,  9,  8,
    12, 13, 14, 15, 15, 14, 13, 12,
    16, 17, 18, 19, 19, 18, 17, 16,
    20, 21, 22, 23, 23, 22, 21, 20,
     0,  0,  0,  0,  0,  0,  0,  0  }
};

static const uint8_t PawnTwist[2][64] = {
  {  0,  0,  0,  0,  0,  0,  0,  0,
    47, 35, 23, 11, 10, 22, 34, 46,
    45, 33, 21,  9,  8, 20, 32, 44,
    43, 31, 19,  7,  6, 18, 30, 42,
    41, 29, 17,  5,  4, 16, 28, 40,
    39, 27, 15,  3,  2, 14, 26, 38,
    37, 25, 13,  1,  0, 12, 24, 36,
     0,  0,  0,  0,  0,  0,  0,  0 },
  {  0,  0,  0,  0,  0,  0,  0,  0,
    47, 45, 43, 41, 40, 42, 44, 46,
    39, 37, 35, 33, 32, 34, 36, 38,
    31, 29, 27, 25, 24, 26, 28, 30,
    23, 21, 19, 17, 16, 18, 20, 22,
    15, 13, 11,  9,  8, 10, 12, 14,
     7,  5,  3,  1,  0,  2,  4,  6,
     0,  0,  0,  0,  0,  0,  0,  0 }
};

static const int16_t KKIdx[10][64] = {
  { -1, -1, -1,  0,  1,  2,  3,  4,
    -1, -1, -1,  5,  6,  7,  8,  9,
    10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 29, 30, 31, 32, 33,
    34, 35, 36, 37, 38, 39, 40, 41,
    42, 43, 44, 45, 46, 47, 48, 49,
    50, 51, 52, 53, 54, 55, 56, 57 },
  { 58, -1, -1, -1, 59, 60, 61, 62,
    63, -1, -1, -1, 64, 65, 66, 67,
    68, 69, 70, 71, 72, 73, 74, 75,
    76, 77, 78, 79, 80, 81, 82, 83,
    84, 85, 86, 87, 88, 89, 90, 91,
    92, 93, 94, 95, 96, 97, 98, 99,
   100,101,102,103,104,105,106,107,
   108,109,110,111,112,113,114,115},
  {116,117, -1, -1, -1,118,119,120,
   121,122, -1, -1, -1,123,124,125,
   126,127,128,129,130,131,132,133,
   134,135,136,137,138,139,140,141,
   142,143,144,145,146,147,148,149,
   150,151,152,153,154,155,156,157,
   158,159,160,161,162,163,164,165,
   166,167,168,169,170,171,172,173 },
  {174, -1, -1, -1,175,176,177,178,
   179, -1, -1, -1,180,181,182,183,
   184, -1, -1, -1,185,186,187,188,
   189,190,191,192,193,194,195,196,
   197,198,199,200,201,202,203,204,
   205,206,207,208,209,210,211,212,
   213,214,215,216,217,218,219,220,
   221,222,223,224,225,226,227,228 },
  {229,230, -1, -1, -1,231,232,233,
   234,235, -1, -1, -1,236,237,238,
   239,240, -1, -1, -1,241,242,243,
   244,245,246,247,248,249,250,251,
   252,253,254,255,256,257,258,259,
   260,261,262,263,264,265,266,267,
   268,269,270,271,272,273,274,275,
   276,277,278,279,280,281,282,283 },
  {284,285,286,287,288,289,290,291,
   292,293, -1, -1, -1,294,295,296,
   297,298, -1, -1, -1,299,300,301,
   302,303, -1, -1, -1,304,305,306,
   307,308,309,310,311,312,313,314,
   315,316,317,318,319,320,321,322,
   323,324,325,326,327,328,329,330,
   331,332,333,334,335,336,337,338 },
  { -1, -1,339,340,341,342,343,344,
    -1, -1,345,346,347,348,349,350,
    -1, -1,441,351,352,353,354,355,
    -1, -1, -1,442,356,357,358,359,
    -1, -1, -1, -1,443,360,361,362,
    -1, -1, -1, -1, -1,444,363,364,
    -1, -1, -1, -1, -1, -1,445,365,
    -1, -1, -1, -1, -1, -1, -1,446 },
  { -1, -1, -1,366,367,368,369,370,
    -1, -1, -1,371,372,373,374,375,
    -1, -1, -1,376,377,378,379,380,
    -1, -1, -1,447,381,382,383,384,
    -1, -1, -1, -1,448,385,386,387,
    -1, -1, -1, -1, -1,449,388,389,
    -1, -1, -1, -1, -1, -1,450,390,
    -1, -1, -1, -1, -1, -1, -1,451 },
  {452,391,392,393,394,395,396,397,
    -1, -1, -1, -1,398,399,400,401,
    -1, -1, -1, -1,402,403,404,405,
    -1, -1, -1, -1,406,407,408,409,
    -1, -1, -1, -1,453,410,411,412,
    -1, -1, -1, -1, -1,454,413,414,
    -1, -1, -1, -1, -1, -1,455,415,
    -1, -1, -1, -1, -1, -1, -1,456 },
  {457,416,417,418,419,420,421,422,
    -1,458,423,424,425,426,427,428,
    -1, -1, -1, -1, -1,429,430,431,
    -1, -1, -1, -1, -1,432,433,434,
    -1, -1, -1, -1, -1,435,436,437,
    -1, -1, -1, -1, -1,459,438,439,
    -1, -1, -1, -1, -1, -1,460,440,
    -1, -1, -1, -1, -1, -1, -1,461 }
};

static const uint8_t FileToFile[] = { 0, 1, 2, 3, 3, 2, 1, 0 };
static const int WdlToMap[5] = { 1, 3, 0, 2, 0 };
static const uint8_t PAFlags[5] = { 8, 0, 0, 0, 4 };

static size_t Binomial[7][64];
static size_t PawnIdx[2][6][24];
static size_t PawnFactorFile[6][4];
static size_t PawnFactorRank[6][6];

static void init_indices(void)
{
  int i, j;

  // Binomial[k][n] = Bin(n, k)
  for (j = 0; j < 64; j++)
    Binomial[0][j] = 1;
  for (i = 1; i < 7; i++)
    for (j = 1; j < 64; j++)
      Binomial[i][j] = Binomial[i - 1][j - 1] + Binomial[i][j - 1];

  for (i = 0; i < 6; i++) {
    size_t s = 0;
    for (j = 0; j < 24; j++) {
      PawnIdx[0][i][j] = s;
      s += Binomial[i][PawnTwist[0][(1 + (j % 6)) * 8 + (j / 6)]];
      if ((j + 1) % 6 == 0) {
        PawnFactorFile[i][j / 6] = s;
        s = 0;
      }
    }
  }

  for (i = 0; i < 6; i++) {
    size_t s = 0;
    for (j = 0; j < 24; j++) {
      PawnIdx[1][i][j] = s;
      s += Binomial[i][PawnTwist[1][(1 + (j / 4)) * 8 + (j % 4)]];
      if ((j + 1) % 4 == 0) {
        PawnFactorRank[i][j / 4] = s;
        s = 0;
      }
    }
  }
}

INLINE int leading_pawn(int *p, struct BaseEntry *be, const int enc)
{
  for (int i = 1; i < be->pawns[0]; i++)
    if (Flap[enc-1][p[0]] > Flap[enc-1][p[i]])
      Swap(p[0], p[i]);

  return enc == FILE_ENC ? FileToFile[p[0] & 7] : (p[0] - 8) >> 3;
}

INLINE size_t encode(int *p, struct EncInfo *ei, struct BaseEntry *be,
    const int enc)
{
  int n = be->num;
  size_t idx;
  int k;

  if (p[0] & 0x04)
    for (int i = 0; i < n; i++)
      p[i] ^= 0x07;

  if (enc == PIECE_ENC) {
    if (p[0] & 0x20)
      for (int i = 0; i < n; i++)
        p[i] ^= 0x38;

    for (int i = 0; i < n; i++)
      if (OffDiag[p[i]]) {
        if (OffDiag[p[i]] > 0 && i < (be->kk_enc ? 2 : 3))
          for (int j = 0; j < n; j++)
            p[j] = FlipDiag[p[j]];
        break;
      }

    if (be->kk_enc) {
      idx = KKIdx[Triangle[p[0]]][p[1]];
      k = 2;
    } else {
      int s1 = (p[1] > p[0]);
      int s2 = (p[2] > p[0]) + (p[2] > p[1]);

      if (OffDiag[p[0]])
        idx = Triangle[p[0]] * 63*62 + (p[1] - s1) * 62 + (p[2] - s2);
      else if (OffDiag[p[1]])
        idx = 6*63*62 + Diag[p[0]] * 28*62 + Lower[p[1]] * 62 + p[2] - s2;
      else if (OffDiag[p[2]])
        idx = 6*63*62 + 4*28*62 + Diag[p[0]] * 7*28 + (Diag[p[1]] - s1) * 28 + Lower[p[2]];
      else
        idx = 6*63*62 + 4*28*62 + 4*7*28 + Diag[p[0]] * 7*6 + (Diag[p[1]] - s1) * 6 + (Diag[p[2]] - s2);
      k = 3;
    }
    idx *= ei->factor[0];
  } else {
    for (int i = 1; i < be->pawns[0]; i++)
      for (int j = i + 1; j < be->pawns[0]; j++)
        if (PawnTwist[enc-1][p[i]] < PawnTwist[enc-1][p[j]])
          Swap(p[i], p[j]);

    k = be->pawns[0];
    idx = PawnIdx[enc-1][k-1][Flap[enc-1][p[0]]];
    for (int i = 1; i < k; i++)
      idx += Binomial[k-i][PawnTwist[enc-1][p[i]]];
    idx *= ei->factor[0];

    // Pawns of other color
    if (be->pawns[1]) {
      int t = k + be->pawns[1];
      for (int i = k; i < t; i++)
        for (int j = i + 1; j < t; j++)
          if (p[i] > p[j]) Swap(p[i], p[j]);
      size_t s = 0;
      for (int i = k; i < t; i++) {
        int sq = p[i];
        int skips = 0;
        for (int j = 0; j < k; j++)
          skips += (sq > p[j]);
        s += Binomial[i - k + 1][sq - skips - 8];
      }
      idx += s * ei->factor[k];
      k = t;
    }
  }

  for (; k < n;) {
    int t = k + ei->norm[k];
    for (int i = k; i < t; i++)
      for (int j = i + 1; j < t; j++)
        if (p[i] > p[j]) Swap(p[i], p[j]);
    size_t s = 0;
    for (int i = k; i < t; i++) {
      int sq = p[i];
      int skips = 0;
      for (int j = 0; j < k; j++)
        skips += (sq > p[j]);
      s += Binomial[i - k + 1][sq - skips];
    }
    idx += s * ei->factor[k];
    k = t;
  }

  return idx;
}

static NOINLINE size_t encode_piece(int *p, struct EncInfo *ei,
    struct BaseEntry *be)
{
  return encode(p, ei, be, PIECE_ENC);
}

static NOINLINE size_t encode_pawn_f(int *p, struct EncInfo *ei,
    struct BaseEntry *be)
{
  return encode(p, ei, be, FILE_ENC);
}

static NOINLINE size_t encode_pawn_r(int *p, struct EncInfo *ei,
    struct BaseEntry *be)
{
  return encode(p, ei, be, RANK_ENC);
}

// Count number of placements of k like pieces on n squares
static size_t subfactor(size_t k, size_t n)
{
  size_t f = n;
  size_t l = 1;
  for (size_t i = 1; i < k; i++) {
    f *= n - i;
    l *= i + 1;
  }

  return f / l;
}

static size_t init_enc_info(struct EncInfo *ei, struct BaseEntry *be,
    const uint8_t *tb, int shift, int t, const int enc)
{
  bool morePawns = enc != PIECE_ENC && be->pawns[1] > 0;

  for (int i = 0; i < be->num; i++) {
    ei->pieces[i] = (tb[i + 1 + morePawns] >> shift) & 0x0f;
    ei->norm[i] = 0;
  }

  int order = (tb[0] >> shift) & 0x0f;
  int order2 = morePawns ? (tb[1] >> shift) & 0x0f : 0x0f;

  int k = ei->norm[0] =  enc != PIECE_ENC ? be->pawns[0]
                       : be->kk_enc ? 2 : 3;

  if (morePawns) {
    ei->norm[k] = be->pawns[1];
    k += ei->norm[k];
  }

  for (int i = k; i < be->num; i += ei->norm[i])
    for (int j = i; j < be->num && ei->pieces[j] == ei->pieces[i]; j++)
      ei->norm[i]++;

  int n = 64 - k;
  size_t f = 1;

  for (int i = 0; k < be->num || i == order || i == order2; i++) {
    if (i == order) {
      ei->factor[0] = f;
      f *=  enc == FILE_ENC ? PawnFactorFile[ei->norm[0] - 1][t]
          : enc == RANK_ENC ? PawnFactorRank[ei->norm[0] - 1][t]
          : be->kk_enc ? 462 : 31332;
    } else if (i == order2) {
      ei->factor[ei->norm[0]] = f;
      f *= subfactor(ei->norm[ei->norm[0]], 48 - ei->norm[0]);
    } else {
      ei->factor[k] = f;
      f *= subfactor(ei->norm[k], n);
      n -= ei->norm[k];
      k += ei->norm[k];
    }
  }

  return f;
}

static void calc_symLen(struct PairsData *d, uint32_t s, char *tmp)
{
  const uint8_t *w = d->symPat + 3 * s;
  uint32_t s2 = (w[2] << 4) | (w[1] >> 4);
  if (s2 == 0x0fff)
    d->symLen[s] = 0;
  else {
    uint32_t s1 = ((w[1] & 0xf) << 8) | w[0];
    if (!tmp[s1]) calc_symLen(d, s1, tmp);
    if (!tmp[s2]) calc_symLen(d, s2, tmp);
    d->symLen[s] = d->symLen[s1] + d->symLen[s2] + 1;
  }
  tmp[s] = 1;
}

static struct PairsData *setup_pairs(const uint8_t **ptr, size_t tb_size,
    size_t *size, uint8_t *flags, int type)
{
  struct PairsData *d;
  const uint8_t *data = *ptr;

  *flags = data[0];
  if (data[0] & 0x80) {
    d = malloc(sizeof(*d));
    d->idxBits = 0;
    d->constValue[0] = type == WDL ? data[1] : 0;
    d->constValue[1] = 0;
    *ptr = data + 2;
    size[0] = size[1] = size[2] = 0;
    return d;
  }

  uint8_t blockSize = data[1];
  uint8_t idxBits = data[2];
  uint32_t realNumBlocks = read_le_u32(&data[4]);
  uint32_t numBlocks = realNumBlocks + data[3];
  int maxLen = data[8];
  int minLen = data[9];
  int h = maxLen - minLen + 1;
  uint32_t numSyms = read_le_u16(&data[10 + 2 * h]);
  d = malloc(sizeof(*d) + h * sizeof(uint64_t) + numSyms);
  d->blockSize = blockSize;
  d->idxBits = idxBits;
  d->offset = (uint16_t *)&data[10];
  d->symLen = (uint8_t *)d + sizeof(*d) + h * sizeof(uint64_t);
  d->symPat = &data[12 + 2 * h];
  d->minLen = minLen;
  *ptr = &data[12 + 2 * h + 3 * numSyms + (numSyms & 1)];

  size_t num_indices = (tb_size + (1ULL << idxBits) - 1) >> idxBits;
  size[0] = 6ULL * num_indices;
  size[1] = 2ULL * numBlocks;
  size[2] = (size_t)realNumBlocks << blockSize;

  char tmp[numSyms];
  memset(tmp, 0, numSyms);
  for (uint32_t s = 0; s < numSyms; s++)
    if (!tmp[s])
      calc_symLen(d, s, tmp);

  d->base[h - 1] = 0;
  for (int i = h - 2; i >= 0; i--)
    d->base[i] = (d->base[i + 1] + read_le_u16((uint8_t *)(d->offset + i)) - read_le_u16((uint8_t *)(d->offset + i + 1))) / 2;
  for (int i = 0; i < h; i++)
    d->base[i] <<= 64 - (minLen + i);

  d->offset -= d->minLen;

  return d;
}

static NOINLINE bool init_table(struct BaseEntry *be, const char *str, int type)
{
  const uint8_t *data = map_tb(str, tbSuffix[type], &be->mapping[type]);
  if (!data) return false;

  if (read_le_u32(data) != tbMagic[type]) {
    fprintf(stderr, "Corrupted table.\n");
    unmap_file(data, be->mapping[type]);
    return false;
  }

  be->data[type] = data;

  bool split = type != DTZ && (data[4] & 0x01);
  if (type == DTM)
    be->dtmLossOnly = data[4] & 0x04;

  data += 5;

  size_t tb_size[6][2];
  int num = num_tables(be, type);
  struct EncInfo *ei = first_ei(be, type);
  int enc = !be->hasPawns ? PIECE_ENC : type != DTM ? FILE_ENC : RANK_ENC;

  for (int t = 0; t < num; t++) {
    tb_size[t][0] = init_enc_info(&ei[t], be, data, 0, t, enc);
    if (split)
      tb_size[t][1] = init_enc_info(&ei[num + t], be, data, 4, t, enc);
    data += be->num + 1 + (be->hasPawns && be->pawns[1]);
  }
  data += (uintptr_t)data & 1;

  size_t size[6][2][3];
  for (int t = 0; t < num; t++) {
    uint8_t flags;
    ei[t].precomp = setup_pairs(&data, tb_size[t][0], size[t][0], &flags, type);
    if (type == DTZ) {
      if (!be->hasPawns)
        PIECE(be)->dtzFlags = flags;
      else
        PAWN(be)->dtzFlags[t] = flags;
    }
    if (split)
      ei[num + t].precomp = setup_pairs(&data, tb_size[t][1], size[t][1], &flags, type);
    else if (type != DTZ)
      ei[num + t].precomp = NULL;
  }

  if (type == DTM && !be->dtmLossOnly) {
    uint16_t *map = (uint16_t *)data;
    *(be->hasPawns ? &PAWN(be)->dtmMap : &PIECE(be)->dtmMap) = map;
    uint16_t (*mapIdx)[2][2] = be->hasPawns ? &PAWN(be)->dtmMapIdx[0]
                                             : &PIECE(be)->dtmMapIdx;
    for (int t = 0; t < num; t++) {
      for (int i = 0; i < 2; i++) {
        mapIdx[t][0][i] = (uint16_t *)data + 1 - map;
        data += 2 + 2 * read_le_u16(data);
      }
      if (split) {
        for (int i = 0; i < 2; i++) {
          mapIdx[t][1][i] = (uint16_t *)data + 1 - map;
          data += 2 + 2 * read_le_u16(data);
        }
      }
    }
  }

  if (type == DTZ) {
    const void *map = data;
    *(be->hasPawns ? &PAWN(be)->dtzMap : &PIECE(be)->dtzMap) = map;
    uint16_t (*mapIdx)[4] = be->hasPawns ? &PAWN(be)->dtzMapIdx[0]
                                          : &PIECE(be)->dtzMapIdx;
    uint8_t *flags = be->hasPawns ? &PAWN(be)->dtzFlags[0]
                                  : &PIECE(be)->dtzFlags;
    for (int t = 0; t < num; t++) {
      if (flags[t] & 2) {
        if (!(flags[t] & 16)) {
          for (int i = 0; i < 4; i++) {
            mapIdx[t][i] = data + 1 - (uint8_t *)map;
            data += 1 + data[0];
          }
        } else {
          data += (uintptr_t)data & 0x01;
          for (int i = 0; i < 4; i++) {
            mapIdx[t][i] = (uint16_t *)data + 1 - (uint16_t *)map;
            data += 2 + 2 * read_le_u16(data);
          }
        }
      }
    }
    data += (uintptr_t)data & 0x01;
  }

  for (int t = 0; t < num; t++) {
    ei[t].precomp->indexTable = data;
    data += size[t][0][0];
    if (split) {
      ei[num + t].precomp->indexTable = data;
      data += size[t][1][0];
    }
  }

  for (int t = 0; t < num; t++) {
    ei[t].precomp->sizeTable = (uint16_t *)data;
    data += size[t][0][1];
    if (split) {
      ei[num + t].precomp->sizeTable = (uint16_t *)data;
      data += size[t][1][1];
    }
  }

  for (int t = 0; t < num; t++) {
    data = (uint8_t *)(((uintptr_t)data + 0x3f) & ~0x3f);
    ei[t].precomp->data = data;
    data += size[t][0][2];
    if (split) {
      data = (uint8_t *)(((uintptr_t)data + 0x3f) & ~0x3f);
      ei[num + t].precomp->data = data;
      data += size[t][1][2];
    }
  }

  if (type == DTM && be->hasPawns)
    PAWN(be)->dtmSwitched =
      calc_key_from_pieces(ei[0].pieces, be->num) != be->key;

  return true;
}

static const uint8_t *decompress_pairs(struct PairsData *d, size_t idx)
{
  if (!d->idxBits)
    return d->constValue;

  uint32_t mainIdx = idx >> d->idxBits;
  int litIdx = (idx & (((size_t)1 << d->idxBits) - 1)) - ((size_t)1 << (d->idxBits - 1));
  uint32_t block;
  memcpy(&block, d->indexTable + 6 * mainIdx, sizeof(block));
  block = from_le_u32(block);

  uint16_t idxOffset = *(uint16_t *)(d->indexTable + 6 * mainIdx + 4);
  litIdx += from_le_u16(idxOffset);

  if (litIdx < 0)
    while (litIdx < 0)
      litIdx += d->sizeTable[--block] + 1;
  else
    while (litIdx > d->sizeTable[block])
      litIdx -= d->sizeTable[block++] + 1;

  uint32_t *ptr = (uint32_t *)(d->data + ((size_t)block << d->blockSize));

  int m = d->minLen;
  const uint16_t *offset = d->offset;
  uint64_t *base = d->base - m;
  uint8_t *symLen = d->symLen;
  uint32_t sym, bitCnt;

  uint64_t code = from_be_u64(*(uint64_t *)ptr);

  ptr += 2;
  bitCnt = 0; // number of "empty bits" in code
  for (;;) {
    int l = m;
    while (code < base[l]) l++;
    sym = from_le_u16(offset[l]);
    sym += (code - base[l]) >> (64 - l);
    if (litIdx < (int)symLen[sym] + 1) break;
    litIdx -= (int)symLen[sym] + 1;
    code <<= l;
    bitCnt += l;
    if (bitCnt >= 32) {
      bitCnt -= 32;
      uint32_t tmp = from_be_u32(*ptr++);
      code |= (uint64_t)tmp << bitCnt;
    }
  }

  const uint8_t *symPat = d->symPat;
  while (symLen[sym] != 0) {
    const uint8_t *w = symPat + (3 * sym);
    int s1 = ((w[1] & 0xf) << 8) | w[0];
    if (litIdx < (int)symLen[s1] + 1)
      sym = s1;
    else {
      litIdx -= (int)symLen[s1] + 1;
      sym = (w[2] << 4) | (w[1] >> 4);
    }
  }

  return &symPat[3 * sym];
}

// p[i] is to contain the square 0-63 (A1-H8) for a piece of type
// pc[i] ^ flip, where 1 = white pawn, ..., 14 = black king and pc ^ flip
// flips between white and black if flip == true.
// Pieces of the same type are guaranteed to be consecutive.
INLINE int fill_squares(Position *pos, uint8_t *pc, bool flip, int mirror, int *p,
    int i)
{
  Bitboard bb = pieces_cp((pc[i] >> 3) ^ flip, pc[i] & 7);
  do {
    p[i++] = pop_lsb(&bb) ^ mirror;
  } while (bb);
  return i;
}

INLINE int probe_table(Position *pos, int s, int *success, const int type)
{
  // Obtain the position's material-signature key
  Key key = material_key();

  // Test for KvK
  if (type == WDL && key == 2ULL)
    return 0;

  int hashIdx = key >> (64 - TB_HASHBITS);
  while (tbHash[hashIdx].key && tbHash[hashIdx].key != key)
    hashIdx = (hashIdx + 1) & ((1 << TB_HASHBITS) - 1);
  if (!tbHash[hashIdx].ptr) {
    *success = 0;
    return 0;
  }

  struct BaseEntry *be = tbHash[hashIdx].ptr;
  if ((type == DTM && !be->hasDtm) || (type == DTZ && !be->hasDtz)) {
    *success = 0;
    return 0;
  }

  // Use double-checked locking to reduce locking overhead
  if (!atomic_load_explicit(&be->ready[type], memory_order_acquire)) {
    LOCK(tbMutex);
    if (!atomic_load_explicit(&be->ready[type], memory_order_relaxed)) {
      char str[16];
      prt_str(pos, str, be->key != key);
      if (!init_table(be, str, type)) {
        tbHash[hashIdx].ptr = NULL; // mark as deleted
        *success = 0;
        UNLOCK(tbMutex);
        return 0;
      }
      atomic_store_explicit(&be->ready[type], true, memory_order_release);
    }
    UNLOCK(tbMutex);
  }

  bool bside, flip;
  if (!be->symmetric) {
    flip = key != be->key;
    bside = (stm() == WHITE) == flip;
    if (type == DTM && be->hasPawns && PAWN(be)->dtmSwitched) {
      flip = !flip;
      bside = !bside;
    }
  } else {
    flip = stm() != WHITE;
    bside = false;
  }

  struct EncInfo *ei = first_ei(be, type);
  int p[TB_PIECES];
  size_t idx;
  int t = 0;
  uint8_t flags;

  if (!be->hasPawns) {
    if (type == DTZ) {
      flags = PIECE(be)->dtzFlags;
      if ((flags & 1) != bside && !be->symmetric) {
        *success = -1;
        return 0;
      }
    }
    ei = type != DTZ ? &ei[bside] : ei;
    for (int i = 0; i < be->num;)
      i = fill_squares(pos, ei->pieces, flip, 0, p, i);
    idx = encode_piece(p, ei, be);
  } else {
    int i = fill_squares(pos, ei->pieces, flip, flip ? 0x38 : 0, p, 0);
    t = leading_pawn(p, be, type != DTM ? FILE_ENC : RANK_ENC);
    if (type == DTZ) {
      flags = PAWN(be)->dtzFlags[t];
      if ((flags & 1) != bside && !be->symmetric) {
        *success = -1;
        return 0;
      }
    }
    ei =  type == WDL ? &ei[t + 4 * bside]
        : type == DTM ? &ei[t + 6 * bside] : &ei[t];
    while (i < be->num)
      i = fill_squares(pos, ei->pieces, flip, flip ? 0x38 : 0, p, i);
    idx = type != DTM ? encode_pawn_f(p, ei, be) : encode_pawn_r(p, ei, be);
  }

  const uint8_t *w = decompress_pairs(ei->precomp, idx);

  if (type == WDL)
    return (int)w[0] - 2;

  int v = w[0] + ((w[1] & 0x0f) << 8);

  if (type == DTM) {
    if (!be->dtmLossOnly)
      v =  from_le_u16(be->hasPawns
         ? PAWN(be)->dtmMap[PAWN(be)->dtmMapIdx[t][bside][s] + v]
         : PIECE(be)->dtmMap[PIECE(be)->dtmMapIdx[bside][s] + v]);
  } else {
    if (flags & 2) {
      int m = WdlToMap[s + 2];
      if (!(flags & 16))
        v =  be->hasPawns
           ? ((uint8_t *)PAWN(be)->dtzMap)[PAWN(be)->dtzMapIdx[t][m] + v]
           : ((uint8_t *)PIECE(be)->dtzMap)[PIECE(be)->dtzMapIdx[m] + v];
      else
        v =  from_le_u16(be->hasPawns
           ? ((uint16_t *)PAWN(be)->dtzMap)[PAWN(be)->dtzMapIdx[t][m] + v]
           : ((uint16_t *)PIECE(be)->dtzMap)[PIECE(be)->dtzMapIdx[m] + v]);
    }
    if (!(flags & PAFlags[s + 2]) || (s & 1))
      v *= 2;
  }

  return v;
}

static NOINLINE int probe_wdl_table(Position *pos, int *success)
{
  return probe_table(pos, 0, success, WDL);
}

static NOINLINE int probe_dtm_table(Position *pos, int won, int *success)
{
  return probe_table(pos, won, success, DTM);
}

static NOINLINE int probe_dtz_table(Position *pos, int wdl, int *success)
{
  return probe_table(pos, wdl, success, DTZ);
}

// Add missing underpromotion captures to list of captures.
// generate_captures() generates all queen promotions and knight promotions
// that give check.
static ExtMove *add_underprom_caps(Position *pos, ExtMove *m, ExtMove *end)
{
  ExtMove *extra = end;

  for (; m < end; m++) {
    Move move = m->move;
    if (   type_of_m(move) == PROMOTION
        && promotion_type(move) == QUEEN
        && piece_on(to_sq(move)))
    {
      (*extra++).move = (Move)(move - (1 << 12)); // ROOK
      (*extra++).move = (Move)(move - (2 << 12)); // BISHOP
      // Skip knight promotion if it was already generated
      if (m+1 == end || from_to((m+1)->move) != from_to(move))
        (*extra++).move = (Move)(move - (3 << 12)); // KNIGHT
    }
  }

  return extra;
}

// probe_ab() is not called for positions with en passant captures.
static int probe_ab(Position *pos, int alpha, int beta, int *success)
{
  assert(ep_square() == 0);

  // Generate (at least) all legal captures including (under)promotions.
  // It is OK to generate more, as long as they are filtered out below.
  ExtMove *m = (pos->st-1)->endMoves;
  ExtMove *end = !checkers()
                ? add_underprom_caps(pos, m, generate_captures(pos, m))
                : generate_evasions(pos, m);
  pos->st->endMoves = end;

  for (; m < end; m++) {
    Move move = m->move;
    if (!is_capture(pos, move) || !is_legal(pos, move))
      continue;
    do_move(pos, move, gives_check(pos, pos->st, move));
    int v = -probe_ab(pos, -beta, -alpha, success);
    undo_move(pos, move);
    if (*success == 0) return 0;
    if (v > alpha) {
      if (v >= beta)
        return v;
      alpha = v;
    }
  }

  int v = probe_wdl_table(pos, success);

  return alpha >= v ? alpha : v;
}

// Probe the WDL table for a particular position.
//
// If *success != 0, the probe was successful.
//
// If *success == 2, the position has a winning capture, or the position
// is a cursed win and has a cursed winning capture, or the position
// has an ep capture as only best move.
// This is used in probe_dtz().
//
// The return value is from the point of view of the side to move:
// -2 : loss
// -1 : loss, but draw under 50-move rule
//  0 : draw
//  1 : win, but draw under 50-move rule
//  2 : win
int TB_probe_wdl(Position *pos, int *success)
{
  *success = 1;

  // Generate (at least) all legal captures including (under)promotions.
  ExtMove *m = (pos->st-1)->endMoves;
  ExtMove *end = !checkers()
                ? add_underprom_caps(pos, m, generate_captures(pos, m))
                : generate_evasions(pos, m);
  pos->st->endMoves = end;

  int bestCap = -3, bestEp = -3;

  // We do capture resolution, letting bestCap keep track of the best
  // capture without ep rights and letting bestEp keep track of still
  // better ep captures if they exist.

  for (; m < end; m++) {
    Move move = m->move;
    if (!is_capture(pos, move) || !is_legal(pos, move))
      continue;
    do_move(pos, move, gives_check(pos, pos->st, move));
    int v = -probe_ab(pos, -2, -bestCap, success);
    undo_move(pos, move);
    if (*success == 0) return 0;
    if (v > bestCap) {
      if (v == 2) {
        *success = 2;
        return 2;
      }
      if (type_of_m(move) != ENPASSANT)
        bestCap = v;
      else if (v > bestEp)
        bestEp = v;
    }
  }

  int v = probe_wdl_table(pos, success);
  if (*success == 0) return 0;

  // Now max(v, bestCap) is the WDL value of the position without ep rights.
  // If the position without ep rights is not stalemate or no ep captures
  // exist, then the value of the position is max(v, bestCap, bestEp).
  // If the position without ep rights is stalemate and bestEp > -3,
  // then the value of the position is bestEp (and we will have v == 0).

  if (bestEp > bestCap) {
    if (bestEp > v) { // ep capture (possibly cursed losing) is best.
      *success = 2;
      return bestEp;
    }
    bestCap = bestEp;
  }

  // Now max(v, bestCap) is the WDL value of the position unless
  // the position without ep rights is stalemate and bestEp > -3.

  if (bestCap >= v) {
    // No need to test for the stalemate case here: either there are
    // non-ep captures, or bestCap == bestEp >= v anyway.
    *success = 1 + (bestCap > 0);
    return bestCap;
  }

  // Now handle the stalemate case.
  if (bestEp > -3 && v == 0) {
    // Check for stalemate in the position with ep captures.
    for (m = (pos->st-1)->endMoves; m < end; m++) {
      Move move = m->move;
      if (type_of_m(move) == ENPASSANT) continue;
      if (is_legal(pos, move)) break;
    }
    if (m == end && !checkers()) {
      end = generate_quiets(pos, end);
      for (; m < end; m++) {
        Move move = m->move;
        if (is_legal(pos, move))
          break;
      }
    }
    if (m == end) { // Stalemate detected.
      *success = 2;
      return bestEp;
    }
  }

  // Stalemate / en passant not an issue, so v is the correct value.

  return v;
}

#if 0
// This will not be called for positions with en passant captures
static Value probe_dtm_dc(Position *pos, int won, int *success)
{
  assert(ep_square() == 0);

  Value v, bestCap = -VALUE_INFINITE;

  ExtMove *end, *m = (pos->st-1)->endMoves;

  // Generate at least all legal captures including (under)promotions
  if (!checkers()) {
    end = generate_captures(pos, m);
    end = add_underprom_caps(pos, m, end);
  } else
    end = generate_evasions(pos, m);
  pos->st->endMoves = end;

  for (; m < end; m++) {
    Move move = m->move;
    if (!is_capture(pos, move) || !is_legal(pos, move))
      continue;
    do_move(pos, move, gives_check(pos, pos->st, move));
    if (!won)
      v = -probe_dtm_dc(pos, 1, success) + 1;
    else if (probe_ab(pos, -1, 0, success) < 0 && *success)
      v = -probe_dtm_dc(pos, 0, success) - 1;
    else
      v = -VALUE_INFINITE;
    undo_move(pos, move);
    bestCap = max(bestCap, v);
    if (*success == 0) return 0;
  }

  int dtm = probe_dtm_table(pos, won, success);
  v = won ? VALUE_MATE - 2 * dtm + 1 : -VALUE_MATE + 2 * dtm;

  return max(bestCap, v);
}
#endif

static Value probe_dtm_win(Position *pos, int *success);

// Probe a position known to lose by probing the DTM table and looking
// at captures.
static Value probe_dtm_loss(Position *pos, int *success)
{
  Value v, best = -VALUE_INFINITE, numEp = 0;

  // Generate at least all legal captures including (under)promotions
  ExtMove *end, *m = (pos->st-1)->endMoves;
  end = checkers() ? generate_evasions(pos, m)
                   : add_underprom_caps(pos, m, generate_captures(pos, m));
  pos->st->endMoves = end;

  for (; m < end; m++) {
    Move move = m->move;
    if (!is_capture(pos, move) || !is_legal(pos, move))
      continue;
    if (type_of_m(move) == ENPASSANT)
      numEp++;
    do_move(pos, move, gives_check(pos, pos->st, move));
    v = -probe_dtm_win(pos, success) + 1;
    undo_move(pos, move);
    best = max(best, v);
    if (*success == 0)
      return 0;
  }

  // If there are en passant captures, the position without ep rights
  // may be a stalemate. If it is, we must avoid probing the DTM table.
  if (numEp != 0 && generate_legal(pos, m) == m + numEp)
    return best;

  v = -VALUE_MATE + 2 * probe_dtm_table(pos, 0, success);
  return max(best, v);
}

static Value probe_dtm_win(Position *pos, int *success)
{
  Value v, best = -VALUE_INFINITE;

  // Generate all moves
  ExtMove *m = (pos->st-1)->endMoves;
  ExtMove *end = checkers() ? generate_evasions(pos, m)
                            : generate_non_evasions(pos, m);
  pos->st->endMoves = end;

  // Perform a 1-ply search
  for (; m < end; m++) {
    Move move = m->move;
    if (!is_legal(pos, move))
      continue;
    do_move(pos, move, gives_check(pos, pos->st, move));
    if (   (ep_square() ? TB_probe_wdl(pos, success)
                        : probe_ab(pos, -1, 0, success)) < 0
        && *success)
      v = -probe_dtm_loss(pos, success) - 1;
    else
      v = -VALUE_INFINITE;
    undo_move(pos, move);
    best = max(best, v);
    if (*success == 0) return 0;
  }

  return best;
}

Value TB_probe_dtm(Position *pos, int wdl, int *success)
{
  assert(wdl != 0);

  *success = 1;

  return wdl > 0 ? probe_dtm_win(pos, success)
                 : probe_dtm_loss(pos, success);
}

#if 0
// To be called only for non-drawn positions.
Value TB_probe_dtm2(Position *pos, int wdl, int *success)
{
  assert(wdl != 0);

  *success = 1;
  Value v, bestCap = -VALUE_INFINITE, bestEp = -VALUE_INFINITE;

  ExtMove *end, *m = (pos->st-1)->endMoves;

  // Generate at least all legal captures including (under)promotions
  if (!checkers()) {
    end = generate_captures(pos, m);
    end = add_underprom_caps(pos, m, end);
  } else
    end = generate_evasions(pos, m);
  pos->st->endMoves = end;

  // Resolve captures, letting bestCap keep track of the best non-ep
  // capture and letting bestEp keep track of the best ep capture.
  for (; m < end; m++) {
    Move move = m->move;
    if (!is_capture(pos, move) || !is_legal(pos, move))
      continue;
    do_move(pos, move, gives_check(pos, pos->st, move));
    if (wdl < 0)
      v = -probe_dtm_dc(pos, 1, success) + 1;
    else if (probe_ab(pos, -1, 0, success) < 0 && *success)
      v = -probe_dtm_dc(pos, 0, success) - 1;
    else
      v = -VALUE_MATE;
    undo_move(pos, move);
    if (type_of_m(move) == ENPASSANT)
      bestEp = max(bestEp, v);
    else
      bestCap = max(bestCap, v);
    if (*success == 0)
      return 0;
  }

  // If there are en passant captures, we have to determine the WDL value
  // for the position without ep rights if it might be different.
  if (bestEp > -VALUE_INFINITE && (bestEp < 0 || bestCap < 0)) {
    assert(ep_square() != 0);
    uint8_t s = pos->st->epSquare;
    pos->st->epSquare = 0;
    wdl = probe_ab(pos, -2, 2, success);
    pos->st->epSquare = s;
    if (*success == 0)
      return 0;
    if (wdl == 0)
      return bestEp;
  }

  bestCap = max(bestCap, bestEp);
  int dtm = probe_dtm_table(pos, wdl > 0, success);
  v = wdl > 0 ? VALUE_MATE - 2 * dtm + 1 : -VALUE_MATE + 2 * dtm;
  return max(bestCap, v);
}
#endif

static int WdlToDtz[] = { -1, -101, 0, 101, 1 };

// Probe the DTZ table for a particular position.
// If *success != 0, the probe was successful.
// The return value is from the point of view of the side to move:
//         n < -100 : loss, but draw under 50-move rule
// -100 <= n < -1   : loss in n ply (assuming 50-move counter == 0)
//         0        : draw
//     1 < n <= 100 : win in n ply (assuming 50-move counter == 0)
//   100 < n        : win, but draw under 50-move rule
//
// If the position mate, -1 is returned instead of 0.
//
// The return value n can be off by 1: a return value -n can mean a loss
// in n+1 ply and a return value +n can mean a win in n+1 ply. This
// cannot happen for tables with positions exactly on the "edge" of
// the 50-move rule.
//
// This means that if dtz > 0 is returned, the position is certainly
// a win if dtz + 50-move-counter <= 99. Care must be taken that the engine
// picks moves that preserve dtz + 50-move-counter <= 99.
//
// If n = 100 immediately after a capture or pawn move, then the position
// is also certainly a win, and during the whole phase until the next
// capture or pawn move, the inequality to be preserved is
// dtz + 50-movecounter <= 100.
//
// In short, if a move is available resulting in dtz + 50-move-counter <= 99,
// then do not accept moves leading to dtz + 50-move-counter == 100.
//
int TB_probe_dtz(Position *pos, int *success)
{
  int wdl = TB_probe_wdl(pos, success);
  if (*success == 0) return 0;

  // If draw, then dtz = 0.
  if (wdl == 0) return 0;

  // Check for winning capture or en passant capture as only best move.
  if (*success == 2)
    return WdlToDtz[wdl + 2];

  ExtMove *m = (pos->st-1)->endMoves, *end = NULL;

  // If winning, check for a winning pawn move.
  if (wdl > 0) {
    // Generate at least all legal non-capturing pawn moves
    // including non-capturing promotions.
    // (The following calls in fact generate all moves.)
    end = !checkers()
         ? generate_non_evasions(pos, m)
         : generate_evasions(pos, m);
    pos->st->endMoves = end;

    for (; m < end; m++) {
      Move move = m->move;
      if (type_of_p(moved_piece(move)) != PAWN || is_capture(pos, move)
                || !is_legal(pos, move))
        continue;
      do_move(pos, move, gives_check(pos, pos->st, move));
      int v = -TB_probe_wdl(pos, success);
      undo_move(pos, move);
      if (*success == 0) return 0;
      if (v == wdl)
        return WdlToDtz[wdl + 2];
    }
  }

  // If we are here, we know that the best move is not an ep capture.
  // In other words, the value of wdl corresponds to the WDL value of
  // the position without ep rights. It is therefore safe to probe the
  // DTZ table with the current value of wdl.

  int dtz = probe_dtz_table(pos, wdl, success);
  if (*success >= 0)
    return WdlToDtz[wdl + 2] + ((wdl > 0) ? dtz : -dtz);

  // *success < 0 means we need to probe DTZ for the other side to move.
  int best;
  if (wdl > 0) {
    best = INT32_MAX;
    // If wdl > 0, we have already generated all moves.
    m = (pos->st-1)->endMoves;
  } else {
    // If (cursed) loss, the worst case is a losing capture or pawn move
    // as the "best" move, leading to dtz of -1 or -101.
    // In case of mate, this will cause -1 to be returned.
    best = WdlToDtz[wdl + 2];
    // If wdl < 0, we still have to generate all moves.
    if (!checkers())
      end = generate_non_evasions(pos, m);
    else
      end = generate_evasions(pos, m);
    pos->st->endMoves = end;
  }

  for (; m < end; m++) {
    Move move = m->move;
    // We can skip pawn moves and captures.
    // If wdl > 0, we already caught them. If wdl < 0, the initial value
    // of best already takes account of them.
    if (is_capture(pos, move) || type_of_p(moved_piece(move)) == PAWN
              || !is_legal(pos, move))
      continue;
    do_move(pos, move, gives_check(pos, pos->st, move));
    int v = -TB_probe_dtz(pos, success);
    if (   v == 1
        && checkers()
        && generate_legal(pos, (pos->st-1)->endMoves) == (pos->st-1)->endMoves)
      best = 1;
    else if (wdl > 0) {
      if (v > 0 && v + 1 < best)
        best = v + 1;
    } else {
      if (v - 1 < best)
        best = v - 1;
    }
    undo_move(pos, move);
    if (*success == 0) return 0;
  }
  return best;
}

// Use the DTZ tables to rank and score all root moves in the list.
// A return value of 0 means that not all probes were successful.
bool TB_root_probe_dtz(Position *pos, RootMoves *rm)
{
  int v, success;

  // Obtain 50-move counter for the root position.
  int cnt50 = rule50_count();

  // Check whether a position was repeated since the last zeroing move.
  // In that case, we need to be careful and play DTZ-optimal moves if
  // winning.
  bool rep = pos->hasRepeated;

  // The border between draw and win lies at rank 1 or rank 900, depending
  // on whether the 50-move rule is used.
  int bound = option_value(OPT_SYZ_50_MOVE) ? 900 : 1;

  // Probe, rank and score each move.
  pos->st->endMoves = (pos->st-1)->endMoves;
  for (int i = 0; i < rm->size; i++) {
    RootMove *m = &rm->move[i];
    do_move(pos, m->pv[0], gives_check(pos, pos->st, m->pv[0]));

    // Calculate dtz for the current move counting from the root position.
    if (rule50_count() == 0) {
      // If the move resets the 50-move counter, dtz is -101/-1/0/1/101.
      v = -TB_probe_wdl(pos, &success);
      v = WdlToDtz[v + 2];
    } else {
      // Otherwise, take dtz for the new position and correct by 1 ply.
      v = -TB_probe_dtz(pos, &success);
      if (v > 0) v++;
      else if (v < 0) v--;
    }
    // Make sure that a mating move gets value 1.
    if (checkers() && v == 2) {
      if (generate_legal(pos, (pos->st-1)->endMoves) == (pos->st-1)->endMoves)
        v = 1;
    }

    undo_move(pos, m->pv[0]);
    if (!success) return false;

    // Better moves are ranked higher. Guaranteed wins are ranked equally.
    // Losing moves are ranked equally unless a 50-move draw is in sight.
    // Note that moves ranked 900 have dtz + cnt50 == 100, which in rare
    // cases may be insufficient to win as dtz may be one off (see the
    // comments before TB_probe_dtz()).
    int r =  v > 0 ? (v + cnt50 <= 99 && !rep ? 1000 : 1000 - (v + cnt50))
           : v < 0 ? (-v * 2 + cnt50 < 100 ? -1000 : -1000 + (-v + cnt50))
           : 0;
    m->tbRank = r;

    // Determine the score to be displayed for this move. Assign at least
    // 1 cp to cursed wins and let it grow to 49 cp as the position gets
    // closer to a real win.
    m->tbScore =  r >= bound ? VALUE_MATE - MAX_MATE_PLY - 1
                : r >  0     ? max( 3, r - 800) * PawnValueEg / 200
                : r == 0     ? VALUE_DRAW
                : r > -bound ? min(-3, r + 800) * PawnValueEg / 200
                :             -VALUE_MATE + MAX_MATE_PLY + 1;
  }

  return true;
}

// Use the WDL tables to rank all root moves in the list.
// This is a fallback for the case that some or all DTZ tables are missing.
// A return value of 0 means that not all probes were successful.
bool TB_root_probe_wdl(Position *pos, RootMoves *rm)
{
  static int WdlToRank[] = { -1000, -899, 0, 899, 1000 };
  static Value WdlToValue[] = {
    -VALUE_MATE + MAX_MATE_PLY + 1,
    VALUE_DRAW - 2,
    VALUE_DRAW,
    VALUE_DRAW + 2,
    VALUE_MATE - MAX_MATE_PLY - 1
  };

  int v, success;
  int move50 = option_value(OPT_SYZ_50_MOVE);

  // Probe, rank and score each move.
  pos->st->endMoves = (pos->st-1)->endMoves;
  for (int i = 0; i < rm->size; i++) {
    RootMove *m = &rm->move[i];
    do_move(pos, m->pv[0], gives_check(pos, pos->st, m->pv[0]));
    v = -TB_probe_wdl(pos, &success);
    undo_move(pos, m->pv[0]);
    if (!success) return false;
    if (!move50)
      v = v > 0 ? 2 : v < 0 ? -2 : 0;
    m->tbRank = WdlToRank[v + 2];
    m->tbScore = WdlToValue[v + 2];
  }

  return true;
}

// Use the DTM tables to find mate scores.
// Either DTZ or WDL must have been probed successfully earlier.
// A return value of 0 means that not all probes were successful.
bool TB_root_probe_dtm(Position *pos, RootMoves *rm)
{
  int success;
  Value tmpScore[rm->size];

  // Probe each move.
  pos->st->endMoves = (pos->st-1)->endMoves;
  for (int i = 0; i < rm->size; i++) {
    RootMove *m = &rm->move[i];

    // Use tbScore to find out if the position is won or lost.
    int wdl =  m->tbScore >  PawnValueEg ?  2
             : m->tbScore < -PawnValueEg ? -2 : 0;

    if (wdl == 0)
      tmpScore[i] = 0;
    else {
      // Probe and adjust mate score by 1 ply.
      do_move(pos, m->pv[0], gives_check(pos, pos->st, m->pv[0]));
      Value v = -TB_probe_dtm(pos, -wdl, &success);
      tmpScore[i] = wdl > 0 ? v - 1 : v + 1;
      undo_move(pos, m->pv[0]);
      if (success == 0)
        return false;
    }
  }

  // All probes were successful. Now adjust TB scores and ranks.
  for (int i = 0; i < rm->size; i++) {
    RootMove *m = &rm->move[i];

    m->tbScore = tmpScore[i];

    // Let rank correspond to mate score, except for critical moves
    // ranked 900, which we rank below all other mates for safety.
    // By ranking mates above 1000 or below -1000, we let the search
    // know it need not search those moves.
    m->tbRank = m->tbRank == 900 ? 1001 : m->tbScore;
  }

  return true;
}

// Use the DTM tables to complete a PV with mate score.
void TB_expand_mate(Position *pos, RootMove *move)
{
  int success = 1, chk = 0;
  Value v = move->score, w = 0;
  int wdl = v > 0 ? 2 : -2;
  ExtMove *m;

  if (move->pvSize == MAX_PLY)
    return;

  // First get to the end of the incomplete PV.
  for (int i = 0; i < move->pvSize; i++) {
    v = v > 0 ? -v - 1 : -v + 1;
    wdl = -wdl;
    pos->st->endMoves = (pos->st-1)->endMoves;
    do_move(pos, move->pv[i], gives_check(pos, pos->st, move->pv[i]));
  }

  // Now try to expand until the actual mate.
  if (popcount(pieces()) <= TB_CardinalityDTM)
    while (v != -VALUE_MATE && move->pvSize < MAX_PLY) {
      v = v > 0 ? -v - 1 : -v + 1;
      wdl = -wdl;
      pos->st->endMoves = generate_legal(pos, (pos->st-1)->endMoves);
      for (m = (pos->st-1)->endMoves; m < pos->st->endMoves; m++) {
        do_move(pos, m->move, gives_check(pos, pos->st, m->move));
        if (wdl < 0)
          chk = TB_probe_wdl(pos, &success); // verify that m->move wins
        w =  success && (wdl > 0 || chk < 0)
           ? TB_probe_dtm(pos, wdl, &success)
           : 0;
        undo_move(pos, m->move);
        if (!success || v == w) break;
      }
      if (!success || v != w)
        break;
      move->pv[move->pvSize++] = m->move;
      do_move(pos, m->move, gives_check(pos, pos->st, m->move));
    }

  // Get back to the root position.
  for (int i = move->pvSize - 1; i >= 0; i--)
    undo_move(pos, move->pv[i]);
}

/* ==========================================
   FILE: main.c
   ========================================== */

/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2015 Marco Costalba, Joona Kiiski, Tord Romstad
  Copyright (C) 2015-2016 Marco Costalba, Joona Kiiski, Gary Linscott, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/



int main(int argc, char **argv)
{
  print_engine_info(false);

  psqt_init();
  bitboards_init();
  zob_init();
  bitbases_init();
#ifndef NNUE_PURE
  endgames_init();
#endif
  threads_init();
  options_init();
  search_clear();

  uci_loop(argc, argv);

  threads_exit();
  TB_free();
  options_free();
  tt_free();
  pb_free();
  #ifdef NNUE
  nnue_free();
  #endif

  return 0;
}
