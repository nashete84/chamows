/* Chamows built-in apps: Tienda Chamows, Terminal, Este equipo. */
#include "gui.h"
#include "fat.h"
#include "usb/usb.h"

#define C_TEXT   0xFF1B1D24
#define C_MUTED  0xFF6B7080
#define C_ACCENT 0xFF1F6FE5
#define C_BG     0xFFF3F4F8

/* ================================================================ launcher */
int launch(const char *id) {
    if (strcmp(id, "store") == 0) { store_open(); return 0; }
    if (strcmp(id, "terminal") == 0) { terminal_open(); return 0; }
    if (strcmp(id, "pc") == 0) { pc_open(); return 0; }
    installed_app_t *a = installed_find(id);
    if (!a) return -1;
    return app_run(a);
}

/* ================================================================ store */
#define ST_W 740
#define ST_H 590
#define CARD_W 342
#define CARD_H 134

static cw_window *store_win;
/* Installs run side by side, like the Microsoft Store download queue. */
static u32 install_start[64];   /* per repo index; 0 = not installing */
static int installs_running(void) {
    for (int i = 0; i < repo_count() && i < 64; i++) if (install_start[i]) return 1;
    return 0;
}

static rect_t card_rect(int i) { return (rect_t){20 + (i % 2) * (CARD_W + 16), 112 + (i / 2) * (CARD_H + 14), CARD_W, CARD_H}; }
static rect_t main_btn(int i) { rect_t c = card_rect(i); return (rect_t){c.x + c.w - 112, c.y + c.h - 42, 96, 30}; }
static rect_t second_btn(int i) { rect_t c = card_rect(i); return (rect_t){c.x + c.w - 220, c.y + c.h - 42, 100, 30}; }

static void store_draw(void) {
    cw_window *w = store_win;
    if (!w) return;
    surface_t *s = &w->surf;
    fill_rect(s, 0, 0, w->w, w->h, C_BG);
    blit_argb(s, icon_px(&ICON_STORE, 48), 48, 48, 20, 18);
    text_draw(s, FONT_TITLE, 80, 18, "Tienda Chamows", C_TEXT);
    text_draw(s, FONT_UI, 80, 50, "Elige una app y pulsa Instalar. Se copia del disco del sistema a C:\\Apps.", C_MUTED);
    char buf[64];
    ksnprintf(buf, sizeof buf, "%d apps disponibles · %d instaladas", repo_count(), installed_count());
    text_draw(s, FONT_UI, 20, 84, buf, C_MUTED);
    for (int i = 0; i < repo_count(); i++) {
        const repo_entry_t *e = repo_get(i);
        rect_t c = card_rect(i);
        fill_round(s, c.x, c.y, c.w, c.h, 8, 0xFFFFFFFF);
        stroke_round(s, c.x, c.y, c.w, c.h, 8, 0xFFDDE0E8);
        icon_t ic = repo_icon(e);
        blit_argb(s, ic.s48, 48, 48, c.x + 16, c.y + 16);
        text_fit(s, FONT_BOLD, c.x + 78, c.y + 14, e->name, c.w - 94, C_TEXT);
        ksnprintf(buf, sizeof buf, "%s · %u KB · v%s", e->category, e->size_kb, e->version);
        text_fit(s, FONT_UI, c.x + 78, c.y + 33, buf, c.w - 94, C_MUTED);
        text_wrap(s, FONT_UI, c.x + 78, c.y + 52, e->desc, c.w - 94, 2, C_TEXT);
        if (i < 64 && install_start[i]) {
            int install_pct = (int)((ticks_ms - install_start[i]) * 100 / 1600);
            if (install_pct > 100) install_pct = 100;
            rect_t b = main_btn(i);
            int bx = c.x + 78, bw = c.w - 94;
            fill_round(s, bx, b.y + 18, bw, 6, 3, 0xFFE3E6EE);
            fill_round(s, bx, b.y + 18, bw * install_pct / 100, 6, 3, C_ACCENT);
            ksnprintf(buf, sizeof buf, install_pct < 60 ? "Descargando del disco… %d%%" : "Instalando… %d%%", install_pct);
            text_draw(s, FONT_UI, bx, b.y - 2, buf, C_MUTED);
        } else if (installed_find(e->id)) {
            rect_t b = main_btn(i), b2 = second_btn(i);
            draw_button(s, b.x, b.y, b.w, b.h, "Abrir", 1, 0);
            draw_button(s, b2.x, b2.y, b2.w, b2.h, "Desinstalar", 0, 0);
        } else {
            rect_t b = main_btn(i);
            draw_button(s, b.x, b.y, b.w, b.h, "Instalar", 1, 0);
        }
    }
    win_invalidate(w);
}

