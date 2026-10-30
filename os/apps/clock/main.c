/* Reloj: analog + digital clock and a stopwatch. */
#include "chamows.h"

#define W 380
#define H 470

static const cw_api *api;
/* sin(i * 6 degrees) * 1000 */
static const short SIN[60] = {0,105,208,309,407,500,588,669,743,809,866,914,951,978,995,1000,995,978,951,914,866,809,743,669,588,500,407,309,208,105,0,-105,-208,-309,-407,-500,-588,-669,-743,-809,-866,-914,-951,-978,-995,-1000,-995,-978,-951,-914,-866,-809,-743,-669,-588,-500,-407,-309,-208,-105};
static int sn(int i) { return SIN[((i % 60) + 60) % 60]; }
static int cs(int i) { return sn(i + 15); }

typedef struct { int running; unsigned start, acc; } clk_t;

static const char *days[7] = {"domingo", "lunes", "martes", "mi\xc3\xa9rcoles", "jueves", "viernes", "s\xc3\xa1" "bado"};
static const char *months[12] = {"enero", "febrero", "marzo", "abril", "mayo", "junio", "julio",
                                 "agosto", "septiembre", "octubre", "noviembre", "diciembre"};

static int weekday(int d, int m, int y) { /* Sakamoto */
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static void hand(cw_window *w, int cx, int cy, int pos60, int len, int th, unsigned c) {
    api->line(w, cx, cy, cx + sn(pos60) * len / 1000, cy - cs(pos60) * len / 1000, th, c);
}

static void draw(cw_window *w) {
    clk_t *k = api->user(w);
    int h, m, s, d, mo, y;
    api->clock(&h, &m, &s);
    api->date(&d, &mo, &y);
    api->clear(w, 0xFFF3F3F3);
    int cx = W / 2, cy = 150, r = 120;
    api->circle(w, cx, cy + 2, r + 2, 0x22000000);
    api->circle(w, cx, cy, r, 0xFFFFFFFF);
    for (int i = 0; i < 60; i++) {
        int big = i % 5 == 0, in = big ? r - 16 : r - 9;
        api->line(w, cx + sn(i) * in / 1000, cy - cs(i) * in / 1000, cx + sn(i) * (r - 5) / 1000, cy - cs(i) * (r - 5) / 1000,
                  big ? 3 : 1, big ? 0xFF303030 : 0xFFA0A0A0);
    }
    hand(w, cx, cy, (h % 12) * 5 + m / 12, 62, 6, 0xFF1A1A1A);
    hand(w, cx, cy, m, 92, 4, 0xFF1A1A1A);
    hand(w, cx, cy, s, 102, 2, 0xFF0067C0);
    api->circle(w, cx, cy, 6, 0xFF0067C0);

    char buf[64];
    api->format(buf, sizeof buf, "%02d:%02d:%02d", h, m, s);
    api->text(w, (W - api->text_width(buf, CW_FONT_TITLE)) / 2, 290, buf, 0xFF1A1A1A, CW_FONT_TITLE);
    const char *dn = days[weekday(d, mo, y)];
    char date[64];
    api->format(date, sizeof date, "%s, %d de ", dn, d);
    unsigned l = strlen(date);
    api->format(date + l, sizeof date - (int)l, "%s de %d", months[mo - 1], y);
    api->text(w, (W - api->text_width(date, CW_FONT_UI)) / 2, 324, date, 0xFF505050, CW_FONT_UI);

    /* stopwatch */
    api->rect(w, 20, 356, W - 40, 1, 0xFFDDDDDD);
    unsigned t = k->acc + (k->running ? api->ticks_ms() - k->start : 0);
    api->format(buf, sizeof buf, "Cron\xc3\xb3metro  %02d:%02d,%d", (int)(t / 60000), (int)(t / 1000 % 60), (int)(t / 100 % 10));
    api->text(w, 24, 376, buf, 0xFF1A1A1A, CW_FONT_BOLD);
    const char *b1 = k->running ? "Pausa" : "Iniciar";
    api->round_rect(w, 24, 408, 150, 36, 5, 0xFF005FB8);
    api->text(w, 24 + (150 - api->text_width(b1, CW_FONT_BOLD)) / 2, 418, b1, 0xFFFFFFFF, CW_FONT_BOLD);
    api->round_rect(w, W - 174, 408, 150, 36, 5, 0xFFD0D0D0);
    api->round_rect(w, W - 173, 409, 148, 34, 5, 0xFFFFFFFF);
    api->text(w, W - 174 + (150 - api->text_width("Reiniciar", CW_FONT_UI)) / 2, 418, "Reiniciar", 0xFF1A1A1A, CW_FONT_UI);
    api->invalidate(w);
}

static void on_event(cw_window *w, const cw_event *e) {
    clk_t *k = api->user(w);
    if (e->type == CW_MOUSE_DOWN && e->y >= 408 && e->y < 444) {
        if (e->x >= 24 && e->x < 174) {
            if (k->running) { k->acc += api->ticks_ms() - k->start; k->running = 0; }
            else { k->start = api->ticks_ms(); k->running = 1; }
            api->set_timer(w, k->running ? 100 : 1000);
        } else if (e->x >= W - 174 && e->x < W - 24) {
            k->acc = 0; k->running = 0;
            api->set_timer(w, 1000);
        }
        draw(w);
    } else if (e->type == CW_TICK) draw(w);
}

int app_main(const cw_api *a) {
    api = a;
    clk_t *k = api->alloc(sizeof *k);
    if (!k) return 1;
    cw_window *w = api->window("Reloj", W, H, on_event, k);
    if (!w) return 1;
    api->set_timer(w, 1000);
    draw(w);
    return 0;
}
