#ifndef __APPLE__
#define TEMPLATE(F,a,...) _Generic((a), \
    int: F##_int,                        \
    uint64_t: F##_uint64_t,              \
    unsigned: F##_unsigned,              \
    int64_t: F##_int64_t,               \
    int16_t: F##_int16_t,               \
    uint8_t: F##_uint8_t,               \
    size_t: F##_size_t,                  \
    long: F##_long,                      \
    unsigned long: F##_uint64_t,         \
    double: F##_double                   \
) (a,__VA_ARGS__)
#else
#define TEMPLATE(F,a,...) _Generic((a), \
    int: F##_int,                        \
    uint64_t: F##_uint64_t,              \
    unsigned: F##_unsigned,              \
    int64_t: F##_int64_t,               \
    int16_t: F##_int16_t,               \
    uint8_t: F##_uint8_t,               \
    size_t: F##_size_t,                  \
    long: F##_long,                      \
    unsigned long: F##_uint64_t,         \
    double: F##_double                   \
) (a,__VA_ARGS__)
#endif
