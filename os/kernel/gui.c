/* Chamows window system: compositor, desktop, taskbar, Start menu, input. */
#include "gui.h"
#include "assets.h"

const font_t *FONT_UI = &font_ui, *FONT_BOLD = &font_bold, *FONT_TITLE = &font_title, *FONT_MONO = &font_mono;
const font_t *font_by_id(int id) {
    switch (id) {
    case CW_FONT_BOLD: return FONT_BOLD;
    case CW_FONT_TITLE: return FONT_TITLE;
    case CW_FONT_MONO: return FONT_MONO;
    default: return FONT_UI;
    }
}

const icon_t ICON_LOGO = {icon_logo_24, icon_logo_32, icon_logo_48};
const icon_t ICON_STORE = {icon_store_24, icon_store_32, icon_store_48};
const icon_t ICON_TERMINAL = {icon_terminal_24, icon_terminal_32, icon_terminal_48};
const icon_t ICON_PC = {icon_pc_24, icon_pc_32, icon_pc_48};
const icon_t ICON_APP = {icon_app_24, icon_app_32, icon_app_48};
const icon_t ICON_FOLDER = {icon_folder_24, icon_folder_32, icon_folder_48};
const u32 *icon_px(const icon_t *ic, int size) { return size <= 24 ? ic->s24 : size <= 32 ? ic->s32 : ic->s48; }

/* ---- palette ---- */
#define C_ACCENT     0xFF1F6FE5
#define C_TEXT       0xFF1B1D24
#define C_MUTED      0xFF6B7080
#define C_SURFACE    0xFFF7F8FB
#define C_TITLE_BG   0xFFEEF0F5
#define C_BORDER     0xFFC9CDD8
#define C_HOVER      0x14000000
#define C_TASKBAR    0xE0F1F3F8

int SCREEN_W, SCREEN_H;
static surface_t front, back, wallpaper;
static rect_t dirty;

static cw_window *windows; /* bottom ... top */
static int mx, my, mbuttons;
static cw_window *drag_win, *capture_win;
static int drag_dx, drag_dy;

static int start_open, power_open;
static char start_query[32];
static int hover_key = -1;     /* id of the hovered control, to repaint on change */
static int last_minute = -1;

typedef struct { const icon_t *icon; char title[48]; char text[96]; u32 until; } toast_t;
static toast_t toasts[3];

/* =============================================================== helpers */
void invalidate_rect(rect_t r) {
    r = rect_intersect(r, (rect_t){0, 0, SCREEN_W, SCREEN_H});
    if (!rect_empty(r)) dirty = rect_union(dirty, r);
}
static void invalidate_all(void) { invalidate_rect((rect_t){0, 0, SCREEN_W, SCREEN_H}); }
static rect_t frame_rect(cw_window *w) { return (rect_t){w->x, w->y, w->w, w->h + TITLE_H}; }
static rect_t frame_with_shadow(cw_window *w) {
    rect_t r = frame_rect(w);
    return (rect_t){r.x - 8, r.y - 8, r.w + 16, r.h + 18};
}
static rect_t taskbar_rect(void) { return (rect_t){0, SCREEN_H - TASKBAR_H, SCREEN_W, TASKBAR_H}; }

void draw_button(surface_t *s, int x, int y, int w, int h, const char *label, int primary, int hover) {
    if (primary) {
        fill_round(s, x, y, w, h, 5, hover ? 0xFF2B7BF0 : C_ACCENT);
        int tw = text_width(FONT_BOLD, label);
        text_draw(s, FONT_BOLD, x + (w - tw) / 2, y + (h - FONT_BOLD->height) / 2, label, 0xFFFFFFFF);
    } else {
        fill_round(s, x, y, w, h, 5, hover ? 0xFFF0F2F6 : 0xFFFFFFFF);
        stroke_round(s, x, y, w, h, 5, C_BORDER);
        int tw = text_width(FONT_UI, label);
        text_draw(s, FONT_UI, x + (w - tw) / 2, y + (h - FONT_UI->height) / 2, label, C_TEXT);
    }
}

/* =============================================================== wallpaper */
static u32 isqrt(u32 v) {
    u32 r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}
static u32 lerp_c(u32 a, u32 b, int t /*0..255*/) { return blend(a, b, (u32)t); }

