/* Chamows graphics: surfaces, clipping, shapes, antialiased text. */
#ifndef CHAMOWS_GFX_H
#define CHAMOWS_GFX_H
#include "kernel.h"

typedef struct { u32 off; u8 w; u8 adv; i8 xoff; } glyph_t;
typedef struct { u8 height, ascent; const glyph_t *g; const u8 *data; } font_t;

typedef struct { int x, y, w, h; } rect_t;

typedef struct {
    u32 *px;
    int w, h, stride;
    rect_t clip;       /* drawing is limited to this rectangle */
} surface_t;

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

rect_t rect_union(rect_t a, rect_t b);
rect_t rect_intersect(rect_t a, rect_t b);
static inline int rect_empty(rect_t r) { return r.w <= 0 || r.h <= 0; }
static inline int rect_has(rect_t r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

void surf_init(surface_t *s, u32 *px, int w, int h);
void surf_reset_clip(surface_t *s);

u32 blend(u32 dst, u32 src, u32 alpha);           /* alpha 0..255 */
void fill_rect(surface_t *s, int x, int y, int w, int h, u32 argb);
void fill_round(surface_t *s, int x, int y, int w, int h, int r, u32 argb);
void stroke_round(surface_t *s, int x, int y, int w, int h, int r, u32 argb);
void fill_circle(surface_t *s, int cx, int cy, int r, u32 argb);
void draw_line(surface_t *s, int x0, int y0, int x1, int y1, int thick, u32 argb);
void draw_shadow(surface_t *s, int x, int y, int w, int h, int r);
void blit(surface_t *dst, const surface_t *src, int sx, int sy, int w, int h, int dx, int dy);
void blit_argb(surface_t *s, const u32 *px, int w, int h, int x, int y); /* with alpha */

int text_draw(surface_t *s, const font_t *f, int x, int y, const char *utf8, u32 argb);
int text_width(const font_t *f, const char *utf8);
/* Draws text cut with "…" so it fits in max_w. */
void text_fit(surface_t *s, const font_t *f, int x, int y, const char *utf8, int max_w, u32 argb);
/* Word-wraps into lines of max_w; returns the height used. */
int text_wrap(surface_t *s, const font_t *f, int x, int y, const char *utf8, int max_w, int max_lines, u32 argb);

#endif
