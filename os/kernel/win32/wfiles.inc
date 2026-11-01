/* Files for Windows programs: KERNEL32 (CreateFile, FindFirstFile...) and
 * msvcrt (fopen, _open, _stat...) on top of the FAT32 pendrive.
 *
 * C:\ and D:\ are both the pendrive root. A file is read whole into memory
 * when opened and written back when closed (or flushed), which keeps the
 * FAT driver simple; long names can be read, new files need 8.3 names.
 * Included from win32.c (64-bit only). */
#include "../fat.h"

#define FMAGIC 0x46494C45u /* "FILE" */
#define DMAGIC 0x46494E44u /* "FIND" */
#define INVALID_H ((u64)-1)

typedef struct {
    u32 magic;
    char path[160];
    u8 *data;
    u64 size, cap, pos;
    int can_read, can_write, append, dirty, eof, err, ungot;
    void *next; /* all open files, so a second handle sees unsaved writes */
    int del_on_close;
} wfile_t;
static wfile_t *wf_open_list;
static int wf_flush(wfile_t *f);
/* save what other handles wrote to this file, before it is opened or looked at again */
static void wf_sync_path(const char *fp) {
    for (wfile_t *f = wf_open_list; f; f = f->next) if (f->dirty && !stricmp_(f->path, fp)) wf_flush(f);
}

typedef struct {
    u32 magic;
    fat_entry_t *list;
    int n, i;
    char pat[64];
} wfind_t;

static wfile_t *WF(u64 v) { wfile_t *f = (wfile_t *)v; return v > 0x10000 && !(v & 7) && f->magic == FMAGIC ? f : NULL; }
static wfind_t *WD(u64 v) { wfind_t *d = (wfind_t *)v; return v > 0x10000 && !(v & 7) && d->magic == DMAGIC ? d : NULL; }

/* ---- paths ---- */
static void w2a(const u16 *w, char *a, int n) {
    int i = 0;
    for (; w && w[i] && i < n - 1; i++) a[i] = w[i] < 256 ? (char)w[i] : '_';
    a[i] = 0;
}
static u32 a2w(const char *a, u16 *w, u32 n) {
    u32 i = 0;
    for (; a[i] && i + 1 < n; i++) w[i] = (u8)a[i];
    if (n) w[i] = 0;
    return i;
}

