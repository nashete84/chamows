/* Chamows USB: xHCI host controller driver (USB 1.1, 2.0 and 3.x devices).
 *
 * The controller is polled: usb_poll() reads the event ring from the GUI
 * loop and blocking transfers poll until their completion event arrives.
 * Memory is identity-mapped, so a pointer is also the DMA address.
 * Devices on root ports are supported; hubs are not (yet). */
#include "usb.h"

#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_EP 12
#define TRB_EVALUATE_CTX 13
#define TRB_EV_TRANSFER 32
#define TRB_EV_COMMAND 33
#define TRB_EV_PORT 34

#define CC_SUCCESS 1
#define CC_SHORT_PACKET 13

#define RING_TRBS 64
#define MAX_SLOTS 32

typedef struct { u64 param; u32 status; u32 control; } __attribute__((packed, aligned(16))) trb_t;

typedef struct {
    trb_t *trb;
    u32 enq, cycle;
} ring_t;

/* per endpoint state (DCI 1..31) */
typedef struct {
    ring_t ring;
    int type;
    /* blocking transfers */
    volatile int done;
    u32 code, residual;
    /* interrupt IN endpoints that keep a report queued */
    usb_int_fn fn;
    void *ctx;
    u8 *buf;
    u16 len;
    u8 addr;
} ep_t;

typedef struct {
    usb_dev_t *dev;
    void *out_ctx;          /* device context (written by the controller) */
    u8 *in_ctx;             /* input context (written by us) */
    ep_t ep[32];
} slot_t;

static volatile u8 *cap, *op, *rt;
static volatile u32 *db;
static u32 max_slots, max_ports, ctx_size;
static u64 *dcbaa;
static ring_t cmd_ring;
static trb_t *ev_ring;
static u32 ev_deq, ev_cycle;
static slot_t slots[MAX_SLOTS + 1];
static volatile int cmd_done;
static u32 cmd_code, cmd_slot;

usb_dev_t *usb_devices;
int usb_controllers;

static inline u32 rd32(volatile u8 *b, u32 off) { return *(volatile u32 *)(b + off); }
static inline void wr32(volatile u8 *b, u32 off, u32 v) { *(volatile u32 *)(b + off) = v; }
static inline void wr64(volatile u8 *b, u32 off, u64 v) {
    *(volatile u32 *)(b + off) = (u32)v;
    *(volatile u32 *)(b + off + 4) = (u32)(v >> 32);
}
#define barrier() __asm__ volatile("" ::: "memory")
#define PHYS(p) ((u64)(uptr)(p))

static void ring_init(ring_t *r) {
    r->trb = kmalloc_aligned(RING_TRBS * sizeof(trb_t), 64);
    r->enq = 0;
    r->cycle = 1;
    trb_t *link = &r->trb[RING_TRBS - 1];
    link->param = PHYS(r->trb);
    link->control = (TRB_LINK << 10) | 2; /* toggle cycle */
}

static trb_t *ring_push(ring_t *r, u64 param, u32 status, u32 control) {
    trb_t *t = &r->trb[r->enq];
    t->param = param;
    t->status = status;
    barrier();
    t->control = (control & ~1u) | r->cycle;
    if (++r->enq == RING_TRBS - 1) {
        trb_t *link = &r->trb[RING_TRBS - 1];
        link->control = (link->control & ~1u) | r->cycle;
        r->cycle ^= 1;
        r->enq = 0;
    }
    return t;
}

/* ---- contexts ---- */
static u32 *ctx(u8 *base, int idx) { return (u32 *)(base + (uptr)idx * ctx_size); }
/* input context: [0] control, [1] slot, [2..] endpoints by DCI */
static u32 *in_slot(slot_t *s) { return ctx(s->in_ctx, 1); }
static u32 *in_ep(slot_t *s, int dci) { return ctx(s->in_ctx, 1 + dci); }

