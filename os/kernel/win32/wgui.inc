/* The USER32 and GDI32 pieces that SDL2 programs (like ScummVM) need:
 * Unicode window classes, window properties and longs, one monitor with one
 * display mode, DIB sections for the software framebuffer, and input helpers.
 * Included from win32.c after wfiles.inc (64-bit only). */

#define HMON ((void *)0x4D4F4E31)
#define HDESK ((void *)0x4445534B)

static void wname(const u16 *w, char *a, int n) {
    if ((uptr)w < 0x10000) ksnprintf(a, (size_t)n, "#%u", (unsigned)(uptr)w);
    else w2a(w, a, n);
}

/* ---- classes and windows ---- */
static WINAPI u16 g_RegisterClassW(const u8 *wc) {
    char n[64];
    wname(*(const u16 **)(wc + 64), n, sizeof n);
    if (find_class(n)) return 0xC000; /* already there */
    return register_class(n, *(WNDPROC *)(wc + 8), *(u64 *)(wc + 48));
}
static WINAPI int g_GetClassInfoExW(void *inst, const u16 *name, u8 *out) {
    char n[64];
    wname(name, n, sizeof n);
    wclass_t *c = find_class(n);
    if (!c) { last_error = 1411; return 0; }
    *(WNDPROC *)(out + 8) = c->proc;
    *(const u16 **)(out + 64) = name;
    return 0xC000 + (int)(c - cur->classes);
}

static w32win_t *focused_win(void) {
    cw_window *t = win_top();
    for (w32win_t *w = cur->wins; w; w = w->next) if (w->cw && w->cw == t && !w->destroyed) return w;
    return NULL;
}
static void activate(w32win_t *w) {
    if (!w || w->activated || !w->cw || !w->proc) return;
    w->activated = 1;
    post(cur, w, 0x18, 1, 0);                                                  /* WM_SHOWWINDOW */
    post(cur, w, 0x5, 0, (i64)((u32)w->cw->h << 16 | (u32)w->cw->w));          /* WM_SIZE */
    post(cur, w, 0x6, 1, 0);                                                   /* WM_ACTIVATE */
    post(cur, w, 0x7, 0, 0);                                                   /* WM_SETFOCUS */
}

static WINAPI void *g_CreateWindowExW(u32 ex, const u16 *cls, const u16 *title, u32 style, int x, int y, int w, int h,
                                      void *parent, void *menu, void *inst, void *param) {
    char c[64], t[80];
    const char *cn = (const char *)cls;
    if ((uptr)cls >= 0x10000) { w2a(cls, c, sizeof c); cn = c; }
    w2a(title, t, sizeof t);
    if ((i64)parent == -3) { /* HWND_MESSAGE: a window that only receives messages */
        wclass_t *k = find_class(cn);
        w32win_t *win = new_win();
        win->hidden = 1;
        win->style = style;
        win->exstyle = ex;
        if (!k) { win->destroyed = 1; return win; }
        win->proc = k->proc;
        u64 cs[10] = {(u64)param};
        win->proc(win, 1, 0, (i64)cs);
        return win;
    }
    w32win_t *win = u_CreateWindowExA(ex, cn, t, style, x, y, w, h, parent, menu, inst, param);
    if (win) {
        win->style = style;
        win->exstyle = ex;
        if (win->cw) win->cw->hover = 1;
    }
    return win;
}
static WINAPI int g_ShowWindow(void *h, int cmd) {
    w32win_t *w = W(h);
    if (!w) return 0;
    w->dirty = 1;
    if (cmd != 0 && w->cw) { win_focus(w->cw); activate(w); }
    return 0;
}
static WINAPI int g_SetWindowPos(void *h, void *after, int x, int y, int cx, int cy, u32 fl) {
    w32win_t *w = W(h);
    if (!w || !w->cw) return 1;
    if (!(fl & 1) && cx > 0 && cy > 0) { /* not SWP_NOSIZE: outer size, as AdjustWindowRect made it */
        int ow = w->cw->w, oh = w->cw->h;
        if (win_resize(w->cw, cx - 16, cy - 39) == 0 && (ow != w->cw->w || oh != w->cw->h) && w->proc)
            w->proc(w, 0x5, 0, (i64)((u32)w->cw->h << 16 | (u32)w->cw->w)); /* WM_SIZE */
    }
    if (fl & 0x40) g_ShowWindow(h, 5); /* SWP_SHOWWINDOW */
    return 1;
}
static WINAPI int g_MoveWindow(void *h, int x, int y, int cx, int cy, int repaint) { return g_SetWindowPos(h, NULL, x, y, cx, cy, 0); }

