/* Serpiente: classic snake game. */
#include "chamows.h"

#define GW 24
#define GH 18
#define CELL 22
#define TOPBAR 40
#define W (GW * CELL)
#define H (GH * CELL + TOPBAR)

static const cw_api *api;

typedef struct {
    short x[GW * GH], y[GW * GH];
    int len, dir, next_dir; /* 0 right 1 down 2 left 3 up */
    int ax, ay;
    int score, best, over, paused, started;
} snake_t;

static void place_apple(snake_t *s) {
    for (;;) {
        int x = (int)(api->random() % GW), y = (int)(api->random() % GH), ok = 1;
        for (int i = 0; i < s->len; i++) if (s->x[i] == x && s->y[i] == y) ok = 0;
        if (ok) { s->ax = x; s->ay = y; return; }
    }
}

static void reset(snake_t *s) {
    s->len = 4;
    for (int i = 0; i < s->len; i++) { s->x[i] = (short)(8 - i); s->y[i] = GH / 2; }
    s->dir = s->next_dir = 0;
    s->score = 0; s->over = 0; s->paused = 0; s->started = 0;
    place_apple(s);
}

static void draw(cw_window *w) {
    snake_t *s = api->user(w);
    api->rect(w, 0, 0, W, TOPBAR, 0xFF2E7D32);
    char b[48];
    api->format(b, sizeof b, "Puntos: %d", s->score);
    api->text(w, 12, 11, b, 0xFFFFFFFF, CW_FONT_BOLD);
    api->format(b, sizeof b, "R\xc3\xa9" "cord: %d", s->best);
    api->text(w, W - 12 - api->text_width(b, CW_FONT_BOLD), 11, b, 0xFFFFFFFF, CW_FONT_BOLD);
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++)
            api->rect(w, x * CELL, TOPBAR + y * CELL, CELL, CELL, (x + y) & 1 ? 0xFFA2D149 : 0xFFAAD751);
    api->circle(w, s->ax * CELL + CELL / 2, TOPBAR + s->ay * CELL + CELL / 2, CELL / 2 - 2, 0xFFE7471D);
    api->rect(w, s->ax * CELL + CELL / 2, TOPBAR + s->ay * CELL + 2, 2, 5, 0xFF5D4037);
    for (int i = s->len - 1; i >= 0; i--) {
        unsigned c = i == 0 ? 0xFF3F6FE0 : 0xFF4E7CF6;
        api->round_rect(w, s->x[i] * CELL + 1, TOPBAR + s->y[i] * CELL + 1, CELL - 2, CELL - 2, 6, c);
    }
    /* eyes */
    int hx = s->x[0] * CELL + CELL / 2, hy = TOPBAR + s->y[0] * CELL + CELL / 2;
    int ex = s->dir == 0 ? 4 : s->dir == 2 ? -4 : 0, ey = s->dir == 1 ? 4 : s->dir == 3 ? -4 : 0;
    api->circle(w, hx + ex + (ey ? 4 : 0), hy + ey + (ex ? 4 : 0), 3, 0xFFFFFFFF);
    api->circle(w, hx + ex - (ey ? 4 : 0), hy + ey - (ex ? 4 : 0), 3, 0xFFFFFFFF);
    const char *msg = s->over ? "Fin de la partida. Pulsa Intro para jugar otra vez"
                    : !s->started ? "Pulsa una flecha para empezar"
                    : s->paused ? "Pausa. Pulsa P para seguir" : 0;
    if (msg) {
        int tw = api->text_width(msg, CW_FONT_BOLD);
        api->round_rect(w, (W - tw) / 2 - 16, H / 2 - 22, tw + 32, 44, 8, 0xCC000000);
        api->text(w, (W - tw) / 2, H / 2 - 8, msg, 0xFFFFFFFF, CW_FONT_BOLD);
    }
    api->invalidate(w);
}

static void step(snake_t *s) {
    if (s->over || s->paused || !s->started) return;
    s->dir = s->next_dir;
    int nx = s->x[0] + (s->dir == 0) - (s->dir == 2);
    int ny = s->y[0] + (s->dir == 1) - (s->dir == 3);
    int grow = nx == s->ax && ny == s->ay;
    if (nx < 0 || ny < 0 || nx >= GW || ny >= GH) { s->over = 1; return; }
    for (int i = 0; i < s->len - (grow ? 0 : 1); i++)
        if (s->x[i] == nx && s->y[i] == ny) { s->over = 1; return; }
    if (grow && s->len < GW * GH) s->len++;
    for (int i = s->len - 1; i > 0; i--) { s->x[i] = s->x[i - 1]; s->y[i] = s->y[i - 1]; }
    s->x[0] = (short)nx; s->y[0] = (short)ny;
    if (grow) {
        s->score += 10;
        if (s->score > s->best) s->best = s->score;
        place_apple(s);
    }
}

static void on_event(cw_window *w, const cw_event *e) {
    snake_t *s = api->user(w);
    if (e->type == CW_TICK) { step(s); draw(w); return; }
    if (e->type != CW_KEY) return;
    int d = -1;
    switch (e->key) {
    case CW_KEY_RIGHT: case 'd': d = 0; break;
    case CW_KEY_DOWN: case 's': d = 1; break;
    case CW_KEY_LEFT: case 'a': d = 2; break;
    case CW_KEY_UP: case 'w': d = 3; break;
    case 'p': case 'P': if (s->started && !s->over) s->paused ^= 1; break;
    case CW_KEY_ENTER: case ' ': if (s->over) reset(s); break;
    }
    if (d >= 0 && !s->over) {
        if ((d + 2) % 4 != s->dir) s->next_dir = d;
        s->started = 1;
    }
    draw(w);
}

int app_main(const cw_api *a) {
    api = a;
    snake_t *s = api->alloc(sizeof *s);
    if (!s) return 1;
    reset(s);
    cw_window *w = api->window("Serpiente", W, H, on_event, s);
    if (!w) return 1;
    api->set_timer(w, 120);
    draw(w);
    return 0;
}
