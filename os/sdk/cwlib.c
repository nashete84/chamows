/* Tiny C helpers linked into every Chamows app. */
#include "chamows.h"

void *memset(void *d, int c, unsigned n) {
    unsigned char *p = d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned n) {
    unsigned char *p = d;
    const unsigned char *q = s;
    while (n--) *p++ = *q++;
    return d;
}
unsigned strlen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

/* 64-bit division for 32-bit code (normally in libgcc). */
typedef unsigned long long u64;
typedef long long i64;
u64 __udivmoddi4(u64 n, u64 d, u64 *rem) {
    u64 q = 0;
    if (d == 0) { if (rem) *rem = 0; return 0; }
    int shift = 0;
    while (!(d >> 63) && (d << 1) <= n) { d <<= 1; shift++; }
    for (; shift >= 0; shift--) {
        q <<= 1;
        if (n >= d) { n -= d; q |= 1; }
        d >>= 1;
    }
    if (rem) *rem = n;
    return q;
}
u64 __udivdi3(u64 n, u64 d) { return __udivmoddi4(n, d, 0); }
u64 __umoddi3(u64 n, u64 d) { u64 r; __udivmoddi4(n, d, &r); return r; }
i64 __divmoddi4(i64 a, i64 b, i64 *rem) {
    int na = a < 0, nb = b < 0;
    u64 r, q = __udivmoddi4(na ? -(u64)a : (u64)a, nb ? -(u64)b : (u64)b, &r);
    if (rem) *rem = na ? -(i64)r : (i64)r;
    return na != nb ? -(i64)q : (i64)q;
}
i64 __divdi3(i64 a, i64 b) { return __divmoddi4(a, b, 0); }
i64 __moddi3(i64 a, i64 b) { i64 r; __divmoddi4(a, b, &r); return r; }
