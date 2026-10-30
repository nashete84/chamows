/* Chamows kernel: PS/2 keyboard (Spanish layout) and mouse. */
#include "kernel.h"

#define QSIZE 256
static input_event_t queue[QSIZE];
static volatile u32 q_head, q_tail;

static void push(const input_event_t *ev) {
    u32 next = (q_head + 1) % QSIZE;
    if (next == q_tail) return; /* full: drop */
    queue[q_head] = *ev;
    q_head = next;
}

void input_push(const input_event_t *ev) {
    cli();
    push(ev);
    sti();
}

int input_poll(input_event_t *ev) {
    cli();
    if (q_tail == q_head) { sti(); return 0; }
    *ev = queue[q_tail];
    q_tail = (q_tail + 1) % QSIZE;
    sti();
    return 1;
}

/* ---- keyboard: scancode set 1, Spanish (ES) layout, Latin-1 output ---- */
static const u8 es_normal[0x59] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '\'', 0xA1, 8, 9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '`', '+', 10, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', 0xF1, 0xB4, 0xBA, 0, 0xE7, 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '-', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, '<', 0, 0,
};
static const u8 es_shift[0x59] = {
    0, 27, '!', '"', 0xB7, '$', '%', '&', '/', '(', ')', '=', '?', 0xBF, 8, 9,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '^', '*', 10, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', 0xD1, 0xA8, 0xAA, 0, 0xC7, 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', ';', ':', '_', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, '>', 0, 0,
};
static u8 shift, ctrl, altgr, caps, extended, dead;

static u16 apply_dead(u8 d, u16 c) {
    static const char base[] = "aeiouAEIOU";
    static const u8 acute[] = {0xE1, 0xE9, 0xED, 0xF3, 0xFA, 0xC1, 0xC9, 0xCD, 0xD3, 0xDA};
    static const u8 grave[] = {0xE0, 0xE8, 0xEC, 0xF2, 0xF9, 0xC0, 0xC8, 0xCC, 0xD2, 0xD9};
    static const u8 diaer[] = {0xE4, 0xEB, 0xEF, 0xF6, 0xFC, 0xC4, 0xCB, 0xCF, 0xD6, 0xDC};
    for (int i = 0; base[i]; i++)
        if (base[i] == c) return d == 0xB4 ? acute[i] : d == '`' ? grave[i] : diaer[i];
    return c;
}

static void key_event(u16 key, u8 pressed) {
    input_event_t ev = { .type = EV_KEY, .pressed = pressed, .key = key };
    push(&ev);
}

void kbd_scancode(u8 sc) {
    if (sc == 0xE0) { extended = 1; return; }
    u8 released = sc & 0x80;
    sc &= 0x7F;
    if (extended) {
        extended = 0;
        u16 k = 0;
        switch (sc) {
        case 0x48: k = KEY_UP; break;
        case 0x50: k = KEY_DOWN; break;
        case 0x4B: k = KEY_LEFT; break;
        case 0x4D: k = KEY_RIGHT; break;
        case 0x47: k = KEY_HOME; break;
        case 0x4F: k = KEY_END; break;
        case 0x53: k = KEY_DELETE; break;
        case 0x49: k = KEY_PGUP; break;
        case 0x51: k = KEY_PGDN; break;
        case 0x5B: case 0x5C: k = KEY_SUPER; break;
        case 0x1C: k = KEY_ENTER; break;
        case 0x1D: ctrl = !released; return;
        case 0x38: altgr = !released; return;
        case 0x35: k = '/'; break;
        default: return;
        }
        key_event(k, !released);
        return;
    }
    switch (sc) {
    case 0x2A: case 0x36: shift = !released; return;
    case 0x1D: ctrl = !released; return;
    case 0x38: return; /* left alt */
    case 0x3A: if (!released) caps = !caps; return;
    case 0x01: key_event(KEY_ESC, !released); return;
    case 0x3B: key_event(KEY_F1, !released); return;
    }
    if (sc >= sizeof es_normal) return;
    if (released) { key_event(0, 0); return; }

    u16 c;
    if (altgr) {
        switch (sc) {
        case 0x02: c = '|'; break;
        case 0x03: c = '@'; break;
        case 0x04: c = '#'; break;
        case 0x05: c = '~'; break;
        case 0x07: c = 0xAC; break;
        case 0x29: c = '\\'; break;
        case 0x1A: c = '['; break;
        case 0x1B: c = ']'; break;
        case 0x28: c = '{'; break;
        case 0x2B: c = '}'; break;
        case 0x12: c = 0x80; break; /* euro: shown as a box in Latin-1 */
        default: return;
        }
        key_event(c, 1);
        return;
    }
    u8 upper = shift;
    c = upper ? es_shift[sc] : es_normal[sc];
    if (caps && ((c >= 'a' && c <= 'z') || c == 0xF1 || c == 0xE7)) c = es_shift[sc];
    else if (caps && ((c >= 'A' && c <= 'Z') || c == 0xD1 || c == 0xC7)) c = es_normal[sc];
    if (!c) return;
    if (c == 0xB4 || c == 0xA8 || c == '`') { /* dead keys: ´ ¨ ` */
        if (dead) { key_event(dead, 1); dead = 0; key_event(c, 1); return; }
        dead = (u8)c;
        return;
    }
    if (dead) {
        u16 composed = apply_dead(dead, c);
        if (composed == c && c != ' ') key_event(dead, 1);
        if (c == ' ') composed = dead;
        dead = 0;
        c = composed;
    }
    if (ctrl && c >= 'a' && c <= 'z') c = (u16)(c - 'a' + 1) | 0x200; /* Ctrl+letter */
    key_event(c, 1);
}

