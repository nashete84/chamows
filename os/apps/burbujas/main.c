/* Burbujas: click a group of two or more touching bubbles of the same color to
 * pop it. Bubbles above fall down and empty columns close up. Bigger groups
 * give many more points: (n - 1)^2. */
#include "chamows.h"

#define GW 10
#define GH 9
#define CELL 40
#define TOPBAR 44
#define W (GW * CELL)
#define H (GH * CELL + TOPBAR)

static const cw_api *api;
static const unsigned colors[5] = {0, 0xFFFF5A6E, 0xFF3FA9FF, 0xFFFFC93C, 0xFF5FD38D};

typedef struct { unsigned char g[GW][GH]; int score, best, over, hover; } game_t;

static void reset(game_t *s) {
    for (int x = 0; x < GW; x++)
        for (int y = 0; y < GH; y++) s->g[x][y] = (unsigned char)(1 + api->random() % 4);
    s->score = 0;
    s->over = 0;
}

/* marks the group at (x, y) with bit 0x80 and returns its size */
static int mark(game_t *s, int x, int y, int c) {
    if (x < 0 || y < 0 || x >= GW || y >= GH || s->g[x][y] != c) return 0;
    s->g[x][y] |= 0x80;
    return 1 + mark(s, x + 1, y, c) + mark(s, x - 1, y, c) + mark(s, x, y + 1, c) + mark(s, x, y - 1, c);
}
static void unmark(game_t *s) {
    for (int x = 0; x < GW; x++)
        for (int y = 0; y < GH; y++) s->g[x][y] &= 0x7F;
}
static int has_moves(game_t *s) {
    for (int x = 0; x < GW; x++)
        for (int y = 0; y < GH; y++) {
            int c = s->g[x][y];
            if (c && ((x + 1 < GW && s->g[x + 1][y] == c) || (y + 1 < GH && s->g[x][y + 1] == c))) return 1;
        }
    return 0;
}

static void pop(game_t *s, int x, int y) {
    int c = s->g[x][y];
    if (!c) return;
    int n = mark(s, x, y, c);
    if (n < 2) { unmark(s); return; }
    s->score += (n - 1) * (n - 1);
    /* remove marked bubbles; let the rest fall (y = 0 is the top) */
    for (int cx = 0; cx < GW; cx++) {
        int to = GH - 1;
        for (int cy = GH - 1; cy >= 0; cy--)
            if (s->g[cx][cy] && !(s->g[cx][cy] & 0x80)) s->g[cx][to--] = s->g[cx][cy];
        while (to >= 0) s->g[cx][to--] = 0;
    }
    /* close empty columns */
    int to = 0;
    for (int cx = 0; cx < GW; cx++)
        if (s->g[cx][GH - 1]) { if (to != cx) for (int cy = 0; cy < GH; cy++) { s->g[to][cy] = s->g[cx][cy]; s->g[cx][cy] = 0; } to++; }
    if (!has_moves(s)) {
        int left = 0;
        for (int i = 0; i < GW; i++) for (int j = 0; j < GH; j++) left += s->g[i][j] != 0;
        if (!left) s->score += 500;
        s->over = 1;
        if (s->score > s->best) s->best = s->score;
    }
}

static void draw(cw_window *w, game_t *s) {
    char buf[64];
    api->clear(w, 0xFF161A2E);
    api->rect(w, 0, 0, W, TOPBAR, 0xFF20253D);
    api->format(buf, sizeof buf, "Puntos: %d", s->score);
    api->text(w, 14, 14, buf, 0xFFFFFFFF, CW_FONT_BOLD);
    api->format(buf, sizeof buf, "Récord: %d", s->best);
    api->text(w, W - 14 - api->text_width(buf, CW_FONT_UI), 14, buf, 0xFFB8C0DA, CW_FONT_UI);
    for (int x = 0; x < GW; x++)
        for (int y = 0; y < GH; y++) {
            int c = s->g[x][y] & 0x7F;
            if (!c) continue;
            int cx = x * CELL + CELL / 2, cy = TOPBAR + y * CELL + CELL / 2;
            int lit = s->hover >= 0 && (s->g[x][y] & 0x80);
            api->circle(w, cx, cy, CELL / 2 - (lit ? 1 : 3), colors[c]);
            api->circle(w, cx - 6, cy - 6, 5, 0x66FFFFFF);
        }
    if (s->over) {
        api->round_rect(w, 50, TOPBAR + 120, W - 100, 110, 14, 0xEE2B3152);
        api->text(w, 80, TOPBAR + 140, "Ya no quedan grupos", 0xFFFFFFFF, CW_FONT_TITLE);
        api->text(w, 80, TOPBAR + 185, "Haz clic para jugar otra vez", 0xFFB8C0DA, CW_FONT_UI);
    }
    api->invalidate(w);
}

static void on_event(cw_window *w, const cw_event *ev) {
    game_t *s = api->user(w);
    int x = ev->x / CELL, y = (ev->y - TOPBAR) / CELL;
    int inside = ev->y >= TOPBAR && x >= 0 && x < GW && y >= 0 && y < GH;
    switch (ev->type) {
    case CW_MOUSE_DOWN:
        unmark(s);
        if (s->over) reset(s);
        else if (inside) pop(s, x, y);
        s->hover = -1;
        draw(w, s);
        break;
    case CW_MOUSE_MOVE:
        unmark(s);
        s->hover = -1;
        if (inside && !s->over && s->g[x][y]) {
            if (mark(s, x, y, s->g[x][y]) >= 2) s->hover = 1; else unmark(s);
        }
        draw(w, s);
        break;
    case CW_CLOSE:
        api->free(s);
        break;
    }
}

int app_main(const cw_api *a) {
    api = a;
    game_t *s = api->alloc(sizeof *s);
    s->hover = -1;
    reset(s);
    cw_window *w = api->window("Burbujas", W, H, on_event, s);
    draw(w, s);
    return 0;
}
