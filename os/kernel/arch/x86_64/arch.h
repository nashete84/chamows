/* 64-bit x86: registers saved by isr.asm on every interrupt. */
#ifndef CHAMOWS_ARCH_H
#define CHAMOWS_ARCH_H
#define ARCH_BITS 64
typedef struct regs {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rdi, rsi, rdx, rcx, rbx, rax;
    u64 int_no, err_code;
    u64 rip, cs, rflags, rsp, ss;
} regs_t;
#define REG_IP(r) ((r)->rip)
#endif
