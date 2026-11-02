/* Program icons: the icon in a program's .exe (its resources), the icon a
 * program builds itself (CreateIconIndirect, as Qt does) and WM_SETICON.
 * Chamows shows them on the window's title bar, in the taskbar and in
 * notifications, like Windows 11. */

#define ICON_MAGIC 0x4E4F4349 /* "ICON" */
typedef struct {
    u32 magic;
    icon_t ic;
    u32 px[24 * 24 + 32 * 32 + 48 * 48];
} hicon_t;
static hicon_t *HI(u64 h) { hicon_t *i = (hicon_t *)h; return h > 0x10000 && !(h & 7) && i->magic == ICON_MAGIC ? i : NULL; }

/* straight-alpha ARGB, any size -> the three sizes Chamows draws (area average) */
static void icon_scale(const u32 *src, int sw, int sh, int sstride, u32 *dst, int n) {
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int x0 = x * sw / n, x1 = imax(x0 + 1, (x + 1) * sw / n), y0 = y * sh / n, y1 = imax(y0 + 1, (y + 1) * sh / n);
            u32 a = 0, r = 0, g = 0, b = 0, k = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    u32 p = src[yy * sstride + xx], pa = p >> 24;
                    a += pa; r += (p >> 16 & 0xFF) * pa; g += (p >> 8 & 0xFF) * pa; b += (p & 0xFF) * pa; k++;
                }
            dst[y * n + x] = a ? (a / k) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a) : 0;
        }
}
static hicon_t *icon_make(const u32 *src, int w, int h, int stride) {
    if (w <= 0 || h <= 0) return NULL;
    hicon_t *i = kzalloc(sizeof *i);
    if (!i) return NULL;
    i->magic = ICON_MAGIC;
    u32 *p24 = i->px, *p32 = p24 + 24 * 24, *p48 = p32 + 32 * 32;
    icon_scale(src, w, h, stride, p24, 24);
    icon_scale(src, w, h, stride, p32, 32);
    icon_scale(src, w, h, stride, p48, 48);
    i->ic = (icon_t){p24, p32, p48};
    return i;
}

