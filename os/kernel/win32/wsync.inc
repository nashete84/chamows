/* Locks for programs whose threads can now be paused at any moment
 * (preemption): critical sections, SRW locks, condition variables,
 * WaitOnAddress and one-time initialisation.
 *
 * Chamows' own code is never paused halfway, so each of these functions runs
 * in one piece: checking and taking a lock needs no atomic instructions here.
 * A thread that has to wait gives its turn away until the lock is free. */

static int timed_out(u32 start, u32 ms) { return ms != 0xFFFFFFFF && ticks_ms - start >= ms; }
static u64 my_tid(void) { return cur_th ? cur_th->id : 1; }

/* CRITICAL_SECTION: LockCount at 8, RecursionCount at 12, OwningThread at 16 */
#define CS_REC(c) (*(i32 *)((u8 *)(c) + 12))
#define CS_OWNER(c) (*(u64 *)((u8 *)(c) + 16))
static WINAPI void s_InitializeCriticalSection(void *c) { memset(c, 0, 40); *(i32 *)((u8 *)c + 8) = -1; }
static WINAPI int s_InitializeCriticalSectionEx(void *c, u32 spin, u32 fl) { s_InitializeCriticalSection(c); return 1; }
static WINAPI void s_EnterCriticalSection(void *c) {
    u64 me = my_tid();
    while (CS_OWNER(c) && CS_OWNER(c) != me) yield(W_RUN);
    CS_OWNER(c) = me;
    CS_REC(c)++;
}
static WINAPI int s_TryEnterCriticalSection(void *c) {
    u64 me = my_tid();
    if (CS_OWNER(c) && CS_OWNER(c) != me) return 0;
    CS_OWNER(c) = me;
    CS_REC(c)++;
    return 1;
}
static WINAPI void s_LeaveCriticalSection(void *c) {
    if (CS_OWNER(c) != my_tid()) return;
    if (--CS_REC(c) <= 0) { CS_REC(c) = 0; CS_OWNER(c) = 0; }
}

/* SRWLOCK: 0 free, 1 taken to write, 2*n read by n threads */
static WINAPI void s_AcquireSRWLockExclusive(u64 *l) { while (*l) yield(W_RUN); *l = 1; }
static WINAPI void s_AcquireSRWLockShared(u64 *l) { while (*l & 1) yield(W_RUN); *l += 2; }
static WINAPI void s_ReleaseSRWLockExclusive(u64 *l) { *l = 0; }
static WINAPI void s_ReleaseSRWLockShared(u64 *l) { if (*l >= 2) *l -= 2; }
static WINAPI u8 s_TryAcquireSRWLockExclusive(u64 *l) { if (*l) return 0; *l = 1; return 1; }
static WINAPI u8 s_TryAcquireSRWLockShared(u64 *l) { if (*l & 1) return 0; *l += 2; return 1; }
static WINAPI void s_InitializeSRWLock(u64 *l) { *l = 0; }

/* CONDITION_VARIABLE: a counter that every wake changes */
static int cv_wait(u64 *cv, u32 ms) {
    u64 seen = *cv;
    u32 start = ticks_ms;
    while (*cv == seen) {
        if (timed_out(start, ms)) { last_error = 1460; return 0; } /* ERROR_TIMEOUT */
        yield(W_RUN);
    }
    return 1;
}
static WINAPI int s_SleepConditionVariableCS(u64 *cv, void *c, u32 ms) {
    i32 rec = CS_REC(c);
    CS_REC(c) = 0;
    CS_OWNER(c) = 0;
    int ok = cv_wait(cv, ms);
    s_EnterCriticalSection(c);
    CS_REC(c) = rec;
    return ok;
}
static WINAPI int s_SleepConditionVariableSRW(u64 *cv, u64 *l, u32 ms, u32 flags) {
    int shared = flags & 1;
    if (shared) s_ReleaseSRWLockShared(l); else s_ReleaseSRWLockExclusive(l);
    int ok = cv_wait(cv, ms);
    if (shared) s_AcquireSRWLockShared(l); else s_AcquireSRWLockExclusive(l);
    return ok;
}
static WINAPI void s_WakeConditionVariable(u64 *cv) { (*cv)++; }
static WINAPI void s_InitializeConditionVariable(u64 *cv) { *cv = 0; }