static WINAPI i64 g_GetWindowLongPtrW(void *h, int i) {
    w32win_t *w = W(h);
    if (!w) return 0;
    switch (i) {
    case -4: return (i64)w->proc;
    case -6: return (i64)cur->image;
    case -16: return (i64)(w->style | (w->cw ? 0x10000000u : 0));
    case -20: return w->exstyle;
    case -21: return w->userdata;
    }
    return 0;
}
static WINAPI i64 g_SetWindowLongPtrW(void *h, int i, i64 v) {
    w32win_t *w = W(h);
    if (!w) return 0;
    i64 old = g_GetWindowLongPtrW(h, i);
    switch (i) {
    case -4: w->proc = (WNDPROC)v; break;
    case -16: w->style = (u32)v; break;
    case -20: w->exstyle = (u32)v; break;
    case -21: w->userdata = v; break;
    }
    return old;
}
static WINAPI i64 g_CallWindowProcW(WNDPROC proc, void *h, u32 msg, u64 wp, i64 lp) { return proc ? proc(h, msg, wp, lp) : 0; }

/* properties: SetPropW(hwnd, L"SDL_WindowData", data) */
static WINAPI int g_SetPropW(void *h, const u16 *name, u64 v) {
    w32win_t *w = W(h);
    char n[32];
    if (!w) return 0;
    wname(name, n, sizeof n);
    int slot = -1;
    for (int i = 0; i < 8; i++) if (w->props[i].name[0] && !strcmp(w->props[i].name, n)) slot = i;
    for (int i = 0; i < 8 && slot < 0; i++) if (!w->props[i].name[0]) slot = i;
    if (slot < 0) return 0;
    strncpy0(w->props[slot].name, n, sizeof w->props[slot].name);
    w->props[slot].v = v;
    return 1;
}
static WINAPI u64 g_GetPropW(void *h, const u16 *name) {
    w32win_t *w = W(h);
    char n[32];
    if (!w) return 0;
    wname(name, n, sizeof n);
    for (int i = 0; i < 8; i++) if (w->props[i].name[0] && !strcmp(w->props[i].name, n)) return w->props[i].v;
    return 0;
}
static WINAPI u64 g_RemovePropW(void *h, const u16 *name) {
    w32win_t *w = W(h);
    char n[32];
    if (!w) return 0;
    wname(name, n, sizeof n);
    for (int i = 0; i < 8; i++)
        if (w->props[i].name[0] && !strcmp(w->props[i].name, n)) { w->props[i].name[0] = 0; return w->props[i].v; }
    return 0;
}

