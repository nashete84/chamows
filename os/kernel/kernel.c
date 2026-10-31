/* Chamows kernel entry point. */
#include "gui.h"
#include "usb/usb.h"
#include "net/net.h"
#include "fat.h"

u32 total_memory_kb;
extern u8 _kernel_end[];

/* ---- Bochs/QEMU/v86 display adapter (BGA) ---- */
static void vbe_write(u16 idx, u16 v) { outw(0x1CE, idx); outw(0x1CF, v); }
static u16 vbe_read(u16 idx) { outw(0x1CE, idx); return inw(0x1CF); }

static u32 *video_init(int w, int h) {
    u16 id = vbe_read(0);
    if (id < 0xB0C0 || id > 0xB0CF) {
        klog("video: no hay adaptador BGA (id %x)\n", id);
        return NULL;
    }
    vbe_write(4, 0);       /* disable */
    vbe_write(1, (u16)w);
    vbe_write(2, (u16)h);
    vbe_write(3, 32);
    vbe_write(4, 0x41);    /* enable + linear framebuffer */
    if (vbe_read(1) != w || vbe_read(2) != h) {
        klog("video: el adaptador no aceptó %dx%d\n", w, h);
        return NULL;
    }
    u32 addr = 0xE0000000; /* v86 default */
    u8 b, d, f;
    if (pci_find(0x1234, 0x1111, &b, &d, &f)) addr = pci_read(b, d, f, 0x10) & ~0xFu;
    klog("video: %dx%d en %x\n", w, h, addr);
    return (u32 *)(uptr)addr;
}

void kmain(u32 magic, multiboot_info_t *mbi) {
    serial_init();
    klog("\nChamows %s arrancando\n", CHAMOWS_VERSION);
    cpu_init();

    total_memory_kb = 32 * 1024;
    if (magic == 0x2BADB002 && (mbi->flags & 1)) total_memory_kb = mbi->mem_upper + 1024;
    else {
        /* No memory map from the loader (v86): ask the CMOS for RAM above 16 MB. */
        outb(0x70, 0x34); u32 lo = inb(0x71);
        outb(0x70, 0x35); u32 hi = inb(0x71);
        u32 above16 = ((hi << 8) | lo) * 64; /* KB */
        if (above16) total_memory_kb = 16 * 1024 + above16;
    }
    uptr heap_start = ((uptr)_kernel_end + 0xFFF) & ~(uptr)0xFFF;
    uptr heap_end = (uptr)total_memory_kb * 1024;
    if (heap_end > 0x20000000) heap_end = 0x20000000; /* 512 MB is plenty */
    heap_init(heap_start, heap_end);
    klog("memoria: %u MB, montón %u KB\n", total_memory_kb / 1024, heap_total() / 1024);

    pit_init(100);
    ps2_init();
    sti();

    int sw = 1024, sh = 768;
    u32 *lfb = video_init(sw, sh);
    if (!lfb) { sw = 800; sh = 600; lfb = video_init(sw, sh); }
    if (!lfb) {
        klog("Chamows necesita una tarjeta gráfica compatible (QEMU -vga std, v86 o Bochs).\n");
        for (;;) __asm__ volatile("hlt");
    }
    repo_init();
    usb_init();
    net_init();
    if (fat_mount() == 0) apps_load_from_disk();
    gui_init(lfb, sw, sh);
    gui_run();
}
