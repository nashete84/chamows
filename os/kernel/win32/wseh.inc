/* Exceptions the Windows way (x64 SEH), which C++ try/catch is built on.
 *
 * Every function in a .exe or .dll is described in its .pdata table: where
 * its prologue saves registers and how much stack it takes. With that, the
 * unwinder can step from any function back to its caller ("virtual unwind").
 * RaiseException walks up the stack this way asking each function's handler
 * (vcruntime's C++ handler, __C_specific_handler, ...) whether it wants the
 * exception; the one that does calls RtlUnwindEx, which walks again running
 * cleanups (destructors, __finally) and then jumps into the catch block.
 * Crashes in the program's own code (bad memory access, division by zero)
 * are turned into exceptions too, so they close only that program.
 * Included by win32.c. */

extern char _load_start[], _load_end[];
static int in_chamows(u64 ip) { return ip >= (u64)(uptr)_load_start && ip < (u64)(uptr)_load_end; }

/* CONTEXT (x64): only the offsets used here */
#define CX_FLAGS 0x30
#define CX_MXCSR 0x34
#define CX_EFLAGS 0x44
#define CX_RAX 0x78
#define CX_RSP 0x98
#define CX_RIP 0xf8
#define CX_FLT 0x100
#define CX_XMM 0x1a0
#define CX_SIZE 0x4d0
typedef struct { u8 b[CX_SIZE]; } __attribute__((aligned(16))) context_t;
#define CXQ(c, off) (*(u64 *)((c)->b + (off)))
static u64 *cx_reg(context_t *c, int r) { return (u64 *)(c->b + CX_RAX + 8 * r); } /* rax rcx rdx rbx rsp rbp rsi rdi r8..r15 */

typedef struct ex_rec {
    u32 code, flags;
    struct ex_rec *next;
    u64 address;
    u32 nparams, pad;
    u64 info[15];
} ex_rec_t;
typedef struct { u32 begin, end, unwind; } runtime_fn_t;
typedef struct {
    u64 control_pc, image_base;
    runtime_fn_t *fn;
    u64 establisher;
    u64 target_ip;
    context_t *context;
    void *handler;
    void *handler_data;
    void *history;
    u32 scope_index, fill;
} dispatcher_t;
typedef int (WINAPI *EXHANDLER)(ex_rec_t *, u64, context_t *, dispatcher_t *);
enum { EX_CONTINUE_EXECUTION, EX_CONTINUE_SEARCH, EX_NESTED, EX_COLLIDED };
#define EXF_NONCONTINUABLE 1
#define EXF_UNWINDING 2
#define EXF_EXIT_UNWIND 4
#define EXF_TARGET_UNWIND 0x20
#define EXF_COLLIDED_UNWIND 0x40
#define STATUS_UNWIND_CONSOLIDATE 0x80000029u

/* While Chamows code (the dispatcher, the unwinder) calls a program's
 * handler, the stack has Chamows frames in the middle, which have no unwind
 * tables. Each such call leaves a note: "if a walk reaches Chamows code above
 * here, carry on from this context instead" (the throw site for dispatching,
 * the frame being unwound for unwinding, like Windows' collided unwinds). */
typedef struct ex_link { struct ex_link *prev; u64 low; context_t *resume; } ex_link_t;
static void ex_push(ex_link_t *l, context_t *resume) {
    l->prev = (ex_link_t *)cur_th->ex_chain;
    l->low = (u64)(uptr)&l;
    l->resume = resume;
    cur_th->ex_chain = l;
}
static void ex_pop(ex_link_t *l) { cur_th->ex_chain = l->prev; }
static void ex_trim(u64 rsp) { /* frames below rsp are being abandoned */
    while (cur_th && cur_th->ex_chain && ((ex_link_t *)cur_th->ex_chain)->low < rsp) cur_th->ex_chain = ((ex_link_t *)cur_th->ex_chain)->prev;
}