/* ---- events ---- */
static void handle_event(trb_t *e) {
    u32 type = (e->control >> 10) & 0x3F;
    u32 code = e->status >> 24;
    if (type == TRB_EV_COMMAND) {
        cmd_code = code;
        cmd_slot = e->control >> 24;
        cmd_done = 1;
    } else if (type == TRB_EV_TRANSFER) {
        u32 slot = e->control >> 24, dci = (e->control >> 16) & 0x1F;
        if (slot == 0 || slot > MAX_SLOTS) return;
        ep_t *ep = &slots[slot].ep[dci];
        if (ep->fn) {
            if (code != CC_SUCCESS && code != CC_SHORT_PACKET) {
                klog("usb: el dispositivo %u dejó de enviar datos (código %u)\n", slot, code);
                ep->fn = NULL; /* a stalled endpoint would fail forever */
                return;
            }
            u32 got = ep->len - (e->status & 0xFFFFFF);
            ep->fn(ep->ctx, ep->buf, got);
            /* queue the next report request */
            ring_push(&ep->ring, PHYS(ep->buf), ep->len, (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));
            db[slot] = dci;
        } else {
            ep->code = code;
            ep->residual = e->status & 0xFFFFFF;
            ep->done = 1;
        }
    }
    /* port status changes are ignored: devices are found at boot */
}

static void poll_events(void) {
    if (!ev_ring) return;
    int any = 0;
    for (;;) {
        trb_t *e = &ev_ring[ev_deq];
        if ((e->control & 1) != ev_cycle) break;
        barrier();
        handle_event(e);
        any = 1;
        if (++ev_deq == RING_TRBS) { ev_deq = 0; ev_cycle ^= 1; }
    }
    if (any) wr64(rt, 0x38, PHYS(&ev_ring[ev_deq]) | 8); /* ERDP, clear busy */
}

void usb_poll(void) {
    if (!usb_controllers) return;
    poll_events();
    hid_tick();
}

static int wait_flag(volatile int *flag, u32 ms) {
    u32 start = ticks_ms;
    while (!*flag) {
        poll_events();
        if (ticks_ms - start > ms) return 0;
        __asm__ volatile("pause");
    }
    return 1;
}

static int command(u64 param, u32 control, u32 *slot_out) {
    cmd_done = 0;
    ring_push(&cmd_ring, param, 0, control);
    db[0] = 0;
    if (!wait_flag(&cmd_done, 1000)) { klog("usb: la orden %u no responde\n", (control >> 10) & 0x3F); return -1; }
    if (slot_out) *slot_out = cmd_slot;
    if (cmd_code != CC_SUCCESS) { klog("usb: orden %u falló (código %u)\n", (control >> 10) & 0x3F, cmd_code); return -1; }
    return 0;
}

/* ---- transfers ---- */
static int wait_ep(int slot, int dci, u32 ms) {
    ep_t *ep = &slots[slot].ep[dci];
    if (!wait_flag(&ep->done, ms)) return -1;
    return (ep->code == CC_SUCCESS || ep->code == CC_SHORT_PACKET) ? 0 : -(int)ep->code;
}

int usb_control(usb_dev_t *d, u8 req_type, u8 req, u16 value, u16 index, void *data, u16 len) {
    slot_t *s = &slots[d->slot];
    ep_t *ep = &s->ep[1];
    static u8 *bounce;
    if (!bounce) bounce = kmalloc_aligned(4096, 4096);
    if (len > 4096) return -1;
    int in = req_type & 0x80;
    if (!in && len) memcpy(bounce, data, len);
    u64 setup = req_type | ((u64)req << 8) | ((u64)value << 16) | ((u64)index << 32) | ((u64)len << 48);
    u32 trt = len ? (in ? 3 : 2) : 0;
    ep->done = 0;
    ring_push(&ep->ring, setup, 8, (TRB_SETUP << 10) | (1 << 6) | (trt << 16));
    if (len) ring_push(&ep->ring, PHYS(bounce), len, (TRB_DATA << 10) | (in ? 1u << 16 : 0));
    ring_push(&ep->ring, 0, 0, (TRB_STATUS << 10) | (1 << 5) | ((len && in) ? 0 : 1u << 16));
    db[d->slot] = 1;
    int rc = wait_ep(d->slot, 1, 1000);
    if (rc == 0 && in && len) memcpy(data, bounce, len);
    return rc;
}

static int dci_of(u8 addr) { return (addr & 0x0F) * 2 + ((addr & 0x80) ? 1 : 0); }

