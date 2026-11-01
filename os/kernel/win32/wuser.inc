/* More of user32 for Qt's Windows platform plugin: DPI (always 96), display
 * configuration (not offered), menus and carets (accepted, not drawn), system
 * colours and metrics, keyboard layout (US). */

static WINAPI int y_GetSystemMetrics(int i) {
    switch (i) {
    case 0: case 16: case 78: return SCREEN_W;                 /* screen, full-screen client, virtual screen */
    case 1: case 79: return SCREEN_H;
    case 17: return SCREEN_H - TASKBAR_H - 31;
    case 2: case 3: case 20: case 21: return 17;               /* scroll bar sizes */
    case 4: return 31;                                         /* caption */
    case 5: case 6: return 1;                                  /* border */
    case 7: case 8: case 32: case 33: return 8;                /* frame */
    case 11: case 12: return 32;                               /* icon */
    case 13: case 14: return 32;                               /* cursor */
    case 15: return 20;                                        /* menu bar */
    case 28: case 29: return 112;                              /* minimum track size */
    case 36: case 37: return 4;                                /* double-click area */
    case 43: return 3;                                         /* mouse buttons */
    case 49: case 50: return 16;                               /* small icon */
    case 59: case 60: return SCREEN_W;                         /* maximum track size */
    case 61: case 62: return SCREEN_W;                         /* maximized */
    case 68: case 69: return 4;                                /* drag distance */
    case 80: return 1;                                         /* monitors */
    case 92: return 4;                                         /* padded border */
    }
    return 0;
}
static WINAPI int y_GetSystemMetricsForDpi(int i, u32 dpi) { return y_GetSystemMetrics(i); }

/* LOGFONTW: height, width, escapement, orientation, weight, 8 bytes of flags, 32-unit face name */
static void fill_logfont(u8 *lf, int height, int weight) {
    memset(lf, 0, 92);
    *(i32 *)lf = height;
    *(i32 *)(lf + 16) = weight;
    lf[23] = 1; /* DEFAULT_CHARSET */
    lf[26] = 5; /* CLEARTYPE_QUALITY */
    a2w("DejaVu Sans", (u16 *)(lf + 28), 32);
}
static WINAPI int y_SystemParametersInfoW(u32 act, u32 ui, void *pv, u32 ini) {
    u8 *p = pv;
    switch (act) {
    case 0x29: /* SPI_GETNONCLIENTMETRICS */
        if (!p) return 0;
        memset(p + 4, 0, *(u32 *)p > 4 ? *(u32 *)p - 4 : 0);
        *(i32 *)(p + 4) = 1;  *(i32 *)(p + 8) = 17; *(i32 *)(p + 12) = 17; *(i32 *)(p + 16) = 31; *(i32 *)(p + 20) = 31;
        fill_logfont(p + 24, -12, 700);               /* caption */
        *(i32 *)(p + 116) = 22; *(i32 *)(p + 120) = 22;
        fill_logfont(p + 124, -12, 400);              /* small caption */
        *(i32 *)(p + 216) = 19; *(i32 *)(p + 220) = 19;
        fill_logfont(p + 224, -12, 400);              /* menu */
        fill_logfont(p + 316, -12, 400);              /* status */
        fill_logfont(p + 408, -12, 400);              /* message */
        return 1;
    case 0x1F: if (p) fill_logfont(p, -12, 400); return 1; /* SPI_GETICONTITLELOGFONT */
    case 0x68: case 0x6A: if (p) *(u32 *)p = 3; return 1;  /* wheel scroll lines / chars */
    case 0x1024: case 0x1022: case 0x1042: case 0x100A: case 0x1012: if (p) *(u32 *)p = 0; return 1; /* effects off */
    case 0x200E: if (p) *(u32 *)p = 400; return 1;         /* SPI_GETFOCUSBORDER... harmless */
    case 0x0C: if (p) *(u32 *)p = 0; return 1;             /* SPI_GETKEYBOARDCUES */
    case 0x16: if (p) *(u32 *)p = 530; return 1;           /* keyboard delay */
    case 0x2008: if (p) *(u32 *)p = 1; return 1;           /* caret width */
    case 0x4A: if (p) *(u32 *)p = 0; return 1;             /* SPI_GETFONTSMOOTHING... */
    }
    return g_SystemParametersInfoW(act, ui, pv, ini);
}
static WINAPI int y_SystemParametersInfoForDpi(u32 act, u32 ui, void *pv, u32 ini, u32 dpi) { return y_SystemParametersInfoW(act, ui, pv, ini); }
static WINAPI u32 y_GetSysColor(int i) {
    static const u32 c[31] = {
        0xC8C8C8, 0x000000, 0xD1B499, 0xDBCDBF, 0xF0F0F0, 0xFFFFFF, 0x646464, 0x000000, 0x000000, 0x000000,
        0xB4B4B4, 0xFCF7F4, 0xABABAB, 0xD77800, 0xFFFFFF, 0xF0F0F0, 0xA0A0A0, 0x6D6D6D, 0x000000, 0x000000,
        0xFFFFFF, 0x696969, 0xE3E3E3, 0x000000, 0xE1FFFF, 0x000000, 0xCC6600, 0xEAD1B9, 0xF2E4D7, 0xD77800, 0xF0F0F0,
    };
    return i >= 0 && i < 31 ? c[i] : 0;
}
static WINAPI u64 y_GetSysColorBrush(int i) { return (u64)g_CreateSolidBrush(y_GetSysColor(i)); }

