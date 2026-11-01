/* Chamows: runs Windows programs (.exe, 64-bit PE files).
 *
 * The loader copies the program into memory, fixes its addresses and links
 * every function it imports from Windows DLLs (kernel32, user32, gdi32,
 * msvcrt) to the small Win32 layer in this file. Each program runs on its own
 * stack and gives the CPU back whenever it waits for a message, sleeps or
 * shows a message box, so the desktop keeps working (cooperative multitasking).
 *
 * This file is built with SSE allowed, because Windows code uses it. */
#include "../kernel.h"
#include "../gui.h"
#include "../net/net.h"

#ifdef __x86_64__
#define WINAPI __attribute__((ms_abi))
typedef i64 (WINAPI *WNDPROC)(void *, u32, u64, i64);
typedef void (WINAPI *VOIDFN)(void);
typedef int (WINAPI *INTFN)(void);

/* ---------- process and window state ---------- */
typedef struct { void *hwnd; u32 message; u64 wparam; i64 lparam; u32 time; i32 ptx, pty; u32 priv; } MSG;
typedef struct { i32 left, top, right, bottom; } RECT;

enum { K_BRUSH = 1, K_PEN, K_FONT, K_BITMAP };
typedef struct gobj { int kind; u32 color; int width, font, null; surface_t surf; int dib; u32 *base; void *ttf; } gobj_t; /* dib: pixels the program writes; base: their allocation; ttf: TrueType font (wtext.inc) */
struct w32win;
typedef struct { struct w32win *w; gobj_t *brush, *pen, *font, *bmp; u32 text, bk; int bkmode, cx, cy; int gmode, talign, has_xf; float xf[6]; } dc_t;

typedef struct { u64 id; u32 every, next; int used, pending; } wtimer_t;
typedef struct w32win {
    u32 magic;
    struct w32proc *p;
    cw_window *cw;
    WNDPROC proc;
    u32 bg;              /* class background, 0 = none */
    int dirty, erase, touched, destroyed;
    i64 userdata;
    u32 style, exstyle;
    int activated, hidden;   /* hidden: HWND_MESSAGE windows, never on screen */
    int big_icon;            /* WM_SETICON gave it a big icon */
    u32 owner;               /* id of the thread that made it: its messages go to that thread */
    struct { char name[32]; u64 v; } props[8];
    dc_t dc;
    wtimer_t timers[48];
    struct w32win *next;
} w32win_t;
#define WIN_MAGIC 0x57494E44

typedef struct { char name[64]; WNDPROC proc; u32 bg; const icon_t *icon; } wclass_t;
typedef struct blk { struct blk *next, *prev; u64 size, pad; } blk_t;

typedef struct w32thread {
    struct w32proc *p;
    uptr rsp;
    u8 *stack, *teb, *tlsb[16];   /* tlsb: one thread-local block per module with TLS */
    int wait;            /* W_RUN, W_MSG, W_SLEEP, W_BOX */
    u32 wake_at, id, exit_code;
    int done;
    u64 start, param;
    u64 tls_slots[64];
    void *ex_chain;      /* exceptions: see wseh.inc */
    u32 lasterr, preempts; /* GetLastError of this thread; times its turn was taken */
    int errno_v;
    int quit, quit_code;  /* PostQuitMessage: WM_QUIT is per thread, like on Windows */
    int suspended;        /* CREATE_SUSPENDED / SuspendThread count */
    struct w32thread *next;
} w32thread_t;

#define QMAX 128
#define CON_COLS 80
#define CON_ROWS 24
typedef struct w32proc {
    char name[40];
    char cmdline[256];
    u16 wcmdline[256];
    char *argv[17];
    int argc;
    char cwd[160];          /* current directory on the pendrive, "" = root */
    const icon_t *icon;
    u8 *raw, *image;        /* the .exe in memory (also p->exe) */
    u64 entry;
    u8 *peb;
    struct w32mod *mods, *exe; /* loaded modules: the .exe and its DLLs */
    struct w32mod *init_order[64];
    int ninit;
    struct { u8 *tmpl; u64 len; } tlsm[16]; /* thread-local data templates */
    int ntls, detached;
    char exedir[160], dlldir[160];
    VOIDFN atexit_fns[32];
    int n_atexit;
    w32thread_t *threads;
    int done, killed, cleaned, quit, exit_code;
    MSG q[QMAX];
    u32 qtid[QMAX];         /* the thread each queued message is for */
    int qhead, qcount;
    struct sent *sent;      /* SendMessage from one thread to a window of another */
    w32win_t *wins;
    wclass_t classes[48];
    int nclasses;
    blk_t *blocks;
    /* console */
    cw_window *con;
    char con_text[CON_ROWS][CON_COLS + 1];
    int con_row, con_col, con_dirty;
    /* message box */
    int box_open, box_result;
    struct w32proc *next;
} w32proc_t;
enum { W_RUN, W_MSG, W_SLEEP, W_BOX };

static w32proc_t *procs, *cur;
static w32thread_t *cur_th;
static void thread_main(void);
static w32thread_t *thread_new(w32proc_t *p, void (*entry)(void), u64 start, u64 param, u32 stack_size);
static u32 next_tid = 8;
#define SLICE_MS 20
static u32 slice_end;
static uptr kernel_rsp;

/* ---------- switching stacks ---------- */
__attribute__((naked)) static void ctx_switch(uptr *save, uptr load) {
    __asm__ volatile(
        "push %rbp\n push %rbx\n push %r12\n push %r13\n push %r14\n push %r15\n"
        "mov %rsp, (%rdi)\n mov %rsi, %rsp\n"
        "pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbx\n pop %rbp\n ret\n");
}
static void set_gs(u64 v) { __asm__ volatile("wrmsr" ::"c"(0xC0000101), "a"((u32)v), "d"((u32)(v >> 32))); }

static void flush_windows(w32proc_t *p) {
    for (w32win_t *w = p->wins; w; w = w->next)
        if (w->touched && w->cw) { w->touched = 0; win_invalidate(w->cw); }
}
static void yield(int wait) {
    cur_th->wait = wait;
    ctx_switch(&cur_th->rsp, kernel_rsp);
}
static void proc_exit(int code) {
    cur->exit_code = code;
    cur->done = 1;
    yield(W_RUN);
    for (;;) {}
}

/* ---------- memory owned by the program (freed when it ends) ---------- */
static void *p_alloc(u64 n, int zero) {
    blk_t *b = kmalloc(sizeof *b + n);
    if (!b) return NULL;
    b->size = n;
    b->pad = 0xB10CB10C;
    b->prev = NULL;
    b->next = cur->blocks;
    if (b->next) b->next->prev = b;
    cur->blocks = b;
    if (zero) memset(b + 1, 0, n);
    return b + 1;
}
static void p_free(void *ptr) {
    if (!ptr) return;
    blk_t *b = (blk_t *)ptr - 1;
    if (((uptr)ptr & 15) || b->pad != 0xB10CB10C) { klog("exe: free() de un puntero desconocido %x\n", (u32)(uptr)ptr); return; }
    b->pad = 0;
    if (b->prev) b->prev->next = b->next; else cur->blocks = b->next;
    if (b->next) b->next->prev = b->prev;
    kfree(b);
}
static void *p_realloc(void *ptr, u64 n) {
    if (!ptr) return p_alloc(n, 0);
    blk_t *b = (blk_t *)ptr - 1;
    if (n <= b->size) return ptr;
    void *q = p_alloc(n, 0);
    if (q) { memcpy(q, ptr, b->size); p_free(ptr); }
    return q;
}