int usb_configure_ep(usb_dev_t *d, const usb_ep_desc_t *e) {
    slot_t *s = &slots[d->slot];
    int dci = dci_of(e->address);
    int in = e->address & 0x80, kind = e->attributes & 3;
    int type = kind == 2 ? (in ? EP_BULK_IN : EP_BULK_OUT) : kind == 3 ? (in ? EP_INT_IN : EP_INT_OUT) : 0;
    if (!type) return -1;
    ep_t *ep = &s->ep[dci];
    ring_init(&ep->ring);
    ep->type = type;
    ep->addr = e->address;

    u32 interval = 0;
    if (kind == 3) {
        if (d->speed == USB_SPEED_HIGH || d->speed == USB_SPEED_SUPER) interval = e->interval ? e->interval - 1u : 0;
        else { /* full/low speed: bInterval in ms -> 2^n x 125 us */
            u32 us8 = (u32)(e->interval ? e->interval : 1) * 8;
            while ((1u << (interval + 1)) <= us8) interval++;
            if (interval < 3) interval = 3;
        }
        if (interval > 15) interval = 15;
    }
    u32 mps = e->max_packet & 0x7FF;
    memset(s->in_ctx, 0, 33 * ctx_size);
    u32 *icc = ctx(s->in_ctx, 0);
    icc[1] = 1u | (1u << dci);                     /* add slot + this endpoint */
    memcpy(in_slot(s), ctx(s->out_ctx, 0), ctx_size);
    u32 entries = (in_slot(s)[0] >> 27) & 0x1F;
    if ((u32)dci > entries) entries = (u32)dci;
    in_slot(s)[0] = (in_slot(s)[0] & ~(0x1Fu << 27)) | (entries << 27);
    u32 *c = in_ep(s, dci);
    c[0] = interval << 16;
    c[1] = (3u << 1) | ((u32)type << 3) | (mps << 16);
    c[2] = (u32)PHYS(ep->ring.trb) | 1;
    c[3] = (u32)(PHYS(ep->ring.trb) >> 32);
    c[4] = (kind == 3 ? mps : 3072) | ((kind == 3 ? mps : 0) << 16);
    return command(PHYS(s->in_ctx), (TRB_CONFIGURE_EP << 10) | ((u32)d->slot << 24), NULL);
}

int usb_bulk(usb_dev_t *d, u8 addr, void *data, u32 len, u32 *done) {
    /* data must not cross a 64 KB boundary: callers use 4 KB-aligned buffers */
    int dci = dci_of(addr);
    ep_t *ep = &slots[d->slot].ep[dci];
    ep->done = 0;
    ring_push(&ep->ring, PHYS(data), len, (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));
    db[d->slot] = (u32)dci;
    int rc = wait_ep(d->slot, dci, 5000);
    if (done) *done = rc == 0 ? len - ep->residual : 0;
    return rc;
}

int usb_interrupt_start(usb_dev_t *d, u8 addr, u16 len, usb_int_fn fn, void *ctx) {
    int dci = dci_of(addr);
    ep_t *ep = &slots[d->slot].ep[dci];
    ep->buf = kmalloc_aligned(len < 64 ? 64 : len, 64);
    ep->len = len;
    ep->ctx = ctx;
    ep->fn = fn;
    ring_push(&ep->ring, PHYS(ep->buf), len, (TRB_NORMAL << 10) | (1 << 5) | (1 << 2));
    db[d->slot] = (u32)dci;
    return 0;
}

/* ---- enumeration ---- */
static volatile u32 *portsc(u32 port) { return (volatile u32 *)(op + 0x400 + 0x10 * (port - 1)); }
#define PORT_CHANGE_BITS 0x00FE0000u
#define PORT_PED (1u << 1)

static int port_reset(u32 port) {
    volatile u32 *p = portsc(port);
    u32 v = *p;
    *p = (v & ~(PORT_PED | PORT_CHANGE_BITS)) | (1u << 4); /* PR */
    u32 start = ticks_ms;
    while (!(*p & (1u << 21))) {                            /* PRC */
        if (ticks_ms - start > 500) return -1;
    }
    v = *p;
    *p = (v & ~(PORT_PED | PORT_CHANGE_BITS)) | (1u << 21); /* clear PRC */
    return (*p & PORT_PED) ? 0 : -1;
}

static void attach_interfaces(usb_dev_t *d, u8 *cfg, u32 len) {
    for (u32 off = 0; off + 2 <= len && cfg[off] >= 2; off += cfg[off]) {
        if (cfg[off + 1] != 4) continue; /* interface */
        const usb_iface_desc_t *ifc = (const usb_iface_desc_t *)(cfg + off);
        if (ifc->alt != 0) continue;
        if (ifc->cls == 3) hid_attach(d, ifc, cfg, len);
        else if (ifc->cls == 8) msd_attach(d, ifc, cfg, len);
        else klog("usb:   interfaz clase %u sin controlador\n", ifc->cls);
    }
}

