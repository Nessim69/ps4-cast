#include "netpolicy.h"

#include <string.h>

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ieq(const char *a, const char *b) {
    while (*a && *b && lower((unsigned char)*a) == lower((unsigned char)*b)) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

// 1 if `host` ends with `.suffix` (case-insensitive); suffix without the dot.
static int ends_with_label(const char *host, const char *suffix) {
    size_t hl = strlen(host), sl = strlen(suffix);
    if (hl <= sl + 1 || host[hl - sl - 1] != '.') return 0;
    return ieq(host + hl - sl, suffix);
}

static int parse_ipv4(const char *s, unsigned char out[4]) {
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        int v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s++ - '0');
            if (++digits > 3 || v > 255) return 0;
        }
        out[part] = (unsigned char)v;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    return *s == '\0';
}

int netpol_is_ipv4_literal(const char *host) {
    unsigned char ip[4];
    return host && parse_ipv4(host, ip);
}

int netpol_host_is_private(const char *host) {
    if (!host || !host[0]) return 0;
    unsigned char ip[4];
    if (parse_ipv4(host, ip)) {
        return ip[0] == 10 || ip[0] == 127 ||
               (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) ||
               (ip[0] == 192 && ip[1] == 168) ||
               (ip[0] == 169 && ip[1] == 254);
    }
    if (ieq(host, "localhost")) return 1;
    if (!strchr(host, '.')) return 1;                     // single-label LAN name
    static const char *const lan[] = { "local", "lan", "home", "internal", "home.arpa", 0 };
    for (int i = 0; lan[i]; i++) if (ends_with_label(host, lan[i])) return 1;
    return 0;
}

int netpol_cookie_host_ok(const char *origin, const char *target) {
    if (!origin || !target || !origin[0] || !target[0]) return 0;
    if (ieq(origin, target)) return 1;
    if (netpol_is_ipv4_literal(origin) || netpol_is_ipv4_literal(target)) return 0;
    // Parent domain: drop the first label only when at least three remain, so
    // "site.com" stays "site.com" rather than widening to "com".
    const char *parent = origin;
    const char *dot = strchr(origin, '.');
    if (dot && strchr(dot + 1, '.')) parent = dot + 1;
    return ieq(target, parent) || ends_with_label(target, parent);
}

// Days from 1970-01-01 to y-m-d (proleptic Gregorian; Howard Hinnant's algorithm).
static long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

long long netpol_date_to_unix(const char *date) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if (!date || strlen(date) < 11) return 0;
    int mon = -1;
    for (int i = 0; i < 12; i++) if (strncmp(date, months + i * 3, 3) == 0) { mon = i + 1; break; }
    if (mon < 0) return 0;
    int day = 0, year = 0;
    const char *p = date + 3;
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') day = day * 10 + (*p++ - '0');
    while (*p == ' ') p++;
    while (*p >= '0' && *p <= '9') year = year * 10 + (*p++ - '0');
    if (day < 1 || day > 31 || year < 2000) return 0;
    return days_from_civil(year, (unsigned)mon, (unsigned)day) * 86400LL;
}
