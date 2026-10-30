/* Chamows apps: the repository on the system disk, installing into memory,
 * loading app ELF files and the API they call. */
#include "gui.h"

/* ---- repository (built by tools/mkrepo.py, linked in by repo.asm) ---- */
extern const u8 repo_blob[];
typedef struct { char magic[8]; u32 count; u32 reserved; } __attribute__((packed)) repo_hdr_t;
static const repo_hdr_t *repo;

void repo_init(void) {
    repo = (const repo_hdr_t *)repo_blob;
    if (memcmp(repo->magic, "CHREPO01", 8) != 0) {
        klog("repo: firma no válida\n");
        repo = NULL;
        return;
    }
    klog("repo: %u apps en el repositorio\n", repo->count);
}
int repo_count(void) { return repo ? (int)repo->count : 0; }
const repo_entry_t *repo_get(int i) {
    return (const repo_entry_t *)(repo_blob + sizeof(repo_hdr_t) + (u32)i * sizeof(repo_entry_t));
}
const repo_entry_t *repo_find(const char *id) {
    for (int i = 0; i < repo_count(); i++)
        if (strcmp(repo_get(i)->id, id) == 0) return repo_get(i);
    return NULL;
}
icon_t repo_icon(const repo_entry_t *e) {
    const u32 *p = (const u32 *)(repo_blob + e->icon_off);
    return (icon_t){p, p + 24 * 24, p + 24 * 24 + 32 * 32};
}
const u8 *repo_elf(const repo_entry_t *e) { return repo_blob + e->elf_off; }

/* ---- installed apps ("C:\Apps", in memory) ---- */
#define MAX_INSTALLED 32
static installed_app_t *installed[MAX_INSTALLED]; /* heap objects: pointers stay valid */
static int n_installed;

int installed_count(void) { return n_installed; }
installed_app_t *installed_get(int i) { return installed[i]; }
installed_app_t *installed_find(const char *id) {
    for (int i = 0; i < n_installed; i++)
        if (strcmp(installed[i]->entry->id, id) == 0) return installed[i];
    return NULL;
}

int app_install(const char *id) {
    const repo_entry_t *e = repo_find(id);
    if (!e) return -1;
    if (installed_find(id)) return 0;
    if (n_installed == MAX_INSTALLED) return -2;
    u8 *copy = kmalloc(e->elf_size);
    installed_app_t *a = kmalloc(sizeof *a);
    if (!copy || !a) { kfree(copy); kfree(a); return -3; }
    memcpy(copy, repo_elf(e), e->elf_size);
    *a = (installed_app_t){e, repo_icon(e), copy, e->elf_size};
    installed[n_installed++] = a;
    klog("instalada: %s (%u bytes)\n", id, e->elf_size);
    shell_refresh();
    return 0;
}

/* ---- running apps ---- */
typedef struct alloc_hdr { struct alloc_hdr *next; u32 pad[3]; } alloc_hdr_t;
struct app_instance {
    installed_app_t *app;
    u8 *mem;
    int windows;
    int dead;
    alloc_hdr_t *allocs;
    struct app_instance *next;
};
static struct app_instance *instances;
struct app_instance *current_app;

int app_running(const char *id) {
    for (struct app_instance *i = instances; i; i = i->next)
        if (!i->dead && strcmp(i->app->entry->id, id) == 0) return 1;
    return 0;
}

void app_window_closed(struct app_instance *inst) {
    if (--inst->windows <= 0) inst->dead = 1; /* freed later, outside app code */
}

void apps_collect(void) {
    struct app_instance **pp = &instances;
    while (*pp) {
        struct app_instance *i = *pp;
        if (!i->dead) { pp = &i->next; continue; }
        *pp = i->next;
        for (alloc_hdr_t *a = i->allocs; a;) { alloc_hdr_t *n = a->next; kfree(a); a = n; }
        kfree(i->mem);
        klog("app terminada: %s\n", i->app->entry->id);
        kfree(i);
    }
}