/* coordinates: a client area starts below the Chamows title bar */
static void client_origin(w32win_t *w, int *x, int *y) {
    *x = w && w->cw ? w->cw->x : 0;
    *y = w && w->cw ? w->cw->y + TITLE_H : 0;
}
static WINAPI int g_GetWindowRect(void *h, RECT *r) {
    w32win_t *w = W(h);
    if (h == HDESK) { *r = (RECT){0, 0, SCREEN_W, SCREEN_H}; return 1; }
    if (!w || !w->cw) { *r = (RECT){0, 0, 0, 0}; return w != NULL; }
    int x, y;
    client_origin(w, &x, &y);
    *r = (RECT){x - 8, y - 31, x + w->cw->w + 8, y + w->cw->h + 8};
    return 1;
}
static WINAPI int g_ClientToScreen(void *h, i32 *pt) { int x, y; client_origin(W(h), &x, &y); pt[0] += x; pt[1] += y; return 1; }
static WINAPI int g_ScreenToClient(void *h, i32 *pt) { int x, y; client_origin(W(h), &x, &y); pt[0] -= x; pt[1] -= y; return 1; }
static WINAPI int g_GetCursorPos(i32 *pt) { int x, y; mouse_pos(&x, &y); pt[0] = x; pt[1] = y; return 1; }
static WINAPI int g_GetClipCursor(RECT *r) { *r = (RECT){0, 0, SCREEN_W, SCREEN_H}; return 1; }
static WINAPI void *g_GetForegroundWindow(void) { return focused_win(); }
static WINAPI void *g_SetFocus(void *h) {
    w32win_t *old = focused_win(), *w = W(h);
    if (w && w->cw) win_focus(w->cw);
    return old;
}
static WINAPI int g_SetForegroundWindow(void *h) { g_SetFocus(h); return 1; }
static WINAPI void *g_GetDesktopWindow(void) { return HDESK; }
static WINAPI int g_IsIconic(void *h) { w32win_t *w = W(h); return w && w->cw && w->cw->minimized; }
static WINAPI int g_IsWindowVisible(void *h) { w32win_t *w = W(h); return w && w->cw && !w->cw->minimized; }
static WINAPI int g_SetWindowTextW(void *h, const u16 *t) { char a[80]; w2a(t, a, sizeof a); return u_SetWindowTextA(h, a); }
static WINAPI int g_GetWindowTextW(void *h, u16 *out, int n) {
    w32win_t *w = W(h);
    if (n > 0) out[0] = 0;
    return w && w->cw && n > 0 ? (int)a2w(w->cw->title, out, (u32)n) : 0;
}
static WINAPI int g_GetUpdateRect(void *h, RECT *r, int erase) {
    w32win_t *w = W(h);
    if (r) { *r = (RECT){0, 0, 0, 0}; if (w && w->cw && w->dirty) { r->right = w->cw->w; r->bottom = w->cw->h; } }
    return w && w->dirty;
}
static WINAPI u32 g_GetWindowThreadProcessId(void *h, u32 *pid) { if (pid) *pid = 4; return cur_th ? cur_th->id : 1; }
static WINAPI int g_PostThreadMessageW(u32 tid, u32 msg, u64 wp, i64 lp) { post(cur, NULL, msg, wp, lp); return 1; }
static u32 next_wmsg = 0xC100;
static WINAPI u32 g_RegisterWindowMessageA(const char *n) { return next_wmsg++; }
static WINAPI u32 g_GetDoubleClickTime(void) { return 500; }

/* rectangles */
static WINAPI int g_SetRect(RECT *r, int a, int b, int c, int d) { *r = (RECT){a, b, c, d}; return 1; }
static WINAPI int g_IsRectEmpty(const RECT *r) { return r->right <= r->left || r->bottom <= r->top; }
static WINAPI int g_PtInRect(const RECT *r, i64 pt) { int x = (i32)pt, y = (i32)(pt >> 32); return x >= r->left && x < r->right && y >= r->top && y < r->bottom; }
static WINAPI int g_IntersectRect(RECT *o, const RECT *a, const RECT *b) {
    RECT r = {imax(a->left, b->left), imax(a->top, b->top), imin(a->right, b->right), imin(a->bottom, b->bottom)};
    if (r.right <= r.left || r.bottom <= r.top) { *o = (RECT){0, 0, 0, 0}; return 0; }
    *o = r;
    return 1;
}