/* "C:\Juegos\..\SKY\sky.dsk" -> "SKY/sky.dsk" (relative to the pendrive root). */
static int fix_path(const char *in, char *out, int n) {
    char tmp[320];
    const char *p = in;
    if (p[0] == '\\' && p[1] == '\\' && p[2] == '?' && p[3] == '\\') p += 4;
    int abs = 0;
    if (((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z') && p[1] == ':') { p += 2; abs = 1; }
    if (*p == '\\' || *p == '/') abs = 1;
    if (abs || !cur || !cur->cwd[0]) ksnprintf(tmp, sizeof tmp, "%s", p);
    else ksnprintf(tmp, sizeof tmp, "%s/%s", cur->cwd, p);
    /* split into parts, resolving "." and ".." */
    int len = 0;
    out[0] = 0;
    for (char *q = tmp; *q;) {
        while (*q == '/' || *q == '\\') q++;
        if (!*q) break;
        char *e = q;
        while (*e && *e != '/' && *e != '\\') e++;
        int pl = (int)(e - q);
        if (pl == 1 && q[0] == '.') { q = e; continue; }
        if (pl == 2 && q[0] == '.' && q[1] == '.') {
            while (len > 0 && out[len - 1] != '/') len--;
            if (len > 0) len--;
            out[len] = 0;
            q = e;
            continue;
        }
        if (len + pl + 2 >= n) return -1;
        if (len) out[len++] = '/';
        memcpy(out + len, q, (size_t)pl);
        len += pl;
        out[len] = 0;
        q = e;
    }
    return 0;
}
/* "SKY/sky.dsk" -> "C:\SKY\sky.dsk" */
static void win_path(const char *fp, char *out, int n) {
    int k = ksnprintf(out, (size_t)n, "C:\\%s", fp);
    for (int i = 3; i < k && out[i]; i++) if (out[i] == '/') out[i] = '\\';
}

static int fstat_path(const char *fp, fat_entry_t *e) { wf_sync_path(fp); return fat_mounted ? fat_stat(fp, e) : -1; }

/* ---- open files ---- */
static int wf_flush(wfile_t *f) {
    if (!f->dirty) return 0;
    f->dirty = 0;
    if (fat_write(f->path, f->data ? f->data : (const u8 *)"", (u32)f->size) != 0) { klog("exe: no se pudo guardar %s\n", f->path); return -1; }
    return 0;
}
static int wf_reserve(wfile_t *f, u64 need) {
    if (need <= f->cap) return 0;
    u64 cap = f->cap ? f->cap : 4096;
    while (cap < need) cap *= 2;
    u8 *d = kmalloc(cap);
    if (!d) return -1;
    if (f->data) { memcpy(d, f->data, f->size); kfree(f->data); }
    f->data = d;
    f->cap = cap;
    return 0;
}
static void wf_close(wfile_t *f) {
    if (f->del_on_close) { f->dirty = 0; if (fat_mounted) fat_delete(f->path); }
    else wf_flush(f);
    for (wfile_t **pp = &wf_open_list; *pp; pp = (wfile_t **)&(*pp)->next) if (*pp == f) { *pp = f->next; break; }
    f->magic = 0;
    if (f->data) kfree(f->data);
    kfree(f);
}
/* how: 'r' must exist; 'w' create/truncate; 'a' create/append; 'c' create if missing; 'o' open, create if missing */
static wfile_t *wf_open(const char *fp, char how, int rd, int wr) {
    fat_entry_t e;
    int exists = fstat_path(fp, &e) == 0;
    if (exists && e.is_dir) { last_error = 5; return NULL; }
    if (!exists && how == 'r') { last_error = 2; return NULL; }
    if (!fat_mounted) { last_error = 3; return NULL; }
    wfile_t *f = kzalloc(sizeof *f);
    if (!f) { last_error = 8; return NULL; }
    f->magic = FMAGIC;
    strncpy0(f->path, fp, sizeof f->path);
    f->can_read = rd;
    f->can_write = wr;
    f->ungot = -1;
    if (exists && how != 'w') {
        u8 *d;
        u32 sz;
        if (fat_read(fp, &d, &sz) != 0) { kfree(f); last_error = 5; return NULL; }
        f->data = d;
        f->size = f->cap = sz;
    }
    if (!exists || how == 'w') {
        /* create now, so a name that can't exist (not 8.3) fails here */
        if (fat_write(fp, "", 0) != 0) { kfree(f); last_error = 5; return NULL; }
    }
    if (how == 'a') { f->append = 1; f->pos = f->size; }
    f->next = wf_open_list;
    wf_open_list = f;
    return f;
}
static u64 wf_read(wfile_t *f, void *buf, u64 n) {
    if (!f->can_read) { f->err = 1; return 0; }
    u8 *b = buf;
    u64 got = 0;
    if (n && f->ungot >= 0) { b[got++] = (u8)f->ungot; f->ungot = -1; }
    u64 left = f->pos < f->size ? f->size - f->pos : 0;
    u64 k = n - got < left ? n - got : left;
    if (k) memcpy(b + got, f->data + f->pos, k);
    f->pos += k;
    got += k;
    if (got < n) f->eof = 1;
    return got;
}
static u64 wf_write(wfile_t *f, const void *buf, u64 n) {
    if (!f->can_write) { f->err = 1; return 0; }
    if (f->append) f->pos = f->size;
    if (wf_reserve(f, f->pos + n) != 0) { f->err = 1; return 0; }
    if (f->pos > f->size) memset(f->data + f->size, 0, f->pos - f->size);
    memcpy(f->data + f->pos, buf, n);
    f->pos += n;
    if (f->pos > f->size) f->size = f->pos;
    f->dirty = 1;
    return n;
}
static i64 wf_seek(wfile_t *f, i64 off, int whence) {
    i64 base = whence == 1 ? (i64)f->pos : whence == 2 ? (i64)f->size : 0;
    if (base + off < 0) return -1;
    f->pos = (u64)(base + off);
    f->eof = 0;
    f->ungot = -1;
    return (i64)f->pos;
}

/* ---- KERNEL32 ---- */
static u64 create_file(const char *name, u32 access, u32 disp) {
    char fp[160];
    if (fix_path(name, fp, sizeof fp) != 0) { last_error = 3; return INVALID_H; }
    if (!strcmp(name, "CONOUT$") || !strcmp(name, "CON")) return 0x11;
    if (!strcmp(name, "CONIN$")) return 0x10;
    int drive_long = name[0] == '\\' && name[1] == '\\' && name[2] == '?' && name[3] == '\\' && name[4] && name[5] == ':'; /* \\?\C:\... */
    if ((name[0] == '\\' && name[1] == '\\' && (name[2] == '.' || name[2] == '?') && !drive_long) || !c_strncmp(name, "//./", 4)) {
        last_error = 2; /* pipes and devices: there are none */
        return INVALID_H;
    }
    fat_entry_t e;
    int exists = fstat_path(fp, &e) == 0;
    /* FILE_FLAG_BACKUP_SEMANTICS opens folders: hand back a harmless file handle */
    if (exists && e.is_dir) {
        wfile_t *f = kzalloc(sizeof *f);
        f->magic = FMAGIC;
        strncpy0(f->path, fp, sizeof f->path);
        f->ungot = -1;
        return (u64)f;
    }
    char how;
    switch (disp) {
    case 1: if (exists) { last_error = 80; return INVALID_H; } how = 'w'; break; /* CREATE_NEW */
    case 2: how = 'w'; break;                                                    /* CREATE_ALWAYS */
    case 3: how = 'r'; break;                                                    /* OPEN_EXISTING */
    case 4: how = 'o'; break;                                                    /* OPEN_ALWAYS */
    case 5: if (!exists) { last_error = 2; return INVALID_H; } how = 'w'; break; /* TRUNCATE_EXISTING */
    default: how = 'r';
    }
    int wr = (access & 0x40000000u) || (access & 0x10000000u) || (access & 2);
    int rd = (access & 0x80000000u) || (access & 0x10000000u) || (access & 1) || !wr;
    wfile_t *f = wf_open(fp, how, rd, wr);
    if (wr && !f) klog("exe: no se pudo crear \"%s\"\n", name);
    if (!f) return INVALID_H;
    last_error = exists && (disp == 2 || disp == 4) ? 183 : 0;
    return (u64)f;
}
static WINAPI u64 f_CreateFileW(const u16 *n, u32 acc, u32 share, void *sa, u32 disp, u32 fl, u64 tmpl) {
    char a[320]; w2a(n, a, sizeof a);
    u64 r = create_file(a, acc, disp);
    wfile_t *f = WF(r);
    if (f && (fl & 0x04000000)) f->del_on_close = 1; /* FILE_FLAG_DELETE_ON_CLOSE */
    return r;
}
static WINAPI u64 f_CreateFileA(const char *n, u32 acc, u32 share, void *sa, u32 disp, u32 fl, u64 tmpl) { return create_file(n, acc, disp); }

static WINAPI int f_ReadFile(u64 h, void *buf, u32 n, u32 *got, void *ov) {
    wfile_t *f = WF(h);
    if (got) *got = 0;
    if (!f) { last_error = 6; return 0; }
    u64 k = wf_read(f, buf, n);
    if (got) *got = (u32)k;
    return 1;
}
static WINAPI int f_WriteFile(u64 h, const void *buf, u32 n, u32 *put, void *ov) {
    wfile_t *f = WF(h);
    if (!f) return k_WriteFile(h, buf, n, put, ov); /* console */
    u64 k = wf_write(f, buf, n);
    if (put) *put = (u32)k;
    return k == n;
}
static WINAPI u32 f_SetFilePointer(u64 h, i32 lo, i32 *hi, u32 how) {
    wfile_t *f = WF(h);
    if (!f) { last_error = 6; return 0xFFFFFFFF; }
    i64 off = hi ? ((i64)*hi << 32) | (u32)lo : lo;
    i64 r = wf_seek(f, off, (int)how);
    if (r < 0) { last_error = 131; return 0xFFFFFFFF; }
    if (hi) *hi = (i32)(r >> 32);
    return (u32)r;
}
static WINAPI int f_SetFilePointerEx(u64 h, i64 off, i64 *out, u32 how) {
    wfile_t *f = WF(h);
    if (!f) { last_error = 6; return 0; }
    i64 r = wf_seek(f, off, (int)how);
    if (r < 0) { last_error = 131; return 0; }
    if (out) *out = r;
    return 1;
}
static WINAPI u32 f_GetFileSize(u64 h, u32 *hi) { wfile_t *f = WF(h); if (hi) *hi = 0; return f ? (u32)f->size : 0xFFFFFFFF; }
static WINAPI int f_GetFileSizeEx(u64 h, i64 *out) { wfile_t *f = WF(h); if (!f) return 0; *out = (i64)f->size; return 1; }
static WINAPI int f_FlushFileBuffers(u64 h) { wfile_t *f = WF(h); return f ? wf_flush(f) == 0 : 1; }
static WINAPI int f_SetEndOfFile(u64 h) {
    wfile_t *f = WF(h);
    if (!f) return 0;
    if (f->pos > f->size && wf_reserve(f, f->pos) == 0) memset(f->data + f->size, 0, f->pos - f->size);
    f->size = f->pos;
    f->dirty = 1;
    return 1;
}
static WINAPI u32 f_GetFileType(u64 h) { return WF(h) ? 1 : k_GetFileType(h); }
static WINAPI int f_FindClose(u64 h);
static WINAPI int f_CloseHandle(u64 h) {
    wfile_t *f = WF(h);
    if (f) { wf_close(f); return 1; }
    if (WD(h)) return f_FindClose(h);
    return 1; /* events, threads... are kept; the process frees them at the end */
}

static u32 attrs_of(const fat_entry_t *e) { return e->is_dir ? 0x10 : 0x20; }
static u32 file_attrs(const char *name) {
    char fp[160];
    fat_entry_t e;
    if (fix_path(name, fp, sizeof fp) != 0 || fstat_path(fp, &e) != 0) { last_error = 2; return 0xFFFFFFFF; }
    return attrs_of(&e);
}
/* a file's last write time (FAT keeps it to 2 seconds); "now" when there is none */
static u64 entry_ft(const fat_entry_t *e) {
    u64 ft;
    if (!e->wdate) { k_GetSystemTimeAsFileTime(&ft); return ft; }
    return civil_ft(1980 + (e->wdate >> 9), (e->wdate >> 5) & 15, e->wdate & 31, e->wtime >> 11, (e->wtime >> 5) & 63, (e->wtime & 31) * 2);
}
static WINAPI u32 f_GetFileAttributesW(const u16 *n) { char a[320]; w2a(n, a, sizeof a); return file_attrs(a); }
static WINAPI u32 f_GetFileAttributesA(const char *n) { return file_attrs(n); }
static WINAPI int f_GetFileAttributesExW(const u16 *n, u32 level, u32 *out) {
    char a[320], fp[160];
    fat_entry_t e;
    w2a(n, a, sizeof a);
    if (fix_path(a, fp, sizeof fp) != 0 || fstat_path(fp, &e) != 0) { last_error = 2; return 0; }
    memset(out, 0, 36);
    out[0] = attrs_of(&e);
    u64 ft = entry_ft(&e);
    for (int i = 0; i < 3; i++) memcpy(out + 1 + 2 * i, &ft, 8);
    out[8] = e.size;
    return 1;
}
static WINAPI int f_GetFileInformationByHandle(u64 h, u32 *out) {
    wfile_t *f = WF(h);
    if (!f) return 0;
    memset(out, 0, 52);
    out[0] = 0x20;
    u64 ft;
    fat_entry_t e;
    if (f->dirty || fstat_path(f->path, &e) != 0) k_GetSystemTimeAsFileTime(&ft); else ft = entry_ft(&e);
    for (int i = 0; i < 3; i++) memcpy(out + 1 + 2 * i, &ft, 8);
    out[7] = 0xC4A3; /* volume serial */
    out[9] = (u32)f->size;
    out[10] = 1;
    return 1;
}

static WINAPI int f_GetFileTime(u64 h, u64 *c, u64 *a, u64 *w) {
    u32 info[13];
    if (!f_GetFileInformationByHandle(h, info)) return 0;
    if (c) memcpy(c, info + 1, 8);
    if (a) memcpy(a, info + 3, 8);
    if (w) memcpy(w, info + 5, 8);
    return 1;
}

/* SetFileInformationByHandle: delete or rename through an open handle (Qt removes files this way) */
static int move_file(const char *from, const char *to, int replace);
static WINAPI int f_SetFileInformationByHandle(u64 h, u32 cls, const u8 *info, u32 size) {
    wfile_t *f = WF(h);
    if (!f || !info) { last_error = 6; return 0; }
    switch (cls) {
    case 0: case 5: return 1;                                            /* FileBasicInfo, FileAllocationInfo */
    case 4: f->del_on_close = info[0] != 0; return 1;                    /* FileDispositionInfo */
    case 21: f->del_on_close = (*(const u32 *)info & 1) != 0; return 1;  /* FileDispositionInfoEx */
    case 6: {                                                            /* FileEndOfFileInfo */
        u64 n = *(const u64 *)info;
        if (n > f->size && wf_reserve(f, n) == 0) memset(f->data + f->size, 0, n - f->size);
        if (n <= f->cap || !f->data) f->size = n;
        f->dirty = 1;
        return 1;
    }
    case 3: case 22: {                                                   /* FileRenameInfo(Ex) */
        u32 len = *(const u32 *)(info + 16) / 2;
        int replace = cls == 3 ? info[0] != 0 : (*(const u32 *)info & 1) != 0;
        u16 w[300];
        if (len >= 300) { last_error = 206; return 0; }
        memcpy(w, info + 20, 2 * (u64)len);
        w[len] = 0;
        char a[320], to[160];
        w2a(w, a, sizeof a);
        if (fix_path(a, to, sizeof to) != 0) { last_error = 3; return 0; }
        f->dirty = 1;
        char fa[170], ta[170];
        ksnprintf(fa, sizeof fa, "/%s", f->path);
        ksnprintf(ta, sizeof ta, "/%s", to);
        if (wf_flush(f) != 0 || !move_file(fa, ta, replace)) return 0;
        strncpy0(f->path, to, sizeof f->path);
        return 1;
    }
    }
    last_error = 50;
    return 0;
}

/* wildcards: '*' and '?', any case */
static int wild(const char *p, const char *s) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return 1;
            for (; *s; s++) if (wild(p + 1, s)) return 1;
            return wild(p + 1, s);
        }
        if (!*s) return 0;
        char a = *p, b = *s;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != '?' && a != b) return 0;
    }
    return !*s;
}
static int find_next(wfind_t *d, u8 *out, int wide) {
    while (d->i < d->n) {
        fat_entry_t *e = &d->list[d->i++];
        const char *nm = e->lname[0] ? e->lname : e->name;
        if (!wild(d->pat, nm) && !wild(d->pat, e->name) && strcmp(d->pat, "*.*") != 0) continue;
        memset(out, 0, wide ? 592 : 320);
        *(u32 *)out = attrs_of(e);
        u64 ft = entry_ft(e);
        for (int i = 0; i < 3; i++) memcpy(out + 4 + 8 * i, &ft, 8);
        *(u32 *)(out + 32) = e->size;
        if (wide) { a2w(nm, (u16 *)(out + 44), 260); a2w(e->name, (u16 *)(out + 44 + 520), 14); }
        else { strncpy0((char *)out + 44, nm, 260); strncpy0((char *)out + 44 + 260, e->name, 14); }
        return 1;
    }
    last_error = 18; /* no more files */
    return 0;
}
static u64 find_first(const char *name, u8 *out, int wide) {
    char fp[160];
    if (fix_path(name, fp, sizeof fp) != 0 || !fat_mounted) { last_error = 3; return INVALID_H; }
    char *slash = NULL;
    for (char *q = fp; *q; q++) if (*q == '/') slash = q;
    wfind_t *d = kzalloc(sizeof *d);
    d->magic = DMAGIC;
    strncpy0(d->pat, slash ? slash + 1 : fp, sizeof d->pat);
    if (slash) *slash = 0; else fp[0] = 0;
    d->list = kmalloc(sizeof(fat_entry_t) * 512);
    d->n = d->list ? fat_list(fp, d->list, 512) : -1;
    if (d->n > 512) d->n = 512;
    if (d->n < 0 || !find_next(d, out, wide)) {
        if (d->list) kfree(d->list);
        kfree(d);
        last_error = 2;
        return INVALID_H;
    }
    return (u64)d;
}
static WINAPI u64 f_FindFirstFileW(const u16 *n, u8 *out) { char a[320]; w2a(n, a, sizeof a); return find_first(a, out, 1); }
static WINAPI u64 f_FindFirstFileA(const char *n, u8 *out) { return find_first(n, out, 0); }
static WINAPI u64 f_FindFirstFileExW(const u16 *n, u32 lvl, u8 *out, u32 op, void *f, u32 fl) { return f_FindFirstFileW(n, out); }
static WINAPI int f_FindNextFileW(u64 h, u8 *out) { wfind_t *d = WD(h); return d ? find_next(d, out, 1) : 0; }
static WINAPI int f_FindNextFileA(u64 h, u8 *out) { wfind_t *d = WD(h); return d ? find_next(d, out, 0) : 0; }
static WINAPI int f_FindClose(u64 h) {
    wfind_t *d = WD(h);
    if (!d) return 0;
    d->magic = 0;
    kfree(d->list);
    kfree(d);
    return 1;
}

