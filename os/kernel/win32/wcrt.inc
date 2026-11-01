/* More of the Microsoft C runtime (UCRT) that programs built with Visual C++
 * import: start-up tables, the "common" printf family, wide strings, math,
 * locale and time helpers. Qt 6 needs most of these. */

/* ---- start-up and exit ---- */
typedef struct { VOIDFN *first, *last, *end; } onexit_t;
static WINAPI int *r___p___argc(void) { return &argc_var; }
static WINAPI char ***r___p___argv(void) { return &argv_var; }
static WINAPI char **r__get_initial_narrow_environment(void) { return envp0; }
static WINAPI int r__initialize_onexit_table(onexit_t *t) { if (t) t->first = t->last = t->end = NULL; return 0; }
static WINAPI int r__register_onexit_function(onexit_t *t, VOIDFN f) {
    if (!t) return -1;
    if (t->last == t->end) {
        u64 n = (u64)(t->end - t->first), nn = n ? n * 2 : 32;
        VOIDFN *a = p_realloc(t->first, nn * sizeof *a);
        if (!a) return -1;
        t->first = a; t->last = a + n; t->end = a + nn;
    }
    *t->last++ = f;
    return 0;
}
static WINAPI int r__execute_onexit_table(onexit_t *t) {
    if (!t || !t->first) return 0;
    VOIDFN *a = t->first, *e = t->last;
    t->first = t->last = t->end = NULL;
    while (e > a) { e--; if (*e) (*e)(); }
    p_free(a);
    return 0;
}
static WINAPI int r__seh_filter(u32 code, void *ep) { return 0; } /* EXCEPTION_CONTINUE_SEARCH */
static WINAPI void r__invoke_watson(void) { klog("exe: el runtime de C detectó un error grave\n"); proc_exit(3); }
static WINAPI void r_terminate(void) { klog("exe: std::terminate (excepción sin capturar)\n"); proc_exit(3); }

/* ---- heap ---- */
static WINAPI void *r__aligned_malloc(u64 n, u64 al) {
    if (al < 16) al = 16;
    u8 *m = p_alloc(n + al + 8, 0);
    if (!m) return NULL;
    u8 *r = (u8 *)(((uptr)m + 8 + al - 1) & ~(uptr)(al - 1));
    ((void **)r)[-1] = m;
    return r;
}
static WINAPI void r__aligned_free(void *p) { if (p) p_free(((void **)p)[-1]); }
static WINAPI int r__callnewh(u64 n) { return 0; }

/* ---- locale ---- */
static u16 ctype_tab[257];
static WINAPI const u16 *r___pctype_func(void) {
    if (!ctype_tab[1 + 'A']) /* _UPPER 1, _LOWER 2, _DIGIT 4, _SPACE 8, _PUNCT 16, _CONTROL 32, _BLANK 64, _HEX 128, _ALPHA 0x100 */
        for (int c = 0; c < 256; c++) {
            u16 v = 0;
            if (c >= 'A' && c <= 'Z') v |= 1 | 0x100;
            if (c >= 'a' && c <= 'z') v |= 2 | 0x100;
            if (c >= '0' && c <= '9') v |= 4;
            if (c == ' ' || (c >= 9 && c <= 13)) v |= 8;
            if (c > 32 && c < 127 && !(v & (1 | 2 | 4))) v |= 16;
            if (c < 32 || c == 127) v |= 32;
            if (c == ' ' || c == 9) v |= 64;
            if ((c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'f')) v |= 128;
            ctype_tab[c + 1] = v;
        }
    return ctype_tab + 1;
}
static u16 loc_name_c[] = {'C', 0};
static const u16 *loc_names[6] = {NULL, loc_name_c, loc_name_c, loc_name_c, loc_name_c, loc_name_c};
static WINAPI const u16 **r___lc_locale_name_func(void) { return loc_names; }

