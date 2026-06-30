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

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <malloc.h> // Native memory alignment allocation

#include "evaluate.h"
#include "misc.h"
#include "movegen.h"
#include "position.h"
#include "search.h"
#include "settings.h"
#include "thread.h"
#include "timeman.h"
#include "uci.h"

extern void benchmark(Position *pos, char *str);

static const char StartFEN[] =
  "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

// Fast inline integer-to-string formatter (eliminates expensive sprintf overhead)
static inline void fast_itoa(int val, char *buf) {
    char *p = buf;
    if (val < 0) {
        *p++ = '-';
        val = -val;
    }
    char temp[16];
    int i = 0;
    do {
        temp[i++] = '0' + (val % 10);
        val /= 10;
    } while (val > 0);
    while (i > 0) {
        *p++ = temp[--i];
    }
    *p = '\0';
}

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

  pos->st = pos->stack + 100;
  pos_set(pos, fen, option_value(OPT_CHESS960));

  if (moves) {
    int ply = 0;

    for (moves = strtok(moves, " \t"); moves; moves = strtok(NULL, " \t")) {
      Move m = uci_to_move(pos, moves);
      if (!m) break;
      do_move(pos, m, gives_check(pos, pos->st, m));
      pos->gamePly++;
      if (++ply == 100) {
        memcpy(pos->st - 100, pos->st, StateSize);
        pos->st -= 100;
        pos_set_check_info(pos);
        ply -= 100;
      }
    }

    if (pos->st->pliesFromNull > 99)
      pos->st->pliesFromNull = 99;

    // FIX: Only slide the stack back if we actually exceeded 100 plies.
    if (ply >= 100) {
        int k = (pos->st - (pos->stack + 100)) - max(7, pos->st->pliesFromNull);
        for (; k < 0; k++)
          memcpy(pos->stack + 100 + k, pos->stack + 200 + k, StateSize);
    }
  }

  pos->rootKeyFlip = pos->st->key;
  (pos->st-1)->endMoves = pos->moveList;

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

static void go(Position *pos, char *str)
{
  char *token;
  bool ponderMode = false;

  process_delayed_settings();

  Limits = (struct LimitsType){ 0 };
  Limits.startTime = now();

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

void uci_loop(int argc, char **argv)
{
  Position pos;
  char fen[strlen(StartFEN) + 1];
  char str_buf[64];
  char *token;

  LOCK_INIT(Threads.lock);
  Threads.searching = false;
  Threads.sleeping = false;

  // Use cache-aligned heap blocks (64-byte boundary alignment)
  pos.stackAllocation = memalign(64, 63 + 215 * sizeof(Stack));
  pos.stack = (Stack *)(((uintptr_t)pos.stackAllocation + 0x3f) & ~0x3f);
  pos.moveList = memalign(64, 1000 * sizeof(ExtMove));
  pos.st = pos.stack + 100;
  pos.st[-1].endMoves = pos.moveList;

  size_t buf_size = 1;
  for (int i = 1; i < argc; i++)
    buf_size += strlen(argv[i]) + 1;

  // Enforce command allocation limit
  if (buf_size < 8192) buf_size = 8192;

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
    if (argc == 1) {
        sf_recv_command(cmd, buf_size);
    }

    // Safely strip Windows carriage returns (\r), Unix newlines (\n), and trailing spaces
    size_t cmd_len = strlen(cmd);
    while (cmd_len > 0 && (cmd[cmd_len - 1] == '\n' || cmd[cmd_len - 1] == '\r' || cmd[cmd_len - 1] == ' ')) {
      cmd[cmd_len - 1] = 0;
      cmd_len--;
    }

    printf("[ENGINE_RECV] %s\n", cmd);

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
      Threads.ponder = false;
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
      printf("id name ");
      print_engine_info(true);
      printf("\n");
      print_options();
      printf("uciok\n");
    }
    else if (strcmp(token, "ucinewgame") == 0) {
      process_delayed_settings();
      search_clear();
    } 
    else if (strcmp(token, "isready") == 0) {
      printf("[DIAGNOSTIC] Engine entering 'isready' execution block...\n");
      printf("[DIAGNOSTIC] Resizing memory/threads via process_delayed_settings()...\n");
      process_delayed_settings();
      printf("[DIAGNOSTIC] process_delayed_settings() successfully completed!\n");
      printf("readyok\n");
      printf("[DIAGNOSTIC] Sent 'readyok\\n' to bridge output.\n");
    }
    else if (strcmp(token, "go") == 0)        go(&pos, str);
    else if (strcmp(token, "position") == 0)  position(&pos, str);
    else if (strcmp(token, "setoption") == 0) setoption(str);
    else if (strcmp(token, "bench") == 0)     benchmark(&pos, str);
    else if (strcmp(token, "d") == 0)         print_pos(&pos);
    else if (strcmp(token, "perft") == 0) {
      sprintf(str_buf, "%d %d %d current perft", option_value(OPT_HASH),
                    option_value(OPT_THREADS), atoi(str));
      benchmark(&pos, str_buf);
    }
    else if (strcmp(token, "compiler") == 0)  print_compiler_info();
    #ifndef NO_NNUE
    else if (strcmp(token, "export_net") == 0) nnue_export_net();
    #endif
    else if (strncmp(token, "#", 1)) {
      printf("Unknown command: %s %s\n", token, str);
    }
  } while (argc == 1 && strcmp(token, "quit") != 0);

  if (Threads.searching)
    thread_wait_until_sleeping(threads_main());

  free(cmd);
  free(pos.stackAllocation);
  free(pos.moveList);

  LOCK_DESTROY(Threads.lock);
}