/* ---------- text helpers ---------- */
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int stricmp_(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return lower(*a) - lower(*b);
}
/* Windows programs write bytes: keep valid UTF-8, read anything else as Latin-1. */
static void to_utf8(const char *s, int len, char *out, int max) {
    int o = 0;
    for (int i = 0; i < len && o < max - 4; i++) {
        u8 c = (u8)s[i];
        if (c < 0x80) { out[o++] = (char)c; continue; }
        int n = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : 0;
        int ok = n && i + n < len;
        for (int k = 1; ok && k <= n; k++) ok = ((u8)s[i + k] & 0xC0) == 0x80;
        if (ok) { for (int k = 0; k <= n; k++) out[o++] = s[i + k]; i += n; }
        else { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
}

/* UTF-16 -> UTF-8 (for wide strings inside printf) */
static int w_to_u8(const u16 *w, int wlen, char *out, int max) {
    int o = 0;
    for (int i = 0; (wlen < 0 ? w[i] != 0 : i < wlen) && o < max - 4; i++) {
        u32 c = w[i];
        if (c >= 0xD800 && c < 0xDC00 && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (w[i + 1] - 0xDC00); i++; }
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { out[o++] = (char)(0xE0 | c >> 12); out[o++] = (char)(0x80 | (c >> 6 & 63)); out[o++] = (char)(0x80 | (c & 63)); }
        else { out[o++] = (char)(0xF0 | c >> 18); out[o++] = (char)(0x80 | (c >> 12 & 63)); out[o++] = (char)(0x80 | (c >> 6 & 63)); out[o++] = (char)(0x80 | (c & 63)); }
    }
    out[o] = 0;
    return o;
}
/* printf-style formatting with the Windows variadic convention. fmt_wide:
 * the wide printf family, where %s and %c mean wide strings/characters. */
static int fmt_wide;
static int vfmt(char *out, int size, const char *f, __builtin_ms_va_list ap) {
    int n = 0;
    char wtmp[1024];
#define PUT(ch) do { if (n < size - 1) out[n] = (char)(ch); n++; } while (0)
    for (; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        f++;
        int left = 0, zero = 0, plus = 0, width = 0, prec = -1, lng = 0, shrt = 0;
        for (;; f++) {
            if (*f == '-') left = 1; else if (*f == '0') zero = 1; else if (*f == '+') plus = 1;
            else if (*f == ' ' || *f == '#') {} else break;
        }
        if (*f == '*') { width = __builtin_va_arg(ap, int); f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = __builtin_va_arg(ap, int); f++; }
            while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        for (;; f++) {
            if (*f == 'l' || *f == 'z' || *f == 'j' || *f == 't') lng++;
            else if (*f == 'I' && f[1] == '6' && f[2] == '4') { lng = 2; f += 2; }
            else if (*f == 'h') shrt = 1;
            else if (*f == 'L' || *f == 'q') lng = 2;
            else if (*f == 'I' && f[1] == '3' && f[2] == '2') f += 2;
            else if (*f == 'I') lng = 2;
            else break;
        }
        char tmp[64];
        int len = 0, neg = 0;
        const char *str = tmp;
        switch (*f) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            u64 v;
            int base = (*f == 'x' || *f == 'X' || *f == 'p') ? 16 : *f == 'o' ? 8 : 10;
            if (*f == 'p') { v = (u64)__builtin_va_arg(ap, void *); prec = 16; }
            else if (*f == 'd' || *f == 'i') {
                i64 s = lng ? __builtin_va_arg(ap, i64) : __builtin_va_arg(ap, int);
                if (s < 0) { neg = 1; v = (u64)(-s); } else v = (u64)s;
            } else v = lng ? __builtin_va_arg(ap, u64) : __builtin_va_arg(ap, unsigned);
            const char *dig = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char rev[32]; int r = 0;
            do { rev[r++] = dig[v % (u64)base]; v /= (u64)base; } while (v);
            while (r < prec && r < 30) rev[r++] = '0';
            if (neg) tmp[len++] = '-'; else if (plus && (*f == 'd' || *f == 'i')) tmp[len++] = '+';
            while (r) tmp[len++] = rev[--r];
            break;
        }
        case 'f': case 'F': case 'g': case 'G': case 'e': case 'E': {
            double d = __builtin_va_arg(ap, double);
            if (prec < 0) prec = 6;
            if (prec > 9) prec = 9;
            if (d < 0) { neg = 1; d = -d; }
            u64 scale = 1;
            for (int i = 0; i < prec; i++) scale *= 10;
            if (d > 1e18) d = 1e18;
            u64 whole = (u64)d;
            u64 frac = (u64)((d - (double)whole) * (double)scale + 0.5);
            if (frac >= scale) { whole++; frac -= scale; }
            if (neg) tmp[len++] = '-'; else if (plus) tmp[len++] = '+';
            char rev[24]; int r = 0;
            do { rev[r++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
            while (r) tmp[len++] = rev[--r];
            if (prec) {
                tmp[len++] = '.';
                for (int i = prec - 1; i >= 0; i--) { tmp[len + i] = (char)('0' + frac % 10); frac /= 10; }
                len += prec;
            }
            break;
        }
        case 'c': case 'C': {
            int c = __builtin_va_arg(ap, int);
            int wide = *f == 'C' ? !fmt_wide : shrt ? 0 : lng ? 1 : fmt_wide;
            if (wide) { u16 w[2] = {(u16)c, 0}; len = w_to_u8(w, 1, tmp, sizeof tmp); }
            else { tmp[0] = (char)c; len = 1; }
            break;
        }
        case 's': case 'S': {
            int wide = *f == 'S' ? !fmt_wide : shrt ? 0 : lng ? 1 : fmt_wide;
            if (wide) {
                const u16 *w = __builtin_va_arg(ap, const u16 *);
                if (!w) str = "(null)";
                else { w_to_u8(w, prec >= 0 ? -1 : -1, wtmp, sizeof wtmp); str = wtmp; }
            } else str = __builtin_va_arg(ap, const char *);
            if (!str) str = "(null)";
            while (str[len] && (prec < 0 || len < prec)) len++;
            break;
        }
        case '%': tmp[0] = '%'; len = 1; break;
        case 0: f--; continue;
        default: tmp[0] = '%'; tmp[1] = *f; len = 2; break;
        }
        int pad = width > len ? width - len : 0;
        if (!left && zero && str == tmp && (tmp[0] == '-' || tmp[0] == '+')) { PUT(tmp[0]); str++; len--; }
        if (!left) while (pad--) PUT(zero && *f != 's' && *f != 'c' ? '0' : ' ');
        for (int i = 0; i < len; i++) PUT(str[i]);
        if (left) while (pad-- > 0) PUT(' ');
    }
    if (size > 0) out[n < size ? n : size - 1] = 0;
#undef PUT
    return n;
}

/* ---------- console window (for programs that print text) ---------- */
static void con_event(cw_window *cw, const cw_event *ev);
static void con_open(w32proc_t *p) {
    if (p->con) return;
    int cwid = text_width(FONT_MONO, "M"), h = FONT_MONO->height;
    char title[64];
    ksnprintf(title, sizeof title, "%s - Consola", p->name);
    p->con = win_create(title, CON_COLS * cwid + 16, CON_ROWS * h + 16, con_event, p, p->icon);
    p->con_dirty = 1;
}
static void con_put(w32proc_t *p, char c) {
    if (c == '\r') { p->con_col = 0; return; }
    if (c == '\t') { do con_put(p, ' '); while (p->con_col % 8); return; }
    if (c == '\n' || p->con_col == CON_COLS) {
        p->con_col = 0;
        if (++p->con_row == CON_ROWS) {
            memmove(p->con_text[0], p->con_text[1], sizeof p->con_text[0] * (CON_ROWS - 1));
            memset(p->con_text[CON_ROWS - 1], 0, sizeof p->con_text[0]);
            p->con_row = CON_ROWS - 1;
        }
        if (c == '\n') return;
    }
    p->con_text[p->con_row][p->con_col++] = c;
    p->con_dirty = 1;
}
static void con_write(const char *s, int n) {
    w32proc_t *p = cur;
    if (!p) return;
    con_open(p);
    for (int i = 0; i < n; i++) con_put(p, s[i]);
    p->con_dirty = 1;
}
static void con_draw(w32proc_t *p) {
    if (!p->con || !p->con_dirty) return;
    p->con_dirty = 0;
    surface_t *s = &p->con->surf;
    fill_rect(s, 0, 0, s->w, s->h, 0xFF0C0C12);
    char line[CON_COLS * 4 + 8];
    for (int r = 0; r < CON_ROWS; r++) {
        to_utf8(p->con_text[r], (int)strlen(p->con_text[r]), line, sizeof line);
        text_draw(s, FONT_MONO, 8, 8 + r * FONT_MONO->height, line, 0xFFE6E6E6);
    }
    win_invalidate(p->con);
}
static void con_event(cw_window *cw, const cw_event *ev) {
    w32proc_t *p = cw->user;
    if (ev->type == CW_CLOSE) { p->con = NULL; if (!p->done) p->killed = 1; }
}

/* ---------- messages ----------
 * Like on Windows, each thread has its own queue: a window's messages go to
 * the thread that made it, and only that thread's GetMessage/PeekMessage sees
 * them. (Qt keeps a hidden window per thread; with one shared queue a worker
 * thread ran the main thread's timers and painting.) */
static u32 main_tid(w32proc_t *p) { return p->threads ? p->threads->id : 0; }
static u32 win_tid(w32proc_t *p, const w32win_t *w) { return w->owner ? w->owner : main_tid(p); }
static w32thread_t *thread_by_id(w32proc_t *p, u32 id) {
    for (w32thread_t *t = p->threads; t; t = t->next) if (t->id == id) return t;
    return NULL;
}
static void post_tid(w32proc_t *p, u32 tid, void *hwnd, u32 msg, u64 wp, i64 lp) {
    if (msg == 0x200 && p->qcount) { /* mouse moves: keep only the newest */
        int k = (p->qhead + p->qcount - 1) % QMAX;
        MSG *last = &p->q[k];
        if (last->message == 0x200 && last->hwnd == hwnd && p->qtid[k] == tid) { last->wparam = wp; last->lparam = lp; return; }
    }
    if (p->qcount == QMAX) return;
    int k = (p->qhead + p->qcount++) % QMAX;
    p->q[k] = (MSG){hwnd, msg, wp, lp, ticks_ms, 0, 0, 0};
    p->qtid[k] = tid;
}
static void post(w32proc_t *p, void *hwnd, u32 msg, u64 wp, i64 lp) {
    u32 tid = 0;
    for (w32win_t *x = p->wins; x; x = x->next) if (x == hwnd) { tid = win_tid(p, x); break; }
    if (!tid) tid = p == cur && cur_th ? cur_th->id : main_tid(p);
    post_tid(p, tid, hwnd, msg, wp, lp);
}
/* the first message queued for thread tid; remove takes it out of the queue */
static int queued(w32proc_t *p, u32 tid, MSG *m, int remove) {
    for (int i = 0; i < p->qcount; i++) {
        int k = (p->qhead + i) % QMAX;
        if (p->qtid[k] != tid) continue;
        if (m) *m = p->q[k];
        if (remove) {
            for (int j = i; j > 0; j--) {
                int a = (p->qhead + j) % QMAX, b = (p->qhead + j - 1) % QMAX;
                p->q[a] = p->q[b];
                p->qtid[a] = p->qtid[b];
            }
            p->qhead = (p->qhead + 1) % QMAX;
            p->qcount--;
        }
        return 1;
    }
    return 0;
}
/* SendMessage to a window of another thread waits until that thread runs it */
typedef struct sent { struct sent *next; void *hwnd; u32 msg, tid; u64 wp; i64 lp; i64 result; int done; } sent_t;
static int has_sent(w32proc_t *p, u32 tid) {
    for (sent_t *s = p->sent; s; s = s->next) if (s->tid == tid) return 1;
    return 0;
}
static void unlink_sent(w32proc_t *p, sent_t *s) {
    for (sent_t **pp = &p->sent; *pp; pp = &(*pp)->next) if (*pp == s) { *pp = s->next; return; }
}
static w32win_t *W(void *h) {
    w32win_t *w = h;
    if (!w || !cur) return NULL;
    for (w32win_t *x = cur->wins; x; x = x->next)
        if (x == w) return w;
    return NULL;
}

static int key_to_vk(int k, int *ch) {
    *ch = 0;
    switch (k) {
    case CW_KEY_BACKSPACE: *ch = 8; return 0x08;
    case CW_KEY_TAB: *ch = 9; return 0x09;
    case CW_KEY_ENTER: *ch = 13; return 0x0D;
    case CW_KEY_ESC: *ch = 27; return 0x1B;
    case CW_KEY_UP: return 0x26;
    case CW_KEY_DOWN: return 0x28;
    case CW_KEY_LEFT: return 0x25;
    case CW_KEY_RIGHT: return 0x27;
    case CW_KEY_HOME: return 0x24;
    case CW_KEY_END: return 0x23;
    case CW_KEY_DELETE: return 0x2E;
    case CW_KEY_PGUP: return 0x21;
    case CW_KEY_PGDN: return 0x22;
    }
    if (k < 256) *ch = k;
    if (k >= 'a' && k <= 'z') return k - 32;
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') || k == ' ') return k;
    return 0;
}
static u32 key_until[256]; /* GetAsyncKeyState: a key counts as held shortly after each press */

/* PC scancodes (set 1) for WM_KEYDOWN's lParam; SDL reads keys from them */
static u32 vk_scan(int vk) {
    static const char *letters = "\x1E\x30\x2E\x20\x12\x21\x22\x23\x17\x24\x25\x26\x32\x31\x18\x19\x10\x13\x1F\x14\x16\x2F\x11\x2D\x15\x2C";
    if (vk >= 'A' && vk <= 'Z') return (u8)letters[vk - 'A'];
    if (vk >= '1' && vk <= '9') return (u32)(vk - '1' + 2);
    switch (vk) {
    case '0': return 0x0B;
    case 0x08: return 0x0E;
    case 0x09: return 0x0F;
    case 0x0D: return 0x1C;
    case 0x1B: return 0x01;
    case ' ': return 0x39;
    case 0x26: return 0x148; /* extended keys: bit 8 */
    case 0x28: return 0x150;
    case 0x25: return 0x14B;
    case 0x27: return 0x14D;
    case 0x24: return 0x147;
    case 0x23: return 0x14F;
    case 0x21: return 0x149;
    case 0x22: return 0x151;
    case 0x2E: return 0x153;
    }
    return 0;
}

static void win_event(cw_window *cw, const cw_event *ev) {
    w32win_t *w = cw->user;
    w32proc_t *p = w->p;
    if (p->done) return;
    i64 lp = (i64)(((u32)(u16)ev->y << 16) | (u16)ev->x);
    switch (ev->type) {
    case CW_MOUSE_DOWN: post(p, w, ev->button == 2 ? 0x204 : 0x201, ev->button == 2 ? 2 : 1, lp); break;
    case CW_MOUSE_UP: post(p, w, ev->button == 2 ? 0x205 : 0x202, 0, lp); break;
    case CW_MOUSE_MOVE: post(p, w, 0x200, ev->button == 1 ? 1 : ev->button == 2 ? 2 : 0, lp); break;
    case CW_KEY: {
        /* Chamows only reports presses: send WM_KEYDOWN, WM_CHAR and WM_KEYUP together */
        int ch, vk = key_to_vk(ev->key, &ch);
        u32 sc = vk_scan(vk);
        i64 klp = 1 | (i64)(sc & 0xFF) << 16 | (sc & 0x100 ? 1 << 24 : 0);
        if (vk) { post(p, w, 0x100, (u64)vk, klp); key_until[vk] = ticks_ms + 150; }
        if (ch) post(p, w, 0x102, (u64)(u8)ch, klp);
        if (vk) post(p, w, 0x101, (u64)vk, klp | (i64)0xC0000000);
        break;
    }
    case CW_RESIZE: /* maximized or restored from the title bar */
        w->dirty = 1;
        post(p, w, 0x3, 0, (i64)((u32)(u16)(cw->y + TITLE_H) << 16 | (u16)cw->x));               /* WM_MOVE */
        post(p, w, 0x5, cw->maximized ? 2 : 0, (i64)((u32)cw->h << 16 | (u32)cw->w));          /* WM_SIZE */
        break;
    case CW_CLOSE:
        w->cw = NULL;
        if (!w->destroyed) post(p, w, 0x10, 0, 0); /* WM_CLOSE */
        break;
    }
}

/* ---------- GDI ---------- */
static u32 rgb(u32 c) { return 0xFF000000 | (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF); }
static u32 syscolor(u64 i) {
    static const u32 c[] = {0xC8C8C8, 0x3A6EA5, 0x000000, 0x808080, 0xF0F0F0, 0xFFFFFF, 0x646464, 0x000000,
                            0x000000, 0x000000, 0xB4B4B4, 0xF4F7FC, 0xABABAB, 0x0078D7, 0xFFFFFF, 0xF0F0F0};
    return 0xFF000000 | c[i & 15];
}
static gobj_t stock_obj[20];
static void stock_init(void) {
    static int done;
    if (done) return;
    done = 1;
    u32 grays[5] = {0xFFFFFFFF, 0xFFC0C0C0, 0xFF808080, 0xFF404040, 0xFF000000};
    for (int i = 0; i < 5; i++) stock_obj[i] = (gobj_t){K_BRUSH, grays[i], 0, 0, 0, {0}, 0, NULL, NULL};
    stock_obj[5] = (gobj_t){K_BRUSH, 0, 0, 0, 1, {0}, 0, NULL, NULL};
    stock_obj[6] = (gobj_t){K_PEN, 0xFFFFFFFF, 1, 0, 0, {0}, 0, NULL, NULL};
    stock_obj[7] = (gobj_t){K_PEN, 0xFF000000, 1, 0, 0, {0}, 0, NULL, NULL};
    stock_obj[8] = (gobj_t){K_PEN, 0, 1, 0, 1, {0}, 0, NULL, NULL};
    for (int i = 10; i <= 17; i++) stock_obj[i] = (gobj_t){K_FONT, 0, 0, CW_FONT_UI, 0, {0}, 0, NULL, NULL};
    stock_obj[18] = (gobj_t){K_BRUSH, 0xFFFFFFFF, 0, 0, 0, {0}, 0, NULL, NULL};
    stock_obj[19] = (gobj_t){K_PEN, 0xFF000000, 1, 0, 0, {0}, 0, NULL, NULL};
}
static void dc_init(dc_t *d, w32win_t *w) {
    stock_init();
    *d = (dc_t){w, &stock_obj[0], &stock_obj[7], &stock_obj[13], NULL, 0xFF000000, 0xFFFFFFFF, 2, 0, 0, 1, 0, 0, {1, 0, 0, 1, 0, 0}};
}
static surface_t *dc_surf(dc_t *d) {
    if (!d) return NULL;
    if (d->bmp) return &d->bmp->surf;
    if (d->w && d->w->cw) { d->w->touched = 1; return &d->w->cw->surf; }
    return NULL;
}
static u32 brush_color(u64 h, int *null) {
    *null = 0;
    if (h && h < 64) return syscolor(h - 1); /* (HBRUSH)(COLOR_x + 1) */
    gobj_t *b = (gobj_t *)h;
    if (!b || b->null) { *null = 1; return 0; }
    return b->color;
}

static void fill_ellipse(surface_t *s, int l, int t, int r, int b, u32 c) {
    double rx = (r - l) / 2.0, ry = (b - t) / 2.0, cx = l + rx, cy = t + ry;
    if (rx <= 0 || ry <= 0) return;
    for (int y = t; y < b; y++) {
        double dy = (y + 0.5 - cy) / ry, k = 1 - dy * dy;
        if (k <= 0) continue;
        double hw;
        __asm__("sqrtsd %1, %0" : "=x"(hw) : "x"(k));
        hw *= rx;
        int x0 = (int)(cx - hw + 0.5), x1 = (int)(cx + hw + 0.5);
        fill_rect(s, x0, y, x1 - x0, 1, c);
    }
}

/* ---------- the Win32 functions ---------- */
/* kernel32 */
static void mods_detach(w32proc_t *p);
static WINAPI void k_ExitProcess(u32 code) { mods_detach(cur); proc_exit((int)code); }
static WINAPI void *k_GetModuleHandleA(const char *n) { return n ? NULL : cur->image; }
static WINAPI const char *k_GetCommandLineA(void) { return cur->cmdline; }
static WINAPI const u16 *k_GetCommandLineW(void) { return cur->wcmdline; }
static WINAPI void k_GetStartupInfoA(u8 *si) { memset(si, 0, 104); *(u32 *)si = 104; }
static WINAPI u64 k_GetStdHandle(u32 n) { return 0x10 + (u64)(-(i32)n - 10); }
static WINAPI int k_WriteFile(u64 h, const char *buf, u32 n, u32 *written, void *ov) {
    if (h == 0x11 || h == 0x12) con_write(buf, (int)n);
    if (written) *written = n;
    return 1;
}
static WINAPI int k_WriteConsoleA(u64 h, const char *buf, u32 n, u32 *written, void *r) { return k_WriteFile(0x11, buf, n, written, r); }
static WINAPI u32 k_GetTickCount(void) { return ticks_ms; }
static WINAPI u64 k_GetTickCount64(void) { return ticks_ms; }
static WINAPI int k_QPC(i64 *v) { *v = (i64)ticks_ms * 1000; return 1; }
static WINAPI int k_QPF(i64 *v) { *v = 1000000; return 1; }
static WINAPI void k_Sleep(u32 ms) {
    flush_windows(cur);
    cur_th->wake_at = ticks_ms + ms;
    yield(W_SLEEP);
}
static WINAPI u32 k_SleepEx(u32 ms, int a) { k_Sleep(ms); return 0; }
static u32 last_error;
static WINAPI u32 k_GetLastError(void) { return last_error; }
static WINAPI void k_SetLastError(u32 e) { last_error = e; }
static WINAPI int k_one(void) { return 1; }
static WINAPI u64 k_zero(void) { return 0; }
static WINAPI int k_VirtualProtect(void *a, u64 n, u32 prot, u32 *old) { if (old) *old = 4; return 1; }
static WINAPI u64 k_VirtualQuery(u64 a, u8 *mbi, u64 len) {
    if (len < 48) return 0;
    memset(mbi, 0, 48);
    *(u64 *)mbi = a & ~0xFFFULL;
    *(u64 *)(mbi + 8) = (u64)cur->image;
    *(u32 *)(mbi + 16) = 4;
    *(u64 *)(mbi + 24) = 0x1000;
    *(u32 *)(mbi + 32) = 0x1000;
    *(u32 *)(mbi + 36) = 4;
    *(u32 *)(mbi + 40) = 0x1000000;
    return 48;
}
static WINAPI void *k_VirtualAlloc(void *a, u64 n, u32 type, u32 prot) {
    u8 *m = p_alloc(n + 4096, 1);
    return m ? (void *)(((uptr)m + 4095) & ~(uptr)4095) : NULL;
}
static WINAPI void *k_HeapAlloc(u64 h, u32 flags, u64 n) { return p_alloc(n, (flags & 8) != 0); }
static WINAPI int k_HeapFree(u64 h, u32 f, void *p) { p_free(p); return 1; }
static WINAPI void *k_HeapReAlloc(u64 h, u32 f, void *p, u64 n) { return p_realloc(p, n); }
static WINAPI u32 k_GetCurrentProcessId(void) { return 4; }
static WINAPI u64 k_GetCurrentProcess(void) { return (u64)-1; }
/* FILETIME (100 ns since 1601) from the real-time clock, read once and moved on by the tick counter */
static u64 boot_ft;
static u64 civil_ft(int year, int month, int day, int hour, int min, int sec) {
    i64 y = year - (month <= 2), era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400, mp = (month + 9) % 12, doy = (153 * mp + 2) / 5 + day - 1;
    i64 days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468; /* since 1970-01-01 */
    i64 secs = days * 86400 + hour * 3600 + min * 60 + sec;
    return (u64)(secs + 11644473600LL) * 10000000ULL;
}
static WINAPI void k_GetSystemTimeAsFileTime(u64 *ft) {
    if (!boot_ft) {
        rtc_time_t t;
        rtc_read(&t);
        boot_ft = civil_ft(t.year, t.month, t.day, t.hour, t.min, t.sec) - (u64)ticks_ms * 10000;
    }
    *ft = boot_ft + (u64)ticks_ms * 10000;
}
static WINAPI void k_GetLocalTime(u16 *st) {
    rtc_time_t t;
    rtc_read(&t);
    u16 v[8] = {(u16)t.year, (u16)t.month, 0, (u16)t.day, (u16)t.hour, (u16)t.min, (u16)t.sec, 0};
    memcpy(st, v, sizeof v);
}
static WINAPI void k_OutputDebugStringA(const char *s) { klog("exe: %s\n", s); }
static int tls_next;
static WINAPI u32 k_TlsAlloc(void) { return tls_next < 64 ? (u32)tls_next++ : 0xFFFFFFFF; }
static WINAPI u64 k_TlsGetValue(u32 i) { return i < 64 ? cur_th->tls_slots[i] : 0; }
static WINAPI int k_TlsSetValue(u32 i, u64 v) { if (i < 64) cur_th->tls_slots[i] = v; return 1; }
static WINAPI int k_lstrlenA(const char *s) { return s ? (int)strlen(s) : 0; }

/* user32 */
static w32win_t *new_win(void) {
    w32win_t *w = p_alloc(sizeof *w, 1);
    w->magic = WIN_MAGIC;
    w->p = cur;
    w->owner = cur_th ? cur_th->id : 0;
    dc_init(&w->dc, w);
    w->next = cur->wins;
    cur->wins = w;
    return w;
}
static u16 register_class(const char *name, WNDPROC proc, u64 bg) {
    if (cur->nclasses == 48) { klog("exe: demasiadas clases de ventana\n"); return 0; }
    wclass_t *c = &cur->classes[cur->nclasses++];
    if ((uptr)name < 0x10000) ksnprintf(c->name, sizeof c->name, "#%u", (unsigned)(uptr)name);
    else strncpy0(c->name, name, sizeof c->name);
    c->proc = proc;
    int null;
    c->bg = bg ? brush_color(bg, &null) : 0;
    if (bg && null) c->bg = 0;
    return (u16)(0xC000 + cur->nclasses - 1);
}
static WINAPI u16 u_RegisterClassA(const u8 *wc) { return register_class(*(const char **)(wc + 64), *(WNDPROC *)(wc + 8), *(u64 *)(wc + 48)); }
static WINAPI u16 u_RegisterClassExA(const u8 *wc) { return register_class(*(const char **)(wc + 64), *(WNDPROC *)(wc + 8), *(u64 *)(wc + 48)); }
static wclass_t *find_class(const char *name) {
    for (int i = 0; i < cur->nclasses; i++) {
        if ((uptr)name < 0x10000) { if ((uptr)name == (uptr)(0xC000 + i)) return &cur->classes[i]; }
        else if (stricmp_(cur->classes[i].name, name) == 0) return &cur->classes[i];
    }
    return NULL;
}
/* WS_POPUP without a caption or sizing border: drawn with no Chamows frame */
static int is_popup(u32 style) { return (style & 0x80000000) && !(style & 0xC40000); }
static WINAPI void *u_CreateWindowExA(u32 ex, const char *cls, const char *title, u32 style, int x, int y, int w, int h,
                                      void *parent, void *menu, void *inst, void *param) {
    wclass_t *c = find_class(cls);
    w32win_t *win = new_win();
    if (!c || (style & 0x40000000)) { /* unknown class or child control: an invisible window */
        if (!c) klog("exe: clase de ventana desconocida\n");
        win->destroyed = 1;
        return win;
    }
    win->proc = c->proc;
    win->bg = c->bg;
    int cwid = (w == (int)0x80000000 || w <= 0) ? 640 : w - 16;
    int chei = (w == (int)0x80000000 || h <= 0 || h == (int)0x80000000) ? 480 : h - 39;
    cwid = imax(120, imin(cwid, SCREEN_W - 20));
    chei = imax(60, imin(chei, SCREEN_H - TASKBAR_H - TITLE_H - 20));
    char t[80];
    to_utf8(title ? title : "", title ? (int)strlen(title) : 0, t, sizeof t);
    win->cw = win_create(t, cwid, chei, win_event, win, c->icon ? c->icon : cur->icon);
    if (!win->cw) { win->destroyed = 1; return NULL; }
    if (!(style & 0x10000000)) win_show(win->cw, 0); /* no WS_VISIBLE: hidden until ShowWindow */
    if (is_popup(style)) { /* menus, tooltips: no frame, exact size and place */
        win_make_popup(win->cw);
        if (w > 0 && w != (int)0x80000000 && h > 0) win_resize(win->cw, w, h);
        if (x != (int)0x80000000) win_place(win->cw, x, y);
        cwid = win->cw->w;
        chei = win->cw->h;
    }
    if (win->bg) fill_rect(&win->cw->surf, 0, 0, cwid, chei, win->bg);
    win->dirty = win->erase = 1;
    u64 cs[10] = {(u64)param};
    if (win->proc(win, 1, 0, (i64)cs) == -1) { win->destroyed = 1; win_close(win->cw); return NULL; }
    return win;
}
static WINAPI void *u_CreateWindowExW(u32 ex, const u16 *cls, const u16 *title, u32 style, int x, int y, int w, int h,
                                      void *parent, void *menu, void *inst, void *param) {
    char c[64], t[80];
    int i = 0;
    if ((uptr)cls < 0x10000) return u_CreateWindowExA(ex, (const char *)cls, "", style, x, y, w, h, parent, menu, inst, param);
    for (; cls[i] && i < 63; i++) c[i] = (char)cls[i];
    c[i] = 0;
    for (i = 0; title && title[i] && i < 79; i++) t[i] = (char)title[i];
    t[i] = 0;
    return u_CreateWindowExA(ex, c, t, style, x, y, w, h, parent, menu, inst, param);
}
static WINAPI int u_ShowWindow(void *h, int cmd) { w32win_t *w = W(h); if (w) w->dirty = 1; return 0; }
static WINAPI i64 u_DefWindowProcA(void *h, u32 msg, u64 wp, i64 lp);
static WINAPI int u_UpdateWindow(void *h) {
    w32win_t *w = W(h);
    if (w && w->dirty && w->proc && !w->destroyed) w->proc(w, 0xF, 0, 0);
    return 1;
}
static WINAPI int u_DestroyWindow(void *h) {
    w32win_t *w = W(h);
    if (!w || w->destroyed) return 0;
    w->destroyed = 1;
    if (w->proc) w->proc(w, 2, 0, 0); /* WM_DESTROY */
    if (w->cw) { cw_window *cw = w->cw; w->cw = NULL; win_close(cw); }
    return 1;
}
static WINAPI void u_PostQuitMessage(int code) { cur_th->quit = 1; cur_th->quit_code = code; cur->exit_code = code; }
static const icon_t *icon_of(u64 h);
static WINAPI i64 u_DefWindowProcA(void *h, u32 msg, u64 wp, i64 lp) {
    w32win_t *w = W(h);
    if (msg == 0x80 && w && w->cw) { /* WM_SETICON: the big icon, or the small one until there is a big one */
        const icon_t *ic = icon_of((u64)lp);
        if (ic && (wp == 1 || !w->big_icon)) { w->cw->icon = ic; w->big_icon |= wp == 1; win_invalidate(w->cw); }
        return 0;
    }
    if (msg == 0x10) u_DestroyWindow(h);
    else if (msg == 0xF && w) { w->dirty = 0; w->erase = 0; }
    return 0;
}
static WINAPI int u_PostMessageA(void *h, u32 msg, u64 wp, i64 lp) { post(cur, h, msg, wp, lp); return 1; }
/* runs the messages other threads sent to this thread's windows */
static void run_sent(void) {
    for (sent_t *s = cur->sent; s; s = s->next) {
        if (s->tid != cur_th->id) continue;
        unlink_sent(cur, s);
        w32win_t *w = W(s->hwnd);
        s->result = w && w->proc && !w->destroyed ? w->proc(w, s->msg, s->wp, s->lp) : 0;
        s->done = 1;
        s = (sent_t *)&cur->sent; /* the list may have changed: start again */
    }
}
static WINAPI i64 u_SendMessageA(void *h, u32 msg, u64 wp, i64 lp) {
    w32win_t *w = W(h);
    if (!w || !w->proc || w->destroyed) return 0;
    w32thread_t *o = thread_by_id(cur, win_tid(cur, w));
    if (!cur_th || !o || o == cur_th || o->done) return w->proc(w, msg, wp, lp);
    sent_t s = {NULL, h, msg, o->id, wp, lp, 0, 0};
    sent_t **pp = &cur->sent;
    while (*pp) pp = &(*pp)->next;
    *pp = &s;
    while (!s.done) {
        run_sent(); /* two threads sending to each other must not get stuck */
        if (s.done) break;
        if (o->done) { unlink_sent(cur, &s); break; }
        yield(W_RUN);
    }
    return s.result;
}
static int pending_paint(MSG *m) {
    for (w32win_t *w = cur->wins; w; w = w->next)
        if (w->dirty && !w->destroyed && w->cw && win_tid(cur, w) == cur_th->id) { *m = (MSG){w, 0xF, 0, 0, ticks_ms, 0, 0, 0}; return 1; }
    return 0;
}
static void timer_popped(MSG *m) {
    w32win_t *w = W(m->hwnd);
    if (m->message == 0x113 && w)
        for (int i = 0; i < 48; i++) if (w->timers[i].used && w->timers[i].id == m->wparam) w->timers[i].pending = 0;
}
/* WM_TIMER is made when it is asked for, like on Windows: only when nothing
 * else is waiting, and never piling up in the queue. */
static int due_timer(w32proc_t *p, u32 tid, MSG *m, int remove) {
    for (w32win_t *w = p->wins; w; w = w->next) {
        if (w->destroyed || win_tid(p, w) != tid) continue;
        for (int i = 0; i < 48; i++) {
            wtimer_t *t = &w->timers[i];
            if (!t->used || (i32)(ticks_ms - t->next) < 0) continue;
            if (m) *m = (MSG){w, 0x113, t->id, 0, ticks_ms, 0, 0, 0};
            if (remove) { t->next = ticks_ms + t->every; }
            return 1;
        }
    }
    return 0;
}
static int next_msg(MSG *m, int remove) {
    run_sent();
    if (queued(cur, cur_th->id, m, remove)) {
        if (remove) timer_popped(m);
        return 1;
    }
    if (pending_paint(m)) return 1;
    if (cur_th->quit) {
        *m = (MSG){NULL, 0x12, (u64)cur_th->quit_code, 0, ticks_ms, 0, 0, 0};
        if (remove) cur_th->quit = 0;
        return 1;
    }
    return due_timer(cur, cur_th->id, m, remove);
}
static WINAPI int u_GetMessageA(MSG *m, void *h, u32 a, u32 b) {
    flush_windows(cur);
    while (!next_msg(m, 1)) yield(W_MSG);
    return m->message != 0x12;
}
static WINAPI int u_PeekMessageA(MSG *m, void *h, u32 a, u32 b, u32 rm) {
    if (next_msg(m, rm & 1)) return 1;
    flush_windows(cur);
    yield(W_RUN); /* let the desktop run between frames of a busy loop */
    return 0;
}
static WINAPI int u_TranslateMessage(const MSG *m) { return 1; }
static WINAPI i64 u_DispatchMessageA(const MSG *m) {
    w32win_t *w = W(m->hwnd);
    if (m->message == 0x113 && m->lparam) { ((void (WINAPI *)(void *, u32, u64, u32))m->lparam)(m->hwnd, 0x113, m->wparam, ticks_ms); return 0; }
    if (!w || !w->proc || w->destroyed) return 0;
    return w->proc(w, m->message, m->wparam, m->lparam);
}
static WINAPI void *u_BeginPaint(void *h, u8 *ps) {
    w32win_t *w = W(h);
    memset(ps, 0, 72);
    if (!w) return NULL;
    if (w->erase && w->bg && w->cw) fill_rect(&w->cw->surf, 0, 0, w->cw->w, w->cw->h, w->bg);
    w->dirty = w->erase = 0;
    w->touched = 1;
    *(void **)ps = &w->dc;
    RECT *r = (RECT *)(ps + 12);
    if (w->cw) { r->right = w->cw->w; r->bottom = w->cw->h; }
    return &w->dc;
}
static WINAPI int u_EndPaint(void *h, const void *ps) { w32win_t *w = W(h); if (w) w->touched = 1; return 1; }
static WINAPI int u_InvalidateRect(void *h, const RECT *r, int erase) {
    w32win_t *w = W(h);
    if (w) { w->dirty = 1; w->erase |= erase; }
    return 1;
}
static WINAPI int u_ValidateRect(void *h, const RECT *r) { w32win_t *w = W(h); if (w) w->dirty = 0; return 1; }
static dc_t screen_dc; /* GetDC(NULL): for measuring text; drawing on it does nothing */
static WINAPI void *u_GetDC(void *h) {
    w32win_t *w = W(h);
    if (w) return &w->dc;
    if (h) return NULL;
    if (!screen_dc.brush) dc_init(&screen_dc, NULL);
    return &screen_dc;
}
static WINAPI int u_ReleaseDC(void *h, void *dc) { w32win_t *w = W(h); if (w) w->touched = 1; return 1; }
static WINAPI int u_GetClientRect(void *h, RECT *r) {
    w32win_t *w = W(h);
    *r = (RECT){0, 0, 0, 0};
    if (w && w->cw) { r->right = w->cw->w; r->bottom = w->cw->h; }
    return 1;
}
static WINAPI int u_AdjustWindowRect(RECT *r, u32 style, int menu) { if (is_popup(style)) return 1; r->left -= 8; r->right += 8; r->top -= 31; r->bottom += 8; return 1; }
static WINAPI int u_AdjustWindowRectEx(RECT *r, u32 style, int menu, u32 ex) { return u_AdjustWindowRect(r, style, menu); }
static WINAPI u64 u_SetTimer(void *h, u64 id, u32 ms, void *fn) {
    w32win_t *w = W(h);
    if (!w) return 0;
    int slot = -1;
    for (int i = 0; i < 48; i++) if (w->timers[i].used && w->timers[i].id == id) slot = i;
    for (int i = 0; i < 48 && slot < 0; i++) if (!w->timers[i].used) slot = i;
    if (slot < 0) { klog("exe: demasiados temporizadores en una ventana\n"); return 0; }
    if (ms < 10) ms = 10;
    w->timers[slot] = (wtimer_t){id, ms, ticks_ms + ms, 1, 0};
    (void)fn;
    return id ? id : 1;
}
static WINAPI int u_KillTimer(void *h, u64 id) {
    w32win_t *w = W(h);
    if (w) for (int i = 0; i < 48; i++) if (w->timers[i].id == id) w->timers[i].used = 0;
    return 1;
}
static WINAPI int u_GetSystemMetrics(int i) { return i == 0 ? SCREEN_W : i == 1 ? SCREEN_H - TASKBAR_H : 0; }
static WINAPI int u_SetWindowTextA(void *h, const char *t) {
    w32win_t *w = W(h);
    if (w && w->cw) { to_utf8(t, (int)strlen(t), w->cw->title, sizeof w->cw->title); invalidate_rect((rect_t){w->cw->x, w->cw->y, w->cw->w, TITLE_H}); }
    return 1;
}
static WINAPI i64 u_GetWindowLongPtrA(void *h, int i) { w32win_t *w = W(h); return w && i == -21 ? w->userdata : 0; }
static WINAPI i64 u_SetWindowLongPtrA(void *h, int i, i64 v) {
    w32win_t *w = W(h);
    if (!w || i != -21) return 0;
    i64 old = w->userdata;
    w->userdata = v;
    return old;
}
static WINAPI i16 u_GetAsyncKeyState(int vk) { return vk >= 0 && vk < 256 && (i32)(key_until[vk] - ticks_ms) > 0 ? (i16)0x8000 : 0; }
static WINAPI int u_IsWindow(void *h) { w32win_t *w = W(h); return w && !w->destroyed; }
static WINAPI int u_wsprintfA(char *out, const char *f, ...) {
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, f);
    int n = vfmt(out, 1024, f, ap);
    __builtin_ms_va_end(ap);
    return n;
}

/* message box: a small Chamows window; the program waits until it closes */
typedef struct { w32proc_t *p; char text[400]; int two; const char *b1, *b2; int r1, r2; } box_t;
static box_t box;
static void box_draw(cw_window *cw) {
    surface_t *s = &cw->surf;
    fill_rect(s, 0, 0, s->w, s->h, 0xFFFFFFFF);
    int y = 22;
    char line[200];
    const char *t = box.text;
    while (*t && y < s->h - 60) {
        int n = 0;
        while (t[n] && t[n] != '\n' && n < 199) n++;
        memcpy(line, t, (size_t)n);
        line[n] = 0;
        text_draw(s, FONT_UI, 24, y, line, 0xFF1B1F2A);
        y += 20;
        t += n + (t[n] == '\n');
    }
    fill_rect(s, 0, s->h - 52, s->w, 52, 0xFFF3F4F8);
    draw_button(s, s->w - 104, s->h - 40, 88, 30, box.b1, 1, 0);
    if (box.two) draw_button(s, s->w - 200, s->h - 40, 88, 30, box.b2, 0, 0);
    win_invalidate(cw);
}
static void box_event(cw_window *cw, const cw_event *ev) {
    int r = 0;
    if (!box.p) { if (ev->type != CW_CLOSE) win_close(cw); return; }
    if (ev->type == CW_MOUSE_DOWN && ev->y >= cw->h - 40 && ev->y < cw->h - 10) {
        if (ev->x >= cw->w - 104 && ev->x < cw->w - 16) r = box.r1;
        else if (box.two && ev->x >= cw->w - 200 && ev->x < cw->w - 112) r = box.r2;
    } else if (ev->type == CW_KEY && (ev->key == CW_KEY_ENTER || ev->key == ' ')) r = box.r1;
    else if (ev->type == CW_KEY && ev->key == CW_KEY_ESC) r = box.two ? box.r2 : box.r1;
    else if (ev->type == CW_CLOSE) { if (box.p->box_open) { box.p->box_open = 0; box.p->box_result = box.two ? box.r2 : box.r1; } return; }
    if (r) { box.p->box_result = r; box.p->box_open = 0; win_close(cw); }
}
static WINAPI int u_MessageBoxA(void *h, const char *text, const char *cap, u32 type) {
    flush_windows(cur);
    while (box.p && box.p->box_open) yield(W_RUN); /* one message box at a time */
    to_utf8(text ? text : "", text ? (int)strlen(text) : 0, box.text, sizeof box.text);
    u32 kind = type & 15;
    box.p = cur;
    box.two = kind == 1 || kind == 4;
    box.b1 = kind == 4 ? "Sí" : "Aceptar";
    box.b2 = kind == 4 ? "No" : "Cancelar";
    box.r1 = kind == 4 ? 6 : 1;
    box.r2 = kind == 4 ? 7 : 2;
    int lines = 1;
    for (const char *t = box.text; *t; t++) lines += *t == '\n';
    int w = 380;
    for (const char *t = box.text; *t;) {
        char line[200]; int n = 0;
        while (t[n] && t[n] != '\n' && n < 199) n++;
        memcpy(line, t, (size_t)n); line[n] = 0;
        w = imax(w, text_width(FONT_UI, line) + 48);
        t += n + (t[n] == '\n');
    }
    char c[80];
    to_utf8(cap ? cap : "Mensaje", cap ? (int)strlen(cap) : 7, c, sizeof c);
    cw_window *cw = win_create(c, imin(w, SCREEN_W - 40), 96 + lines * 20, box_event, NULL, cur->icon);
    if (!cw) return box.r1;
    cur->box_open = 1;
    box_draw(cw);
    while (cur->box_open) yield(W_BOX);
    return cur->box_result;
}

/* gdi32 */
static gobj_t *new_obj(int kind, u32 color) {
    gobj_t *o = p_alloc(sizeof *o, 1);
    o->kind = kind;
    o->color = color;
    return o;
}
static WINAPI void *g_GetStockObject(int i) { stock_init(); return i >= 0 && i < 20 ? &stock_obj[i] : NULL; }
static WINAPI void *g_CreateSolidBrush(u32 c) { return new_obj(K_BRUSH, rgb(c)); }
static WINAPI void *g_CreatePen(int style, int width, u32 c) {
    gobj_t *o = new_obj(K_PEN, rgb(c));
    o->width = width < 1 ? 1 : width;
    o->null = style == 5;
    return o;
}
static WINAPI void *g_CreateFontA(int h, int w, int e, int o, int weight, u32 it, u32 un, u32 st, u32 cs, u32 op, u32 cp, u32 q, u32 pf, const char *face) {
    gobj_t *f = new_obj(K_FONT, 0);
    if (h < 0) h = -h;
    f->font = h >= 20 ? CW_FONT_TITLE : weight >= 600 ? CW_FONT_BOLD : CW_FONT_UI;
    return f;
}
static WINAPI void *g_SelectObject(dc_t *d, gobj_t *o) {
    if (!d || !o) return NULL;
    gobj_t **slot = o->kind == K_BRUSH ? &d->brush : o->kind == K_PEN ? &d->pen : o->kind == K_FONT ? &d->font : &d->bmp;
    gobj_t *old = *slot;
    *slot = o;
    return old;
}
static WINAPI int g_DeleteObject(gobj_t *o) {
    if (!o || (o >= stock_obj && o < stock_obj + 20)) return 1;
    blk_t *b = (blk_t *)o - 1; /* only our own GDI objects (programs also pass DCs and icons here) */
    if (((uptr)o & 15) || (uptr)o < 0x10000 || b->pad != 0xB10CB10C || b->size != sizeof(gobj_t) || o->kind < K_BRUSH || o->kind > K_BITMAP) return 0;
    if (o->kind == K_BITMAP) kfree(o->base ? o->base : o->surf.px);
    if (o->ttf) p_free(o->ttf);
    p_free(o);
    return 1;
}
static WINAPI u32 g_SetTextColor(dc_t *d, u32 c) { u32 o = d->text; d->text = rgb(c); return o; }
static WINAPI u32 g_SetBkColor(dc_t *d, u32 c) { u32 o = d->bk; d->bk = rgb(c); return o; }
static WINAPI int g_SetBkMode(dc_t *d, int m) { int o = d->bkmode; d->bkmode = m; return o; }
static WINAPI u32 g_SetDCBrushColor(dc_t *d, u32 c) { stock_obj[18].color = rgb(c); return 0; }
static WINAPI u32 g_SetDCPenColor(dc_t *d, u32 c) { stock_obj[19].color = rgb(c); return 0; }
static WINAPI int u_FillRect(dc_t *d, const RECT *r, u64 br) {
    surface_t *s = dc_surf(d);
    int null;
    u32 c = brush_color(br, &null);
    if (s && !null) fill_rect(s, r->left, r->top, r->right - r->left, r->bottom - r->top, c);
    return 1;
}
static WINAPI int g_Rectangle(dc_t *d, int l, int t, int r, int b) {
    surface_t *s = dc_surf(d);
    if (!s) return 0;
    if (!d->brush->null) fill_rect(s, l, t, r - l, b - t, d->brush->color);
    if (!d->pen->null) {
        int k = d->pen->width;
        u32 c = d->pen->color;
        fill_rect(s, l, t, r - l, k, c); fill_rect(s, l, b - k, r - l, k, c);
        fill_rect(s, l, t, k, b - t, c); fill_rect(s, r - k, t, k, b - t, c);
    }
    return 1;
}
static WINAPI int g_Ellipse(dc_t *d, int l, int t, int r, int b) {
    surface_t *s = dc_surf(d);
    if (!s) return 0;
    int k = d->pen->null ? 0 : d->pen->width;
    if (k) fill_ellipse(s, l, t, r, b, d->pen->color);
    if (!d->brush->null) fill_ellipse(s, l + k, t + k, r - k, b - k, d->brush->color);
    return 1;
}
static WINAPI int g_RoundRect(dc_t *d, int l, int t, int r, int b, int w, int h) {
    surface_t *s = dc_surf(d);
    if (!s) return 0;
    int k = d->pen->null ? 0 : d->pen->width;
    if (k) fill_round(s, l, t, r - l, b - t, w / 2, d->pen->color);
    if (!d->brush->null) fill_round(s, l + k, t + k, r - l - 2 * k, b - t - 2 * k, imax(0, w / 2 - k), d->brush->color);
    return 1;
}
static WINAPI int g_MoveToEx(dc_t *d, int x, int y, void *old) { d->cx = x; d->cy = y; return 1; }
static WINAPI int g_LineTo(dc_t *d, int x, int y) {
    surface_t *s = dc_surf(d);
    if (s && !d->pen->null) draw_line(s, d->cx, d->cy, x, y, d->pen->width, d->pen->color);
    d->cx = x; d->cy = y;
    return 1;
}
static WINAPI u32 g_SetPixel(dc_t *d, int x, int y, u32 c) { surface_t *s = dc_surf(d); if (s) fill_rect(s, x, y, 1, 1, rgb(c)); return c; }
static const font_t *dc_font(dc_t *d) { return font_by_id(d->font ? d->font->font : CW_FONT_UI); }
static WINAPI int g_TextOutA(dc_t *d, int x, int y, const char *str, int len) {
    surface_t *s = dc_surf(d);
    if (!s) return 0;
    char u[512];
    to_utf8(str, len, u, sizeof u);
    const font_t *f = dc_font(d);
    if (d->bkmode == 2) fill_rect(s, x, y, text_width(f, u), f->height, d->bk);
    text_draw(s, f, x, y, u, d->text);
    return 1;
}
static WINAPI int g_GetTextExtentPoint32A(dc_t *d, const char *str, int len, i32 *size) {
    char u[512];
    to_utf8(str, len, u, sizeof u);
    size[0] = text_width(dc_font(d), u);
    size[1] = dc_font(d)->height;
    return 1;
}
static WINAPI int u_DrawTextA(dc_t *d, const char *str, int len, RECT *r, u32 fmt) {
    char u[512];
    to_utf8(str, len < 0 ? (int)strlen(str) : len, u, sizeof u);
    const font_t *f = dc_font(d);
    int tw = text_width(f, u), th = f->height;
    if (fmt & 0x400) { r->right = r->left + tw; r->bottom = r->top + th; return th; }
    int x = r->left, y = r->top;
    if (fmt & 1) x = (r->left + r->right - tw) / 2; else if (fmt & 2) x = r->right - tw;
    if (fmt & 4) y = (r->top + r->bottom - th) / 2; else if (fmt & 8) y = r->bottom - th;
    surface_t *s = dc_surf(d);
    if (!s) return th;
    if (d->bkmode == 2) fill_rect(s, x, y, tw, th, d->bk);
    text_draw(s, f, x, y, u, d->text);
    return th;
}
static WINAPI void *g_CreateCompatibleDC(dc_t *d) {
    dc_t *n = p_alloc(sizeof *n, 1);
    dc_init(n, NULL);
    return n;
}
static WINAPI int g_DeleteDC(dc_t *d) { p_free(d); return 1; }
static WINAPI void *g_CreateCompatibleBitmap(dc_t *d, int w, int h) {
    gobj_t *b = new_obj(K_BITMAP, 0);
    w = imax(1, imin(w, 4096)); h = imax(1, imin(h, 4096));
    u32 *px = kzalloc((size_t)w * (size_t)h * 4);
    surf_init(&b->surf, px, w, h);
    fill_rect(&b->surf, 0, 0, w, h, 0xFF000000);
    return b;
}
static WINAPI int g_BitBlt(dc_t *dst, int x, int y, int w, int h, dc_t *src, int sx, int sy, u32 rop) {
    surface_t *d = dc_surf(dst), *s = dc_surf(src);
    if (!d) return 0;
    if (rop == 0x42) { fill_rect(d, x, y, w, h, 0xFF000000); return 1; }      /* BLACKNESS */
    if (rop == 0xFF0062) { fill_rect(d, x, y, w, h, 0xFFFFFFFF); return 1; }  /* WHITENESS */
    if (s) blit(d, s, sx, sy, w, h, x, y);
    if (s && src->bmp && src->bmp->dib) { /* programs leave alpha at 0: make the pixels opaque */
        rect_t r = rect_intersect((rect_t){x, y, w, h}, d->clip);
        for (int yy = 0; yy < r.h; yy++)
            for (int xx = 0; xx < r.w; xx++) d->px[(r.y + yy) * d->stride + r.x + xx] |= 0xFF000000;
    }
    return 1;
}
static WINAPI int u_MessageBeep(u32 t) { return 1; }

/* msvcrt */
typedef struct { u8 pad[48]; } FILE_;
static FILE_ iob[3];
static int fmode, commode, errno_;
static char *envp0[1];
static int out_file(void *f) { return f == &iob[1] || f == &iob[2]; }
static WINAPI void *c___iob_func(void) { return iob; }
static WINAPI void *c___acrt_iob_func(u32 i) { return &iob[i < 3 ? i : 1]; }
static WINAPI int c___getmainargs(int *argc, char ***argv, char ***env, int glob, void *si) {
    *argc = cur->argc; *argv = cur->argv; *env = envp0;
    return 0;
}
static WINAPI int *c___p__fmode(void) { return &fmode; }
static WINAPI int *c___p__commode(void) { return &commode; }
static WINAPI int *c__errno(void) { return &errno_; }
static WINAPI void c__initterm(VOIDFN *a, VOIDFN *b) { for (; a < b; a++) if (*a) (*a)(); }
static WINAPI int c__initterm_e(INTFN *a, INTFN *b) { for (; a < b; a++) if (*a) { int r = (*a)(); if (r) return r; } return 0; }
static WINAPI void c_exit(int code) {
    while (cur->n_atexit) cur->atexit_fns[--cur->n_atexit]();
    mods_detach(cur);
    proc_exit(code);
}
static WINAPI void c__exit(int code) { proc_exit(code); }
static WINAPI void c_abort(void) { klog("exe: el programa llamó a abort()\n"); proc_exit(3); }
static WINAPI void c__amsg_exit(int n) { klog("exe: error interno del programa (%d)\n", n); proc_exit(255); }
static WINAPI int c_atexit(VOIDFN f) { if (cur->n_atexit < 32) cur->atexit_fns[cur->n_atexit++] = f; return 0; }
static WINAPI void *c__onexit(VOIDFN f) { c_atexit(f); return (void *)f; }
static WINAPI void *c_malloc(u64 n) { return p_alloc(n ? n : 1, 0); }
static WINAPI void *c_calloc(u64 a, u64 b) { u64 n = a * b; return p_alloc(n ? n : 1, 1); }
static WINAPI void *c_realloc(void *p, u64 n) { return p_realloc(p, n); }
static WINAPI void c_free(void *p) { p_free(p); }
static WINAPI void *c_memcpy(void *d, const void *s, u64 n) { return memcpy(d, s, n); }
static WINAPI void *c_memmove(void *d, const void *s, u64 n) { return memmove(d, s, n); }
static WINAPI void *c_memset(void *d, int c, u64 n) { return memset(d, c, n); }
static WINAPI int c_memcmp(const void *a, const void *b, u64 n) {
    const u8 *x = a, *y = b;
    for (u64 i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
static WINAPI u64 c_strlen(const char *s) { return strlen(s); }
static WINAPI int c_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static WINAPI int c_strncmp(const char *a, const char *b, u64 n) {
    for (; n && *a && *a == *b; n--) { a++; b++; }
    return n ? (u8)*a - (u8)*b : 0;
}
static WINAPI char *c_strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
static WINAPI char *c_strncpy(char *d, const char *s, u64 n) { u64 i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
static WINAPI char *c_strcat(char *d, const char *s) { c_strcpy(d + strlen(d), s); return d; }
static WINAPI char *c_strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; } }
static WINAPI char *c_strrchr(const char *s, int c) { const char *r = NULL; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
static WINAPI int c_atoi(const char *s) {
    int n = 0, neg = 0;
    while (*s == ' ') s++;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return neg ? -n : n;
}
static WINAPI int c_abs(int v) { return v < 0 ? -v : v; }
static u32 seed = 1;
static WINAPI void c_srand(u32 s) { seed = s; }
static WINAPI int c_rand(void) { seed = seed * 214013 + 2531011; return (int)(seed >> 16 & 0x7FFF); }
static WINAPI i64 c_time(i64 *t) {
    rtc_time_t r;
    rtc_read(&r);
    static const int mdays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    i64 days = (r.year - 1970) * 365 + (r.year - 1969) / 4 + mdays[(r.month - 1) % 12] + r.day - 1 + (r.month > 2 && r.year % 4 == 0);
    i64 v = days * 86400 + r.hour * 3600 + r.min * 60 + r.sec;
    if (t) *t = v;
    return v;
}
static WINAPI i32 c_clock(void) { return (i32)ticks_ms; }
static WINAPI int c_puts(const char *s) { con_write(s, (int)strlen(s)); con_write("\n", 1); return 0; }
static WINAPI int c_putchar(int c) { char ch = (char)c; con_write(&ch, 1); return c; }
static WINAPI int c_fputs(const char *s, void *f) { if (out_file(f)) con_write(s, (int)strlen(s)); return 0; }
static WINAPI int c_fputc(int c, void *f) { char ch = (char)c; if (out_file(f)) con_write(&ch, 1); return c; }
static WINAPI u64 c_fwrite(const void *b, u64 sz, u64 n, void *f) { if (out_file(f)) con_write(b, (int)(sz * n)); return n; }
static WINAPI int c_fflush(void *f) { return 0; }
static WINAPI int c_vprintf(const char *f, __builtin_ms_va_list ap) {
    char buf[1024];
    int n = vfmt(buf, sizeof buf, f, ap);
    con_write(buf, imin(n, (int)sizeof buf - 1));
    return n;
}
static WINAPI int c_printf(const char *f, ...) {
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, f);
    int n = c_vprintf(f, ap);
    __builtin_ms_va_end(ap);
    return n;
}
static WINAPI int c_vfprintf(void *fp, const char *f, __builtin_ms_va_list ap) {
    char buf[1024];
    int n = vfmt(buf, sizeof buf, f, ap);
    if (out_file(fp)) con_write(buf, imin(n, (int)sizeof buf - 1));
    return n;
}
static WINAPI int c_fprintf(void *fp, const char *f, ...) {
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, f);
    int n = c_vfprintf(fp, f, ap);
    __builtin_ms_va_end(ap);
    return n;
}
static WINAPI int c_sprintf(char *out, const char *f, ...) {
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, f);
    int n = vfmt(out, 1 << 20, f, ap);
    __builtin_ms_va_end(ap);
    return n;
}
static WINAPI int c_snprintf(char *out, u64 size, const char *f, ...) {
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, f);
    int n = vfmt(out, (int)size, f, ap);
    __builtin_ms_va_end(ap);
    return n;
}
static WINAPI int c_vsnprintf(char *out, u64 size, const char *f, __builtin_ms_va_list ap) { return vfmt(out, (int)size, f, ap); }
static WINAPI int c_vsprintf(char *out, const char *f, __builtin_ms_va_list ap) { return vfmt(out, 1 << 20, f, ap); }
static WINAPI double c_sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static WINAPI double c_fabs(double x) { return x < 0 ? -x : x; }
static WINAPI double c_floor(double x) { double t = (double)(i64)x; return t > x ? t - 1 : t; }
static WINAPI double c_ceil(double x) { double t = (double)(i64)x; return t < x ? t + 1 : t; }
static WINAPI double c_sin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(x)); return r; }
static WINAPI double c_cos(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(x)); return r; }
static WINAPI void *c_signal(int s, void *h) { return NULL; }