static int make_dir(const char *name) {
    char fp[160];
    if (fix_path(name, fp, sizeof fp) != 0 || !fat_mounted) { last_error = 3; return 0; }
    fat_entry_t e;
    if (fstat_path(fp, &e) == 0) { last_error = 183; return 0; }
    if (fat_mkdir(fp) != 0) { last_error = 5; return 0; }
    return 1;
}
static WINAPI int f_CreateDirectoryW(const u16 *n, void *sa) { char a[320]; w2a(n, a, sizeof a); return make_dir(a); }
static WINAPI int f_CreateDirectoryA(const char *n, void *sa) { return make_dir(n); }
static int del_file(const char *name) {
    char fp[160];
    if (fix_path(name, fp, sizeof fp) != 0 || fat_delete(fp) != 0) { last_error = 2; return 0; }
    return 1;
}
static WINAPI int f_DeleteFileW(const u16 *n) { char a[320]; w2a(n, a, sizeof a); return del_file(a); }
static WINAPI int f_DeleteFileA(const char *n) { return del_file(n); }
/* rename = copy + delete (the FAT driver has no rename yet) */
static int move_file(const char *from, const char *to, int replace) {
    char a[160], b[160];
    u8 *d;
    u32 sz;
    fat_entry_t e;
    if (fix_path(from, a, sizeof a) != 0 || fix_path(to, b, sizeof b) != 0) { last_error = 3; return 0; }
    if (!replace && fstat_path(b, &e) == 0) { last_error = 183; return 0; }
    if (fat_read(a, &d, &sz) != 0) { last_error = 2; return 0; }
    int ok = fat_write(b, d, sz) == 0 && fat_delete(a) == 0;
    kfree(d);
    if (!ok) last_error = 5;
    return ok;
}
static WINAPI int f_MoveFileW(const u16 *x, const u16 *y) { char a[320], b[320]; w2a(x, a, sizeof a); w2a(y, b, sizeof b); return move_file(a, b, 0); }
static WINAPI int f_MoveFileExW(const u16 *x, const u16 *y, u32 fl) { char a[320], b[320]; w2a(x, a, sizeof a); w2a(y, b, sizeof b); return move_file(a, b, fl & 1); }
static WINAPI int f_MoveFileExA(const char *x, const char *y, u32 fl) { return move_file(x, y, fl & 1); }