/* WaitOnAddress: wait while *addr still equals *cmp */
static WINAPI int s_WaitOnAddress(volatile void *addr, const void *cmp, u64 size, u32 ms) {
    u32 start = ticks_ms;
    if (size > 8) size = 8;
    while (memcmp((const void *)addr, cmp, (size_t)size) == 0) {
        if (timed_out(start, ms)) { last_error = 1460; return 0; }
        yield(W_RUN);
    }
    return 1;
}

/* INIT_ONCE: 0 not started, 1 running, 2 done */
typedef int (WINAPI *ONCEFN)(u64 *, void *, void **);
static WINAPI int s_InitOnceExecuteOnce(u64 *o, ONCEFN fn, void *param, void **ctx) {
    for (;;) {
        if (*o == 2) return 1;
        if (*o == 0) break;
        yield(W_RUN);
    }
    *o = 1;
    int ok = fn(o, param, ctx);
    *o = ok ? 2 : 0;
    return ok;
}
static WINAPI int s_InitOnceBeginInitialize(u64 *o, u32 flags, int *pending, void **ctx) {
    for (;;) {
        if (*o == 2) { *pending = 0; return 1; }
        if (flags & 2) { last_error = 87; return 0; } /* INIT_ONCE_CHECK_ONLY, not done yet */
        if (*o == 0) { *o = 1; *pending = 1; return 1; }
        yield(W_RUN);
    }
}
static WINAPI int s_InitOnceComplete(u64 *o, u32 flags, void *ctx) { *o = (flags & 4) ? 0 : 2; return 1; }
static WINAPI void s_InitOnceInitialize(u64 *o) { *o = 0; }

static WINAPI int s_SwitchToThread(void) { yield(W_RUN); return 1; }
static WINAPI u32 s_FlsAlloc(void *cb) { return k_TlsAlloc(); }

static const struct { const char *name; void *fn; } exports_sync[] = {
    {"InitializeCriticalSection", s_InitializeCriticalSection}, {"InitializeCriticalSectionEx", s_InitializeCriticalSectionEx},
    {"InitializeCriticalSectionAndSpinCount", s_InitializeCriticalSectionEx}, {"EnterCriticalSection", s_EnterCriticalSection},
    {"TryEnterCriticalSection", s_TryEnterCriticalSection}, {"LeaveCriticalSection", s_LeaveCriticalSection},
    {"DeleteCriticalSection", k_zero}, {"SetCriticalSectionSpinCount", k_zero},
    {"InitializeSRWLock", s_InitializeSRWLock}, {"AcquireSRWLockExclusive", s_AcquireSRWLockExclusive},
    {"AcquireSRWLockShared", s_AcquireSRWLockShared}, {"ReleaseSRWLockExclusive", s_ReleaseSRWLockExclusive},
    {"ReleaseSRWLockShared", s_ReleaseSRWLockShared}, {"TryAcquireSRWLockExclusive", s_TryAcquireSRWLockExclusive},
    {"TryAcquireSRWLockShared", s_TryAcquireSRWLockShared}, {"InitializeConditionVariable", s_InitializeConditionVariable},
    {"SleepConditionVariableCS", s_SleepConditionVariableCS}, {"SleepConditionVariableSRW", s_SleepConditionVariableSRW},
    {"WakeConditionVariable", s_WakeConditionVariable}, {"WakeAllConditionVariable", s_WakeConditionVariable},
    {"WaitOnAddress", s_WaitOnAddress}, {"WakeByAddressSingle", k_zero}, {"WakeByAddressAll", k_zero},
    {"InitOnceExecuteOnce", s_InitOnceExecuteOnce}, {"InitOnceBeginInitialize", s_InitOnceBeginInitialize},
    {"InitOnceComplete", s_InitOnceComplete}, {"InitOnceInitialize", s_InitOnceInitialize},
    {"SwitchToThread", s_SwitchToThread}, {"FlsAlloc", s_FlsAlloc}, {"FlsGetValue", k_TlsGetValue},
    {"FlsSetValue", k_TlsSetValue}, {"FlsFree", k_one}, {"GetCurrentProcessorNumber", k_zero},
};