/* ---- printf family ("common" entry points take options and a locale) ---- */
#define PF_SNPRINTF 2 /* _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR */
static int sprint_common(u64 opt, char *buf, u64 n, const char *f, __builtin_ms_va_list ap, int secure) {
    char tmp[2];
    int len = buf && n ? vfmt(buf, (int)(n > 0x7FFFFFFF ? 0x7FFFFFFF : n), f, ap) : vfmt(tmp, 1, f, ap);
    if (!buf || !n) return len;
    if ((u64)len >= n) return (opt & PF_SNPRINTF) ? len : secure ? (buf[0] = 0, -1) : -1;
    return len;
}
static WINAPI int r__stdio_common_vsprintf(u64 opt, char *b, u64 n, const char *f, void *loc, __builtin_ms_va_list ap) { return sprint_common(opt, b, n, f, ap, 0); }
static WINAPI int r__stdio_common_vsprintf_s(u64 opt, char *b, u64 n, const char *f, void *loc, __builtin_ms_va_list ap) { return sprint_common(opt, b, n, f, ap, 1); }
static WINAPI int r__stdio_common_vsnprintf_s(u64 opt, char *b, u64 n, u64 cnt, const char *f, void *loc, __builtin_ms_va_list ap) {
    if (cnt != (u64)-1 && cnt + 1 < n) n = cnt + 1;
    int r = sprint_common(opt, b, n, f, ap, 0);
    if (r < 0 && b && n) b[n - 1] = 0;
    return r;
}
static WINAPI int r__stdio_common_vfprintf(u64 opt, void *fp, const char *f, void *loc, __builtin_ms_va_list ap) { return cf_vfprintf(fp, f, ap); }

/* UTF-8 -> UTF-16; returns the number of units written (without the 0) */
static int u8_to_w(const char *s, u16 *w, int max) {
    int n = 0;
    const u8 *p = (const u8 *)s;
    while (*p) {
        u32 c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
        if (c >= 0x10000) {
            if (w && n + 2 < max) { w[n] = (u16)(0xD800 + ((c - 0x10000) >> 10)); w[n + 1] = (u16)(0xDC00 + (c & 0x3FF)); }
            n += 2;
        } else {
            if (w && n + 1 < max) w[n] = (u16)c;
            n++;
        }
    }
    if (w && max) w[n < max ? n : max - 1] = 0;
    return n;
}
/* wide printf: turn the format into UTF-8, format with wide arguments, turn back */
static int wprint_common(u64 opt, u16 *buf, u64 n, const u16 *f, __builtin_ms_va_list ap, int secure) {
    int fl = (int)c_wcslen(f) * 3 + 1;
    char *f8 = p_alloc((u64)fl, 0), *out = p_alloc(65536, 0);
    if (!f8 || !out) { p_free(f8); p_free(out); return -1; }
    w_to_u8(f, -1, f8, fl);
    int save = fmt_wide;
    fmt_wide = 1;
    vfmt(out, 65536, f8, ap);
    fmt_wide = save;
    int len = u8_to_w(out, NULL, 0);
    if (buf && n) u8_to_w(out, buf, n > 0x7FFFFFFF ? 0x7FFFFFFF : (int)n);
    p_free(f8);
    p_free(out);
    if (!buf || !n) return len;
    if ((u64)len >= n) return (opt & PF_SNPRINTF) ? len : secure ? (buf[0] = 0, -1) : -1;
    return len;
}
static WINAPI int r__stdio_common_vswprintf(u64 opt, u16 *b, u64 n, const u16 *f, void *loc, __builtin_ms_va_list ap) { return wprint_common(opt, b, n, f, ap, 0); }
static WINAPI int r__stdio_common_vswprintf_s(u64 opt, u16 *b, u64 n, const u16 *f, void *loc, __builtin_ms_va_list ap) { return wprint_common(opt, b, n, f, ap, 1); }
static WINAPI int r__stdio_common_vfwprintf(u64 opt, void *fp, const u16 *f, void *loc, __builtin_ms_va_list ap) {
    u16 *w = p_alloc(65536 * 2, 0);
    if (!w) return -1;
    int n = wprint_common(opt | PF_SNPRINTF, w, 65536, f, ap, 0);
    char *a = p_alloc(65536 * 3, 0);
    if (a) { int k = w_to_u8(w, -1, a, 65536 * 3); cf_fwrite(a, 1, (u64)k, fp); p_free(a); }
    p_free(w);
    return n;
}
static void *nullp;
static WINAPI int r__get_stream_buffer_pointers(void *fp, char ***base, char ***ptr, int **cnt) {
    static int zero;
    if (base) *base = (char **)&nullp;
    if (ptr) *ptr = (char **)&nullp;
    if (cnt) *cnt = &zero;
    return 0;
}
static WINAPI void *r__wfsopen(const u16 *n, const u16 *m, int sh) { return c__wfopen(n, m); }
static WINAPI int r_fgetwc(void *f) { int c = c_fgetc(f); return c < 0 ? 0xFFFF : c; }
static WINAPI int r_ungetwc(int c, void *f) { return c_ungetc(c, f); }
static WINAPI int r_freopen_s(void **out, const char *n, const char *m, void *f) { if (out) *out = f; return 0; }
static WINAPI int r__open_osfhandle(u64 h, int fl) { return (int)h; }

