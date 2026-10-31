/* Chamows built-in app: Explorador de archivos.
 * Shows the installed apps (C:) and the files of the pendrive (D:). Opens
 * folders, runs .exe programs, shows text files, creates folders and deletes
 * files. Keys: arrows to select, Enter to open, Backspace to go up, Supr to delete. */
#include "gui.h"
#include "fat.h"

#define C_TEXT   0xFF1B1D24
#define C_MUTED  0xFF6B7080
#define C_ACCENT 0xFF1F6FE5
#define C_BG     0xFFF6F7FA
#define SIDE_W   170
#define BAR_H    48
#define ROW_H    30
#define HEAD_H   28
#define MAX_ITEMS 128

enum { VIEW_PC, VIEW_APPS, VIEW_DISK };

static cw_window *ex_win;
static int view = VIEW_DISK;
static char cwd[64];                  /* "" = root of D:, else "JUEGOS/SUB" */
static char history[8][64];
static int n_history;
static fat_entry_t items[MAX_ITEMS];
static int n_items, selected = -1, scroll, hover_btn = -1;
static u32 last_click;
static int last_click_row = -1;

static int list_rows(void) { return (ex_win->h - BAR_H - HEAD_H - 8) / ROW_H; }

static int has_ext(const char *name, const char *ext) {
    int n = (int)strlen(name), e = (int)strlen(ext);
    return n > e && strcmp(name + n - e, ext) == 0;
}
static const char *kind_of(const fat_entry_t *e) {
    if (e->is_dir) return "Carpeta";
    if (has_ext(e->name, ".EXE")) return "Programa .exe";
    if (has_ext(e->name, ".TXT")) return "Documento de texto";
    if (has_ext(e->name, ".ELF")) return "App de Chamows";
    return "Archivo";
}

static void reload(void) {
    n_items = 0;
    if (view == VIEW_APPS) {
        for (int i = 0; i < installed_count() && i < MAX_ITEMS; i++) {
            installed_app_t *a = installed_get(i);
            strncpy0(items[n_items].name, a->entry->id, sizeof items[0].name);
            items[n_items].size = a->elf_size;
            items[n_items].is_dir = 0;
            n_items++;
        }
    } else if (view == VIEW_DISK && fat_mounted) {
        static fat_entry_t raw[MAX_ITEMS];
        int n = fat_list(cwd, raw, MAX_ITEMS);
        /* hide "." and "..", folders first */
        for (int pass = 0; pass < 2; pass++)
            for (int i = 0; i < n && i < MAX_ITEMS; i++)
                if (raw[i].name[0] != '.' && raw[i].is_dir == (pass == 0)) items[n_items++] = raw[i];
    }
    selected = n_items ? 0 : -1;
    scroll = 0;
}

static void path_of(const char *name, char *out, int n) {
    if (*cwd) ksnprintf(out, (size_t)n, "%s/%s", cwd, name);
    else strncpy0(out, name, (size_t)n);
}

/* toolbar buttons: 0 back, 1 up, 2 new folder, 3 delete */
static rect_t btn(int i) {
    if (i < 2) return (rect_t){SIDE_W + 12 + i * 40, 9, 32, 30};
    return (rect_t){ex_win->w - (i == 2 ? 236 : 112), 9, i == 2 ? 116 : 100, 30};
}
static void draw_arrow(surface_t *s, rect_t r, int up, u32 c) {
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (up) { draw_line(s, cx, cy - 7, cx, cy + 7, 2, c); draw_line(s, cx - 6, cy - 1, cx, cy - 7, 2, c); draw_line(s, cx + 6, cy - 1, cx, cy - 7, 2, c); }
    else { draw_line(s, cx - 7, cy, cx + 7, cy, 2, c); draw_line(s, cx - 1, cy - 6, cx - 7, cy, 2, c); draw_line(s, cx - 1, cy + 6, cx - 7, cy, 2, c); }
}