char *uci_value(char *str, Value v)
{
  if (abs(v) < VALUE_MATE_IN_MAX_PLY) {
    strcpy(str, "cp ");
    fast_itoa(v * 100 / PawnValueEg, str + 3);
  } else {
    strcpy(str, "mate ");
    fast_itoa((v > 0 ? VALUE_MATE - v + 1 : -VALUE_MATE - v) / 2, str + 5);
  }
  return str;
}

char *uci_square(char *str, Square s)
{
  str[0] = 'a' + file_of(s);
  str[1] = '1' + rank_of(s);
  str[2] = 0;
  return str;
}

// Optimized array-writing coordinate formatter (removes strcpy/strcat dependencies)
char *uci_move(char *str, Move m, int chess960)
{
  if (m == 0) {
    strcpy(str, "(none)");
    return str;
  }
  if (m == MOVE_NULL) {
    strcpy(str, "0000");
    return str;
  }

  Square from = from_sq(m);
  Square to = to_sq(m);

  if (type_of_m(m) == CASTLING && !chess960) {
    to = make_square(to > from ? FILE_G : FILE_C, rank_of(from));
  }

  str[0] = 'a' + file_of(from);
  str[1] = '1' + rank_of(from);
  str[2] = 'a' + file_of(to);
  str[3] = '1' + rank_of(to);

  if (type_of_m(m) == PROMOTION) {
    str[4] = " pnbrqk"[promotion_type(m)];
    str[5] = '\0';
  } else {
    str[4] = '\0';
  }

  return str;
}

// Extremely optimized direct coordinate matcher (eliminates format-to-string lookup loops)
Move uci_to_move(const Position *pos, char *str)
{
  size_t len = strlen(str);
  if (len < 4) return 0;

  int f_from = str[0] - 'a';
  int r_from = str[1] - '1';
  int f_to   = str[2] - 'a';
  int r_to   = str[3] - '1';

  // Sanity check coordinates
  if (f_from < 0 || f_from > 7 || r_from < 0 || r_from > 7) return 0;
  if (f_to   < 0 || f_to   > 7 || r_to   < 0 || r_to   > 7) return 0;

  int promo = 0;
  if (len == 5) {
    char p = tolower(str[4]);
    if (p == 'n') promo = KNIGHT;
    else if (p == 'b') promo = BISHOP;
    else if (p == 'r') promo = ROOK;
    else if (p == 'q') promo = QUEEN;
  }

  ExtMove list[MAX_MOVES];
  ExtMove *last = generate_legal(pos, list);

  for (ExtMove *m = list; m < last; m++) {
    Move move = m->move;
    Square m_from = from_sq(move);
    Square m_to = to_sq(move);

    if (file_of(m_from) == f_from && rank_of(m_from) == r_from) {
      Square match_to = m_to;
      if (type_of_m(move) == CASTLING && !pos->chess960) {
        match_to = make_square(m_to > m_from ? FILE_G : FILE_C, rank_of(m_from));
      }

      if (file_of(match_to) == f_to && rank_of(match_to) == r_to) {
        if (type_of_m(move) == PROMOTION) {
          if (promotion_type(move) == promo) {
            return move;
          }
        } else if (promo == 0) {
          return move;
        }
      }
    }
  }

  return 0;
}