/* ---- capturing and restoring all registers ---- */
__attribute__((naked)) static void WINAPI k_RtlCaptureContext(context_t *c) {
    __asm__ volatile(
        "mov %rax, 0x78(%rcx)\n mov %rcx, 0x80(%rcx)\n mov %rdx, 0x88(%rcx)\n mov %rbx, 0x90(%rcx)\n"
        "lea 8(%rsp), %rax\n mov %rax, 0x98(%rcx)\n mov %rbp, 0xa0(%rcx)\n mov %rsi, 0xa8(%rcx)\n mov %rdi, 0xb0(%rcx)\n"
        "mov %r8, 0xb8(%rcx)\n mov %r9, 0xc0(%rcx)\n mov %r10, 0xc8(%rcx)\n mov %r11, 0xd0(%rcx)\n"
        "mov %r12, 0xd8(%rcx)\n mov %r13, 0xe0(%rcx)\n mov %r14, 0xe8(%rcx)\n mov %r15, 0xf0(%rcx)\n"
        "mov (%rsp), %rax\n mov %rax, 0xf8(%rcx)\n"
        "fxsave64 0x100(%rcx)\n stmxcsr 0x34(%rcx)\n"
        "movw %cs, 0x38(%rcx)\n movw %ds, 0x3a(%rcx)\n movw %es, 0x3c(%rcx)\n movw %fs, 0x3e(%rcx)\n movw %gs, 0x40(%rcx)\n movw %ss, 0x42(%rcx)\n"
        "pushfq\n pop %rax\n mov %eax, 0x44(%rcx)\n"
        "movl $0x10000F, 0x30(%rcx)\n"
        "mov 0x78(%rcx), %rax\n ret\n");
}
/* Loads every register from the context and jumps to its Rip (never returns). */
__attribute__((naked, noreturn, ms_abi)) void w32_restore_asm(context_t *c) {
    __asm__ volatile(
        "fxrstor64 0x100(%rcx)\n ldmxcsr 0x34(%rcx)\n"
        "mov 0x98(%rcx), %rax\n"                       /* new rsp */
        "mov 0xf8(%rcx), %rdx\n mov %rdx, -8(%rax)\n"  /* rip, popped by ret */
        "mov 0x44(%rcx), %edx\n mov %rdx, -16(%rax)\n" /* rflags */
        "mov 0x78(%rcx), %rdx\n mov %rdx, -24(%rax)\n" /* rax */
        "mov 0x80(%rcx), %rdx\n mov %rdx, -32(%rax)\n" /* rcx */
        "mov 0x90(%rcx), %rbx\n mov 0xa0(%rcx), %rbp\n mov 0xa8(%rcx), %rsi\n mov 0xb0(%rcx), %rdi\n"
        "mov 0xb8(%rcx), %r8\n mov 0xc0(%rcx), %r9\n mov 0xc8(%rcx), %r10\n mov 0xd0(%rcx), %r11\n"
        "mov 0xd8(%rcx), %r12\n mov 0xe0(%rcx), %r13\n mov 0xe8(%rcx), %r14\n mov 0xf0(%rcx), %r15\n"
        "mov 0x88(%rcx), %rdx\n"
        "lea -32(%rax), %rsp\n pop %rcx\n pop %rax\n popfq\n ret\n");
}
/* STATUS_UNWIND_CONSOLIDATE: run the callback (vcruntime's "call the catch
 * block") on the target frame's stack, then continue where it says. The
 * context is copied next to it so the unwinder can step through this frame. */
__attribute__((naked, noreturn, ms_abi)) static void consolidate_asm(context_t *c, void *callback, ex_rec_t *rec, context_t *copy) {
    __asm__ volatile(
        "lea -0x30(%r9), %rsp\n"         /* copy sits at rsp + 0x30 */
        "movq $0x434F4E53, 0x20(%rsp)\n" /* "CONS" */
        "mov %r8, %rcx\n call *%rdx\n"
        ".globl consolidate_ret\nconsolidate_ret:\n"
        "lea 0x30(%rsp), %rcx\n mov %rax, 0xf8(%rcx)\n jmp w32_restore_asm\n");
}
extern char consolidate_ret[];
static WINAPI void k_RtlRestoreContext(context_t *c, ex_rec_t *rec) {
    ex_trim(CXQ(c, CX_RSP) - CX_SIZE - 0x100);
    if (rec && rec->code == STATUS_UNWIND_CONSOLIDATE && rec->nparams >= 1) {
        u64 at = (CXQ(c, CX_RSP) - CX_SIZE - 0x40) & ~15ULL;
        memmove((void *)at, c, CX_SIZE);
        consolidate_asm(c, (void *)rec->info[0], rec, (context_t *)at);
    }
    w32_restore_asm(c);
}