static void make_wallpaper(void) {
    int W = SCREEN_W, H = SCREEN_H;
    int cx = W / 2, cy = (H - TASKBAR_H) * 56 / 100;
    int R = (W < H ? W : H) * 22 / 100;
    struct { int x, y; u32 c; } lobes[3] = {
        {cx, cy - R * 55 / 100, 0xFF2AA7FF},
        {cx - R * 50 / 100, cy + R * 30 / 100, 0xFF5B3CF0},
        {cx + R * 50 / 100, cy + R * 30 / 100, 0xFF2A6FD8},
    };
    int glow_r = W * 45 / 100;
    for (int y = 0; y < H; y++) {
        u32 base = lerp_c(0xFF9CC7FF, 0xFF1D3F99, y * 255 / H);
        for (int x = 0; x < W; x++) {
            u32 c = base;
            int dx = x - cx, dy = y - cy;
            u32 d2 = (u32)(dx * dx + dy * dy);
            u32 g2 = (u32)(glow_r * glow_r);
            if (d2 < g2) {
                int t = 255 - (int)(d2 * 255 / g2);
                c = lerp_c(c, 0xFFDDEBFF, t * t / 255 * 70 / 100);
            }
            for (int i = 0; i < 3; i++) {
                int lx = x - lobes[i].x, ly = y - lobes[i].y;
                u32 ld2 = (u32)(lx * lx + ly * ly);
                if (ld2 > (u32)((R + 2) * (R + 2))) continue;
                int d = (int)isqrt(ld2);
                int edge = R - d; /* soft edge of 2px */
                int a = edge >= 2 ? 150 : edge <= 0 ? 0 : 150 * edge / 2;
                /* lighter towards the middle of each lobe */
                u32 lc = lerp_c(lobes[i].c, 0xFFFFFFFF, (R - d) * 60 / R);
                c = lerp_c(c, lc, a);
            }
            wallpaper.px[y * W + x] = c;
        }
    }
    /* soft light in the centre of the bloom */
    for (int y = cy - R / 3; y < cy + R / 3; y++)
        for (int x = cx - R / 3; x < cx + R / 3; x++) {
            int dx = x - cx, dy = y - cy, rr = R / 3;
            int d2 = dx * dx + dy * dy;
            if (d2 >= rr * rr) continue;
            int t = 255 - d2 * 255 / (rr * rr);
            wallpaper.px[y * W + x] = lerp_c(wallpaper.px[y * W + x], 0xFFFFFFFF, t * 40 / 100);
        }
}

/* =============================================================== cursor */
static const char *CURSOR[] = {
    "X", "XX", "X.X", "X..X", "X...X", "X....X", "X.....X", "X......X", "X.......X",
    "X........X", "X.........X", "X..........X", "X......XXXXX", "X...X..X", "X..XX..X",
    "X.X  X..X", "XX   X..X", "X     X..X", "      X..X", "       XX",
};
#define CUR_W 12
#define CUR_H 20
static u32 under[CUR_W * CUR_H];
static int cur_x = -100, cur_y = -100, cur_shown;

static void cursor_hide(void) {
    if (!cur_shown) return;
    for (int y = 0; y < CUR_H; y++)
        for (int x = 0; x < CUR_W; x++) {
            int sx = cur_x + x, sy = cur_y + y;
            if (sx < SCREEN_W && sy < SCREEN_H) front.px[sy * front.stride + sx] = under[y * CUR_W + x];
        }
    cur_shown = 0;
}
static void cursor_show(void) {
    cur_x = mx; cur_y = my;
    for (int y = 0; y < CUR_H; y++) {
        const char *row = CURSOR[y];
        int len = (int)strlen(row);
        for (int x = 0; x < CUR_W; x++) {
            int sx = cur_x + x, sy = cur_y + y;
            if (sx >= SCREEN_W || sy >= SCREEN_H) continue;
            u32 *p = &front.px[sy * front.stride + sx];
            under[y * CUR_W + x] = *p;
            char c = x < len ? row[x] : ' ';
            if (c == 'X') *p = 0xFF000000;
            else if (c == '.') *p = 0xFFFFFFFF;
        }
    }
    cur_shown = 1;
}

/* =============================================================== desktop icons */
typedef struct { const char *id; const char *label; const icon_t *icon; } launcher_t;
#define MAX_LAUNCHERS 32
static launcher_t launchers[MAX_LAUNCHERS];
static int n_launchers, selected_icon = -1;
static u32 last_click_time;
static int last_click_icon = -1;

static void build_launchers(void) {
    n_launchers = 0;
    launchers[n_launchers++] = (launcher_t){"pc", "Este equipo", &ICON_PC};
    launchers[n_launchers++] = (launcher_t){"store", "Tienda Chamows", &ICON_STORE};
    launchers[n_launchers++] = (launcher_t){"terminal", "Terminal", &ICON_TERMINAL};
    for (int i = 0; i < installed_count() && n_launchers < MAX_LAUNCHERS; i++) {
        installed_app_t *a = installed_get(i);
        launchers[n_launchers++] = (launcher_t){a->entry->id, a->entry->name, &a->icon};
    }
}
#define DI_W 88
#define DI_H 92
static rect_t desktop_icon_rect(int i) {
    int per_col = (SCREEN_H - TASKBAR_H - 16) / DI_H;
    if (per_col < 1) per_col = 1;
    return (rect_t){8 + (i / per_col) * DI_W, 8 + (i % per_col) * DI_H, DI_W - 4, DI_H - 4};
}
static void draw_desktop_icons(surface_t *s) {
    for (int i = 0; i < n_launchers; i++) {
        rect_t r = desktop_icon_rect(i);
        if (rect_empty(rect_intersect(r, s->clip))) continue;
        if (i == selected_icon) fill_round(s, r.x, r.y, r.w, r.h, 6, 0x50FFFFFF);
        else if (hover_key == 1000 + i) fill_round(s, r.x, r.y, r.w, r.h, 6, 0x28FFFFFF);
        blit_argb(s, icon_px(launchers[i].icon, 48), 48, 48, r.x + (r.w - 48) / 2, r.y + 6);
        /* label: centred, up to two lines, with a dark halo for contrast */
        const char *label = launchers[i].label;
        int tw = text_width(FONT_UI, label);
        int lx = r.x + (r.w - imin(tw, r.w - 6)) / 2, ly = r.y + 58;
        for (int oy = -1; oy <= 1; oy++)
            for (int ox = -1; ox <= 1; ox++)
                if (ox || oy) text_fit(s, FONT_UI, lx + ox, ly + oy + 1, label, r.w - 6, 0x40000000);
        text_fit(s, FONT_UI, lx, ly, label, r.w - 6, 0xFFFFFFFF);
    }
}

