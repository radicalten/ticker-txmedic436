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
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>
#include <nds.h>

#include "misc.h"
#include "thread.h"

// Version number. If Version is left empty, then compile date in the format
// DD-MM-YY and show in engine_info.
char Version[] = "";

// Statically initialize our global Calico-powered IO Mutex
LightLock ioMutex = { 0, { NULL, NULL } };

static char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
static char date[] = __DATE__;

// print engine_info() prints the full name of the current Stockfish version.
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

         " on Nintendo DSi"

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

FD open_file(const char *name)
{
  return open(name, O_RDONLY);
}

void close_file(FD fd)
{
  close(fd);
}

size_t file_size(FD fd)
{
  struct stat statbuf;
  fstat(fd, &statbuf);
  return statbuf.st_size;
}

const void *map_file(FD fd, map_t *map)
{
  // Allocate standard heap memory and read the file directly into it (mmap fallback)
  size_t size = file_size(fd);
  *map = (map_t)size;
  void *data = malloc(size);
  if (!data) return NULL;

  if (read(fd, data, size) != (ssize_t)size) {
    free(data);
    return NULL;
  }
  return data;
}

void unmap_file(const void *data, map_t map)
{
  (void)map;
  if (data) {
    free((void *)data);
  }
}

void *allocate_memory(size_t size, bool lp, alloc_t *alloc)
{
  (void)lp; // Large pages are not supported on DS/DSi
  
  // Allocate memory aligned to 64 bytes (optimal for ARM cache-lines)
  void *ptr = memalign(64, size);
  alloc->ptr = ptr;
  alloc->size = size;
  return ptr;
}

void free_memory(alloc_t *alloc)
{
  if (alloc->ptr) {
    free(alloc->ptr);
  }
}

// Hardware-accurate millisecond timer implementation utilizing our compatibility bridge
int64_t now(void) {
  return (int64_t)osGetTime();
}