/* DPI awareness: one answer, "per monitor" at 96 */
static WINAPI i64 y_GetThreadDpiAwarenessContext(void) { return -3; } /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE */
static WINAPI int y_AreDpiAwarenessContextsEqual(i64 a, i64 b) { return a == b; }
static WINAPI u32 y_GetDpi(void) { return 96; }
static WINAPI u32 y_GetDpiForMonitor(void *m, int type, u32 *x, u32 *y) { *x = *y = 96; return 0; }
static WINAPI int y_AdjustWindowRectExForDpi(RECT *r, u32 style, int menu, u32 ex, u32 dpi) { return u_AdjustWindowRect(r, style, menu); }

/* keyboard */
static WINAPI int y_GetKeyboardLayoutNameW(u16 *b) { a2w("00000409", b, 9); return 1; }
static WINAPI int y_GetKeyboardLayoutList(int n, u64 *l) { if (n >= 1 && l) l[0] = 0x04090409; return 1; }

/* windows */
static WINAPI void *y_GetAncestor(void *h, u32 f) { return h; } /* every window here is top-level */
static WINAPI void *y_GetWindow(void *h, u32 cmd) { return NULL; }
static WINAPI int y_MapWindowPoints(void *from, void *to, i32 *p, u32 n) {
    i32 d[2] = {0, 0};
    if (from) { i32 q[2] = {0, 0}; g_ClientToScreen(from, q); d[0] += q[0]; d[1] += q[1]; }
    if (to) { i32 q[2] = {0, 0}; g_ClientToScreen(to, q); d[0] -= q[0]; d[1] -= q[1]; }
    for (u32 i = 0; i < n; i++) { p[2 * i] += d[0]; p[2 * i + 1] += d[1]; }
    return (int)((u32)(u16)d[0] | (u32)(u16)d[1] << 16);
}
static WINAPI int y_GetWindowPlacement(void *h, u32 *wp) {
    RECT r;
    g_GetWindowRect(h, &r);
    memset(wp + 1, 0, 40);
    wp[0] = 44; wp[2] = 1; /* SW_SHOWNORMAL */
    memcpy(wp + 7, &r, 16);
    return 1;
}
static WINAPI i64 y_SendMessageTimeoutW(void *h, u32 m, u64 wp, i64 lp, u32 fl, u32 t, u64 *res) {
    i64 r = u_SendMessageA(h, m, wp, lp);
    if (res) *res = (u64)r;
    return 1;
}
static WINAPI u32 y_GetMessageTime(void) { return ticks_ms; }
static WINAPI u32 y_GetCaretBlinkTime(void) { return 530; }
static WINAPI int y_GetCursorInfo(u32 *ci) { i32 p[2]; g_GetCursorPos(p); ci[1] = 1; *(u64 *)(ci + 2) = 1; memcpy(ci + 4, p, 8); return 1; }
static u64 next_fake = 0x5000;
static WINAPI u64 y_fake_handle(void) { return next_fake += 8; }
static WINAPI u32 y_RegisterClipboardFormatW(const u16 *n) { return next_wmsg++; }
static WINAPI u32 y_RegisterWindowMessageW(const u16 *n) { return next_wmsg++; }

/* the command line after the program's name, for WinMain */
static WINAPI char *y__get_narrow_winmain_command_line(void) {
    char *c = cur->cmdline;
    int q = 0;
    while (*c && (q || (*c != ' ' && *c != '\t'))) { if (*c == '"') q = !q; c++; }
    while (*c == ' ' || *c == '\t') c++;
    return c;
}
static WINAPI u16 *y__get_wide_winmain_command_line(void) {
    u16 *c = cur->wcmdline;
    int q = 0;
    while (*c && (q || (*c != ' ' && *c != '\t'))) { if (*c == '"') q = !q; c++; }
    while (*c == ' ' || *c == '\t') c++;
    return c;
}

