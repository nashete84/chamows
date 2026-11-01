/* DLLs: loading the program's own DLLs from the pendrive (Qt, SDL, ...).
 *
 * Windows' own DLLs (kernel32, user32, ...) are not files here: their
 * functions are the Win32 layer in this folder, so imports from them are
 * linked to it. Any other DLL is looked for next to the program, then in the
 * current folder, then in C:\CHAMOWS\SYSTEM. It is placed in memory like the
 * .exe, its own imports are linked the same way (loading more DLLs), and its
 * DllMain runs before the program starts. Included by win32.c. */

typedef struct w32mod {
    char name[64];        /* "qt6core.dll" */
    char path[160];       /* on the pendrive, "TG/Qt6Core.dll" */
    u8 *raw, *image;
    u32 size;
    u64 entry;            /* DllMain, 0 = none */
    u64 tls_cb;           /* TLS callback list, 0 = none */
    int is_dll, refs, inited, datafile, no_thread_calls, loading;
    struct w32mod *next;
} w32mod_t;

/* Windows' own DLLs: always the Win32 layer, never a file. */
static const char *const sys_dlls[] = {
    "kernel32", "kernelbase", "ntdll", "user32", "gdi32", "msvcrt", "ucrtbase", "ws2_32", "wsock32", "advapi32",
    "shell32", "ole32", "oleaut32", "shlwapi", "winmm", "imm32", "comctl32", "comdlg32", "uxtheme", "dwmapi",
    "version", "setupapi", "crypt32", "bcrypt", "secur32", "iphlpapi", "dnsapi", "userenv", "winspool", "wtsapi32",
    "netapi32", "mpr", "opengl32", "shcore", "propsys", "powrprof", "hid", "dbghelp", "psapi", "normaliz", "rpcrt4",
    "msimg32", "usp10", "gdiplus", "winhttp", "wininet", "urlmon", "cfgmgr32", "dwrite", "d2d1", "d3d9", "d3d11",
    "d3d12", "dxgi", "d3dcompiler_47", "mf", "mfplat", "mfreadwrite", "avrt", "ksuser", "ncrypt", "wldap32",
    "authz", "sspicli", "msacm32", "combase", "windowscodecs", "wintrust", "credui", "dxva2", "evr", "mmdevapi",
    NULL};
static w32mod_t sys_mod = {"windows", "", 0, 0, 0, 0, 0, 1, 1000000, 1, 0, 0, 0, NULL}; /* handle for all of them */

