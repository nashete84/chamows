/* Chamows FAT32 driver: enough to keep files on a pendrive.
 * Supports a whole-disk FAT32 or the first FAT32 partition of an MBR disk,
 * 512-byte sectors, 8.3 names, subfolders, create/replace/delete. */
#include "fat.h"
#include "usb/usb.h"

typedef struct {
    char name[11];
    u8 attr, ntres, ctime_tenth;
    u16 ctime, cdate, adate, clus_hi, wtime, wdate, clus_lo;
    u32 size;
} __attribute__((packed)) dirent_t;

#define ATTR_DIR 0x10
#define ATTR_LFN 0x0F
#define ATTR_VOLUME 0x08
#define EOC 0x0FFFFFF8u

int fat_mounted;
char fat_label[12];
static u32 part_lba, fat_lba, fat_sectors, n_fats, data_lba, spc, root_cluster, n_clusters;
static u32 fsinfo_lba, free_count, next_free;
static u8 *clbuf;                    /* one cluster */
static u32 clbytes;

/* ---- FAT table with a one-sector cache ---- */
static u8 fat_cache[512];
static u32 fat_cache_lba = 0xFFFFFFFF;
static int fat_dirty;

static int fat_flush(void) {
    if (!fat_dirty) return 0;
    for (u32 i = 0; i < n_fats; i++)
        if (disk_write(fat_cache_lba + i * fat_sectors, 1, fat_cache) != 0) return -1;
    fat_dirty = 0;
    return 0;
}
static u32 *fat_slot(u32 c) {
    u32 lba = fat_lba + c / 128;
    if (lba != fat_cache_lba) {
        if (fat_flush() != 0) return NULL;
        if (disk_read(lba, 1, fat_cache) != 0) { fat_cache_lba = 0xFFFFFFFF; return NULL; }
        fat_cache_lba = lba;
    }
    return (u32 *)fat_cache + c % 128;
}
static u32 fat_get(u32 c) {
    u32 *p = fat_slot(c);
    return p ? (*p & 0x0FFFFFFF) : EOC;
}
static int fat_set(u32 c, u32 v) {
    u32 *p = fat_slot(c);
    if (!p) return -1;
    *p = (*p & 0xF0000000) | (v & 0x0FFFFFFF);
    fat_dirty = 1;
    return 0;
}

static void fsinfo_save(void) {
    if (!fsinfo_lba) return;
    u8 s[512];
    if (disk_read(fsinfo_lba, 1, s) != 0) return;
    if (memcmp(s, "RRaA", 4) != 0) return;
    memcpy(s + 488, &free_count, 4);
    memcpy(s + 492, &next_free, 4);
    disk_write(fsinfo_lba, 1, s);
}

static u32 alloc_cluster(void) {
    u32 start = (next_free >= 2 && next_free < n_clusters + 2) ? next_free : 2;
    for (u32 i = 0; i < n_clusters; i++) {
        u32 c = start + i;
        if (c >= n_clusters + 2) c -= n_clusters;
        if (fat_get(c) == 0) {
            if (fat_set(c, 0x0FFFFFFF) != 0) return 0;
            if (free_count != 0xFFFFFFFF) free_count--;
            next_free = c + 1;
            return c;
        }
    }
    return 0; /* disk full */
}
static void free_chain(u32 c) {
    while (c >= 2 && c < EOC) {
        u32 next = fat_get(c);
        fat_set(c, 0);
        if (free_count != 0xFFFFFFFF) free_count++;
        c = next;
    }
}

static u32 cl_lba(u32 c) { return data_lba + (c - 2) * spc; }
static int read_cluster(u32 c) { return disk_read(cl_lba(c), spc, clbuf); }
static int write_cluster(u32 c) { return disk_write(cl_lba(c), spc, clbuf); }

/* ---- mounting ---- */
static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