/* ---- finding a function's unwind description ---- */
static w32mod_t *mod_at(u64 pc, u64 *base) {
    w32mod_t *m = cur ? mod_of_addr(pc) : NULL;
    if (m && base) *base = (u64)m->image;
    return m;
}
static WINAPI runtime_fn_t *k_RtlLookupFunctionEntry(u64 pc, u64 *base, void *hist) {
    u64 b;
    w32mod_t *m = mod_at(pc, &b);
    if (!m) { if (base) *base = 0; return NULL; }
    if (base) *base = b;
    u32 len, d = dir_rva(m->image, 3, &len);
    if (!d) return NULL;
    runtime_fn_t *t = (runtime_fn_t *)(m->image + d);
    u32 rva = (u32)(pc - b), lo = 0, hi = len / 12;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (rva < t[mid].begin) hi = mid;
        else if (rva >= t[mid].end) lo = mid + 1;
        else {
            runtime_fn_t *f = &t[mid];
            if (f->unwind & 1) f = (runtime_fn_t *)(m->image + (f->unwind & ~1u)); /* shared entry */
            return f;
        }
    }
    return NULL;
}
static WINAPI u64 k_RtlPcToFileHeader(u64 pc, u64 *base) { u64 b = 0; mod_at(pc, &b); *base = b; return b; }

/* An epilogue looks like: [add/lea rsp] pop... ret|jmp. Inside one, the
 * function has already undone part of its prologue, so we finish the job by
 * "executing" those instructions on the context. */
static int epilog_at(const u8 *pc, u64 fbegin, u64 fend, int frame_reg) {
    if (pc[0] == 0x48 && pc[1] == 0x83 && pc[2] == 0xc4) pc += 4;
    else if (pc[0] == 0x48 && pc[1] == 0x81 && pc[2] == 0xc4) pc += 7;
    else if ((pc[0] & 0xf8) == 0x48 && pc[1] == 0x8d && frame_reg) {
        u8 modrm = pc[2];
        if (((modrm >> 3) & 7) != 4) return 0; /* lea must target rsp */
        pc += (modrm >> 6) == 1 ? 4 : (modrm >> 6) == 2 ? 7 : 3;
    }
    for (int n = 0; n < 20; n++) {
        if ((*pc & 0xf0) == 0x40 && (pc[1] & 0xf8) == 0x58) { pc += 2; continue; }
        if ((*pc & 0xf8) == 0x58) { pc++; continue; }
        if (*pc == 0xc3 || *pc == 0xc2) return 1;
        if (*pc == 0xf3 && pc[1] == 0xc3) return 1;
        if (*pc == 0xe9) { u64 t = (u64)(pc + 5 + *(const i32 *)(pc + 1)); return t < fbegin || t >= fend; }
        if (*pc == 0xeb) { u64 t = (u64)(pc + 2 + (i8)pc[1]); return t < fbegin || t >= fend; }
        if (*pc == 0xff && pc[1] == 0x25) return 1;
        if (pc[0] == 0x48 && pc[1] == 0xff && pc[2] == 0x25) return 1;
        return 0;
    }
    return 0;
}
static void run_epilog(const u8 *pc, context_t *c) {
    if (pc[0] == 0x48 && pc[1] == 0x83 && pc[2] == 0xc4) { CXQ(c, CX_RSP) += (u64)(i8)pc[3]; pc += 4; }
    else if (pc[0] == 0x48 && pc[1] == 0x81 && pc[2] == 0xc4) { CXQ(c, CX_RSP) += (u64)(i64) * (const i32 *)(pc + 3); pc += 7; }
    else if ((pc[0] & 0xf8) == 0x48 && pc[1] == 0x8d) {
        u8 modrm = pc[2];
        int base = (modrm & 7) | ((pc[0] & 1) << 3);
        i64 disp = (modrm >> 6) == 1 ? (i8)pc[3] : (modrm >> 6) == 2 ? *(const i32 *)(pc + 3) : 0;
        CXQ(c, CX_RSP) = *cx_reg(c, base) + (u64)disp;
        pc += (modrm >> 6) == 1 ? 4 : (modrm >> 6) == 2 ? 7 : 3;
    }
    for (;;) {
        int r;
        if ((*pc & 0xf0) == 0x40 && (pc[1] & 0xf8) == 0x58) { r = (pc[1] & 7) + ((*pc & 1) << 3); pc += 2; }
        else if ((*pc & 0xf8) == 0x58) { r = *pc & 7; pc++; }
        else break;
        *cx_reg(c, r) = *(u64 *)CXQ(c, CX_RSP);
        CXQ(c, CX_RSP) += 8;
    }
    CXQ(c, CX_RIP) = *(u64 *)CXQ(c, CX_RSP); /* ret, or a jump to another function: same thing for unwinding */
    CXQ(c, CX_RSP) += 8;
}