static WINAPI int c_one_mb(void) { return 1; }
static struct { const char *dp, *ts, *grp; } lconv_ = {".", "", ""};
static WINAPI void *c_localeconv(void) { return &lconv_; }
static WINAPI const char *c_strerror(int e) { return "error"; }
static WINAPI u64 c_wcslen(const u16 *s) { u64 n = 0; while (s[n]) n++; return n; }
static char *acmdln = "programa.exe";
static int argc_var;
static char **argv_var;

static WINAPI u64 unknown_import(void) { return 0; }
/* Unknown imports get a tiny thunk that puts the function's name in r10, so
 * the first calls of each one show up in the log ("exe: llamada a X"). */
const char *unk_name;
static const char *unk_seen[96];
static int n_unk_seen;
WINAPI u64 unknown_logged(void) {
    const char *n = unk_name;
    for (int i = 0; i < n_unk_seen; i++) if (strcmp(unk_seen[i], n) == 0) return 0;
    if (n_unk_seen < 96) { unk_seen[n_unk_seen++] = n; klog("exe: llamada a %s (no existe, devuelve 0)\n", n); }
    return 0;
}
__attribute__((naked)) static void unknown_stub(void) {
    __asm__ volatile("mov %r10, unk_name(%rip)\n\tjmp unknown_logged");
}
static void *unknown_thunk(const char *name) {
    u32 n = (u32)strlen(name) + 1;
    u8 *t = kmalloc(24 + n); /* small; kept after the program ends, with its own copy of the name */
    if (!t) return (void *)unknown_import;
    memcpy(t + 24, name, n);
    t[0] = 0x49; t[1] = 0xBA; *(u64 *)(t + 2) = (u64)(t + 24);         /* mov r10, name */
    t[10] = 0x49; t[11] = 0xBB; *(u64 *)(t + 12) = (u64)unknown_stub;    /* mov r11, stub */
    t[20] = 0x41; t[21] = 0xFF; t[22] = 0xE3;                            /* jmp r11 */
    return t;
}