int fat_mount(void) {
    fat_mounted = 0;
    if (!usb_disk.present) return -1;
    u8 s[512];
    if (disk_read(0, 1, s) != 0 || s[510] != 0x55 || s[511] != 0xAA) return -1;
    part_lba = 0;
    if (memcmp(s + 82, "FAT32", 5) != 0) { /* not a boot sector: look at the partition table */
        for (int i = 0; i < 4; i++) {
            const u8 *e = s + 446 + i * 16;
            if (e[4] == 0x0B || e[4] == 0x0C) { part_lba = le32(e + 8); break; }
        }
        if (!part_lba || disk_read(part_lba, 1, s) != 0) { klog("fat: no hay ninguna partición FAT32\n"); return -1; }
        if (memcmp(s + 82, "FAT32", 5) != 0) { klog("fat: la partición no es FAT32\n"); return -1; }
    }
    if (le16(s + 11) != 512) { klog("fat: solo se admiten sectores de 512 bytes\n"); return -1; }
    spc = s[13];
    u32 reserved = le16(s + 14);
    n_fats = s[16];
    u32 total = le32(s + 32);
    fat_sectors = le32(s + 36);
    root_cluster = le32(s + 44);
    fsinfo_lba = le16(s + 48) ? part_lba + le16(s + 48) : 0;
    if (!spc || !n_fats || !fat_sectors) return -1;
    fat_lba = part_lba + reserved;
    data_lba = fat_lba + n_fats * fat_sectors;
    n_clusters = (total - reserved - n_fats * fat_sectors) / spc;
    clbytes = spc * 512;
    if (!clbuf) clbuf = kmalloc(clbytes > 4096 ? clbytes : 4096);
    if (!clbuf) return -1;
    memcpy(fat_label, s + 71, 11);
    fat_label[11] = 0;
    for (int i = 10; i >= 0 && fat_label[i] == ' '; i--) fat_label[i] = 0;
    free_count = next_free = 0xFFFFFFFF;
    if (fsinfo_lba && disk_read(fsinfo_lba, 1, s) == 0 && memcmp(s, "RRaA", 4) == 0) {
        free_count = le32(s + 488);
        next_free = le32(s + 492);
        if (free_count > n_clusters) free_count = 0xFFFFFFFF;
    }
    fat_cache_lba = 0xFFFFFFFF;
    fat_mounted = 1;
    klog("fat: pendrive \"%s\" montado, %u MB libres\n", fat_label, fat_free_mb());
    return 0;
}

u32 fat_free_mb(void) {
    if (!fat_mounted) return 0;
    if (free_count == 0xFFFFFFFF) { /* count once */
        u32 n = 0;
        for (u32 c = 2; c < n_clusters + 2; c++) if (fat_get(c) == 0) n++;
        free_count = n;
    }
    return free_count / (1048576 / clbytes); /* clusters are at most 64 KB */
}

/* ---- names ---- */
static int to83(const char *s, int len, char out[11]) {
    memset(out, ' ', 11);
    int dot = -1;
    for (int i = 0; i < len; i++) if (s[i] == '.') dot = i;
    int base = dot < 0 ? len : dot;
    if (base < 1 || base > 8 || (dot >= 0 && len - dot - 1 > 3)) return -1;
    for (int i = 0; i < len; i++) {
        if (i == dot) continue;
        char c = s[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c == ' ' || c == '/' || c == '\\' || c == '*' || c == '?' || c == ':' || c == '"' || c == '<' || c == '>' || c == '|' || c == '.') return -1;
        if (i < base) out[i] = c; else out[8 + i - dot - 1] = c;
    }
    return 0;
}
static void from83(const char *n, char out[13]) {
    int k = 0;
    for (int i = 0; i < 8 && n[i] != ' '; i++) out[k++] = n[i];
    if (n[8] != ' ') {
        out[k++] = '.';
        for (int i = 8; i < 11 && n[i] != ' '; i++) out[k++] = n[i];
    }
    out[k] = 0;
}

static u32 ent_cluster(const dirent_t *e) { return (u32)e->clus_hi << 16 | e->clus_lo; }

