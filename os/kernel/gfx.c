/* Chamows graphics primitives. Every function respects the surface clip. */
#include "gfx.h"

rect_t rect_union(rect_t a, rect_t b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = imin(a.x, b.x), y0 = imin(a.y, b.y);
    int x1 = imax(a.x + a.w, b.x + b.w), y1 = imax(a.y + a.h, b.y + b.h);
    return (rect_t){x0, y0, x1 - x0, y1 - y0};
}
rect_t rect_intersect(rect_t a, rect_t b) {
    int x0 = imax(a.x, b.x), y0 = imax(a.y, b.y);
    int x1 = imin(a.x + a.w, b.x + b.w), y1 = imin(a.y + a.h, b.y + b.h);
    return (rect_t){x0, y0, x1 - x0, y1 - y0};
}

void surf_init(surface_t *s, u32 *px, int w, int h) {
    s->px = px; s->w = w; s->h = h; s->stride = w;
    surf_reset_clip(s);
}
void surf_reset_clip(surface_t *s) { s->clip = (rect_t){0, 0, s->w, s->h}; }

static inline u32 div255(u32 v) { return (v + 1 + (v >> 8)) >> 8; }

u32 blend(u32 d, u32 s, u32 a) {
    if (a >= 255) return s | 0xFF000000;
    if (a == 0) return d;
    u32 ia = 255 - a;
    u32 rb = div255(((s >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * ia);
    u32 g = div255(((s >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * ia);
    u32 b = div255((s & 0xFF) * a + (d & 0xFF) * ia);
    return 0xFF000000 | (rb << 16) | (g << 8) | b;
}

static inline void put(surface_t *s, int x, int y, u32 c, u32 a) {
    if (!rect_has(s->clip, x, y)) return;
    u32 *p = &s->px[y * s->stride + x];
    *p = blend(*p, c, a);
}

void fill_rect(surface_t *s, int x, int y, int w, int h, u32 c) {
    rect_t r = rect_intersect((rect_t){x, y, w, h}, s->clip);
    if (rect_empty(r)) return;
    u32 a = c >> 24;
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        u32 *p = &s->px[yy * s->stride + r.x];
        if (a == 255) {
            u32 v = c;
            int n = r.w;
            __asm__ volatile("rep stosl" : "+D"(p), "+c"(n) : "a"(v) : "memory");
        } else {
            for (int i = 0; i < r.w; i++) p[i] = blend(p[i], c, a);
        }
    }
}

/* Coverage (0..255) of pixel (px,py) by a circle of radius r centred at
 * (cx,cy), sampled 4x4 per pixel. Coordinates are scaled by 8 so sample
 * centres land on integers. */
static u32 corner_cov(int px, int py, int cx, int cy, int r) {
    int inside = 0, R = r * 8, C2 = R * R;
    for (int sy = 0; sy < 4; sy++)
        for (int sx = 0; sx < 4; sx++) {
            int X = px * 8 + sx * 2 + 1 - cx * 8, Y = py * 8 + sy * 2 + 1 - cy * 8;
            if (X * X + Y * Y <= C2) inside++;
        }
    return (u32)(inside * 255 / 16);
}

void fill_round(surface_t *s, int x, int y, int w, int h, int r, u32 c) {
    if (r <= 0) { fill_rect(s, x, y, w, h, c); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    fill_rect(s, x, y + r, w, h - 2 * r, c);
    fill_rect(s, x + r, y, w - 2 * r, r, c);
    fill_rect(s, x + r, y + h - r, w - 2 * r, r, c);
    u32 a = c >> 24;
    for (int yy = 0; yy < r; yy++)
        for (int xx = 0; xx < r; xx++) {
            u32 cov = corner_cov(xx, yy, r, r, r);
            if (!cov) continue;
            u32 aa = div255(cov * a);
            put(s, x + xx, y + yy, c, aa);
            put(s, x + w - 1 - xx, y + yy, c, aa);
            put(s, x + xx, y + h - 1 - yy, c, aa);
            put(s, x + w - 1 - xx, y + h - 1 - yy, c, aa);
        }
}

void stroke_round(surface_t *s, int x, int y, int w, int h, int r, u32 c) {
    /* 1px outline: straight edges plus corner rings (outer minus inner disc). */
    fill_rect(s, x + r, y, w - 2 * r, 1, c);
    fill_rect(s, x + r, y + h - 1, w - 2 * r, 1, c);
    fill_rect(s, x, y + r, 1, h - 2 * r, c);
    fill_rect(s, x + w - 1, y + r, 1, h - 2 * r, c);
    u32 a = c >> 24;
    for (int yy = 0; yy < r; yy++)
        for (int xx = 0; xx < r; xx++) {
            int outer = (int)corner_cov(xx, yy, r, r, r);
            int inner = r > 1 ? (int)corner_cov(xx, yy, r, r, r - 1) : 0;
            int cov = outer - inner;
            if (cov <= 0) continue;
            u32 aa = div255((u32)cov * a);
            put(s, x + xx, y + yy, c, aa);
            put(s, x + w - 1 - xx, y + yy, c, aa);
            put(s, x + xx, y + h - 1 - yy, c, aa);
            put(s, x + w - 1 - xx, y + h - 1 - yy, c, aa);
        }
}

void fill_circle(surface_t *s, int cx, int cy, int r, u32 c) {
    fill_round(s, cx - r, cy - r, 2 * r, 2 * r, r, c);
}

void draw_line(surface_t *s, int x0, int y0, int x1, int y1, int t, u32 c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = -(y1 > y0 ? y1 - y0 : y0 - y1), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, half = t / 2;
    for (;;) {
        if (t <= 1) put(s, x0, y0, c, c >> 24);
        else if (t <= 3) fill_rect(s, x0 - half, y0 - half, t, t, c);
        else fill_circle(s, x0, y0, half, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void draw_shadow(surface_t *s, int x, int y, int w, int h, int r) {
    /* Soft shadow as 1px rings around the frame (cheap: only the edges). */
    static const u8 alpha[] = {34, 24, 16, 10, 6, 3};
    (void)r;
    y += 2;
    for (int i = 0; i < 6; i++) {
        int g = i + 1;
        u32 c = (u32)alpha[i] << 24;
        fill_rect(s, x - g, y - g, w + 2 * g, 1, c);
        fill_rect(s, x - g, y + h + g - 1, w + 2 * g, 1, c);
        fill_rect(s, x - g, y - g + 1, 1, h + 2 * g - 2, c);
        fill_rect(s, x + w + g - 1, y - g + 1, 1, h + 2 * g - 2, c);
    }
}

void blit(surface_t *d, const surface_t *src, int sx, int sy, int w, int h, int dx, int dy) {
    rect_t r = rect_intersect((rect_t){dx, dy, w, h}, d->clip);
    if (rect_empty(r)) return;
    sx += r.x - dx; sy += r.y - dy;
    for (int yy = 0; yy < r.h; yy++)
        memcpy(&d->px[(r.y + yy) * d->stride + r.x], &src->px[(sy + yy) * src->stride + sx], (size_t)r.w * 4);
}

void blit_argb(surface_t *s, const u32 *px, int w, int h, int x, int y) {
    rect_t r = rect_intersect((rect_t){x, y, w, h}, s->clip);
    if (rect_empty(r)) return;
    for (int yy = r.y; yy < r.y + r.h; yy++) {
        const u32 *sp = &px[(yy - y) * w + (r.x - x)];
        u32 *dp = &s->px[yy * s->stride + r.x];
        for (int i = 0; i < r.w; i++) {
            u32 a = sp[i] >> 24;
            if (a) dp[i] = blend(dp[i], sp[i], a);
        }
    }
}

/* ---- text ---- */
static int next_char(const char **p) {
    const u8 *s = (const u8 *)*p;
    int c = *s;
    if (!c) return 0;
    if (c < 0x80) { *p += 1; return c; }
    if ((c & 0xE0) == 0xC0 && s[1]) { *p += 2; return ((c & 0x1F) << 6) | (s[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
        *p += 3;
        int cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        if (cp == 0x2026) return 0x85; /* … mapped to a private slot */
        if (cp == 0x20AC) return 0x80; /* € */
        return '?';
    }
    *p += 1;
    return '?';
}

static const glyph_t *glyph(const font_t *f, int c) {
    if (c == 0x85) c = '.';
    if (c < 32 || c > 255) c = '?';
    return &f->g[c - 32];
}

static int draw_glyph(surface_t *s, const font_t *f, int x, int y, int c, u32 color) {
    if (c == 0x85) { /* ellipsis: three dots */
        int w = 0;
        for (int i = 0; i < 3; i++) w += draw_glyph(s, f, x + w, y, '.', color) - (i < 2 ? 1 : 0);
        return w;
    }
    const glyph_t *g = glyph(f, c);
    int gx = x + g->xoff;
    rect_t r = rect_intersect((rect_t){gx, y, g->w, f->height}, s->clip);
    if (!rect_empty(r)) {
        u32 ca = color >> 24;
        for (int yy = r.y; yy < r.y + r.h; yy++) {
            const u8 *src = &f->data[g->off + (yy - y) * g->w + (r.x - gx)];
            u32 *dp = &s->px[yy * s->stride + r.x];
            for (int i = 0; i < r.w; i++)
                if (src[i]) dp[i] = blend(dp[i], color, div255(src[i] * ca));
        }
    }
    return g->adv;
}

int text_draw(surface_t *s, const font_t *f, int x, int y, const char *str, u32 color) {
    int x0 = x, c;
    while ((c = next_char(&str))) x += draw_glyph(s, f, x, y, c, color);
    return x - x0;
}

int text_width(const font_t *f, const char *str) {
    int w = 0, c;
    while ((c = next_char(&str))) w += c == 0x85 ? glyph(f, '.')->adv * 3 - 2 : glyph(f, c)->adv;
    return w;
}

void text_fit(surface_t *s, const font_t *f, int x, int y, const char *str, int max_w, u32 color) {
    if (text_width(f, str) <= max_w) { text_draw(s, f, x, y, str, color); return; }
    int ell = glyph(f, '.')->adv * 3, w = 0, c;
    const char *p = str;
    while ((c = next_char(&p))) {
        int a = glyph(f, c)->adv;
        if (w + a + ell > max_w) break;
        x += draw_glyph(s, f, x, y, c, color);
        w += a;
    }
    draw_glyph(s, f, x, y, 0x85, color);
}

int text_wrap(surface_t *s, const font_t *f, int x, int y, const char *str, int max_w, int max_lines, u32 color) {
    int line = 0;
    char buf[256];
    while (*str && line < max_lines) {
        /* take as many words as fit */
        int n = 0, last_space = -1;
        const char *p = str;
        while (*p && *p != '\n') {
            int len = 1;
            u8 b = (u8)*p;
            if (b >= 0xC0) len = b >= 0xE0 ? 3 : 2;
            if (n + len >= (int)sizeof buf - 1) break;
            memcpy(buf + n, p, (size_t)len);
            buf[n + len] = 0;
            if (text_width(f, buf) > max_w) break;
            if (*p == ' ') last_space = n;
            n += len;
            p += len;
        }
        int cut = n;
        if (*p && *p != '\n' && last_space > 0) cut = last_space;
        if (cut == 0) cut = n ? n : 1;
        buf[cut] = 0;
        if (line == max_lines - 1 && str[cut] && str[cut] != '\n') {
            text_fit(s, f, x, y + line * f->height, str, max_w, color); /* last line: ellipsis */
        } else {
            text_draw(s, f, x, y + line * f->height, buf, color);
        }
        str += cut;
        while (*str == ' ' || *str == '\n') str++;
        line++;
    }
    return line * f->height;
}