static const struct { const char *name; void *fn; } exports_user[] = {
    {"GetSystemMetrics", y_GetSystemMetrics}, {"GetSystemMetricsForDpi", y_GetSystemMetricsForDpi},
    {"SystemParametersInfoW", y_SystemParametersInfoW}, {"SystemParametersInfoForDpi", y_SystemParametersInfoForDpi},
    {"GetSysColor", y_GetSysColor}, {"GetSysColorBrush", y_GetSysColorBrush},
    {"GetThreadDpiAwarenessContext", y_GetThreadDpiAwarenessContext}, {"GetWindowDpiAwarenessContext", y_GetThreadDpiAwarenessContext},
    {"IsValidDpiAwarenessContext", k_one}, {"AreDpiAwarenessContextsEqual", y_AreDpiAwarenessContextsEqual},
    {"SetProcessDpiAwarenessContext", k_one}, {"SetThreadDpiAwarenessContext", y_GetThreadDpiAwarenessContext},
    {"GetDpiForWindow", y_GetDpi}, {"GetDpiForSystem", y_GetDpi}, {"GetDpiForMonitor", y_GetDpiForMonitor},
    {"EnableNonClientDpiScaling", k_one}, {"AdjustWindowRectExForDpi", y_AdjustWindowRectExForDpi},
    {"GetDisplayConfigBufferSizes", m_fail_code}, {"QueryDisplayConfig", m_fail_code}, {"DisplayConfigGetDeviceInfo", m_fail_code},
    {"GetKeyboardLayoutNameW", y_GetKeyboardLayoutNameW}, {"GetKeyboardLayoutList", y_GetKeyboardLayoutList},
    {"GetAncestor", y_GetAncestor}, {"GetWindow", y_GetWindow}, {"IsChild", k_zero}, {"IsWindowEnabled", k_one},
    {"MapWindowPoints", y_MapWindowPoints}, {"GetWindowPlacement", y_GetWindowPlacement}, {"SetWindowPlacement", k_one},
    {"SendMessageTimeoutW", y_SendMessageTimeoutW}, {"GetMessageTime", y_GetMessageTime}, {"GetCursorInfo", y_GetCursorInfo},
    {"WindowFromPoint", k_zero}, {"ChildWindowFromPointEx", k_zero}, {"SetParent", k_zero}, {"FindWindowA", k_zero},
    {"FindWindowW", k_zero}, {"IsHungAppWindow", k_zero}, {"AttachThreadInput", k_one},
    {"AddClipboardFormatListener", k_one}, {"RemoveClipboardFormatListener", k_one}, {"SetClipboardViewer", k_zero},
    {"ChangeClipboardChain", k_one}, {"RegisterClipboardFormatW", y_RegisterClipboardFormatW},
    {"RegisterWindowMessageW", y_RegisterWindowMessageW}, {"GetClipboardFormatNameW", k_zero},
    {"RegisterPowerSettingNotification", y_fake_handle}, {"UnregisterPowerSettingNotification", k_one},
    {"ChangeWindowMessageFilter", k_one}, {"ChangeWindowMessageFilterEx", k_one}, {"SetDisplayAutoRotationPreferences", k_one},
    {"RegisterTouchWindow", k_one}, {"UnregisterTouchWindow", k_one}, {"IsTouchWindow", k_zero}, {"GetPointerType", k_zero},
    {"SkipPointerFrameMessages", k_one}, {"SendInput", k_zero}, {"ToAscii", k_zero},
    /* menus and the caret: accepted, never shown */
    {"CreateMenu", y_fake_handle}, {"CreatePopupMenu", y_fake_handle}, {"DestroyMenu", k_one}, {"AppendMenuW", k_one},
    {"InsertMenuW", k_one}, {"ModifyMenuW", k_one}, {"RemoveMenu", k_one}, {"EnableMenuItem", k_zero}, {"HiliteMenuItem", k_one},
    {"SetMenu", k_one}, {"GetMenu", k_zero}, {"DrawMenuBar", k_one}, {"GetSystemMenu", y_fake_handle},
    {"SetMenuItemInfoW", k_one}, {"GetMenuItemInfoW", k_zero}, {"SetMenuDefaultItem", k_one},
    {"TrackPopupMenu", k_zero}, {"TrackPopupMenuEx", k_zero},
    {"CreateCaret", k_one}, {"DestroyCaret", k_one}, {"SetCaretPos", k_one}, {"ShowCaret", k_one}, {"HideCaret", k_one},
    {"GetCaretBlinkTime", y_GetCaretBlinkTime},
    /* cursors and icons: Chamows draws its own pointer */
    {"GetCursor", y_fake_handle}, {"CreateCursor", y_fake_handle}, {"CreateIconIndirect", y_fake_handle},
    {"DestroyCursor", k_one}, {"DestroyIcon", k_one}, {"GetIconInfo", k_zero}, {"DrawIconEx", k_one}, {"LoadImageW", y_fake_handle},
    {"UpdateLayeredWindow", k_one}, {"UpdateLayeredWindowIndirect", k_one},
    {"_get_narrow_winmain_command_line", y__get_narrow_winmain_command_line},
    {"_get_wide_winmain_command_line", y__get_wide_winmain_command_line},
};
