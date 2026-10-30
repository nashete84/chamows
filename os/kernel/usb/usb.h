/* Chamows USB stack: xHCI host controller, HID (keyboard, mouse, tablet)
 * and mass storage (pendrives). Everything is polled from the GUI loop;
 * no USB interrupts are used. */
#ifndef CHAMOWS_USB_H
#define CHAMOWS_USB_H
#include "kernel.h"

/* ---- standard descriptors ---- */
typedef struct {
    u8 len, type;
    u16 bcd_usb;
    u8 cls, subcls, proto, max_packet0;
    u16 vendor, product, bcd_dev;
    u8 i_manufacturer, i_product, i_serial, n_configs;
} __attribute__((packed)) usb_device_desc_t;

typedef struct {
    u8 len, type;
    u16 total_len;
    u8 n_ifaces, config_value, i_config, attributes, max_power;
} __attribute__((packed)) usb_config_desc_t;

typedef struct {
    u8 len, type, number, alt, n_eps, cls, subcls, proto, i_iface;
} __attribute__((packed)) usb_iface_desc_t;

typedef struct {
    u8 len, type, address, attributes;
    u16 max_packet;
    u8 interval;
} __attribute__((packed)) usb_ep_desc_t;

enum { USB_SPEED_FULL = 1, USB_SPEED_LOW = 2, USB_SPEED_HIGH = 3, USB_SPEED_SUPER = 4 };
enum { EP_BULK_OUT = 2, EP_INT_OUT = 3, EP_BULK_IN = 6, EP_INT_IN = 7 };

struct usb_dev;
typedef void (*usb_int_fn)(void *ctx, const u8 *data, u32 len);

typedef struct usb_dev {
    int slot, port, speed;
    usb_device_desc_t desc;
    char name[48];
    struct usb_dev *next;
} usb_dev_t;

/* ---- xhci.c ---- */
void usb_init(void);
void usb_poll(void);           /* call often: handles key/mouse reports */
int usb_control(usb_dev_t *d, u8 req_type, u8 req, u16 value, u16 index, void *data, u16 len);
/* endpoint: address from the descriptor (0x81 = EP1 IN). */
int usb_configure_ep(usb_dev_t *d, const usb_ep_desc_t *ep);
int usb_bulk(usb_dev_t *d, u8 ep_addr, void *data, u32 len, u32 *done); /* blocking */
/* Keeps a report request queued on an interrupt IN endpoint; fn gets each report. */
int usb_interrupt_start(usb_dev_t *d, u8 ep_addr, u16 len, usb_int_fn fn, void *ctx);
extern usb_dev_t *usb_devices;
extern int usb_controllers;

/* ---- hid.c / msd.c: called once per interface during enumeration ---- */
int hid_attach(usb_dev_t *d, const usb_iface_desc_t *iface, const u8 *cfg, u32 cfg_len);
int msd_attach(usb_dev_t *d, const usb_iface_desc_t *iface, const u8 *cfg, u32 cfg_len);
void hid_tick(void);           /* key repeat */

/* ---- block device (the first pendrive found) ---- */
typedef struct {
    int present;
    u32 sectors, sector_size;
    char name[40];
} blockdev_t;
extern blockdev_t usb_disk;
int disk_read(u32 lba, u32 count, void *buf);
int disk_write(u32 lba, u32 count, const void *buf);

#endif
