/* Fonts for Windows programs: the TrueType/OpenType files in WINDOWS\FONTS on
 * the pendrive. EnumFontFamiliesExW lists them (names and coverage read from
 * the files themselves) and a tiny read-only registry gives their file names
 * under HKLM\...\CurrentVersion\Fonts, which is how Qt's FreeType font
 * database finds them. The program draws the text itself. */

typedef struct {
    char file[64];            /* name inside WINDOWS\FONTS */
    u16 family[64], style[32], full[96];
    int weight, italic, fixed;
    u32 usb[4], csb[2];       /* Unicode ranges and code pages the font covers (OS/2 table) */
} wfontinfo_t;
static wfontinfo_t *fonts;
static int nfonts = -1;
#define FONT_DIR "WINDOWS/FONTS"

static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static const u8 *sfnt_table(const u8 *d, u32 size, const char *tag, u32 *len) {
    if (size < 12) return NULL;
    u16 n = be16(d + 4);
    for (u32 i = 0; i < n && 12 + 16 * (i + 1) <= size; i++) {
        const u8 *e = d + 12 + 16 * i;
        u32 off = be32(e + 8), l = be32(e + 12);
        if (memcmp(e, tag, 4) == 0 && off + l <= size && off + l >= off) { if (len) *len = l; return d + off; }
    }
    return NULL;
}
/* name table entry: Windows Unicode, English (US) first, any Windows language next, Macintosh Roman last */
static int sfnt_name(const u8 *t, u32 len, u16 id, u16 *out, int max) {
    if (!t || len < 6) return 0;
    u16 count = be16(t + 2), strs = be16(t + 4);
    int best = -1, score = 0;
    for (u32 i = 0; i < count && 6 + 12 * (i + 1) <= len; i++) {
        const u8 *r = t + 6 + 12 * i;
        if (be16(r + 6) != id) continue;
        u16 plat = be16(r), lang = be16(r + 4);
        int s = plat == 3 && lang == 0x409 ? 3 : plat == 3 ? 2 : plat == 1 && lang == 0 ? 1 : 0;
        if (s > score) { score = s; best = (int)i; }
    }
    if (best < 0) return 0;
    const u8 *r = t + 6 + 12 * best;
    u32 l = be16(r + 8), off = strs + be16(r + 10u);
    if (off + l > len) return 0;
    int n = 0;
    if (be16(r) == 3) for (u32 k = 0; k + 1 < l && n < max - 1; k += 2) out[n++] = be16(t + off + k);
    else for (u32 k = 0; k < l && n < max - 1; k++) out[n++] = t[off + k];
    out[n] = 0;
    return n;
}
static void font_add(const char *file, const u8 *d, u32 size) {
    if (size < 12 || nfonts >= 64) return;
    u32 tag = be32(d);
    if (tag != 0x00010000 && tag != 0x4F54544F && tag != 0x74727565) return; /* TrueType, OpenType (CFF), 'true' */
    u32 nl, ol, pl;
    const u8 *name = sfnt_table(d, size, "name", &nl), *os2 = sfnt_table(d, size, "OS/2", &ol), *post = sfnt_table(d, size, "post", &pl);
    wfontinfo_t *f = &fonts[nfonts];
    memset(f, 0, sizeof *f);
    if (!sfnt_name(name, nl, 1, f->family, 64)) return;
    if (!sfnt_name(name, nl, 2, f->style, 32)) a2w("Regular", f->style, 32);
    if (!sfnt_name(name, nl, 4, f->full, 96)) memcpy(f->full, f->family, sizeof f->family);
    strncpy0(f->file, file, sizeof f->file);
    f->weight = 400;
    if (os2 && ol >= 64) {
        f->weight = be16(os2 + 4);
        f->italic = be16(os2 + 62) & 1;
        if (be16(os2 + 62) & 0x20) f->weight = f->weight < 700 ? 700 : f->weight; /* BOLD bit */
        for (int i = 0; i < 4; i++) f->usb[i] = be32(os2 + 42 + 4 * i);
        if (be16(os2) >= 1 && ol >= 86) { f->csb[0] = be32(os2 + 78); f->csb[1] = be32(os2 + 82); }
    }
    if (!f->usb[0] && !f->csb[0]) { f->usb[0] = 3; f->csb[0] = 1; } /* no coverage data: assume Latin */
    if (post && pl >= 16) f->fixed = be32(post + 12) != 0;
    nfonts++;
}
static void fonts_scan(void) {
    if (nfonts >= 0 && fonts) return;
    nfonts = 0;
    if (!fonts) fonts = kzalloc(64 * sizeof *fonts);
    fat_entry_t *list = kmalloc(128 * sizeof *list);
    if (!fonts || !list || !fat_mounted) { kfree(list); return; }
    int n = fat_list(FONT_DIR, list, 128);
    for (int i = 0; i < n; i++) {
        if (list[i].is_dir) continue;
        const char *ln = list[i].lname;
        u64 k = strlen(ln);
        if (k < 5) continue;
        const char *ext = ln + k - 4;
        if (c__stricmp(ext, ".ttf") && c__stricmp(ext, ".otf")) continue;
        char path[96];
        ksnprintf(path, sizeof path, FONT_DIR "/%s", ln);
        u8 *d; u32 sz;
        if (fat_read(path, &d, &sz) != 0) continue;
        font_add(ln, d, sz);
        kfree(d);
    }
    kfree(list);
    klog("exe: %d fuentes en C:\\WINDOWS\\FONTS\n", nfonts);
}

