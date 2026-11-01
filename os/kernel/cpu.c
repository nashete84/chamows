/* Chamows kernel: GDT, IDT, PIC, PIT timer, exceptions, reboot/poweroff. */
#include "kernel.h"

#ifndef __x86_64__
/* ---- GDT: flat 4 GiB code and data segments, ring 0 (the 64-bit GDT is in boot.asm) ---- */
static u64 gdt[3];
static struct { u16 limit; u32 base; } __attribute__((packed)) gdtr;
extern void gdt_flush(void *);

static u64 gdt_entry(u32 base, u32 limit, u8 access, u8 flags) {
    u64 e = limit & 0xFFFF;
    e |= (u64)(base & 0xFFFFFF) << 16;
    e |= (u64)access << 40;
    e |= (u64)((limit >> 16) & 0xF) << 48;
    e |= (u64)(flags & 0xF) << 52;
    e |= (u64)(base >> 24) << 56;
    return e;
}
#endif

/* ---- IDT ---- */
#ifdef __x86_64__
typedef struct {
    u16 off_lo, sel;
    u8 ist, attr;
    u16 off_mid;
    u32 off_hi, zero;
} __attribute__((packed)) idt_entry_t;
#else
typedef struct {
    u16 off_lo, sel;
    u8 zero, attr;
    u16 off_hi;
} __attribute__((packed)) idt_entry_t;
#endif
static idt_entry_t idt[256];
static struct { u16 limit; uptr base; } __attribute__((packed)) idtr;
extern uptr isr_stub_table[48];

static irq_handler_t irq_handlers[16];
irq_handler_t tick_hook, fault_hook; /* win32.c: takes the CPU from a Windows thread whose turn is over */
volatile u32 ticks_ms;
static u32 ms_per_tick = 10;

static const char *exc_names[] = {
    "division por cero", "depuracion", "NMI", "punto de ruptura", "desbordamiento",
    "fuera de rango", "instruccion no valida", "sin coprocesador", "doble fallo",
    "segmento de coprocesador", "TSS no valido", "segmento ausente", "fallo de pila",
    "proteccion general", "fallo de pagina", "reservado", "error x87",
    "alineacion", "machine check", "error SIMD",
};

void panic_screen(const char *msg, u32 eip); /* gui.c */

void isr_dispatch(regs_t *r) {
    if (r->int_no < 32) {
        if (fault_hook) fault_hook(r); /* a Windows program's crash: returns only if it was not one */
        const char *name = r->int_no < 20 ? exc_names[r->int_no] : "excepcion";
        klog("\n*** EXCEPCION %u (%s) ip=%x err=%x\n", (u32)r->int_no, name, (u32)REG_IP(r), (u32)r->err_code);
        panic_screen(name, (u32)REG_IP(r));
        for (;;) __asm__ volatile("cli; hlt");
    }
    int irq = (int)r->int_no - 32;
    if (irq == 0) ticks_ms += ms_per_tick;
    if (irq_handlers[irq]) irq_handlers[irq](r);
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
    /* after the EOI: the hook may switch to another thread and come back later */
    if (irq == 0 && tick_hook) tick_hook(r);
}

void irq_install(int irq, irq_handler_t h) {
    irq_handlers[irq] = h;
    u16 port = irq < 8 ? 0x21 : 0xA1;
    outb(port, inb(port) & ~(1 << (irq & 7)));
    if (irq >= 8) outb(0x21, inb(0x21) & ~(1 << 2)); /* cascade */
}

static void pic_remap(void) {
    outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait(); outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait(); outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait(); outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFF); outb(0xA1, 0xFF); /* everything masked until installed */
}

void pit_init(u32 hz) {
    u32 div = 1193182 / hz;
    ms_per_tick = 1000 / hz;
    outb(0x43, 0x36);
    outb(0x40, (u8)(div & 0xFF));
    outb(0x40, (u8)(div >> 8));
    irq_install(0, NULL);
}

void cpu_init(void) {
#ifndef __x86_64__
    gdt[0] = 0;
    gdt[1] = gdt_entry(0, 0xFFFFF, 0x9A, 0xC);
    gdt[2] = gdt_entry(0, 0xFFFFF, 0x92, 0xC);
    gdtr.limit = sizeof gdt - 1;
    gdtr.base = (u32)gdt;
    gdt_flush(&gdtr);
#endif
    for (int i = 0; i < 48; i++) {
        uptr a = isr_stub_table[i];
        idt[i].off_lo = a & 0xFFFF;
        idt[i].sel = 0x08;
        idt[i].attr = 0x8E; /* present, ring 0, interrupt gate */
#ifdef __x86_64__
        idt[i].off_mid = (a >> 16) & 0xFFFF;
        idt[i].off_hi = (u32)(a >> 32);
#else
        idt[i].off_hi = a >> 16;
#endif
    }
    idtr.limit = sizeof idt - 1;
    idtr.base = (uptr)idt;
    __asm__ volatile("lidt %0" : : "m"(idtr));
    pic_remap();
}

void reboot(void) {
    cli();
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) {}
    outb(0x64, 0xFE);
    for (;;) __asm__ volatile("hlt");
}

void poweroff(void) {
    outw(0x604, 0x2000);   /* QEMU (q35/piix ACPI) */
    outw(0xB004, 0x2000);  /* Bochs / older QEMU */
    outw(0x4004, 0x3400);  /* VirtualBox */
}