/* ---- one monitor, one display mode ---- */
typedef int (WINAPI *MONENUMPROC)(void *, void *, RECT *, i64);
static WINAPI int g_EnumDisplayMonitors(void *hdc, const RECT *clip, MONENUMPROC fn, i64 lp) {
    RECT r = {0, 0, SCREEN_W, SCREEN_H};
    if (fn) fn(HMON, hdc, &r, lp);
    return 1;
}
static WINAPI void *g_MonitorFrom(void) { return HMON; }
static WINAPI int g_GetMonitorInfoW(void *m, u8 *mi) {
    u32 cb = *(u32 *)mi;
    *(RECT *)(mi + 4) = (RECT){0, 0, SCREEN_W, SCREEN_H};
    *(RECT *)(mi + 20) = (RECT){0, 0, SCREEN_W, SCREEN_H - TASKBAR_H};
    *(u32 *)(mi + 36) = 1; /* primary */
    if (cb >= 104) a2w("\\\\.\\DISPLAY1", (u16 *)(mi + 40), 32);
    return 1;
}
static WINAPI int g_EnumDisplaySettingsW(const u16 *dev, u32 mode, u8 *dm) {
    if (mode != 0 && mode != 0xFFFFFFFF && mode != 0xFFFFFFFE) return 0;
    u16 size = *(u16 *)(dm + 68), extra = *(u16 *)(dm + 70);
    memset(dm, 0, size >= 188 ? size : 220);
    *(u16 *)(dm + 68) = size ? size : 220;
    *(u16 *)(dm + 70) = extra;
    *(u32 *)(dm + 72) = 0x40000 | 0x80000 | 0x100000 | 0x400000 | 0x20; /* bpp, width, height, frequency, position */
    *(u32 *)(dm + 168) = 32;
    *(u32 *)(dm + 172) = (u32)SCREEN_W;
    *(u32 *)(dm + 176) = (u32)SCREEN_H;
    *(u32 *)(dm + 184) = 60;
    return 1;
}
static WINAPI int g_EnumDisplaySettingsExW(const u16 *dev, u32 mode, u8 *dm, u32 fl) { return g_EnumDisplaySettingsW(dev, mode, dm); }
static WINAPI int g_EnumDisplayDevicesW(const u16 *dev, u32 i, u8 *dd, u32 fl) {
    if (i != 0) return 0;
    u32 cb = *(u32 *)dd;
    memset(dd + 4, 0, cb > 4 ? cb - 4 : 0);
    a2w("\\\\.\\DISPLAY1", (u16 *)(dd + 4), 32);
    a2w("Pantalla de Chamows", (u16 *)(dd + 68), 128);
    *(u32 *)(dd + 324) = 1 | 4; /* attached, primary */
    return 1;
}
static WINAPI int g_SystemParametersInfoW(u32 act, u32 ui, void *pv, u32 ini) {
    if (act == 0x30 && pv) { *(RECT *)pv = (RECT){0, 0, SCREEN_W, SCREEN_H - TASKBAR_H}; return 1; } /* SPI_GETWORKAREA */
    return 0;
}
static WINAPI int g_GetDeviceCaps(void *hdc, int i) {
    switch (i) {
    case 8: return SCREEN_W;   /* HORZRES */
    case 10: return SCREEN_H;  /* VERTRES */
    case 12: return 32;        /* BITSPIXEL */
    case 14: return 1;         /* PLANES */
    case 88: case 90: return 96; /* LOGPIXELSX/Y */
    case 116: return 60;       /* VREFRESH */
    case 117: return SCREEN_W; /* DESKTOPHORZRES */
    case 118: return SCREEN_H; /* DESKTOPVERTRES */
    }
    return 0;
}

