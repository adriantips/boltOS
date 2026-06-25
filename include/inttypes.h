#pragma once
#include <stdint.h>
/* <inttypes.h> -- printf/scanf length modifiers for the fixed-width types and
 * the wide integer string conversions. LP64: int64 == long. */

#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "ld"
#define PRIi32 "i"
#define PRIi64 "li"
#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "lu"
#define PRIx32 "x"
#define PRIx64 "lx"
#define PRIX32 "X"
#define PRIX64 "lX"
#define PRIo32 "o"
#define PRIo64 "lo"
#define PRIdPTR "ld"
#define PRIuPTR "lu"
#define PRIxPTR "lx"
#define PRIdMAX "ld"
#define PRIuMAX "lu"
#define PRIxMAX "lx"

#define SCNd32 "d"
#define SCNd64 "ld"
#define SCNu32 "u"
#define SCNu64 "lu"
#define SCNx32 "x"
#define SCNx64 "lx"

typedef struct { intmax_t quot, rem; } imaxdiv_t;
intmax_t  strtoimax(const char *s, char **end, int base);
uintmax_t strtoumax(const char *s, char **end, int base);
intmax_t  imaxabs(intmax_t v);
imaxdiv_t imaxdiv(intmax_t num, intmax_t den);
