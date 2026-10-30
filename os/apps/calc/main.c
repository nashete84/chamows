/* Calculadora: a basic Windows-style calculator.
 * Numbers are fixed point with 6 decimals (no floating point in Chamows yet). */
#include "chamows.h"

#define SCALE 1000000LL
#define W 320
#define H 452

static const cw_api *api;

typedef struct {
    long long acc, cur;   /* accumulator and number being typed (scaled) */
    int op;               /* pending operator: 0 + - * / */
    int typing;           /* cur is being typed */
    int decimals;         /* digits typed after the comma, -1 = no comma */
    int error;
    char expr[48];
    int hover, pressed;
} calc_t;

static const char *labels[24] = {
    "%", "CE", "C", "Borrar",
    "1/x", "x\xc2\xb2", "Ra\xc3\xadz", "\xc3\xb7",
    "7", "8", "9", "\xc3\x97",
    "4", "5", "6", "-",
    "1", "2", "3", "+",
    "\xc2\xb1", "0", ",", "=",
};

#define BX 6
#define BY 128
#define BW 75
#define BH 50
#define GAP 2

static int btn_at(int x, int y) {
    if (x < BX || y < BY) return -1;
    int c = (x - BX) / (BW + GAP), r = (y - BY) / (BH + GAP);
    if (c > 3 || r > 5) return -1;
    return r * 4 + c;
}