/* ---- icons inside a module: RT_GROUP_ICON (14) -> RT_ICON (3) ---- */
static const u8 *res_dir_find(const u8 *base, const u8 *root, const u8 *dir, u32 id, const u16 *name) {
    u16 nnamed = *(const u16 *)(dir + 12), nid = *(const u16 *)(dir + 14);
    const u8 *e = dir + 16;
    for (u32 i = 0; i < (u32)nnamed + nid; i++, e += 8) {
        u32 n = *(const u32 *)e, off = *(const u32 *)(e + 4);
        int match;
        if (id == 0xFFFFFFFF && !name) match = 1; /* the first one */
        else if (n & 0x80000000) {
            const u16 *s = (const u16 *)(root + (n & 0x7FFFFFFF));
            match = 0;
            if (name) {
                u16 len = s[0];
                match = 1;
                for (u32 k = 0; k < len; k++) {
                    u16 a = s[1 + k], b = name[k];
                    if (a >= 'a' && a <= 'z') a -= 32;
                    if (b >= 'a' && b <= 'z') b -= 32;
                    if (a != b) { match = 0; break; }
                }
                if (match && name[len]) match = 0;
            }
        } else match = !name && n == id;
        if (match) return root + (off & 0x7FFFFFFF);
    }
    return NULL;
}
/* the data of resource type/name (first language); NULL if missing */
static const u8 *res_find(const u8 *base, u32 type, u32 id, const u16 *name, u32 *size) {
    if (!base || *(const u16 *)base != 0x5A4D) return NULL;
    const u8 *nt = base + *(const u32 *)(base + 0x3C);
    if (*(const u32 *)nt != 0x4550) return NULL;
    const u8 *opt = nt + 24;
    u32 rva = *(const u16 *)opt == 0x20B ? *(const u32 *)(opt + 112 + 16) : *(const u32 *)(opt + 96 + 16);
    if (!rva) return NULL;
    const u8 *root = base + rva;
    const u8 *d1 = res_dir_find(base, root, root, type, NULL);
    if (!d1) return NULL;
    const u8 *d2 = res_dir_find(base, root, d1, id, name);
    if (!d2) return NULL;
    const u8 *d3 = res_dir_find(base, root, d2, 0xFFFFFFFF, NULL); /* any language */
    if (!d3) return NULL;
    if (size) *size = *(const u32 *)(d3 + 4);
    return base + *(const u32 *)d3;
}
/* one RT_ICON image (a DIB with its AND mask; PNG images are skipped) as ARGB */
static u32 *dib_icon(const u8 *d, u32 size, int *ow, int *oh) {
    if (size < 40 || d[0] == 0x89) return NULL;
    u32 hs = *(const u32 *)d;
    i32 w = *(const i32 *)(d + 4), h = *(const i32 *)(d + 8) / 2;
    u16 bpp = *(const u16 *)(d + 14);
    u32 used = *(const u32 *)(d + 32);
    if (w <= 0 || h <= 0 || w > 256 || h > 256 || (bpp != 32 && bpp != 24 && bpp != 8 && bpp != 4 && bpp != 1)) return NULL;
    u32 ncol = bpp <= 8 ? (used ? used : 1u << bpp) : 0;
    const u8 *pal = d + hs, *xr = pal + 4 * ncol;
    u32 xs = (((u32)w * bpp + 31) / 32) * 4, ms = (((u32)w + 31) / 32) * 4;
    const u8 *mask = xr + xs * (u32)h;
    if (hs + 4 * ncol + xs * (u32)h > size) return NULL;
    int has_mask = mask + ms * (u32)h <= d + size;
    u32 *out = kmalloc((size_t)w * (size_t)h * 4);
    if (!out) return NULL;
    int any_alpha = 0;
    for (int y = 0; y < h; y++) {
        const u8 *row = xr + xs * (u32)(h - 1 - y);
        for (int x = 0; x < w; x++) {
            u32 c;
            if (bpp == 32) { c = *(const u32 *)(row + 4 * x); if (c >> 24) any_alpha = 1; }
            else if (bpp == 24) c = row[3 * x] | row[3 * x + 1] << 8 | row[3 * x + 2] << 16;
            else {
                u32 idx = bpp == 8 ? row[x] : bpp == 4 ? (row[x / 2] >> ((x & 1) ? 0 : 4)) & 15 : (row[x / 8] >> (7 - (x & 7))) & 1;
                c = idx < ncol ? *(const u32 *)(pal + 4 * idx) & 0xFFFFFF : 0;
            }
            out[y * w + x] = c;
        }
    }
    for (int y = 0; y < h; y++) {
        const u8 *mrow = mask + ms * (u32)(h - 1 - y);
        for (int x = 0; x < w; x++) {
            if (bpp == 32 && any_alpha) continue;
            int transparent = has_mask && (mrow[x / 8] >> (7 - (x & 7)) & 1);
            out[y * w + x] = (out[y * w + x] & 0xFFFFFF) | (transparent ? 0 : 0xFF000000u);
        }
    }
    *ow = w; *oh = h;
    return out;
}
/* the group's largest DIB image up to 48 pixels (or the smallest above that) */
static hicon_t *icon_from_group(const u8 *base, const u8 *grp, u32 gsize) {
    if (!grp || gsize < 6) return NULL;
    u16 n = *(const u16 *)(grp + 4);
    hicon_t *best = NULL;
    int best_score = -1;
    for (u32 i = 0; i < n && 6 + 14 * (i + 1) <= gsize; i++) {
        const u8 *e = grp + 6 + 14 * i;
        int w = e[0] ? e[0] : 256;
        u16 id = *(const u16 *)(e + 12);
        u32 sz;
        const u8 *img = res_find(base, 3, id, NULL, &sz);
        if (!img || img[0] == 0x89) continue;
        int score = w <= 48 ? w * 10 + *(const u16 *)(e + 6) : 1000 - w;
        if (score <= best_score) continue;
        int iw, ih;
        u32 *px = dib_icon(img, sz, &iw, &ih);
        if (!px) continue;
        hicon_t *h = icon_make(px, iw, ih, iw);
        kfree(px);
        if (!h) continue;
        if (best) kfree(best);
        best = h;
        best_score = score;
    }
    return best;
}
static hicon_t *icon_load(const u8 *base, const u16 *name) {
    u32 sz, id = 0xFFFFFFFF;
    const u16 *nm = NULL;
    if ((uptr)name < 0x10000) id = (u32)(uptr)name;
    else if (name[0] == '#') { id = 0; for (const u16 *c = name + 1; *c >= '0' && *c <= '9'; c++) id = id * 10 + (u32)(*c - '0'); }
    else nm = name;
    const u8 *grp = res_find(base, 14, id, nm, &sz);
    return grp ? icon_from_group(base, grp, sz) : NULL;
}
/* the .exe's own icon (its first icon group), for the taskbar and its windows */
static const icon_t *exe_icon(const u8 *image) {
    u32 sz = 0;
    const u8 *grp = res_find(image, 14, 0xFFFFFFFF, NULL, &sz);
    hicon_t *h = icon_from_group(image, grp, sz);
    return h ? &h->ic : NULL;
}

