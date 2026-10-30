/* Chamows window system and shell. */
#ifndef CHAMOWS_GUI_H
#define CHAMOWS_GUI_H
#include "gfx.h"
#include "../sdk/chamows.h"

#define TITLE_H 32
#define TASKBAR_H 48

/* Fonts (defined in gui.c from the generated assets). */
extern const font_t *FONT_UI, *FONT_BOLD, *FONT_TITLE, *FONT_MONO;
const font_t *font_by_id(int id);

/* Icons: one app/system icon in three sizes. */
typedef struct { const u32 *s24, *s32, *s48; } icon_t;
const u32 *icon_px(const icon_t *ic, int size);

struct app_instance;

struct cw_window {
    char title[48];
    int x, y;                 /* outer frame position on screen */
    int w, h;                 /* client size */
    surface_t surf;           /* client pixels */
    const icon_t *icon;
    cw_event_fn on_event;
    void *user;
    int timer_ms;
    u32 next_tick;
    int minimized;
    int closing;
    struct app_instance *app; /* NULL for built-in apps */
    struct cw_window *next;   /* z-order: list head is the bottom */
};

/* screen */
extern int SCREEN_W, SCREEN_H;
void gui_init(u32 *lfb, int w, int h);
void gui_run(void);
void invalidate_rect(rect_t r);

/* windows (also exposed to apps through cw_api) */
cw_window *win_create(const char *title, int w, int h, cw_event_fn fn, void *user, const icon_t *icon);
void win_close(cw_window *w);
void win_invalidate(cw_window *w);
void win_focus(cw_window *w);
extern const cw_api *kernel_api;

/* launcher: built-in ids ("store", "terminal", "pc") or installed app ids */
int launch(const char *id);
void shell_refresh(void); /* start menu / desktop icons changed */
void notify(const icon_t *icon, const char *title, const char *text);

/* ---- app repository and installed apps (apps.c) ---- */
typedef struct {
    char id[24];
    char name[40];
    char category[24];
    char desc[120];
    char version[12];
    u32 elf_off, elf_size, icon_off, size_kb;
    u8 pad[20];
} __attribute__((packed)) repo_entry_t;

typedef struct {
    const repo_entry_t *entry;
    icon_t icon;
    u8 *elf;          /* the installed copy ("C:\Apps") */
    u32 elf_size;
} installed_app_t;

void repo_init(void);
int repo_count(void);
const repo_entry_t *repo_get(int i);
const repo_entry_t *repo_find(const char *id);
icon_t repo_icon(const repo_entry_t *e);
const u8 *repo_elf(const repo_entry_t *e);

int installed_count(void);
installed_app_t *installed_get(int i);
installed_app_t *installed_find(const char *id);
/* Copies the package from the repository into memory. */
int app_install(const char *id);
int app_uninstall(const char *id); /* closes running windows first */
int app_run(installed_app_t *a);   /* loads the ELF and calls app_main */
void app_window_closed(struct app_instance *inst);
void close_app_windows(struct app_instance *inst); /* gui.c */
void apps_collect(void);                           /* frees apps whose windows all closed */
extern struct app_instance *current_app;
int app_running(const char *id);

/* ---- built-in apps (builtin.c) ---- */
void store_open(void);
void terminal_open(void);
void pc_open(void);
extern const icon_t ICON_STORE, ICON_TERMINAL, ICON_PC, ICON_LOGO, ICON_APP, ICON_FOLDER;

/* text helpers for built-ins */
void draw_button(surface_t *s, int x, int y, int w, int h, const char *label, int primary, int hover);

#endif
