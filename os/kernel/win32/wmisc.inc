/* The rest of kernel32, advapi32, ole32, shell32 and friends that Qt 6 imports.
 * Chamows has one user, no registry and no security model, so many of these
 * give a fixed answer or fail cleanly. The ones that run code later
 * (thread pool, timers) use real threads. */

/* ---- functions imported by number (ordinal) instead of by name ---- */
static const char *ord_name(const char *dll, u32 ord) {
    static const struct { u16 o; const char *n; } ws[] = {
        {1, "accept"}, {2, "bind"}, {3, "closesocket"}, {4, "connect"}, {5, "getpeername"}, {6, "getsockname"},
        {7, "getsockopt"}, {8, "htonl"}, {9, "htons"}, {10, "ioctlsocket"}, {11, "inet_addr"}, {12, "inet_ntoa"},
        {13, "listen"}, {14, "ntohl"}, {15, "ntohs"}, {16, "recv"}, {17, "recvfrom"}, {18, "select"}, {19, "send"},
        {20, "sendto"}, {21, "setsockopt"}, {22, "shutdown"}, {23, "socket"}, {51, "gethostbyaddr"},
        {52, "gethostbyname"}, {57, "gethostname"}, {101, "WSAAsyncSelect"}, {111, "WSAGetLastError"},
        {112, "WSASetLastError"}, {115, "WSAStartup"}, {116, "WSACleanup"}, {151, "__WSAFDIsSet"},
    }, oa[] = {
        {2, "SysAllocString"}, {4, "SysAllocStringLen"}, {6, "SysFreeString"}, {7, "SysStringLen"},
        {8, "VariantInit"}, {9, "VariantClear"}, {200, "GetErrorInfo"}, {201, "SetErrorInfo"},
    };
    if (strcmp(dll, "ws2_32.dll") == 0 || strcmp(dll, "wsock32.dll") == 0) {
        for (u32 i = 0; i < sizeof ws / sizeof ws[0]; i++) if (ws[i].o == ord) return ws[i].n;
    } else if (strcmp(dll, "oleaut32.dll") == 0) {
        for (u32 i = 0; i < sizeof oa / sizeof oa[0]; i++) if (oa[i].o == ord) return oa[i].n;
    }
    return NULL;
}

/* ---- small helpers ---- */
static int wcopy(u16 *dst, int max, const char *s) { /* returns length in units, or the size needed when max is 0 */
    int n = u8_to_w(s, NULL, 0);
    if (!max) return n + 1;
    if (n + 1 > max) { last_error = 122; return 0; }
    u8_to_w(s, dst, max);
    return n;
}
static WINAPI int m_fail(void) { last_error = 50; return 0; }   /* ERROR_NOT_SUPPORTED */
static WINAPI u32 m_err2(void) { return 2; }
static WINAPI u32 m_fail_code(void) { return 50; }              /* ERROR_NOT_SUPPORTED as a return value */                   /* ERROR_FILE_NOT_FOUND as a return value */
static WINAPI u64 m_neg1(void) { return (u64)-1; }
static WINAPI u32 m_hresult_fail(void) { return 0x80004001; }  /* E_NOTIMPL */

/* ---- environment variables: one table for all programs ---- */
static char *envv[64];
static int nenv;
static void env_init(void) {
    static const char *def[] = {
        "QT_QPA_PLATFORM=windows:fontengine=freetype", /* Qt draws text with its own FreeType: no DirectWrite here */
        "QT_STYLE_OVERRIDE=windows11", /* the Windows 11 look of Qt widgets, not the Windows 95 one */
        "USERNAME=usuario", "COMPUTERNAME=CHAMOWS", "USERPROFILE=C:\\CHAMOWS\\USUARIO",
        "APPDATA=C:\\CHAMOWS\\APPDATA", "LOCALAPPDATA=C:\\CHAMOWS\\APPDATA", "SystemRoot=C:\\WINDOWS",
        "windir=C:\\WINDOWS", "SystemDrive=C:", "OS=Windows_NT",
    };
    if (nenv) return;
    for (u32 i = 0; i < sizeof def / sizeof def[0]; i++) {
        u64 k = strlen(def[i]) + 1;
        envv[nenv] = kmalloc(k);
        memcpy(envv[nenv++], def[i], k);
    }
}
static int env_find(const char *n) {
    env_init();
    u64 k = strlen(n);
    for (int i = 0; i < nenv; i++) if (!c__strnicmp(envv[i], n, k) && envv[i][k] == '=') return i;
    return -1;
}
static const char *env_get(const char *n) { int i = env_find(n); return i < 0 ? NULL : envv[i] + strlen(n) + 1; }
static int env_set(const char *n, const char *v) {
    int i = env_find(n);
    if (i >= 0) { kfree(envv[i]); envv[i] = envv[--nenv]; }
    if (!v || !*v) return 1;
    if (nenv == 64) return 0;
    char *e = kmalloc(strlen(n) + strlen(v) + 2);
    if (!e) return 0;
    ksnprintf(e, strlen(n) + strlen(v) + 2, "%s=%s", n, v);
    envv[nenv++] = e;
    return 1;
}
static WINAPI u32 m_GetEnvironmentVariableA(const char *n, char *b, u32 sz) {
    const char *v = env_get(n);
    if (!v) { last_error = 203; return 0; } /* ERROR_ENVVAR_NOT_FOUND */
    u32 k = (u32)strlen(v);
    if (!b || sz <= k) return k + 1;
    memcpy(b, v, k + 1);
    return k;
}
static WINAPI u32 m_GetEnvironmentVariableW(const u16 *n, u16 *b, u32 sz) {
    char a[128];
    w_to_u8(n, -1, a, sizeof a);
    const char *v = env_get(a);
    if (!v) { last_error = 203; return 0; }
    u32 k = (u32)u8_to_w(v, NULL, 0);
    if (!b || sz <= k) return k + 1;
    u8_to_w(v, b, (int)sz);
    return k;
}
static WINAPI int m_SetEnvironmentVariableA(const char *n, const char *v) { return env_set(n, v); }
static WINAPI int m_SetEnvironmentVariableW(const u16 *n, const u16 *v) {
    char a[128], b[1024];
    w_to_u8(n, -1, a, sizeof a);
    if (v) w_to_u8(v, -1, b, sizeof b);
    return env_set(a, v ? b : NULL);
}
static WINAPI char *m_getenv(const char *n) { return (char *)env_get(n); }
static WINAPI int m_getenv_s(u64 *len, char *b, u64 sz, const char *n) {
    const char *v = env_get(n);
    u64 k = v ? strlen(v) + 1 : 0;
    if (len) *len = k;
    if (!v) { if (b && sz) b[0] = 0; return 0; }
    if (!b || !sz) return 0;
    if (sz < k) return 34; /* ERANGE */
    memcpy(b, v, k);
    return 0;
}
static WINAPI int m__wgetenv_s(u64 *len, u16 *b, u64 sz, const u16 *n) {
    char a[128];
    w_to_u8(n, -1, a, sizeof a);
    const char *v = env_get(a);
    u64 k = v ? (u64)u8_to_w(v, NULL, 0) + 1 : 0;
    if (len) *len = k;
    if (!v) { if (b && sz) b[0] = 0; return 0; }
    if (!b || !sz) return 0;
    if (sz < k) return 34;
    u8_to_w(v, b, (int)sz);
    return 0;
}
static WINAPI u16 *m__wgetenv(const u16 *n) { /* each value gets its own UTF-16 copy, kept */
    char a[128];
    w_to_u8(n, -1, a, sizeof a);
    const char *v = env_get(a);
    if (!v) return NULL;
    int k = u8_to_w(v, NULL, 0) + 1;
    u16 *w = p_alloc(2ULL * (u64)k, 0);
    if (w) u8_to_w(v, w, k);
    return w;
}
static WINAPI int m__putenv_s(const char *n, const char *v) { return env_set(n, v) ? 0 : 22; }
static WINAPI int m__wputenv_s(const u16 *n, const u16 *v) { return m_SetEnvironmentVariableW(n, v) ? 0 : 22; }
static WINAPI u16 *m_GetEnvironmentStringsW(void) {
    env_init();
    int tot = 1;
    for (int i = 0; i < nenv; i++) tot += u8_to_w(envv[i], NULL, 0) + 1;
    u16 *w = p_alloc(2ULL * (u64)tot, 1), *o = w;
    for (int i = 0; i < nenv; i++) o += u8_to_w(envv[i], o, tot) + 1;
    *o = 0;
    return w;
}
static WINAPI int m_FreeEnvironmentStringsW(u16 *w) { p_free(w); return 1; }