static const icon_t *icon_of(u64 h) { hicon_t *i = HI(h); return i ? &i->ic : NULL; }

/* ---- user32 / gdi32 ---- */
static WINAPI u64 i_LoadImageW(void *inst, const u16 *name, u32 type, int cx, int cy, u32 fl) {
    if (type == 1 && inst && !(fl & 0x10)) { /* IMAGE_ICON from the module, not LR_LOADFROMFILE */
        hicon_t *h = icon_load(inst, name);
        if (h) return (u64)h;
    }
    return 0x1000 + type; /* cursors and system images: a handle that draws nothing */
}
static WINAPI u64 i_LoadIconW(void *inst, const u16 *name) {
    if (inst) { hicon_t *h = icon_load(inst, name); if (h) return (u64)h; }
    return 0x1001;
}
static WINAPI u64 i_LoadIconA(void *inst, const char *name) {
    if ((uptr)name < 0x10000) return i_LoadIconW(inst, (const u16 *)name);
    u16 w[64];
    a2w(name, w, 64);
    return i_LoadIconW(inst, w);
}
/* bitmaps given as bits: 1 bit (icon masks) and 32 bits */
static WINAPI void *i_CreateBitmap(int w, int h, u32 planes, u32 bpp, const u8 *bits) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
    gobj_t *b = new_obj(K_BITMAP, 0);
    u32 *px = kzalloc((size_t)w * (size_t)h * 4);
    if (!px) { p_free(b); return NULL; }
    surf_init(&b->surf, px, w, h);
    if (bits && bpp == 1) {
        u32 stride = (((u32)w + 15) / 16) * 2;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) px[y * w + x] = (bits[y * stride + x / 8] >> (7 - (x & 7)) & 1) ? 0xFFFFFFFF : 0xFF000000;
    } else if (bits && bpp == 32) memcpy(px, bits, (size_t)w * (size_t)h * 4);
    return b;
}
static WINAPI void *i_CreateIconIndirect(const u8 *ii) {
    if (!ii) return NULL;
    gobj_t *mask = *(gobj_t **)(ii + 16), *color = *(gobj_t **)(ii + 24);
    gobj_t *src = color ? color : mask;
    if (!src || src->kind != K_BITMAP) return (void *)0x1001;
    int w = src->surf.w, h = src->surf.h;
    if (!color) h /= 2; /* monochrome icon: AND mask on top of XOR */
    u32 *px = kmalloc((size_t)w * (size_t)h * 4);
    if (!px) return NULL;
    int any_alpha = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) { u32 c = src->surf.px[y * src->surf.stride + x]; px[y * w + x] = c; if (c >> 24) any_alpha = 1; }
    if (!color || !any_alpha || !src->dib) /* no alpha of its own: the mask says what is transparent */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int t = mask && mask->kind == K_BITMAP && y < mask->surf.h && x < mask->surf.w && (mask->surf.px[y * mask->surf.stride + x] & 0xFFFFFF);
                px[y * w + x] = (px[y * w + x] & 0xFFFFFF) | (t ? 0 : 0xFF000000u);
            }
    hicon_t *ic = icon_make(px, w, h, w);
    kfree(px);
    return ic ? (void *)ic : (void *)0x1001;
}
static WINAPI void *i_CopyIcon(void *h) { return h; }

static const struct { const char *name; void *fn; } exports_icon[] = {
    {"LoadImageW", i_LoadImageW}, {"LoadIconW", i_LoadIconW}, {"LoadIconA", i_LoadIconA},
    {"CreateBitmap", i_CreateBitmap}, {"CreateIconIndirect", i_CreateIconIndirect}, {"CopyIcon", i_CopyIcon},
};
