/* TrueType text for Windows programs (the GDI font functions).
 *
 * Programs such as Telegram draw their text with GDI: they create a font
 * (CreateFontIndirectW), ask for its measurements (GetTextMetricsW,
 * GetOutlineTextMetricsW, GetFontData) and have Windows draw each letter
 * (GetGlyphOutlineW, ExtTextOutW). Here the letters come from the .ttf/.otf
 * files in WINDOWS\FONTS, or from fonts the program adds itself, and are
 * drawn by stb_truetype (public domain, by Sean Barrett). */

/* ---- stb_truetype, with the few C library pieces it needs ---- */
static double tt_floor(double x) { double i = (double)(i64)x; return i > x ? i - 1 : i; }
static double tt_ceil(double x) { double i = (double)(i64)x; return i < x ? i + 1 : i; }
static double tt_sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static double tt_fabs(double x) { return x < 0 ? -x : x; }
static double tt_none(double x, double y) { (void)x; (void)y; return 0; } /* only the SDF code, which we never call, uses pow/fmod/cos/acos */
#define STBTT_ifloor(x) ((int)tt_floor(x))
#define STBTT_iceil(x) ((int)tt_ceil(x))
#define STBTT_sqrt(x) tt_sqrt(x)
#define STBTT_pow(x, y) tt_none(x, y)
#define STBTT_fmod(x, y) tt_none(x, y)
#define STBTT_cos(x) tt_none(x, 0)
#define STBTT_acos(x) tt_none(x, 0)
#define STBTT_fabs(x) tt_fabs(x)
#define STBTT_malloc(x, u) ((void)(u), kmalloc(x))
#define STBTT_free(x, u) ((void)(u), kfree(x))
#define STBTT_assert(x) ((void)0)
#define STBTT_strlen(x) strlen(x)
#define STBTT_memcpy memcpy
#define STBTT_memset memset
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
/* int coordinates: we transform the outlines ourselves, in 1/64 pixel */
#define stbtt_vertex_type int
typedef struct { int x, y, cx, cy, cx1, cy1; unsigned char type, padding; } stbtt_vertex;
#define stbtt_vertex stbtt_vertex
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "stb_truetype.h"
#pragma GCC diagnostic pop

/* ---- one loaded font file ---- */
typedef struct {
    stbtt_fontinfo info;
    int upem, asc, desc, gap;        /* hhea: ascender, descender (negative), line gap */
    int winasc, windesc;             /* OS/2: what Windows uses for the line height */
    int avgw, maxw, xh, caph;
    int ulsize, ulpos, stsize, stpos, fstype, fsel;
    int ok;
} tface_t;
static tface_t *face_load(wfontinfo_t *f) {
    if (f->face) return ((tface_t *)f->face)->ok ? f->face : NULL;
    tface_t *t = kzalloc(sizeof *t);
    if (!t) return NULL;
    f->face = t;
    if (!f->data) {
        char path[96];
        ksnprintf(path, sizeof path, FONT_DIR "/%s", f->file);
        if (fat_read(path, &f->data, &f->size) != 0) { f->data = NULL; klog("exe: no puedo leer la fuente %s\n", path); return NULL; }
    }
    if (!stbtt_InitFont(&t->info, f->data, 0)) { klog("exe: fuente %s no válida\n", f->file); return NULL; }
    u32 l;
    const u8 *hhea = sfnt_table(f->data, f->size, "hhea", &l), *os2 = sfnt_table(f->data, f->size, "OS/2", &l);
    const u8 *head = sfnt_table(f->data, f->size, "head", NULL), *post = sfnt_table(f->data, f->size, "post", NULL);
    t->upem = head ? be16(head + 18) : 2048;
    if (!t->upem) t->upem = 2048;
    if (hhea) { t->asc = (i16)be16(hhea + 4); t->desc = (i16)be16(hhea + 6); t->gap = (i16)be16(hhea + 8); t->maxw = be16(hhea + 10); }
    else { t->asc = t->upem * 8 / 10; t->desc = -t->upem / 5; t->maxw = t->upem; }
    t->winasc = t->asc; t->windesc = -t->desc;
    t->avgw = t->upem / 2;
    t->xh = t->upem / 2; t->caph = t->upem * 7 / 10;
    t->ulsize = t->stsize = t->upem / 14;
    t->ulpos = -t->upem / 10; t->stpos = t->upem / 4;
    if (os2 && l >= 78) {
        t->avgw = (i16)be16(os2 + 2);
        t->fstype = be16(os2 + 8);
        t->stsize = (i16)be16(os2 + 26); t->stpos = (i16)be16(os2 + 28);
        t->fsel = be16(os2 + 62);
        if (be16(os2 + 74) || be16(os2 + 76)) { t->winasc = be16(os2 + 74); t->windesc = be16(os2 + 76); }
        if (be16(os2) >= 2 && l >= 90) { t->xh = (i16)be16(os2 + 86); t->caph = (i16)be16(os2 + 88); }
    }
    if (post) { t->ulpos = (i16)be16(post + 8); t->ulsize = (i16)be16(post + 10); }
    if (t->winasc + t->windesc <= 0) { t->winasc = t->upem; t->windesc = 0; }
    t->ok = 1;
    return t;
}