enum { UWOP_PUSH_NONVOL, UWOP_ALLOC_LARGE, UWOP_ALLOC_SMALL, UWOP_SET_FPREG, UWOP_SAVE_NONVOL, UWOP_SAVE_NONVOL_FAR,
       UWOP_EPILOG, UWOP_SPARE, UWOP_SAVE_XMM128, UWOP_SAVE_XMM128_FAR, UWOP_PUSH_MACHFRAME };
/* Steps the context from inside one function back to its caller. Returns the
 * function's exception handler (if it has one of the kind asked for). */
static WINAPI void *k_RtlVirtualUnwind(u32 type, u64 base, u64 pc, runtime_fn_t *fn, context_t *c, void **data, u64 *frame_out, void *ptrs) {
    const u8 *info = (const u8 *)(base + fn->unwind);
    u32 prolog_off = (u32)(pc - base - fn->begin);
    void *handler = NULL;
    int frame_reg = info[3] & 15;
    u64 frame = CXQ(c, CX_RSP);
    if (frame_reg) {
        /* the frame register only counts once the prologue has set it */
        int set = 0;
        const u16 *codes = (const u16 *)(info + 4);
        for (int i = 0; i < info[2]; i++) {
            u8 op = (u8)(codes[i] >> 8 & 15);
            if (op == UWOP_SET_FPREG && prolog_off >= (u8)codes[i]) set = 1;
        }
        if (set || prolog_off >= info[1]) frame = *cx_reg(c, frame_reg) - (u64)(info[3] >> 4) * 16;
    }
    *frame_out = frame;
    if (prolog_off >= info[1] && epilog_at((const u8 *)pc, base + fn->begin, base + fn->end, frame_reg)) {
        run_epilog((const u8 *)pc, c);
        if (data) *data = NULL;
        return NULL;
    }
    if (prolog_off >= info[1] && ((info[0] >> 3) & type)) {
        const u32 *h = (const u32 *)(info + 4 + 2 * ((info[2] + 1) & ~1));
        handler = (void *)(base + h[0]);
        if (data) *data = (void *)(h + 1);
    }
    int mach = 0;
    for (;;) {
        const u16 *codes = (const u16 *)(info + 4);
        int count = info[2];
        int fr = info[3] & 15;
        u64 fbase = CXQ(c, CX_RSP);
        if (fr) {
            int set = 0;
            for (int i = 0; i < count; i++) if ((u8)(codes[i] >> 8 & 15) == UWOP_SET_FPREG && prolog_off >= (u8)codes[i]) set = 1;
            if (set) fbase = *cx_reg(c, fr) - (u64)(info[3] >> 4) * 16;
        }
        for (int i = 0; i < count;) {
            u8 off = (u8)codes[i], op = (u8)(codes[i] >> 8 & 15), oi = (u8)(codes[i] >> 12);
            int slots = 1;
            switch (op) {
            case UWOP_ALLOC_LARGE: slots = oi ? 3 : 2; break;
            case UWOP_SAVE_NONVOL: case UWOP_SAVE_XMM128: case UWOP_EPILOG: slots = 2; break;
            case UWOP_SAVE_NONVOL_FAR: case UWOP_SAVE_XMM128_FAR: case UWOP_SPARE: slots = 3; break;
            }
            if (prolog_off < off || op == UWOP_EPILOG || op == UWOP_SPARE) { i += slots; continue; } /* not run yet */
            u64 *rsp = &CXQ(c, CX_RSP);
            switch (op) {
            case UWOP_PUSH_NONVOL: *cx_reg(c, oi) = *(u64 *)*rsp; *rsp += 8; break;
            case UWOP_ALLOC_LARGE: *rsp += oi ? *(const u32 *)&codes[i + 1] : (u64)codes[i + 1] * 8; break;
            case UWOP_ALLOC_SMALL: *rsp += (u64)oi * 8 + 8; break;
            case UWOP_SET_FPREG: *rsp = *cx_reg(c, fr) - (u64)(info[3] >> 4) * 16; break;
            case UWOP_SAVE_NONVOL: *cx_reg(c, oi) = *(u64 *)(fbase + (u64)codes[i + 1] * 8); break;
            case UWOP_SAVE_NONVOL_FAR: *cx_reg(c, oi) = *(u64 *)(fbase + *(const u32 *)&codes[i + 1]); break;
            case UWOP_SAVE_XMM128: memcpy(c->b + CX_XMM + 16 * oi, (void *)(fbase + (u64)codes[i + 1] * 16), 16); break;
            case UWOP_SAVE_XMM128_FAR: memcpy(c->b + CX_XMM + 16 * oi, (void *)(fbase + *(const u32 *)&codes[i + 1]), 16); break;
            case UWOP_PUSH_MACHFRAME:
                if (oi) *rsp += 8;
                CXQ(c, CX_RIP) = *(u64 *)*rsp;
                *(u32 *)(c->b + CX_EFLAGS) = *(u32 *)(*rsp + 16);
                *rsp = *(u64 *)(*rsp + 24);
                mach = 1;
                break;
            }
            i += slots;
        }
        if (!((info[0] >> 3) & 4)) break; /* UNW_FLAG_CHAININFO */
        fn = (runtime_fn_t *)(info + 4 + 2 * ((count + 1) & ~1));
        info = (const u8 *)(base + fn->unwind);
        prolog_off = 0xFFFFFFFF; /* the chained parts ran completely */
    }
    if (!mach) { CXQ(c, CX_RIP) = *(u64 *)CXQ(c, CX_RSP); CXQ(c, CX_RSP) += 8; }
    return handler;
}