/* Finds name83 in directory `dir`. On success copies the entry and where it lives. */
typedef struct { u32 cluster, index; } loc_t;
static int dir_find(u32 dir, const char name83[11], dirent_t *out, loc_t *loc) {
    for (u32 c = dir; c >= 2 && c < EOC; c = fat_get(c)) {
        if (read_cluster(c) != 0) return -1;
        dirent_t *e = (dirent_t *)clbuf;
        for (u32 i = 0; i < clbytes / 32; i++) {
            if (e[i].name[0] == 0) return -1;
            if ((u8)e[i].name[0] == 0xE5 || e[i].attr == ATTR_LFN || (e[i].attr & ATTR_VOLUME)) continue;
            if (memcmp(e[i].name, name83, 11) == 0) {
                if (out) *out = e[i];
                if (loc) { loc->cluster = c; loc->index = i; }
                return 0;
            }
        }
    }
    return -1;
}

/* Walks "A/B/C": returns the cluster of the folder holding the last part and its 8.3 name. */
static int walk(const char *path, u32 *parent, char last83[11]) {
    u32 dir = root_cluster;
    const char *p = path;
    while (*p == '/') p++;
    for (;;) {
        const char *end = p;
        while (*end && *end != '/') end++;
        char n83[11];
        if (to83(p, (int)(end - p), n83) != 0) return -1;
        if (!*end) { *parent = dir; memcpy(last83, n83, 11); return 0; }
        dirent_t e;
        if (dir_find(dir, n83, &e, NULL) != 0 || !(e.attr & ATTR_DIR)) return -1;
        dir = ent_cluster(&e);
        if (dir == 0) dir = root_cluster;
        p = end + 1;
    }
}

static int resolve_dir(const char *path, u32 *dir) {
    if (!path[0] || (path[0] == '/' && !path[1])) { *dir = root_cluster; return 0; }
    u32 parent;
    char n83[11];
    dirent_t e;
    if (walk(path, &parent, n83) != 0 || dir_find(parent, n83, &e, NULL) != 0 || !(e.attr & ATTR_DIR)) return -1;
    *dir = ent_cluster(&e) ? ent_cluster(&e) : root_cluster;
    return 0;
}

int fat_list(const char *path, fat_entry_t *out, int max) {
    if (!fat_mounted) return -1;
    u32 dir;
    if (resolve_dir(path, &dir) != 0) return -1;
    int n = 0;
    for (u32 c = dir; c >= 2 && c < EOC; c = fat_get(c)) {
        if (read_cluster(c) != 0) return -1;
        dirent_t *e = (dirent_t *)clbuf;
        for (u32 i = 0; i < clbytes / 32; i++) {
            if (e[i].name[0] == 0) return n;
            if ((u8)e[i].name[0] == 0xE5 || e[i].attr == ATTR_LFN || (e[i].attr & ATTR_VOLUME) || e[i].name[0] == '.') continue;
            if (n < max) {
                from83(e[i].name, out[n].name);
                out[n].size = e[i].size;
                out[n].is_dir = (e[i].attr & ATTR_DIR) != 0;
            }
            n++;
        }
    }
    return n;
}

int fat_read(const char *path, u8 **data, u32 *size) {
    if (!fat_mounted) return -1;
    u32 parent;
    char n83[11];
    dirent_t e;
    if (walk(path, &parent, n83) != 0 || dir_find(parent, n83, &e, NULL) != 0 || (e.attr & ATTR_DIR)) return -1;
    u8 *buf = kmalloc(e.size ? e.size : 1);
    if (!buf) return -1;
    u32 left = e.size, off = 0;
    for (u32 c = ent_cluster(&e); left && c >= 2 && c < EOC; c = fat_get(c)) {
        if (read_cluster(c) != 0) { kfree(buf); return -1; }
        u32 n = left < clbytes ? left : clbytes;
        memcpy(buf + off, clbuf, n);
        off += n; left -= n;
    }
    if (left) { kfree(buf); return -1; } /* chain shorter than the size */
    *data = buf;
    *size = e.size;
    return 0;
}

static void stamp(dirent_t *e) {
    rtc_time_t t;
    rtc_read(&t);
    e->wdate = e->cdate = e->adate = (u16)(((t.year - 1980) << 9) | (t.month << 5) | t.day);
    e->wtime = e->ctime = (u16)((t.hour << 11) | (t.min << 5) | (t.sec / 2));
}