/* ---- a font the program created ---- */
typedef struct {
    u8 lf[92];           /* the LOGFONTW it asked for */
    wfontinfo_t *f;
    tface_t *t;
    float sx, sy;        /* font units -> pixels */
    int asc, desc, height, em;
} tfont_t;
static const u16 *lf_face(const u8 *lf) { return (const u16 *)(lf + 28); }
static int is_icon_font(const wfontinfo_t *f) { u16 w[24]; a2w("Segoe Fluent Icons", w, 24); return wcsieq(f->family, w); }
/* the font Windows would pick: same family, closest weight, same slant; otherwise a plain sans-serif */
static wfontinfo_t *font_match(const u16 *face, int weight, int italic) {
    fonts_scan();
    static const char *const fallback[] = {"Segoe UI", "DejaVu Sans", "Arial", "Open Sans", "Roboto"};
    for (int pass = 0; pass <= 6; pass++) {
        u16 want[32];
        if (pass == 0) { if (!face || !face[0]) continue; int i = 0; for (; i < 31 && face[i]; i++) want[i] = face[i]; want[i] = 0; }
        else if (pass <= 5) a2w(fallback[pass - 1], want, 32);
        wfontinfo_t *best = NULL;
        int bs = 1 << 30;
        for (int i = 0; i < nfonts; i++) {
            wfontinfo_t *f = &fonts[i];
            if (pass <= 5 ? !wcsieq(f->family, want) : is_icon_font(f)) continue;
            int s = (f->weight > weight ? f->weight - weight : weight - f->weight) + (f->italic != italic ? 1000 : 0);
            if (s < bs) { bs = s; best = f; }
        }
        if (best) return best;
    }
    return NULL;
}
static int iround(float v) { return v < 0 ? -(int)(-v + 0.5f) : (int)(v + 0.5f); }
static tfont_t *tfont_make(const u8 *lf) {
    i32 h = *(const i32 *)lf, w = *(const i32 *)(lf + 4), weight = *(const i32 *)(lf + 16);
    if (!weight) weight = 400;
    wfontinfo_t *f = font_match(lf_face(lf), weight, lf[20] != 0);
    tface_t *t = f ? face_load(f) : NULL;
    if (!t) return NULL;
    tfont_t *tf = p_alloc(sizeof *tf, 1);
    if (!tf) return NULL;
    memcpy(tf->lf, lf, 92);
    tf->f = f; tf->t = t;
    int cell = t->winasc + t->windesc;
    if (h < 0) tf->sy = (float)-h / (float)t->upem;
    else if (h > 0) tf->sy = (float)h / (float)cell;
    else tf->sy = 16.0f / (float)cell;
    tf->sx = w > 0 && t->avgw > 0 ? (float)w / (float)t->avgw : tf->sy;
    tf->asc = iround(t->winasc * tf->sy);
    tf->desc = iround(t->windesc * tf->sy);
    tf->height = tf->asc + tf->desc;
    tf->em = iround(t->upem * tf->sy);
    return tf;
}
static tfont_t *dc_tfont(dc_t *d) {
    gobj_t *o = d && d->font ? d->font : &stock_obj[17];
    if (!o->ttf) { /* a stock font: the system's UI font at 9 points */
        u8 lf[92] = {0};
        *(i32 *)lf = -12;
        *(i32 *)(lf + 16) = 400;
        o->ttf = tfont_make(lf);
    }
    return o->ttf;
}

/* ---- drawing one letter ---- */
typedef struct { int x0, y0, w, h; float advx, advy; u8 *px; } tglyph_t; /* pixels are kmalloc'd, 0..255 */
/* m: 2x2 matrix applied in pixels (y down), as in XFORM: x' = x*m0 + y*m2, y' = x*m1 + y*m3 */
static int glyph_render(tfont_t *tf, int g, const float *m, tglyph_t *out, int want_px) {
    stbtt_vertex *v = NULL;
    int n = stbtt_GetGlyphShape(&tf->t->info, g, &v);
    int adv, lsb;
    stbtt_GetGlyphHMetrics(&tf->t->info, g, &adv, &lsb);
    float ax = adv * tf->sx;
    out->advx = ax * m[0];
    out->advy = ax * m[1];
    out->px = NULL;
    out->x0 = out->y0 = out->w = out->h = 0;
    if (n <= 0) { kfree(v); return 1; }
    int minx = 1 << 30, miny = 1 << 30, maxx = -(1 << 30), maxy = -(1 << 30);
#define TT_XF(X, Y) do { \
        float lx = (X) * tf->sx, ly = -(Y) * tf->sy; \
        float dx = lx * m[0] + ly * m[2], dy = lx * m[1] + ly * m[3]; \
        X = iround(dx * 64); Y = iround(-dy * 64); \
    } while (0)
    for (int i = 0; i < n; i++) {
        TT_XF(v[i].x, v[i].y);
        TT_XF(v[i].cx, v[i].cy);
        TT_XF(v[i].cx1, v[i].cy1);
        int px[3] = {v[i].x, v[i].cx, v[i].cx1}, py[3] = {v[i].y, v[i].cy, v[i].cy1};
        int k = v[i].type == STBTT_vline || v[i].type == STBTT_vmove ? 1 : v[i].type == STBTT_vcurve ? 2 : 3;
        for (int j = 0; j < k; j++) {
            if (px[j] < minx) minx = px[j];
            if (px[j] > maxx) maxx = px[j];
            if (py[j] < miny) miny = py[j];
            if (py[j] > maxy) maxy = py[j];
        }
    }
#undef TT_XF
    int x0 = (int)tt_floor(minx / 64.0), x1 = (int)tt_ceil(maxx / 64.0);
    int y0 = (int)tt_floor(-maxy / 64.0), y1 = (int)tt_ceil(-miny / 64.0);
    out->x0 = x0; out->y0 = y0; out->w = x1 - x0; out->h = y1 - y0;
    if (out->w <= 0 || out->h <= 0 || out->w > 2048 || out->h > 2048) { out->w = out->h = 0; kfree(v); return 1; }
    if (want_px) {
        out->px = kzalloc((size_t)out->w * (size_t)out->h);
        if (!out->px) { kfree(v); return 0; }
        stbtt__bitmap bm = {out->w, out->h, out->w, out->px};
        stbtt_Rasterize(&bm, 0.35f, v, n, 1.0f / 64, 1.0f / 64, 0, 0, x0, y0, 1, NULL);
    }
    kfree(v);
    return 1;
}
static const float ident[4] = {1, 0, 0, 1};
static int glyph_of(tfont_t *tf, u32 ch, int is_index) { return is_index ? (int)ch : stbtt_FindGlyphIndex(&tf->t->info, (int)ch); }