/* One step up the stack, for dispatching or unwinding. Returns 0 when the
 * walk cannot go on (Chamows code, or the top of the stack). */
static int step_(u32 type, dispatcher_t *d, context_t *c);
static int seh_debug = 0;
static int step(u32 type, dispatcher_t *d, context_t *c) {
    u64 pc0 = CXQ(c, CX_RIP), sp0 = CXQ(c, CX_RSP);
    int r = step_(type, d, c);
    if (seh_debug) { u64 b = 0; w32mod_t *m = mod_at(pc0, &b); klog("seh: t%u %s+%x sp=%x -> ip=%x sp=%x est=%x h=%x r=%d\n", type, m ? m->name : "?", (u32)(pc0 - b), (u32)sp0, (u32)CXQ(c, CX_RIP), (u32)CXQ(c, CX_RSP), (u32)d->establisher, (u32)(u64)d->handler, r); }
    return r;
}
static int step_(u32 type, dispatcher_t *d, context_t *c) {
    u64 pc = CXQ(c, CX_RIP);
    d->control_pc = pc;
    d->handler = NULL;
    d->establisher = 0;
    if (pc == (u64)(uptr)consolidate_ret) { /* through a catch block, back into its function */
        context_t *saved = (context_t *)(CXQ(c, CX_RSP) + 0x30);
        d->establisher = CXQ(c, CX_RSP);
        memcpy(c, saved, CX_SIZE);
        d->fn = NULL;
        return 1;
    }
    if (pc && in_chamows(pc)) {
        u64 rsp = CXQ(c, CX_RSP);
        for (ex_link_t *l = cur_th ? (ex_link_t *)cur_th->ex_chain : NULL; l; l = l->prev)
            if (rsp <= CXQ(l->resume, CX_RSP)) {
                memcpy(c, l->resume, CX_SIZE);
                d->fn = NULL;
                d->establisher = rsp;
                return 1;
            }
        return 0;
    }
    if (!pc) return 0;
    u64 base;
    d->fn = k_RtlLookupFunctionEntry(pc, &base, NULL);
    d->image_base = base;
    if (!base) return 0;
    if (!d->fn) { /* leaf function: just the return address on the stack */
        d->establisher = CXQ(c, CX_RSP);
        CXQ(c, CX_RIP) = *(u64 *)CXQ(c, CX_RSP);
        CXQ(c, CX_RSP) += 8;
        return 1;
    }
    d->handler = k_RtlVirtualUnwind(type, base, pc, d->fn, c, &d->handler_data, &d->establisher, NULL);
    return 1;
}

