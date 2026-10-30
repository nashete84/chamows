/* Bloc de notas: a plain text editor. Text is kept as Latin-1 bytes. */
#include "chamows.h"

#define W 620
#define H 420
#define MAXTEXT 32768
#define TOP 30      /* menu strip */
#define BOTTOM 26   /* status bar */
#define PAD 8

static const cw_api *api;

typedef struct {
    char text[MAXTEXT];
    int len, cur, scroll; /* scroll = first visible line */
    int cw, lh;           /* character width and line height (mono font) */
    int blink;
    int dirty;
} note_t;

static int line_start(note_t *n, int pos) { while (pos > 0 && n->text[pos - 1] != '\n') pos--; return pos; }
static int line_end(note_t *n, int pos) { while (pos < n->len && n->text[pos] != '\n') pos++; return pos; }
static int line_of(note_t *n, int pos) { int l = 0; for (int i = 0; i < pos; i++) if (n->text[i] == '\n') l++; return l; }
static int line_pos(note_t *n, int line) {
    int p = 0;
    while (line > 0 && p < n->len) { if (n->text[p] == '\n') line--; p++; }
    return line > 0 ? n->len : p;
}

/* Latin-1 slice -> UTF-8 for drawing */
static void to_utf8(const char *s, int len, char *out) {
    int o = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) out[o++] = (char)c;
        else { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
}

static int visible_lines(note_t *n) { return (H - TOP - BOTTOM - PAD) / n->lh; }

static void keep_visible(note_t *n) {
    int l = line_of(n, n->cur);
    if (l < n->scroll) n->scroll = l;
    if (l >= n->scroll + visible_lines(n)) n->scroll = l - visible_lines(n) + 1;
}

static void draw(cw_window *w) {
    note_t *n = api->user(w);
    api->clear(w, 0xFFFFFFFF);
    /* menu strip like Windows 11 Notepad */
    api->rect(w, 0, 0, W, TOP, 0xFFF3F3F3);
    const char *menu[3] = {"Archivo", "Editar", "Ver"};
    int x = 12;
    for (int i = 0; i < 3; i++) { x += api->text(w, x, 8, menu[i], 0xFF1A1A1A, CW_FONT_UI) + 22; }
    api->rect(w, 0, TOP - 1, W, 1, 0xFFE5E5E5);

    char line[400];
    int maxcols = (W - 2 * PAD) / n->cw;
    int p = line_pos(n, n->scroll);
    int curline = line_of(n, n->cur), curcol = n->cur - line_start(n, n->cur);
    for (int l = 0; l < visible_lines(n) && p <= n->len; l++) {
        int e = line_end(n, p);
        int len = e - p;
        if (len > maxcols) len = maxcols;
        if (len > 190) len = 190;
        to_utf8(n->text + p, len, line);
        api->text(w, PAD, TOP + PAD / 2 + l * n->lh, line, 0xFF1A1A1A, CW_FONT_MONO);
        if (n->scroll + l == curline && n->blink == 0 && curcol <= maxcols)
            api->rect(w, PAD + curcol * n->cw, TOP + PAD / 2 + l * n->lh, 1, n->lh, 0xFF1A1A1A);
        if (e >= n->len) break;
        p = e + 1;
    }
    /* status bar */
    api->rect(w, 0, H - BOTTOM, W, BOTTOM, 0xFFF3F3F3);
    api->rect(w, 0, H - BOTTOM, W, 1, 0xFFE5E5E5);
    char st[64];
    api->format(st, sizeof st, "L\xc3\xadn. %d, col. %d", curline + 1, curcol + 1);
    api->text(w, 12, H - BOTTOM + 6, st, 0xFF404040, CW_FONT_UI);
    api->format(st, sizeof st, "%d caracteres", n->len);
    api->text(w, 200, H - BOTTOM + 6, st, 0xFF404040, CW_FONT_UI);
    api->text(w, W - 160, H - BOTTOM + 6, "Windows (CRLF)   UTF-8", 0xFF404040, CW_FONT_UI);
    api->invalidate(w);
}

static void insert(note_t *n, char c) {
    if (n->len >= MAXTEXT - 1) return;
    for (int i = n->len; i > n->cur; i--) n->text[i] = n->text[i - 1];
    n->text[n->cur++] = c;
    n->len++;
    n->dirty = 1;
}
static void erase(note_t *n, int pos) {
    if (pos < 0 || pos >= n->len) return;
    for (int i = pos; i < n->len - 1; i++) n->text[i] = n->text[i + 1];
    n->len--;
    n->dirty = 1;
}

static void move_vert(note_t *n, int d) {
    int col = n->cur - line_start(n, n->cur);
    int l = line_of(n, n->cur) + d;
    if (l < 0) { n->cur = 0; return; }
    int s = line_pos(n, l);
    if (s >= n->len && l > line_of(n, n->len)) { n->cur = n->len; return; }
    int e = line_end(n, s);
    n->cur = s + col < e ? s + col : e;
}

static void on_event(cw_window *w, const cw_event *e) {
    note_t *n = api->user(w);
    if (e->type == CW_TICK) { n->blink ^= 1; draw(w); return; }
    if (e->type == CW_MOUSE_DOWN && e->y > TOP && e->y < H - BOTTOM) {
        int l = n->scroll + (e->y - TOP - PAD / 2) / n->lh;
        int s = line_pos(n, l);
        if (l > line_of(n, n->len)) { n->cur = n->len; }
        else {
            int col = (e->x - PAD + n->cw / 2) / n->cw;
            if (col < 0) col = 0;
            int le = line_end(n, s);
            n->cur = s + col < le ? s + col : le;
        }
    } else if (e->type == CW_KEY) {
        int k = e->key;
        switch (k) {
        case CW_KEY_LEFT: if (n->cur > 0) n->cur--; break;
        case CW_KEY_RIGHT: if (n->cur < n->len) n->cur++; break;
        case CW_KEY_UP: move_vert(n, -1); break;
        case CW_KEY_DOWN: move_vert(n, 1); break;
        case CW_KEY_PGUP: for (int i = 0; i < visible_lines(n); i++) move_vert(n, -1); break;
        case CW_KEY_PGDN: for (int i = 0; i < visible_lines(n); i++) move_vert(n, 1); break;
        case CW_KEY_HOME: n->cur = line_start(n, n->cur); break;
        case CW_KEY_END: n->cur = line_end(n, n->cur); break;
        case CW_KEY_BACKSPACE: if (n->cur > 0) { n->cur--; erase(n, n->cur); } break;
        case CW_KEY_DELETE: erase(n, n->cur); break;
        case CW_KEY_ENTER: insert(n, '\n'); break;
        case CW_KEY_TAB: for (int i = 0; i < 4; i++) insert(n, ' '); break;
        default:
            if ((k >= 32 && k < 127) || (k >= 160 && k < 256)) insert(n, (char)k);
            else return;
        }
    } else return;
    n->blink = 0;
    keep_visible(n);
    api->set_title(w, n->dirty ? "*Sin t\xc3\xadtulo: Bloc de notas" : "Sin t\xc3\xadtulo: Bloc de notas");
    draw(w);
}

int app_main(const cw_api *a) {
    api = a;
    note_t *n = api->alloc(sizeof *n);
    if (!n) return 1;
    n->cw = api->text_width("M", CW_FONT_MONO);
    n->lh = api->font_height(CW_FONT_MONO) + 2;
    cw_window *w = api->window("Sin t\xc3\xadtulo: Bloc de notas", W, H, on_event, n);
    if (!w) return 1;
    api->set_timer(w, 530);
    draw(w);
    return 0;
}