static void fmt_num(long long v, char *out) {
    char tmp[40];
    int n = 0, neg = v < 0;
    if (neg) v = -v;
    v = (v + 5) / 10 * 10; /* round the last digit */
    long long ip = v / SCALE, fp = v % SCALE;
    if (ip > 999999999999LL) { out[0] = 0; return; }
    /* integer part with thousands separators (Spanish: dots) */
    char digits[24];
    int nd = 0;
    do { digits[nd++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
    if (neg) tmp[n++] = '-';
    for (int i = nd - 1; i >= 0; i--) {
        tmp[n++] = digits[i];
        if (i && i % 3 == 0) tmp[n++] = '.';
    }
    if (fp) {
        tmp[n++] = ',';
        char fd[6];
        for (int i = 5; i >= 0; i--) { fd[i] = (char)('0' + fp % 10); fp /= 10; }
        int last = 5;
        while (last > 0 && fd[last] == '0') last--;
        for (int i = 0; i <= last; i++) tmp[n++] = fd[i];
    }
    tmp[n] = 0;
    memcpy(out, tmp, (unsigned)n + 1);
}

static long long apply(long long a, long long b, int op, int *err) {
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a / 1000 * (b / 1000);
    case '/':
        if (b == 0) { *err = 1; return 0; }
        return a * 1000 / b * 1000;
    }
    return b;
}

static void draw(cw_window *w) {
    calc_t *c = api->user(w);
    api->clear(w, 0xFFF3F3F3);
    api->text(w, W - 14 - api->text_width(c->expr, CW_FONT_UI), 22, c->expr, 0xFF606060, CW_FONT_UI);
    char buf[48];
    if (c->error) memcpy(buf, "No se puede dividir entre cero", 31);
    else {
        fmt_num(c->cur, buf);
        if (!buf[0]) memcpy(buf, "Desbordamiento", 15);
        else if (c->typing && c->decimals == 0) { unsigned n = strlen(buf); buf[n] = ','; buf[n + 1] = 0; }
    }
    int f = strlen(buf) > 16 ? CW_FONT_BOLD : CW_FONT_TITLE;
    api->text(w, W - 14 - api->text_width(buf, f), 64, buf, 0xFF1A1A1A, f);
    for (int i = 0; i < 24; i++) {
        int r = i / 4, col = i % 4;
        int x = BX + col * (BW + GAP), y = BY + r * (BH + GAP);
        int digit = (r >= 2 && col < 3);
        unsigned bg = i == 23 ? 0xFF005FB8 : digit ? 0xFFFFFFFF : 0xFFF9F9F9;
        if (i == c->hover) bg = i == 23 ? 0xFF1975C5 : 0xFFF0F0F0;
        if (i == c->pressed) bg = i == 23 ? 0xFF3183CA : 0xFFE5E5E5;
        api->round_rect(w, x, y, BW, BH, 5, 0xFFE2E2E2);
        api->round_rect(w, x + 1, y + 1, BW - 2, BH - 2, 4, bg);
        int font = digit ? CW_FONT_BOLD : CW_FONT_UI;
        int tw = api->text_width(labels[i], font);
        api->text(w, x + (BW - tw) / 2, y + (BH - api->font_height(font)) / 2, labels[i],
                  i == 23 ? 0xFFFFFFFF : 0xFF1A1A1A, font);
    }
    api->invalidate(w);
}

static void set_expr(calc_t *c, long long v, int op) {
    char n[40];
    fmt_num(v, n);
    unsigned l = strlen(n);
    memcpy(c->expr, n, l);
    if (op) {
        const char *s = op == '+' ? " +" : op == '-' ? " -" : op == '*' ? " \xc3\x97" : op == '/' ? " \xc3\xb7" : " =";
        memcpy(c->expr + l, s, strlen(s) + 1);
    } else c->expr[l] = 0;
}

static void press(calc_t *c, int i) {
    if (c->error && i != 1 && i != 2) return;
    if (i == 2 || (c->error)) { /* C */
        memset(c, 0, sizeof *c - 2 * sizeof(int));
        c->decimals = -1;
        return;
    }
    const char *L = labels[i];
    if (L[0] >= '0' && L[0] <= '9' && L[1] == 0) {
        int d = L[0] - '0';
        if (!c->typing) { c->cur = 0; c->typing = 1; c->decimals = -1; if (c->op == '=') { c->op = 0; c->expr[0] = 0; } }
        long long a = c->cur < 0 ? -c->cur : c->cur;
        if (c->decimals < 0) {
            if (a / SCALE > 99999999999LL) return;
            a = a * 10 + (long long)d * SCALE;
        } else {
            if (c->decimals >= 6) return;
            long long p = SCALE;
            for (int k = 0; k <= c->decimals; k++) p /= 10;
            a += d * p;
            c->decimals++;
        }
        c->cur = c->cur < 0 ? -a : a;
        return;
    }
    switch (i) {
    case 22: /* , */
        if (!c->typing) { c->cur = 0; c->typing = 1; }
        if (c->decimals < 0) c->decimals = 0;
        return;
    case 1: c->cur = 0; c->typing = 1; c->decimals = -1; return; /* CE */
    case 3: /* backspace */
        if (!c->typing) return;
        if (c->decimals > 0) {
            long long p = SCALE;
            for (int k = 0; k < c->decimals; k++) p /= 10;
            c->cur = c->cur / (p * 10) * (p * 10);
            c->decimals--;
        } else if (c->decimals == 0) c->decimals = -1;
        else c->cur = c->cur / SCALE / 10 * SCALE;
        return;
    case 20: c->cur = -c->cur; return;
    case 0: c->cur = c->acc / 1000 * (c->cur / 1000) / 100; c->typing = 0; return; /* % of acc */
    case 4: c->cur = apply(SCALE, c->cur, '/', &c->error); c->typing = 0; return;
    case 5: c->cur = apply(c->cur, c->cur, '*', &c->error); c->typing = 0; return;
    case 6: { /* square root by bisection */
        if (c->cur < 0) { c->error = 1; return; }
        long long lo = 0, hi = c->cur > SCALE ? c->cur : SCALE;
        for (int k = 0; k < 80; k++) {
            long long mid = (lo + hi) / 2;
            if (mid / 1000 * (mid / 1000) <= c->cur) lo = mid; else hi = mid;
        }
        c->cur = lo; c->typing = 0; return;
    }
    }
    int op = i == 7 ? '/' : i == 11 ? '*' : i == 15 ? '-' : i == 19 ? '+' : '=';
    if (op == '=' && c->op && c->op != '=') {
        /* show "a op b =" like Windows */
        char nb[40];
        set_expr(c, c->acc, c->op);
        fmt_num(c->cur, nb);
        unsigned l = strlen(c->expr), k = strlen(nb);
        if (l + k + 4 < sizeof c->expr) {
            c->expr[l] = ' ';
            memcpy(c->expr + l + 1, nb, k);
            memcpy(c->expr + l + 1 + k, " =", 3);
        }
        c->cur = apply(c->acc, c->cur, c->op, &c->error);
    } else {
        if (c->op && c->op != '=' && c->typing) c->cur = apply(c->acc, c->cur, c->op, &c->error);
        set_expr(c, c->cur, op);
    }
    c->acc = c->cur;
    c->op = op;
    c->typing = 0;
}

static int key_button(int k) {
    if (k >= '0' && k <= '9') { static const int map[10] = {21, 16, 17, 18, 12, 13, 14, 8, 9, 10}; return map[k - '0']; }
    switch (k) {
    case '+': return 19; case '-': return 15; case '*': return 11; case '/': return 7;
    case ',': case '.': return 22; case CW_KEY_ENTER: case '=': return 23;
    case CW_KEY_BACKSPACE: return 3; case CW_KEY_ESC: return 2; case CW_KEY_DELETE: return 1;
    case '%': return 0;
    }
    return -1;
}

static void on_event(cw_window *w, const cw_event *e) {
    calc_t *c = api->user(w);
    switch (e->type) {
    case CW_MOUSE_DOWN: c->pressed = btn_at(e->x, e->y); break;
    case CW_MOUSE_MOVE: c->hover = btn_at(e->x, e->y); break;
    case CW_MOUSE_UP: {
        int b = btn_at(e->x, e->y);
        if (b >= 0 && b == c->pressed) press(c, b);
        c->pressed = -1;
        break;
    }
    case CW_KEY: { int b = key_button(e->key); if (b >= 0) press(c, b); break; }
    case CW_CLOSE: return;
    default: return;
    }
    draw(w);
}

int app_main(const cw_api *a) {
    api = a;
    calc_t *c = api->alloc(sizeof *c);
    c->hover = c->pressed = -1;
    c->decimals = -1;
    cw_window *w = api->window("Calculadora", W, H, on_event, c);
    if (!w) return 1;
    draw(w);
    return 0;
}