static void ex_draw(void) {
    cw_window *w = ex_win;
    surface_t *s = &w->surf;
    fill_rect(s, 0, 0, w->w, w->h, C_BG);
    /* sidebar */
    fill_rect(s, 0, 0, SIDE_W, w->h, 0xFFEDEFF4);
    const char *places[] = {"Este equipo", "Apps (C:)", "Pendrive (D:)"};
    const icon_t *picons[] = {&ICON_PC, &ICON_APP, &ICON_FOLDER};
    for (int i = 0; i < 3; i++) {
        int y = 14 + i * 38;
        if (view == i) fill_round(s, 8, y, SIDE_W - 16, 32, 6, 0xFFDCE6F8);
        blit_argb(s, icon_px(picons[i], 24), 24, 24, 16, y + 4);
        text_draw(s, view == i ? FONT_BOLD : FONT_UI, 48, y + 8, places[i], C_TEXT);
    }
    /* toolbar */
    fill_rect(s, SIDE_W, 0, w->w - SIDE_W, BAR_H, 0xFFFFFFFF);
    fill_rect(s, SIDE_W, BAR_H - 1, w->w - SIDE_W, 1, 0xFFE2E5EC);
    for (int i = 0; i < 2; i++) {
        rect_t r = btn(i);
        int on = i == 0 ? n_history > 0 : (view == VIEW_DISK && *cwd);
        if (hover_btn == i && on) fill_round(s, r.x, r.y, r.w, r.h, 6, 0x14000000);
        draw_arrow(s, r, i == 1, on ? C_TEXT : 0xFFC0C4CE);
    }
    char path[96];
    if (view == VIEW_PC) strncpy0(path, "Este equipo", sizeof path);
    else if (view == VIEW_APPS) strncpy0(path, "C:\\Apps", sizeof path);
    else {
        ksnprintf(path, sizeof path, "D:\\%s", cwd);
        for (char *p = path; *p; p++) if (*p == '/') *p = '\\';
    }
    int px = SIDE_W + 100, pw = w->w - px - (view == VIEW_DISK ? 250 : 12);
    fill_round(s, px, 9, pw, 30, 6, 0xFFF3F4F8);
    text_fit(s, FONT_UI, px + 12, 16, path, pw - 24, C_TEXT);
    if (view == VIEW_DISK && fat_mounted) {
        draw_button(s, btn(2).x, btn(2).y, btn(2).w, btn(2).h, "Nueva carpeta", 0, hover_btn == 2);
        draw_button(s, btn(3).x, btn(3).y, btn(3).w, btn(3).h, "Eliminar", 0, hover_btn == 3);
    }
    int x0 = SIDE_W + 16, y0 = BAR_H + 8;
    /* content */
    if (view == VIEW_PC) {
        const char *names[] = {"Apps (C:)", "Pendrive (D:)"};
        char sub[2][64];
        ksnprintf(sub[0], 64, "%d apps instaladas", installed_count());
        if (fat_mounted) ksnprintf(sub[1], 64, "%s · %u MB libres", fat_label, fat_free_mb());
        else strncpy0(sub[1], "No hay pendrive FAT32 conectado", 64);
        for (int i = 0; i < 2; i++) {
            int y = y0 + 8 + i * 80;
            fill_round(s, x0, y, w->w - x0 - 16, 68, 8, selected == i ? 0xFFDCE6F8 : 0xFFFFFFFF);
            blit_argb(s, icon_px(i ? &ICON_FOLDER : &ICON_APP, 48), 48, 48, x0 + 12, y + 10);
            text_draw(s, FONT_BOLD, x0 + 74, y + 16, names[i], C_TEXT);
            text_draw(s, FONT_UI, x0 + 74, y + 38, sub[i], C_MUTED);
        }
        win_invalidate(w);
        return;
    }
    text_draw(s, FONT_BOLD, x0 + 36, y0 + 4, "Nombre", C_MUTED);
    text_draw(s, FONT_BOLD, w->w - 300, y0 + 4, "Tipo", C_MUTED);
    text_draw(s, FONT_BOLD, w->w - 110, y0 + 4, "Tamaño", C_MUTED);
    if (view == VIEW_DISK && !fat_mounted) {
        text_draw(s, FONT_UI, x0, y0 + 44, "Conecta un pendrive con formato FAT32 para ver sus archivos.", C_MUTED);
    } else if (!n_items) {
        text_draw(s, FONT_UI, x0, y0 + 44, "Esta carpeta está vacía.", C_MUTED);
    }
    int rows = list_rows();
    for (int k = 0; k < rows && scroll + k < n_items; k++) {
        int i = scroll + k, y = y0 + HEAD_H + k * ROW_H;
        fat_entry_t *e = &items[i];
        if (i == selected) fill_round(s, x0 - 6, y, w->w - x0 - 10, ROW_H - 2, 5, 0xFFDCE6F8);
        const icon_t *ic = &ICON_APP;
        char label[48];
        strncpy0(label, e->name, sizeof label);
        const char *kind = kind_of(e);
        if (view == VIEW_APPS) {
            installed_app_t *a = installed_find(e->name);
            if (a) { ic = &a->icon; strncpy0(label, a->entry->name, sizeof label); }
            kind = a && a->elf_size > 2 && a->elf[0] == 'M' ? "Programa .exe" : "App de Chamows";
        } else if (e->is_dir) ic = &ICON_FOLDER;
        blit_argb(s, icon_px(ic, 24), 24, 24, x0, y + 2);
        text_fit(s, FONT_UI, x0 + 36, y + 7, label, w->w - 300 - x0 - 44, C_TEXT);
        text_draw(s, FONT_UI, w->w - 300, y + 7, kind, C_MUTED);
        if (!e->is_dir) {
            char sz[24];
            if (e->size < 1024) ksnprintf(sz, sizeof sz, "%u B", e->size);
            else ksnprintf(sz, sizeof sz, "%u KB", (e->size + 1023) / 1024);
            text_draw(s, FONT_UI, w->w - 110, y + 7, sz, C_MUTED);
        }
    }
    if (n_items > rows) { /* scroll bar */
        int track = rows * ROW_H, th = imax(24, track * rows / n_items);
        int ty = y0 + HEAD_H + (track - th) * scroll / imax(1, n_items - rows);
        fill_round(s, w->w - 8, ty, 4, th, 2, 0x60000000);
    }
    win_invalidate(w);
}