int app_uninstall(const char *id) {
    installed_app_t *a = installed_find(id);
    if (!a) return -1;
    /* close its windows */
    for (struct app_instance *i = instances; i; i = i->next)
        if (i->app == a && !i->dead) close_app_windows(i);
    apps_collect();
    int idx = 0;
    while (installed[idx] != a) idx++;
    for (int i = idx; i < n_installed - 1; i++) installed[i] = installed[i + 1];
    n_installed--;
    kfree(a->elf);
    kfree(a);
    shell_refresh();
    return 0;
}

/* ---- ELF loader: 32-bit position-independent executables ---- */
typedef struct {
    u8 ident[16];
    u16 type, machine;
    u32 version, entry, phoff, shoff, flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_hdr_t;
typedef struct { u32 type, offset, vaddr, paddr, filesz, memsz, flags, align; } elf_phdr_t;
typedef struct { i32 tag; u32 val; } elf_dyn_t;
typedef struct { u32 offset, info; } elf_rel_t;

static const char *elf_load(const u8 *file, u32 size, u8 **mem_out, u32 *entry_out) {
    const elf_hdr_t *h = (const elf_hdr_t *)file;
    if (size < sizeof *h || memcmp(h->ident, "\x7f" "ELF", 4) != 0) return "no es un archivo ELF";
    if (h->ident[4] != 1 || h->machine != 3) return "no es un programa de 32 bits para x86";
    if (h->type != 3) return "el programa no es reubicable (PIE)";
    if (h->phoff + (u32)h->phnum * sizeof(elf_phdr_t) > size) return "cabeceras dañadas";
    const elf_phdr_t *ph = (const elf_phdr_t *)(file + h->phoff);
    u32 lo = 0xFFFFFFFF, hi = 0;
    for (int i = 0; i < h->phnum; i++) {
        if (ph[i].type != 1) continue;
        if (ph[i].vaddr < lo) lo = ph[i].vaddr;
        if (ph[i].vaddr + ph[i].memsz > hi) hi = ph[i].vaddr + ph[i].memsz;
        if (ph[i].offset + ph[i].filesz > size) return "segmento fuera del archivo";
    }
    if (hi <= lo || hi - lo > 8 * 1024 * 1024) return "tamaño de programa no válido";
    u8 *mem = kzalloc(hi - lo + 16);
    if (!mem) return "sin memoria";
    u32 base = (((u32)mem + 15) & ~15u) - lo;
    for (int i = 0; i < h->phnum; i++)
        if (ph[i].type == 1) memcpy((u8 *)(base + ph[i].vaddr), file + ph[i].offset, ph[i].filesz);
    /* relocations */
    for (int i = 0; i < h->phnum; i++) {
        if (ph[i].type != 2) continue; /* PT_DYNAMIC */
        const elf_dyn_t *d = (const elf_dyn_t *)(base + ph[i].vaddr);
        u32 rel = 0, relsz = 0, relent = sizeof(elf_rel_t);
        for (; d->tag; d++) {
            if (d->tag == 17) rel = d->val;
            else if (d->tag == 18) relsz = d->val;
            else if (d->tag == 19) relent = d->val;
            else if (d->tag == 7) { kfree(mem); return "reubicaciones RELA no admitidas"; }
        }
        for (u32 off = 0; rel && off < relsz; off += relent) {
            const elf_rel_t *r = (const elf_rel_t *)(base + rel + off);
            u32 type = r->info & 0xFF;
            if (type == 8) *(u32 *)(base + r->offset) += base; /* R_386_RELATIVE */
            else if (type != 0) { kfree(mem); return "el programa usa bibliotecas externas"; }
        }
    }
    *mem_out = mem;
    *entry_out = base + h->entry;
    return NULL;
}

int app_run(installed_app_t *a) {
    struct app_instance *inst = kzalloc(sizeof *inst);
    if (!inst) return -1;
    u32 entry;
    const char *err = elf_load(a->elf, a->elf_size, &inst->mem, &entry);
    if (err) {
        klog("no se pudo abrir %s: %s\n", a->entry->id, err);
        notify(&a->icon, "No se pudo abrir la app", err);
        kfree(inst);
        return -1;
    }
    inst->app = a;
    inst->next = instances;
    instances = inst;
    struct app_instance *prev = current_app;
    current_app = inst;
    int (*app_main)(const cw_api *) = (int (*)(const cw_api *))entry;
    int rc = app_main(kernel_api);
    current_app = prev;
    if (rc != 0 || inst->windows == 0) {
        if (rc != 0) klog("app %s terminó con código %d\n", a->entry->id, rc);
        inst->dead = 1;
        apps_collect();
    }
    return rc;
}

/* ---- the API apps receive ---- */
static cw_window *api_window(const char *title, int w, int h, cw_event_fn fn, void *user) {
    const icon_t *icon = current_app ? &current_app->app->icon : &ICON_APP;
    cw_window *win = win_create(title, w, h, fn, user, icon);
    if (win && current_app) { win->app = current_app; current_app->windows++; }
    return win;
}
static void *api_user(cw_window *w) { return w->user; }
static void api_set_timer(cw_window *w, int ms) { w->timer_ms = ms > 0 ? (ms < 10 ? 10 : ms) : 0; w->next_tick = ticks_ms + (u32)w->timer_ms; }
static void api_set_title(cw_window *w, const char *t) {
    strncpy0(w->title, t, sizeof w->title);
    invalidate_rect((rect_t){w->x, w->y, w->w, TITLE_H});
}
static void api_clear(cw_window *w, u32 c) { fill_rect(&w->surf, 0, 0, w->w, w->h, c | 0xFF000000); }
static void api_rect(cw_window *w, int x, int y, int ww, int hh, u32 c) { fill_rect(&w->surf, x, y, ww, hh, c); }
static void api_round(cw_window *w, int x, int y, int ww, int hh, int r, u32 c) { fill_round(&w->surf, x, y, ww, hh, r, c); }
static void api_line(cw_window *w, int x0, int y0, int x1, int y1, int t, u32 c) { draw_line(&w->surf, x0, y0, x1, y1, t, c); }
static void api_circle(cw_window *w, int cx, int cy, int r, u32 c) { fill_circle(&w->surf, cx, cy, r, c); }
static int api_text(cw_window *w, int x, int y, const char *s, u32 c, int f) { return text_draw(&w->surf, font_by_id(f), x, y, s, c); }
static int api_text_width(const char *s, int f) { return text_width(font_by_id(f), s); }
static int api_font_height(int f) { return font_by_id(f)->height; }
static void api_pixels(cw_window *w, int x, int y, int ww, int hh, const unsigned *px) { blit_argb(&w->surf, px, ww, hh, x, y); }
static void *api_alloc(unsigned n) {
    alloc_hdr_t *a = kzalloc(sizeof(alloc_hdr_t) + n);
    if (!a) return NULL;
    if (current_app) { a->next = current_app->allocs; current_app->allocs = a; }
    return a + 1;
}
static void api_free(void *p) {
    if (!p) return;
    alloc_hdr_t *a = (alloc_hdr_t *)p - 1;
    if (current_app) {
        for (alloc_hdr_t **pp = &current_app->allocs; *pp; pp = &(*pp)->next)
            if (*pp == a) { *pp = a->next; break; }
    }
    kfree(a);
}
static unsigned api_ticks(void) { return ticks_ms; }
static unsigned rng = 0x1234567;
static unsigned api_random(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng + ticks_ms; }
static void api_clock(int *h, int *m, int *s) { rtc_time_t t; rtc_read(&t); if (h) *h = t.hour; if (m) *m = t.min; if (s) *s = t.sec; }
static void api_date(int *d, int *mo, int *y) { rtc_time_t t; rtc_read(&t); if (d) *d = t.day; if (mo) *mo = t.month; if (y) *y = t.year; }
static int api_format(char *buf, int n, const char *fmt, int a, int b, int c) { return ksnprintf(buf, (size_t)n, fmt, a, b, c); }
static void api_log(const char *m) { klog("[app] %s\n", m); }

static const cw_api api_table = {
    CW_API_VERSION,
    api_window, api_user, win_invalidate, win_close, api_set_timer, api_set_title,
    api_clear, api_rect, api_round, api_line, api_circle, api_text, api_text_width, api_font_height, api_pixels,
    api_alloc, api_free, api_ticks, api_random, api_clock, api_date, api_format, api_log,
};
const cw_api *kernel_api = &api_table;