/* ---- fonts: create, describe ---- */
static WINAPI void *x_CreateFontIndirectW(const u8 *lf) {
    if (!lf) return NULL;
    tfont_t *tf = tfont_make(lf);
    if (!tf) { klog("exe: no hay ninguna fuente TrueType en C:\\WINDOWS\\FONTS\n"); return NULL; }
    gobj_t *o = new_obj(K_FONT, 0);
    o->font = tf->height >= 20 ? CW_FONT_TITLE : *(const i32 *)(lf + 16) >= 600 ? CW_FONT_BOLD : CW_FONT_UI; /* for the A functions */
    o->ttf = tf;
    return o;
}
static WINAPI void *x_CreateFontW(int h, int w, int e, int o, int weight, u32 it, u32 un, u32 st, u32 cs, u32 op, u32 cp, u32 q, u32 pf, const u16 *face) {
    u8 lf[92] = {0};
    i32 *l = (i32 *)lf;
    l[0] = h; l[1] = w; l[2] = e; l[3] = o; l[4] = weight;
    lf[20] = (u8)it; lf[21] = (u8)un; lf[22] = (u8)st; lf[23] = (u8)cs; lf[24] = (u8)op; lf[25] = (u8)cp; lf[26] = (u8)q; lf[27] = (u8)pf;
    if (face) for (int i = 0; i < 31 && face[i]; i++) ((u16 *)(lf + 28))[i] = face[i];
    return x_CreateFontIndirectW(lf);
}
static void tm_fill(tfont_t *tf, u8 *tm) {
    tface_t *t = tf->t;
    memset(tm, 0, 60);
    i32 *v = (i32 *)tm;
    int lgap = iround((t->asc - t->desc + t->gap) * tf->sy) - tf->height;
    v[0] = tf->height; v[1] = tf->asc; v[2] = tf->desc; v[3] = tf->height - tf->em; v[4] = lgap > 0 ? lgap : 0;
    v[5] = iround(t->avgw * tf->sx); v[6] = iround(t->maxw * tf->sx); v[7] = tf->f->weight; v[8] = 0; v[9] = 96; v[10] = 96;
    u16 *c = (u16 *)(tm + 44);
    c[0] = 32; c[1] = 0xFFFC; c[2] = 31; c[3] = 32;
    tm[52] = (u8)tf->f->italic;
    tm[53] = tf->lf[21];
    tm[54] = tf->lf[22];
    tm[55] = (u8)((tf->f->fixed ? 0 : 1) | 2 | 4 | (tf->f->fixed ? 0x30 : 0x20));
    tm[56] = 0; /* ANSI_CHARSET */
}
static WINAPI int x_GetTextMetricsW(dc_t *d, u8 *tm) {
    tfont_t *tf = dc_tfont(d);
    if (!tf || !tm) return 0;
    tm_fill(tf, tm);
    return 1;
}
static WINAPI int x_GetTextMetricsA(dc_t *d, u8 *tm) {
    u8 w[60];
    if (!x_GetTextMetricsW(d, w)) return 0;
    memcpy(tm, w, 44);
    tm[44] = 32; tm[45] = 255; tm[46] = 31; tm[47] = 32;
    memcpy(tm + 48, w + 52, 5);
    return 1;
}
static u32 tt_wcopy(u16 *dst, const u16 *src) { u32 n = 0; while (src[n]) { dst[n] = src[n]; n++; } dst[n] = 0; return n + 1; }
static WINAPI u32 x_GetOutlineTextMetricsW(dc_t *d, u32 cb, u8 *o) {
    tfont_t *tf = dc_tfont(d);
    if (!tf) return 0;
    wfontinfo_t *f = tf->f;
    u32 need = 232 + 2 * ((u32)c_wcslen(f->family) + 1 + (u32)c_wcslen(f->full) + 1 + (u32)c_wcslen(f->style) + 1 + (u32)c_wcslen(f->full) + 1);
    if (!o) return need;
    if (cb < need) return 0;
    tface_t *t = tf->t;
    float s = tf->sy;
    memset(o, 0, need);
    *(u32 *)o = need;
    tm_fill(tf, o + 4);
    i32 *v = (i32 *)(o + 76);
    v[0] = t->fsel; v[1] = t->fstype; v[2] = 1; v[3] = 0; v[4] = 0;     /* fsSelection, fsType, slope rise/run, italic angle */
    v[5] = t->upem;
    v[6] = iround(t->asc * s); v[7] = iround(t->desc * s); v[8] = iround(t->gap * s);
    v[9] = iround(t->caph * s); v[10] = iround(t->xh * s);
    int bx0, by0, bx1, by1;
    stbtt_GetFontBoundingBox(&t->info, &bx0, &by0, &bx1, &by1);
    v[11] = iround(bx0 * tf->sx); v[12] = iround(by1 * s); v[13] = iround(bx1 * tf->sx); v[14] = iround(by0 * s);
    v[15] = iround(t->asc * s); v[16] = iround(t->desc * s); v[17] = iround(t->gap * s); v[18] = 9;
    v[19] = v[20] = iround(t->upem * s * 0.6f); v[21] = 0; v[22] = iround(t->upem * s * 0.1f);   /* subscript */
    v[23] = v[24] = iround(t->upem * s * 0.6f); v[25] = 0; v[26] = iround(t->upem * s * 0.35f);  /* superscript */
    v[27] = imax(1, iround(t->stsize * s)); v[28] = iround(t->stpos * s);
    v[29] = imax(1, iround(t->ulsize * s)); v[30] = iround(t->ulpos * s);
    u32 at = 232;
    u64 *ptr = (u64 *)(o + 200);
    const u16 *str[4] = {f->family, f->full, f->style, f->full};
    for (int i = 0; i < 4; i++) { ptr[i] = at; at += 2 * tt_wcopy((u16 *)(o + at), str[i]); }
    return need;
}
static WINAPI int x_GetTextFaceW(dc_t *d, int n, u16 *buf) {
    tfont_t *tf = dc_tfont(d);
    if (!tf) return 0;
    int len = (int)c_wcslen(tf->f->family);
    if (!buf) return len + 1;
    if (n <= 0) return 0;
    int k = len < n - 1 ? len : n - 1;
    memcpy(buf, tf->f->family, 2 * (u64)k);
    buf[k] = 0;
    return k + 1;
}
static WINAPI u32 x_GetFontData(dc_t *d, u32 table, u32 off, u8 *buf, u32 cb) {
    tfont_t *tf = dc_tfont(d);
    if (!tf) return 0xFFFFFFFF;
    const u8 *p = tf->f->data;
    u32 len = tf->f->size;
    if (table) {
        char tag[4] = {(char)table, (char)(table >> 8), (char)(table >> 16), (char)(table >> 24)};
        p = sfnt_table(tf->f->data, tf->f->size, tag, &len);
        if (!p) return 0xFFFFFFFF;
    }
    if (off > len) return 0xFFFFFFFF;
    len -= off;
    if (!buf || !cb) return len;
    if (cb > len) cb = len;
    memcpy(buf, p + off, cb);
    return cb;
}
static WINAPI u32 x_GetGlyphIndicesW(dc_t *d, const u16 *s, int n, u16 *out, u32 fl) {
    tfont_t *tf = dc_tfont(d);
    if (!tf || n < 0) return 0xFFFFFFFF;
    for (int i = 0; i < n; i++) {
        int g = stbtt_FindGlyphIndex(&tf->t->info, s[i]);
        out[i] = (u16)(g ? g : (fl & 1) ? 0xFFFF : 0);
    }
    return (u32)n;
}
static WINAPI u32 x_GetKerningPairsW(dc_t *d, u32 n, void *p) { return 0; }
static WINAPI u32 x_GetFontUnicodeRanges(dc_t *d, void *gs) { return 0; }
static WINAPI int x_GetTextCharsetInfo(dc_t *d, void *sig, u32 fl) { if (sig) memset(sig, 0, 24); return 0; }
static WINAPI int x_GetTextCharset(dc_t *d) { return 0; }
static WINAPI u32 x_GetFontLanguageInfo(dc_t *d) { return 0; }