/* ---- dispatching ---- */
typedef i32 (WINAPI *VEH)(void *ptrs[2]);
static WINAPI i32 (*unhandled_filter)(void *ptrs[2]);
static struct { VEH fn; w32proc_t *p; } vehs[8];
static WINAPI u64 k_AddVectoredExceptionHandler(u32 first, VEH fn) {
    for (int i = 0; i < 8; i++) if (!vehs[i].fn) { vehs[i].fn = fn; vehs[i].p = cur; return (u64)&vehs[i]; }
    return 0;
}
static WINAPI u32 k_RemoveVectoredExceptionHandler(u64 h) {
    for (int i = 0; i < 8; i++) if ((u64)&vehs[i] == h) { vehs[i].fn = NULL; return 1; }
    return 0;
}
static WINAPI void *k_SetUnhandledExceptionFilter(void *f) { void *old = (void *)unhandled_filter; unhandled_filter = f; return old; }
static void ex_fatal(ex_rec_t *rec, context_t *c) __attribute__((noreturn));
static void ex_fatal(ex_rec_t *rec, context_t *c) {
    u64 b = 0;
    w32mod_t *m = mod_at(rec->address, &b);
    klog("exe: %s se cerró: excepción %x en %s+0x%x\n", cur->name, rec->code, m ? m->name : "?", (u32)(rec->address - b));
    if ((rec->code == 0xC06D007E || rec->code == 0xC06D007F) && rec->info[0]) { /* delay-load: which DLL and function */
        const u8 *dli = (const u8 *)rec->info[0];
        const char *dll = *(const char **)(dli + 24), *fn = *(const char **)(dli + 40);
        if (*(const u32 *)(dli + 32)) klog("exe:   carga diferida: %s!%s no existe\n", dll ? dll : "?", fn ? fn : "?");
        else klog("exe:   carga diferida: %s!#%u no existe\n", dll ? dll : "?", (u32)(u64)fn);
    }
    { /* where it came from: the callers, found with the unwind tables */
        context_t t;
        memcpy(&t, c, CX_SIZE);
        dispatcher_t d;
        memset(&d, 0, sizeof d);
        d.context = &t;
        for (int i = 0; i < 12 && step(0, &d, &t); i++) {
            u64 ip = CXQ(&t, CX_RIP), mb = 0;
            w32mod_t *mm = mod_at(ip, &mb);
            if (!mm) break;
            klog("exe:   llamada desde %s+0x%x\n", mm->name, (u32)(ip - mb));
        }
    }
    char msg[120];
    ksnprintf(msg, sizeof msg, "Error %x en %s. El programa se cerró.", rec->code, m ? m->name : "Chamows");
    notify(cur->icon, cur->name, msg);
    proc_exit(255);
    for (;;) {}
}
static void dispatch(ex_rec_t *rec, context_t *orig) {
    void *ptrs[2] = {rec, orig};
    for (int i = 0; i < 8; i++)
        if (vehs[i].fn && vehs[i].p == cur && vehs[i].fn(ptrs) == -1) k_RtlRestoreContext(orig, NULL);
    context_t c;
    memcpy(&c, orig, CX_SIZE);
    dispatcher_t d;
    memset(&d, 0, sizeof d);
    d.context = &c;
    for (int depth = 0; depth < 4096; depth++) {
        if (!step(1, &d, &c)) break;
        if (d.handler) {
            ex_link_t l;
            ex_push(&l, orig);
            int r = ((EXHANDLER)d.handler)(rec, d.establisher, orig, &d);
            ex_pop(&l);
            if (r == EX_CONTINUE_EXECUTION) {
                if (rec->flags & EXF_NONCONTINUABLE) break;
                k_RtlRestoreContext(orig, NULL);
            }
        }
    }
    if (unhandled_filter && unhandled_filter(ptrs) == -1) k_RtlRestoreContext(orig, NULL);
    ex_fatal(rec, orig);
}
/* RaiseException: its context is the caller's, captured on entry. */
__attribute__((ms_abi, used)) void w32_raise_c(u32 code, u32 flags, u32 n, const u64 *args, context_t *c) {
    ex_rec_t rec;
    memset(&rec, 0, sizeof rec);
    rec.code = code;
    rec.flags = flags & EXF_NONCONTINUABLE;
    rec.address = CXQ(c, CX_RIP);
    rec.nparams = n > 15 ? 15 : n;
    if (args) for (u32 i = 0; i < rec.nparams; i++) rec.info[i] = args[i];
    dispatch(&rec, c);
}
#define CAPTURE_HERE(ctxoff, frame)                                                                              \
    "mov %rbx, " ctxoff "+0x90(%rsp)\n mov %rbp, " ctxoff "+0xa0(%rsp)\n mov %rsi, " ctxoff "+0xa8(%rsp)\n"     \
    "mov %rdi, " ctxoff "+0xb0(%rsp)\n mov %r12, " ctxoff "+0xd8(%rsp)\n mov %r13, " ctxoff "+0xe0(%rsp)\n"     \
    "mov %r14, " ctxoff "+0xe8(%rsp)\n mov %r15, " ctxoff "+0xf0(%rsp)\n"                                        \
    "fxsave64 " ctxoff "+0x100(%rsp)\n stmxcsr " ctxoff "+0x34(%rsp)\n"                                         \
    "pushfq\n pop %rax\n mov %eax, " ctxoff "+0x44(%rsp)\n movl $0x10000F, " ctxoff "+0x30(%rsp)\n"            \
    "lea " frame "+8(%rsp), %rax\n mov %rax, " ctxoff "+0x98(%rsp)\n"                                            \
    "mov " frame "(%rsp), %rax\n mov %rax, " ctxoff "+0xf8(%rsp)\n"