static WINAPI u32 f_GetCurrentDirectoryW(u32 n, u16 *out) {
    char a[200];
    win_path(cur->cwd, a, sizeof a);
    u32 len = (u32)strlen(a);
    if (!out || n <= len) return len + 1;
    return a2w(a, out, n);
}
static WINAPI u32 f_GetCurrentDirectoryA(u32 n, char *out) {
    char a[200];
    win_path(cur->cwd, a, sizeof a);
    u32 len = (u32)strlen(a);
    if (!out || n <= len) return len + 1;
    strncpy0(out, a, n);
    return len;
}
static int set_cwd(const char *name) {
    char fp[160];
    fat_entry_t e;
    if (fix_path(name, fp, sizeof fp) != 0 || fstat_path(fp, &e) != 0 || !e.is_dir) { last_error = 3; return 0; }
    strncpy0(cur->cwd, fp, sizeof cur->cwd);
    return 1;
}
static WINAPI int f_SetCurrentDirectoryW(const u16 *n) { char a[320]; w2a(n, a, sizeof a); return set_cwd(a); }
static WINAPI int f_SetCurrentDirectoryA(const char *n) { return set_cwd(n); }
static WINAPI u32 f_GetFullPathNameW(const u16 *n, u32 len, u16 *out, u16 **file) {
    char a[320], fp[160], w[200];
    w2a(n, a, sizeof a);
    if (fix_path(a, fp, sizeof fp) != 0) return 0;
    win_path(fp, w, sizeof w);
    u32 l = (u32)strlen(w);
    if (!out || len <= l) return l + 1;
    a2w(w, out, len);
    if (file) { u32 k = l; while (k > 0 && out[k - 1] != '\\') k--; *file = out + k; }
    return l;
}
static WINAPI u32 f_GetFullPathNameA(const char *n, u32 len, char *out, char **file) {
    char fp[160], w[200];
    if (fix_path(n, fp, sizeof fp) != 0) return 0;
    win_path(fp, w, sizeof w);
    u32 l = (u32)strlen(w);
    if (!out || len <= l) return l + 1;
    strncpy0(out, w, len);
    if (file) { u32 k = l; while (k > 0 && out[k - 1] != '\\') k--; *file = out + k; }
    return l;
}
static WINAPI u32 f_GetDriveTypeW(const u16 *root) { return 3; } /* fixed disk */
static WINAPI u32 f_GetLogicalDriveStringsW(u32 n, u16 *out) {
    static const u16 s[] = {'C', ':', '\\', 0, 0};
    if (n < 5) return 5;
    memcpy(out, s, sizeof s);
    return 4;
}
static WINAPI u32 f_GetLogicalDrives(void) { return 1u << 2; }
/* Chamows' own folder on the pendrive stands in for C:\Windows */
static WINAPI u32 f_GetWindowsDirectoryW(u16 *out, u32 n) { if (fat_mounted) fat_mkdir("CHAMOWS"); return a2w("C:\\CHAMOWS", out, n); }
static WINAPI u32 f_GetTempPathW(u32 n, u16 *out) { if (fat_mounted) fat_mkdir("TEMP"); return a2w("C:\\TEMP\\", out, n); }
static WINAPI u32 f_GetTempPathA(u32 n, char *out) { if (fat_mounted) fat_mkdir("TEMP"); strncpy0(out, "C:\\TEMP\\", n); return 8; }
static WINAPI u32 f_GetModuleFileNameW(void *m, u16 *out, u32 n) {
    char fp[200], a[240];
    if (cur->cwd[0]) ksnprintf(fp, sizeof fp, "%s/%s", cur->cwd, cur->name); else strncpy0(fp, cur->name, sizeof fp);
    win_path(fp, a, sizeof a);
    return a2w(a, out, n);
}
static WINAPI u32 f_GetModuleFileNameA(void *m, char *out, u32 n) {
    char fp[200], a[240];
    if (cur->cwd[0]) ksnprintf(fp, sizeof fp, "%s/%s", cur->cwd, cur->name); else strncpy0(fp, cur->name, sizeof fp);
    win_path(fp, a, sizeof a);
    strncpy0(out, a, n);
    return (u32)strlen(out);
}
/* SHGetFolderPathW: every special folder is C:\CHAMOWS\APPDATA */
static WINAPI u32 f_SHGetFolderPathW(void *hwnd, int csidl, u64 tok, u32 fl, u16 *out) {
    if (fat_mounted) { fat_mkdir("CHAMOWS"); fat_mkdir("CHAMOWS/APPDATA"); }
    a2w("C:\\CHAMOWS\\APPDATA", out, 260);
    return 0;
}