/* ---- a small read-only text viewer ---- */
typedef struct { char *text; u32 len; int top; } viewer_t;
static int viewer_lines(viewer_t *v, int width, int *starts, int max) {
    int n = 0, cols = width / text_width(FONT_MONO, "M");
    u32 i = 0;
    while (i < v->len && n < max) {
        starts[n++] = (int)i;
        int c = 0;
        while (i < v->len && v->text[i] != '\n' && c < cols) { i++; c++; }
        if (i < v->len && v->text[i] == '\n') i++;
    }
    return n;
}
static void viewer_draw(cw_window *w) {
    viewer_t *v = w->user;
    surface_t *s = &w->surf;
    fill_rect(s, 0, 0, w->w, w->h, 0xFFFFFFFF);
    static int starts[4096];
    int n = viewer_lines(v, w->w - 32, starts, 4096), lh = FONT_MONO->height + 2;
    int rows = (w->h - 24) / lh;
    if (v->top > n - rows) v->top = imax(0, n - rows);
    char line[256];
    for (int k = 0; k < rows && v->top + k < n; k++) {
        int a = starts[v->top + k], b = v->top + k + 1 < n ? starts[v->top + k + 1] : (int)v->len;
        int len = imin(b - a, 255);
        memcpy(line, v->text + a, (size_t)len);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
        line[len] = 0;
        text_draw(s, FONT_MONO, 16, 12 + k * lh, line, C_TEXT);
    }
    win_invalidate(w);
}
static void viewer_event(cw_window *w, const cw_event *ev) {
    viewer_t *v = w->user;
    if (ev->type == CW_CLOSE) { kfree(v->text); kfree(v); return; }
    if (ev->type != CW_KEY) return;
    int rows = (w->h - 24) / (FONT_MONO->height + 2);
    if (ev->key == CW_KEY_DOWN) v->top++;
    else if (ev->key == CW_KEY_UP && v->top) v->top--;
    else if (ev->key == CW_KEY_PGDN) v->top += rows;
    else if (ev->key == CW_KEY_PGUP) v->top = imax(0, v->top - rows);
    else return;
    viewer_draw(w);
}
static void open_text(const char *path, const char *name) {
    u8 *data;
    u32 size;
    if (fat_read(path, &data, &size) != 0) { notify(&ICON_FOLDER, "No se pudo abrir", name); return; }
    viewer_t *v = kzalloc(sizeof *v);
    v->text = (char *)data;
    v->len = imin((int)size, 256 * 1024);
    char title[64];
    ksnprintf(title, sizeof title, "%s - Visor de texto", name);
    cw_window *w = win_create(title, 640, 420, viewer_event, v, &ICON_FOLDER);
    if (!w) { kfree(data); kfree(v); return; }
    viewer_draw(w);
}

