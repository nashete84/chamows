/* Chamows kernel: strings, formatting, serial log, RTC, PCI. */
#include "kernel.h"
#include <stdarg.h>

void *memset(void *d, int c, size_t n) {
    u8 *p = d;
    while (n--) *p++ = (u8)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    void *ret = d;
    size_t words = n >> 2, rest = n & 3;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(words) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(rest) : : "memory");
    return ret;
}
void *memmove(void *d, const void *s, size_t n) {
    u8 *dp = d;
    const u8 *sp = s;
    if (dp < sp || dp >= sp + n) return memcpy(d, s, n);
    while (n--) dp[n] = sp[n];
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}
size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (u8)*a - (u8)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (u8)*a - (u8)*b;
        if (!*a) return 0;
    }
    return 0;
}
char *strncpy0(char *d, const char *s, size_t n) {
    size_t i = 0;
    if (!n) return d;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    d[i] = 0;
    return d;
}
char *utoa(u32 v, char *buf, int base) {
    char tmp[16];
    int i = 0, j = 0;
    do {
        u32 r = v % base;
        tmp[i++] = r < 10 ? '0' + r : 'a' + r - 10;
        v /= base;
    } while (v);
    while (i) buf[j++] = tmp[--i];
    buf[j] = 0;
    return buf;
}
char *itoa(int v, char *buf) {
    if (v < 0) {
        buf[0] = '-';
        utoa((u32)(-v), buf + 1, 10);
    } else {
        utoa((u32)v, buf, 10);
    }
    return buf;
}

static int vfmt(char *buf, size_t n, const char *fmt, va_list ap) {
    size_t o = 0;
#define PUT(c) do { if (o + 1 < n) buf[o] = (c); o++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        char num[24];
        const char *s = num;
        switch (*fmt) {
        case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; break;
        case 'd': itoa(va_arg(ap, int), num); break;
        case 'u': utoa(va_arg(ap, u32), num, 10); break;
        case 'x': utoa(va_arg(ap, u32), num, 16); break;
        case 'c': num[0] = (char)va_arg(ap, int); num[1] = 0; break;
        case '%': num[0] = '%'; num[1] = 0; break;
        default: num[0] = 0; break;
        }
        int len = (int)strlen(s);
        for (; width > len; width--) PUT(pad);
        while (*s) PUT(*s++);
    }
    if (n) buf[o < n ? o : n - 1] = 0;
#undef PUT
    return (int)o;
}
int kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap) { return vfmt(buf, n, fmt, ap); }

int ksnprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vfmt(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- serial: COM1, 38400 8N1. Used for logs and automated tests. ---- */
#define COM1 0x3F8
void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}
static void serial_put(char c) {
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) {}
    outb(COM1, (u8)c);
}
void klog(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vfmt(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (char *p = buf; *p; p++) {
        if (*p == '\n') serial_put('\r');
        serial_put(*p);
    }
}

/* ---- RTC (CMOS clock) ---- */
static u8 cmos(u8 reg) { outb(0x70, reg); return inb(0x71); }
void rtc_read(rtc_time_t *t) {
    while (cmos(0x0A) & 0x80) {}
    u8 sec = cmos(0x00), min = cmos(0x02), hour = cmos(0x04);
    u8 day = cmos(0x07), mon = cmos(0x08), year = cmos(0x09), b = cmos(0x0B);
    if (!(b & 0x04)) { /* BCD */
#define BCD(x) (((x) & 0x0F) + ((x) >> 4) * 10)
        sec = BCD(sec); min = BCD(min); day = BCD(day); mon = BCD(mon); year = BCD(year);
        hour = (u8)(BCD(hour & 0x7F) | (hour & 0x80));
#undef BCD
    }
    if (!(b & 0x02) && (hour & 0x80)) hour = (u8)(((hour & 0x7F) + 12) % 24);
    t->sec = sec; t->min = min; t->hour = hour; t->day = day; t->month = mon; t->year = 2000 + year;
}

/* ---- PCI config space (mechanism #1) ---- */
u32 pci_read(u8 bus, u8 dev, u8 fn, u8 off) {
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC));
    return inl(0xCFC);
}
void pci_write(u8 bus, u8 dev, u8 fn, u8 off, u32 v) {
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | (off & 0xFC));
    outl(0xCFC, v);
}
/* Finds the (skip+1)-th device of a class; returns 1 if found. */
u32 pci_find_class(u8 cls, u8 sub, u8 prog, u8 *bus, u8 *dev, u8 *fn, u32 skip) {
    for (int b = 0; b < 256; b++)
        for (int d = 0; d < 32; d++)
            for (int f = 0; f < 8; f++) {
                u32 id = pci_read((u8)b, (u8)d, (u8)f, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
                u32 c = pci_read((u8)b, (u8)d, (u8)f, 8);
                if ((c >> 24) == cls && ((c >> 16) & 0xFF) == sub && ((c >> 8) & 0xFF) == prog) {
                    if (skip--) continue;
                    *bus = (u8)b; *dev = (u8)d; *fn = (u8)f;
                    return 1;
                }
            }
    return 0;
}
int pci_find(u16 vendor, u16 device, u8 *bus, u8 *dev, u8 *fn) {
    for (int b = 0; b < 256; b++)
        for (int d = 0; d < 32; d++)
            for (int f = 0; f < 8; f++) {
                u32 id = pci_read((u8)b, (u8)d, (u8)f, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
                if ((id & 0xFFFF) == vendor && (id >> 16) == device) {
                    *bus = (u8)b; *dev = (u8)d; *fn = (u8)f;
                    return 1;
                }
            }
    return 0;
}

void cpu_vendor(char out[13]) {
    u32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    memcpy(out, &b, 4); memcpy(out + 4, &d, 4); memcpy(out + 8, &c, 4);
    out[12] = 0;
}