/* ---- pixels the program draws itself ---- */
static WINAPI void *g_CreateDIBSection(void *hdc, const u8 *bmi, u32 usage, void **bits, void *sec, u32 off) {
    i32 w = *(i32 *)(bmi + 4), h = *(i32 *)(bmi + 8);
    u16 bpp = *(u16 *)(bmi + 14);
    int ah = h < 0 ? -h : h;
    if (bits) *bits = NULL;
    if (bpp != 32 || w <= 0 || ah <= 0 || w > 4096 || ah > 4096) { klog("exe: CreateDIBSection %dx%d de %u bits no soportado\n", w, h, bpp); return NULL; }
    u32 *px = kzalloc((size_t)w * (size_t)ah * 4);
    if (!px) return NULL;
    gobj_t *b = new_obj(K_BITMAP, 0);
    surf_init(&b->surf, px, w, ah);
    if (h > 0) { b->surf.px = px + (size_t)(ah - 1) * (size_t)w; b->surf.stride = -w; } /* bottom-up rows */
    b->dib = 1;
    b->base = px;
    if (bits) *bits = px;
    return b;
}
/* StretchDIBits: 32-bit pixels, nearest neighbour */
static WINAPI int g_StretchDIBits(dc_t *dc, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs,
                                  const u32 *bits, const u8 *bmi, u32 usage, u32 rop) {
    surface_t *d = dc_surf(dc);
    i32 bw = *(i32 *)(bmi + 4), bh = *(i32 *)(bmi + 8);
    if (!d || !bits || *(u16 *)(bmi + 14) != 32 || wd <= 0 || hd <= 0 || ws <= 0 || hs <= 0) return 0;
    int ah = bh < 0 ? -bh : bh;
    rect_t r = rect_intersect((rect_t){xd, yd, wd, hd}, d->clip);
    for (int y = r.y; y < r.y + r.h; y++) {
        int sy = ys + (y - yd) * hs / hd;
        if (sy < 0 || sy >= ah) continue;
        const u32 *row = bits + (size_t)(bh < 0 ? sy : ah - 1 - sy) * (size_t)bw;
        u32 *out = &d->px[y * d->stride];
        for (int x = r.x; x < r.x + r.w; x++) {
            int sx = xs + (x - xd) * ws / wd;
            if (sx >= 0 && sx < bw) out[x] = row[sx] | 0xFF000000;
        }
    }
    return hd;
}

/* ---- input helpers ---- */
static WINAPI int g_GetKeyboardState(u8 *k) {
    for (int i = 0; i < 256; i++) k[i] = u_GetAsyncKeyState(i) ? 0x80 : 0;
    return 1;
}
static WINAPI u32 g_MapVirtualKeyW(u32 code, u32 type) {
    if (type == 0) return vk_scan((int)code) & 0xFF;
    if (type == 1 || type == 3) for (int vk = 1; vk < 256; vk++) if ((vk_scan(vk) & 0xFF) == code && code) return (u32)vk;
    if (type == 2) return code >= 'A' && code <= 'Z' ? code : code >= '0' && code <= '9' ? code : 0;
    return 0;
}
static WINAPI u64 g_GetKeyboardLayout(u32 t) { return 0x040A040A; } /* es-ES */

/* COM: no objects here; say so properly so programs take their fallback path */
static WINAPI u32 g_CoCreateInstance(const void *clsid, void *outer, u32 ctx, const void *iid, void **out) {
    if (out) *out = NULL;
    return 0x80040154; /* REGDB_E_CLASSNOTREG */
}

/* one process: a duplicated handle is the same handle */
static WINAPI int g_DuplicateHandle(u64 sp, u64 h, u64 dp, u64 *out, u32 acc, int inh, u32 opt) { if (out) *out = h; return 1; }
static WINAPI u32 g_no_driver(void) { return 6; } /* MMSYSERR_NODRIVER: no sound in Chamows */