/* Call tracing, for finding out what a program expects: with W32_TRACE set to 1
 * every Windows function the program imports from Chamows logs its name and
 * first two arguments ("exe> Name(a, b)"), up to TRACE_MAX lines. */
#define W32_TRACE 0
#define TRACE_MAX 40000
static int trace_lines;
__attribute__((ms_abi)) void trace_log(const char *name, u64 a, u64 b, void *ret) {
    static const char *quiet[] = {"TlsGetValue", "TlsSetValue", "EnterCriticalSection", "LeaveCriticalSection", "HeapAlloc",
        "HeapFree", "HeapReAlloc", "malloc", "free", "realloc", "calloc", "memcpy", "memmove", "memset", "memcmp", "strlen",
        "wcslen", "GetLastError", "SetLastError", "AcquireSRWLockExclusive", "ReleaseSRWLockExclusive", "AcquireSRWLockShared",
        "ReleaseSRWLockShared", "FlsGetValue", "_errno", "GetCurrentThreadId", "QueryPerformanceCounter", "TryAcquireSRWLockExclusive",
        "_aligned_malloc", "_aligned_free", "ceil", "floor", "ceilf", "floorf", "powf", "GetForegroundWindow", "GetSystemMetricsForDpi", "SystemParametersInfoForDpi", "strnlen", "_wgetenv_s", "strcmp", "strncmp", "WakeAllConditionVariable", "WakeConditionVariable", NULL};
    for (int i = 0; quiet[i]; i++) if (strcmp(quiet[i], name) == 0) return;
    if (trace_lines++ < TRACE_MAX) klog("exe> %s(%x, %x) desde %x\n", name, (u32)a, (u32)b, (u32)(uptr)ret);
}
/* Saves the argument registers, logs, restores them and jumps to the function (in r11). */
__attribute__((naked)) static void trace_stub(void) {
    __asm__ volatile(
        "push %rcx\n\tpush %rdx\n\tpush %r8\n\tpush %r9\n\tpush %r11\n\t"
        "sub $0x60, %rsp\n\t"
        "movdqu %xmm0, 0x20(%rsp)\n\tmovdqu %xmm1, 0x30(%rsp)\n\tmovdqu %xmm2, 0x40(%rsp)\n\tmovdqu %xmm3, 0x50(%rsp)\n\t"
        "mov 0x88(%rsp), %r9\n\t"          /* return address of the program's call */
        "mov %rdx, %r8\n\tmov %rcx, %rdx\n\tmov %r10, %rcx\n\t"
        "call trace_log\n\t"
        "movdqu 0x20(%rsp), %xmm0\n\tmovdqu 0x30(%rsp), %xmm1\n\tmovdqu 0x40(%rsp), %xmm2\n\tmovdqu 0x50(%rsp), %xmm3\n\t"
        "add $0x60, %rsp\n\t"
        "pop %r11\n\tpop %r9\n\tpop %r8\n\tpop %rdx\n\tpop %rcx\n\t"
        "jmp *%r11");
}
extern char _text_end[];
static void *trace_thunk(const char *name, void *fn) {
    if (!W32_TRACE || !fn || (char *)fn >= _text_end) return fn; /* variables are left alone */
    u32 n = (u32)strlen(name) + 1;
    u8 *t = kmalloc(36 + n);
    if (!t) return fn;
    memcpy(t + 36, name, n);
    t[0] = 0x49; t[1] = 0xBA; *(u64 *)(t + 2) = (u64)(t + 36);       /* mov r10, name */
    t[10] = 0x49; t[11] = 0xBB; *(u64 *)(t + 12) = (u64)fn;          /* mov r11, fn */
    t[20] = 0x48; t[21] = 0xB8; *(u64 *)(t + 22) = (u64)trace_stub;  /* mov rax, stub */
    t[30] = 0xFF; t[31] = 0xE0;                                       /* jmp rax */
    return t;
}