/* ---- process and system ---- */
static WINAPI void *m_EncodePointer(void *p) { return p; }
static WINAPI u32 m_GetACP(void) { return 65001; }
static WINAPI int m_GetCPInfo(u32 cp, u8 *info) { memset(info, 0, 20); *(u32 *)info = cp == 65001 || cp == 0 ? 4 : 1; info[4] = '?'; return 1; }
static WINAPI void m_GetStartupInfoW(u8 *si) { memset(si, 0, 104); *(u32 *)si = 104; }
static WINAPI void *m_LocalAlloc(u32 fl, u64 n) { return p_alloc(n ? n : 1, (fl & 0x40) != 0); }
static WINAPI u32 m_GetActiveProcessorCount(u16 g) { return 1; }
static WINAPI int m_GetExitCodeThread(u64 v, u32 *code) {
    hobj_t *h = H(v);
    if (v == (u64)-2) { *code = 259; return 1; }
    if (!h || h->kind != H_THREAD) { last_error = 6; return 0; }
    *code = h->th->done ? h->th->exit_code : 259; /* STILL_ACTIVE */
    return 1;
}
static WINAPI int m_GetExitCodeProcess(u64 h, u32 *code) { *code = 259; return 1; }
static WINAPI u32 m_GetProcessId(u64 h) { return 4; }
static WINAPI int m_GetProcessGroupAffinity(u64 h, u16 *n, u16 *g) { if (*n < 1) { *n = 1; last_error = 122; return 0; } *n = 1; g[0] = 0; return 1; }
static WINAPI u32 m_GetSystemDirectoryW(u16 *b, u32 n) { return (u32)wcopy(b, (int)n, "C:\\CHAMOWS\\SYSTEM"); }
static WINAPI int m_GetComputerNameExW(int kind, u16 *b, u32 *n) {
    int k = wcopy(b, (int)*n, "CHAMOWS");
    if (!k) { *n = 8; last_error = 234; return 0; }
    *n = (u32)k;
    return 1;
}
static WINAPI u32 m_GetLongPathNameW(const u16 *s, u16 *d, u32 n) {
    u32 k = (u32)c_wcslen(s);
    if (k + 1 > n) return k + 1;
    if (d != s) memmove(d, s, (k + 1) * 2);
    return k;
}
static WINAPI int m_GetDiskFreeSpaceExW(const u16 *p, u64 *a, u64 *t, u64 *f) {
    if (a) *a = 512ULL << 20;
    if (t) *t = 1ULL << 30;
    if (f) *f = 512ULL << 20;
    return 1;
}
static WINAPI int m_GetVolumeInformationW(const u16 *root, u16 *name, u32 nn, u32 *serial, u32 *maxlen, u32 *flags, u16 *fs, u32 fsn) {
    if (name && nn) wcopy(name, (int)nn, "CHAMOWS");
    if (serial) *serial = 0xC4A30005;
    if (maxlen) *maxlen = 255;
    if (flags) *flags = 3; /* case preserved, case sensitive search off */
    if (fs && fsn) wcopy(fs, (int)fsn, "FAT32");
    return 1;
}
static WINAPI int m_GetVolumePathNameW(const u16 *p, u16 *out, u32 n) { return wcopy(out, (int)n, "C:\\") > 0; }
static WINAPI u32 m_GetTimeZoneInformation(u8 *tz) { memset(tz, 0, 172); return 0; } /* UTC, no daylight saving */
static WINAPI int m_FileTimeToSystemTime(const u64 *ft, u16 *st) {
    i64 secs = (i64)(*ft / 10000000ULL) - 11644473600LL;
    u32 ms = (u32)(*ft / 10000ULL % 1000);
    int *t = c__gmtime64(&secs);
    u16 v[8] = {(u16)(t[5] + 1900), (u16)(t[4] + 1), (u16)t[6], (u16)t[3], (u16)t[2], (u16)t[1], (u16)t[0], (u16)ms};
    memcpy(st, v, 16);
    return 1;
}
static WINAPI int m_SystemTimeToFileTime(const u16 *st, u64 *ft) {
    int t[9] = {st[6], st[5], st[4], st[3], st[1] - 1, st[0] - 1900, 0, 0, 0};
    i64 secs = c__mktime64(t);
    *ft = (u64)(secs + 11644473600LL) * 10000000ULL + (u64)st[7] * 10000ULL;
    return 1;
}
static WINAPI int m_TzSpecificLocalTimeToSystemTime(void *tz, const u16 *in, u16 *out) { memcpy(out, in, 16); return 1; }
static WINAPI u32 m_FormatMessageW(u32 fl, const void *src, u32 id, u32 lang, u16 *buf, u32 n, void *args) {
    char a[48];
    ksnprintf(a, sizeof a, "Error %u", id);
    if (fl & 0x100) { /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        u16 *w = p_alloc(96, 1);
        u8_to_w(a, w, 48);
        *(u16 **)buf = w;
        return (u32)c_wcslen(w);
    }
    return (u32)wcopy(buf, (int)n, a);
}
static WINAPI u32 m_FormatMessageA(u32 fl, const void *src, u32 id, u32 lang, char *buf, u32 n, void *args) {
    char a[48];
    int k = ksnprintf(a, sizeof a, "Error %u", id);
    if (fl & 0x100) { char *s = p_alloc(48, 1); memcpy(s, a, (u64)k + 1); *(char **)buf = s; return (u32)k; }
    if ((u32)k + 1 > n) return 0;
    memcpy(buf, a, (u64)k + 1);
    return (u32)k;
}

static WINAPI u64 m_GetConsoleWindow(void) { return cur->con ? 0xC0C0 : 0; } /* Qt prints help there instead of a message box */

