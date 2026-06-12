#ifdef USE_EMBEDDED_BOOK

#define INCBIN_PREFIX embedded_
#define INCBIN_STYLE INCBIN_STYLE_SNAKE
#include "incbin.h"

/* Embeds book.bin into raw machine code */
INCBIN(book, "SCCT.bin");

#else
// Prevent compiler warnings for empty translation unit when disabled
typedef int make_iso_compiler_happy;
#endif