/* ---- actions ---- */
static void go(int new_view, const char *dir) {
    if (n_history < 8) {
        history[n_history][0] = (char)('0' + view);
        strncpy0(history[n_history] + 1, cwd, sizeof history[0] - 1);
        n_history++;
    }
    view = new_view;
    strncpy0(cwd, dir, sizeof cwd);
    reload();
}
static void back(void) {
    if (!n_history) return;
    n_history--;
    view = history[n_history][0] - '0';
    strncpy0(cwd, history[n_history] + 1, sizeof cwd);
    reload();
}
static void up(void) {
    if (view != VIEW_DISK || !*cwd) return;
    char d[64];
    strncpy0(d, cwd, sizeof d);
    char *slash = NULL;
    for (char *p = d; *p; p++) if (*p == '/') slash = p;
    if (slash) *slash = 0; else d[0] = 0;
    go(VIEW_DISK, d);
}
static void open_item(int i) {
    if (view == VIEW_PC) { go(i == 0 ? VIEW_APPS : VIEW_DISK, ""); return; }
    if (i < 0 || i >= n_items) return;
    fat_entry_t *e = &items[i];
    if (view == VIEW_APPS) { launch(e->name); return; }
    char path[80];
    path_of(e->name, path, sizeof path);
    if (e->is_dir) {
        if (strlen(path) < sizeof cwd) go(VIEW_DISK, path);
        return;
    }
    if (has_ext(e->name, ".EXE")) {
        u8 *data;
        u32 size;
        if (fat_read(path, &data, &size) != 0) { notify(&ICON_FOLDER, "No se pudo leer el archivo", e->name); return; }
        pe_run(data, size, e->name, &ICON_APP);
        kfree(data);
    } else if (has_ext(e->name, ".TXT") || has_ext(e->name, ".INI") || has_ext(e->name, ".MD") || has_ext(e->name, ".LOG")) {
        open_text(path, e->name);
    } else {
        notify(&ICON_FOLDER, "No hay ninguna app para este archivo", "Chamows abre carpetas, programas .exe y archivos de texto.");
    }
}
static void new_folder(void) {
    if (view != VIEW_DISK || !fat_mounted) return;
    for (int n = 1; n < 100; n++) {
        char name[16], path[80];
        ksnprintf(name, sizeof name, n == 1 ? "NUEVA" : "NUEVA%d", n);
        int exists = 0;
        for (int i = 0; i < n_items; i++) if (strcmp(items[i].name, name) == 0) exists = 1;
        if (exists) continue;
        path_of(name, path, sizeof path);
        if (fat_mkdir(path) != 0) notify(&ICON_FOLDER, "No se pudo crear la carpeta", path);
        reload();
        for (int i = 0; i < n_items; i++) if (strcmp(items[i].name, name) == 0) selected = i;
        return;
    }
}
static void delete_selected(void) {
    if (view != VIEW_DISK || selected < 0 || selected >= n_items) return;
    fat_entry_t *e = &items[selected];
    if (e->is_dir) { notify(&ICON_FOLDER, "Las carpetas no se pueden eliminar todavía", "Elimina primero los archivos de dentro desde otro equipo."); return; }
    char path[80];
    path_of(e->name, path, sizeof path);
    if (fat_delete(path) == 0) notify(&ICON_FOLDER, "Archivo eliminado", e->name);
    else notify(&ICON_FOLDER, "No se pudo eliminar", e->name);
    int keep = selected;
    reload();
    selected = imin(keep, n_items - 1);
}
static void keep_visible(void) {
    int rows = list_rows();
    if (selected < scroll) scroll = selected;
    if (selected >= scroll + rows) scroll = selected - rows + 1;
    if (scroll < 0) scroll = 0;
}