/* ---- strings ---- */
static WINAPI u64 r_strnlen(const char *s, u64 n) { u64 i = 0; while (i < n && s[i]) i++; return i; }
static WINAPI u64 r_wcsnlen(const u16 *s, u64 n) { u64 i = 0; while (i < n && s[i]) i++; return i; }
static WINAPI u16 *r__wcsdup(const u16 *s) { u64 n = (c_wcslen(s) + 1) * 2; u16 *d = p_alloc(n, 0); if (d) memcpy(d, s, n); return d; }
static WINAPI int r_strcpy_s(char *d, u64 n, const char *s) { if (!d || !n) return 22; u64 k = strlen(s); if (k >= n) { d[0] = 0; return 34; } memcpy(d, s, k + 1); return 0; }
static WINAPI int r_iswdigit(u32 c) { return c >= '0' && c <= '9'; }
static WINAPI int r_iswspace(u32 c) { return c == ' ' || (c >= 9 && c <= 13) || c == 0xA0 || c == 0x3000; }
static WINAPI int r_iswxdigit(u32 c) { return c < 128 && (r___pctype_func()[c] & 128); }
static WINAPI int r_iswalnum(u32 c) { return c >= 128 || (r___pctype_func()[c] & (1 | 2 | 4 | 0x100)); }
static WINAPI u16 r_btowc(int c) { return c < 0 ? 0xFFFF : (u16)(c & 0xFF); }

/* strtod with fraction and exponent */
static double pow10_(int e) {
    double r = 1, b = 10;
    int neg = e < 0;
    if (neg) e = -e;
    for (; e; e >>= 1, b *= b) if (e & 1) r *= b;
    return neg ? 1 / r : r;
}
static WINAPI double r_strtod(const char *s, char **end) {
    const char *p = s;
    while (*p == ' ' || (*p >= 9 && *p <= 13)) p++;
    int neg = 0;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if ((p[0] | 32) == 'i' && (p[1] | 32) == 'n' && (p[2] | 32) == 'f') {
        if (end) *end = (char *)p + 3;
        return neg ? -__builtin_inf() : __builtin_inf();
    }
    if ((p[0] | 32) == 'n' && (p[1] | 32) == 'a' && (p[2] | 32) == 'n') { if (end) *end = (char *)p + 3; return __builtin_nan(""); }
    double v = 0;
    int e = 0, digits = 0;
    for (; *p >= '0' && *p <= '9'; p++, digits++) v = v * 10 + (*p - '0');
    if (*p == '.') for (p++; *p >= '0' && *p <= '9'; p++, digits++, e--) v = v * 10 + (*p - '0');
    if (!digits) { if (end) *end = (char *)s; return 0; }
    if ((*p | 32) == 'e') {
        const char *q = p + 1;
        int en = 0, eneg = 0;
        if (*q == '-' || *q == '+') eneg = *q++ == '-';
        if (*q >= '0' && *q <= '9') { for (; *q >= '0' && *q <= '9'; q++) if (en < 10000) en = en * 10 + (*q - '0'); e += eneg ? -en : en; p = q; }
    }
    if (end) *end = (char *)p;
    v *= pow10_(e);
    return neg ? -v : v;
}
static WINAPI float r_strtof(const char *s, char **end) { return (float)r_strtod(s, end); }