/* ---- msvcrt: FILE* and file descriptors ---- */
#define MAXFD 64
static wfile_t *fds[MAXFD];
static wfile_t *fopen_mode(const char *name, const char *mode) {
    char fp[160];
    if (fix_path(name, fp, sizeof fp) != 0) { errno_ = 2; return NULL; }
    int plus = c_strchr(mode, '+') != NULL;
    char how = mode[0] == 'w' ? 'w' : mode[0] == 'a' ? 'a' : 'r';
    wfile_t *f = wf_open(fp, how, how == 'r' || plus, how != 'r' || plus);
    if (how != 'r' && !f) klog("exe: no se pudo crear \"%s\"\n", name);
    if (!f) errno_ = last_error == 2 ? 2 : 13;
    return f;
}
static WINAPI void *c_fopen(const char *n, const char *m) { return fopen_mode(n, m); }
static WINAPI void *c__wfopen(const u16 *n, const u16 *m) { char a[320], b[16]; w2a(n, a, sizeof a); w2a(m, b, sizeof b); return fopen_mode(a, b); }
static WINAPI int c_fclose(void *p) { wfile_t *f = WF((u64)p); if (!f) return 0; wf_close(f); return 0; }
static WINAPI u64 c_fread(void *b, u64 sz, u64 n, void *p) {
    wfile_t *f = WF((u64)p);
    if (!f || !sz) return 0;
    return wf_read(f, b, sz * n) / sz;
}
static WINAPI u64 cf_fwrite(const void *b, u64 sz, u64 n, void *p) {
    wfile_t *f = WF((u64)p);
    if (!f) return c_fwrite(b, sz, n, p);
    return sz ? wf_write(f, b, sz * n) / sz : 0;
}
static WINAPI int cf_fputs(const char *s, void *p) { wfile_t *f = WF((u64)p); if (!f) return c_fputs(s, p); wf_write(f, s, strlen(s)); return 0; }
static WINAPI int cf_fputc(int c, void *p) { wfile_t *f = WF((u64)p); if (!f) return c_fputc(c, p); u8 ch = (u8)c; wf_write(f, &ch, 1); return c & 255; }
static WINAPI int c_fgetc(void *p) { wfile_t *f = WF((u64)p); u8 ch; return f && wf_read(f, &ch, 1) == 1 ? ch : -1; }
static WINAPI int c_ungetc(int c, void *p) { wfile_t *f = WF((u64)p); if (!f || c < 0) return -1; f->ungot = c & 255; f->eof = 0; return c; }
static WINAPI char *c_fgets(char *s, int n, void *p) {
    wfile_t *f = WF((u64)p);
    if (!f || n <= 0) return NULL;
    int k = 0;
    while (k < n - 1) {
        u8 ch;
        if (wf_read(f, &ch, 1) != 1) break;
        s[k++] = (char)ch;
        if (ch == '\n') break;
    }
    s[k] = 0;
    return k ? s : NULL;
}
static WINAPI int c_fseek(void *p, i32 off, int wh) { wfile_t *f = WF((u64)p); return f && wf_seek(f, off, wh) >= 0 ? 0 : -1; }
static WINAPI int c__fseeki64(void *p, i64 off, int wh) { wfile_t *f = WF((u64)p); return f && wf_seek(f, off, wh) >= 0 ? 0 : -1; }
static WINAPI i32 c_ftell(void *p) { wfile_t *f = WF((u64)p); return f ? (i32)f->pos : -1; }
static WINAPI i64 c__ftelli64(void *p) { wfile_t *f = WF((u64)p); return f ? (i64)f->pos : -1; }
/* MinGW's fseeko64/ftello64 go through these */
static WINAPI int c_fgetpos(void *p, i64 *pos) { wfile_t *f = WF((u64)p); if (!f) return -1; *pos = (i64)f->pos; return 0; }
static WINAPI int c_fsetpos(void *p, const i64 *pos) { wfile_t *f = WF((u64)p); return f && wf_seek(f, *pos, 0) >= 0 ? 0 : -1; }
static WINAPI int c_feof(void *p) { wfile_t *f = WF((u64)p); return f ? f->eof : 0; }
static WINAPI int c_ferror(void *p) { wfile_t *f = WF((u64)p); return f ? f->err : 0; }
static WINAPI void c_clearerr(void *p) { wfile_t *f = WF((u64)p); if (f) f->eof = f->err = 0; }
static WINAPI void c_rewind(void *p) { wfile_t *f = WF((u64)p); if (f) wf_seek(f, 0, 0); }
static WINAPI int cf_fflush(void *p) { wfile_t *f = WF((u64)p); return f ? wf_flush(f) : 0; }
static WINAPI int cf_vfprintf(void *p, const char *fmt, __builtin_ms_va_list ap) {
    wfile_t *f = WF((u64)p);
    if (!f) return c_vfprintf(p, fmt, ap);
    char buf[1024];
    int n = vfmt(buf, sizeof buf, fmt, ap);
    wf_write(f, buf, (u64)imin(n, (int)sizeof buf - 1));
    return n;
}
static WINAPI int cf_fprintf(void *p, const char *fmt, ...) {
    __builtin_ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int n = cf_vfprintf(p, fmt, ap);
    __builtin_ms_va_end(ap);
    return n;
}
static WINAPI int cf__fileno(void *p) {
    wfile_t *f = WF((u64)p);
    if (!f) return c__fileno(p);
    for (int i = 3; i < MAXFD; i++) if (fds[i] == f) return i;
    for (int i = 3; i < MAXFD; i++) if (!fds[i]) { fds[i] = f; return i; }
    return -1;
}