static const struct { const char *name; void *fn; } exports[] = {
    /* kernel32 */
    {"ExitProcess", k_ExitProcess}, {"GetModuleHandleA", k_GetModuleHandleA}, {"GetModuleHandleW", k_GetModuleHandleA},
    {"GetCommandLineA", k_GetCommandLineA}, {"GetStartupInfoA", k_GetStartupInfoA}, {"GetStdHandle", k_GetStdHandle},
    {"WriteFile", k_WriteFile}, {"WriteConsoleA", k_WriteConsoleA}, {"GetTickCount", k_GetTickCount},
    {"GetTickCount64", k_GetTickCount64}, {"QueryPerformanceCounter", k_QPC}, {"QueryPerformanceFrequency", k_QPF},
    {"Sleep", k_Sleep}, {"SleepEx", k_SleepEx}, {"GetLastError", k_GetLastError}, {"SetLastError", k_SetLastError},
    {"SetUnhandledExceptionFilter", k_zero}, {"InitializeCriticalSection", k_zero}, {"EnterCriticalSection", k_zero},
    {"LeaveCriticalSection", k_zero}, {"DeleteCriticalSection", k_zero}, {"InitializeCriticalSectionAndSpinCount", k_one},
    {"VirtualProtect", k_VirtualProtect}, {"VirtualQuery", k_VirtualQuery}, {"VirtualAlloc", k_VirtualAlloc},
    {"VirtualFree", k_one}, {"GetProcessHeap", k_one}, {"HeapAlloc", k_HeapAlloc}, {"HeapFree", k_HeapFree},
    {"HeapReAlloc", k_HeapReAlloc}, {"GetCurrentProcessId", k_GetCurrentProcessId},
    {"GetCurrentProcess", k_GetCurrentProcess}, {"GetSystemTimeAsFileTime", k_GetSystemTimeAsFileTime},
    {"GetLocalTime", k_GetLocalTime}, {"GetSystemTime", k_GetLocalTime}, {"OutputDebugStringA", k_OutputDebugStringA},
    {"TlsAlloc", k_TlsAlloc}, {"TlsGetValue", k_TlsGetValue}, {"TlsSetValue", k_TlsSetValue}, {"TlsFree", k_one},
    {"IsDebuggerPresent", k_zero}, {"lstrlenA", k_lstrlenA}, {"FreeConsole", k_one}, {"AllocConsole", k_one},
    {"SetConsoleTitleA", k_one}, {"SetConsoleOutputCP", k_one}, {"SetConsoleCP", k_one}, {"GetConsoleMode", k_one},
    /* user32 */
    {"RegisterClassA", u_RegisterClassA}, {"RegisterClassExA", u_RegisterClassExA}, {"RegisterClassW", u_RegisterClassA},
    {"RegisterClassExW", u_RegisterClassExA}, {"CreateWindowExA", u_CreateWindowExA}, {"CreateWindowExW", u_CreateWindowExW},
    {"ShowWindow", u_ShowWindow}, {"UpdateWindow", u_UpdateWindow}, {"DestroyWindow", u_DestroyWindow},
    {"PostQuitMessage", u_PostQuitMessage}, {"DefWindowProcA", u_DefWindowProcA}, {"DefWindowProcW", u_DefWindowProcA},
    {"PostMessageA", u_PostMessageA}, {"SendMessageA", u_SendMessageA}, {"GetMessageA", u_GetMessageA},
    {"GetMessageW", u_GetMessageA}, {"PeekMessageA", u_PeekMessageA}, {"PeekMessageW", u_PeekMessageA},
    {"TranslateMessage", u_TranslateMessage}, {"DispatchMessageA", u_DispatchMessageA}, {"DispatchMessageW", u_DispatchMessageA},
    {"BeginPaint", u_BeginPaint}, {"EndPaint", u_EndPaint}, {"InvalidateRect", u_InvalidateRect}, {"ValidateRect", u_ValidateRect},
    {"GetDC", u_GetDC}, {"ReleaseDC", u_ReleaseDC}, {"GetClientRect", u_GetClientRect}, {"AdjustWindowRect", u_AdjustWindowRect},
    {"AdjustWindowRectEx", u_AdjustWindowRectEx}, {"SetTimer", u_SetTimer}, {"KillTimer", u_KillTimer},
    {"GetSystemMetrics", u_GetSystemMetrics}, {"SetWindowTextA", u_SetWindowTextA}, {"GetWindowLongPtrA", u_GetWindowLongPtrA},
    {"SetWindowLongPtrA", u_SetWindowLongPtrA}, {"GetAsyncKeyState", u_GetAsyncKeyState}, {"GetKeyState", u_GetAsyncKeyState},
    {"IsWindow", u_IsWindow}, {"wsprintfA", u_wsprintfA}, {"MessageBoxA", u_MessageBoxA}, {"MessageBeep", u_MessageBeep},
    {"FillRect", u_FillRect}, {"DrawTextA", u_DrawTextA}, {"LoadCursorA", k_one}, {"LoadCursorW", k_one}, {"LoadIconA", k_one},
    {"LoadIconW", k_one}, {"SetCursor", k_zero}, {"ShowCursor", k_zero}, {"SetFocus", k_zero}, {"SetForegroundWindow", k_one},
    /* gdi32 */
    {"GetStockObject", g_GetStockObject}, {"CreateSolidBrush", g_CreateSolidBrush}, {"CreatePen", g_CreatePen},
    {"CreateFontA", g_CreateFontA}, {"SelectObject", g_SelectObject}, {"DeleteObject", g_DeleteObject},
    {"SetTextColor", g_SetTextColor}, {"SetBkColor", g_SetBkColor}, {"SetBkMode", g_SetBkMode},
    {"SetDCBrushColor", g_SetDCBrushColor}, {"SetDCPenColor", g_SetDCPenColor}, {"Rectangle", g_Rectangle},
    {"Ellipse", g_Ellipse}, {"RoundRect", g_RoundRect}, {"MoveToEx", g_MoveToEx}, {"LineTo", g_LineTo},
    {"SetPixel", g_SetPixel}, {"TextOutA", g_TextOutA}, {"GetTextExtentPoint32A", g_GetTextExtentPoint32A},
    {"CreateCompatibleDC", g_CreateCompatibleDC}, {"DeleteDC", g_DeleteDC}, {"CreateCompatibleBitmap", g_CreateCompatibleBitmap},
    {"BitBlt", g_BitBlt},
    /* msvcrt */
    {"__iob_func", c___iob_func}, {"__acrt_iob_func", c___acrt_iob_func}, {"__getmainargs", c___getmainargs},
    {"__set_app_type", k_zero}, {"__p__fmode", c___p__fmode}, {"__p__commode", c___p__commode}, {"_errno", c__errno},
    {"_initterm", c__initterm}, {"_initterm_e", c__initterm_e}, {"exit", c_exit}, {"_exit", c__exit}, {"_cexit", k_zero},
    {"abort", c_abort}, {"_amsg_exit", c__amsg_exit}, {"atexit", c_atexit}, {"_onexit", c__onexit},
    {"__setusermatherr", k_zero}, {"__lconv_init", k_zero}, {"_lock", k_zero}, {"_unlock", k_zero}, {"_fpreset", k_zero},
    {"signal", c_signal}, {"_set_invalid_parameter_handler", k_zero}, {"_setmode", k_zero},
    {"malloc", c_malloc}, {"calloc", c_calloc}, {"realloc", c_realloc}, {"free", c_free},
    {"memcpy", c_memcpy}, {"memmove", c_memmove}, {"memset", c_memset}, {"memcmp", c_memcmp},
    {"strlen", c_strlen}, {"strcmp", c_strcmp}, {"strncmp", c_strncmp}, {"strcpy", c_strcpy}, {"strncpy", c_strncpy},
    {"strcat", c_strcat}, {"strchr", c_strchr}, {"strrchr", c_strrchr}, {"atoi", c_atoi}, {"abs", c_abs},
    {"srand", c_srand}, {"rand", c_rand}, {"time", c_time}, {"_time64", c_time}, {"clock", c_clock},
    {"puts", c_puts}, {"putchar", c_putchar}, {"fputs", c_fputs}, {"fputc", c_fputc}, {"fwrite", c_fwrite},
    {"fflush", c_fflush}, {"printf", c_printf}, {"vprintf", c_vprintf}, {"fprintf", c_fprintf}, {"vfprintf", c_vfprintf},
    {"sprintf", c_sprintf}, {"snprintf", c_snprintf}, {"_snprintf", c_snprintf}, {"_vsnprintf", c_vsnprintf},
    {"vsnprintf", c_vsnprintf}, {"vsprintf", c_vsprintf}, {"sqrt", c_sqrt}, {"fabs", c_fabs}, {"floor", c_floor},
    {"ceil", c_ceil}, {"sin", c_sin}, {"cos", c_cos}, {"___mb_cur_max_func", c_one_mb}, {"___lc_codepage_func", k_zero},
    {"_ismbblead", k_zero}, {"IsDBCSLeadByteEx", k_zero}, {"MultiByteToWideChar", k_zero}, {"WideCharToMultiByte", k_zero},
    {"localeconv", c_localeconv}, {"strerror", c_strerror}, {"wcslen", c_wcslen}, {"__C_specific_handler", k_zero},
    /* variables (the program reads them through the import table) */
    {"__argc", &argc_var}, {"__argv", &argv_var}, {"GetCommandLineW", k_GetCommandLineW},
    {"_fmode", &fmode}, {"_commode", &commode}, {"__initenv", &envp0}, {"_acmdln", &acmdln},
};

/* ---------- more of Windows: threads, waits, strings, numbers ---------- */
enum { H_EVENT = 1, H_MUTEX, H_SEM, H_THREAD };
typedef struct { u32 magic; int kind, signaled, manual, count, max; w32thread_t *th; } hobj_t;
#define HMAGIC 0x484F424A
static hobj_t *hnew(int kind) { hobj_t *h = p_alloc(sizeof *h, 1); h->magic = HMAGIC; h->kind = kind; return h; }
static hobj_t *H(u64 v) { hobj_t *h = (hobj_t *)v; return v > 0x10000 && !(v & 7) && h->magic == HMAGIC ? h : NULL; }