static void ex_event(cw_window *w, const cw_event *ev) {
    switch (ev->type) {
    case CW_CLOSE:
        ex_win = NULL;
        return;
    case CW_MOUSE_MOVE: {
        int h = -1;
        for (int i = 0; i < 4; i++) if (rect_has(btn(i), ev->x, ev->y)) h = i;
        if (h != hover_btn) { hover_btn = h; ex_draw(); }
        return;
    }
    case CW_MOUSE_DOWN: {
        if (ev->x < SIDE_W) {
            int i = (ev->y - 14) / 38;
            if (i >= 0 && i < 3 && i != view) go(i, "");
            break;
        }
        if (ev->y < BAR_H) {
            if (rect_has(btn(0), ev->x, ev->y)) back();
            else if (rect_has(btn(1), ev->x, ev->y)) up();
            else if (view == VIEW_DISK && rect_has(btn(2), ev->x, ev->y)) new_folder();
            else if (view == VIEW_DISK && rect_has(btn(3), ev->x, ev->y)) delete_selected();
            break;
        }
        int row;
        if (view == VIEW_PC) row = (ev->y - BAR_H - 16) / 80;
        else row = ev->y >= BAR_H + 8 + HEAD_H ? scroll + (ev->y - BAR_H - 8 - HEAD_H) / ROW_H : -1;
        int max = view == VIEW_PC ? 2 : n_items;
        if (row < 0 || row >= max) { selected = -1; break; }
        int dbl = row == last_click_row && ticks_ms - last_click < 450;
        selected = row;
        last_click = ticks_ms;
        last_click_row = row;
        if (dbl) { last_click_row = -1; open_item(row); }
        break;
    }
    case CW_KEY: {
        int max = view == VIEW_PC ? 2 : n_items;
        if (ev->key == CW_KEY_DOWN && selected + 1 < max) selected++;
        else if (ev->key == CW_KEY_UP && selected > 0) selected--;
        else if (ev->key == CW_KEY_ENTER) open_item(selected);
        else if (ev->key == CW_KEY_BACKSPACE) { if (view == VIEW_DISK && *cwd) up(); else back(); }
        else if (ev->key == CW_KEY_DELETE) delete_selected();
        else return;
        keep_visible();
        break;
    }
    default:
        return;
    }
    if (ex_win) ex_draw();
}

void explorer_open(void) {
    if (ex_win) { win_focus(ex_win); return; }
    ex_win = win_create("Explorador de archivos", 760, 470, ex_event, NULL, &ICON_FOLDER);
    if (!ex_win) return;
    n_history = 0;
    view = fat_mounted ? VIEW_DISK : VIEW_PC;
    cwd[0] = 0;
    reload();
    ex_draw();
}

void explorer_refresh(void) {
    if (!ex_win) return;
    reload();
    ex_draw();
}
