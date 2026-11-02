/* Intel 8254x ("e1000") network card, polled. QEMU's default card (8086:100E);
 * also 82545EM and 82574-like ids that use the same legacy descriptors. */
#include "net.h"

#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_IMC 0x00D8
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818
#define REG_MTA 0x5200
#define REG_RAL 0x5400
#define REG_RAH 0x5404

#define NRX 32
#define NTX 16
#define BUFSZ 2048

typedef struct { u64 addr; u16 len, csum; u8 status, errors; u16 special; } __attribute__((packed)) rxd_t;
typedef struct { u64 addr; u16 len; u8 cso, cmd, status, css; u16 special; } __attribute__((packed)) txd_t;

u8 nic_mac[6];
int nic_ready;
static volatile u8 *mmio;
static rxd_t *rx;
static txd_t *tx;
static u8 *rxbuf[NRX], *txbuf[NTX];
static u32 rx_next, tx_next;

static u32 rd(u32 r) { return *(volatile u32 *)(mmio + r); }
static void wr(u32 r, u32 v) { *(volatile u32 *)(mmio + r) = v; }

int nic_init(void) {
    static const u16 ids[] = {0x100E, 0x100F, 0x10D3, 0x1004, 0x100C, 0x10EA};
    u8 bus, dev, fn;
    int found = 0;
    for (u32 i = 0; i < sizeof ids / sizeof ids[0] && !found; i++) found = pci_find(0x8086, ids[i], &bus, &dev, &fn) == 0;
    if (!found) { klog("red: no hay tarjeta de red e1000\n"); return -1; }
    u32 bar0 = pci_read(bus, dev, fn, 0x10);
    u64 addr = bar0 & ~0xFu;
    if ((bar0 & 6) == 4) addr |= (u64)pci_read(bus, dev, fn, 0x14) << 32;
    if (addr >> 32 || (bar0 & 1)) { klog("red: la tarjeta usa una dirección no soportada\n"); return -1; }
    pci_write(bus, dev, fn, 0x04, pci_read(bus, dev, fn, 0x04) | 0x6); /* memory + bus master */
    mmio = (volatile u8 *)(uptr)addr;

    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, rd(REG_CTRL) | (1u << 26)); /* reset */
    u32 t = ticks_ms;
    while ((rd(REG_CTRL) & (1u << 26)) && ticks_ms - t < 100) {}
    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, (rd(REG_CTRL) | (1u << 6) | (1u << 5)) & ~(1u << 3)); /* link up, auto speed */

    u32 lo = rd(REG_RAL), hi = rd(REG_RAH);
    for (int i = 0; i < 4; i++) nic_mac[i] = (u8)(lo >> (8 * i));
    nic_mac[4] = (u8)hi;
    nic_mac[5] = (u8)(hi >> 8);
    wr(REG_RAH, hi | (1u << 31)); /* address valid */
    for (int i = 0; i < 128; i++) wr(REG_MTA + 4 * (u32)i, 0);

    rx = kmalloc_aligned(sizeof(rxd_t) * NRX, 128);
    tx = kmalloc_aligned(sizeof(txd_t) * NTX, 128);
    for (int i = 0; i < NRX; i++) { rxbuf[i] = kmalloc_aligned(BUFSZ, 16); rx[i].addr = (u64)(uptr)rxbuf[i]; }
    for (int i = 0; i < NTX; i++) { txbuf[i] = kmalloc_aligned(BUFSZ, 16); tx[i].status = 1; }
    wr(REG_RDBAL, (u32)(uptr)rx); wr(REG_RDBAH, 0);
    wr(REG_RDLEN, sizeof(rxd_t) * NRX);
    wr(REG_RDH, 0); wr(REG_RDT, NRX - 1);
    wr(REG_RCTL, (1u << 1) | (1u << 15) | (1u << 26)); /* enable, broadcast, strip CRC, 2 KB buffers */
    wr(REG_TDBAL, (u32)(uptr)tx); wr(REG_TDBAH, 0);
    wr(REG_TDLEN, sizeof(txd_t) * NTX);
    wr(REG_TDH, 0); wr(REG_TDT, 0);
    wr(REG_TCTL, (1u << 1) | (1u << 3) | (0x10u << 4) | (0x40u << 12)); /* enable, pad short packets */
    wr(REG_TIPG, 0x0060200A);
    nic_ready = 1;
    klog("red: tarjeta e1000 %02x:%02x:%02x:%02x:%02x:%02x, enlace %s\n", nic_mac[0], nic_mac[1], nic_mac[2], nic_mac[3], nic_mac[4], nic_mac[5],
         (rd(REG_STATUS) & 2) ? "conectado" : "sin cable");
    return 0;
}

int nic_send(const void *frame, u32 len) {
    if (!nic_ready || len > BUFSZ) return -1;
    txd_t *d = &tx[tx_next];
    u32 t = ticks_ms;
    while (!(d->status & 1)) if (ticks_ms - t > 50) return -1; /* still sending */
    memcpy(txbuf[tx_next], frame, len);
    d->addr = (u64)(uptr)txbuf[tx_next];
    d->len = (u16)len;
    d->cmd = 1 | 2 | 8; /* end of packet, add CRC, report status */
    d->status = 0;
    tx_next = (tx_next + 1) % NTX;
    wr(REG_TDT, tx_next);
    return 0;
}

int nic_recv(void *frame, u32 max) {
    if (!nic_ready) return 0;
    rxd_t *d = &rx[rx_next];
    if (!(d->status & 1)) return 0;
    u32 n = d->len;
    int ok = (d->status & 2) && !d->errors && n <= max; /* whole packet, no errors */
    if (ok) memcpy(frame, rxbuf[rx_next], n);
    d->status = 0;
    u32 done = rx_next;
    rx_next = (rx_next + 1) % NRX;
    wr(REG_RDT, done);
    return ok ? (int)n : 0;
}
