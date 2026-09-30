#pragma GCC optimize("Os")
#include "Fmt.h"

char *fmtInt(char *p, int32_t v) {
    uint32_t u = v < 0 ? (uint32_t)(-v) : (uint32_t)v;
    if (v < 0) *p++ = '-';
    char tmp[10];
    int n = 0;
    do { tmp[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}

char *fmtMoney(char *p, int32_t v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    *p++ = '$';
    return fmtInt(p, v);
}

char *fmtStr(char *p, const char *s) {
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}