/* ---- per-letter measurements ---- */
static void abc_of(tfont_t *tf, int g, float *a, float *b, float *c) {
    int adv, lsb, x0, y0, x1, y1;
    stbtt_GetGlyphHMetrics(&tf->t->info, g, &adv, &lsb);
    if (!stbtt_GetGlyphBox(&tf->t->info, g, &x0, &y0, &x1, &y1)) x0 = x1 = lsb;
    *a = x0 * tf->sx;
    *b = (x1 - x0) * tf->sx;
    *c = adv * tf->sx - *a - *b;
}
typedef struct { i32 a; u32 b; i32 c; } ABC;
static void abc_int(tfont_t *tf, int g, ABC *r) {
    float a, b, c;
    abc_of(tf, g, &a, &b, &c);
    int adv = iround(a + b + c);
    r->a = (i32)tt_floor(a);
    r->b = (u32)imax(1, (int)tt_ceil(a + b) - r->a);
    r->c = adv - r->a - (i32)r->b;
}
static WINAPI int x_GetCharABCWidthsW(dc_t *d, u32 first, u32 last, ABC *r) {
    tfont_t *tf = dc_tfont(d);
    if (!tf || last < first) return 0;
    for (u32 ch = first; ch <= last; ch++) abc_int(tf, glyph_of(tf, ch, 0), &r[ch - first]);
    return 1;
}
static WINAPI int x_GetCharABCWidthsI(dc_t *d, u32 first, u32 n, const u16 *gi, ABC *r) {
    tfont_t *tf = dc_tfont(d);
    if (!tf) return 0;
    for (u32 i = 0; i < n; i++) abc_int(tf, gi ? gi[i] : (int)(first + i), &r[i]);
    return 1;
}
static WINAPI int x_GetCharABCWidthsFloatW(dc_t *d, u32 first, u32 last, float *r) {
    tfont_t *tf = dc_tfont(d);
    if (!tf || last < first) return 0;
    for (u32 ch = first; ch <= last; ch++) abc_of(tf, glyph_of(tf, ch, 0), &r[3 * (ch - first)], &r[3 * (ch - first) + 1], &r[3 * (ch - first) + 2]);
    return 1;
}
static int adv_of(tfont_t *tf, int g) { int adv, lsb; stbtt_GetGlyphHMetrics(&tf->t->info, g, &adv, &lsb); return iround(adv * tf->sx); }
static WINAPI int x_GetCharWidthW(dc_t *d, u32 first, u32 last, i32 *w) {
    tfont_t *tf = dc_tfont(d);
    if (!tf || last < first) return 0;
    for (u32 ch = first; ch <= last; ch++) w[ch - first] = adv_of(tf, glyph_of(tf, ch, 0));
    return 1;
}
static WINAPI int x_GetCharWidthI(dc_t *d, u32 first, u32 n, const u16 *gi, i32 *w) {
    tfont_t *tf = dc_tfont(d);
    if (!tf) return 0;
    for (u32 i = 0; i < n; i++) w[i] = adv_of(tf, gi ? gi[i] : (int)(first + i));
    return 1;
}
static WINAPI int x_GetTextExtentExPointW(dc_t *d, const u16 *s, int n, int max, i32 *fit, i32 *dx, i32 *size) {
    tfont_t *tf = dc_tfont(d);
    if (!tf || n < 0) return 0;
    int x = 0, k = 0;
    for (int i = 0; i < n; i++) {
        x += adv_of(tf, glyph_of(tf, s[i], 0));
        if (dx) dx[i] = x;
        if (x <= max) k = i + 1;
    }
    if (fit) *fit = k;
    size[0] = x;
    size[1] = tf->height;
    return 1;
}
static WINAPI int x_GetTextExtentPoint32W(dc_t *d, const u16 *s, int n, i32 *size) { return x_GetTextExtentExPointW(d, s, n, 0, NULL, NULL, size); }
static WINAPI int x_GetTextExtentPointI(dc_t *d, const u16 *gi, int n, i32 *size) {
    tfont_t *tf = dc_tfont(d);
    if (!tf) return 0;
    int x = 0;
    for (int i = 0; i < n; i++) x += adv_of(tf, gi[i]);
    size[0] = x;
    size[1] = tf->height;
    return 1;
}

