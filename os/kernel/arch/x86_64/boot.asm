; Chamows boot entry, 64-bit version.
; A Multiboot loader (QEMU -kernel, GRUB) starts us in 32-bit protected mode.
; We build page tables that map the first 4 GiB 1:1 with 2 MiB pages
; (RAM, the framebuffer and device registers all live there), switch on
; long mode and jump to 64-bit code.

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
bits 32
_start:
    cli
    mov esp, stack_top
    mov edi, eax            ; multiboot magic  -> 1st argument of kmain
    mov esi, ebx            ; multiboot info   -> 2nd argument

    ; clear the page tables (6 pages: PML4, PDPT, 4 page directories)
    push edi
    mov edi, pml4
    mov ecx, 6 * 4096 / 4
    xor eax, eax
    rep stosd
    pop edi

    mov dword [pml4], pdpt + 3                 ; present | writable
    mov dword [pdpt + 0], pd + 0 * 4096 + 3
    mov dword [pdpt + 8], pd + 1 * 4096 + 3
    mov dword [pdpt + 16], pd + 2 * 4096 + 3
    mov dword [pdpt + 24], pd + 3 * 4096 + 3
    xor ecx, ecx
.map:                                          ; 2048 x 2 MiB = 4 GiB
    mov eax, ecx
    shl eax, 21
    or eax, 0x83                               ; present | writable | 2 MiB page
    mov [pd + ecx * 8], eax
    inc ecx
    cmp ecx, 2048
    jne .map

    mov eax, cr4
    or eax, 1 << 5                             ; PAE
    mov cr4, eax
    mov eax, pml4
    mov cr3, eax
    mov ecx, 0xC0000080                        ; EFER
    rdmsr
    or eax, 1 << 8                             ; long mode enable
    wrmsr
    mov eax, cr0
    or eax, (1 << 31) | 1                      ; paging + protected mode
    mov cr0, eax
    lgdt [gdt64.ptr]
    jmp 0x08:long_mode

bits 64
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax
    mov rsp, stack_top
    mov edi, edi            ; upper halves are undefined after the switch
    mov esi, esi
    cld
    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .rodata
align 16
gdt64:
    dq 0
    dq 0x00AF9A000000FFFF   ; 0x08: 64-bit code, ring 0
    dq 0x00CF92000000FFFF   ; 0x10: data, ring 0
.ptr:
    dw $ - gdt64 - 1
    dd gdt64

section .bss
align 4096
pml4: resb 4096
pdpt: resb 4096
pd:   resb 4 * 4096
align 16
stack_bottom:
    resb 65536
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