static void read_string(usb_dev_t *d, u8 idx, char *out, int max) {
    u8 buf[128];
    out[0] = 0;
    if (!idx || usb_control(d, 0x80, 6, 0x0300 | idx, 0x0409, buf, sizeof buf) != 0) return;
    int n = 0;
    for (int i = 2; i + 1 < buf[0] && n < max - 1; i += 2) { /* UTF-16 -> Latin-1 */
        u16 c = (u16)(buf[i] | (buf[i + 1] << 8));
        out[n++] = c < 0x80 ? (char)c : '?';
    }
    out[n] = 0;
}

static void enumerate_port(u32 port) {
    u32 v = *portsc(port);
    if (!(v & 1)) return;                        /* nothing connected */
    if (!(v & PORT_PED) && port_reset(port) != 0) { klog("usb: puerto %u no se pudo reiniciar\n", port); return; }
    int speed = (int)((*portsc(port) >> 10) & 0xF);

    u32 slot_id;
    if (command(0, TRB_ENABLE_SLOT << 10, &slot_id) != 0 || slot_id == 0 || slot_id > MAX_SLOTS) return;
    slot_t *s = &slots[slot_id];
    s->out_ctx = kmalloc_aligned(32 * ctx_size, 64);
    s->in_ctx = kmalloc_aligned(33 * ctx_size, 64);
    dcbaa[slot_id] = PHYS(s->out_ctx);
    ring_init(&s->ep[1].ring);

    u32 mps0 = speed == USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 : 8;
    ctx(s->in_ctx, 0)[1] = 3;                    /* add slot + EP0 */
    in_slot(s)[0] = ((u32)speed << 20) | (1u << 27);
    in_slot(s)[1] = port << 16;
    u32 *e0 = in_ep(s, 1);
    e0[1] = (3u << 1) | (4u << 3) | (mps0 << 16); /* CErr 3, control */
    e0[2] = (u32)PHYS(s->ep[1].ring.trb) | 1;
    e0[3] = (u32)(PHYS(s->ep[1].ring.trb) >> 32);
    e0[4] = 8;
    if (command(PHYS(s->in_ctx), (TRB_ADDRESS_DEVICE << 10) | (slot_id << 24), NULL) != 0) return;

    usb_dev_t *d = kzalloc(sizeof *d);
    d->slot = (int)slot_id;
    d->port = (int)port;
    d->speed = speed;
    s->dev = d;

    if (usb_control(d, 0x80, 6, 0x0100, 0, &d->desc, 8) != 0) { klog("usb: puerto %u no responde\n", port); return; }
    if (d->desc.max_packet0 && speed != USB_SPEED_SUPER && d->desc.max_packet0 != mps0) {
        memset(s->in_ctx, 0, 33 * ctx_size);
        ctx(s->in_ctx, 0)[1] = 2;                /* evaluate EP0 */
        in_ep(s, 1)[1] = (3u << 1) | (4u << 3) | ((u32)d->desc.max_packet0 << 16);
        command(PHYS(s->in_ctx), (TRB_EVALUATE_CTX << 10) | (slot_id << 24), NULL);
    }
    if (usb_control(d, 0x80, 6, 0x0100, 0, &d->desc, sizeof d->desc) != 0) return;
    read_string(d, d->desc.i_product, d->name, sizeof d->name);
    if (!d->name[0]) ksnprintf(d->name, sizeof d->name, "Dispositivo %x:%x", d->desc.vendor, d->desc.product);
    static const char *speeds[] = {"?", "12 Mb/s", "1,5 Mb/s", "480 Mb/s", "5 Gb/s", "10 Gb/s"};
    klog("usb: puerto %u: %s (%x:%x, %s)\n", port, d->name, d->desc.vendor, d->desc.product, speeds[speed <= 5 ? speed : 0]);

    usb_config_desc_t cd;
    if (usb_control(d, 0x80, 6, 0x0200, 0, &cd, sizeof cd) != 0) return;
    u32 total = cd.total_len > 1024 ? 1024 : cd.total_len;
    u8 *cfg = kmalloc(total);
    if (!cfg || usb_control(d, 0x80, 6, 0x0200, 0, cfg, (u16)total) != 0) { kfree(cfg); return; }
    if (usb_control(d, 0x00, 9, cd.config_value, 0, NULL, 0) != 0) { kfree(cfg); return; }
    d->next = usb_devices;
    usb_devices = d;
    attach_interfaces(d, cfg, total);
    kfree(cfg);
}