/* ---- GetGlyphOutlineW: metrics, gray bitmaps and outlines of one letter ---- */
typedef struct { u32 bx, by; i32 ox, oy; i16 incx, incy; } GLYPHMETRICS;
static float fixed_of(const u8 *p) { return (float)(i16)(p[2] | p[3] << 8) + (float)(p[0] | p[1] << 8) / 65536.0f; }
static void put_fx(u8 *p, float v) { i32 f = (i32)tt_floor(v * 65536.0 + 0.5); memcpy(p, &f, 4); }
static void dc_matrix(dc_t *d, float *m) {
    if (d && d->gmode == 2 && d->has_xf) { m[0] = d->xf[0]; m[1] = d->xf[1]; m[2] = d->xf[2]; m[3] = d->xf[3]; }
    else memcpy(m, ident, sizeof ident);
}
static WINAPI u32 x_GetGlyphOutlineW(dc_t *d, u32 ch, u32 fmt, GLYPHMETRICS *gm, u32 cb, u8 *buf, const u8 *mat) {
    tfont_t *tf = dc_tfont(d);
    if (!tf || !gm) { last_error = 87; return 0xFFFFFFFF; }
    int g = glyph_of(tf, ch, fmt & 0x80);
    u32 kind = fmt & 0x0F;
    float m[4], w[4];
    dc_matrix(d, w);
    if (mat) { /* MAT2 is y up; XFORM-style m is y down: flip the off-diagonal terms */
        float a = fixed_of(mat), b = fixed_of(mat + 4), c = fixed_of(mat + 8), e = fixed_of(mat + 12);
        float mm[4] = {a, -b, -c, e};
        m[0] = w[0] * mm[0] + w[1] * mm[2]; m[1] = w[0] * mm[1] + w[1] * mm[3];
        m[2] = w[2] * mm[0] + w[3] * mm[2]; m[3] = w[2] * mm[1] + w[3] * mm[3];
    } else memcpy(m, w, sizeof m);
    tglyph_t gl;
    if (!glyph_render(tf, g, m, &gl, kind == 1 || (kind >= 4 && kind <= 6))) return 0xFFFFFFFF;
    int empty = gl.w <= 0 || gl.h <= 0;
    gm->bx = empty ? 1 : (u32)gl.w;
    gm->by = empty ? 1 : (u32)gl.h;
    gm->ox = empty ? 0 : gl.x0;
    gm->oy = empty ? 0 : -gl.y0;
    gm->incx = (i16)iround(gl.advx);
    gm->incy = (i16)-iround(gl.advy);
    if (kind == 0) { kfree(gl.px); return 1; }
    if (kind == 1 || (kind >= 4 && kind <= 6)) { /* bitmaps: rows padded to 4 bytes */
        int levels = kind == 4 ? 4 : kind == 5 ? 16 : 64;
        u32 row = kind == 1 ? (((u32)gl.w + 31) / 32) * 4 : ((u32)gl.w + 3) & ~3u;
        u32 need = empty ? 0 : row * (u32)gl.h;
        if (!buf || !cb) { kfree(gl.px); return need; }
        if (cb < need) { kfree(gl.px); return 0xFFFFFFFF; }
        memset(buf, 0, need);
        for (int y = 0; y < gl.h; y++)
            for (int x = 0; x < gl.w; x++) {
                u32 a = gl.px[y * gl.w + x];
                if (kind == 1) { if (a >= 128) buf[y * row + x / 8] |= (u8)(0x80 >> (x & 7)); }
                else buf[y * row + x] = (u8)((a * (u32)levels + 127) / 255);
            }
        kfree(gl.px);
        return need;
    }
    if (kind != 2 && kind != 3) { last_error = 87; return 0xFFFFFFFF; }
    /* GGO_NATIVE / GGO_BEZIER: TTPOLYGONHEADER + TTPOLYCURVE records, 16.16 pixels, y up */
    stbtt_vertex *v = NULL;
    int n = stbtt_GetGlyphShape(&tf->t->info, g, &v);
    u32 need = 0, at = 0, head = 0;
    for (int pass = 0; pass < 2; pass++) {
        at = 0;
        for (int i = 0; i < n; i++) {
            float pts[3][2];
            int k = v[i].type == STBTT_vcurve ? 2 : v[i].type == STBTT_vcubic ? 3 : 1;
            int xs[3] = {v[i].cx, v[i].cx1, v[i].x}, ys[3] = {v[i].cy, v[i].cy1, v[i].y};
            if (k == 1) { xs[0] = v[i].x; ys[0] = v[i].y; }
            if (k == 2) { xs[1] = v[i].x; ys[1] = v[i].y; }
            for (int j = 0; j < k; j++) {
                float lx = xs[j] * tf->sx, ly = -ys[j] * tf->sy;
                pts[j][0] = lx * m[0] + ly * m[2];
                pts[j][1] = -(lx * m[1] + ly * m[3]);
            }
            if (v[i].type == STBTT_vmove) {
                if (pass && at) *(u32 *)(buf + head) = at - head;
                head = at;
                if (pass) { *(u32 *)(buf + at + 4) = 24; put_fx(buf + at + 8, pts[0][0]); put_fx(buf + at + 12, pts[0][1]); }
                at += 16;
                continue;
            }
            if (pass) {
                *(u16 *)(buf + at) = (u16)(k == 1 ? 1 : k == 2 ? 2 : 3);
                *(u16 *)(buf + at + 2) = (u16)k;
                for (int j = 0; j < k; j++) { put_fx(buf + at + 4 + 8 * j, pts[j][0]); put_fx(buf + at + 8 + 8 * j, pts[j][1]); }
            }
            at += 4 + 8 * (u32)k;
        }
        if (pass && at) *(u32 *)(buf + head) = at - head;
        need = at;
        if (!pass && (!buf || !cb)) break;
        if (!pass && cb < need) { kfree(v); return 0xFFFFFFFF; }
        if (!pass) memset(buf, 0, need);
    }
    kfree(v);
    return need;
}