static void store_event(cw_window *w, const cw_event *ev) {
    (void)w;
    if (ev->type == CW_CLOSE) { store_win = NULL; memset(install_start, 0, sizeof install_start); return; }
    if (ev->type == CW_TICK) {
        for (int i = 0; i < repo_count() && i < 64; i++) {
            if (!install_start[i] || ticks_ms - install_start[i] < 1600) continue;
            const repo_entry_t *e = repo_get(i);
            install_start[i] = 0;
            int rc = app_install(e->id);
            installed_app_t *a = installed_find(e->id);
            char msg[96];
            if (rc == 0 && a) {
                ksnprintf(msg, sizeof msg, "%s ya está en el menú Inicio y en el escritorio.", e->name);
                notify(&a->icon, "App instalada", msg);
            } else {
                notify(&ICON_STORE, "No se pudo instalar", "No queda memoria libre.");
            }
        }
        if (!installs_running()) w->timer_ms = 0;
        store_draw();
        return;
    }
    if (ev->type != CW_MOUSE_DOWN || ev->button != 1) return;
    for (int i = 0; i < repo_count(); i++) {
        const repo_entry_t *e = repo_get(i);
        if (installed_find(e->id)) {
            if (rect_has(main_btn(i), ev->x, ev->y)) { launch(e->id); return; }
            if (rect_has(second_btn(i), ev->x, ev->y)) {
                char msg[96];
                ksnprintf(msg, sizeof msg, "%s se ha quitado de este equipo.", e->name);
                icon_t ic = repo_icon(e);
                app_uninstall(e->id);
                static icon_t last_icon;
                last_icon = ic;
                notify(&last_icon, "App desinstalada", msg);
                return;
            }
        } else if (i < 64 && !install_start[i] && rect_has(main_btn(i), ev->x, ev->y)) {
            install_start[i] = ticks_ms ? ticks_ms : 1;
            w->timer_ms = 50;
            w->next_tick = ticks_ms + 50;
            store_draw();
            return;
        }
    }
}

void store_open(void) {
    if (store_win) { win_focus(store_win); return; }
    store_win = win_create("Tienda Chamows", ST_W, ST_H, store_event, NULL, &ICON_STORE);
    store_draw();
}

/* ================================================================ terminal */
#define T_COLS 84
#define T_ROWS 22
#define T_HIST 200
typedef struct {
    char lines[T_HIST][T_COLS + 1];
    int n;          /* lines used */
    char input[T_COLS - 6];
    int len;
    int blink;
} term_t;

static void t_print(term_t *t, const char *text) {
    /* splits on newlines and wraps long lines */
    while (1) {
        char line[T_COLS + 1];
        int i = 0;
        while (*text && *text != '\n' && i < T_COLS) line[i++] = *text++;
        line[i] = 0;
        if (t->n == T_HIST) {
            memmove(t->lines[0], t->lines[1], sizeof t->lines[0] * (T_HIST - 1));
            t->n--;
        }
        memcpy(t->lines[t->n++], line, (size_t)i + 1);
        if (*text == '\n') text++;
        if (!*text) break;
    }
}

static int t_cell_w(void) { return FONT_MONO->g['M' - 32].adv; }

