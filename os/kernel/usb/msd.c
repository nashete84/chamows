/* Chamows USB: pendrives and other USB disks (mass storage, bulk-only
 * transport with SCSI commands). The first disk found becomes usb_disk. */
#include "usb.h"

typedef struct {
    usb_dev_t *dev;
    u8 ep_in, ep_out;
    u32 tag;
    u8 *cbw, *csw, *buf;   /* DMA buffers; buf is 4 KB and 4 KB-aligned */
} msd_t;

static msd_t *disk;
blockdev_t usb_disk;

/* Runs one SCSI command. dir_in: data comes from the device. */
static int scsi(msd_t *m, const u8 *cmd, int cmd_len, int dir_in, u32 len) {
    u8 *c = m->cbw;
    memset(c, 0, 31);
    c[0] = 'U'; c[1] = 'S'; c[2] = 'B'; c[3] = 'C';
    u32 tag = ++m->tag;
    memcpy(c + 4, &tag, 4);
    memcpy(c + 8, &len, 4);
    c[12] = dir_in ? 0x80 : 0;
    c[14] = (u8)cmd_len;
    memcpy(c + 15, cmd, (size_t)cmd_len);
    if (usb_bulk(m->dev, m->ep_out, c, 31, NULL) != 0) return -1;
    if (len) {
        u32 got;
        if (usb_bulk(m->dev, dir_in ? m->ep_in : m->ep_out, m->buf, len, &got) != 0) {
            /* a stall here still ends with a status block; read it below */
        }
    }
    if (usb_bulk(m->dev, m->ep_in, m->csw, 13, NULL) != 0) return -1;
    if (m->csw[0] != 'U' || m->csw[3] != 'S') return -1;
    return m->csw[12] == 0 ? 0 : -1;
}

static int read_capacity(msd_t *m) {
    u8 cmd[10] = {0x25};
    if (scsi(m, cmd, 10, 1, 8) != 0) return -1;
    u8 *b = m->buf;
    u32 last = (u32)b[0] << 24 | (u32)b[1] << 16 | (u32)b[2] << 8 | b[3];
    u32 bs = (u32)b[4] << 24 | (u32)b[5] << 16 | (u32)b[6] << 8 | b[7];
    usb_disk.sectors = last + 1;
    usb_disk.sector_size = bs;
    return 0;
}

int msd_attach(usb_dev_t *d, const usb_iface_desc_t *ifc, const u8 *cfg, u32 cfg_len) {
    if (ifc->subcls != 6 || ifc->proto != 0x50) {
        klog("usb:   almacenamiento de tipo %u/%x no soportado\n", ifc->subcls, ifc->proto);
        return -1;
    }
    if (disk) { klog("usb:   ya hay un disco USB; este se ignora\n"); return -1; }
    const usb_ep_desc_t *in = NULL, *out = NULL;
    for (u32 off = (u32)((const u8 *)ifc - cfg) + ifc->len; off + 2 <= cfg_len && cfg[off] >= 2; off += cfg[off]) {
        if (cfg[off + 1] == 4) break;
        if (cfg[off + 1] != 5) continue;
        const usb_ep_desc_t *e = (const usb_ep_desc_t *)(cfg + off);
        if ((e->attributes & 3) != 2) continue;
        if (e->address & 0x80) { if (!in) in = e; } else if (!out) out = e;
    }
    if (!in || !out) return -1;
    if (usb_configure_ep(d, in) != 0 || usb_configure_ep(d, out) != 0) return -1;
    msd_t *m = kzalloc(sizeof *m);
    m->dev = d;
    m->ep_in = in->address;
    m->ep_out = out->address;
    m->cbw = kmalloc_aligned(64, 64);
    m->csw = kmalloc_aligned(64, 64);
    m->buf = kmalloc_aligned(4096, 4096);

    /* the device may need a moment: TEST UNIT READY until it answers */
    u8 tur[6] = {0};
    for (int i = 0; i < 20 && scsi(m, tur, 6, 0, 0) != 0; i++) {
        u8 sense[6] = {0x03, 0, 0, 0, 18, 0};
        scsi(m, sense, 6, 1, 18);
        u32 t = ticks_ms;
        while (ticks_ms - t < 50) {}
    }
    if (read_capacity(m) != 0 || usb_disk.sector_size != 512) {
        klog("usb:   el disco no responde o no usa sectores de 512 bytes\n");
        return -1;
    }
    disk = m;
    usb_disk.present = 1;
    strncpy0(usb_disk.name, d->name, sizeof usb_disk.name);
    klog("usb:   disco listo: %u MB\n", usb_disk.sectors / 2048);
    return 0;
}

/* Reads and writes go 8 sectors (4 KB) at a time through the DMA buffer. */
int disk_read(u32 lba, u32 count, void *buf) {
    if (!disk) return -1;
    u8 *out = buf;
    while (count) {
        u32 n = count > 8 ? 8 : count;
        u8 cmd[10] = {0x28, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba, 0, 0, (u8)n, 0};
        if (scsi(disk, cmd, 10, 1, n * 512) != 0) { klog("usb: error leyendo el sector %u\n", lba); return -1; }
        memcpy(out, disk->buf, n * 512);
        out += n * 512; lba += n; count -= n;
    }
    return 0;
}

int disk_write(u32 lba, u32 count, const void *buf) {
    if (!disk) return -1;
    const u8 *in = buf;
    while (count) {
        u32 n = count > 8 ? 8 : count;
        memcpy(disk->buf, in, n * 512);
        u8 cmd[10] = {0x2A, 0, (u8)(lba >> 24), (u8)(lba >> 16), (u8)(lba >> 8), (u8)lba, 0, 0, (u8)n, 0};
        if (scsi(disk, cmd, 10, 0, n * 512) != 0) { klog("usb: error escribiendo el sector %u\n", lba); return -1; }
        in += n * 512; lba += n; count -= n;
    }
    return 0;
}