/* ---- drawing text on a DC ---- */
static void mask_draw(surface_t *s, rect_t clip, const tglyph_t *g, int x, int y, u32 color) {
    rect_t r = rect_intersect(rect_intersect((rect_t){x, y, g->w, g->h}, s->clip), clip);
    for (int yy = 0; yy < r.h; yy++) {
        const u8 *m = g->px + (r.y + yy - y) * g->w + (r.x - x);
        u32 *p = s->px + (r.y + yy) * s->stride + r.x;
        for (int xx = 0; xx < r.w; xx++)
            if (m[xx]) { u32 a = p[xx] & 0xFF000000; p[xx] = (blend(p[xx], color, m[xx]) & 0xFFFFFF) | a; }
    }
}
static WINAPI int x_ExtTextOutW(dc_t *d, int x, int y, u32 opt, const RECT *rc, const u16 *str, u32 n, const i32 *dx) {
    tfont_t *tf = dc_tfont(d);
    surface_t *s = dc_surf(d);
    if (!tf) return 0;
    if (!s) return 1;
    rect_t clip = {-100000, -100000, 200000, 200000};
    if (rc && (opt & 4)) clip = (rect_t){rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top};
    if (rc && (opt & 2)) fill_rect(s, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, d->bk);
    if (!str || !n) return 1;
    float m[4];
    dc_matrix(d, m);
    float ox = 0, oy = 0;
    if (d->gmode == 2 && d->has_xf) { ox = d->xf[4]; oy = d->xf[5]; }
    int pdy = (opt & 0x2000) != 0, gidx = (opt & 0x10) != 0;
    /* the starting point: TA_BASELINE / TA_TOP / TA_BOTTOM, TA_LEFT / TA_CENTER / TA_RIGHT */
    float bx = 0, by = 0, total = 0;
    for (u32 i = 0; i < n; i++) total += dx ? (float)dx[pdy ? 2 * i : i] : (float)adv_of(tf, glyph_of(tf, str[i], gidx));
    if ((d->talign & 6) == 6) bx = -total / 2; else if (d->talign & 2) bx = -total;
    if ((d->talign & 24) == 0) by = (float)tf->asc; else if ((d->talign & 24) == 8) by = (float)-tf->desc;
    if (d->bkmode == 2 && !(opt & 2) && !(d->gmode == 2 && d->has_xf))
        fill_rect(s, x + (int)bx, y + (int)by - tf->asc, (int)total, tf->height, d->bk);
    float px = x + bx, py = y + by; /* pen in logical units */
    for (u32 i = 0; i < n; i++) {
        tglyph_t g;
        int gi = glyph_of(tf, str[i], gidx);
        float sx = px * m[0] + py * m[2] + ox, sy = px * m[1] + py * m[3] + oy;
        if (glyph_render(tf, gi, m, &g, 1) && g.px) {
            mask_draw(s, clip, &g, iround(sx) + g.x0, iround(sy) + g.y0, d->text);
            kfree(g.px);
        }
        if (dx) { px += (float)dx[pdy ? 2 * i : i]; if (pdy) py += (float)dx[2 * i + 1]; }
        else px += (float)adv_of(tf, gi);
    }
    if ((tf->lf[21] || tf->lf[22]) && !(d->gmode == 2 && d->has_xf)) {
        tface_t *t = tf->t;
        int lx = iround(x + bx), ly = iround(y + by);
        if (tf->lf[21]) fill_rect(s, lx, ly - iround(t->ulpos * tf->sy), (int)total, imax(1, iround(t->ulsize * tf->sy)), d->text);
        if (tf->lf[22]) fill_rect(s, lx, ly - iround(t->stpos * tf->sy), (int)total, imax(1, iround(t->stsize * tf->sy)), d->text);
    }
    if (d->talign & 1) d->cx = iround(px);
    return 1;
}
static WINAPI int x_TextOutW(dc_t *d, int x, int y, const u16 *s, int n) { return x_ExtTextOutW(d, x, y, 0, NULL, s, (u32)imax(n, 0), NULL); }
static WINAPI int x_DrawTextW(dc_t *d, const u16 *s, int n, RECT *r, u32 fmt) {
    tfont_t *tf = dc_tfont(d);
    if (!tf || !s || !r) return 0;
    if (n < 0) n = (int)c_wcslen(s);
    i32 sz[2];
    x_GetTextExtentPoint32W(d, s, n, sz);
    if (fmt & 0x400) { r->right = r->left + sz[0]; r->bottom = r->top + sz[1]; return sz[1]; }
    int x = r->left, y = r->top;
    if (fmt & 1) x = (r->left + r->right - sz[0]) / 2; else if (fmt & 2) x = r->right - sz[0];
    if (fmt & 4) y = (r->top + r->bottom - sz[1]) / 2; else if (fmt & 8) y = r->bottom - sz[1];
    int ta = d->talign;
    d->talign = 0;
    x_ExtTextOutW(d, x, y, (fmt & 0x100) ? 0 : 4, r, s, (u32)n, NULL);
    d->talign = ta;
    return sz[1];
}