/* Writes an entry into a free slot of `dir`, growing the folder if it is full. */
static int dir_add(u32 dir, const dirent_t *ne) {
    u32 c = dir, last = dir;
    for (; c >= 2 && c < EOC; last = c, c = fat_get(c)) {
        if (read_cluster(c) != 0) return -1;
        dirent_t *e = (dirent_t *)clbuf;
        for (u32 i = 0; i < clbytes / 32; i++) {
            if (e[i].name[0] == 0 || (u8)e[i].name[0] == 0xE5) {
                e[i] = *ne;
                return write_cluster(c);
            }
        }
    }
    u32 nc = alloc_cluster();
    if (!nc || fat_set(last, nc) != 0) return -1;
    memset(clbuf, 0, clbytes);
    ((dirent_t *)clbuf)[0] = *ne;
    return write_cluster(nc);
}

static int update_entry(const loc_t *loc, const dirent_t *ne) {
    if (read_cluster(loc->cluster) != 0) return -1;
    ((dirent_t *)clbuf)[loc->index] = *ne;
    return write_cluster(loc->cluster);
}

static int finish(int rc) {
    if (fat_flush() != 0) rc = -1;
    fsinfo_save();
    return rc;
}

int fat_write(const char *path, const void *data, u32 size) {
    if (!fat_mounted) return -1;
    u32 parent;
    char n83[11];
    if (walk(path, &parent, n83) != 0) return -1;
    dirent_t e;
    loc_t loc;
    int exists = dir_find(parent, n83, &e, &loc) == 0;
    if (exists && (e.attr & ATTR_DIR)) return -1;
    if (exists) free_chain(ent_cluster(&e));
    /* data first, then the entry that points at it */
    u32 first = 0, prev = 0, left = size;
    const u8 *src = data;
    while (left) {
        u32 c = alloc_cluster();
        if (!c) { free_chain(first); return finish(-1); }
        if (prev) fat_set(prev, c); else first = c;
        u32 n = left < clbytes ? left : clbytes;
        memcpy(clbuf, src, n);
        if (n < clbytes) memset(clbuf + n, 0, clbytes - n);
        if (write_cluster(c) != 0) { free_chain(first); return finish(-1); }
        src += n; left -= n; prev = c;
    }
    if (!exists) { memset(&e, 0, sizeof e); memcpy(e.name, n83, 11); e.attr = 0x20; }
    e.clus_hi = (u16)(first >> 16);
    e.clus_lo = (u16)first;
    e.size = size;
    stamp(&e);
    int rc = exists ? update_entry(&loc, &e) : dir_add(parent, &e);
    return finish(rc);
}

int fat_mkdir(const char *path) {
    if (!fat_mounted) return -1;
    u32 parent;
    char n83[11];
    dirent_t e;
    if (walk(path, &parent, n83) != 0) return -1;
    if (dir_find(parent, n83, &e, NULL) == 0) return (e.attr & ATTR_DIR) ? 0 : -1;
    u32 c = alloc_cluster();
    if (!c) return finish(-1);
    memset(&e, 0, sizeof e);
    memcpy(e.name, n83, 11);
    e.attr = ATTR_DIR;
    e.clus_hi = (u16)(c >> 16);
    e.clus_lo = (u16)c;
    stamp(&e);
    /* the new folder: "." and ".." */
    memset(clbuf, 0, clbytes);
    dirent_t *d = (dirent_t *)clbuf;
    d[0] = e;
    memcpy(d[0].name, ".          ", 11);
    d[1] = e;
    memcpy(d[1].name, "..         ", 11);
    u32 pc = parent == root_cluster ? 0 : parent;
    d[1].clus_hi = (u16)(pc >> 16);
    d[1].clus_lo = (u16)pc;
    if (write_cluster(c) != 0) return finish(-1);
    return finish(dir_add(parent, &e));
}

int fat_delete(const char *path) {
    if (!fat_mounted) return -1;
    u32 parent;
    char n83[11];
    dirent_t e;
    loc_t loc;
    if (walk(path, &parent, n83) != 0 || dir_find(parent, n83, &e, &loc) != 0 || (e.attr & ATTR_DIR)) return -1;
    free_chain(ent_cluster(&e));
    e.name[0] = (char)0xE5;
    return finish(update_entry(&loc, &e));
}