/* =============================================================== taskbar */
typedef struct { int kind; const char *id; cw_window *win; const icon_t *icon; rect_t r; } tb_item_t;
enum { TB_START, TB_PIN, TB_WIN };
static tb_item_t tb_items[24];
static int n_tb;

static void layout_taskbar(void) {
    n_tb = 0;
    tb_items[n_tb++] = (tb_item_t){TB_START, "start", NULL, &ICON_LOGO, {0}};
    static const char *pins[] = {"store", "terminal"};
    static const icon_t *pin_icons[] = {&ICON_STORE, &ICON_TERMINAL};
    for (int i = 0; i < 2; i++) tb_items[n_tb++] = (tb_item_t){TB_PIN, pins[i], NULL, pin_icons[i], {0}};
    for (cw_window *w = windows; w && n_tb < 24; w = w->next) {
        /* a pinned built-in shows its window on the pin itself */
        int pinned = 0;
        for (int i = 1; i < 3; i++)
            if (tb_items[i].icon == w->icon && !tb_items[i].win) { tb_items[i].win = w; pinned = 1; break; }
        if (!pinned) tb_items[n_tb++] = (tb_item_t){TB_WIN, NULL, w, w->icon, {0}};
    }
    int size = 44, gap = 4, total = n_tb * size + (n_tb - 1) * gap;
    int x = (SCREEN_W - total) / 2, y = SCREEN_H - TASKBAR_H + 2;
    for (int i = 0; i < n_tb; i++, x += size + gap) tb_items[i].r = (rect_t){x, y, size, TASKBAR_H - 4};
}

static cw_window *top_window(void) {
    cw_window *top = NULL;
    for (cw_window *w = windows; w; w = w->next)
        if (!w->minimized) top = w;
    return top;
}

static void draw_taskbar(surface_t *s) {
    rect_t tb = taskbar_rect();
    if (rect_empty(rect_intersect(tb, s->clip))) return;
    fill_rect(s, tb.x, tb.y, tb.w, tb.h, C_TASKBAR);
    fill_rect(s, tb.x, tb.y, tb.w, 1, 0x30FFFFFF);
    layout_taskbar();
    cw_window *active = top_window();
    for (int i = 0; i < n_tb; i++) {
        tb_item_t *it = &tb_items[i];
        int is_active = it->win && it->win == active;
        if (hover_key == 2000 + i || (it->kind == TB_START && start_open) || is_active)
            fill_round(s, it->r.x, it->r.y, it->r.w, it->r.h, 6, is_active ? 0x22FFFFFF | 0x10000000 : C_HOVER);
        blit_argb(s, icon_px(it->icon, 24), 24, 24, it->r.x + 10, it->r.y + 9);
        if (it->win) {
            int wbar = is_active ? 16 : 6;
            fill_round(s, it->r.x + (it->r.w - wbar) / 2, it->r.y + it->r.h - 4, wbar, 3, 1, is_active ? C_ACCENT : 0xFF8A8F9C);
        }
    }
    /* clock */
    rtc_time_t t;
    rtc_read(&t);
    char a[16], b[16];
    ksnprintf(a, sizeof a, "%02d:%02d", t.hour, t.min);
    ksnprintf(b, sizeof b, "%d/%d/%d", t.day, t.month, t.year);
    int w1 = text_width(FONT_UI, a), w2 = text_width(FONT_UI, b);
    int right = SCREEN_W - 14;
    text_draw(s, FONT_UI, right - w1, tb.y + 6, a, C_TEXT);
    text_draw(s, FONT_UI, right - w2, tb.y + 24, b, C_TEXT);
    /* left: system name */
    blit_argb(s, icon_px(&ICON_LOGO, 24), 24, 24, 12, tb.y + 12);
    text_draw(s, FONT_BOLD, 42, tb.y + 15, "Chamows", C_TEXT);
}

/* =============================================================== start menu */
#define SM_W 560
static rect_t start_rect(void) {
    int h = imin(540, SCREEN_H - TASKBAR_H - 24);
    return (rect_t){(SCREEN_W - SM_W) / 2, SCREEN_H - TASKBAR_H - 12 - h, SM_W, h};
}
typedef struct { const char *id; const char *label; const icon_t *icon; rect_t r; } sm_item_t;
static sm_item_t sm_items[MAX_LAUNCHERS];
static int n_sm;

