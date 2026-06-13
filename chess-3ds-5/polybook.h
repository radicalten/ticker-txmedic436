#ifndef EMBEDDED_BOOK_H
#define EMBEDDED_BOOK_H

#ifdef USE_EMBEDDED_BOOK

#ifdef __cplusplus
extern "C" {
#endif

extern const unsigned char embedded_book_data[];

#if defined(__3DS__)
/* 3DS Specific: Use linker symbol subtraction to prevent size evaluation crashes */
extern const unsigned char embedded_book_end[];
#define embedded_book_size ((unsigned int)(embedded_book_end - embedded_book_data))
#else
/* Non-3DS standard fallback */
extern const unsigned int embedded_book_size;
#endif

#ifdef __cplusplus
}
#endif

#endif // USE_EMBEDDED_BOOK
#endif // EMBEDDED_BOOK_H