/* _open flags: 1 write-only, 2 read-write, 8 append, 0x100 create, 0x200 truncate, 0x400 excl */
static int open_fd(const char *name, int fl) {
    char fp[160];
    if (fix_path(name, fp, sizeof fp) != 0) { errno_ = 2; return -1; }
    fat_entry_t e;
    int exists = fstat_path(fp, &e) == 0;
    if (exists && (fl & 0x400) && (fl & 0x100)) { errno_ = 17; return -1; }
    char how = (fl & 0x200) ? 'w' : (fl & 0x100) ? (exists ? 'o' : 'w') : 'r';
    wfile_t *f = wf_open(fp, how, (fl & 3) != 1, (fl & 3) != 0);
    if (!f) { errno_ = last_error == 2 ? 2 : 13; return -1; }
    if (fl & 8) f->append = 1;
    for (int i = 3; i < MAXFD; i++) if (!fds[i]) { fds[i] = f; return i; }
    wf_close(f);
    errno_ = 24;
    return -1;
}
static WINAPI int c__open(const char *n, int fl, int mode) { return open_fd(n, fl); }
static WINAPI int c__wopen(const u16 *n, int fl, int mode) { char a[320]; w2a(n, a, sizeof a); return open_fd(a, fl); }
static wfile_t *FD(int fd) { return fd >= 3 && fd < MAXFD ? fds[fd] : NULL; }
static WINAPI int c__read(int fd, void *b, u32 n) { wfile_t *f = FD(fd); return f ? (int)wf_read(f, b, n) : -1; }
static WINAPI int cf__write(int fd, const void *b, u32 n) { wfile_t *f = FD(fd); if (!f) return c__write(fd, b, n); return (int)wf_write(f, b, n); }
static WINAPI int c__close(int fd) { wfile_t *f = FD(fd); if (!f) return fd < 3 ? 0 : -1; fds[fd] = NULL; wf_close(f); return 0; }
static WINAPI i32 c__lseek(int fd, i32 off, int wh) { wfile_t *f = FD(fd); return f ? (i32)wf_seek(f, off, wh) : -1; }
static WINAPI i64 c__lseeki64(int fd, i64 off, int wh) { wfile_t *f = FD(fd); return f ? wf_seek(f, off, wh) : -1; }
static WINAPI i64 c__filelengthi64(int fd) { wfile_t *f = FD(fd); return f ? (i64)f->size : -1; }
static WINAPI u64 c__get_osfhandle(int fd) { wfile_t *f = FD(fd); return f ? (u64)f : fd >= 0 && fd < 3 ? 0x10 + (u64)fd : INVALID_H; }