static int matches(const char *label, const char *q) {
    if (!*q) return 1;
    size_t n = strlen(q);
    for (const char *p = label; *p; p++) {
        size_t i = 0;
        for (; i < n; i++) {
            char a = p[i], b = q[i];
            if (!a) return 0;
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

static void layout_start(void) {
    rect_t sr = start_rect();
    n_sm = 0;
    int col = 0, row = 0, cw = (SM_W - 48) / 6;
    for (int i = 0; i < n_launchers; i++) {
        if (!matches(launchers[i].label, start_query)) continue;
        sm_items[n_sm] = (sm_item_t){launchers[i].id, launchers[i].label, launchers[i].icon,
                                     {sr.x + 24 + col * cw, sr.y + 108 + row * 88, cw, 84}};
        n_sm++;
        if (++col == 6) { col = 0; row++; }
    }
}
static rect_t power_btn_rect(void) {
    rect_t sr = start_rect();
    return (rect_t){sr.x + sr.w - 56, sr.y + sr.h - 52, 40, 40};
}
static rect_t power_menu_rect(void) {
    rect_t p = power_btn_rect();
    return (rect_t){p.x + p.w - 170, p.y - 92, 170, 84};
}

static void draw_start(surface_t *s) {
    if (!start_open) return;
    rect_t sr = start_rect();
    if (rect_empty(rect_intersect(frame_with_shadow(&(cw_window){.x = sr.x, .y = sr.y, .w = sr.w, .h = sr.h - TITLE_H}), s->clip))) return;
    draw_shadow(s, sr.x, sr.y, sr.w, sr.h, 8);
    fill_round(s, sr.x, sr.y, sr.w, sr.h, 8, 0xFFF5F6FA);
    stroke_round(s, sr.x, sr.y, sr.w, sr.h, 8, 0xFFD5D9E2);
    /* search box */
    fill_round(s, sr.x + 24, sr.y + 22, sr.w - 48, 36, 18, 0xFFFFFFFF);
    stroke_round(s, sr.x + 24, sr.y + 22, sr.w - 48, 36, 18, *start_query ? C_ACCENT : 0xFFD5D9E2);
    if (*start_query) {
        int tw = text_draw(s, FONT_UI, sr.x + 44, sr.y + 31, start_query, C_TEXT);
        fill_rect(s, sr.x + 45 + tw, sr.y + 31, 1, 17, C_TEXT);
    } else {
        text_draw(s, FONT_UI, sr.x + 44, sr.y + 31, "Escribe para buscar apps", C_MUTED);
    }
    text_draw(s, FONT_BOLD, sr.x + 40, sr.y + 78, "Apps instaladas", C_TEXT);
    layout_start();
    for (int i = 0; i < n_sm; i++) {
        sm_item_t *it = &sm_items[i];
        if (it->r.y + it->r.h > sr.y + sr.h - 72) break;
        if (hover_key == 3000 + i || (i == 0 && *start_query)) fill_round(s, it->r.x, it->r.y, it->r.w, it->r.h, 6, C_HOVER);
        blit_argb(s, icon_px(it->icon, 32), 32, 32, it->r.x + (it->r.w - 32) / 2, it->r.y + 12);
        int tw = imin(text_width(FONT_UI, it->label), it->r.w - 8);
        text_fit(s, FONT_UI, it->r.x + (it->r.w - tw) / 2, it->r.y + 52, it->label, it->r.w - 8, C_TEXT);
    }
    if (!n_sm) text_draw(s, FONT_UI, sr.x + 40, sr.y + 112, "No hay apps con ese nombre. Búscala en la Tienda Chamows.", C_MUTED);
    /* footer */
    int fy = sr.y + sr.h - 64;
    fill_rect(s, sr.x + 1, fy, sr.w - 2, 1, 0xFFE2E5EC);
    fill_round(s, sr.x + 1, fy + 1, sr.w - 2, 62, 8, 0xFFEFF1F6);
    fill_rect(s, sr.x + 1, fy + 1, sr.w - 2, 20, 0xFFEFF1F6);
    fill_circle(s, sr.x + 52, fy + 32, 16, C_ACCENT);
    text_draw(s, FONT_BOLD, sr.x + 47, fy + 23, "U", 0xFFFFFFFF);
    text_draw(s, FONT_UI, sr.x + 78, fy + 23, "Usuario", C_TEXT);
    rect_t pb = power_btn_rect();
    if (hover_key == 3900 || power_open) fill_round(s, pb.x, pb.y, pb.w, pb.h, 6, C_HOVER);
    int pcx = pb.x + 20, pcy = pb.y + 21;
    u32 bg = (hover_key == 3900 || power_open) ? 0xFFDDE0E7 : 0xFFEFF1F6;
    stroke_round(s, pcx - 8, pcy - 8, 17, 17, 8, C_TEXT); /* ring */
    stroke_round(s, pcx - 7, pcy - 7, 15, 15, 7, C_TEXT);
    fill_rect(s, pcx - 3, pcy - 10, 7, 6, bg);            /* gap at the top */
    fill_rect(s, pcx - 1, pcy - 11, 2, 10, C_TEXT);       /* bar */
    if (power_open) {
        rect_t pm = power_menu_rect();
        draw_shadow(s, pm.x, pm.y, pm.w, pm.h, 8);
        fill_round(s, pm.x, pm.y, pm.w, pm.h, 8, 0xFFFFFFFF);
        stroke_round(s, pm.x, pm.y, pm.w, pm.h, 8, 0xFFD5D9E2);
        const char *opts[] = {"Reiniciar", "Apagar"};
        for (int i = 0; i < 2; i++) {
            rect_t r = {pm.x + 4, pm.y + 4 + i * 38, pm.w - 8, 36};
            if (hover_key == 3950 + i) fill_round(s, r.x, r.y, r.w, r.h, 5, C_HOVER);
            text_draw(s, FONT_UI, r.x + 14, r.y + 9, opts[i], C_TEXT);
        }
    }
}

/* =============================================================== windows */
#define WIN_BTN_KEY(w, b) (100000 + (int)((uptr)(w) & 0xFFFFF) * 4 + (b))
static rect_t btn_rect(cw_window *w, int which) { /* 0 close, 1 minimize */
    return (rect_t){w->x + w->w - 46 * (which + 1), w->y, 46, TITLE_H};
}

static void draw_window(surface_t *s, cw_window *w, int active) {
    rect_t f = frame_rect(w);
    if (rect_empty(rect_intersect(frame_with_shadow(w), s->clip))) return;
    draw_shadow(s, f.x, f.y, f.w, f.h, 8);
    fill_round(s, f.x - 1, f.y - 1, f.w + 2, f.h + 2, 9, active ? 0x60404858 : 0x40404858);
    fill_round(s, f.x, f.y, f.w, TITLE_H + 10, 8, C_TITLE_BG);
    blit_argb(s, icon_px(w->icon, 24), 24, 24, f.x + 10, f.y + 4);
    text_fit(s, FONT_UI, f.x + 42, f.y + 8, w->title, w->w - 42 - 100, active ? C_TEXT : 0xFF8A8F9C);
    /* buttons */
    for (int b = 0; b < 2; b++) {
        rect_t r = btn_rect(w, b);
        int hov = hover_key == WIN_BTN_KEY(w, b);
        u32 fg = active ? C_TEXT : 0xFF8A8F9C;
        if (hov) {
            if (b == 0) { fill_rect(s, r.x, r.y, r.w, r.h - 1, 0xFFC42B1C); fg = 0xFFFFFFFF; }
            else fill_rect(s, r.x, r.y, r.w, r.h - 1, C_HOVER);
            if (b == 0) fill_round(s, r.x + r.w - 8, r.y, 8, 8, 0, 0xFFC42B1C);
        }
        int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (b == 0) {
            draw_line(s, cx - 5, cy - 5, cx + 5, cy + 5, 1, fg);
            draw_line(s, cx - 5, cy + 5, cx + 5, cy - 5, 1, fg);
        } else {
            fill_rect(s, cx - 5, cy, 11, 1, fg);
        }
    }
    /* client area: square top, rounded bottom corners */
    int r = 8, cy0 = f.y + TITLE_H;
    for (int yy = 0; yy < w->h; yy++) {
        int inset = 0;
        int from_bottom = w->h - 1 - yy;
        if (from_bottom < r) {
            int d = r - from_bottom;
            inset = r - (int)isqrt((u32)(r * r - (d - 1) * (d - 1)));
            if (inset < 0) inset = 0;
        }
        blit(s, &w->surf, inset, yy, w->w - 2 * inset, 1, f.x + inset, cy0 + yy);
    }
    fill_rect(s, f.x, cy0, f.w, 1, 0xFFE0E3EA);
}

/* =============================================================== toasts */
void notify(const icon_t *icon, const char *title, const char *text) {
    for (int i = 2; i > 0; i--) toasts[i] = toasts[i - 1];
    toasts[0].icon = icon;
    strncpy0(toasts[0].title, title, sizeof toasts[0].title);
    strncpy0(toasts[0].text, text, sizeof toasts[0].text);
    toasts[0].until = ticks_ms + 4500;
    invalidate_rect((rect_t){SCREEN_W - 380, SCREEN_H - TASKBAR_H - 300, 380, 300});
}
static rect_t toast_rect(int i) { return (rect_t){SCREEN_W - 364, SCREEN_H - TASKBAR_H - 92 - i * 92, 350, 80}; }
static void draw_toasts(surface_t *s) {
    for (int i = 0; i < 3; i++) {
        if (!toasts[i].until) continue;
        rect_t r = toast_rect(i);
        draw_shadow(s, r.x, r.y, r.w, r.h, 8);
        fill_round(s, r.x, r.y, r.w, r.h, 8, 0xFFFFFFFF);
        stroke_round(s, r.x, r.y, r.w, r.h, 8, 0xFFD5D9E2);
        blit_argb(s, icon_px(toasts[i].icon, 32), 32, 32, r.x + 14, r.y + 16);
        text_fit(s, FONT_BOLD, r.x + 60, r.y + 14, toasts[i].title, r.w - 74, C_TEXT);
        text_wrap(s, FONT_UI, r.x + 60, r.y + 34, toasts[i].text, r.w - 74, 2, C_MUTED);
    }
}

/* =============================================================== compose */
static void compose(void) {
    if (rect_empty(dirty)) return;
    rect_t r = dirty;
    dirty = (rect_t){0, 0, 0, 0};
    back.clip = r;
    blit(&back, &wallpaper, r.x, r.y, r.w, r.h, r.x, r.y);
    draw_desktop_icons(&back);
    cw_window *active = top_window();
    for (cw_window *w = windows; w; w = w->next)
        if (!w->minimized) draw_window(&back, w, w == active);
    draw_toasts(&back);
    draw_start(&back);
    draw_taskbar(&back);
    surf_reset_clip(&back);
    cursor_hide();
    front.clip = r;
    blit(&front, &back, r.x, r.y, r.w, r.h, r.x, r.y);
    surf_reset_clip(&front);
    cursor_show();
}

/* =============================================================== windows API */
cw_window *win_create(const char *title, int w, int h, cw_event_fn fn, void *user, const icon_t *icon) {
    if (w < 120) w = 120;
    if (h < 60) h = 60;
    if (w > SCREEN_W - 20) w = SCREEN_W - 20;
    if (h > SCREEN_H - TASKBAR_H - TITLE_H - 20) h = SCREEN_H - TASKBAR_H - TITLE_H - 20;
    cw_window *win = kzalloc(sizeof *win);
    u32 *px = kmalloc((size_t)w * h * 4);
    if (!win || !px) { kfree(win); kfree(px); return NULL; }
    strncpy0(win->title, title, sizeof win->title);
    surf_init(&win->surf, px, w, h);
    fill_rect(&win->surf, 0, 0, w, h, C_SURFACE);
    win->w = w; win->h = h;
    win->on_event = fn; win->user = user;
    win->icon = icon ? icon : &ICON_APP;
    static int cascade;
    win->x = (SCREEN_W - w) / 2 - 80 + (cascade % 6) * 30;
    win->y = (SCREEN_H - TASKBAR_H - h - TITLE_H) / 2 - 40 + (cascade % 6) * 26;
    cascade++;
    if (win->x < 4) win->x = 4;
    if (win->y < 4) win->y = 4;
    /* append on top */
    cw_window **pp = &windows;
    while (*pp) pp = &(*pp)->next;
    *pp = win;
    start_open = power_open = 0;
    invalidate_all();
    return win;
}

void win_focus(cw_window *w) {
    cw_window **pp = &windows;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (!*pp) return;
    *pp = w->next;
    w->next = NULL;
    pp = &windows;
    while (*pp) pp = &(*pp)->next;
    *pp = w;
    w->minimized = 0;
    invalidate_rect(frame_with_shadow(w));
    invalidate_rect(taskbar_rect());
}

static int win_alive(cw_window *w) {
    for (cw_window *x = windows; x; x = x->next)
        if (x == w) return 1;
    return 0;
}

void win_close(cw_window *w) {
    if (!win_alive(w) || w->closing) return;
    w->closing = 1;
    cw_event ev = {.type = CW_CLOSE};
    if (w->on_event) {
        struct app_instance *prev = current_app;
        current_app = w->app;
        w->on_event(w, &ev);
        current_app = prev;
    }
    cw_window **pp = &windows;
    while (*pp && *pp != w) pp = &(*pp)->next;
    if (*pp) *pp = w->next;
    if (drag_win == w) drag_win = NULL;
    if (capture_win == w) capture_win = NULL;
    invalidate_rect(frame_with_shadow(w));
    invalidate_rect(taskbar_rect());
    struct app_instance *inst = w->app;
    kfree(w->surf.px);
    kfree(w);
    if (inst) app_window_closed(inst);
}

void win_invalidate(cw_window *w) {
    if (!w->minimized) invalidate_rect((rect_t){w->x, w->y + TITLE_H, w->w, w->h});
}

static void send(cw_window *w, int type, int x, int y, int key, int button) {
    if (!w || !w->on_event || w->closing) return;
    cw_event ev = {type, x, y, key, button};
    struct app_instance *prev = current_app;
    current_app = w->app; /* memory the app allocates is tracked to it */
    w->on_event(w, &ev);
    current_app = prev;
}

void close_app_windows(struct app_instance *inst) {
    for (cw_window *w = windows; w;) {
        cw_window *next = w->next;
        if (w->app == inst) win_close(w);
        w = next;
    }
}

int app_running(const char *id); /* apps.c */

void builtins_refresh(void); /* builtin.c */
void shell_refresh(void) {
    build_launchers();
    builtins_refresh();
    invalidate_all();
}

/* =============================================================== input */
static int hit_hover(void) {
    if (power_open && rect_has(power_menu_rect(), mx, my)) {
        rect_t pm = power_menu_rect();
        return 3950 + (my - pm.y - 4) / 38;
    }
    if (start_open && rect_has(start_rect(), mx, my)) {
        if (rect_has(power_btn_rect(), mx, my)) return 3900;
        for (int i = 0; i < n_sm; i++) if (rect_has(sm_items[i].r, mx, my)) return 3000 + i;
        return -1;
    }
    if (rect_has(taskbar_rect(), mx, my)) {
        for (int i = 0; i < n_tb; i++) if (rect_has(tb_items[i].r, mx, my)) return 2000 + i;
        return -1;
    }
    cw_window *found = NULL; /* topmost window under the cursor */
    for (cw_window *x = windows; x; x = x->next)
        if (!x->minimized && rect_has(frame_rect(x), mx, my)) found = x;
    if (found) {
        for (int b = 0; b < 2; b++)
            if (rect_has(btn_rect(found, b), mx, my)) return WIN_BTN_KEY(found, b);
        return -1;
    }
    for (int i = 0; i < n_launchers; i++) if (rect_has(desktop_icon_rect(i), mx, my)) return 1000 + i;
    return -1;
}

static void hover_update(void) {
    int h = hit_hover();
    if (h == hover_key) return;
    hover_key = h;
    /* repaint the areas that can show hover state */
    invalidate_rect(taskbar_rect());
    if (start_open) invalidate_rect(start_rect());
    if (power_open) invalidate_rect(power_menu_rect());
    for (cw_window *w = windows; w; w = w->next)
        if (!w->minimized) invalidate_rect((rect_t){w->x + w->w - 100, w->y, 100, TITLE_H});
    for (int i = 0; i < n_launchers; i++) invalidate_rect(desktop_icon_rect(i));
}

static void toggle_start(void) {
    start_open = !start_open;
    power_open = 0;
    start_query[0] = 0;
    rect_t sr = start_rect();
    invalidate_rect((rect_t){sr.x - 10, sr.y - 10, sr.w + 20, sr.h + 20});
    invalidate_rect(taskbar_rect());
}
static void close_start(void) { if (start_open) toggle_start(); }

static void open_launcher(const char *id) {
    close_start();
    launch(id);
}

static void mouse_down(int button) {
    /* power menu */
    if (power_open) {
        if (rect_has(power_menu_rect(), mx, my)) {
            int i = (my - power_menu_rect().y - 4) / 38;
            if (i == 0) reboot();
            else { poweroff(); notify(&ICON_LOGO, "Ya puedes cerrar Chamows", "Este equipo no admite apagado automático."); }
            power_open = 0;
            invalidate_all();
            return;
        }
        power_open = 0;
        invalidate_rect(start_rect());
    }
    /* start menu */
    if (start_open) {
        rect_t sr = start_rect();
        if (rect_has(sr, mx, my)) {
            if (rect_has(power_btn_rect(), mx, my)) { power_open = 1; invalidate_rect(sr); return; }
            for (int i = 0; i < n_sm; i++)
                if (rect_has(sm_items[i].r, mx, my)) { open_launcher(sm_items[i].id); return; }
            return;
        }
        if (!(rect_has(tb_items[0].r, mx, my))) close_start();
    }
    /* taskbar */
    if (rect_has(taskbar_rect(), mx, my)) {
        for (int i = 0; i < n_tb; i++) {
            tb_item_t *it = &tb_items[i];
            if (!rect_has(it->r, mx, my)) continue;
            if (it->kind == TB_START) { toggle_start(); return; }
            if (it->win) {
                if (it->win == top_window()) {
                    it->win->minimized = 1;
                    invalidate_all();
                } else {
                    win_focus(it->win);
                }
            } else {
                launch(it->id);
            }
            return;
        }
        return;
    }
    /* windows, top first */
    cw_window *hit = NULL;
    for (cw_window *w = windows; w; w = w->next)
        if (!w->minimized && rect_has(frame_rect(w), mx, my)) hit = w;
    if (hit) {
        if (hit != top_window()) win_focus(hit);
        if (my < hit->y + TITLE_H) {
            if (rect_has(btn_rect(hit, 0), mx, my)) { win_close(hit); return; }
            if (rect_has(btn_rect(hit, 1), mx, my)) { hit->minimized = 1; invalidate_all(); return; }
            drag_win = hit;
            drag_dx = mx - hit->x;
            drag_dy = my - hit->y;
            return;
        }
        capture_win = hit;
        send(hit, CW_MOUSE_DOWN, mx - hit->x, my - hit->y - TITLE_H, 0, button);
        return;
    }
    /* desktop icons */
    int old = selected_icon;
    selected_icon = -1;
    for (int i = 0; i < n_launchers; i++) {
        if (!rect_has(desktop_icon_rect(i), mx, my)) continue;
        selected_icon = i;
        if (last_click_icon == i && ticks_ms - last_click_time < 500) {
            last_click_icon = -1;
            launch(launchers[i].id);
        } else {
            last_click_icon = i;
            last_click_time = ticks_ms;
        }
        break;
    }
    if (old != selected_icon) {
        if (old >= 0) invalidate_rect(desktop_icon_rect(old));
        if (selected_icon >= 0) invalidate_rect(desktop_icon_rect(selected_icon));
    }
}

static void mouse_up(int button) {
    drag_win = NULL;
    if (capture_win) {
        cw_window *w = capture_win;
        capture_win = NULL;
        send(w, CW_MOUSE_UP, mx - w->x, my - w->y - TITLE_H, 0, button);
    }
}

static void handle_mouse(const input_event_t *ev) {
    int ox = mx, oy = my;
    mx += ev->dx;
    my += ev->dy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx > SCREEN_W - 1) mx = SCREEN_W - 1;
    if (my > SCREEN_H - 1) my = SCREEN_H - 1;
    if (drag_win) {
        rect_t old = frame_with_shadow(drag_win);
        drag_win->x = mx - drag_dx;
        drag_win->y = my - drag_dy;
        if (drag_win->y < 0) drag_win->y = 0;
        if (drag_win->y > SCREEN_H - TASKBAR_H - TITLE_H) drag_win->y = SCREEN_H - TASKBAR_H - TITLE_H;
        invalidate_rect(old);
        invalidate_rect(frame_with_shadow(drag_win));
    } else if (capture_win && (ox != mx || oy != my)) {
        send(capture_win, CW_MOUSE_MOVE, mx - capture_win->x, my - capture_win->y - TITLE_H, 0, mbuttons & 1 ? 1 : 2);
    }
    int pressed = ev->buttons & ~mbuttons, released = mbuttons & ~ev->buttons;
    mbuttons = ev->buttons;
    if (pressed & 1) mouse_down(1);
    if (pressed & 2) mouse_down(2);
    if (released & 1) mouse_up(1);
    if (released & 2) mouse_up(2);
    if (!drag_win) hover_update();
    if (ox != mx || oy != my) {
        cursor_hide();
        cursor_show();
    }
}

static void handle_key(const input_event_t *ev) {
    if (!ev->pressed || !ev->key) return;
    int k = ev->key;
    if (k == KEY_SUPER || k == KEY_F1) { toggle_start(); return; }
    if (start_open) {
        size_t n = strlen(start_query);
        if (k == KEY_ESC) close_start();
        else if (k == KEY_BACKSPACE && n) start_query[n - 1] = 0;
        else if (k == KEY_ENTER) { layout_start(); if (n_sm) open_launcher(sm_items[0].id); return; }
        else if (k >= 32 && k < 127 && n + 1 < sizeof start_query) { start_query[n] = (char)k; start_query[n + 1] = 0; }
        invalidate_rect(start_rect());
        return;
    }
    cw_window *top = top_window();
    if (top) send(top, CW_KEY, 0, 0, k & 0x1FF, 0);
}

/* =============================================================== panic */
void panic_screen(const char *msg, u32 eip) {
    if (!front.px) return;
    surf_reset_clip(&front);
    fill_rect(&front, 0, 0, SCREEN_W, SCREEN_H, 0xFF0F4FA8);
    int x = SCREEN_W / 6, y = SCREEN_H / 4;
    text_draw(&front, FONT_TITLE, x, y, ":(", 0xFFFFFFFF);
    text_draw(&front, FONT_TITLE, x, y + 50, "Chamows ha encontrado un problema y tiene que reiniciarse.", 0xFFFFFFFF);
    char buf[96];
    ksnprintf(buf, sizeof buf, "Detalle técnico: %s (dirección %x)", msg, eip);
    text_draw(&front, FONT_UI, x, y + 100, buf, 0xFFDDE8FF);
    text_draw(&front, FONT_UI, x, y + 124, "Pulsa el botón de reinicio de la máquina virtual.", 0xFFDDE8FF);
}

/* =============================================================== main loop */
void gui_init(u32 *lfb, int w, int h) {
    SCREEN_W = w;
    SCREEN_H = h;
    surf_init(&front, lfb, w, h);
    surf_init(&back, kmalloc((size_t)w * h * 4), w, h);
    surf_init(&wallpaper, kmalloc((size_t)w * h * 4), w, h);
    make_wallpaper();
    build_launchers();
    mx = w / 2;
    my = h / 2;
    invalidate_all();
}

void gui_run(void) {
    compose();
    notify(&ICON_STORE, "Bienvenido a Chamows", "Abre la Tienda Chamows para instalar tus primeras apps.");
    klog("chamows: escritorio listo\n");
    for (;;) {
        input_event_t ev;
        while (input_poll(&ev)) {
            if (ev.type == EV_MOUSE) handle_mouse(&ev);
            else if (ev.type == EV_KEY) handle_key(&ev);
        }
        /* timers */
        for (cw_window *w = windows; w;) {
            cw_window *next = w->next;
            if (w->timer_ms && (i32)(ticks_ms - w->next_tick) >= 0) {
                w->next_tick = ticks_ms + (u32)w->timer_ms;
                send(w, CW_TICK, 0, 0, 0, 0);
            }
            w = next;
        }
        /* toasts expire */
        for (int i = 0; i < 3; i++)
            if (toasts[i].until && (i32)(ticks_ms - toasts[i].until) >= 0) {
                toasts[i].until = 0;
                invalidate_rect((rect_t){SCREEN_W - 380, SCREEN_H - TASKBAR_H - 300, 380, 300});
            }
        /* clock */
        rtc_time_t t;
        if (ticks_ms % 1000 < 20) {
            rtc_read(&t);
            if (t.min != last_minute) { last_minute = t.min; invalidate_rect((rect_t){SCREEN_W - 140, SCREEN_H - TASKBAR_H, 140, TASKBAR_H}); }
        }
        apps_collect();
        compose();
        __asm__ volatile("sti; hlt");
    }
}