__attribute__((naked)) static void WINAPI k_RaiseException(u32 code, u32 flags, u32 n, const u64 *args) {
    __asm__ volatile(
        "sub $0x508, %rsp\n"
        CAPTURE_HERE("0x30", "0x508")
        "lea 0x30(%rsp), %rax\n mov %rax, 0x20(%rsp)\n"
        "call w32_raise_c\n"
        "ud2\n");
}

/* ---- unwinding ---- */
__attribute__((ms_abi, used)) void w32_unwind_c(u64 end_frame, u64 target_ip, ex_rec_t *rec, u64 retval, context_t *c) {
    if (seh_debug) klog("seh: unwind hasta %x ip=%x code=%x\n", (u32)end_frame, (u32)target_ip, rec ? rec->code : 0);
    ex_rec_t local;
    if (!rec) {
        memset(&local, 0, sizeof local);
        local.code = 0xC0000027; /* STATUS_UNWIND */
        local.address = CXQ(c, CX_RIP);
        rec = &local;
    }
    rec->flags |= EXF_UNWINDING | (end_frame ? 0 : EXF_EXIT_UNWIND);
    context_t nc;
    dispatcher_t d;
    memset(&d, 0, sizeof d);
    d.target_ip = target_ip;
    d.context = c;
    for (int depth = 0; depth < 4096; depth++) {
        memcpy(&nc, c, CX_SIZE);
        if (!step(2, &d, &nc)) break;
        rec->flags &= ~EXF_COLLIDED_UNWIND;
        if (end_frame && d.establisher > end_frame) { klog("exe: desenrollado fuera de destino\n"); break; }
        if (d.establisher == end_frame) rec->flags |= EXF_TARGET_UNWIND;
        if (d.handler) {
            d.context = c;
            ex_link_t l;
            ex_push(&l, c);
            int r = ((EXHANDLER)d.handler)(rec, d.establisher, c, &d);
            ex_pop(&l);
            if (r == EX_COLLIDED) {
                memcpy(&nc, c, CX_SIZE);
                void *hd; u64 fr;
                if (d.fn) k_RtlVirtualUnwind(0, d.image_base, d.control_pc, d.fn, &nc, &hd, &fr, NULL);
                rec->flags |= EXF_COLLIDED_UNWIND;
            }
        }
        if (d.establisher == end_frame) break;
        memcpy(c, &nc, CX_SIZE);
    }
    CXQ(c, CX_RAX) = retval;
    CXQ(c, CX_RIP) = target_ip;
    if (seh_debug) { u64 *g = cx_reg(c, 0); klog("seh: salto ip=%x sp=%x rax=%x rdx=%x rbx=%x rbp=%x rsi=%x rdi=%x r12=%x\n", (u32)target_ip, (u32)g[4], (u32)g[0], (u32)g[2], (u32)g[3], (u32)g[5], (u32)g[6], (u32)g[7], (u32)g[12]); }
    k_RtlRestoreContext(c, rec);
}
/* RtlUnwind / RtlUnwindEx: the unwind starts from the caller's registers,
 * captured on entry (RtlUnwindEx's context argument is only scratch space). */
__attribute__((naked)) static void WINAPI k_RtlUnwind(u64 frame, u64 ip, ex_rec_t *rec, u64 retval) {
    __asm__ volatile(
        "sub $0x508, %rsp\n"
        CAPTURE_HERE("0x30", "0x508")
        "lea 0x30(%rsp), %rax\n mov %rax, 0x20(%rsp)\n"
        "call w32_unwind_c\n"
        "ud2\n");
}
#define k_RtlUnwindEx k_RtlUnwind

