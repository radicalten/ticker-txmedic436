#define INCBIN_PREFIX embedded_
#define INCBIN_STYLE INCBIN_STYLE_SNAKE
#include "incbin.h"

/* 
  This registers book.bin into the binary.
  It generates symbols matching our prefix/style:
    - const unsigned char embedded_book_data[]
    - const unsigned int embedded_book_size
*/
INCBIN(book, "book.bin");
