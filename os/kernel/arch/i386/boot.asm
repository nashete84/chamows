; Chamows boot entry. A Multiboot loader (QEMU -kernel, GRUB, v86) puts the
; CPU in 32-bit protected mode and jumps to _start.
; The "a.out kludge" (flag bit 16) lets loaders place the flat binary
; without parsing ELF.

MB_MAGIC    equ 0x1BADB002
MB_FLAGS    equ (1 << 0) | (1 << 1) | (1 << 16)   ; align, memory info, address fields
MB_CHECKSUM equ -(MB_MAGIC + MB_FLAGS)

extern _load_start, _load_end, _bss_end
extern kmain
global _start

section .multiboot
align 4
mb_header:
    dd MB_MAGIC
    dd MB_FLAGS
    dd MB_CHECKSUM
    dd mb_header        ; header_addr
    dd _load_start      ; load_addr
    dd _load_end        ; load_end_addr
    dd _bss_end         ; bss_end_addr
    dd _start           ; entry_addr

section .text
_start:
    mov esp, stack_top
    cld
    fninit              ; x87 FPU for apps that use floating point
    push ebx            ; multiboot info
    push eax            ; multiboot magic
    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .bss
align 16
stack_bottom:
    resb 65536
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