/* ---- DC state used with text ---- */
static WINAPI u32 x_SetTextAlign(dc_t *d, u32 a) { if (!d) return 0xFFFFFFFF; u32 o = (u32)d->talign; d->talign = (int)a; return o; }
static WINAPI u32 x_GetTextAlign(dc_t *d) { return d ? (u32)d->talign : 0xFFFFFFFF; }
static WINAPI int x_SetGraphicsMode(dc_t *d, int m) { if (!d) return 0; int o = d->gmode ? d->gmode : 1; d->gmode = m; return o; }
static WINAPI int x_GetGraphicsMode(dc_t *d) { return d && d->gmode ? d->gmode : 1; }
static WINAPI int x_SetWorldTransform(dc_t *d, const float *xf) {
    if (!d || !xf || d->gmode != 2) return 0;
    memcpy(d->xf, xf, sizeof d->xf);
    d->has_xf = !(xf[0] == 1 && xf[1] == 0 && xf[2] == 0 && xf[3] == 1 && xf[4] == 0 && xf[5] == 0);
    return 1;
}
static WINAPI int x_GetWorldTransform(dc_t *d, float *xf) {
    static const float id6[6] = {1, 0, 0, 1, 0, 0};
    if (!d || !xf) return 0;
    memcpy(xf, d->has_xf ? d->xf : id6, sizeof id6);
    return 1;
}
static WINAPI int x_ModifyWorldTransform(dc_t *d, const float *xf, u32 mode) {
    if (!d || d->gmode != 2) return 0;
    float cur6[6], r[6];
    x_GetWorldTransform(d, cur6);
    if (mode == 1) { static const float id6[6] = {1, 0, 0, 1, 0, 0}; d->has_xf = 0; memcpy(d->xf, id6, sizeof id6); return 1; }
    const float *a = mode == 2 ? xf : cur6, *b = mode == 2 ? cur6 : xf; /* MWT_LEFTMULTIPLY: xf * current */
    r[0] = a[0] * b[0] + a[1] * b[2]; r[1] = a[0] * b[1] + a[1] * b[3];
    r[2] = a[2] * b[0] + a[3] * b[2]; r[3] = a[2] * b[1] + a[3] * b[3];
    r[4] = a[4] * b[0] + a[5] * b[2] + b[4]; r[5] = a[4] * b[1] + a[5] * b[3] + b[5];
    return x_SetWorldTransform(d, r);
}
static WINAPI int x_SetMapMode(dc_t *d, int m) { return 1; }
static WINAPI int x_GetMapMode(dc_t *d) { return 1; }
static WINAPI u32 x_GetTextColor(dc_t *d) { u32 c = d ? d->text : 0; return (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF); }
static WINAPI u32 x_GetBkColor(dc_t *d) { u32 c = d ? d->bk : 0; return (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF); }
static WINAPI int x_GetBkMode(dc_t *d) { return d ? d->bkmode : 0; }
static WINAPI void *x_GetCurrentObject(dc_t *d, u32 type) {
    if (!d) return NULL;
    return type == 1 ? (void *)d->pen : type == 2 ? (void *)d->brush : type == 6 ? (void *)d->font : type == 7 ? (void *)d->bmp : NULL;
}
static WINAPI int x_GetObjectW(gobj_t *o, int n, u8 *out) {
    if (!o) return 0;
    if (o->kind == K_FONT) {
        if (!out) return 92;
        tfont_t *tf = o->ttf;
        u8 lf[92] = {0};
        if (tf) memcpy(lf, tf->lf, 92); else { *(i32 *)lf = -12; *(i32 *)(lf + 16) = 400; }
        memcpy(out, lf, (u64)imin(n, 92));
        return imin(n, 92);
    }
    if (o->kind == K_BITMAP) {
        if (!out) return 32;
        u8 bm[32] = {0};
        i32 *v = (i32 *)bm;
        v[1] = o->surf.w; v[2] = o->surf.h; v[3] = o->surf.w * 4;
        *(u16 *)(bm + 16) = 1; *(u16 *)(bm + 18) = 32;
        if (o->dib) *(void **)(bm + 24) = o->base;
        memcpy(out, bm, (u64)imin(n, 32));
        return imin(n, 32);
    }
    if (o->kind == K_BRUSH || o->kind == K_PEN) {
        if (!out) return 16;
        u8 b[16] = {0};
        u32 c = o->color;
        *(u32 *)b = o->null ? (o->kind == K_PEN ? 5 : 1) : 0;
        *(u32 *)(b + 4) = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
        memcpy(out, b, (u64)imin(n, 16));
        return imin(n, 16);
    }
    return 0;
}
static WINAPI int x_GetObjectA(gobj_t *o, int n, u8 *out) {
    if (!o || o->kind != K_FONT) return x_GetObjectW(o, n, out);
    if (!out) return 60;
    u8 w[92];
    x_GetObjectW(o, 92, w);
    u8 a[60] = {0};
    memcpy(a, w, 28);
    w2a((const u16 *)(w + 28), (char *)a + 28, 32);
    memcpy(out, a, (u64)imin(n, 60));
    return imin(n, 60);
}