/* ---- math ---- */
static WINAPI short r__dclass(double x) {
    u64 b; memcpy(&b, &x, 8);
    u64 ex = b >> 52 & 0x7FF, m = b & 0xFFFFFFFFFFFFFULL;
    return ex == 0x7FF ? (m ? 2 : 1) : ex == 0 ? (m ? -2 : 0) : -1; /* NAN, INFINITE, SUBNORMAL, ZERO, NORMAL */
}
static WINAPI short r__fdclass(float x) { return r__dclass((double)x); }
static WINAPI double r_log(double x) { double r; __asm__("fldln2; fxch; fyl2x" : "=t"(r) : "0"(x)); return r; }
static WINAPI float r_logf(float x) { return (float)r_log(x); }
static WINAPI double r_pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 0) return y > 0 ? 0 : __builtin_inf();
    double iy = (double)(i64)y;
    if (iy == y && y > -1e9 && y < 1e9) { /* exact for whole powers */
        i64 n = (i64)y; double r = 1, b = x; int neg = n < 0; if (neg) n = -n;
        for (; n; n >>= 1, b *= b) if (n & 1) r *= b;
        return neg ? 1 / r : r;
    }
    if (x < 0) return __builtin_nan("");
    return exp_(y * r_log(x));
}
static WINAPI double r_round(double x) { return x < 0 ? -c_floor(-x + 0.5) : c_floor(x + 0.5); }
static WINAPI float r_ceilf(float x) { return (float)c_ceil(x); }
static WINAPI double r_copysign(double a, double b) { u64 x, y; memcpy(&x, &a, 8); memcpy(&y, &b, 8); x = (x & ~(1ULL << 63)) | (y & 1ULL << 63); memcpy(&a, &x, 8); return a; }
static WINAPI double r_hypot(double a, double b) { return c_sqrt(a * a + b * b); }
static WINAPI double r_ldexp(double x, int e) {
    for (; e > 60; e -= 60) x *= (double)(1ULL << 60);
    for (; e < -60; e += 60) x /= (double)(1ULL << 60);
    return e >= 0 ? x * (double)(1ULL << e) : x / (double)(1ULL << -e);
}
static WINAPI double r_exp(double x) { return exp_(x); }
static WINAPI double r_frexp(double x, int *e) {
    u64 b; memcpy(&b, &x, 8);
    int ex = (int)(b >> 52 & 0x7FF);
    if (x == 0 || ex == 0x7FF) { *e = 0; return x; }
    if (!ex) { x *= 18014398509481984.0; memcpy(&b, &x, 8); ex = (int)(b >> 52 & 0x7FF) - 54; } /* subnormal: * 2^54 */
    *e = ex - 1022;
    b = (b & ~(0x7FFULL << 52)) | (1022ULL << 52);
    memcpy(&x, &b, 8);
    return x;
}

