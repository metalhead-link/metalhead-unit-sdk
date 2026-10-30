/*
 * Minimal freestanding runtime linked into every unit. Units don't link the C library (it
 * isn't position independent), but GCC may still emit calls to these for struct copies and
 * initialisation.
 */
#include <stddef.h>
#include <stdint.h>

void * memcpy(void * dest, const void * src, size_t n) {
    uint8_t * d = dest;
    const uint8_t * s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void * memmove(void * dest, const void * src, size_t n) {
    uint8_t * d = dest;
    const uint8_t * s = src;
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dest;
}

void * memset(void * dest, int c, size_t n) {
    uint8_t * d = dest;
    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dest;
}

int memcmp(const void * a, const void * b, size_t n) {
    const uint8_t * x = a;
    const uint8_t * y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y) {
            return *x - *y;
        }
    }
    return 0;
}

// newlib's libm (LIBM := 1 in unit.mk) sets errno through this. Build with -fno-math-errno so
// the compiler doesn't count on it: units can't read errno meaningfully anyway.
static int unitErrno;

int * __errno(void) {
    return &unitErrno;
}
