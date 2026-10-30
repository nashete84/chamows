/* Chamows SDK: everything an app can use.
 *
 * An app is a 32-bit position-independent ELF with one entry point:
 *
 *     int app_main(const cw_api *api);
 *
 * app_main creates one or more windows and returns 0. After that, Chamows
 * calls the window's event function for clicks, keys and timer ticks. The
 * app draws into its window with the api functions and calls invalidate()
 * to show the changes. The app's memory is released when its last window
 * closes.
 */
#ifndef CHAMOWS_SDK_H
#define CHAMOWS_SDK_H

#define CW_API_VERSION 1

typedef struct cw_window cw_window;

enum cw_event_type {
    CW_MOUSE_DOWN = 1, /* x, y inside the window; button: 1 left, 2 right */
    CW_MOUSE_UP,
    CW_MOUSE_MOVE,     /* only while a button is held, or over the window */
    CW_KEY,            /* key: Latin-1 character or CW_KEY_* */
    CW_TICK,           /* timer set with set_timer */
    CW_CLOSE,          /* window is closing: free your resources */
};

enum {
    CW_KEY_BACKSPACE = 8, CW_KEY_TAB = 9, CW_KEY_ENTER = 10,
    CW_KEY_ESC = 0x100, CW_KEY_UP, CW_KEY_DOWN, CW_KEY_LEFT, CW_KEY_RIGHT,
    CW_KEY_HOME, CW_KEY_END, CW_KEY_DELETE, CW_KEY_PGUP, CW_KEY_PGDN,
};

enum { CW_FONT_UI = 0, CW_FONT_BOLD = 1, CW_FONT_TITLE = 2, CW_FONT_MONO = 3 };

typedef struct {
    int type;
    int x, y;
    int key;
    int button;
} cw_event;

typedef void (*cw_event_fn)(cw_window *w, const cw_event *ev);

/* Colors are 0xAARRGGBB. Alpha below 0xFF blends. */
typedef struct cw_api {
    unsigned version;

    /* windows */
    cw_window *(*window)(const char *title, int width, int height, cw_event_fn on_event, void *user);
    void *(*user)(cw_window *w);
    void (*invalidate)(cw_window *w);
    void (*close)(cw_window *w);
    void (*set_timer)(cw_window *w, int every_ms); /* 0 stops it */
    void (*set_title)(cw_window *w, const char *title);

    /* drawing (coordinates inside the window) */
    void (*clear)(cw_window *w, unsigned color);
    void (*rect)(cw_window *w, int x, int y, int width, int height, unsigned color);
    void (*round_rect)(cw_window *w, int x, int y, int width, int height, int radius, unsigned color);
    void (*line)(cw_window *w, int x0, int y0, int x1, int y1, int thickness, unsigned color);
    void (*circle)(cw_window *w, int cx, int cy, int radius, unsigned color);
    int (*text)(cw_window *w, int x, int y, const char *utf8, unsigned color, int font); /* returns width */
    int (*text_width)(const char *utf8, int font);
    int (*font_height)(int font);
    void (*pixels)(cw_window *w, int x, int y, int width, int height, const unsigned *argb);

    /* system */
    void *(*alloc)(unsigned bytes); /* zeroed */
    void (*free)(void *p);
    unsigned (*ticks_ms)(void);
    unsigned (*random)(void);
    void (*clock)(int *hour, int *minute, int *second);
    void (*date)(int *day, int *month, int *year);
    int (*format)(char *buf, int size, const char *fmt, int a, int b, int c); /* %d %s(int as char*) %02d */
    void (*log)(const char *msg);
} cw_api;

int app_main(const cw_api *api);

/* small helpers every app gets from cwlib.c */
void *memset(void *d, int c, unsigned n);
void *memcpy(void *d, const void *s, unsigned n);
unsigned strlen(const char *s);

#endif