/* ---- time ---- */
static const char *wday_n[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *mon_n[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
static char *names_list(const char **n, int k) { /* ":Sun:Sunday:Mon:Monday..." in a new block */
    char *s = p_alloc(256, 1), *o = s;
    for (int i = 0; i < k; i++) o += ksnprintf(o, 40, ":%.3s:%s", n[i], n[i]);
    return s;
}
static WINAPI char *r__Getdays(void) { return names_list(wday_n, 7); }
static WINAPI char *r__Getmonths(void) { return names_list(mon_n, 12); }
static WINAPI u16 *r__W_Getdays(void) { char *a = r__Getdays(); u16 *w = p_alloc(512, 1); u8_to_w(a, w, 256); p_free(a); return w; }
static WINAPI u16 *r__W_Getmonths(void) { char *a = r__Getmonths(); u16 *w = p_alloc(512, 1); u8_to_w(a, w, 256); p_free(a); return w; }
/* __lc_time_data: 7+7+12+12+2 names, 3 formats, then the same in UTF-16 */
static void *time_data;
static WINAPI void *r__Gettnames(void) {
    if (time_data) return time_data;
    static const char *ampm[] = {"AM", "PM"};
    const char *s[46];
    static char abbr[19][4];
    for (int i = 0; i < 7; i++) { memcpy(abbr[i], wday_n[i], 3); s[i] = abbr[i]; s[7 + i] = wday_n[i]; }
    for (int i = 0; i < 12; i++) { memcpy(abbr[7 + i], mon_n[i], 3); s[14 + i] = abbr[7 + i]; s[26 + i] = mon_n[i]; }
    s[38] = ampm[0]; s[39] = ampm[1]; s[40] = "MM/dd/yy"; s[41] = "dddd, MMMM dd, yyyy"; s[42] = "HH:mm:ss";
    u8 *t = p_alloc(43 * 8 * 2 + 64, 1);
    for (int i = 0; i < 43; i++) ((const char **)t)[i] = s[i];
    *(u32 *)(t + 43 * 8) = 0x409; /* lcid, then refcount */
    *(u32 *)(t + 43 * 8 + 4) = 1;
    u16 **w = (u16 **)(t + 43 * 8 + 8);
    for (int i = 0; i < 43; i++) { w[i] = p_alloc(64, 1); u8_to_w(s[i], w[i], 32); }
    time_data = t;
    return t;
}
static WINAPI int r__get_tzname(u64 *ret, char *buf, u64 n, int idx) {
    if (ret) *ret = 4;
    if (buf && n >= 4) memcpy(buf, "UTC", 4);
    return 0;
}
static WINAPI int r__localtime64_s(void *tm, const i64 *t) { memcpy(tm, c__gmtime64(t), 36); return 0; }
static WINAPI int r_rand_s(u32 *v) { *v = (u32)c_rand() << 17 ^ (u32)c_rand() << 4 ^ (u32)ticks_ms ^ (u32)c_rand(); return 0; }

/* ---- environment and files ---- */
static WINAPI int r__wgetenv_s(u64 *n, u16 *b, u64 sz, const u16 *name) { if (n) *n = 0; if (b && sz) b[0] = 0; return 0; }
static WINAPI int r__getdrive(void) { return 3; }
static WINAPI int r__wchdir(const u16 *d) { return f_SetCurrentDirectoryW(d) ? 0 : -1; }
static WINAPI u16 *r__wgetdcwd(int drive, u16 *b, int n) { return c__wgetcwd(b, n); }
static WINAPI int r_minus1(void) { errno_ = 2; return -1; }

static const struct { const char *name; void *fn; } exports_crt[] = {
    {"__p___argc", r___p___argc}, {"__p___argv", r___p___argv}, {"_get_initial_narrow_environment", r__get_initial_narrow_environment},
    {"_configure_narrow_argv", k_zero}, {"_initialize_narrow_environment", k_zero},
    {"_configure_wide_argv", k_zero}, {"_initialize_wide_environment", k_zero},
    {"_initialize_onexit_table", r__initialize_onexit_table}, {"_register_onexit_function", r__register_onexit_function},
    {"_execute_onexit_table", r__execute_onexit_table}, {"_crt_atexit", c_atexit}, {"_c_exit", k_zero},
    {"_set_app_type", k_zero}, {"_set_new_handler", k_zero}, {"_set_new_mode", k_zero}, {"_callnewh", r__callnewh},
    {"_invalid_parameter_noinfo", k_zero}, {"_invalid_parameter_noinfo_noreturn", r__invoke_watson}, {"_invoke_watson", r__invoke_watson},
    {"_register_thread_local_exe_atexit_callback", k_zero}, {"_seh_filter_dll", r__seh_filter}, {"_seh_filter_exe", r__seh_filter},
    {"terminate", r_terminate}, {"_aligned_malloc", r__aligned_malloc}, {"_aligned_free", r__aligned_free},
    {"___lc_collate_cp_func", k_zero}, {"___lc_locale_name_func", r___lc_locale_name_func}, {"__pctype_func", r___pctype_func},
    {"_configthreadlocale", k_zero}, {"_lock_locales", k_zero}, {"_unlock_locales", k_zero},
    {"__stdio_common_vsprintf", r__stdio_common_vsprintf}, {"__stdio_common_vsprintf_s", r__stdio_common_vsprintf_s},
    {"__stdio_common_vsnprintf_s", r__stdio_common_vsnprintf_s}, {"__stdio_common_vfprintf", r__stdio_common_vfprintf},
    {"__stdio_common_vswprintf", r__stdio_common_vswprintf}, {"__stdio_common_vswprintf_s", r__stdio_common_vswprintf_s},
    {"__stdio_common_vfwprintf", r__stdio_common_vfwprintf}, {"_get_stream_buffer_pointers", r__get_stream_buffer_pointers},
    {"_open_osfhandle", r__open_osfhandle}, {"_set_fmode", k_zero}, {"_wfsopen", r__wfsopen}, {"fgetwc", r_fgetwc},
    {"ungetwc", r_ungetwc}, {"freopen_s", r_freopen_s}, {"_lock_file", k_zero}, {"_unlock_file", k_zero},
    {"__strncnt", r_strnlen}, {"strnlen", r_strnlen}, {"wcsnlen", r_wcsnlen}, {"_wcsdup", r__wcsdup}, {"strcpy_s", r_strcpy_s},
    {"iswalnum", r_iswalnum}, {"iswdigit", r_iswdigit}, {"iswspace", r_iswspace}, {"iswxdigit", r_iswxdigit}, {"btowc", r_btowc},
    {"strtod", r_strtod}, {"strtof", r_strtof}, {"atof", r_strtod},
    {"_dclass", r__dclass}, {"_ldclass", r__dclass}, {"_fdclass", r__fdclass}, {"atan2", c_atan2_}, {"ceilf", r_ceilf},
    {"copysign", r_copysign}, {"frexp", r_frexp}, {"hypot", r_hypot}, {"ldexp", r_ldexp}, {"log", r_log}, {"logf", r_logf},
    {"pow", r_pow}, {"round", r_round}, {"exp", r_exp},
    {"_Getdays", r__Getdays}, {"_Getmonths", r__Getmonths}, {"_Gettnames", r__Gettnames}, {"_W_Getdays", r__W_Getdays},
    {"_W_Getmonths", r__W_Getmonths}, {"_W_Gettnames", r__Gettnames}, {"_Strftime", k_zero}, {"_Wcsftime", k_zero},
    {"_get_tzname", r__get_tzname}, {"_localtime64_s", r__localtime64_s}, {"_tzset", k_zero}, {"rand_s", r_rand_s},
    {"_putenv_s", k_zero}, {"_wgetenv_s", r__wgetenv_s}, {"_wgetdcwd", r__wgetdcwd}, {"_getdrive", r__getdrive},
    {"_wchdir", r__wchdir}, {"_wchmod", k_zero}, {"_wrmdir", r_minus1},
};