/* ---- locale: one locale, English (United States) with UTF-8 ---- */
static WINAPI u32 m_GetUserDefaultLCID(void) { return 0x409; }
static WINAPI u32 m_LocaleNameToLCID(const u16 *n, u32 f) { return 0x409; }
static WINAPI int m_GetUserPreferredUILanguages(u32 fl, u32 *num, u16 *buf, u32 *len) {
    static const u16 en[] = {'e', 'n', '-', 'U', 'S', 0, 0};
    *num = 1;
    if (buf && *len >= 7) memcpy(buf, en, sizeof en);
    else if (buf) { *len = 7; last_error = 122; return 0; }
    *len = 7;
    return 1;
}
static const char *locale_value(u32 type) {
    switch (type & 0xFFFF) {
    case 0x01: return "0409";  case 0x02: case 0x72: return "English (United States)";
    case 0x59: return "en";    case 0x5A: return "US";    case 0x5C: return "en-US";
    case 0x0E: return ".";     case 0x0F: return ",";     case 0x10: return "3;0";
    case 0x16: return ".";     case 0x17: return ",";     case 0x14: return "$";
    case 0x1F: return "M/d/yyyy"; case 0x20: return "dddd, MMMM d, yyyy"; case 0x1003: return "h:mm:ss tt";
    case 0x1004: return "1252"; case 0x0B: return "437";  case 0x28: return "AM"; case 0x29: return "PM";
    case 0x1009: return "6";   case 0x100C: return "0";  case 0x50: return "-"; case 0x51: return "";
    case 0x1E: return "/"; case 0x1D: return ":"; case 0x1E0: return "+"; case 0x4B: return "-";
    case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F: case 0x30: {
        static const char *d[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
        return d[(type & 0xFFFF) - 0x2A];
    }
    case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: {
        static const char *d[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
        return d[(type & 0xFFFF) - 0x31];
    }
    }
    if ((type & 0xFFFF) >= 0x38 && (type & 0xFFFF) <= 0x43) return mon_n[(type & 0xFFFF) - 0x38];
    if ((type & 0xFFFF) >= 0x44 && (type & 0xFFFF) <= 0x4F) { static char ab[12][4]; char *a = ab[(type & 0xFFFF) - 0x44]; memcpy(a, mon_n[(type & 0xFFFF) - 0x44], 3); return a; }
    return NULL;
}
static WINAPI int m_GetLocaleInfoW(u32 lcid, u32 type, u16 *buf, int n) {
    if (type & 0x20000000) { /* LOCALE_RETURN_NUMBER */
        u32 v = (type & 0xFFFF) == 0x1009 ? 6 : (type & 0xFFFF) == 0x1004 ? 1252 : (type & 0xFFFF) == 0x100C ? 0 : 0;
        if (n >= 2) memcpy(buf, &v, 4);
        return 2;
    }
    const char *v = locale_value(type);
    if (!v) { last_error = 1004; return 0; }
    return n ? (wcopy(buf, n, v) ? (int)c_wcslen(buf) + 1 : 0) : wcopy(NULL, 0, v);
}
static WINAPI int m_GetLocaleInfoEx(const u16 *name, u32 type, u16 *buf, int n) { return m_GetLocaleInfoW(0x409, type, buf, n); }
static WINAPI int m_GetGeoInfoW(u32 id, u32 type, u16 *buf, int n, u16 lang) { return n ? wcopy(buf, n, "US") + 1 : 3; }
static WINAPI u32 m_GetUserGeoID(u32 c) { return 244; }
static WINAPI int m_GetStringTypeW(u32 type, const u16 *s, int n, u16 *out) {
    if (n < 0) n = (int)c_wcslen(s) + 1;
    const u16 *t = r___pctype_func();
    for (int i = 0; i < n; i++) out[i] = s[i] < 256 ? (type == 1 ? t[s[i]] : 0) : (type == 1 ? 0x100 : 0);
    return 1;
}
static int lcmap(u32 fl, const u16 *s, int n, u16 *d, int dn) {
    if (n < 0) n = (int)c_wcslen(s) + 1;
    if (!dn) return n;
    if (dn < n) { last_error = 122; return 0; }
    for (int i = 0; i < n; i++) d[i] = (fl & 0x100) ? c_towlower(s[i]) : (fl & 0x200) ? c_towupper(s[i]) : s[i];
    return n;
}
static WINAPI int m_LCMapStringW(u32 lcid, u32 fl, const u16 *s, int n, u16 *d, int dn) { return lcmap(fl, s, n, d, dn); }
static WINAPI int m_LCMapStringEx(const u16 *name, u32 fl, const u16 *s, int n, u16 *d, int dn, void *a, void *b, u64 c) { return lcmap(fl, s, n, d, dn); }
static WINAPI int m_CompareStringEx(const u16 *name, u32 fl, const u16 *a, int an, const u16 *b, int bn, void *x, void *y, u64 z) {
    if (an < 0) an = (int)c_wcslen(a);
    if (bn < 0) bn = (int)c_wcslen(b);
    for (int i = 0; i < an && i < bn; i++) {
        int x1 = a[i], y1 = b[i];
        if (fl & 1) { x1 = c_towlower((u16)x1); y1 = c_towlower((u16)y1); } /* NORM_IGNORECASE */
        if (x1 != y1) return x1 < y1 ? 1 : 3;
    }
    return an == bn ? 2 : an < bn ? 1 : 3;
}
static WINAPI int m_GetDateFormatW(u32 l, u32 f, const u16 *st, const u16 *fmt, u16 *b, int n) {
    char a[32];
    u16 now[8];
    if (!st) { k_GetLocalTime(now); st = now; }
    ksnprintf(a, sizeof a, "%u/%u/%u", st[1], st[3], st[0]);
    return n ? (wcopy(b, n, a) ? (int)strlen(a) + 1 : 0) : (int)strlen(a) + 1;
}
static WINAPI int m_GetTimeFormatW(u32 l, u32 f, const u16 *st, const u16 *fmt, u16 *b, int n) {
    char a[32];
    u16 now[8];
    if (!st) { k_GetLocalTime(now); st = now; }
    ksnprintf(a, sizeof a, "%02u:%02u:%02u", st[4], st[5], st[6]);
    return n ? (wcopy(b, n, a) ? (int)strlen(a) + 1 : 0) : (int)strlen(a) + 1;
}

/* ---- lock-free lists (SLIST): one pointer to the first entry, then a count ---- */
static WINAPI void m_InitializeSListHead(u64 *h) { h[0] = h[1] = 0; }
static WINAPI void *m_InterlockedPushEntrySList(u64 *h, u64 *e) { u64 old = h[0]; e[0] = old; h[0] = (u64)e; h[1]++; return (void *)old; }
static WINAPI void *m_InterlockedPopEntrySList(u64 *h) { u64 *e = (u64 *)h[0]; if (e) { h[0] = e[0]; h[1]--; } return e; }
static WINAPI void *m_InterlockedFlushSList(u64 *h) { u64 old = h[0]; h[0] = h[1] = 0; return (void *)old; }

/* ---- events and waits ---- */
static WINAPI u64 m_CreateEventExW(void *sa, const u16 *name, u32 fl, u32 acc) { return k_CreateEventA(sa, fl & 1, (fl & 2) != 0, NULL); }
static WINAPI u64 m_CreateSemaphoreExW(void *sa, int init, int max, const u16 *name, u32 fl, u32 acc) { return k_CreateSemaphoreA(sa, init, max, NULL); }
static WINAPI u32 m_WaitForMultipleObjectsEx(u32 n, const u64 *h, int all, u32 ms, int alert) { return k_WaitForMultipleObjects(n, h, all, ms); }
static WINAPI u32 m_MsgWaitForMultipleObjectsEx(u32 n, const u64 *hs, u32 ms, u32 mask, u32 fl) {
    u32 start = ticks_ms;
    MSG m;
    for (;;) {
        if (fl & 1) { /* MWMO_WAITALL */
            if (n && k_WaitForMultipleObjects(n, hs, 1, 0) == 0) return 0;
        } else
            for (u32 i = 0; i < n; i++) if (try_take(hs[i])) return i;
        /* a 0 ms check without MWMO_INPUTAVAILABLE asks for *new* input only: a timer that fell
         * due while the last empty PeekMessage yielded is not new (Qt's loop spins on it otherwise) */
        if (ms == 0 && !(fl & 4) ? (cur->qcount || pending_paint(&m)) : next_msg(&m, 0)) return n;
        if (ms == 0 || (ms != 0xFFFFFFFF && ticks_ms - start >= ms)) return 0x102;
        flush_windows(cur);
        yield(W_RUN);
    }
}
/* what kinds of messages are waiting: keys 1, mouse moves 2, buttons 4, posted 8, timers 0x10, paint 0x20 */
static WINAPI u32 m_GetQueueStatus(u32 fl) {
    u32 have = 0;
    for (int i = 0; i < cur->qcount; i++) {
        u32 msg = cur->q[(cur->qhead + i) % QMAX].message;
        have |= msg >= 0x100 && msg <= 0x109 ? 1 : msg == 0x200 ? 2 : msg >= 0x201 && msg <= 0x20E ? 4 : 8;
    }
    MSG m;
    if (pending_paint(&m)) have |= 0x20;
    if (due_timer(cur, NULL, 0)) have |= 0x10;
    have &= fl;
    return have << 16 | have;
}

/* ---- work that runs later on its own thread: thread pool, timers ---- */
typedef struct {
    u32 magic, kind;          /* 1 work, 2 timer, 3 wait, 4 waitable timer, 5 multimedia timer */
    void *cb, *ctx;
    u64 handle;               /* wait: what to wait for; waitable timer: its event */
    u32 due, period, timeout; /* in ticks_ms */
    u32 gen;                  /* bumped to cancel what is armed */
    int running, closed;
    int in_cb;                /* callbacks executing right now */
    u32 id;
} later_t;
#define LMAGIC 0x4C415452
static later_t *later_new(u32 kind, void *cb, void *ctx) {
    later_t *l = p_alloc(sizeof *l, 1);
    l->magic = LMAGIC; l->kind = kind; l->cb = cb; l->ctx = ctx;
    return l;
}
typedef void (WINAPI *TPCB)(void *inst, void *ctx, void *obj);
typedef void (WINAPI *TPWAITCB)(void *inst, void *ctx, void *obj, u32 res);
typedef void (WINAPI *MMCB)(u32 id, u32 msg, u64 user, u64 a, u64 b);
static WINAPI u32 later_thread(u64 arg) {
    later_t *l = (later_t *)arg;
    u32 gen = l->gen;
    l->running++;
    if (l->kind == 1) { l->in_cb++; ((TPCB)l->cb)(NULL, l->ctx, l); l->in_cb--; }
    else if (l->kind == 3) {
        u32 r = k_WaitForSingleObject(l->handle, l->timeout);
        if (l->gen == gen && !l->closed) { l->in_cb++; ((TPWAITCB)l->cb)(NULL, l->ctx, l, r); l->in_cb--; }
    } else
        for (;;) {
            while ((i32)(ticks_ms - l->due) < 0 && l->gen == gen && !l->closed) k_Sleep(l->due - ticks_ms > 10 ? 10 : 1);
            if (l->gen != gen || l->closed) break;
            if (l->kind == 2) { l->in_cb++; ((TPCB)l->cb)(NULL, l->ctx, l); l->in_cb--; }
            else if (l->kind == 4) k_SetEvent(l->handle);
            else ((MMCB)l->cb)(l->id, 0, (u64)l->ctx, 0, 0);
            if (!l->period) break;
            l->due += l->period;
        }
    l->running--;
    return 0;
}
static void later_start(later_t *l) { k_CreateThread(NULL, 0, (u64)later_thread, (u64)l, 0, NULL); }
static u32 due_ms(const i64 *ft) { /* negative: relative in 100 ns; positive: absolute FILETIME */
    if (!ft) return 0;
    if (*ft < 0) return (u32)((-*ft) / 10000);
    u64 now; k_GetSystemTimeAsFileTime(&now);
    return (u64)*ft > now ? (u32)(((u64)*ft - now) / 10000) : 0;
}
static WINAPI void *m_CreateThreadpoolWork(void *cb, void *ctx, void *env) { return later_new(1, cb, ctx); }
static WINAPI void m_SubmitThreadpoolWork(later_t *l) { later_start(l); }
/* Waits for callbacks that are running; work items also for the ones not started yet, unless
 * cancelled. A wait or timer that has not fired is not a callback: waiting for it would hang. */
static WINAPI void m_WaitForThreadpoolCallbacks(later_t *l, int cancel) {
    if (cancel) l->gen++;
    while (l->kind == 1 && !cancel ? l->running : l->in_cb) yield(W_RUN);
}
static WINAPI void m_CloseThreadpool(later_t *l) { l->closed = 1; l->gen++; } /* the block stays: a callback may still hold it */
static WINAPI void *m_CreateThreadpoolTimer(void *cb, void *ctx, void *env) { return later_new(2, cb, ctx); }
static WINAPI void m_SetThreadpoolTimer(later_t *l, const i64 *ft, u32 period, u32 window) {
    l->gen++;
    if (!ft) return; /* NULL: cancel */
    l->due = ticks_ms + due_ms(ft);
    l->period = period;
    later_start(l);
}
static WINAPI int m_IsThreadpoolTimerSet(later_t *l) { return l->running > 0; }
static WINAPI void *m_CreateThreadpoolWait(void *cb, void *ctx, void *env) { return later_new(3, cb, ctx); }
static WINAPI void m_SetThreadpoolWait(later_t *l, u64 h, const i64 *ft) {
    l->gen++;
    if (!h) return;
    l->handle = h;
    l->timeout = ft ? due_ms(ft) : 0xFFFFFFFF;
    later_start(l);
}
/* A waitable timer is an event; its timing data hangs off a small table. */
static WINAPI u64 m_CreateWaitableTimerExW(void *sa, const u16 *name, u32 fl, u32 acc) { return k_CreateEventA(NULL, fl & 1, 0, NULL); }
static WINAPI u64 m_CreateWaitableTimerW(void *sa, int manual, const u16 *name) { return k_CreateEventA(NULL, manual, 0, NULL); }
static later_t *wtimers[32];
static later_t *wtimer_of(u64 h) {
    for (int i = 0; i < 32; i++) if (wtimers[i] && wtimers[i]->handle == h) return wtimers[i];
    for (int i = 0; i < 32; i++) if (!wtimers[i]) { wtimers[i] = later_new(4, NULL, NULL); wtimers[i]->handle = h; return wtimers[i]; }
    return NULL;
}
static WINAPI int m_SetWaitableTimerEx(u64 h, const i64 *ft, i32 period, void *cb, void *arg, void *ctx, u32 delay) {
    later_t *l = wtimer_of(h);
    if (!l) return 0;
    k_ResetEvent(h);
    l->gen++;
    l->due = ticks_ms + due_ms(ft);
    l->period = period > 0 ? (u32)period : 0;
    later_start(l);
    return 1;
}
static WINAPI int m_SetWaitableTimer(u64 h, const i64 *ft, i32 period, void *cb, void *arg, int resume) { return m_SetWaitableTimerEx(h, ft, period, cb, arg, NULL, 0); }
static WINAPI int m_CancelWaitableTimer(u64 h) { later_t *l = wtimer_of(h); if (l) l->gen++; return 1; }
static u32 mm_next_id = 1;
static later_t *mm_timers[32];
static WINAPI u32 m_timeSetEvent(u32 delay, u32 res, void *cb, u64 user, u32 fl) {
    int slot = -1;
    for (int i = 0; i < 32; i++) if (!mm_timers[i]) { slot = i; break; }
    if (slot < 0 || (fl & 0x10)) return 0; /* TIME_CALLBACK_EVENT_SET is not supported */
    later_t *l = later_new(5, cb, (void *)user);
    l->id = mm_next_id++;
    l->due = ticks_ms + delay;
    l->period = (fl & 1) ? (delay ? delay : 1) : 0;
    mm_timers[slot] = l;
    later_start(l);
    return l->id;
}
static WINAPI u32 m_timeKillEvent(u32 id) {
    for (int i = 0; i < 32; i++) if (mm_timers[i] && mm_timers[i]->id == id) { mm_timers[i]->closed = 1; mm_timers[i] = NULL; return 0; }
    return 97; /* MMSYSERR_INVALPARAM */
}

/* ---- memory-mapped files: a view is a copy of the file's bytes ---- */
static WINAPI u64 m_CreateFileMappingW(u64 file, void *sa, u32 prot, u32 hi, u32 lo, const u16 *name) {
    if (!WF(file) && file != (u64)-1) { last_error = 6; return 0; }
    u64 *m = p_alloc(24, 1);
    m[0] = 0x4D415050; m[1] = file; m[2] = (u64)hi << 32 | lo;
    return (u64)m;
}
static WINAPI void *m_MapViewOfFile(u64 map, u32 acc, u32 hi, u32 lo, u64 n) {
    u64 *m = (u64 *)map;
    if (!m || m[0] != 0x4D415050) { last_error = 6; return NULL; }
    wfile_t *f = WF(m[1]);
    u64 off = (u64)hi << 32 | lo, size = f ? f->size : m[2];
    if (!n) n = size > off ? size - off : 0;
    u8 *v = p_alloc(n ? n : 1, 1);
    if (f && off < f->size) memcpy(v, f->data + off, off + n <= f->size ? n : f->size - off);
    return v;
}
static WINAPI int m_UnmapViewOfFile(void *v) { p_free(v); return 1; }

/* ---- security (one user, everything allowed) ---- */
static WINAPI int m_OpenProcessToken(u64 p, u32 acc, u64 *tok) { *tok = 0x70C; return 1; }
static u8 world_sid[12] = {1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
static WINAPI int m_AllocateAndInitializeSid(void *auth, u8 n, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4, u32 a5, u32 a6, u32 a7, void **sid) {
    u8 *s = p_alloc(8 + 4 * 8, 1);
    s[0] = 1; s[1] = n;
    memcpy(s + 2, auth, 6);
    u32 sub[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
    memcpy(s + 8, sub, 4u * n);
    *sid = s;
    return 1;
}
static WINAPI void *m_FreeSid(void *s) { p_free(s); return NULL; }
static WINAPI u32 m_GetLengthSid(const u8 *s) { return 8 + 4u * s[1]; }
static WINAPI int m_CopySid(u32 n, void *d, const u8 *s) { memcpy(d, s, m_GetLengthSid(s)); return 1; }
static WINAPI u8 *m_GetSidSubAuthorityCount(u8 *s) { return s + 1; }
static WINAPI u32 *m_GetSidSubAuthority(u8 *s, u32 i) { return (u32 *)(s + 8 + 4 * i); }
static WINAPI int m_GetTokenInformation(u64 tok, int cls, u8 *buf, u32 n, u32 *ret) {
    /* TokenUser (1) and TokenOwner/TokenPrimaryGroup: a pointer to a SID, then the SID */
    u32 need = 16 + 12;
    if (ret) *ret = need;
    if (n < need) { last_error = 122; return 0; }
    memset(buf, 0, need);
    *(u8 **)buf = buf + 16;
    memcpy(buf + 16, world_sid, 12);
    return 1;
}
static WINAPI int m_LookupAccountSidW(const u16 *sys, void *sid, u16 *name, u32 *nn, u16 *dom, u32 *dn, u32 *use) {
    if (*nn < 8 || *dn < 8) { *nn = 8; *dn = 8; last_error = 122; return 0; }
    *nn = (u32)wcopy(name, 8, "usuario");
    *dn = (u32)wcopy(dom, 8, "CHAMOWS");
    if (use) *use = 1;
    return 1;
}
static WINAPI u32 m_GetNamedSecurityInfoW(const u16 *n, int type, u32 info, void **owner, void **group, void **dacl, void **sacl, void **sd) {
    if (owner) *owner = world_sid;
    if (group) *group = world_sid;
    if (dacl) *dacl = NULL; /* no list: everyone may do everything */
    if (sacl) *sacl = NULL;
    if (sd) *sd = p_alloc(64, 1);
    return 0;
}
static WINAPI int m_GetUserProfileDirectoryW(u64 tok, u16 *b, u32 *n) {
    int k = wcopy(b, (int)*n, "C:\\CHAMOWS\\USUARIO");
    if (!k) { *n = 20; return 0; }
    *n = (u32)k + 1;
    return 1;
}
static WINAPI int m_SystemFunction036(u8 *buf, u32 n) { /* RtlGenRandom */
    for (u32 i = 0; i < n; i++) { u32 r; r_rand_s(&r); buf[i] = (u8)(r >> 7); }
    return 1;
}

/* ---- COM, shell ---- */
static WINAPI u32 m_DwmIsCompositionEnabled(int *on) { if (on) *on = 0; return 0; }
static WINAPI u32 m_CoCreateGuid(u8 *g) { m_SystemFunction036(g, 16); g[7] = (u8)((g[7] & 0x0F) | 0x40); g[8] = (u8)((g[8] & 0x3F) | 0x80); return 0; }
static WINAPI u32 m_CoInitializeEx(void *r, u32 fl) { return 0; }
static WINAPI void m_CoTaskMemFree(void *p) { p_free(p); }
static WINAPI void *m_CoTaskMemAlloc(u64 n) { return p_alloc(n ? n : 1, 0); }
static WINAPI u32 m_SHGetKnownFolderPath(const u8 *id, u32 fl, u64 tok, u16 **out) {
    u16 *w = p_alloc(128, 1);
    wcopy(w, 64, "C:\\CHAMOWS\\APPDATA");
    *out = w;
    return 0;
}
static WINAPI u16 *m_SysAllocStringLen(const u16 *s, u32 n) {
    u8 *b = p_alloc(4 + 2ULL * n + 2, 1);
    if (!b) return NULL;
    *(u32 *)b = n * 2;
    if (s) memcpy(b + 4, s, 2ULL * n);
    return (u16 *)(b + 4);
}
static WINAPI u16 *m_SysAllocString(const u16 *s) { return s ? m_SysAllocStringLen(s, (u32)c_wcslen(s)) : NULL; }
static WINAPI void m_SysFreeString(u16 *s) { if (s) p_free((u8 *)s - 4); }
static WINAPI u32 m_SysStringLen(const u16 *s) { return s ? *(const u32 *)((const u8 *)s - 4) / 2 : 0; }
static WINAPI u32 m_GetErrorInfo(u32 r, void **e) { *e = NULL; return 1; } /* S_FALSE: no error object */

/* ---- ICU (Qt's text codecs): no converters, so Qt uses its own ---- */
static WINAPI void *m_ucnv_open(const char *name, int *err) { *err = 4; return NULL; } /* U_FILE_ACCESS_ERROR */

/* ---- user32 ---- */
static WINAPI int m_MessageBoxW(void *h, const u16 *text, const u16 *cap, u32 type) {
    char t[512], c[128];
    w_to_u8(text ? text : (const u16 *)u"", -1, t, sizeof t);
    w_to_u8(cap ? cap : (const u16 *)u"", -1, c, sizeof c);
    return u_MessageBoxA(h, t, c, type);
}
static WINAPI char *m_CharPrevExA(u16 cp, const char *start, const char *p, u32 fl) { return (char *)(p > start ? p - 1 : start); }
static WINAPI u64 m_SetCoalescableTimer(void *h, u64 id, u32 ms, void *fn, u32 tol) { return u_SetTimer(h, id, ms, fn); }


/* ---- WinRT, just enough for Qt to read the system colors ----
 * HSTRING has Windows's own layout, because C++/WinRT builds reference strings by
 * itself: {flags (1 = reference), length, 2 x padding, buffer}. NULL is the empty string. */
typedef struct { u32 flags, len, pad1, pad2; const u16 *buf; } hstr_t;
static WINAPI u32 m_WindowsCreateStringReference(const u16 *src, u32 len, void *hdr, hstr_t **out) {
    hstr_t *h = hdr;
    *h = (hstr_t){1, len, 0, 0, src};
    *out = len ? h : NULL;
    return 0;
}
static WINAPI u32 m_WindowsCreateString(const u16 *src, u32 len, hstr_t **out) {
    *out = NULL;
    if (!len) return 0;
    hstr_t *h = kmalloc(sizeof *h + (len + 1) * 2);
    if (!h) return 0x8007000E; /* E_OUTOFMEMORY */
    u16 *b = (u16 *)(h + 1);
    memcpy(b, src, len * 2);
    b[len] = 0;
    *h = (hstr_t){0, len, 0, 0, b};
    *out = h;
    return 0;
}
static WINAPI u32 m_WindowsDeleteString(hstr_t *h) { if (h && !(h->flags & 1)) kfree(h); return 0; }
static WINAPI u32 m_WindowsDuplicateString(hstr_t *h, hstr_t **out) { return h ? m_WindowsCreateString(h->buf, h->len, out) : (*out = NULL, 0); }
static WINAPI const u16 *m_WindowsGetStringRawBuffer(hstr_t *h, u32 *len) {
    if (len) *len = h ? h->len : 0;
    return h ? h->buf : (const u16 *)u"";
}
static WINAPI u32 m_WindowsGetStringLen(hstr_t *h) { return h ? h->len : 0; }
static WINAPI int m_WindowsIsStringEmpty(hstr_t *h) { return !h || !h->len; }

/* Windows.UI.ViewManagement.UISettings: one object that answers every interface Qt asks for.
 * Slot 6 is GetColorValue (IUISettings3); 7 and 8 add/remove the color-change event. */
typedef struct { void **vt; } rt_obj;
static WINAPI u32 rt_QueryInterface(rt_obj *o, const u32 *iid, void **out) {
    /* IMarshal, INoMarshal, IStdMarshalInfo: a plain in-process object has none of these */
    if (iid[0] == 3 || iid[0] == 0xECC8691B || iid[0] == 0x18) { *out = NULL; return 0x80004002; }
    *out = o;
    return 0;
}
static WINAPI u32 rt_AddRef(rt_obj *o) { return 1; }
static WINAPI u32 rt_GetIids(rt_obj *o, u32 *n, void **ids) { *n = 0; *ids = NULL; return 0; }
static WINAPI u32 rt_GetRuntimeClassName(rt_obj *o, hstr_t **name) { *name = NULL; return 0; }
static WINAPI u32 rt_GetTrustLevel(rt_obj *o, u32 *t) { *t = 0; return 0; }
static WINAPI u32 rt_notimpl(void) { return 0x80004001; }
/* Windows 11 light theme, blue accent: Background, Foreground, AccentDark3..1, Accent, AccentLight1..3, Complement (0xAARRGGBB) */
static const u32 ui_colors[10] = {0xFFFFFFFF, 0xFF000000, 0xFF002642, 0xFF004275, 0xFF005A9E, 0xFF0078D4,
                                  0xFF429CE3, 0xFF76B9ED, 0xFF99EBFF, 0xFF0078D4};
static WINAPI u32 rt_GetColorValue(rt_obj *o, u32 type, u8 *c) {
    u32 v = ui_colors[type < 10 ? type : 5];
    c[0] = (u8)(v >> 24); c[1] = (u8)(v >> 16); c[2] = (u8)(v >> 8); c[3] = (u8)v; /* Windows.UI.Color is A, R, G, B */
    return 0;
}
static WINAPI u32 rt_add_event(rt_obj *o, void *handler, i64 *token) { if (token) *token = 1; return 0; }
static WINAPI u32 rt_ok(void) { return 0; }
static void *uiset_vt[24], *uifact_vt[8];
static rt_obj uiset = {uiset_vt}, uifact = {uifact_vt};
static WINAPI u32 rt_ActivateInstance(rt_obj *o, void **out) { *out = &uiset; return 0; }
static void rt_fill(void **vt) {
    vt[0] = rt_QueryInterface; vt[1] = rt_AddRef; vt[2] = rt_AddRef; /* Release: static object */
    vt[3] = rt_GetIids; vt[4] = rt_GetRuntimeClassName; vt[5] = rt_GetTrustLevel;
}
static WINAPI u32 m_RoGetActivationFactory(hstr_t *cls, const u32 *iid, void **out) {
    static const char want[] = "Windows.UI.ViewManagement.UISettings";
    *out = NULL;
    int ok = cls && cls->len == sizeof want - 1;
    for (u32 i = 0; ok && i < cls->len; i++) ok = cls->buf[i] == (u16)want[i];
    if (!ok) { char nm[96]; u32 i = 0; for (; cls && i < cls->len && i < 95; i++) nm[i] = (char)cls->buf[i]; nm[i] = 0; klog("exe: clase WinRT %s no existe\n", nm); }
    if (!ok) return 0x80040154; /* REGDB_E_CLASSNOTREG */
    if (!uifact_vt[0]) {
        rt_fill(uifact_vt);
        uifact_vt[6] = rt_ActivateInstance;
        rt_fill(uiset_vt);
        for (int i = 6; i < 24; i++) uiset_vt[i] = rt_notimpl;
        uiset_vt[6] = rt_GetColorValue;
        uiset_vt[7] = rt_add_event;
        uiset_vt[8] = rt_ok;
    }
    *out = &uifact;
    return 0;
}

/* ---- smaller kernel32/shell32 pieces Telegram asks for ---- */
static WINAPI u64 m_HeapSize(u64 h, u32 fl, const void *p) { return p ? ((const blk_t *)p - 1)->size : (u64)-1; }
static WINAPI int m_CompareStringW(u32 lcid, u32 fl, const u16 *a, int an, const u16 *b, int bn) {
    return m_CompareStringEx(NULL, fl, a, an, b, bn, NULL, NULL, 0);
}
static WINAPI int m_times4(u64 h, u64 *a, u64 *b, u64 *c, u64 *d) { /* GetThreadTimes, GetProcessTimes */
    if (a) *a = 132000000000000000ULL;
    if (b) *b = 0;
    if (c) *c = (u64)ticks_ms * 10000;
    if (d) *d = 0;
    return 1;
}
static WINAPI int m_IsWow64Process(u64 h, int *wow) { if (wow) *wow = 0; return 1; }
typedef int (WINAPI *LOCALE_ENUMW)(const u16 *);
static WINAPI int m_EnumSystemLocalesW(LOCALE_ENUMW fn, u32 fl) { if (fn) fn(u"00000409"); return 1; }
static WINAPI int m_SystemTimeToTzSpecificLocalTime(void *tz, const u16 *in, u16 *out) { memcpy(out, in, 16); return 1; }

/* Named pipes: a program can listen (Telegram and Qt's QLocalServer, to find a copy already
 * running), but no client ever connects, as there are no other programs talking to it. */
static WINAPI u64 m_CreateNamedPipeW(const u16 *name, u32 mode, u32 pmode, u32 n, u32 outb, u32 inb, u32 ms, void *sa) {
    return k_CreateEventA(NULL, 1, 0, NULL);
}
static WINAPI int m_ConnectNamedPipe(u64 h, void *ov) {
    last_error = ov ? 997 : 6; /* ERROR_IO_PENDING: waits forever; without OVERLAPPED there is nothing to wait for */
    return 0;
}
static WINAPI int m_awareness(i64 ctx) { return 2; } /* per-monitor aware */
static WINAPI u32 m_GetProcessDpiAwareness(u64 h, int *v) { if (v) *v = 2; return 0; }
/* ---- more kernel32 for Telegram ---- */
static WINAPI int m_GetProcessAffinityMask(u64 h, u64 *proc, u64 *sys) { if (proc) *proc = 1; if (sys) *sys = 1; return 1; } /* one core */
static WINAPI int m_GetSystemPowerStatus(u8 *st) {
    memset(st, 0, 12);
    st[0] = 1;     /* on AC power */
    st[1] = 128;   /* no battery */
    st[2] = 255;   /* percentage unknown */
    *(u32 *)(st + 4) = 0xFFFFFFFF;
    *(u32 *)(st + 8) = 0xFFFFFFFF;
    return 1;
}
static WINAPI u32 m_ExpandEnvironmentStringsW(const u16 *in, u16 *out, u32 n) {
    u32 k = 0;
    for (u32 i = 0; in[i];) {
        if (in[i] == '%') {
            u32 j = i + 1;
            char name[64];
            int nl = 0;
            while (in[j] && in[j] != '%' && nl < 63) name[nl++] = (char)in[j++];
            name[nl] = 0;
            const char *v = in[j] == '%' ? env_get(name) : NULL;
            if (v) {
                for (; *v; v++, k++) if (k < n) out[k] = (u8)*v;
                i = j + 1;
                continue;
            }
        }
        if (k < n) out[k] = in[i];
        k++, i++;
    }
    if (k < n) out[k] = 0;
    return k + 1;
}
static WINAPI int m_QueryFullProcessImageNameW(u64 h, u32 fl, u16 *buf, u32 *size) {
    u32 k = f_GetModuleFileNameW(NULL, buf, *size);
    if (!k) return 0;
    *size = k;
    return 1;
}
static WINAPI u32 m_GetFinalPathNameByHandleW(u64 h, u16 *buf, u32 n, u32 fl) {
    wfile_t *f = WF(h);
    if (!f) { last_error = 6; return 0; }
    char p[200];
    ksnprintf(p, sizeof p, "\\\\?\\C:\\%s", f->path);
    for (char *q = p; *q; q++) if (*q == '/') *q = '\\';
    u32 len = (u32)strlen(p);
    if (len + 1 > n) return len + 1;
    return a2w(p, buf, n);
}
static WINAPI u16 *m_lstrcpyW(u16 *d, const u16 *s) { u16 *r = d; while ((*d++ = *s++)) {} return r; }
static WINAPI u16 *m_lstrcpynW(u16 *d, const u16 *s, int n) {
    int i = 0;
    for (; i < n - 1 && s[i]; i++) d[i] = s[i];
    if (n > 0) d[i] = 0;
    return d;
}
static WINAPI int m_ProcessIdToSessionId(u32 pid, u32 *sid) { if (sid) *sid = 1; return 1; }
static WINAPI u32 m_WTSGetActiveConsoleSessionId(void) { return 1; }
static WINAPI u32 m_GetPriorityClass(u64 h) { return 0x20; } /* NORMAL_PRIORITY_CLASS */
static WINAPI int m_GetUserDefaultLocaleName(u16 *out, int n) { return (int)a2w("en-US", out, (u32)n) + 1; }
static WINAPI void m_GetCurrentProcessorNumberEx(u16 *pn) { pn[0] = 0; ((u8 *)pn)[2] = 0; }
static WINAPI int m_GetNumaHighestNodeNumber(u32 *n) { *n = 0; return 1; }
static WINAPI int m_CheckRemoteDebuggerPresent(u64 h, int *b) { if (b) *b = 0; return 1; }
static WINAPI int m_GetThreadGroupAffinity(u64 h, u8 *ga) { memset(ga, 0, 16); *(u64 *)ga = 1; return 1; }
static WINAPI u32 m_GetSystemDirectoryA(char *b, u32 n) { const char *d = "C:\\WINDOWS\\SYSTEM32"; u32 l = (u32)strlen(d); if (l + 1 > n) return l + 1; strncpy0(b, d, n); return l; }
static WINAPI u32 m_RtlNtStatusToDosError(u32 st) { return st == 0 ? 0 : 317; } /* ERROR_MR_MID_NOT_FOUND */
static WINAPI u32 m_GetConsoleOutputCP(void) { return 65001; }
static WINAPI u32 m_GetThreadId(u64 h) { return 1; }

/* random numbers for crypto libraries (OpenSSL in Telegram): old CryptoAPI and CNG */
static WINAPI int m_CryptAcquireContextW(u64 *h, const u16 *cont, const u16 *prov, u32 type, u32 fl) { if (h) *h = 0xC4C4; return 1; }
static WINAPI int m_CryptGenRandom(u64 h, u32 n, u8 *buf) { return m_SystemFunction036(buf, n); }
static WINAPI u32 m_BCryptGenRandom(u64 alg, u8 *buf, u32 n, u32 fl) { m_SystemFunction036(buf, n); return 0; }
static WINAPI u32 m_BCryptOpenAlgorithmProvider(u64 *h, const u16 *id, const u16 *impl, u32 fl) { if (h) *h = 0xB4B4; return 0; }
static const struct { const char *name; void *fn; } exports_misc[] = {
    /* kernel32 */
    {"EncodePointer", m_EncodePointer}, {"DecodePointer", m_EncodePointer}, {"GetACP", m_GetACP}, {"GetOEMCP", m_GetACP},
    {"GetCPInfo", m_GetCPInfo}, {"GetStartupInfoW", m_GetStartupInfoW}, {"GetEnvironmentStringsW", m_GetEnvironmentStringsW},
    {"FreeEnvironmentStringsW", m_FreeEnvironmentStringsW},
    {"GetEnvironmentVariableA", m_GetEnvironmentVariableA}, {"GetEnvironmentVariableW", m_GetEnvironmentVariableW},
    {"SetEnvironmentVariableA", m_SetEnvironmentVariableA}, {"SetEnvironmentVariableW", m_SetEnvironmentVariableW},
    {"getenv", m_getenv}, {"getenv_s", m_getenv_s}, {"_wgetenv_s", m__wgetenv_s}, {"_wgetenv", m__wgetenv},
    {"_putenv_s", m__putenv_s}, {"_wputenv_s", m__wputenv_s}, {"LocalAlloc", m_LocalAlloc}, {"GetNativeSystemInfo", k_GetSystemInfo},
    {"GetActiveProcessorCount", m_GetActiveProcessorCount}, {"GetExitCodeThread", m_GetExitCodeThread},
    {"GetExitCodeProcess", m_GetExitCodeProcess}, {"GetProcessId", m_GetProcessId}, {"GetProcessGroupAffinity", m_GetProcessGroupAffinity},
    {"GetSystemDirectoryW", m_GetSystemDirectoryW}, {"GetComputerNameExW", m_GetComputerNameExW}, {"GetLongPathNameW", m_GetLongPathNameW},
    {"GetDiskFreeSpaceExW", m_GetDiskFreeSpaceExW}, {"GetVolumeInformationW", m_GetVolumeInformationW},
    {"GetVolumePathNameW", m_GetVolumePathNameW}, {"GetVolumeNameForVolumeMountPointW", m_fail}, {"GetVolumePathNamesForVolumeNameW", m_fail},
    {"GetTimeZoneInformation", m_GetTimeZoneInformation}, {"FileTimeToSystemTime", m_FileTimeToSystemTime},
    {"SystemTimeToFileTime", m_SystemTimeToFileTime}, {"TzSpecificLocalTimeToSystemTime", m_TzSpecificLocalTimeToSystemTime},
    {"FormatMessageW", m_FormatMessageW}, {"FormatMessageA", m_FormatMessageA}, {"AreFileApisANSI", k_one},
    {"GetUserDefaultLCID", m_GetUserDefaultLCID}, {"LocaleNameToLCID", m_LocaleNameToLCID},
    {"GetUserPreferredUILanguages", m_GetUserPreferredUILanguages}, {"GetLocaleInfoW", m_GetLocaleInfoW},
    {"GetLocaleInfoEx", m_GetLocaleInfoEx}, {"GetGeoInfoW", m_GetGeoInfoW}, {"GetUserGeoID", m_GetUserGeoID},
    {"GetStringTypeW", m_GetStringTypeW}, {"LCMapStringW", m_LCMapStringW}, {"LCMapStringEx", m_LCMapStringEx},
    {"CompareStringEx", m_CompareStringEx}, {"CompareStringW", m_CompareStringW}, {"HeapSize", m_HeapSize},
    {"GetThreadTimes", m_times4}, {"GetProcessTimes", m_times4}, {"IsWow64Process", m_IsWow64Process},
    {"EnumSystemLocalesW", m_EnumSystemLocalesW}, {"SystemTimeToTzSpecificLocalTime", m_SystemTimeToTzSpecificLocalTime},
    {"IsValidCodePage", k_one}, {"IsValidLocale", k_one}, {"GetUserDefaultUILanguage", m_GetUserDefaultLCID},
    {"GetSystemDefaultUILanguage", m_GetUserDefaultLCID}, {"HeapQueryInformation", m_fail}, {"CancelIo", k_one},
    {"WinExec", k_zero}, {"SetCurrentProcessExplicitAppUserModelID", k_zero}, {"GetDateFormatW", m_GetDateFormatW}, {"GetTimeFormatW", m_GetTimeFormatW},
    {"GetCurrencyFormatW", m_fail}, {"InitializeSListHead", m_InitializeSListHead},
    {"InterlockedPushEntrySList", m_InterlockedPushEntrySList}, {"InterlockedPopEntrySList", m_InterlockedPopEntrySList},
    {"InterlockedFlushSList", m_InterlockedFlushSList}, {"CreateEventExW", m_CreateEventExW},
    {"CreateSemaphoreExW", m_CreateSemaphoreExW}, {"WaitForMultipleObjectsEx", m_WaitForMultipleObjectsEx},
    {"CreateThreadpoolWork", m_CreateThreadpoolWork}, {"SubmitThreadpoolWork", m_SubmitThreadpoolWork},
    {"WaitForThreadpoolWorkCallbacks", m_WaitForThreadpoolCallbacks}, {"CloseThreadpoolWork", m_CloseThreadpool},
    {"CreateThreadpoolTimer", m_CreateThreadpoolTimer}, {"SetThreadpoolTimer", m_SetThreadpoolTimer},
    {"IsThreadpoolTimerSet", m_IsThreadpoolTimerSet}, {"WaitForThreadpoolTimerCallbacks", m_WaitForThreadpoolCallbacks},
    {"CloseThreadpoolTimer", m_CloseThreadpool}, {"CreateThreadpoolWait", m_CreateThreadpoolWait},
    {"SetThreadpoolWait", m_SetThreadpoolWait}, {"WaitForThreadpoolWaitCallbacks", m_WaitForThreadpoolCallbacks},
    {"CloseThreadpoolWait", m_CloseThreadpool}, {"FreeLibraryWhenCallbackReturns", k_zero},
    {"CreateWaitableTimerExW", m_CreateWaitableTimerExW}, {"CreateWaitableTimerW", m_CreateWaitableTimerW}, {"SetWaitableTimerEx", m_SetWaitableTimerEx},
    {"SetWaitableTimer", m_SetWaitableTimer}, {"CancelWaitableTimer", m_CancelWaitableTimer},
    {"CreateFileMappingW", m_CreateFileMappingW}, {"MapViewOfFile", m_MapViewOfFile}, {"UnmapViewOfFile", m_UnmapViewOfFile},
    {"OpenFileMappingW", k_zero}, {"SetThreadDescription", k_zero}, {"SetThreadInformation", k_one},
    {"FlushProcessWriteBuffers", k_zero}, {"GetCurrentPackageFullName", m_err2}, {"GetConsoleWindow", m_GetConsoleWindow},
    {"AttachConsole", m_fail}, {"CancelIoEx", m_fail}, {"GetOverlappedResult", m_fail}, {"DeviceIoControl", m_fail},
    {"CreateNamedPipeW", m_CreateNamedPipeW}, {"ConnectNamedPipe", m_ConnectNamedPipe}, {"DisconnectNamedPipe", k_one}, {"PeekNamedPipe", m_fail}, {"CreateProcessW", m_fail},
    {"OpenProcess", k_zero}, {"TerminateProcess", m_fail}, {"TerminateThread", m_fail}, {"K32GetModuleFileNameExW", k_zero},
    {"CopyFileW", m_fail}, {"CreateHardLinkW", m_fail}, {"CreateSymbolicLinkW", m_fail}, {"RemoveDirectoryW", m_fail},
    {"SetFileAttributesW", k_one}, {"SetFileTime", k_one}, {"SetFileInformationByHandle", m_fail},
    {"GetFileInformationByHandleEx", m_fail}, {"FindFirstChangeNotificationW", m_neg1}, {"FindNextChangeNotification", m_fail},
    {"FindCloseChangeNotification", k_one},
    /* ntdll */
    {"NtCreateFile", m_hresult_fail}, {"NtQueryKey", m_hresult_fail}, {"NtQueryVolumeInformationFile", m_hresult_fail},
    /* advapi32 */
    {"RegOpenKeyExW", m_err2}, {"RegCreateKeyExW", m_err2}, {"RegQueryValueExW", m_err2}, {"RegSetValueExW", m_err2},
    {"RegEnumKeyExW", m_err2}, {"RegEnumValueW", m_err2}, {"RegQueryInfoKeyW", m_err2}, {"RegDeleteKeyW", m_err2},
    {"RegDeleteValueW", m_err2}, {"RegCloseKey", k_zero}, {"RegFlushKey", k_zero}, {"RegNotifyChangeKeyValue", m_err2},
    {"RegOpenKeyExA", m_err2}, {"RegQueryValueExA", m_err2}, {"RegGetValueW", m_err2},
    {"OpenProcessToken", m_OpenProcessToken}, {"OpenThreadToken", m_fail}, {"DuplicateToken", m_fail},
    {"GetTokenInformation", m_GetTokenInformation}, {"AllocateAndInitializeSid", m_AllocateAndInitializeSid},
    {"FreeSid", m_FreeSid}, {"GetLengthSid", m_GetLengthSid}, {"CopySid", m_CopySid},
    {"GetSidSubAuthority", m_GetSidSubAuthority}, {"GetSidSubAuthorityCount", m_GetSidSubAuthorityCount},
    {"LookupAccountSidW", m_LookupAccountSidW}, {"GetNamedSecurityInfoW", m_GetNamedSecurityInfoW},
    {"InitializeAcl", k_one}, {"AddAccessAllowedAceEx", k_one}, {"AddAccessDeniedAceEx", k_one},
    {"InitializeSecurityDescriptor", k_one}, {"SetSecurityDescriptorDacl", k_one}, {"SetSecurityDescriptorGroup", k_one},
    {"SetSecurityDescriptorOwner", k_one}, {"SystemFunction036", m_SystemFunction036},
    {"CryptAcquireContextW", m_CryptAcquireContextW}, {"CryptAcquireContextA", m_CryptAcquireContextW}, {"CryptGenRandom", m_CryptGenRandom},
    {"CryptReleaseContext", k_one}, {"BCryptGenRandom", m_BCryptGenRandom}, {"BCryptOpenAlgorithmProvider", m_BCryptOpenAlgorithmProvider},
    {"BCryptCloseAlgorithmProvider", k_zero},
    {"AuthzInitializeResourceManager", m_fail}, {"AuthzInitializeContextFromSid", m_fail}, {"AuthzInitializeContextFromToken", m_fail},
    {"AuthzAccessCheck", m_fail}, {"AuthzFreeContext", k_one}, {"AuthzFreeResourceManager", k_one},
    {"GetUserProfileDirectoryW", m_GetUserProfileDirectoryW}, {"NetShareEnum", m_err2}, {"NetApiBufferFree", k_zero},
    {"WNetGetUniversalNameW", m_err2}, {"GetFileVersionInfoSizeW", k_zero}, {"GetFileVersionInfoW", m_fail}, {"VerQueryValueW", k_zero},
    /* ole32, oleaut32, shell32, winrt */
    {"CoCreateGuid", m_CoCreateGuid}, {"CoInitializeEx", m_CoInitializeEx}, {"CoInitialize", k_zero}, {"CoUninitialize", k_zero},
    {"CoTaskMemFree", m_CoTaskMemFree}, {"CoTaskMemAlloc", m_CoTaskMemAlloc}, {"OleInitialize", k_zero}, {"OleUninitialize", k_zero},
    {"SysAllocString", m_SysAllocString}, {"SysAllocStringLen", m_SysAllocStringLen}, {"SysFreeString", m_SysFreeString},
    {"SysStringLen", m_SysStringLen}, {"GetErrorInfo", m_GetErrorInfo}, {"SetErrorInfo", k_zero},
    {"VariantInit", k_zero}, {"VariantClear", k_zero},
    {"SHGetKnownFolderPath", m_SHGetKnownFolderPath}, {"SHCreateItemFromParsingName", m_hresult_fail}, {"ShellExecuteExW", m_fail},
    {"RoGetActivationFactory", m_RoGetActivationFactory},
    {"GetProcessAffinityMask", m_GetProcessAffinityMask}, {"GetSystemPowerStatus", m_GetSystemPowerStatus},
    {"ExpandEnvironmentStringsW", m_ExpandEnvironmentStringsW}, {"QueryFullProcessImageNameW", m_QueryFullProcessImageNameW},
    {"GetFinalPathNameByHandleW", m_GetFinalPathNameByHandleW}, {"lstrcpyW", m_lstrcpyW}, {"lstrcpynW", m_lstrcpynW},
    {"ProcessIdToSessionId", m_ProcessIdToSessionId}, {"WTSGetActiveConsoleSessionId", m_WTSGetActiveConsoleSessionId},
    {"GetPriorityClass", m_GetPriorityClass}, {"SetPriorityClass", k_one}, {"GetUserDefaultLocaleName", m_GetUserDefaultLocaleName},
    {"GetUserDefaultLangID", m_GetUserDefaultLCID}, {"GetCurrentProcessorNumberEx", m_GetCurrentProcessorNumberEx},
    {"GetNumaHighestNodeNumber", m_GetNumaHighestNodeNumber}, {"CheckRemoteDebuggerPresent", m_CheckRemoteDebuggerPresent},
    {"GetThreadGroupAffinity", m_GetThreadGroupAffinity}, {"SetThreadGroupAffinity", k_one}, {"GetSystemDirectoryA", m_GetSystemDirectoryA},
    {"RtlNtStatusToDosError", m_RtlNtStatusToDosError}, {"GetConsoleOutputCP", m_GetConsoleOutputCP}, {"SetStdHandle", k_one},
    {"GetThreadId", m_GetThreadId}, {"DebugBreak", k_zero}, {"GetLogicalProcessorInformationEx", m_fail}, {"OpenThread", k_zero},
    {"RoInitialize", k_zero}, {"RoUninitialize", k_zero}, {"GetTempPath2W", f_GetTempPathW},
    {"GetAwarenessFromDpiAwarenessContext", m_awareness}, {"GetProcessDpiAwareness", m_GetProcessDpiAwareness}, {"SetProcessDpiAwareness", k_zero},
    /* graphics and text through COM: not here, and saying so lets programs pick their fallback */
    {"CreateDXGIFactory", m_hresult_fail}, {"CreateDXGIFactory1", m_hresult_fail}, {"CreateDXGIFactory2", m_hresult_fail},
    {"D3D11CreateDevice", m_hresult_fail}, {"D3D11CreateDeviceAndSwapChain", m_hresult_fail}, {"D3D12CreateDevice", m_hresult_fail},
    {"D3D12GetDebugInterface", m_hresult_fail}, {"D3D12SerializeVersionedRootSignature", m_hresult_fail},
    {"Direct3DCreate9", k_zero}, {"Direct3DCreate9Ex", m_hresult_fail}, {"DWriteCreateFactory", m_hresult_fail},
    {"D2D1CreateFactory", m_hresult_fail}, {"DCompositionCreateDevice", m_hresult_fail}, {"DCompositionCreateDevice2", m_hresult_fail},
    {"DwmFlush", m_hresult_fail}, {"DwmIsCompositionEnabled", m_DwmIsCompositionEnabled}, {"DwmGetWindowAttribute", m_hresult_fail},
    {"WindowsCreateStringReference", m_WindowsCreateStringReference}, {"WindowsCreateString", m_WindowsCreateString},
    {"WindowsDeleteString", m_WindowsDeleteString}, {"WindowsDuplicateString", m_WindowsDuplicateString},
    {"WindowsGetStringRawBuffer", m_WindowsGetStringRawBuffer}, {"WindowsGetStringLen", m_WindowsGetStringLen},
    {"WindowsIsStringEmpty", m_WindowsIsStringEmpty}, {"RoOriginateLanguageException", k_zero},
    {"ucnv_open", m_ucnv_open}, {"ucnv_countAvailable", k_zero}, {"ucnv_close", k_zero}, {"ucnv_getAvailableName", k_zero},
    /* winmm */
    {"timeSetEvent", m_timeSetEvent}, {"timeKillEvent", m_timeKillEvent},
    /* user32 */
    {"MessageBoxW", m_MessageBoxW}, {"CharPrevExA", m_CharPrevExA}, {"EnumWindows", k_one},
    {"GetQueueStatus", m_GetQueueStatus}, {"MsgWaitForMultipleObjectsEx", m_MsgWaitForMultipleObjectsEx},
    {"SetCoalescableTimer", m_SetCoalescableTimer},
};