/* ---- EnumFontFamiliesExW ---- */
typedef int (WINAPI *FONTENUMPROCW)(const void *lf, const void *tm, u32 type, i64 lp);
static int wcsieq(const u16 *a, const u16 *b) { return c__wcsicmp(a, b) == 0; }
static int font_call(const wfontinfo_t *f, int charset, FONTENUMPROCW proc, i64 lp) {
    u8 elf[92 + 128 + 64 + 64], tm[100];
    memset(elf, 0, sizeof elf);
    memset(tm, 0, sizeof tm);
    int family = f->fixed ? 0x30 : 0x20; /* FF_MODERN or FF_SWISS */
    *(i32 *)elf = 16;
    *(i32 *)(elf + 16) = f->weight;
    elf[20] = (u8)f->italic;
    elf[23] = (u8)charset;
    elf[24] = 3; elf[25] = 2; elf[26] = 1;  /* OUT_STROKE_PRECIS, CLIP_STROKE_PRECIS, DRAFT_QUALITY */
    elf[27] = (u8)((f->fixed ? 1 : 2) | family);
    memcpy(elf + 28, f->family, 64);
    memcpy(elf + 92, f->full, 128);
    memcpy(elf + 220, f->style, 64);
    a2w("Western", (u16 *)(elf + 284), 32);
    i32 *t = (i32 *)tm;
    t[0] = 16; t[1] = 13; t[2] = 3; t[3] = 3; t[5] = 7; t[6] = 16; t[7] = f->weight; t[9] = 96; t[10] = 96;
    u16 *c = (u16 *)(tm + 44);
    c[0] = 32; c[1] = 0xFFFC; c[2] = 31; c[3] = 32;
    tm[52] = (u8)f->italic;
    tm[55] = (u8)((f->fixed ? 0 : 1) | 2 | 4 | family); /* TMPF_FIXED_PITCH means "not fixed" */
    tm[56] = (u8)charset;
    *(u32 *)(tm + 60) = (f->italic ? 1 : 0) | (f->weight >= 700 ? 0x20 : 0) | (!f->italic && f->weight < 700 ? 0x40 : 0); /* ntmFlags */
    *(u32 *)(tm + 64) = 2048;
    *(u32 *)(tm + 68) = 2400;
    *(u32 *)(tm + 72) = 1000;
    memcpy(tm + 76, f->usb, 16);
    memcpy(tm + 92, f->csb, 8);
    return proc(elf, tm, 4, lp); /* TRUETYPE_FONTTYPE */
}
static WINAPI int t_EnumFontFamiliesExW(void *dc, const u8 *lf, FONTENUMPROCW proc, i64 lp, u32 fl) {
    fonts_scan();
    const u16 *face = (const u16 *)(lf + 28);
    int charset = lf[23] == 1 ? 0 : lf[23], r = 1; /* DEFAULT_CHARSET: report ANSI */
    for (int i = 0; i < nfonts && r; i++) {
        if (face[0]) { if (wcsieq(face, fonts[i].family)) r = font_call(&fonts[i], charset, proc, lp); continue; }
        int seen = 0; /* one call per family */
        for (int j = 0; j < i; j++) if (wcsieq(fonts[j].family, fonts[i].family)) seen = 1;
        if (!seen) r = font_call(&fonts[i], charset, proc, lp);
    }
    return r;
}
static WINAPI int t_EnumFontFamiliesW(void *dc, const u16 *face, FONTENUMPROCW proc, i64 lp) {
    u8 lf[92] = {0};
    lf[23] = 1;
    if (face) for (int i = 0; i < 31 && face[i]; i++) ((u16 *)(lf + 28))[i] = face[i];
    return t_EnumFontFamiliesExW(dc, lf, proc, lp, 0);
}