/* ---- crashes in the program's code ---- */
static void cpu_fault(regs_t *r) {
    if (!cur_th || !cur || cur->done) return; /* not a Windows thread: the kernel stops as before */
    if (in_chamows(r->rip)) {
        klog("exe: fallo %u dentro de Chamows (ip=%x) mientras corría %s; se cierra el programa\n", (u32)r->int_no, (u32)r->rip, cur->name);
        { /* the Chamows and program functions it came through (return addresses found on the stack) */
            u64 *sp = (u64 *)r->rsp;
            for (int i = 0, n = 0; i < 400 && n < 10; i++) {
                u64 v = sp[i], mb = 0;
                if (in_chamows(v)) { klog("exe:   pila: chamows %x\n", (u32)v); n++; }
                else { w32mod_t *m = mod_at(v, &mb); if (m) { klog("exe:   pila: %s+0x%x\n", m->name, (u32)(v - mb)); n++; } }
            }
        }
        notify(cur->icon, cur->name, "Error interno de Chamows. El programa se cerró.");
        proc_exit(255);
    }
    context_t c;
    memset(&c, 0, sizeof c);
    u64 *g = cx_reg(&c, 0);
    g[0] = r->rax; g[1] = r->rcx; g[2] = r->rdx; g[3] = r->rbx; g[4] = r->rsp; g[5] = r->rbp; g[6] = r->rsi; g[7] = r->rdi;
    g[8] = r->r8; g[9] = r->r9; g[10] = r->r10; g[11] = r->r11; g[12] = r->r12; g[13] = r->r13; g[14] = r->r14; g[15] = r->r15;
    CXQ(&c, CX_RIP) = r->rip;
    *(u32 *)(c.b + CX_EFLAGS) = (u32)r->rflags;
    *(u32 *)(c.b + CX_FLAGS) = 0x10000F;
    __asm__ volatile("fxsave64 %0" : "=m"(*(u8(*)[512])(c.b + CX_FLT)));
    __asm__ volatile("stmxcsr %0" : "=m"(*(u32 *)(c.b + CX_MXCSR)));
    ex_rec_t rec;
    memset(&rec, 0, sizeof rec);
    rec.address = r->rip;
    switch (r->int_no) {
    case 0: rec.code = 0xC0000094; break;               /* integer divide by zero */
    case 3: rec.code = 0x80000003; break;               /* breakpoint */
    case 6: rec.code = 0xC000001D; break;               /* illegal instruction */
    case 14: {
        u64 cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        rec.code = 0xC0000005; rec.nparams = 2; rec.info[0] = (r->err_code >> 1) & 1; rec.info[1] = cr2;
        break;
    }
    case 19: rec.code = 0xC00002B5; break;              /* SSE floating point */
    default: rec.code = 0xC0000005; rec.nparams = 2; rec.info[0] = 0; rec.info[1] = ~0ULL; break;
    }
    if (rec.code == 0x80000003) CXQ(&c, CX_RIP) = rec.address = r->rip - 1; /* the int3 itself */
    dispatch(&rec, &c);
}

static WINAPI u16 k_RtlCaptureStackBackTrace(u32 skip, u32 n, void **out, u32 *hash) {
    context_t c;
    k_RtlCaptureContext(&c);
    dispatcher_t d;
    u16 got = 0;
    if (hash) *hash = 0;
    for (u32 i = 0; got < n && i < 256; i++) {
        if (!step(0, &d, &c)) break;
        if (i >= skip) out[got++] = (void *)CXQ(&c, CX_RIP);
    }
    return got;
}

static const struct { const char *name; void *fn; } exports_seh[] = {
    {"RtlCaptureContext", k_RtlCaptureContext}, {"RtlRestoreContext", k_RtlRestoreContext},
    {"RtlLookupFunctionEntry", k_RtlLookupFunctionEntry}, {"RtlVirtualUnwind", k_RtlVirtualUnwind},
    {"RtlUnwindEx", k_RtlUnwindEx}, {"RtlUnwind", k_RtlUnwind}, {"RtlPcToFileHeader", k_RtlPcToFileHeader},
    {"RaiseException", k_RaiseException},
    {"AddVectoredExceptionHandler", k_AddVectoredExceptionHandler},
    {"RemoveVectoredExceptionHandler", k_RemoveVectoredExceptionHandler},
    {"SetUnhandledExceptionFilter", k_SetUnhandledExceptionFilter}, {"UnhandledExceptionFilter", k_zero},
    {"RtlCaptureStackBackTrace", k_RtlCaptureStackBackTrace},
};