static void term_draw(cw_window *w) {
    term_t *t = w->user;
    surface_t *s = &w->surf;
    int cw = t_cell_w(), lh = FONT_MONO->height;
    fill_rect(s, 0, 0, w->w, w->h, 0xFF0C0E14);
    int rows = (w->h - 16) / lh;
    int first = t->n + 1 > rows ? t->n + 1 - rows : 0;
    int y = 8;
    for (int i = first; i < t->n; i++, y += lh) {
        u32 color = strncmp(t->lines[i], "C:\\>", 4) == 0 ? 0xFF7FC4FF : 0xFFD6DBE8;
        text_draw(s, FONT_MONO, 10, y, t->lines[i], color);
    }
    int px = 10 + text_draw(s, FONT_MONO, 10, y, "C:\\> ", 0xFF5FB3FF);
    int iw = text_draw(s, FONT_MONO, px, y, t->input, 0xFFFFFFFF);
    if (t->blink) fill_rect(s, px + iw, y + 2, cw, lh - 3, 0xFFD6DBE8);
    win_invalidate(w);
}

static void t_cmd(cw_window *w, term_t *t, char *line) {
    char out[160];
    char *cmd = line, *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = 0;
    while (*arg == ' ') arg++;
    for (char *p = cmd; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;

    if (!*cmd) return;
    if (!strcmp(cmd, "ayuda") || !strcmp(cmd, "help")) {
        t_print(t, "Comandos de Chamows:\n"
                   "  apps                apps instaladas\n"
                   "  tienda              apps del repositorio\n"
                   "  instalar <id>       instala una app (ej. instalar paint)\n"
                   "  desinstalar <id>    desinstala una app\n"
                   "  abrir <id>          abre una app\n"
                   "  mem                 memoria usada\n"
                   "  hora                fecha y hora\n"
                   "  ver                 versión de Chamows\n"
                   "  usb                 dispositivos USB conectados\n"
                   "  dir [carpeta]       archivos del pendrive (D:)\n"
                   "  cls                 limpia la pantalla\n"
                   "  reiniciar | apagar");
    } else if (!strcmp(cmd, "apps")) {
        t_print(t, "  store               Tienda Chamows (sistema)\n  terminal            Terminal (sistema)\n  pc                  Este equipo (sistema)");
        for (int i = 0; i < installed_count(); i++) {
            const repo_entry_t *e = installed_get(i)->entry;
            ksnprintf(out, sizeof out, "  %s", e->id);
            int n = (int)strlen(out);
            while (n < 22) out[n++] = ' ';
            out[n] = 0;
            ksnprintf(out + n, sizeof out - (size_t)n, "%s", e->name);
            t_print(t, out);
        }
    } else if (!strcmp(cmd, "tienda")) {
        for (int i = 0; i < repo_count(); i++) {
            const repo_entry_t *e = repo_get(i);
            ksnprintf(out, sizeof out, "  %s", e->id);
            int n = (int)strlen(out);
            while (n < 14) out[n++] = ' ';
            out[n] = 0;
            ksnprintf(out + n, sizeof out - (size_t)n, "%s%s (%u KB)", installed_find(e->id) ? "[instalada] " : "", e->name, e->size_kb);
            t_print(t, out);
        }
    } else if (!strcmp(cmd, "instalar")) {
        const repo_entry_t *e = repo_find(arg);
        if (!e) { ksnprintf(out, sizeof out, "No existe la app '%s'. Escribe: tienda", arg); t_print(t, out); return; }
        if (installed_find(arg)) { ksnprintf(out, sizeof out, "%s ya está instalada.", e->name); t_print(t, out); return; }
        ksnprintf(out, sizeof out, fat_mounted ? "Copiando %u bytes de %s al pendrive (D:\\CHAMOWS)..." : "Copiando %u bytes de %s a C:\\Apps (memoria)...", e->elf_size, e->id);
        t_print(t, out);
        t_print(t, app_install(arg) == 0 ? "Instalada. Ábrela con: abrir <id>" : "Error: no queda memoria.");
    } else if (!strcmp(cmd, "desinstalar")) {
        t_print(t, app_uninstall(arg) == 0 ? "Desinstalada." : "Esa app no está instalada.");
    } else if (!strcmp(cmd, "abrir")) {
        if (launch(arg) == 0) { ksnprintf(out, sizeof out, "Abriendo %s...", arg); t_print(t, out); }
        else t_print(t, "Esa app no está instalada. Escribe: apps");
    } else if (!strcmp(cmd, "mem")) {
        ksnprintf(out, sizeof out, "Memoria: %u KB usados de %u KB (RAM total %u MB)",
                  heap_used() / 1024, heap_total() / 1024, total_memory_kb / 1024);
        t_print(t, out);
    } else if (!strcmp(cmd, "hora")) {
        rtc_time_t r; rtc_read(&r);
        ksnprintf(out, sizeof out, "%02d/%02d/%d %02d:%02d:%02d", r.day, r.month, r.year, r.hour, r.min, r.sec);
        t_print(t, out);
    } else if (!strcmp(cmd, "ver")) {
        t_print(t, ARCH_BITS == 64 ? "Chamows [versión " CHAMOWS_VERSION ", 64 bits] - núcleo propio, sin Linux." : "Chamows [versión " CHAMOWS_VERSION ", 32 bits] - núcleo propio, sin Linux.");
    } else if (!strcmp(cmd, "usb")) {
        if (!usb_controllers) { t_print(t, "No hay controlador USB (xHCI) en este equipo."); return; }
        int n = 0;
        for (usb_dev_t *d = usb_devices; d; d = d->next, n++) {
            ksnprintf(out, sizeof out, "  Puerto %d: %s (%x:%x)", d->port, d->name, d->desc.vendor, d->desc.product);
            t_print(t, out);
        }
        if (!n) t_print(t, "No hay dispositivos USB conectados.");
    } else if (!strcmp(cmd, "dir")) {
        if (!fat_mounted) { t_print(t, usb_disk.present ? "El pendrive no tiene formato FAT32." : "No hay ningún pendrive conectado."); return; }
        static fat_entry_t list[64];
        int n = fat_list(arg, list, 64);
        if (n < 0) { t_print(t, "No existe esa carpeta."); return; }
        ksnprintf(out, sizeof out, " Directorio de D:\\%s  (pendrive %s, %u MB libres)", arg, fat_label, fat_free_mb());
        t_print(t, out);
        for (int i = 0; i < n && i < 64; i++) {
            if (list[i].is_dir) ksnprintf(out, sizeof out, "  <DIR>          %s", list[i].name);
            else ksnprintf(out, sizeof out, "  %u bytes%s %s", list[i].size, list[i].size < 10 ? "      " : list[i].size < 1000 ? "    " : list[i].size < 100000 ? "  " : "", list[i].name);
            t_print(t, out);
        }
        if (!n) t_print(t, "  (vacía)");
    } else if (!strcmp(cmd, "cls") || !strcmp(cmd, "clear")) {
        t->n = 0;
    } else if (!strcmp(cmd, "reiniciar")) {
        reboot();
    } else if (!strcmp(cmd, "apagar")) {
        poweroff();
        t_print(t, "Este equipo no admite apagado automático. Ya puedes cerrar la ventana.");
    } else if (!strcmp(cmd, "eco") || !strcmp(cmd, "echo")) {
        t_print(t, arg);
    } else {
        ksnprintf(out, sizeof out, "'%s' no se reconoce como un comando. Escribe 'ayuda'.", cmd);
        t_print(t, out);
    }
    (void)w;
}

static void term_event(cw_window *w, const cw_event *ev) {
    term_t *t = w->user;
    if (ev->type == CW_CLOSE) { kfree(t); return; }
    if (ev->type == CW_TICK) { t->blink = !t->blink; term_draw(w); return; }
    if (ev->type != CW_KEY) return;
    if (ev->key == CW_KEY_ENTER) {
        char line[T_COLS + 8];
        ksnprintf(line, sizeof line, "C:\\> %s", t->input);
        t_print(t, line);
        char cmd[sizeof t->input];
        memcpy(cmd, t->input, sizeof cmd);
        t->input[0] = 0;
        t->len = 0;
        t_cmd(w, t, cmd);
    } else if (ev->key == CW_KEY_BACKSPACE) {
        if (t->len) t->input[--t->len] = 0;
    } else if (ev->key >= 32 && ev->key < 256 && t->len < (int)sizeof t->input - 3) {
        /* store as UTF-8 */
        if (ev->key < 128) t->input[t->len++] = (char)ev->key;
        else { t->input[t->len++] = (char)(0xC0 | (ev->key >> 6)); t->input[t->len++] = (char)(0x80 | (ev->key & 0x3F)); }
        t->input[t->len] = 0;
    }
    t->blink = 1;
    term_draw(w);
}

void terminal_open(void) {
    term_t *t = kzalloc(sizeof *t);
    if (!t) return;
    t_print(t, "Chamows [versión " CHAMOWS_VERSION "]");
    t_print(t, "Escribe 'ayuda' para ver los comandos.");
    t_print(t, "");
    cw_window *w = win_create("Terminal", t_cell_w() * T_COLS + 20, FONT_MONO->height * T_ROWS + 16, term_event, t, &ICON_TERMINAL);
    if (!w) { kfree(t); return; }
    w->timer_ms = 530;
    w->next_tick = ticks_ms + 530;
    term_draw(w);
}

/* ================================================================ this PC */
static cw_window *pc_win;
static void pc_draw(void) {
    cw_window *w = pc_win;
    surface_t *s = &w->surf;
    fill_rect(s, 0, 0, w->w, w->h, C_BG);
    blit_argb(s, icon_px(&ICON_LOGO, 48), 48, 48, 24, 22);
    text_draw(s, FONT_TITLE, 86, 22, "Chamows " CHAMOWS_VERSION, C_TEXT);
    text_draw(s, FONT_UI, 86, 54, "Sistema operativo propio, escrito desde cero.", C_MUTED);
    char cpu[13], buf[96];
    cpu_vendor(cpu);
    rtc_time_t r; rtc_read(&r);
    u32 up = ticks_ms / 1000;
    const char *labels[] = {"Procesador", "Memoria", "Pantalla", "Encendido hace", "Apps instaladas", "Disco del sistema", "Pendrive (D:)", "USB"};
    char vals[8][64];
    ksnprintf(vals[0], 64, "x86 de %d bits (%s)", ARCH_BITS, cpu);
    ksnprintf(vals[1], 64, "%u MB en total · %u KB en uso", total_memory_kb / 1024, heap_used() / 1024);
    ksnprintf(vals[2], 64, "%d × %d, 32 bits de color", SCREEN_W, SCREEN_H);
    ksnprintf(vals[3], 64, "%u min %u s", up / 60, up % 60);
    ksnprintf(vals[4], 64, "%d de %d disponibles", installed_count(), repo_count());
    ksnprintf(vals[5], 64, "Solo lectura, dentro del núcleo");
    if (fat_mounted) ksnprintf(vals[6], 64, "%s · %u MB libres · las apps se guardan aquí", fat_label, fat_free_mb());
    else ksnprintf(vals[6], 64, usb_disk.present ? "Conectado, pero no es FAT32" : "No conectado (las apps viven en memoria)");
    int nd = 0;
    for (usb_dev_t *d = usb_devices; d; d = d->next) nd++;
    if (usb_controllers) ksnprintf(vals[7], 64, "%d dispositivos conectados", nd);
    else ksnprintf(vals[7], 64, "Sin controlador USB");
    int y = 100;
    fill_round(s, 20, y - 8, w->w - 40, 8 * 30 + 16, 8, 0xFFFFFFFF);
    for (int i = 0; i < 8; i++, y += 30) {
        text_draw(s, FONT_UI, 36, y, labels[i], C_MUTED);
        text_draw(s, FONT_BOLD, 180, y, vals[i], C_TEXT);
    }
    (void)buf;
    win_invalidate(w);
}
static void pc_event(cw_window *w, const cw_event *ev) {
    (void)w;
    if (ev->type == CW_CLOSE) { pc_win = NULL; return; }
    if (ev->type == CW_TICK) pc_draw();
}
void pc_open(void) {
    if (pc_win) { win_focus(pc_win); return; }
    pc_win = win_create("Este equipo", 620, 370, pc_event, NULL, &ICON_PC);
    if (!pc_win) return;
    pc_win->timer_ms = 1000;
    pc_win->next_tick = ticks_ms + 1000;
    pc_draw();
}

void builtins_refresh(void) {
    if (store_win) store_draw();
    if (pc_win) pc_draw();
}