static WINAPI u64 k_CreateThread(void *sa, u64 stack, u64 start, u64 param, u32 flags, u32 *tid) {
    w32thread_t *t = thread_new(cur, thread_main, start, param, stack > 256 * 1024 ? (u32)stack : 512 * 1024);
    if (!t) return 0;
    hobj_t *h = hnew(H_THREAD);
    h->th = t;
    if (flags & 4) t->suspended = 1; /* CREATE_SUSPENDED: waits for ResumeThread */
    if (tid) *tid = t->id;
    return (u64)h;
}
static WINAPI u64 c__beginthreadex(void *sa, u32 stack, u64 start, u64 param, u32 flags, u32 *tid) { return k_CreateThread(sa, stack, start, param, flags, tid); }
static void mods_thread(w32proc_t *p, u32 reason);
static WINAPI void k_ExitThread(u32 code) { mods_thread(cur, 3); cur_th->exit_code = code; cur_th->done = 1; yield(W_RUN); for (;;) {} }
static WINAPI u64 k_GetCurrentThread(void) { return (u64)-2; }
static w32thread_t *thread_of(u64 v) {
    if (v == (u64)-2) return cur_th;
    hobj_t *h = H(v);
    return h && h->kind == H_THREAD ? h->th : NULL;
}
static WINAPI u32 k_ResumeThread(u64 v) {
    w32thread_t *t = thread_of(v);
    if (!t) return 0xFFFFFFFF;
    u32 was = (u32)t->suspended;
    if (t->suspended) t->suspended--;
    return was;
}
static WINAPI u32 k_SuspendThread(u64 v) {
    w32thread_t *t = thread_of(v);
    if (!t) return 0xFFFFFFFF;
    u32 was = (u32)t->suspended++;
    if (t == cur_th) yield(W_RUN);
    return was;
}
static WINAPI u32 k_GetCurrentThreadId(void) { return cur_th ? cur_th->id : 8; }
static WINAPI u64 k_CreateEventA(void *sa, int manual, int initial, const void *name) {
    hobj_t *h = hnew(H_EVENT);
    h->manual = manual;
    h->signaled = initial;
    return (u64)h;
}
static WINAPI int k_SetEvent(u64 v) { hobj_t *h = H(v); if (h) h->signaled = 1; return 1; }
static WINAPI int k_ResetEvent(u64 v) { hobj_t *h = H(v); if (h) h->signaled = 0; return 1; }
static WINAPI u64 k_CreateMutexA(void *sa, int owned, const void *name) {
    hobj_t *h = hnew(H_MUTEX);
    if (owned) { h->th = cur_th; h->count = 1; }
    return (u64)h;
}
static WINAPI int k_ReleaseMutex(u64 v) { hobj_t *h = H(v); if (h && h->th == cur_th && --h->count <= 0) { h->count = 0; h->th = NULL; } return 1; }
static WINAPI u64 k_CreateSemaphoreA(void *sa, int initial, int max, const void *name) {
    hobj_t *h = hnew(H_SEM);
    h->count = initial;
    h->max = max;
    return (u64)h;
}
static WINAPI int k_ReleaseSemaphore(u64 v, int n, int *prev) {
    hobj_t *h = H(v);
    if (!h) return 0;
    if (prev) *prev = h->count;
    h->count += n;
    if (h->max && h->count > h->max) h->count = h->max;
    return 1;
}
static int try_take(u64 v) {
    hobj_t *h = H(v);
    if (!h) return 0;
    switch (h->kind) {
    case H_EVENT: if (!h->signaled) return 0; if (!h->manual) h->signaled = 0; return 1;
    case H_MUTEX: if (h->th && h->th != cur_th) return 0; h->th = cur_th; h->count++; return 1;
    case H_SEM: if (h->count <= 0) return 0; h->count--; return 1;
    case H_THREAD: return h->th && h->th->done;
    }
    return 0;
}
static WINAPI u32 k_WaitForMultipleObjects(u32 n, const u64 *hs, int all, u32 ms) {
    u32 start = ticks_ms;
    for (;;) {
        if (all) {
            int ok = 1;
            for (u32 i = 0; i < n; i++) { hobj_t *h = H(hs[i]); if (h && h->kind != H_THREAD && !(h->kind == H_EVENT ? h->signaled : h->kind == H_SEM ? h->count > 0 : !h->th || h->th == cur_th)) ok = 0; if (h && h->kind == H_THREAD && !h->th->done) ok = 0; }
            if (ok) { for (u32 i = 0; i < n; i++) try_take(hs[i]); return 0; }
        } else {
            for (u32 i = 0; i < n; i++) if (try_take(hs[i])) return i;
        }
        if (ms == 0 || (ms != 0xFFFFFFFF && ticks_ms - start >= ms)) return 0x102;
        flush_windows(cur);
        yield(W_RUN);
    }
}
static WINAPI u32 k_WaitForSingleObject(u64 h, u32 ms) { return k_WaitForMultipleObjects(1, &h, 0, ms); }
static WINAPI u32 k_WaitForSingleObjectEx(u64 h, u32 ms, int a) { return k_WaitForMultipleObjects(1, &h, 0, ms); }
static WINAPI int k_SleepConditionVariableCS(u64 *cv, void *cs, u32 ms) {
    u64 seen = *cv;
    u32 start = ticks_ms;
    while (*cv == seen) {
        if (ms != 0xFFFFFFFF && ticks_ms - start >= ms) { last_error = 1460; return 0; }
        yield(W_RUN);
    }
    return 1;
}
static WINAPI void k_WakeConditionVariable(u64 *cv) { (*cv)++; }
static WINAPI void k_GetSystemInfo(u8 *si) {
    memset(si, 0, 48);
    *(u16 *)si = 9;                       /* x64 */
    *(u32 *)(si + 4) = 4096;              /* page size */
    *(u64 *)(si + 8) = 0x10000;
    *(u64 *)(si + 16) = 0x7FFFFFFEFFFF;
    *(u64 *)(si + 24) = 1;                /* active processor mask */
    *(u32 *)(si + 32) = 1;                /* processors */
    *(u32 *)(si + 36) = 8664;
    *(u32 *)(si + 40) = 65536;
}
static WINAPI int k_GlobalMemoryStatusEx(u8 *m) {
    u64 total = (u64)total_memory_kb * 1024, avail = total - heap_used();
    *(u32 *)(m + 4) = 50;
    for (int i = 0; i < 6; i++) *(u64 *)(m + 8 + i * 8) = i % 2 ? avail : total;
    return 1;
}
/* Windows 10 22H2. Reporting Windows 11 makes Qt use WinRT display APIs that Chamows lacks (uncaught exception) */
static WINAPI int k_GetVersionExA(u8 *v) { *(u32 *)(v + 4) = 10; *(u32 *)(v + 8) = 0; *(u32 *)(v + 12) = 19045; *(u32 *)(v + 16) = 2; return 1; }
static WINAPI u32 k_RtlGetVersion(u8 *v) { k_GetVersionExA(v); return 0; }
static WINAPI void k_RtlGetNtVersionNumbers(u32 *ma, u32 *mi, u32 *b) { if (ma) *ma = 10; if (mi) *mi = 0; if (b) *b = 19045 | 0xF0000000u; }
static WINAPI int k_IsProcessorFeaturePresent(u32 f) { return f == 6 || f == 10 || f == 3; }
static WINAPI u32 k_GetModuleFileNameA(void *m, char *out, u32 n) { ksnprintf(out, n, "C:\\%s", cur->name); return (u32)strlen(out); }
static WINAPI u32 k_GetModuleFileNameW(void *m, u16 *out, u32 n) {
    char a[64]; ksnprintf(a, sizeof a, "C:\\%s", cur->name);
    u32 i = 0; for (; a[i] && i + 1 < n; i++) out[i] = (u8)a[i];
    out[i] = 0; return i;
}
static WINAPI int k_GetModuleHandleExW(u32 f, const void *n, void **out) { *out = cur->image; return 1; }
static WINAPI int k_lstrlenW(const u16 *s) { int n = 0; while (s && s[n]) n++; return n; }
static WINAPI int k_MulDiv(int a, int b, int c) { return c ? (int)((i64)a * b / c) : -1; }
static WINAPI void k_OutputDebugStringW(const u16 *s) { /* long messages in pieces of 200 */
    char b[201];
    int i = 0;
    while (s[i]) {
        int k = 0;
        for (; s[i] && k < 200; i++, k++) b[k] = s[i] < 128 ? (char)s[i] : '?';
        b[k] = 0;
        klog("exe: %s\n", b);
    }
}
static WINAPI int k_WriteConsoleW(u64 h, const u16 *buf, u32 n, u32 *w, void *r) {
    char tmp[256];
    for (u32 i = 0; i < n;) { u32 k = 0; for (; k < 255 && i < n; k++, i++) tmp[k] = buf[i] < 256 ? (char)buf[i] : '?'; con_write(tmp, (int)k); }
    if (w) *w = n;
    return 1;
}
static WINAPI u32 k_GetFileType(u64 h) { return h >= 0x10 && h <= 0x12 ? 2 : 1; }
static WINAPI void *k_GlobalAlloc(u32 f, u64 n) { return p_alloc(n ? n : 1, 1); }
static WINAPI void *k_GlobalLock(void *p) { return p; }
static WINAPI void *k_GlobalFree(void *p) { p_free(p); return NULL; }
static WINAPI void k_RaiseException_old(u32 code, u32 f, u32 n, const u64 *args) {
    klog("exe: excepción de Windows %x sin manejar; el programa se cierra\n", code);
    proc_exit(255);
}