/* ---- fonts the program brings ---- */
static WINAPI void *x_AddFontMemResourceEx(const u8 *p, u32 cb, void *pdv, u32 *count) {
    fonts_scan();
    if (count) *count = 0;
    if (!p || cb < 12) return NULL;
    u8 *copy = kmalloc(cb);
    if (!copy) return NULL;
    memcpy(copy, p, cb);
    wfontinfo_t *f = font_add("", copy, cb);
    if (!f) { kfree(copy); return NULL; }
    f->data = copy; f->size = cb; f->priv = 1;
    if (count) *count = 1;
    return f;
}
static WINAPI int x_RemoveFontMemResourceEx(void *h) { return 1; } /* kept: fonts made from it may still be in use */
static WINAPI int x_AddFontResourceExW(const u16 *name, u32 fl, void *pdv) {
    fonts_scan();
    char a[320], fp[320];
    w2a(name, a, sizeof a);
    if (fix_path(a, fp, sizeof fp) != 0 || !fat_mounted) return 0;
    for (int i = 0; i < nfonts; i++) { char q[96]; ksnprintf(q, sizeof q, FONT_DIR "/%s", fonts[i].file); if (c__stricmp(q, fp) == 0) return 1; }
    u8 *d; u32 sz;
    if (fat_read(fp, &d, &sz) != 0) return 0;
    const char *base = fp;
    for (const char *c = fp; *c; c++) if (*c == '/') base = c + 1;
    wfontinfo_t *f = font_add(base, d, sz);
    if (!f) { kfree(d); return 0; }
    f->data = d; f->size = sz; f->priv = (fl & 0x10) != 0; /* FR_PRIVATE */
    return 1;
}
static WINAPI int x_AddFontResourceW(const u16 *name) { return x_AddFontResourceExW(name, 0, NULL); }
static WINAPI int x_RemoveFontResourceExW(const u16 *name, u32 fl, void *pdv) { return 1; }

static const struct { const char *name; void *fn; } exports_text[] = {
    {"CreateFontIndirectW", x_CreateFontIndirectW}, {"CreateFontIndirectExW", x_CreateFontIndirectW}, {"CreateFontW", x_CreateFontW},
    {"GetTextMetricsW", x_GetTextMetricsW}, {"GetTextMetricsA", x_GetTextMetricsA}, {"GetOutlineTextMetricsW", x_GetOutlineTextMetricsW},
    {"GetTextFaceW", x_GetTextFaceW}, {"GetFontData", x_GetFontData}, {"GetGlyphIndicesW", x_GetGlyphIndicesW},
    {"GetKerningPairsW", x_GetKerningPairsW}, {"GetKerningPairs", x_GetKerningPairsW}, {"GetFontUnicodeRanges", x_GetFontUnicodeRanges},
    {"GetTextCharsetInfo", x_GetTextCharsetInfo}, {"GetTextCharset", x_GetTextCharset}, {"GetFontLanguageInfo", x_GetFontLanguageInfo},
    {"GetCharABCWidthsW", x_GetCharABCWidthsW}, {"GetCharABCWidthsI", x_GetCharABCWidthsI}, {"GetCharABCWidthsFloatW", x_GetCharABCWidthsFloatW},
    {"GetCharWidthW", x_GetCharWidthW}, {"GetCharWidth32W", x_GetCharWidthW}, {"GetCharWidthI", x_GetCharWidthI},
    {"GetTextExtentPoint32W", x_GetTextExtentPoint32W}, {"GetTextExtentPointW", x_GetTextExtentPoint32W},
    {"GetTextExtentExPointW", x_GetTextExtentExPointW}, {"GetTextExtentPointI", x_GetTextExtentPointI},
    {"GetGlyphOutlineW", x_GetGlyphOutlineW}, {"GetGlyphOutline", x_GetGlyphOutlineW},
    {"ExtTextOutW", x_ExtTextOutW}, {"TextOutW", x_TextOutW}, {"DrawTextW", x_DrawTextW},
    {"SetTextAlign", x_SetTextAlign}, {"GetTextAlign", x_GetTextAlign}, {"SetGraphicsMode", x_SetGraphicsMode}, {"GetGraphicsMode", x_GetGraphicsMode},
    {"SetWorldTransform", x_SetWorldTransform}, {"GetWorldTransform", x_GetWorldTransform}, {"ModifyWorldTransform", x_ModifyWorldTransform},
    {"SetMapMode", x_SetMapMode}, {"GetMapMode", x_GetMapMode}, {"GetTextColor", x_GetTextColor}, {"GetBkColor", x_GetBkColor}, {"GetBkMode", x_GetBkMode},
    {"GetCurrentObject", x_GetCurrentObject}, {"GetObjectW", x_GetObjectW}, {"GetObjectA", x_GetObjectA},
    {"AddFontMemResourceEx", x_AddFontMemResourceEx}, {"RemoveFontMemResourceEx", x_RemoveFontMemResourceEx},
    {"AddFontResourceExW", x_AddFontResourceExW}, {"AddFontResourceW", x_AddFontResourceW}, {"RemoveFontResourceExW", x_RemoveFontResourceExW},
};
