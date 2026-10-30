/* Chamows USB: keyboards, mice and tablets (HID class).
 * Keyboards and mice use the simple "boot" reports. Other pointing devices
 * (QEMU/UTM tablets, most modern mice) are read with a small HID report
 * descriptor parser that finds the X, Y, wheel and button fields. */
#include "usb.h"

/* ---- keyboard: HID usage -> PS/2 set-1 scancode (0x100 = needs the E0 prefix) ---- */
static const u16 usage_to_sc[0x66] = {
    [0x04] = 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, /* a-m */
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,          /* n-z */
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,                              /* 1-0 */
    0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29,      /* enter..` */
    0x33, 0x34, 0x35, 0x3A,                                                                  /* , . / caps */
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,                  /* F1-F12 */
    0, 0x46, 0,                                                                              /* prtsc scroll pause */
    0x152, 0x147, 0x149, 0x153, 0x14F, 0x151, 0x14D, 0x14B, 0x150, 0x148,                    /* ins..up */
    0x45, 0x135, 0x37, 0x4A, 0x4E, 0x11C,                                                    /* numlock kp/ * - + enter */
    0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52, 0x53,                        /* kp1-9 0 . */
    0x56, 0x15D,                                                                             /* < > (Spanish), menu */
};
/* modifier bits: LCtrl LShift LAlt LGui RCtrl RShift RAlt(AltGr) RGui */
static const u16 mod_sc[8] = {0x1D, 0x2A, 0x38, 0x15B, 0x11D, 0x36, 0x138, 0x15C};

static void feed(u16 sc, int down) {
    cli();
    if (sc & 0x100) kbd_scancode(0xE0);
    kbd_scancode((u8)((sc & 0xFF) | (down ? 0 : 0x80)));
    sti();
}

typedef struct { u8 prev[8]; } kbd_t;
static u16 repeat_sc;
static u32 repeat_at;

static int has(const u8 *keys, u8 k) {
    for (int i = 2; i < 8; i++) if (keys[i] == k) return 1;
    return 0;
}

static void kbd_report(void *ctx, const u8 *r, u32 len) {
    kbd_t *k = ctx;
    if (len < 8 || r[2] == 1) return; /* too short, or "too many keys" */
    static int first = 1;
    if (first && r[2]) { first = 0; klog("usb: primera tecla desde el teclado USB\n"); }
    u8 changed = r[0] ^ k->prev[0];
    for (int i = 0; i < 8; i++)
        if (changed & (1 << i)) feed(mod_sc[i], r[0] & (1 << i));
    for (int i = 2; i < 8; i++) {
        u8 u = k->prev[i];
        if (u >= 4 && u < 0x66 && !has(r, u) && usage_to_sc[u]) {
            feed(usage_to_sc[u], 0);
            if (repeat_sc == usage_to_sc[u]) repeat_sc = 0;
        }
    }
    for (int i = 2; i < 8; i++) {
        u8 u = r[i];
        if (u >= 4 && u < 0x66 && !has(k->prev, u) && usage_to_sc[u]) {
            feed(usage_to_sc[u], 1);
            repeat_sc = usage_to_sc[u];
            repeat_at = ticks_ms + 500;
        }
    }
    memcpy(k->prev, r, 8);
}

/* USB keyboards don't repeat keys themselves: 500 ms delay, then 30 per second */
void hid_tick(void) {
    if (repeat_sc && (i32)(ticks_ms - repeat_at) >= 0) {
        feed(repeat_sc, 1);
        repeat_at = ticks_ms + 33;
    }
}

/* ---- pointers ---- */
typedef struct { u16 bit, size; u8 found, absolute, is_signed; i32 lmin, lmax; } field_t;
typedef struct {
    int boot;            /* boot mouse report: buttons, dx, dy */
    u8 report_id;        /* 0 = reports have no id byte */
    field_t x, y, wheel;
    u16 btn_bit, btn_count;
} ptr_t;

static i32 get_field(const u8 *r, u32 len, const field_t *f) {
    u32 v = 0;
    for (u32 i = 0; i < f->size && i < 32; i++) {
        u32 bit = f->bit + i;
        if (bit / 8 >= len) break;
        if (r[bit / 8] & (1u << (bit % 8))) v |= 1u << i;
    }
    if (f->is_signed && f->size < 32 && (v & (1u << (f->size - 1)))) v |= ~0u << f->size;
    return (i32)v;
}

static void ptr_report(void *ctx, const u8 *r, u32 len) {
    ptr_t *p = ctx;
    input_event_t ev = { .type = EV_MOUSE };
    static int first = 1;
    if (first) { first = 0; klog("usb: primer movimiento desde el ratón USB\n"); }
    if (p->boot) {
        if (len < 3) return;
        ev.buttons = r[0] & 7;
        ev.dx = (i8)r[1];
        ev.dy = (i8)r[2];
    } else {
        if (p->report_id) {
            if (len < 1 || r[0] != p->report_id) return;
        }
        for (u32 i = 0; i < p->btn_count && i < 3; i++) {
            u32 bit = p->btn_bit + i;
            if (bit / 8 < len && (r[bit / 8] & (1u << (bit % 8)))) ev.buttons |= (u8)(1u << i);
        }
        i32 x = get_field(r, len, &p->x), y = get_field(r, len, &p->y);
        if (p->x.absolute) {
            i32 span = p->x.lmax - p->x.lmin, spany = p->y.lmax - p->y.lmin;
            if (span <= 0 || spany <= 0) return;
            ev.absolute = 1;
            ev.dx = (i32)((u32)(x - p->x.lmin) * 32767u / (u32)span);
            ev.dy = (i32)((u32)(y - p->y.lmin) * 32767u / (u32)spany);
        } else {
            ev.dx = x;
            ev.dy = y;
        }
    }
    input_push(&ev);
}

