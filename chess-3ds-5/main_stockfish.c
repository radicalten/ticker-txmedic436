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

#include "3ds_bridge.h"
#include <stdio.h>

#include "bitboard.h"
#include "endgame.h"
#include "pawns.h"
#include "polybook.h"
#include "position.h"
#include "search.h"
#include "thread.h"
#include "tt.h"
#include "uci.h"
#include "tbprobe.h"

#ifdef USE_EMBEDDED_BOOK
#include "embedded_book.h"
#endif

/* 
 * FIX: Explicitly declare pb_free to resolve the "implicit declaration" 
 * compiler error, bypassing any duplicate or missing header issues.
 */
extern void pb_free(void);

int main_stockfish(int argc, char **argv)
{
  print_engine_info(false);

#if defined(__3DS__) && defined(USE_EMBEDDED_BOOK)
  /* Diagnostic Boot Message for the 3DS console screen */
  printf("info string 3DS Compiled with USE_EMBEDDED_BOOK\n");
  if (embedded_book_data != NULL && embedded_book_size > 0) {
      printf("info string Embedded book linked successfully! Size: %u KB (%d moves)\n", 
             embedded_book_size / 1024, 
             (int)(embedded_book_size / 16));
  } else {
      printf("info string WARNING: USE_EMBEDDED_BOOK is set, but book data is empty/null!\n");
  }
#endif

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
  pb_free(); // compiler will now safely find the prototype we declared above
  #ifdef NNUE
  nnue_free();
  #endif

  return 0;
}