/* struct _stat64: mode at 6, size at 24, times at 32/40/48 */
static void fill_stat(u8 *st, int dir, u64 size) {
    memset(st, 0, 56);
    *(u16 *)(st + 6) = dir ? 0x41FF : 0x81B6;
    *(u16 *)(st + 8) = 1;
    *(u32 *)(st + 16) = 2;
    *(i64 *)(st + 24) = (i64)size;
    i64 t = 1767225600; /* 2026-01-01 */
    *(i64 *)(st + 32) = *(i64 *)(st + 40) = *(i64 *)(st + 48) = t;
}
static int stat_path(const char *name, u8 *st) {
    char fp[160];
    fat_entry_t e;
    if (fix_path(name, fp, sizeof fp) != 0 || fstat_path(fp, &e) != 0) { errno_ = 2; return -1; }
    fill_stat(st, e.is_dir, e.size);
    return 0;
}
static WINAPI int c__stat64(const char *n, u8 *st) { return stat_path(n, st); }
static WINAPI int c__wstat64(const u16 *n, u8 *st) { char a[320]; w2a(n, a, sizeof a); return stat_path(a, st); }
static WINAPI int c__fstat64(int fd, u8 *st) {
    wfile_t *f = FD(fd);
    if (!f) { fill_stat(st, 0, 0); *(u16 *)(st + 6) = 0x2000; return fd >= 0 && fd < 3 ? 0 : -1; }
    fill_stat(st, 0, f->size);
    return 0;
}
static WINAPI int c_remove(const char *n) { return del_file(n) ? 0 : -1; }
static WINAPI int c__wremove(const u16 *n) { char a[320]; w2a(n, a, sizeof a); return del_file(a) ? 0 : -1; }
static WINAPI int c_rename(const char *a, const char *b) { return move_file(a, b, 0) ? 0 : -1; }
static WINAPI int c__wrename(const u16 *x, const u16 *y) { char a[320], b[320]; w2a(x, a, sizeof a); w2a(y, b, sizeof b); return move_file(a, b, 0) ? 0 : -1; }
static WINAPI int c__access(const char *n, int mode) { return file_attrs(n) == 0xFFFFFFFF ? (errno_ = 2, -1) : 0; }
static WINAPI int c__waccess(const u16 *n, int mode) { char a[320]; w2a(n, a, sizeof a); return c__access(a, mode); }
static WINAPI int c__mkdir(const char *n) { return make_dir(n) ? 0 : -1; }
static WINAPI int c__wmkdir(const u16 *n) { char a[320]; w2a(n, a, sizeof a); return make_dir(a) ? 0 : -1; }
static WINAPI int c__chdir(const char *n) { return set_cwd(n) ? 0 : -1; }
static WINAPI char *c__getcwd(char *b, int n) {
    if (!b) { n = 200; b = p_alloc((u64)n, 1); }
    f_GetCurrentDirectoryA((u32)n, b);
    return b;
}
static WINAPI u16 *c__wgetcwd(u16 *b, int n) {
    if (!b) { n = 200; b = p_alloc((u64)n * 2, 1); }
    f_GetCurrentDirectoryW((u32)n, b);
    return b;
}
static WINAPI char *cf__fullpath(char *out, const char *p, u64 n) {
    if (!out) { out = p_alloc(260, 1); n = 260; }
    return f_GetFullPathNameA(p, (u32)n, out, NULL) ? out : NULL;
}
static WINAPI u16 *c__wfullpath(u16 *out, const u16 *p, u64 n) {
    if (!out) { out = p_alloc(520, 1); n = 260; }
    return f_GetFullPathNameW(p, (u32)n, out, NULL) ? out : NULL;
}

/* ---- text conversion: UTF-8 (code page 65001) or Latin-1 for the rest ---- */
static WINAPI int f_MultiByteToWideChar(u32 cp, u32 fl, const char *src, int n, u16 *dst, int max) {
    if (cp == 0 || cp == 1 || cp == 3) cp = 65001; /* the system code page is UTF-8 (GetACP) */
    if (!src) { last_error = 87; return 0; }
    if (n < 0) n = (int)strlen(src) + 1;
    int k = 0;
    for (int i = 0; i < n;) {
        u32 c = (u8)src[i++];
        if (cp == 65001 && c >= 0xC0) {
            int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            c &= 0x3F >> more;
            for (; more && i < n; more--) c = c << 6 | ((u8)src[i++] & 0x3F);
            if (c > 0xFFFF) c = '?';
        }
        if (max) { if (k >= max) { last_error = 122; return 0; } dst[k] = (u16)c; }
        k++;
    }
    return k;
}
static WINAPI int f_WideCharToMultiByte(u32 cp, u32 fl, const u16 *src, int n, char *dst, int max, const char *def, int *used) {
    if (cp == 0 || cp == 1 || cp == 3) cp = 65001;
    if (!src) { last_error = 87; return 0; }
    if (n < 0) { n = 0; while (src[n]) n++; n++; }
    int k = 0;
    if (used) *used = 0;
    for (int i = 0; i < n; i++) {
        u32 c = src[i];
        char b[3];
        int len = 1;
        if (cp == 65001 && c >= 0x80) {
            if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 63)); len = 2; }
            else { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | ((c >> 6) & 63)); b[2] = (char)(0x80 | (c & 63)); len = 3; }
        } else if (c > 0xFF) { b[0] = def ? *def : '?'; if (used) *used = 1; }
        else b[0] = (char)c;
        if (max) { if (k + len > max) { last_error = 122; return 0; } memcpy(dst + k, b, (size_t)len); }
        k += len;
    }
    return k;
}

