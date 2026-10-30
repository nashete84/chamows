/* Paint: freehand drawing on a canvas kept in app memory. */
#include "chamows.h"

#define W 720
#define H 500
#define BAR 56
#define CW (W)
#define CH (H - BAR)

static const cw_api *api;

static const unsigned colors[12] = {
    0xFF000000, 0xFF7F7F7F, 0xFF880015, 0xFFED1C24, 0xFFFF7F27, 0xFFFFF200,
    0xFF22B14C, 0xFF00A2E8, 0xFF3F48CC, 0xFFA349A4, 0xFFFFFFFF, 0xFFB97A57,
};
static const int sizes[3] = {2, 5, 10};

typedef struct {
    unsigned *px;      /* canvas pixels */
    int color, size, eraser;
    int drawing, lx, ly;
} paint_t;

static void dot(paint_t *p, int cx, int cy, int r, unsigned c) {
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= CH) continue;
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || x >= CW) continue;
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) p->px[y * CW + x] = c;
        }
    }
}

static void stroke(paint_t *p, int x0, int y0, int x1, int y1) {
    unsigned c = p->eraser ? 0xFFFFFFFF : colors[p->color];
    int r = p->eraser ? 12 : sizes[p->size];
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (steps == 0) steps = 1;
    for (int i = 0; i <= steps; i++) dot(p, x0 + dx * i / steps, y0 + dy * i / steps, r / 2 ? r / 2 : 1, c);
}

static void draw(cw_window *w) {
    paint_t *p = api->user(w);
    api->rect(w, 0, 0, W, BAR, 0xFFF5F6F7);
    api->rect(w, 0, BAR - 1, W, 1, 0xFFDADBDC);
    /* sizes */
    api->text(w, 12, 4, "Pincel", 0xFF505050, CW_FONT_UI);
    for (int i = 0; i < 3; i++) {
        int x = 12 + i * 34;
        if (!p->eraser && p->size == i) api->round_rect(w, x, 22, 30, 28, 4, 0xFFCCE4F7);
        api->circle(w, x + 15, 36, sizes[i] / 2 + 1, 0xFF303030);
    }
    /* eraser */
    api->text(w, 128, 4, "Goma", 0xFF505050, CW_FONT_UI);
    if (p->eraser) api->round_rect(w, 124, 22, 40, 28, 4, 0xFFCCE4F7);
    api->round_rect(w, 132, 28, 24, 16, 3, 0xFFF4A6C0);
    api->rect(w, 132, 38, 24, 6, 0xFF6C8EBF);
    /* palette */
    api->text(w, 190, 4, "Colores", 0xFF505050, CW_FONT_UI);
    for (int i = 0; i < 12; i++) {
        int x = 190 + i * 30;
        if (!p->eraser && p->color == i) api->round_rect(w, x - 2, 20, 28, 32, 4, 0xFF0078D4);
        api->round_rect(w, x, 22, 24, 28, 3, 0xFFB0B0B0);
        api->round_rect(w, x + 1, 23, 22, 26, 3, colors[i]);
    }
    /* clear */
    api->round_rect(w, W - 96, 16, 84, 30, 4, 0xFFD0D0D0);
    api->round_rect(w, W - 95, 17, 82, 28, 4, 0xFFFFFFFF);
    api->text(w, W - 96 + (84 - api->text_width("Nuevo", CW_FONT_UI)) / 2, 23, "Nuevo", 0xFF1A1A1A, CW_FONT_UI);
    api->pixels(w, 0, BAR, CW, CH, p->px);
    api->invalidate(w);
}

static void clear(paint_t *p) { for (int i = 0; i < CW * CH; i++) p->px[i] = 0xFFFFFFFF; }

static void on_event(cw_window *w, const cw_event *e) {
    paint_t *p = api->user(w);
    if (e->type == CW_MOUSE_DOWN) {
        if (e->y < BAR) {
            if (e->y >= 20 && e->x >= 12 && e->x < 114) { p->size = (e->x - 12) / 34; p->eraser = 0; }
            else if (e->y >= 20 && e->x >= 124 && e->x < 164) p->eraser = 1;
            else if (e->y >= 20 && e->x >= 188 && e->x < 190 + 12 * 30) { p->color = (e->x - 188) / 30; p->eraser = 0; }
            else if (e->x >= W - 96 && e->x < W - 12) clear(p);
            draw(w);
            return;
        }
        p->drawing = 1;
        p->lx = e->x; p->ly = e->y - BAR;
        stroke(p, p->lx, p->ly, p->lx, p->ly);
        draw(w);
    } else if (e->type == CW_MOUSE_MOVE && p->drawing) {
        stroke(p, p->lx, p->ly, e->x, e->y - BAR);
        p->lx = e->x; p->ly = e->y - BAR;
        draw(w);
    } else if (e->type == CW_MOUSE_UP) {
        p->drawing = 0;
    } else if (e->type == CW_CLOSE) {
        api->free(p->px);
    }
}

int app_main(const cw_api *a) {
    api = a;
    paint_t *p = api->alloc(sizeof *p);
    if (!p) return 1;
    p->px = api->alloc(CW * CH * 4);
    if (!p->px) return 1;
    p->size = 1;
    clear(p);
    cw_window *w = api->window("Sin t\xc3\xadtulo - Paint", W, H, on_event, p);
    if (!w) return 1;
    draw(w);
    return 0;
}
