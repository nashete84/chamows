/* 32-bit x86: registers saved by isr.asm on every interrupt. */
#ifndef CHAMOWS_ARCH_H
#define CHAMOWS_ARCH_H
#define ARCH_BITS 32
typedef struct regs {
    u32 gs, fs, es, ds;
    u32 edi, esi, ebp, esp, ebx, edx, ecx, eax;
    u32 int_no, err_code;
    u32 eip, cs, eflags, useresp, ss;
} regs_t;
#define REG_IP(r) ((r)->eip)
#endif