static void legacy_handoff(volatile u8 *base, u32 hcc1) {
    u32 off = (hcc1 >> 16) << 2;
    while (off) {
        volatile u32 *c = (volatile u32 *)(base + off);
        if ((*c & 0xFF) == 1) {                  /* USB legacy support */
            *c |= 1u << 24;                      /* OS owned */
            u32 start = ticks_ms;
            while ((*c & (1u << 16)) && ticks_ms - start < 1000) {}
            c[1] &= 0x000E1FEE;                  /* disable SMIs */
            return;
        }
        u32 next = (*c >> 8) & 0xFF;
        off = next ? off + (next << 2) : 0;
    }
}

static int controller_init(u8 bus, u8 dev, u8 fn) {
    u32 bar0 = pci_read(bus, dev, fn, 0x10);
    u64 addr = bar0 & ~0xFu;
    if ((bar0 & 6) == 4) addr |= (u64)pci_read(bus, dev, fn, 0x14) << 32;
    if (addr >> 32) { klog("usb: el controlador está por encima de 4 GB, no soportado\n"); return -1; }
    pci_write(bus, dev, fn, 0x04, pci_read(bus, dev, fn, 0x04) | 0x6); /* memory + bus master */

    cap = (volatile u8 *)(uptr)addr;
    op = cap + cap[0];
    u32 hcs1 = rd32(cap, 4), hcs2 = rd32(cap, 8), hcc1 = rd32(cap, 0x10);
    db = (volatile u32 *)(cap + (rd32(cap, 0x14) & ~3u));
    rt = cap + (rd32(cap, 0x18) & ~0x1Fu);
    max_slots = hcs1 & 0xFF;
    if (max_slots > MAX_SLOTS) max_slots = MAX_SLOTS;
    max_ports = hcs1 >> 24;
    ctx_size = (hcc1 & 4) ? 64 : 32;
    legacy_handoff(cap, hcc1);

    /* stop and reset */
    wr32(op, 0, rd32(op, 0) & ~1u);
    u32 start = ticks_ms;
    while (!(rd32(op, 4) & 1) && ticks_ms - start < 500) {}
    wr32(op, 0, 2);
    start = ticks_ms;
    while ((rd32(op, 0) & 2) || (rd32(op, 4) & (1u << 11))) {
        if (ticks_ms - start > 1000) { klog("usb: el controlador no se reinicia\n"); return -1; }
    }

    wr32(op, 0x38, max_slots);
    dcbaa = kmalloc_aligned((max_slots + 1) * 8, 64);
    u32 scratch = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (scratch) {
        u64 *arr = kmalloc_aligned(scratch * 8, 64);
        for (u32 i = 0; i < scratch; i++) arr[i] = PHYS(kmalloc_aligned(4096, 4096));
        dcbaa[0] = PHYS(arr);
    }
    wr64(op, 0x30, PHYS(dcbaa));
    ring_init(&cmd_ring);
    wr64(op, 0x18, PHYS(cmd_ring.trb) | 1);

    /* one event ring segment for interrupter 0 */
    ev_ring = kmalloc_aligned(RING_TRBS * sizeof(trb_t), 64);
    u64 *erst = kmalloc_aligned(16, 64);
    erst[0] = PHYS(ev_ring);
    erst[1] = RING_TRBS;
    ev_deq = 0;
    ev_cycle = 1;
    wr32(rt, 0x28, 1);                            /* ERSTSZ */
    wr64(rt, 0x38, PHYS(ev_ring));                /* ERDP */
    wr64(rt, 0x30, PHYS(erst));                   /* ERSTBA */
    wr32(op, 0, 1);                               /* run */
    start = ticks_ms;
    while ((rd32(op, 4) & 1) && ticks_ms - start < 500) {}
    klog("usb: controlador xHCI con %u puertos\n", max_ports);

    /* give devices time to report their connection, then enumerate */
    start = ticks_ms;
    while (ticks_ms - start < 100) poll_events();
    for (u32 p = 1; p <= max_ports; p++) enumerate_port(p);
    return 0;
}

void usb_init(void) {
    u8 b, d, f;
    if (!pci_find_class(0x0C, 0x03, 0x30, &b, &d, &f, 0)) {
        klog("usb: no hay controlador xHCI\n");
        return;
    }
    if (controller_init(b, d, f) == 0) usb_controllers = 1;
}