/* "C:\Qt\Qt6Core.DLL" -> "qt6core.dll" */
static void dll_base(const char *in, char *out, int n) {
    const char *b = in;
    for (const char *q = in; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
    int i = 0;
    for (; b[i] && i < n - 5; i++) out[i] = (char)lower(b[i]);
    out[i] = 0;
    int dot = 0;
    for (int k = 0; k < i; k++) if (out[k] == '.') dot = 1;
    if (!dot) memcpy(out + i, ".dll", 5);
}
static int is_sys_dll(const char *base) {
    if (!memcmp(base, "api-ms-win-", 11) || !memcmp(base, "ext-ms-", 7)) return 1;
    for (int i = 0; sys_dlls[i]; i++) {
        int n = (int)strlen(sys_dlls[i]);
        if (!memcmp(base, sys_dlls[i], (size_t)n) && base[n] == '.') return 1;
    }
    return 0;
}

static void *builtin_lookup(const char *name);
static void *unknown_thunk(const char *name);
static w32mod_t *mod_load(w32proc_t *p, const char *name, int depth, int datafile, const char **err);
static void mod_unload(w32proc_t *p, w32mod_t *m);

/* ---- placing an image in memory ---- */
#define RD16(p) (*(const u16 *)(p))
#define RD32(p) (*(const u32 *)(p))
#define RD64(p) (*(const u64 *)(p))
static const u8 *pe_headers(const u8 *f, u32 size, const char **err) {
    if (size < 0x200 || f[0] != 'M' || f[1] != 'Z') { *err = "no es un programa de Windows"; return NULL; }
    u32 pe = RD32(f + 0x3C);
    if (pe + 0x108 > size || RD32(f + pe) != 0x4550) { *err = "el archivo está dañado"; return NULL; }
    const u8 *fh = f + pe + 4;
    if (RD16(fh) != 0x8664 || RD16(fh + 20) != 0x20B) { *err = "solo se pueden abrir programas de Windows de 64 bits"; return NULL; }
    return fh;
}
/* Copies the sections and fixes addresses. Returns the image, NULL on error. */
static u8 *pe_map(const u8 *f, u32 size, u8 **raw_out, u32 *size_out, const char **err) {
    const u8 *fh = pe_headers(f, size, err);
    if (!fh) return NULL;
    const u8 *oh = fh + 20;
    u16 nsec = RD16(fh + 2), ohsize = RD16(fh + 16);
    u32 img_size = RD32(oh + 56), hdr_size = RD32(oh + 60), ndirs = RD32(oh + 108);
    u64 pref_base = RD64(oh + 24);
    if (img_size > 1024u << 20 || hdr_size > size || hdr_size > img_size) { *err = "es demasiado grande"; return NULL; }
    u8 *raw = kzalloc(img_size + 4096);
    if (!raw) { *err = "no hay memoria suficiente"; return NULL; }
    u8 *img = (u8 *)(((uptr)raw + 4095) & ~(uptr)4095);
    memcpy(img, f, hdr_size);
    const u8 *sec = oh + ohsize;
    for (int i = 0; i < nsec; i++, sec += 40) {
        u32 va = RD32(sec + 12), len = RD32(sec + 16), ptr = RD32(sec + 20);
        if (len > RD32(sec + 8) && RD32(sec + 8)) len = RD32(sec + 8);
        if ((u64)va + len > img_size || (u64)ptr + len > size) { kfree(raw); *err = "una sección está fuera de su sitio"; return NULL; }
        memcpy(img + va, f + ptr, len);
    }
    const u8 *dir = oh + 112;
    i64 delta = (i64)((u64)img - pref_base);
    if (delta) {
        if (ndirs < 6 || !RD32(dir + 5 * 8)) { kfree(raw); *err = "no se puede mover en memoria (no tiene tabla de reubicación)"; return NULL; }
        u32 off = RD32(dir + 5 * 8), end = off + RD32(dir + 5 * 8 + 4);
        while (off + 8 <= end && end <= img_size) {
            u32 page = RD32(img + off), bsize = RD32(img + off + 4);
            if (bsize < 8) break;
            for (u32 i = 8; i + 2 <= bsize; i += 2) {
                u16 e = RD16(img + off + i);
                u32 at = page + (e & 0xFFF);
                if (at + 8 > img_size) continue;
                if ((e >> 12) == 10) *(u64 *)(img + at) += (u64)delta;
                else if ((e >> 12) == 3) *(u32 *)(img + at) += (u32)delta;
            }
            off += bsize;
        }
    }
    /* the in-memory header says where the image really is */
    *(u64 *)(img + (fh - f) + 20 + 24) = (u64)img;
    *raw_out = raw;
    *size_out = img_size;
    return img;
}
static const u8 *opt_header(const u8 *img) { return img + RD32(img + 0x3C) + 24; }
static u32 dir_rva(const u8 *img, int i, u32 *len) {
    const u8 *oh = opt_header(img);
    if (RD32(oh + 108) <= (u32)i) { if (len) *len = 0; return 0; }
    if (len) *len = RD32(oh + 112 + 8 * i + 4);
    return RD32(oh + 112 + 8 * i);
}

/* ---- exports ---- */
static void *mod_export(w32proc_t *p, w32mod_t *m, const char *name, u32 ord, u16 hint, int depth);
/* "QtCore.qFree" or "NTDLL.#12": a function that lives in another DLL */
static void *forwarded(w32proc_t *p, const char *fwd, int depth) {
    char dll[64], fn[96];
    int i = 0;
    for (; fwd[i] && fwd[i] != '.' && i < 59; i++) dll[i] = fwd[i];
    dll[i] = 0;
    if (fwd[i] != '.') return NULL;
    strncpy0(fn, fwd + i + 1, sizeof fn);
    char base[64];
    dll_base(dll, base, sizeof base);
    const char *err;
    w32mod_t *m = mod_load(p, base, depth + 1, 0, &err);
    if (!m) return NULL;
    if (fn[0] == '#') { u32 o = 0; for (const char *q = fn + 1; *q >= '0' && *q <= '9'; q++) o = o * 10 + (u32)(*q - '0'); return mod_export(p, m, NULL, o, 0, depth + 1); }
    return mod_export(p, m, fn, 0, 0, depth + 1);
}
/* A function of a module, by name (hint: where the name probably is) or by ordinal. */
static void *mod_export(w32proc_t *p, w32mod_t *m, const char *name, u32 ord, u16 hint, int depth) {
    if (m == &sys_mod) return name ? builtin_lookup(name) : NULL;
    if (depth > 16) return NULL;
    u32 elen, e = dir_rva(m->image, 0, &elen);
    if (!e || e + 40 > m->size) return NULL;
    const u8 *img = m->image, *ed = img + e;
    u32 base = RD32(ed + 16), nfn = RD32(ed + 20), nnames = RD32(ed + 24);
    const u32 *fns = (const u32 *)(img + RD32(ed + 28)), *names = (const u32 *)(img + RD32(ed + 32));
    const u16 *ords = (const u16 *)(img + RD32(ed + 36));
    i64 idx = -1;
    if (name) {
        if (hint < nnames && strcmp((const char *)img + names[hint], name) == 0) idx = ords[hint];
        else { /* names are sorted */
            u32 lo = 0, hi = nnames;
            while (lo < hi) {
                u32 mid = (lo + hi) / 2;
                int c = strcmp((const char *)img + names[mid], name);
                if (c == 0) { idx = ords[mid]; break; }
                if (c < 0) lo = mid + 1; else hi = mid;
            }
        }
    } else if (ord >= base) idx = ord - base;
    if (idx < 0 || idx >= nfn || !fns[idx]) return NULL;
    u32 rva = fns[idx];
    if (rva >= e && rva < e + elen) return forwarded(p, (const char *)img + rva, depth);
    return (void *)(img + rva);
}

/* ---- linking imports ---- */
static int link_imports(w32proc_t *p, w32mod_t *m, int depth, const char **err) {
    u8 *img = m->image;
    u32 d = dir_rva(img, 1, NULL);
    int unknown = 0;
    if (!d) return 0;
    for (; d + 20 <= m->size && RD32(img + d + 12); d += 20) {
        u32 lookup = RD32(img + d) ? RD32(img + d) : RD32(img + d + 16), iat = RD32(img + d + 16);
        const char *dll = (const char *)img + RD32(img + d + 12);
        char base[64];
        dll_base(dll, base, sizeof base);
        w32mod_t *dep = mod_load(p, base, depth + 1, 0, err);
        if (!dep && *err) return -1;      /* found but broken */
        if (!dep) dep = &sys_mod;         /* not found: try the Win32 layer */
        for (u32 k = 0;; k++) {
            u64 t = RD64(img + lookup + 8 * k);
            if (!t) break;
            void *fn;
            const char *name = NULL;
            if (t >> 63 && dep == &sys_mod && (name = ord_name(base, (u32)(t & 0xFFFF)))) fn = builtin_lookup(name);
            else if (t >> 63) fn = mod_export(p, dep, NULL, (u32)(t & 0xFFFF), 0, depth);
            else { name = (const char *)img + (u32)t + 2; fn = mod_export(p, dep, name, 0, RD16(img + (u32)t), depth); }
            if (!fn && dep != &sys_mod) {
                if (name) klog("exe: %s no tiene %s\n", base, name);
                else klog("exe: %s no tiene la función número %u\n", base, (u32)(t & 0xFFFF));
            }
            if (!fn) {
                char tmp[96];
                if (!name) { ksnprintf(tmp, sizeof tmp, "%s#%u", base, (u32)(t & 0xFFFF)); name = tmp; }
                if (unknown++ < 12) klog("exe: %s!%s no existe en Chamows (devolverá 0)\n", base, name);
                fn = unknown_thunk(name);
            }
            else if (dep == &sys_mod) fn = trace_thunk(name, fn);
            *(u64 *)(img + iat + 8 * k) = (u64)fn;
        }
    }
    if (unknown) klog("exe: %s usa %d funciones de Windows sin implementar\n", m->name, unknown);
    return 0;
}

/* ---- thread-local storage: one block per module that has a .tls section ---- */
static void tls_setup(w32proc_t *p, w32mod_t *m) {
    u32 t = dir_rva(m->image, 9, NULL);
    if (!t) return;
    const u8 *td = m->image + t;
    u64 start = RD64(td), end = RD64(td + 8), idx = RD64(td + 16);
    m->tls_cb = RD64(td + 24);
    if (p->ntls >= 16) { klog("exe: demasiados módulos con TLS\n"); return; }
    int slot = p->ntls++;
    u64 len = end > start ? end - start : 0;
    p->tlsm[slot].len = len + RD32(td + 32);
    p->tlsm[slot].tmpl = kzalloc(p->tlsm[slot].len + 16);
    if (len && p->tlsm[slot].tmpl) memcpy(p->tlsm[slot].tmpl, (void *)start, len);
    if (idx) *(u32 *)idx = (u32)slot;
    /* threads that already exist get their block too (DLL loaded later) */
    for (w32thread_t *th = p->threads; th; th = th->next) {
        th->tlsb[slot] = kzalloc(p->tlsm[slot].len + 16);
        if (th->tlsb[slot] && p->tlsm[slot].tmpl) memcpy(th->tlsb[slot], p->tlsm[slot].tmpl, p->tlsm[slot].len);
        *(u64 *)(th->teb + 0x800 + 8 * slot) = (u64)th->tlsb[slot];
    }
}

/* ---- finding and loading a DLL ---- */
static w32mod_t *mod_find(w32proc_t *p, const char *base) {
    for (w32mod_t *m = p->mods; m; m = m->next) if (strcmp(m->name, base) == 0) return m;
    return NULL;
}
static int dll_file(w32proc_t *p, const char *name, char *fp, int n) {
    fat_entry_t e;
    int has_dir = 0;
    for (const char *q = name; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') has_dir = 1;
    if (has_dir) return fix_path(name, fp, n) == 0 && fstat_path(fp, &e) == 0 && !e.is_dir ? 0 : -1;
    char base[64];
    dll_base(name, base, sizeof base);
    const char *dirs[4] = {p->exedir, p->dlldir, p->cwd, "CHAMOWS/SYSTEM"};
    for (int i = 0; i < 4; i++) {
        if (i == 1 && !p->dlldir[0]) continue;
        if (dirs[i][0]) ksnprintf(fp, (size_t)n, "%s/%s", dirs[i], base);
        else ksnprintf(fp, (size_t)n, "%s", base);
        if (fstat_path(fp, &e) == 0 && !e.is_dir) return 0;
    }
    return -1;
}
/* Returns the module (refs + 1), &sys_mod for Windows' own DLLs, or NULL:
 * not found (*err NULL) or found but unusable (*err says why). */
static w32mod_t *mod_load(w32proc_t *p, const char *name, int depth, int datafile, const char **err) {
    *err = NULL;
    char base[64], fp[200];
    dll_base(name, base, sizeof base);
    if (is_sys_dll(base)) return &sys_mod;
    w32mod_t *m = mod_find(p, base);
    if (m) { m->refs++; return m; }
    if (depth > 24) { *err = "demasiadas DLL encadenadas"; return NULL; }
    if (!fat_mounted || dll_file(p, name, fp, sizeof fp) < 0) return NULL;
    u8 *data;
    u32 size;
    if (fat_read(fp, &data, &size) < 0) { *err = "no se pudo leer"; return NULL; }
    m = kzalloc(sizeof *m);
    if (!m) { kfree(data); *err = "no hay memoria suficiente"; return NULL; }
    strncpy0(m->name, base, sizeof m->name);
    strncpy0(m->path, fp, sizeof m->path);
    m->image = pe_map(data, size, &m->raw, &m->size, err);
    kfree(data);
    if (!m->image) { klog("exe: %s: %s\n", fp, *err); kfree(m); return NULL; }
    const u8 *oh = opt_header(m->image);
    m->is_dll = (RD16(m->image + RD32(m->image + 0x3C) + 4 + 18) & 0x2000) != 0;
    m->refs = 1;
    m->datafile = datafile;
    m->loading = 1;
    /* on the list before its imports, so DLLs that import each other work */
    m->next = p->mods;
    p->mods = m;
    if (!datafile) {
        if (link_imports(p, m, depth, err) < 0) { klog("exe: %s: %s\n", m->name, *err); mod_unload(p, m); return NULL; }
        tls_setup(p, m);
        if (RD32(oh + 16)) m->entry = (u64)m->image + RD32(oh + 16);
        /* initialisation order: dependencies first */
        p->init_order[p->ninit < 64 ? p->ninit++ : 63] = m;
    }
    m->loading = 0;
    klog("exe: DLL %s cargada en 0x%x\n", fp, (u32)(uptr)m->image);
    return m;
}

/* DllMain and the TLS callbacks of one module */
static int mod_notify(w32mod_t *m, u32 reason, void *reserved) {
    if (m->datafile || !m->is_dll) return 1;
    if ((reason == 2 || reason == 3) && m->no_thread_calls) return 1;
    if (m->tls_cb)
        for (u64 *cb = (u64 *)m->tls_cb; *cb; cb++) ((void (WINAPI *)(void *, u32, void *))*cb)(m->image, reason, reserved);
    if (!m->entry) return 1;
    return ((int (WINAPI *)(void *, u32, void *))m->entry)(m->image, reason, reserved);
}
/* Runs DllMain(PROCESS_ATTACH) of every module that has not had it yet. */
static int mods_init(w32proc_t *p, void *reserved) {
    for (int i = 0; i < p->ninit; i++) {
        w32mod_t *m = p->init_order[i];
        if (!m || m->inited) continue;
        m->inited = 1;
        if (!mod_notify(m, 1, reserved)) { klog("exe: %s no pudo iniciarse (DllMain devolvió 0)\n", m->name); return -1; }
    }
    return 0;
}
static void mods_thread(w32proc_t *p, u32 reason) {
    for (int i = 0; i < p->ninit; i++) if (p->init_order[i] && p->init_order[i]->inited) mod_notify(p->init_order[i], reason, NULL);
}
/* ExitProcess: DllMain(PROCESS_DETACH), last loaded first */
static void mods_detach(w32proc_t *p) {
    if (p->detached) return;
    p->detached = 1;
    for (int i = p->ninit - 1; i >= 0; i--) if (p->init_order[i] && p->init_order[i]->inited) mod_notify(p->init_order[i], 0, (void *)1);
}
static void mod_unload(w32proc_t *p, w32mod_t *m) {
    for (w32mod_t **pp = &p->mods; *pp; pp = &(*pp)->next) if (*pp == m) { *pp = m->next; break; }
    for (int i = 0; i < p->ninit; i++) if (p->init_order[i] == m) p->init_order[i] = NULL;
    kfree(m->raw);
    kfree(m);
}
static void mods_free(w32proc_t *p) {
    while (p->mods) mod_unload(p, p->mods);
    for (int i = 0; i < p->ntls; i++) { kfree(p->tlsm[i].tmpl); p->tlsm[i].tmpl = NULL; }
    p->ntls = p->ninit = 0;
}
static w32mod_t *mod_of_handle(u64 h) {
    if (!h) return cur->exe;
    if (h == (u64)&sys_mod) return &sys_mod;
    for (w32mod_t *m = cur->mods; m; m = m->next) if ((u64)m->image == h) return m;
    return NULL;
}
static w32mod_t *mod_of_addr(u64 a) {
    for (w32mod_t *m = cur->mods; m; m = m->next) if (a >= (u64)m->image && a < (u64)m->image + m->size) return m;
    return NULL;
}
static u64 mod_handle(w32mod_t *m) { return m == &sys_mod ? (u64)&sys_mod : (u64)m->image; }

/* ---- the Win32 functions ---- */
static u64 load_library(const char *name, u32 flags) {
    if (!name || !*name) { last_error = 87; return 0; }
    const char *err;
    int datafile = (flags & 0x22) != 0; /* LOAD_LIBRARY_AS_DATAFILE / AS_IMAGE_RESOURCE */
    w32mod_t *m = mod_load(cur, name, 0, datafile, &err);
    if (!m) {
        last_error = err ? 193 : 126; /* ERROR_BAD_EXE_FORMAT, ERROR_MOD_NOT_FOUND */
        static int shown;
        if (shown++ < 24) klog("exe: LoadLibrary(%s): %s\n", name, err ? err : "no está");
        return 0;
    }
    if (m != &sys_mod && mods_init(cur, NULL) < 0) {
        if (!--m->refs) mod_unload(cur, m);
        last_error = 1114; /* ERROR_DLL_INIT_FAILED */
        return 0;
    }
    return mod_handle(m);
}
static WINAPI u64 d_LoadLibraryA(const char *n) { return load_library(n, 0); }
static WINAPI u64 d_LoadLibraryExA(const char *n, u64 f, u32 fl) { return load_library(n, fl); }
static WINAPI u64 d_LoadLibraryW(const u16 *n) { char a[200]; w2a(n, a, sizeof a); return load_library(a, 0); }
static WINAPI u64 d_LoadLibraryExW(const u16 *n, u64 f, u32 fl) { char a[200]; w2a(n, a, sizeof a); return load_library(a, fl); }
static WINAPI int d_FreeLibrary(u64 h) {
    w32mod_t *m = mod_of_handle(h);
    if (!m) { last_error = 6; return 0; }
    if (m == &sys_mod || m == cur->exe) return 1;
    if (--m->refs > 0) return 1;
    if (m->inited) mod_notify(m, 0, NULL);
    mod_unload(cur, m);
    return 1;
}
static WINAPI void *d_GetProcAddress(u64 h, const char *name) {
    w32mod_t *m = mod_of_handle(h);
    if (!m) { last_error = 6; return NULL; }
    void *fn = (u64)name < 0x10000 ? mod_export(cur, m, NULL, (u32)(u64)name, 0, 0) : mod_export(cur, m, name, 0, 0, 0);
    if (!fn) last_error = 127; /* ERROR_PROC_NOT_FOUND */
    return fn;
}
static u64 module_handle(const char *name) {
    if (!name) return (u64)cur->image;
    char base[64];
    dll_base(name, base, sizeof base);
    if (is_sys_dll(base)) return (u64)&sys_mod;
    w32mod_t *m = mod_find(cur, base);
    if (m) return (u64)m->image;
    last_error = 126;
    return 0;
}
static WINAPI u64 d_GetModuleHandleA(const char *n) { return module_handle(n); }
static WINAPI u64 d_GetModuleHandleW(const u16 *n) { if (!n) return module_handle(NULL); char a[200]; w2a(n, a, sizeof a); return module_handle(a); }
static WINAPI int d_GetModuleHandleExW(u32 fl, const void *n, u64 *out) {
    u64 h;
    if (fl & 4) { w32mod_t *m = mod_of_addr((u64)n); h = m ? (u64)m->image : 0; } /* FROM_ADDRESS */
    else if (!n) h = (u64)cur->image;
    else { char a[200]; w2a(n, a, sizeof a); h = module_handle(a); }
    *out = h;
    if (!h) { last_error = 126; return 0; }
    if (!(fl & 3)) { w32mod_t *m = mod_of_handle(h); if (m && m != &sys_mod) m->refs++; }
    return 1;
}
static WINAPI int d_GetModuleHandleExA(u32 fl, const char *n, u64 *out) {
    if ((fl & 4) || !n) return d_GetModuleHandleExW(fl, n, out);
    u16 w[200]; a2w(n, w, 200); return d_GetModuleHandleExW(fl, w, out);
}
static u32 module_file(u64 h, char *out, u32 n) {
    w32mod_t *m = mod_of_handle(h);
    if (!m || !n) { last_error = 126; return 0; }
    char tmp[200];
    if (m == &sys_mod) strncpy0(tmp, "C:\\CHAMOWS\\SYSTEM\\kernel32.dll", sizeof tmp);
    else win_path(m->path, tmp, sizeof tmp);
    strncpy0(out, tmp, n);
    return (u32)strlen(out);
}
static WINAPI u32 d_GetModuleFileNameA(u64 h, char *out, u32 n) { return module_file(h, out, n); }
static WINAPI u32 d_GetModuleFileNameW(u64 h, u16 *out, u32 n) {
    char a[200];
    if (!module_file(h, a, sizeof a) || !n) return 0;
    return a2w(a, out, n);
}
static WINAPI int d_DisableThreadLibraryCalls(u64 h) { w32mod_t *m = mod_of_handle(h); if (m && m != &sys_mod) m->no_thread_calls = 1; return 1; }
static WINAPI int d_SetDllDirectoryW(const u16 *n) {
    char a[200];
    cur->dlldir[0] = 0;
    if (n && *n) { w2a(n, a, sizeof a); fix_path(a, cur->dlldir, sizeof cur->dlldir); }
    return 1;
}
static WINAPI int d_SetDllDirectoryA(const char *n) { cur->dlldir[0] = 0; if (n && *n) fix_path(n, cur->dlldir, sizeof cur->dlldir); return 1; }
static WINAPI u64 d_AddDllDirectory(const u16 *n) { if (!cur->dlldir[0]) d_SetDllDirectoryW(n); return 1; }

/* ---- resources (icons, strings, version info inside .exe and .dll files) ---- */
static int res_match(const u8 *rd, u32 e, const u8 *ent, const void *id, int wide) {
    u32 nm = RD32(ent);
    if ((u64)id < 0x10000) return !(nm >> 31) && (nm & 0xFFFF) == (u32)(u64)id;
    if (!(nm >> 31)) return 0;
    const u8 *s = rd + (nm & 0x7FFFFFFF);
    u16 len = RD16(s);
    const u16 *w = (const u16 *)(s + 2);
    for (u16 i = 0; i < len; i++) {
        int c = wide ? ((const u16 *)id)[i] : ((const u8 *)id)[i];
        if (!c || lower(c) != lower(w[i])) return 0;
    }
    return wide ? !((const u16 *)id)[len] : !((const u8 *)id)[len];
}
/* one level of the resource tree: returns the offset of the child, or 0 */
static u32 res_level(const u8 *rd, u32 dir, const void *id, int wide, int any) {
    u16 nnamed = RD16(rd + dir + 12), nid = RD16(rd + dir + 14);
    const u8 *ent = rd + dir + 16;
    for (u32 i = 0; i < (u32)nnamed + nid; i++, ent += 8)
        if (any || res_match(rd, dir, ent, id, wide)) return RD32(ent + 4) & 0x7FFFFFFF;
    return 0;
}
static u64 find_resource(u64 h, const void *name, const void *type, int wide) {
    w32mod_t *m = mod_of_handle(h);
    if (!m || m == &sys_mod) { last_error = 1813; return 0; }
    u32 r = dir_rva(m->image, 2, NULL);
    if (!r) { last_error = 1812; return 0; }
    const u8 *rd = m->image + r;
    u32 t = res_level(rd, 0, type, wide, 0);
    u32 n = t ? res_level(rd, t, name, wide, 0) : 0;
    u32 l = n ? res_level(rd, n, NULL, 0, 1) : 0; /* first language */
    if (!l) { last_error = 1814; return 0; }
    return (u64)(rd + l); /* data entry: rva, size */
}
static WINAPI u64 d_FindResourceW(u64 h, const u16 *name, const u16 *type) { return find_resource(h, name, type, 1); }
static WINAPI u64 d_FindResourceA(u64 h, const char *name, const char *type) { return find_resource(h, name, type, 0); }
static WINAPI u64 d_FindResourceExW(u64 h, const u16 *type, const u16 *name, u16 lang) { return find_resource(h, name, type, 1); }
static WINAPI void *d_LoadResource(u64 h, u64 r) {
    w32mod_t *m = mod_of_handle(h);
    return m && m != &sys_mod && r ? m->image + RD32((const u8 *)r) : NULL;
}
static WINAPI void *d_LockResource(void *d) { return d; }
static WINAPI u32 d_SizeofResource(u64 h, u64 r) { return r ? RD32((const u8 *)r + 4) : 0; }
static WINAPI int d_LoadStringW(u64 h, u32 id, u16 *out, int n) {
    u64 r = find_resource(h, (const void *)(u64)(id / 16 + 1), (const void *)6, 1);
    const u16 *s = r ? d_LoadResource(h, r) : NULL;
    if (!s) { if (out && n) out[0] = 0; return 0; }
    for (u32 i = 0; i < id % 16; i++) s += 1 + *s;
    int len = *s;
    if (n == 0) { *(const u16 **)out = s + 1; return len; } /* read-only pointer */
    if (len > n - 1) len = n - 1;
    memcpy(out, s + 1, (size_t)len * 2);
    out[len] = 0;
    return len;
}
static WINAPI int d_LoadStringA(u64 h, u32 id, char *out, int n) {
    u16 w[256];
    int len = d_LoadStringW(h, id, w, 256);
    if (n <= 0) return 0;
    if (len > n - 1) len = n - 1;
    for (int i = 0; i < len; i++) out[i] = w[i] < 256 ? (char)w[i] : '?';
    out[len] = 0;
    return len;
}

static const struct { const char *name; void *fn; } exports_dll[] = {
    {"LoadLibraryA", d_LoadLibraryA}, {"LoadLibraryW", d_LoadLibraryW}, {"LoadLibraryExA", d_LoadLibraryExA},
    {"LoadLibraryExW", d_LoadLibraryExW}, {"FreeLibrary", d_FreeLibrary}, {"GetProcAddress", d_GetProcAddress},
    {"GetModuleHandleA", d_GetModuleHandleA}, {"GetModuleHandleW", d_GetModuleHandleW},
    {"GetModuleHandleExA", d_GetModuleHandleExA}, {"GetModuleHandleExW", d_GetModuleHandleExW},
    {"GetModuleFileNameA", d_GetModuleFileNameA}, {"GetModuleFileNameW", d_GetModuleFileNameW},
    {"DisableThreadLibraryCalls", d_DisableThreadLibraryCalls}, {"SetDllDirectoryW", d_SetDllDirectoryW},
    {"SetDllDirectoryA", d_SetDllDirectoryA}, {"AddDllDirectory", d_AddDllDirectory}, {"RemoveDllDirectory", k_one},
    {"SetDefaultDllDirectories", k_one}, {"FindResourceW", d_FindResourceW}, {"FindResourceA", d_FindResourceA},
    {"FindResourceExW", d_FindResourceExW}, {"LoadResource", d_LoadResource}, {"LockResource", d_LockResource},
    {"SizeofResource", d_SizeofResource}, {"FreeResource", k_zero}, {"LoadStringW", d_LoadStringW},
    {"LoadStringA", d_LoadStringA},
};