/* ---- a read-only registry with one useful key ---- */
#define RKEY_MAGIC 0x52454759
typedef struct { u32 magic; int root; char path[128]; } rkey_t;
static const char *fonts_key = "SOFTWARE\\MICROSOFT\\WINDOWS NT\\CURRENTVERSION\\FONTS";
static void upcase_path(char *s) { for (; *s; s++) { if (*s >= 'a' && *s <= 'z') *s -= 32; if (*s == '/') *s = '\\'; } }
/* 1: the key exists (the fonts key or a folder on the way to it) */
static int reg_exists(int root, const char *path) {
    if (root != 2) return !path[0]; /* other roots exist but are empty */
    u64 k = strlen(path);
    return !path[0] || (strncmp(fonts_key, path, k) == 0 && (fonts_key[k] == 0 || fonts_key[k] == '\\'));
}
/* HKEY_CLASSES_ROOT... are 0x80000000 + n, sign-extended to 64 bits */
static int root_key(u64 h) { u32 lo = (u32)h; return (h >> 32 == 0 || h >> 32 == 0xFFFFFFFF) && lo >= 0x80000000u && lo <= 0x80000006u ? (int)(lo - 0x80000000u) : -1; }
static int reg_open(u64 parent, const u16 *sub, u64 *out) {
    rkey_t *p = (rkey_t *)parent, k = {RKEY_MAGIC, 0, ""};
    if (root_key(parent) >= 0) k.root = root_key(parent);
    else if (parent > 0x10000 && !(parent & 7) && p->magic == RKEY_MAGIC) k = *p;
    else return 6; /* ERROR_INVALID_HANDLE */
    char s[128] = "";
    if (sub) w_to_u8(sub, -1, s, sizeof s);
    upcase_path(s);
    char *q = s;
    while (*q == '\\') q++;
    if (*q) {
        u64 l = strlen(k.path);
        ksnprintf(k.path + l, sizeof k.path - l, "%s%s", l ? "\\" : "", q);
    }
    for (u64 l = strlen(k.path); l && k.path[l - 1] == '\\'; l--) k.path[l - 1] = 0;
    if (!reg_exists(k.root, k.path)) return 2;
    rkey_t *n = p_alloc(sizeof *n, 0);
    *n = k;
    *out = (u64)n;
    return 0;
}
static rkey_t *RK(u64 h) { rkey_t *k = (rkey_t *)h; return h > 0x10000 && !(h & 7) && k->magic == RKEY_MAGIC ? k : NULL; }
static int is_fonts_key(u64 h) { rkey_t *k = RK(h); return k && k->root == 2 && strcmp(k->path, fonts_key) == 0; }
/* value name of font i: "DejaVu Sans Bold (TrueType)" */
static int font_value_name(int i, u16 *out, int max) {
    u16 tmp[200];
    int n = 0;
    for (int j = 0; fonts[i].full[j] && n < 150; j++) tmp[n++] = fonts[i].full[j];
    const char *sfx = " (TrueType)";
    for (int j = 0; sfx[j]; j++) tmp[n++] = (u16)sfx[j];
    tmp[n] = 0;
    if (out) { if (n + 1 > max) return -1; memcpy(out, tmp, 2ULL * (u64)(n + 1)); }
    return n;
}
static WINAPI u32 t_RegOpenKeyExW(u64 h, const u16 *sub, u32 opt, u32 sam, u64 *out) { return (u32)reg_open(h, sub, out); }
static WINAPI u32 t_RegOpenKeyExA(u64 h, const char *sub, u32 opt, u32 sam, u64 *out) { u16 w[128]; a2w(sub ? sub : "", w, 128); return (u32)reg_open(h, w, out); }
static WINAPI u32 t_RegCreateKeyExW(u64 h, const u16 *sub, u32 r, void *cls, u32 opt, u32 sam, void *sa, u64 *out, u32 *disp) {
    u32 e = (u32)reg_open(h, sub, out);
    if (e == 0 && disp) *disp = 2; /* REG_OPENED_EXISTING_KEY */
    return e == 2 ? 5 : e;        /* new keys can't be made: access denied */
}
static WINAPI u32 t_RegCloseKey(u64 h) { rkey_t *k = RK(h); if (k) { k->magic = 0; p_free(k); } return 0; }
static WINAPI u32 t_RegQueryInfoKeyW(u64 h, u16 *cls, u32 *cl, u32 *res, u32 *nsub, u32 *maxsub, u32 *maxcls,
                                     u32 *nval, u32 *maxvn, u32 *maxv, u32 *sd, u64 *ft) {
    if (!RK(h) && root_key(h) < 0) return 6;
    int fk = is_fonts_key(h);
    if (fk) fonts_scan();
    u32 vn = 0, vl = 0;
    for (int i = 0; fk && i < nfonts; i++) {
        u32 a = (u32)font_value_name(i, NULL, 0), b = (u32)(strlen(fonts[i].file) + 1) * 2;
        if (a > vn) vn = a;
        if (b > vl) vl = b;
    }
    rkey_t *k = RK(h);
    int has_child = k && !fk && reg_exists(k->root, k->path) && k->root == 2;
    if (cl) *cl = 0;
    if (nsub) *nsub = has_child ? 1 : 0;
    if (maxsub) *maxsub = has_child ? 32 : 0;
    if (maxcls) *maxcls = 0;
    if (nval) *nval = fk ? (u32)nfonts : 0;
    if (maxvn) *maxvn = vn;
    if (maxv) *maxv = vl;
    if (sd) *sd = 0;
    if (ft) *ft = 0;
    return 0;
}
static WINAPI u32 t_RegEnumKeyExW(u64 h, u32 i, u16 *name, u32 *len, u32 *r, u16 *cls, u32 *cl, u64 *ft) {
    rkey_t *k = RK(h);
    if (!k || k->root != 2 || is_fonts_key(h) || i > 0) return 259; /* ERROR_NO_MORE_ITEMS */
    u64 l = strlen(k->path);
    if (l && strncmp(fonts_key, k->path, l) != 0) return 259;
    const char *rest = fonts_key + l + (l ? 1 : 0);
    char part[64];
    int n = 0;
    while (rest[n] && rest[n] != '\\' && n < 63) { part[n] = rest[n]; n++; }
    part[n] = 0;
    if ((u32)n + 1 > *len) return 234;
    a2w(part, name, *len);
    *len = (u32)n;
    return 0;
}
static u32 put_value(const wfontinfo_t *f, u32 *type, u8 *data, u32 *size) {
    u16 w[64];
    u32 n = a2w(f->file, w, 64), need = (n + 1) * 2;
    if (type) *type = 1; /* REG_SZ */
    if (!size) return data ? 87 : 0;
    if (data && *size < need) { *size = need; return 234; } /* ERROR_MORE_DATA */
    if (data) memcpy(data, w, need);
    *size = need;
    return 0;
}
static WINAPI u32 t_RegEnumValueW(u64 h, u32 i, u16 *name, u32 *len, u32 *r, u32 *type, u8 *data, u32 *size) {
    if (!is_fonts_key(h)) return 259;
    fonts_scan();
    if ((int)i >= nfonts) return 259;
    int n = font_value_name((int)i, name, (int)*len);
    if (n < 0) { *len = (u32)font_value_name((int)i, NULL, 0); return 234; }
    *len = (u32)n;
    return put_value(&fonts[i], type, data, size);
}
static WINAPI u32 t_RegQueryValueExW(u64 h, const u16 *vn, u32 *r, u32 *type, u8 *data, u32 *size) {
    if (!is_fonts_key(h) || !vn) return 2;
    fonts_scan();
    u16 tmp[200];
    for (int i = 0; i < nfonts; i++) {
        font_value_name(i, tmp, 200);
        if (wcsieq(tmp, vn)) return put_value(&fonts[i], type, data, size);
    }
    return 2;
}
static WINAPI u32 t_RegGetValueW(u64 h, const u16 *sub, const u16 *vn, u32 fl, u32 *type, u8 *data, u32 *size) {
    u64 k;
    if (sub && sub[0]) { u32 e = (u32)reg_open(h, sub, &k); if (e) return e; } else k = h;
    u32 e = t_RegQueryValueExW(k, vn, NULL, type, data, size);
    if (k != h) t_RegCloseKey(k);
    return e;
}

static const struct { const char *name; void *fn; } exports_font[] = {
    {"EnumFontFamiliesExW", t_EnumFontFamiliesExW}, {"EnumFontFamiliesW", t_EnumFontFamiliesW},
    {"RegOpenKeyExW", t_RegOpenKeyExW}, {"RegOpenKeyExA", t_RegOpenKeyExA}, {"RegCreateKeyExW", t_RegCreateKeyExW},
    {"RegCloseKey", t_RegCloseKey}, {"RegQueryInfoKeyW", t_RegQueryInfoKeyW}, {"RegEnumKeyExW", t_RegEnumKeyExW},
    {"RegEnumValueW", t_RegEnumValueW}, {"RegQueryValueExW", t_RegQueryValueExW}, {"RegGetValueW", t_RegGetValueW},
};