/* msvcrt: memory, strings and characters */
static WINAPI char *c__strdup(const char *s) { u64 n = strlen(s) + 1; char *d = p_alloc(n, 0); if (d) memcpy(d, s, n); return d; }
static WINAPI int c__stricmp(const char *a, const char *b) { return stricmp_(a, b); }
static WINAPI int c__strnicmp(const char *a, const char *b, u64 n) {
    for (; n; n--, a++, b++) { int d = lower((u8)*a) - lower((u8)*b); if (d || !*a) return d; }
    return 0;
}
static WINAPI char *c__strlwr(char *s) { for (char *p = s; *p; p++) *p = (char)lower(*p); return s; }
static WINAPI char *c__strupr(char *s) { for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32; return s; }
static WINAPI char *c__strrev(char *s) { u64 n = strlen(s); for (u64 i = 0; i < n / 2; i++) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; } return s; }
static WINAPI void *c_memchr(const void *p, int c, u64 n) { const u8 *s = p; for (u64 i = 0; i < n; i++) if (s[i] == (u8)c) return (void *)(s + i); return NULL; }
static WINAPI char *c_strstr(const char *h, const char *n) {
    u64 k = strlen(n);
    for (; *h; h++) if (c_strncmp(h, n, k) == 0) return (char *)h;
    return k ? NULL : (char *)h;
}
static WINAPI u64 c_strspn(const char *s, const char *a) { u64 n = 0; while (s[n] && c_strchr(a, s[n])) n++; return n; }
static WINAPI u64 c_strcspn(const char *s, const char *a) { u64 n = 0; while (s[n] && !c_strchr(a, s[n])) n++; return n; }
static WINAPI char *c_strpbrk(const char *s, const char *a) { for (; *s; s++) if (c_strchr(a, *s)) return (char *)s; return NULL; }
static WINAPI char *c_strncat(char *d, const char *s, u64 n) { char *e = d + strlen(d); u64 i = 0; for (; i < n && s[i]; i++) e[i] = s[i]; e[i] = 0; return d; }
static WINAPI int c_strncpy_s(char *d, u64 dn, const char *s, u64 n) { u64 i = 0; for (; i < n && s[i] && i + 1 < dn; i++) d[i] = s[i]; if (dn) d[i] = 0; return 0; }
static char *tok_next;
static WINAPI char *c_strtok(char *s, const char *d) {
    if (!s) s = tok_next;
    if (!s) return NULL;
    s += c_strspn(s, d);
    if (!*s) { tok_next = NULL; return NULL; }
    char *e = s + c_strcspn(s, d);
    if (*e) { *e = 0; tok_next = e + 1; } else tok_next = NULL;
    return s;
}
static WINAPI u64 c_strxfrm(char *d, const char *s, u64 n) { u64 k = strlen(s); if (n) c_strncpy_s(d, n, s, k); return k; }
static WINAPI int c_isdigit(int c) { return c >= '0' && c <= '9'; }
static WINAPI int c_isalpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static WINAPI int c_isalnum(int c) { return c_isdigit(c) || c_isalpha(c); }
static WINAPI int c_isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static WINAPI int c_isupper(int c) { return c >= 'A' && c <= 'Z'; }
static WINAPI int c_islower(int c) { return c >= 'a' && c <= 'z'; }
static WINAPI int c_isxdigit(int c) { return c_isdigit(c) || ((c | 32) >= 'a' && (c | 32) <= 'f'); }
static WINAPI int c_iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
static WINAPI int c_isgraph(int c) { return c > 32 && c < 127; }
static WINAPI int c_ispunct(int c) { return c_isgraph(c) && !c_isalnum(c); }
static WINAPI int c_tolower(int c) { return c_isupper(c) ? c + 32 : c; }
static WINAPI int c_toupper(int c) { return c_islower(c) ? c - 32 : c; }
static WINAPI u64 c_strtoui64(const char *s, char **end, int base) {
    while (c_isspace(*s)) s++;
    int neg = 0;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 32) == 'x') { s += 2; base = 16; }
    else if (base == 0) base = s[0] == '0' ? 8 : 10;
    u64 v = 0;
    for (;; s++) {
        int d = c_isdigit(*s) ? *s - '0' : c_isalpha(*s) ? (*s | 32) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * (u64)base + (u64)d;
    }
    if (end) *end = (char *)s;
    return neg ? (u64)-(i64)v : v;
}
static WINAPI i64 c_strtoi64(const char *s, char **e, int b) { return (i64)c_strtoui64(s, e, b); }
static WINAPI i32 c_strtol(const char *s, char **e, int b) { return (i32)c_strtoui64(s, e, b); }
static WINAPI u32 c_strtoul(const char *s, char **e, int b) { return (u32)c_strtoui64(s, e, b); }
static WINAPI i32 c_atol(const char *s) { return (i32)c_strtoui64(s, NULL, 10); }
static WINAPI double c_atof(const char *s) {
    char *e;
    double v = (double)c_strtoi64(s, &e, 10), f = 0.1;
    if (*e == '.') for (e++; c_isdigit(*e); e++, f /= 10) v += (v < 0 || *s == '-' ? -1 : 1) * (*e - '0') * f;
    return v;
}
static char *utoa_(u64 v, char *out, int base, int neg) {
    char tmp[72]; int n = 0;
    do { tmp[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[v % (u64)base]; v /= (u64)base; } while (v);
    char *o = out;
    if (neg) *o++ = '-';
    while (n) *o++ = tmp[--n];
    *o = 0;
    return out;
}
static WINAPI char *c__itoa(int v, char *o, int b) { return utoa_(v < 0 && b == 10 ? (u64)-(i64)v : (u32)v, o, b, v < 0 && b == 10); }
static WINAPI char *c__ultoa(u32 v, char *o, int b) { return utoa_(v, o, b, 0); }
static WINAPI char *c__i64toa(i64 v, char *o, int b) { return utoa_(v < 0 && b == 10 ? (u64)-v : (u64)v, o, b, v < 0 && b == 10); }
static WINAPI char *c__ui64toa(u64 v, char *o, int b) { return utoa_(v, o, b, 0); }
static WINAPI int c__vscprintf(const char *f, __builtin_ms_va_list ap) { char tmp[4]; return vfmt(tmp, 1, f, ap); }
static WINAPI u64 c_div(int a, int b) { int q = b ? a / b : 0, r = b ? a % b : 0; return (u32)q | (u64)(u32)r << 32; }
static WINAPI void c_qsort(void *base, u64 n, u64 sz, int (WINAPI *cmp)(const void *, const void *)) {
    u8 *b = base, tmp[256];
    if (sz > sizeof tmp) return;
    for (u64 i = 1; i < n; i++) /* insertion sort: small and stable enough */
        for (u64 j = i; j > 0 && cmp(b + (j - 1) * sz, b + j * sz) > 0; j--) {
            memcpy(tmp, b + j * sz, sz); memcpy(b + j * sz, b + (j - 1) * sz, sz); memcpy(b + (j - 1) * sz, tmp, sz);
        }
}
static WINAPI void *c_bsearch(const void *key, const void *base, u64 n, u64 sz, int (WINAPI *cmp)(const void *, const void *)) {
    u64 lo = 0, hi = n;
    while (lo < hi) {
        u64 mid = (lo + hi) / 2;
        int r = cmp(key, (const u8 *)base + mid * sz);
        if (!r) return (void *)((const u8 *)base + mid * sz);
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
static WINAPI void c__assert(const char *m, const char *f, u32 line) { klog("exe: assert falló: %s (%s:%u)\n", m, f, line); proc_exit(3); }
static WINAPI char *c_setlocale(int c, const char *l) { return "C"; }
static WINAPI char *c_getenv(const char *n) { return NULL; }
static WINAPI int c__write(int fd, const void *b, u32 n) { if (fd == 1 || fd == 2) con_write(b, (int)n); return (int)n; }
static WINAPI int c__fileno(void *f) { return f == &iob[0] ? 0 : f == &iob[1] ? 1 : f == &iob[2] ? 2 : -1; }
static WINAPI int c_minus1(void) { return -1; }
static WINAPI void *c_null(void) { return NULL; }
static WINAPI int c_fputwc(u16 c, void *f) { char ch = c < 256 ? (char)c : '?'; if (out_file(f)) con_write(&ch, 1); return c; }
static WINAPI int c_fputws(const u16 *s, void *f) { while (*s) c_fputwc(*s++, f); return 0; }
static WINAPI char *c__fullpath(char *out, const char *p, u64 n) { if (!out) out = p_alloc(260, 1); strncpy0(out, p, n ? n : 260); return out; }
/* wide strings */
static WINAPI u64 c_wcscmp(const u16 *a, const u16 *b) { while (*a && *a == *b) { a++; b++; } return (u64)(i64)(*a - *b); }
static WINAPI int c_wcsncmp(const u16 *a, const u16 *b, u64 n) { for (; n; n--, a++, b++) { if (*a != *b) return *a - *b; if (!*a) break; } return 0; }
static WINAPI u16 *c_wcscpy(u16 *d, const u16 *s) { u16 *r = d; while ((*d++ = *s++)) {} return r; }
static WINAPI u16 *c_wcscat(u16 *d, const u16 *s) { c_wcscpy(d + c_wcslen(d), s); return d; }
static WINAPI int c_wcscpy_s(u16 *d, u64 n, const u16 *s) { u64 i = 0; for (; s[i] && i + 1 < n; i++) d[i] = s[i]; if (n) d[i] = 0; return 0; }
static WINAPI int c_wcsncpy_s(u16 *d, u64 n, const u16 *s, u64 k) { u64 i = 0; for (; i < k && s[i] && i + 1 < n; i++) d[i] = s[i]; if (n) d[i] = 0; return 0; }
static WINAPI u16 *c_wcsrchr(const u16 *s, u16 c) { const u16 *r = NULL; for (;; s++) { if (*s == c) r = s; if (!*s) return (u16 *)r; } }
static WINAPI u16 *c_wcsstr(const u16 *h, const u16 *n) { u64 k = c_wcslen(n); for (; *h; h++) if (!c_wcsncmp(h, n, k)) return (u16 *)h; return k ? NULL : (u16 *)h; }
static WINAPI int c__wcsicmp(const u16 *a, const u16 *b) { while (*a && lower(*a) == lower(*b)) { a++; b++; } return lower(*a) - lower(*b); }
static WINAPI int c__wcsnicmp(const u16 *a, const u16 *b, u64 n) { for (; n; n--, a++, b++) { int d = lower(*a) - lower(*b); if (d || !*a) return d; } return 0; }
static WINAPI u16 c_towlower(u16 c) { return (u16)lower(c); }
static WINAPI u16 c_towupper(u16 c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static WINAPI i32 c_wcstol(const u16 *s, u16 **e, int b) { char a[64]; int i = 0; for (; s[i] && i < 63; i++) a[i] = (char)s[i]; a[i] = 0; char *ae; i32 v = (i32)c_strtoui64(a, &ae, b); if (e) *e = (u16 *)(s + (ae - a)); return v; }
static WINAPI u64 c_mbstowcs(u16 *d, const char *s, u64 n) { u64 i = 0; for (; s[i] && (!d || i < n); i++) if (d) d[i] = (u8)s[i]; if (d && i < n) d[i] = 0; return i; }
static WINAPI u64 c_wcstombs(char *d, const u16 *s, u64 n) { u64 i = 0; for (; s[i] && (!d || i < n); i++) if (d) d[i] = s[i] < 256 ? (char)s[i] : '?'; if (d && i < n) d[i] = 0; return i; }
static WINAPI int c_mbstowcs_s(u64 *r, u16 *d, u64 dn, const char *s, u64 n) { u64 k = c_mbstowcs(d, s, dn ? dn - 1 : 0); if (d && dn) d[imin((int)k, (int)dn - 1)] = 0; if (r) *r = k + 1; return 0; }
static WINAPI int c_wcstombs_s(u64 *r, char *d, u64 dn, const u16 *s, u64 n) { u64 k = c_wcstombs(d, s, dn ? dn - 1 : 0); if (d && dn) d[imin((int)k, (int)dn - 1)] = 0; if (r) *r = k + 1; return 0; }
/* math with the x87 unit */
static WINAPI double c_atan2_(double y, double x) { double r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
static WINAPI double c_atan(double x) { return c_atan2_(x, 1.0); }
static WINAPI double c_asin(double x) { return c_atan2_(x, c_sqrt(1 - x * x)); }
static WINAPI double c_acos(double x) { return c_atan2_(c_sqrt(1 - x * x), x); }
static WINAPI double c_tan(double x) { double r; __asm__("fptan; fstp %%st(0)" : "=t"(r) : "0"(x)); return r; }
static WINAPI double c_log10(double x) { double r; __asm__("fldlg2; fxch; fyl2x" : "=t"(r) : "0"(x)); return r; }
static double exp_(double x) {
    double r;
    __asm__("fldl2e; fmulp; fld %%st(0); frndint; fsub %%st, %%st(1); fxch; f2xm1; fld1; faddp; fscale; fstp %%st(1)" : "=t"(r) : "0"(x));
    return r;
}
static WINAPI double c_sinh(double x) { return (exp_(x) - exp_(-x)) / 2; }
static WINAPI double c_cosh(double x) { return (exp_(x) + exp_(-x)) / 2; }
static WINAPI double c_tanh(double x) { double a = exp_(x), b = exp_(-x); return (a - b) / (a + b); }
/* time */
static struct { int sec, min, hour, mday, mon, year, wday, yday, isdst; } tm_;
static WINAPI void *c__gmtime64(const i64 *t) {
    i64 v = *t, days = v / 86400, rem = v % 86400;
    tm_.hour = (int)(rem / 3600); tm_.min = (int)(rem / 60 % 60); tm_.sec = (int)(rem % 60);
    tm_.wday = (int)((days + 4) % 7);
    int y = 1970;
    for (;;) { int yd = 365 + (y % 4 == 0); if (days < yd) break; days -= yd; y++; }
    tm_.year = y - 1900; tm_.yday = (int)days;
    static const int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int m = 0;
    for (; m < 11; m++) { int d = md[m] + (m == 1 && y % 4 == 0); if (days < d) break; days -= d; }
    tm_.mon = m; tm_.mday = (int)days + 1; tm_.isdst = 0;
    return &tm_;
}
static WINAPI i64 c__mktime64(void *tmv) {
    int *t = tmv;
    static const int cum[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int y = t[5] + 1900;
    i64 days = (i64)(y - 1970) * 365 + (y - 1969) / 4 + cum[t[4] % 12] + t[3] - 1 + (t[4] > 1 && y % 4 == 0);
    return days * 86400 + t[2] * 3600 + t[1] * 60 + t[0];
}
static WINAPI double c_difftime(i64 a, i64 b) { return (double)(a - b); }
static WINAPI u64 c_strftime(char *o, u64 n, const char *f, const void *t) { if (n) o[0] = 0; return 0; }
/* setjmp / longjmp (our own layout; the program only hands the buffer back) */
__attribute__((naked, ms_abi)) static int c__setjmp(void *buf, void *frame) {
    __asm__ volatile(
        "mov %rbx, (%rcx)\n lea 8(%rsp), %rax\n mov %rax, 8(%rcx)\n mov %rbp, 16(%rcx)\n mov %rsi, 24(%rcx)\n"
        "mov %rdi, 32(%rcx)\n mov %r12, 40(%rcx)\n mov %r13, 48(%rcx)\n mov %r14, 56(%rcx)\n mov %r15, 64(%rcx)\n"
        "mov (%rsp), %rax\n mov %rax, 72(%rcx)\n"
        "movdqu %xmm6, 80(%rcx)\n movdqu %xmm7, 96(%rcx)\n movdqu %xmm8, 112(%rcx)\n movdqu %xmm9, 128(%rcx)\n"
        "movdqu %xmm10, 144(%rcx)\n movdqu %xmm11, 160(%rcx)\n movdqu %xmm12, 176(%rcx)\n movdqu %xmm13, 192(%rcx)\n"
        "movdqu %xmm14, 208(%rcx)\n movdqu %xmm15, 224(%rcx)\n xor %eax, %eax\n ret\n");
}
__attribute__((naked, ms_abi)) static void c_longjmp(void *buf, int val) {
    __asm__ volatile(
        "mov %edx, %eax\n test %eax, %eax\n jnz 1f\n inc %eax\n1:\n"
        "mov (%rcx), %rbx\n mov 16(%rcx), %rbp\n mov 24(%rcx), %rsi\n mov 32(%rcx), %rdi\n"
        "mov 40(%rcx), %r12\n mov 48(%rcx), %r13\n mov 56(%rcx), %r14\n mov 64(%rcx), %r15\n"
        "movdqu 80(%rcx), %xmm6\n movdqu 96(%rcx), %xmm7\n movdqu 112(%rcx), %xmm8\n movdqu 128(%rcx), %xmm9\n"
        "movdqu 144(%rcx), %xmm10\n movdqu 160(%rcx), %xmm11\n movdqu 176(%rcx), %xmm12\n movdqu 192(%rcx), %xmm13\n"
        "movdqu 208(%rcx), %xmm14\n movdqu 224(%rcx), %xmm15\n"
        "mov 8(%rcx), %rsp\n jmp *72(%rcx)\n");
}

#include "wfiles.inc"
#include "wgui.inc"
#include "wsock.inc"
#include "wsync.inc"
#include "wcrt.inc"
#include "wmisc.inc"
#include "wuser.inc"
#include "wfont.inc"
#include "wtext.inc"
#include "wicon.inc"

static const struct { const char *name; void *fn; } exports2[] = {
    {"CreateThread", k_CreateThread}, {"_beginthreadex", c__beginthreadex}, {"_endthreadex", k_ExitThread},
    {"ExitThread", k_ExitThread}, {"GetCurrentThread", k_GetCurrentThread}, {"GetCurrentThreadId", k_GetCurrentThreadId},
    {"CreateEventA", k_CreateEventA}, {"CreateEventW", k_CreateEventA}, {"SetEvent", k_SetEvent}, {"ResetEvent", k_ResetEvent},
    {"CreateMutexA", k_CreateMutexA}, {"CreateMutexW", k_CreateMutexA}, {"ReleaseMutex", k_ReleaseMutex},
    {"CreateSemaphoreA", k_CreateSemaphoreA}, {"CreateSemaphoreW", k_CreateSemaphoreA}, {"ReleaseSemaphore", k_ReleaseSemaphore},
    {"WaitForSingleObject", k_WaitForSingleObject}, {"WaitForSingleObjectEx", k_WaitForSingleObjectEx},
    {"WaitForMultipleObjects", k_WaitForMultipleObjects}, {"CloseHandle", k_one}, {"TryEnterCriticalSection", k_one},
    {"InitializeCriticalSectionEx", k_one}, {"AcquireSRWLockExclusive", k_zero}, {"ReleaseSRWLockExclusive", k_zero},
    {"InitializeConditionVariable", k_zero}, {"SleepConditionVariableCS", k_SleepConditionVariableCS},
    {"WakeConditionVariable", k_WakeConditionVariable}, {"WakeAllConditionVariable", k_WakeConditionVariable},
    {"SetThreadPriority", k_one}, {"GetThreadPriority", k_zero}, {"ResumeThread", k_ResumeThread}, {"SuspendThread", k_SuspendThread},
    {"SetThreadExecutionState", k_one}, {"SetProcessAffinityMask", k_one}, {"GetSystemInfo", k_GetSystemInfo},
    {"GlobalMemoryStatusEx", k_GlobalMemoryStatusEx}, {"GetVersionExA", k_GetVersionExA}, {"GetVersionExW", k_GetVersionExA}, {"RtlGetVersion", k_RtlGetVersion},
    {"RtlGetNtVersionNumbers", k_RtlGetNtVersionNumbers},
    {"VerSetConditionMask", k_zero}, {"VerifyVersionInfoW", k_one}, {"IsProcessorFeaturePresent", k_IsProcessorFeaturePresent},
    {"GetModuleFileNameA", k_GetModuleFileNameA}, {"GetModuleFileNameW", k_GetModuleFileNameW},
    {"GetModuleHandleExW", k_GetModuleHandleExW}, {"LoadLibraryA", k_zero}, {"LoadLibraryW", k_zero},
    {"LoadLibraryExW", k_zero}, {"GetProcAddress", k_zero}, {"FreeLibrary", k_one}, {"GetEnvironmentVariableA", k_zero},
    {"SetEnvironmentVariableA", k_one}, {"SetErrorMode", k_zero}, {"lstrlenW", k_lstrlenW}, {"MulDiv", k_MulDiv},
    {"OutputDebugStringW", k_OutputDebugStringW}, {"WriteConsoleW", k_WriteConsoleW}, {"SetConsoleTitleW", k_one},
    {"GetFileType", k_GetFileType}, {"GlobalAlloc", k_GlobalAlloc}, {"GlobalLock", k_GlobalLock}, {"GlobalUnlock", k_one},
    {"GlobalFree", k_GlobalFree}, {"LocalFree", k_zero}, {"RaiseException", k_RaiseException_old},
    {"_strdup", c__strdup}, {"_stricmp", c__stricmp}, {"_strnicmp", c__strnicmp}, {"_strlwr", c__strlwr},
    {"_strupr", c__strupr}, {"_strrev", c__strrev}, {"memchr", c_memchr}, {"strstr", c_strstr}, {"strspn", c_strspn},
    {"strcspn", c_strcspn}, {"strpbrk", c_strpbrk}, {"strncat", c_strncat}, {"strncpy_s", c_strncpy_s}, {"strtok", c_strtok},
    {"strcoll", c_strcmp}, {"strxfrm", c_strxfrm}, {"isdigit", c_isdigit}, {"isalpha", c_isalpha}, {"isalnum", c_isalnum},
    {"isspace", c_isspace}, {"isupper", c_isupper}, {"islower", c_islower}, {"isxdigit", c_isxdigit}, {"iscntrl", c_iscntrl},
    {"isgraph", c_isgraph}, {"ispunct", c_ispunct}, {"tolower", c_tolower}, {"toupper", c_toupper},
    {"_strtoui64", c_strtoui64}, {"_strtoi64", c_strtoi64}, {"strtol", c_strtol}, {"strtoul", c_strtoul}, {"atol", c_atol},
    {"atof", c_atof}, {"_itoa", c__itoa}, {"_ltoa", c__itoa}, {"_ultoa", c__ultoa}, {"_i64toa", c__i64toa},
    {"_ui64toa", c__ui64toa}, {"_vscprintf", c__vscprintf}, {"div", c_div}, {"qsort", c_qsort}, {"bsearch", c_bsearch},
    {"_assert", c__assert}, {"setlocale", c_setlocale}, {"getenv", c_getenv}, {"_write", c__write}, {"_fileno", c__fileno},
    {"fputwc", c_fputwc}, {"fputws", c_fputws}, {"_fullpath", c__fullpath}, {"wcscmp", c_wcscmp}, {"wcscoll", c_wcscmp},
    {"wcsncmp", c_wcsncmp}, {"wcscpy", c_wcscpy}, {"wcscat", c_wcscat}, {"wcscpy_s", c_wcscpy_s}, {"wcsncpy_s", c_wcsncpy_s},
    {"wcsrchr", c_wcsrchr}, {"wcsstr", c_wcsstr}, {"_wcsicmp", c__wcsicmp}, {"_wcsnicmp", c__wcsnicmp},
    {"towlower", c_towlower}, {"towupper", c_towupper}, {"wcstol", c_wcstol}, {"mbstowcs", c_mbstowcs},
    {"wcstombs", c_wcstombs}, {"mbstowcs_s", c_mbstowcs_s}, {"wcstombs_s", c_wcstombs_s}, {"atan", c_atan},
    {"asin", c_asin}, {"acos", c_acos}, {"tan", c_tan}, {"log10", c_log10}, {"sinh", c_sinh}, {"cosh", c_cosh},
    {"tanh", c_tanh}, {"_gmtime64", c__gmtime64}, {"_localtime64", c__gmtime64}, {"_mktime64", c__mktime64},
    {"difftime", c_difftime}, {"strftime", c_strftime}, {"_setjmp", c__setjmp}, {"longjmp", c_longjmp},
    /* files: not yet (next phase) */
    {"fopen", c_null}, {"_wfopen", c_null}, {"_fsopen", c_null}, {"_fdopen", c_null}, {"freopen", c_null},
    {"_open", c_minus1}, {"_sopen", c_minus1}, {"_read", c_minus1}, {"_close", k_zero}, {"_lseeki64", c_minus1},
    {"_filelengthi64", c_minus1}, {"_get_osfhandle", c_minus1}, {"_stat64", c_minus1}, {"_wstat64", c_minus1},
    {"_fstat64", c_minus1}, {"_unlink", c_minus1}, {"_wunlink", c_minus1}, {"remove", c_minus1}, {"_wremove", c_minus1},
    {"rename", c_minus1}, {"_wrename", c_minus1}, {"fclose", k_zero}, {"fread", k_zero}, {"fgets", c_null},
    {"fgetc", c_minus1}, {"getc", c_minus1}, {"ungetc", c_minus1}, {"fseek", c_minus1}, {"ftell", c_minus1},
    {"feof", k_one}, {"ferror", k_zero}, {"clearerr", k_zero}, {"setvbuf", k_zero}, {"fgetpos", c_minus1},
    {"fsetpos", c_minus1}, {"CreateFileA", c_minus1}, {"CreateFileW", c_minus1}, {"FindFirstFileA", c_minus1},
    {"FindFirstFileW", c_minus1}, {"GetFileAttributesW", c_minus1},
};

/* ---------- loader ---------- */
#include "wdll.inc"
#include "wseh.inc"

/* A function of the Win32 layer, by name; NULL if Chamows does not have it. */
static void *builtin_lookup(const char *name) {
#define LOOK(t) for (u32 j = 0; j < sizeof t / sizeof t[0]; j++) if (strcmp(t[j].name, name) == 0) return t[j].fn;
    LOOK(exports_icon) LOOK(exports_text) LOOK(exports_seh) LOOK(exports_sync) LOOK(exports_crt) LOOK(exports_font) LOOK(exports_misc) LOOK(exports_user) LOOK(exports_dll) LOOK(exports_files) LOOK(exports_sock) LOOK(exports_gui) LOOK(exports) LOOK(exports2)
#undef LOOK
    return NULL;
}

/* The .exe: placed in memory, then its DLLs and imports are linked. */
static const char *pe_load(w32proc_t *p, const u8 *f, u32 size) {
    const char *err = NULL;
    w32mod_t *m = kzalloc(sizeof *m);
    if (!m) return "no hay memoria suficiente";
    m->image = pe_map(f, size, &m->raw, &m->size, &err);
    if (!m->image) { kfree(m); return err; }
    dll_base(p->name, m->name, sizeof m->name);
    if (p->exedir[0]) ksnprintf(m->path, sizeof m->path, "%s/%s", p->exedir, p->name);
    else strncpy0(m->path, p->name, sizeof m->path);
    m->refs = 1000000;
    p->exe = m;
    p->mods = m;
    p->raw = m->raw;
    p->image = m->image;
    if (link_imports(p, m, 0, &err) < 0) return err;
    link_delay_imports(m);
    tls_setup(p, m);
    const u8 *oh = opt_header(m->image);
    p->entry = (u64)m->image + RD32(oh + 16);
    klog("exe: %s cargado en 0x%x (%s)\n", p->name, (u32)(uptr)m->image, RD16(oh + 68) == 2 ? "ventanas" : "consola");
    return RD16(oh + 68) == 3 ? "consola" : NULL;
}

static void fiber_main(void) {
    w32proc_t *p = cur;
    /* the DLLs start first (DllMain), then the .exe's TLS callbacks and main */
    if (mods_init(p, (void *)1) < 0) {
        notify(p->icon, "No se pudo abrir el programa", "Una de sus DLL no pudo iniciarse.");
        proc_exit(-1);
    }
    if (p->exe->tls_cb)
        for (u64 *cb = (u64 *)p->exe->tls_cb; *cb; cb++)
            ((void (WINAPI *)(void *, u32, void *))*cb)(p->image, 1, NULL);
    int rc = ((INTFN)p->entry)();
    proc_exit(rc);
}
static void thread_main(void) {
    w32thread_t *t = cur_th;
    mods_thread(t->p, 2); /* DLL_THREAD_ATTACH */
    u32 rc = ((u32 (WINAPI *)(u64))t->start)(t->param);
    mods_thread(t->p, 3);
    t->exit_code = rc;
    t->done = 1;
    yield(W_RUN);
    for (;;) {}
}
/* A thread: its own stack, TEB and thread-local storage. It runs when the
 * desktop loop gets to it and gives the CPU back at its next wait. */
static w32thread_t *thread_new(w32proc_t *p, void (*entry)(void), u64 start, u64 param, u32 stack_size) {
    w32thread_t *t = kzalloc(sizeof *t);
    if (!t) return NULL;
    t->p = p;
    t->stack = kmalloc(stack_size);
    t->teb = kzalloc(0x1000);
    if (!t->stack || !t->teb) { kfree(t->stack); kfree(t->teb); kfree(t); return NULL; }
    for (int i = 0; i < p->ntls; i++) {
        t->tlsb[i] = kzalloc(p->tlsm[i].len + 16);
        if (t->tlsb[i] && p->tlsm[i].tmpl) memcpy(t->tlsb[i], p->tlsm[i].tmpl, p->tlsm[i].len);
        *(u64 *)(t->teb + 0x800 + 8 * i) = (u64)t->tlsb[i];
    }
    uptr top = ((uptr)t->stack + stack_size) & ~(uptr)15;
    *(u64 *)(t->teb + 0x08) = top;
    *(u64 *)(t->teb + 0x10) = (u64)t->stack;
    *(u64 *)(t->teb + 0x30) = (u64)t->teb;
    *(u64 *)(t->teb + 0x40) = 4;
    t->id = next_tid += 4;
    *(u64 *)(t->teb + 0x48) = t->id;
    *(u64 *)(t->teb + 0x58) = (u64)(t->teb + 0x800); /* TLS array */
    *(u64 *)(t->teb + 0x60) = (u64)p->peb;
    t->rsp = top - 64;
    memset((void *)t->rsp, 0, 64);
    *(u64 *)(t->rsp + 48) = (u64)entry;
    t->start = start;
    t->param = param;
    w32thread_t **pp = &p->threads; /* keep creation order */
    while (*pp) pp = &(*pp)->next;
    *pp = t;
    return t;
}

int pe_run(const u8 *data, u32 size, const char *name, const icon_t *icon) { return pe_run_args(data, size, name, icon, ""); }
char pe_dir[160];
int pe_cwd_root; /* started from the terminal: the current folder is the terminal's (the root) */
/* Like Windows, the first 64 KB are not mapped while .exe programs run, so a
 * NULL pointer crashes at once instead of quietly reading the BIOS's leftovers.
 * The first 2 MB move from one big page to 512 small ones. */
extern u64 pd[];
static u64 low_pt[512] __attribute__((aligned(4096)));
static void null_guard(void) {
    if (low_pt[16]) return;
    for (int i = 0; i < 512; i++) low_pt[i] = i < 16 ? 0 : (u64)i * 4096 | 3;
    pd[0] = (u64)(uptr)low_pt | 3;
    u64 cr3;
    __asm__ volatile("mov %%cr3, %0\n mov %0, %%cr3" : "=r"(cr3) :: "memory");
}
/* The PEB (process block) that programs and their C library read directly:
 * the .exe's address, an empty module list, the process parameters (command
 * line, image name) and the Windows version. Layout: PEB at 0, loader data at
 * 0x400, parameters at 0x600, the image name's text at 0xA00. */
static void us_set(u8 *us, const u16 *text, u32 chars) {
    *(u16 *)us = (u16)(chars * 2);
    *(u16 *)(us + 2) = (u16)(chars * 2 + 2);
    *(u64 *)(us + 8) = (u64)text;
}
static void peb_fill(w32proc_t *p) {
    u8 *peb = p->peb, *ldr = peb + 0x400, *pp = peb + 0x600;
    u16 *img = (u16 *)(peb + 0xA00);
    *(u64 *)(peb + 0x10) = (u64)p->image;
    *(u64 *)(peb + 0x18) = (u64)ldr;
    *(u64 *)(peb + 0x20) = (u64)pp;
    *(u64 *)(peb + 0x30) = 1;                 /* GetProcessHeap's handle */
    *(u32 *)(peb + 0xB8) = 1;                 /* processors */
    *(u32 *)(peb + 0x118) = 10;               /* Windows 10.0.19045, like GetVersionEx */
    *(u32 *)(peb + 0x11C) = 0;
    *(u16 *)(peb + 0x120) = 19045;
    *(u32 *)(peb + 0x124) = 2;                /* VER_PLATFORM_WIN32_NT */
    *(u32 *)ldr = 0x58;                       /* PEB_LDR_DATA: three empty module lists */
    ldr[4] = 1;
    for (int i = 0; i < 3; i++) { u64 *l = (u64 *)(ldr + 0x10 + 16 * i); l[0] = l[1] = (u64)l; }
    *(u32 *)pp = *(u32 *)(pp + 4) = 0x400;    /* RTL_USER_PROCESS_PARAMETERS */
    *(u32 *)(pp + 8) = 1;                     /* normalized */
    char path[200];
    ksnprintf(path, sizeof path, "C:\\%s%s%s", p->exedir, p->exedir[0] ? "\\" : "", p->name);
    u32 n = 0;
    for (; path[n] && n < 199; n++) img[n] = path[n] == '/' ? '\\' : (u8)path[n];
    us_set(pp + 0x60, img, n);
    u32 cl = 0;
    while (cl < 255 && p->wcmdline[cl]) cl++;
    us_set(pp + 0x70, p->wcmdline, cl);
}
int pe_run_args(const u8 *data, u32 size, const char *name, const icon_t *icon, const char *args) {
    null_guard();
    w32proc_t *p = kzalloc(sizeof *p);
    if (!p) return -1;
    strncpy0(p->name, name, sizeof p->name);
    strncpy0(p->cwd, pe_cwd_root ? "" : pe_dir, sizeof p->cwd);
    strncpy0(p->exedir, pe_dir, sizeof p->exedir);
    pe_dir[0] = 0;
    pe_cwd_root = 0;
    /* ScummVM picks OpenGL by default, which Chamows lacks: ask for its software
     * renderer. With no sound card its music never ends and games wait for it
     * forever, so music is switched off too. */
    char extra[256];
    if (!stricmp_(name, "SCUMMVM.EXE")) {
        ksnprintf(extra, sizeof extra, "%s%s%s", c_strstr(args, "--gfx-mode") ? "" : "--gfx-mode=surfacesdl ",
                  c_strstr(args, "--music-driver") ? "" : "--music-driver=null ", args);
        int n = (int)strlen(extra);
        while (n && extra[n - 1] == ' ') extra[--n] = 0;
        args = extra;
    }
    ksnprintf(p->cmdline, sizeof p->cmdline, *args ? "%s %s" : "%s", name, args);
    for (int i = 0; i < 255 && p->cmdline[i]; i++) p->wcmdline[i] = (u8)p->cmdline[i];
    /* argv: split on spaces (no quotes yet), in a copy */
    char *buf = kzalloc(256);
    strncpy0(buf, p->cmdline, 256);
    for (char *q = buf; *q && p->argc < 16;) {
        while (*q == ' ') *q++ = 0;
        if (!*q) break;
        p->argv[p->argc++] = q;
        while (*q && *q != ' ') q++;
    }
    argc_var = p->argc;
    argv_var = p->argv;
    p->icon = icon;
    w32proc_t *prev = cur;
    cur = p;
    const char *err = pe_load(p, data, size);
    if (!err || strcmp(err, "consola") == 0) { const icon_t *own = p->image ? exe_icon(p->image) : NULL; if (own) p->icon = own; } /* the .exe's own icon */
    cur = prev;
    int console = err && strcmp(err, "consola") == 0;
    if (err && !console) {
        klog("exe: no se pudo abrir %s: %s\n", name, err);
        notify(icon, "No se pudo abrir el programa", err);
        mods_free(p);
        kfree(p);
        return -1;
    }
    p->peb = kzalloc(0x1000);
    if (!p->peb) { mods_free(p); kfree(p); return -1; }
    peb_fill(p);
    if (!thread_new(p, fiber_main, 0, 0, 1024 * 1024)) { mods_free(p); kfree(p->peb); kfree(p); return -1; }
    if (console) { cur = p; con_open(p); cur = prev; }
    p->next = procs;
    procs = p;
    return 0;
}

static void proc_free(w32proc_t *p) {
    w32proc_t *prev = cur;
    cur = p;
    wsock_free(p);
    for (w32win_t *w = p->wins; w; w = w->next)
        if (w->cw) { cw_window *cw = w->cw; w->cw = NULL; w->destroyed = 1; win_close(cw); }
    if (box.p == p) box.p = NULL;
    while (p->blocks) p_free(p->blocks + 1);
    cur = prev;
    for (w32thread_t *t = p->threads, *n; t; t = n) {
        n = t->next;
        for (int i = 0; i < 16; i++) kfree(t->tlsb[i]);
        kfree(t->stack); kfree(t->teb); kfree(t);
    }
    p->threads = NULL;
    mods_free(p);
    kfree(p->peb);
    p->raw = p->peb = p->image = NULL;
    p->wins = NULL;
    p->cleaned = 1;
    for (wfile_t *f = wf_open_list; f; f = f->next) wf_flush(f); /* what the program wrote reaches the disk, as on Windows */
    klog("exe: %s terminó (código %d)\n", p->name, p->exit_code);
}

static int ready(w32proc_t *p, w32thread_t *t) {
    if (t->suspended) return 0;
    switch (t->wait) {
    case W_MSG: {
        if (t->quit || has_sent(p, t->id) || queued(p, t->id, NULL, 0)) return 1;
        for (w32win_t *w = p->wins; w; w = w->next) if (w->dirty && !w->destroyed && w->cw && win_tid(p, w) == t->id) return 1;
        return due_timer(p, t->id, NULL, 0);
    }
    case W_SLEEP: return (i32)(ticks_ms - t->wake_at) >= 0;
    case W_BOX: return !p->box_open;
    }
    return 1;
}

/* ---------- preemption ----------
 * Every timer tick (10 ms) the CPU checks whether a Windows thread has used
 * up its turn. If it is running the program's own code (not Chamows' code,
 * which is never interrupted halfway), it is paused right there and the
 * desktop and the other threads run. Its registers stay on its own stack. */
extern char _load_start[], _load_end[];
__attribute__((target("general-regs-only"))) static void preempt_tick(regs_t *r) {
    if (!cur_th || !cur || cur->done) return;
    u64 ip = REG_IP(r);
    if (ip >= (u64)(uptr)_load_start && ip < (u64)(uptr)_load_end) return; /* inside Chamows */
    if ((i32)(ticks_ms - slice_end) < 0) return;
    u8 area[512 + 16];
    u8 *fx = (u8 *)(((uptr)area + 15) & ~(uptr)15);
    __asm__ volatile("fxsave64 (%0)" ::"r"(fx) : "memory"); /* SSE and x87 registers */
    cur_th->preempts++;
    yield(W_RUN);
    __asm__ volatile("fxrstor64 (%0)" ::"r"(fx) : "memory");
}

/* Called by the desktop loop: timers, then each program that can run. */
void win32_poll(void) {
    tick_hook = preempt_tick;
    fault_hook = cpu_fault;
    for (w32proc_t **pp = &procs; *pp;) {
        w32proc_t *p = *pp;
        if (p->killed && !p->done) p->done = 1;
        if (p->done) {
            if (!p->cleaned) {
                proc_free(p);
                if (p->con) {
                    w32proc_t *prev = cur; cur = p;
                    { const char *m = "\n[El programa terminó. Cierra esta ventana cuando quieras.]"; con_write(m, (int)strlen(m)); }
                    cur = prev;
                }
            }
            con_draw(p);
            if (!p->con) { *pp = p->next; kfree(p); continue; }
            pp = &p->next;
            continue;
        }
        wsock_poll(p);
        for (w32thread_t *t = p->threads; t && !p->done; t = t->next) {
            if (t->done || !ready(p, t)) continue;
            cur = p;
            cur_th = t;
            set_gs((u64)t->teb);
            last_error = t->lasterr;
            errno_ = t->errno_v;
            slice_end = ticks_ms + SLICE_MS;
            ctx_switch(&kernel_rsp, t->rsp);
            __asm__ volatile("sti"); /* back from a timer interrupt, interrupts are still off */
            t->lasterr = last_error;
            t->errno_v = errno_;
            cur = NULL;
            cur_th = NULL;
            if (t->done && t != p->threads && t->stack) { /* a finished thread gives back its stack (programs like Telegram start hundreds) */
                for (int i = 0; i < 16; i++) { kfree(t->tlsb[i]); t->tlsb[i] = NULL; }
                kfree(t->stack); kfree(t->teb);
                t->stack = t->teb = NULL;
            }
        }
        flush_windows(p);
        con_draw(p);
        pp = &p->next;
    }
}

#else /* 32-bit build: Windows programs need the 64-bit Chamows */
char pe_dir[160];
int pe_cwd_root; /* started from the terminal: the current folder is the terminal's (the root) */
int pe_run_args(const u8 *data, u32 size, const char *name, const icon_t *icon, const char *args) { return pe_run(data, size, name, icon); }
int pe_run(const u8 *data, u32 size, const char *name, const icon_t *icon) {
    notify(icon, "No se pudo abrir el programa", "Los .exe solo funcionan en Chamows de 64 bits.");
    return -1;
}
void win32_poll(void) {}
#endif