/* the program's own arguments, as wide strings in one block */
static WINAPI u16 **f_CommandLineToArgvW(const u16 *cmd, int *argc) {
    int n = cur->argc;
    u64 len = (u64)(n + 1) * 8;
    for (int i = 0; i < n; i++) len += (strlen(cur->argv[i]) + 1) * 2;
    u16 **v = p_alloc(len, 1);
    u16 *str = (u16 *)(v + n + 1);
    for (int i = 0; i < n; i++) { v[i] = str; str += a2w(cur->argv[i], str, 4096) + 1; }
    if (argc) *argc = n;
    return v;
}

/* Checked before the other tables, so these replace the old stubs. */
static const struct { const char *name; void *fn; } exports_files[] = {
    {"CreateFileW", f_CreateFileW}, {"CreateFileA", f_CreateFileA}, {"ReadFile", f_ReadFile}, {"WriteFile", f_WriteFile},
    {"SetFilePointer", f_SetFilePointer}, {"SetFilePointerEx", f_SetFilePointerEx}, {"GetFileSize", f_GetFileSize},
    {"GetFileSizeEx", f_GetFileSizeEx}, {"FlushFileBuffers", f_FlushFileBuffers}, {"SetEndOfFile", f_SetEndOfFile},
    {"GetFileType", f_GetFileType}, {"CloseHandle", f_CloseHandle}, {"GetFileAttributesW", f_GetFileAttributesW},
    {"GetFileAttributesA", f_GetFileAttributesA}, {"GetFileAttributesExW", f_GetFileAttributesExW},
    {"GetFileInformationByHandle", f_GetFileInformationByHandle}, {"GetFileTime", f_GetFileTime}, {"SetFileInformationByHandle", f_SetFileInformationByHandle}, {"FindFirstFileW", f_FindFirstFileW},
    {"FindFirstFileA", f_FindFirstFileA}, {"FindFirstFileExW", f_FindFirstFileExW}, {"FindNextFileW", f_FindNextFileW},
    {"FindNextFileA", f_FindNextFileA}, {"FindClose", f_FindClose}, {"CreateDirectoryW", f_CreateDirectoryW},
    {"CreateDirectoryA", f_CreateDirectoryA}, {"DeleteFileW", f_DeleteFileW}, {"DeleteFileA", f_DeleteFileA},
    {"MoveFileW", f_MoveFileW}, {"MoveFileExW", f_MoveFileExW}, {"MoveFileExA", f_MoveFileExA},
    {"GetCurrentDirectoryW", f_GetCurrentDirectoryW}, {"GetCurrentDirectoryA", f_GetCurrentDirectoryA},
    {"SetCurrentDirectoryW", f_SetCurrentDirectoryW}, {"SetCurrentDirectoryA", f_SetCurrentDirectoryA},
    {"GetFullPathNameW", f_GetFullPathNameW}, {"GetFullPathNameA", f_GetFullPathNameA}, {"GetDriveTypeW", f_GetDriveTypeW},
    {"GetLogicalDriveStringsW", f_GetLogicalDriveStringsW}, {"GetLogicalDrives", f_GetLogicalDrives},
    {"GetWindowsDirectoryW", f_GetWindowsDirectoryW}, {"GetTempPathW", f_GetTempPathW}, {"GetTempPathA", f_GetTempPathA},
    {"GetModuleFileNameW", f_GetModuleFileNameW}, {"GetModuleFileNameA", f_GetModuleFileNameA},
    {"SHGetFolderPathW", f_SHGetFolderPathW}, {"MultiByteToWideChar", f_MultiByteToWideChar},
    {"WideCharToMultiByte", f_WideCharToMultiByte}, {"CommandLineToArgvW", f_CommandLineToArgvW},
    {"fopen", c_fopen}, {"_wfopen", c__wfopen}, {"fclose", c_fclose}, {"fread", c_fread}, {"fwrite", cf_fwrite},
    {"fputs", cf_fputs}, {"fputc", cf_fputc}, {"putc", cf_fputc}, {"fgetc", c_fgetc}, {"getc", c_fgetc}, {"ungetc", c_ungetc},
    {"fgets", c_fgets}, {"fseek", c_fseek}, {"_fseeki64", c__fseeki64}, {"ftell", c_ftell}, {"_ftelli64", c__ftelli64},
    {"fgetpos", c_fgetpos}, {"fsetpos", c_fsetpos}, {"feof", c_feof}, {"ferror", c_ferror}, {"clearerr", c_clearerr}, {"rewind", c_rewind}, {"fflush", cf_fflush},
    {"fprintf", cf_fprintf}, {"vfprintf", cf_vfprintf}, {"_fileno", cf__fileno}, {"_open", c__open}, {"_wopen", c__wopen},
    {"_read", c__read}, {"_write", cf__write}, {"_close", c__close}, {"_lseek", c__lseek}, {"_lseeki64", c__lseeki64},
    {"_filelengthi64", c__filelengthi64}, {"_get_osfhandle", c__get_osfhandle}, {"_stat64", c__stat64},
    {"_wstat64", c__wstat64}, {"_fstat64", c__fstat64}, {"remove", c_remove}, {"_unlink", c_remove},
    {"_wremove", c__wremove}, {"_wunlink", c__wremove}, {"rename", c_rename}, {"_wrename", c__wrename},
    {"_access", c__access}, {"_waccess", c__waccess}, {"_mkdir", c__mkdir}, {"_wmkdir", c__wmkdir}, {"_chdir", c__chdir},
    {"_getcwd", c__getcwd}, {"_wgetcwd", c__wgetcwd}, {"_fullpath", cf__fullpath}, {"_wfullpath", c__wfullpath},
};