static const struct { const char *name; void *fn; } exports_gui[] = {
    {"DuplicateHandle", g_DuplicateHandle}, {"waveOutOpen", g_no_driver}, {"waveInOpen", g_no_driver},
    {"CoCreateInstance", g_CoCreateInstance},
    {"RegisterClassW", g_RegisterClassW}, {"RegisterClassExW", g_RegisterClassW}, {"GetClassInfoExW", g_GetClassInfoExW},
    {"UnregisterClassW", k_one}, {"UnregisterClassA", k_one}, {"CreateWindowExW", g_CreateWindowExW},
    {"ShowWindow", g_ShowWindow}, {"SetWindowPos", g_SetWindowPos}, {"MoveWindow", g_MoveWindow},
    {"GetWindowLongPtrW", g_GetWindowLongPtrW}, {"SetWindowLongPtrW", g_SetWindowLongPtrW},
    {"GetWindowLongW", g_GetWindowLongPtrW}, {"SetWindowLongW", g_SetWindowLongPtrW},
    {"GetWindowLongPtrA", g_GetWindowLongPtrW}, {"SetWindowLongPtrA", g_SetWindowLongPtrW},
    {"CallWindowProcW", g_CallWindowProcW}, {"CallWindowProcA", g_CallWindowProcW},
    {"SetPropW", g_SetPropW}, {"GetPropW", g_GetPropW}, {"RemovePropW", g_RemovePropW},
    {"GetWindowRect", g_GetWindowRect}, {"ClientToScreen", g_ClientToScreen}, {"ScreenToClient", g_ScreenToClient},
    {"GetCursorPos", g_GetCursorPos}, {"SetCursorPos", k_one}, {"ClipCursor", k_one}, {"GetClipCursor", g_GetClipCursor},
    {"GetForegroundWindow", g_GetForegroundWindow}, {"GetFocus", g_GetForegroundWindow}, {"GetActiveWindow", g_GetForegroundWindow},
    {"SetFocus", g_SetFocus}, {"SetActiveWindow", g_SetFocus}, {"SetForegroundWindow", g_SetForegroundWindow},
    {"BringWindowToTop", g_SetForegroundWindow}, {"GetDesktopWindow", g_GetDesktopWindow}, {"IsIconic", g_IsIconic},
    {"IsZoomed", k_zero}, {"IsWindowVisible", g_IsWindowVisible}, {"GetParent", k_zero},
    {"SetWindowTextW", g_SetWindowTextW}, {"GetWindowTextW", g_GetWindowTextW}, {"GetWindowTextLengthW", k_zero},
    {"GetUpdateRect", g_GetUpdateRect}, {"GetWindowThreadProcessId", g_GetWindowThreadProcessId},
    {"PostMessageW", u_PostMessageA}, {"SendMessageW", u_SendMessageA}, {"PostThreadMessageW", g_PostThreadMessageW},
    {"RegisterWindowMessageA", g_RegisterWindowMessageA}, {"GetDoubleClickTime", g_GetDoubleClickTime},
    {"SetRect", g_SetRect}, {"IsRectEmpty", g_IsRectEmpty}, {"PtInRect", g_PtInRect}, {"IntersectRect", g_IntersectRect},
    {"EnumDisplayMonitors", g_EnumDisplayMonitors}, {"MonitorFromWindow", g_MonitorFrom}, {"MonitorFromPoint", g_MonitorFrom},
    {"MonitorFromRect", g_MonitorFrom}, {"GetMonitorInfoW", g_GetMonitorInfoW}, {"EnumDisplaySettingsW", g_EnumDisplaySettingsW},
    {"EnumDisplaySettingsExW", g_EnumDisplaySettingsExW}, {"EnumDisplayDevicesW", g_EnumDisplayDevicesW},
    {"ChangeDisplaySettingsExW", k_zero}, {"SystemParametersInfoW", g_SystemParametersInfoW},
    {"SystemParametersInfoA", g_SystemParametersInfoW}, {"GetDeviceCaps", g_GetDeviceCaps}, {"CreateDCW", k_zero},
    {"CreateDIBSection", g_CreateDIBSection}, {"StretchDIBits", g_StretchDIBits},
    {"GetKeyboardState", g_GetKeyboardState}, {"MapVirtualKeyW", g_MapVirtualKeyW}, {"GetKeyboardLayout", g_GetKeyboardLayout},
    {"ToUnicode", k_zero}, {"GetMessageExtraInfo", k_zero}, {"TrackMouseEvent", k_one}, {"SetCapture", k_zero},
    {"ReleaseCapture", k_one}, {"GetCapture", k_zero}, {"RegisterRawInputDevices", k_zero}, {"GetRawInputDeviceList", k_zero},
    {"FlashWindowEx", k_one}, {"SetLayeredWindowAttributes", k_one}, {"SetWindowRgn", k_one},
    {"SetWindowsHookExW", k_zero}, {"UnhookWindowsHookEx", k_one}, {"CallNextHookEx", k_zero},
    {"OpenClipboard", k_zero}, {"IsClipboardFormatAvailable", k_zero}, {"GetClipboardSequenceNumber", k_zero},
    {"RegisterDeviceNotificationW", k_zero}, {"UnregisterDeviceNotification", k_one}, {"DragAcceptFiles", k_zero},
};
