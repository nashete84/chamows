/* Chamows kernel: shared declarations. */
#ifndef CHAMOWS_KERNEL_H
#define CHAMOWS_KERNEL_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char i8;
typedef short i16;
typedef int i32;
typedef signed long long i64;
typedef unsigned long size_t;
typedef unsigned long uptr; /* pointer-sized integer */
#define NULL ((void *)0)

#define CHAMOWS_VERSION "0.5"

/* ---- ports ---- */
static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline u16 inw(u16 p) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline u32 inl(u16 p) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }
static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }

/* ---- string.c ---- */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strncpy0(char *d, const char *s, size_t n); /* always NUL-terminates */
char *utoa(u32 v, char *buf, int base);
char *itoa(int v, char *buf);
/* Tiny formatter: %s %d %u %x %c %% and width for d/u (e.g. %02d). */
#include <stdarg.h>
int ksnprintf(char *buf, size_t n, const char *fmt, ...);
int kvsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* ---- serial.c (COM1 log) ---- */
void serial_init(void);
void klog(const char *fmt, ...);

/* ---- mem.c ---- */
void heap_init(uptr start, uptr end);
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void kfree(void *p);
void *kmalloc_aligned(size_t n, size_t align); /* zeroed, permanent */
u32 heap_used(void);
u32 heap_total(void);

/* ---- cpu.c ---- */
#include "arch.h" /* regs_t and REG_IP() for this CPU */
typedef void (*irq_handler_t)(regs_t *);
extern irq_handler_t tick_hook;
void cpu_init(void);
void irq_install(int irq, irq_handler_t h);
void pit_init(u32 hz);
extern volatile u32 ticks_ms;
void reboot(void);
void poweroff(void);

/* ---- input (ps2.c) ---- */
enum { EV_NONE, EV_KEY, EV_MOUSE };
typedef struct {
    u8 type;
    u8 pressed;   /* key: 1 down, 0 up */
    u16 key;      /* key: character (Latin-1) or KEY_* code */
    i32 dx, dy;   /* mouse */
    u8 buttons;   /* mouse: bit0 left, bit1 right, bit2 middle */
    u8 absolute;  /* mouse: dx, dy are a position, 0..32767 across the screen (USB tablet) */
} input_event_t;
enum {
    KEY_ESC = 0x100, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END,
    KEY_DELETE, KEY_PGUP, KEY_PGDN, KEY_F1, KEY_SUPER,
};
#define KEY_BACKSPACE 8
#define KEY_TAB 9
#define KEY_ENTER 10
void ps2_init(void);
int input_poll(input_event_t *ev);
void input_push(const input_event_t *ev);  /* from outside interrupt handlers */
void kbd_scancode(u8 sc);                   /* feed one PS/2 set-1 byte (USB keyboards use this too) */
u32 pci_find_class(u8 cls, u8 sub, u8 prog, u8 *bus, u8 *dev, u8 *fn, u32 skip);
void pci_write(u8 bus, u8 dev, u8 fn, u8 off, u32 v);

/* ---- pci.c ---- */
u32 pci_read(u8 bus, u8 dev, u8 fn, u8 off);
int pci_find(u16 vendor, u16 device, u8 *bus, u8 *dev, u8 *fn);

/* ---- rtc.c ---- */
typedef struct { int year, month, day, hour, min, sec; } rtc_time_t;
void rtc_read(rtc_time_t *t);

/* ---- multiboot ---- */
typedef struct {
    u32 flags, mem_lower, mem_upper, boot_device, cmdline, mods_count, mods_addr;
} multiboot_info_t;

extern u32 total_memory_kb;
void cpu_vendor(char out[13]);

#endif