/* Reads a HID report descriptor and finds the pointer fields of the first report with X/Y. */
static int parse_report(const u8 *rd, u32 len, ptr_t *p) {
    u32 page = 0, rsize = 0, rcount = 0, rid = 0, bit = 0;
    i32 lmin = 0, lmax = 0;
    u32 usages[16], n_usages = 0, umin = 0, umax = 0;
    u32 want_id = 0xFFFFFFFF;
    for (u32 i = 0; i < len;) {
        u8 b = rd[i];
        if (b == 0xFE) { i += 3u + (i + 1 < len ? rd[i + 1] : 0); continue; } /* long item */
        u32 sz = (b & 3) == 3 ? 4 : (b & 3), type = (b >> 2) & 3, tag = b >> 4;
        u32 v = 0;
        for (u32 j = 0; j < sz && i + 1 + j < len; j++) v |= (u32)rd[i + 1 + j] << (8 * j);
        i32 sv = sz == 1 ? (i8)v : sz == 2 ? (i16)v : (i32)v;
        i += 1 + sz;
        if (type == 1) { /* global */
            if (tag == 0) page = v;
            else if (tag == 1) lmin = sv;
            else if (tag == 2) lmax = (lmin < 0) ? sv : (i32)v;
            else if (tag == 7) rsize = v;
            else if (tag == 8) { rid = v; bit = 8; }
            else if (tag == 9) rcount = v;
        } else if (type == 2) { /* local */
            if (tag == 0 && n_usages < 16) usages[n_usages++] = sz == 4 ? v : (page << 16) | v;
            else if (tag == 1) umin = sz == 4 ? v : (page << 16) | v;
            else if (tag == 2) umax = sz == 4 ? v : (page << 16) | v;
        } else if (type == 0) { /* main */
            if (tag == 8 && (want_id == 0xFFFFFFFF || want_id == rid)) { /* input */
                int constant = v & 1, absolute = !(v & 4);
                for (u32 k = 0; k < rcount; k++) {
                    u32 u = k < n_usages ? usages[k] : (umin ? umin + k : (n_usages ? usages[n_usages - 1] : 0));
                    if (umax && umin + k > umax) u = 0;
                    if (constant) break;
                    field_t f = { (u16)(bit + k * rsize), (u16)rsize, 1, (u8)absolute, lmin < 0, lmin, lmax };
                    if (u == 0x10030 && !p->x.found) { p->x = f; want_id = rid; }
                    else if (u == 0x10031 && !p->y.found) p->y = f;
                    else if (u == 0x10038 && !p->wheel.found) p->wheel = f;
                    else if ((u >> 16) == 9 && !p->btn_count) { p->btn_bit = f.bit; p->btn_count = (u16)rcount; }
                }
            }
            if (tag == 8) bit += rsize * rcount;
            n_usages = umin = umax = 0;
        }
    }
    p->report_id = (u8)(want_id == 0xFFFFFFFF ? 0 : want_id);
    return p->x.found && p->y.found;
}

int hid_attach(usb_dev_t *d, const usb_iface_desc_t *ifc, const u8 *cfg, u32 cfg_len) {
    /* find this interface's HID descriptor and interrupt IN endpoint */
    const u8 *start = (const u8 *)ifc;
    u16 report_len = 0;
    const usb_ep_desc_t *in = NULL;
    for (u32 off = (u32)(start - cfg) + ifc->len; off + 2 <= cfg_len && cfg[off] >= 2; off += cfg[off]) {
        if (cfg[off + 1] == 4) break; /* next interface */
        if (cfg[off + 1] == 0x21 && cfg[off] >= 9) report_len = (u16)(cfg[off + 7] | (cfg[off + 8] << 8));
        if (cfg[off + 1] == 5) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)(cfg + off);
            if ((e->attributes & 3) == 3 && (e->address & 0x80) && !in) in = e;
        }
    }
    if (!in) return -1;
    int boot = ifc->subcls == 1;
    if (boot && ifc->proto == 1) { /* keyboard */
        usb_control(d, 0x21, 0x0B, 0, ifc->number, NULL, 0); /* SET_PROTOCOL boot */
        usb_control(d, 0x21, 0x0A, 0, ifc->number, NULL, 0); /* SET_IDLE: only on change */
        if (usb_configure_ep(d, in) != 0) return -1;
        usb_interrupt_start(d, in->address, 8, kbd_report, kzalloc(sizeof(kbd_t)));
        klog("usb:   teclado listo\n");
        return 0;
    }
    ptr_t *p = kzalloc(sizeof *p);
    int ok = 0;
    if (report_len && report_len <= 1024) {
        u8 *rd = kmalloc(report_len);
        if (rd && usb_control(d, 0x81, 6, 0x2200, ifc->number, rd, report_len) == 0) ok = parse_report(rd, report_len, p);
        kfree(rd);
    }
    if (!ok && boot && ifc->proto == 2) { /* fall back to the boot mouse report */
        usb_control(d, 0x21, 0x0B, 0, ifc->number, NULL, 0);
        memset(p, 0, sizeof *p);
        p->boot = 1;
        ok = 1;
    }
    if (!ok) { kfree(p); klog("usb:   dispositivo HID sin ejes X/Y, ignorado\n"); return -1; }
    usb_control(d, 0x21, 0x0A, 0, ifc->number, NULL, 0);
    if (usb_configure_ep(d, in) != 0) { kfree(p); return -1; }
    u16 len = in->max_packet & 0x7FF;
    usb_interrupt_start(d, in->address, len ? len : 8, ptr_report, p);
    klog("usb:   %s listo\n", p->boot ? "ratón" : p->x.absolute ? "tableta (ratón absoluto)" : "ratón");
    return 0;
}