static void kbd_irq(regs_t *r) {
    (void)r;
    kbd_scancode(inb(0x60));
}

/* ---- mouse ---- */
static u8 mpacket[3], mcycle;

static void mouse_irq(regs_t *r) {
    (void)r;
    u8 b = inb(0x60);
    /* Stay in step with the 3-byte packets: a late command ACK (0xFA) or a
     * lost byte would otherwise shift every packet after it. */
    static u32 last_byte_ms;
    if (mcycle && ticks_ms - last_byte_ms > 40) mcycle = 0;
    last_byte_ms = ticks_ms;
    if (mcycle == 0 && (!(b & 0x08) || (b & 0xC0) || b == 0xFA)) return;
    mpacket[mcycle++] = b;
    if (mcycle < 3) return;
    mcycle = 0;
    input_event_t ev = { .type = EV_MOUSE };
    ev.dx = (i32)mpacket[1] - ((mpacket[0] & 0x10) ? 256 : 0);
    ev.dy = -((i32)mpacket[2] - ((mpacket[0] & 0x20) ? 256 : 0));
    ev.buttons = mpacket[0] & 0x07;
    /* merge with a pending move that has the same buttons */
    cli();
    if (q_head != q_tail) {
        input_event_t *last = &queue[(q_head + QSIZE - 1) % QSIZE];
        if (last->type == EV_MOUSE && last->buttons == ev.buttons) {
            last->dx += ev.dx; last->dy += ev.dy;
            return;
        }
    }
    push(&ev);
}

static void wait_write(void) { for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {} }
static void wait_read(void) { for (int i = 0; i < 100000 && !(inb(0x64) & 1); i++) {} }
static void mouse_cmd(u8 c) {
    wait_write(); outb(0x64, 0xD4);
    wait_write(); outb(0x60, c);
    wait_read(); inb(0x60); /* ACK */
}

void ps2_init(void) {
    wait_write(); outb(0x64, 0xAD);          /* disable keyboard */
    wait_write(); outb(0x64, 0xA7);          /* disable mouse */
    while (inb(0x64) & 1) inb(0x60);         /* flush */
    wait_write(); outb(0x64, 0x20);
    wait_read(); u8 cfg = inb(0x60);
    cfg |= 0x03 | 0x40;                      /* IRQ1, IRQ12, translation */
    cfg &= (u8)~0x30;                        /* clocks on */
    wait_write(); outb(0x64, 0x60);
    wait_write(); outb(0x60, cfg);
    wait_write(); outb(0x64, 0xAE);          /* enable keyboard */
    wait_write(); outb(0x64, 0xA8);          /* enable mouse */
    mouse_cmd(0xF6);                         /* defaults */
    mouse_cmd(0xF4);                         /* start streaming */
    while (inb(0x64) & 1) inb(0x60);
    irq_install(1, kbd_irq);
    irq_install(12, mouse_irq);
}
