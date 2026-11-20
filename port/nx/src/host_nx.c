/* The Switch's host of host_api.h, host_posix.c's counterpart: what has no portable answer. Guest
   pages can't be protected on Horizon, so the deferred store-image reads stay off: the fault guard
   says no and graphics.c reads at once. */
#include <SDL3/SDL.h>
#include "host_api.h"
#include "host_os.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>
#include "../../host64/core_host.h"

#define NX_FOLDER "sdmc:/switch/downhill-revived/"

unsigned long host_last_error(void) { return (unsigned long)errno; }

unsigned host_exe_path(char *out, unsigned size)
{
    int k = snprintf(out, size, NX_FOLDER "dhd64");
    return k > 0 && (unsigned)k < size ? (unsigned)k : 0;
}

void host_log_drain(void);
void host_exit(int code)
{
    host_log_drain();
    fflush(stdout);
    fflush(stderr);
    exit(code);
}

/* MXCSR's words to and from FPCR, as host_posix.c (finding 091 AND2). */
static const unsigned rc_to_rmode[4] = {0, 2, 1, 3}, rmode_to_rc[4] = {0, 2, 1, 3};
unsigned host_fp_csr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return 0x1F80u | rmode_to_rc[(v >> 22) & 3] << 13 | ((v >> 24) & 1 ? 0x8040u : 0);
}
void host_fp_set_csr(unsigned csr)
{
    uint64_t now, v = (uint64_t)rc_to_rmode[(csr >> 13) & 3] << 22 | (csr & 0x8040u ? 1ull << 24 : 0);
    __asm__ volatile("mrs %0, fpcr" : "=r"(now));
    if (now != v) __asm__ volatile("msr fpcr, %0\n\tisb" :: "r"(v) : "memory");
}

uint64_t host_cycles(void) { return armTicksToNs(armGetSystemTick()); }

void host_putenv_s(const char *name, const char *value)
{
    if (!value || !*value) unsetenv(name);
    else setenv(name, value, 1);
}
void host_putenv(const char *text)
{
    const char *eq = strchr(text, '=');
    char name[256];
    if (!eq || eq == text || (size_t)(eq - text) >= sizeof name) return;
    memcpy(name, text, (size_t)(eq - text));
    name[eq - text] = 0;
    host_putenv_s(name, eq + 1);
}

int host_mem_protect(uint32_t at, uint32_t size, int protection) { (void)at; (void)size; (void)protection; return 0; }
int host_fault_guard(int (*on_fault)(uint32_t address)) { (void)on_fault; return 0; }
int host_fault_guard_here(void) { return 0; }

void host_code_range(uintptr_t *lo, uintptr_t *hi)
{
    extern char __start__[], __rodata_start[];
    *lo = (uintptr_t)__start__;
    *hi = (uintptr_t)__rodata_start;
}
void host_memory_regions(void (*visit)(uintptr_t base, uintptr_t end, int host_memory)) { (void)visit; }

static void (*heartbeat_beat)(void);
/* A thread of the SDL shim, so that EXIT GAME ends it at its delay (nx_threads_end, sdl_nx.c). */
static int heartbeat_run(void *unused)
{
    (void)unused;
    for (;;) {
        SDL_Delay(30000);
        heartbeat_beat();
    }
    return 0;
}
void host_heartbeat_start(void (*beat)(void))
{
    heartbeat_beat = beat;
    SDL_DetachThread(nx_thread_create(heartbeat_run, "heartbeat", NULL, 0x10000));
}
uintptr_t host_heartbeat_eip(void) { return 0; }

void host_os_system(char *out, unsigned size)
{
    SetSysFirmwareVersion v;
    char fw[32] = "?";
    if (R_SUCCEEDED(setsysInitialize())) {
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&v))) snprintf(fw, sizeof fw, "%s", v.display_version);
        setsysExit();
    }
    snprintf(out, size, "Horizon %s, Tegra X1 (aarch64)", fw);
}
/* host.c's log_start opens dhd.log "w" when stderr isn't a file (nxlink's socket) and then finds no
   pipe: the file keeps the last run's log. */
FILE *host_os_fopen(const char *path, const char *mode)
{
    size_t n = strlen(path);
    if (n >= 7 && !strcmp(path + n - 7, "dhd.log") && mode[0] == 'w') mode = "a";
    return fopen(path, mode);
}

/* host.c's log pipe and its dump signal: the log goes straight to its file here (nx_host.c). */
int pipe(int fds[2]) { (void)fds; errno = ENOSYS; return -1; }
int pthread_kill(pthread_t t, int sig) { (void)t; (void)sig; return 0; }

/* ---- crashes: the report, and the end. libnx's one exception stack is for this only. ---- */
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof __nx_exception_stack;
u32 __nx_exception_ignoredebug = 1;

void __libnx_exception_handler(ThreadExceptionDump *d)
{
    /* The stack above is one: a second fault gives the first's report a moment, then ends the process. */
    static int entered;
    if (__atomic_exchange_n(&entered, 1, __ATOMIC_SEQ_CST)) {
        svcSleepThread(3000000000ll);
        svcExitProcess();
    }
    extern char __start__[];
    uintptr_t code = (uintptr_t)__start__;
    uint64_t far = d->far.x;
    fprintf(stderr, "crash: kind 0x%x at 0x%" PRIx64 "%s", d->error_desc, far,
            guest_base && far >= guest_base && far - guest_base < 0x200000000ull ? " (guest " : "");
    if (guest_base && far >= guest_base && far - guest_base < 0x200000000ull) fprintf(stderr, "0x%" PRIx64 ")", far - guest_base);
    fprintf(stderr, "\n  pc +0x%" PRIx64 " lr +0x%" PRIx64 " sp 0x%" PRIx64 " (aarch64-none-elf-addr2line -e dhd.elf)\n",
            d->pc.x - code, d->lr.x - code, d->sp.x);
    uint64_t *frame = (uint64_t *)d->fp.x;
    for (int i = 0; i < 24 && frame && !((uintptr_t)frame & 7); i++) {
        MemoryInfo info;
        u32 page;
        svcQueryMemory(&info, &page, (uintptr_t)frame);
        if (!(info.perm & Perm_R)) break;
        fprintf(stderr, "  from +0x%" PRIx64 "\n", frame[1] - code);
        frame = (uint64_t *)frame[0];
    }
    host_log_drain();
    fflush(stderr);
    fsync(fileno(stderr));
    svcExitProcess();
}
