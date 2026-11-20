/* M0: can Horizon give the port the guest memory port/host64/guest_memory.c asks for? A test NRO,
   no game code and no game data. It builds the real layout in one 8 GiB reservation and checks:
   - the six RAM views of one 32 MiB block, at the offsets guest_memory.c gives them;
   - commits at fixed guest addresses, as many as the EE stack slots need;
   - an access to an unmapped address reaching a handler, with the right address, and execution
     continuing after it (the port reports such a fault and ends; it never retries the access);
   - fiber_arm64.c's switch, with FPCR kept per fiber (rounding toward zero and flush to zero).
   Results go to the screen, to nxlink (stderr) and to sdmc:/dhd_m0.log. */
#include <arpa/inet.h>
#include <inttypes.h>
#include <malloc.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>
#include "fiber_switch.h"

#define GUEST_SIZE 0x200000000ull
#define RAM_SIZE 0x2000000ull
#define HUGE 0x200000ull
#define PAGE 0x1000ull
#define HOST64_PAGE 0x4000ull

static FILE *log_file;
static int nxlink_on;
static unsigned failures;
static uintptr_t base;
static Handle self;

static void say(const char *format, ...)
{
    va_list a;
    va_start(a, format);
    vprintf(format, a);
    va_end(a);
    if (nxlink_on) {
        va_start(a, format);
        vfprintf(stderr, format, a);
        va_end(a);
    }
    if (log_file) {
        va_start(a, format);
        vfprintf(log_file, format, a);
        va_end(a);
        fflush(log_file);
    }
    consoleUpdate(NULL);
}

static void result(const char *name, int ok)
{
    say("%s  %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}

static uint64_t now_ns(void) { return armTicksToNs(armGetSystemTick()); }

/* ---- the mapping recipe: heap, code memory (read/write), then any number of views ---- */

static uint64_t backing(uint64_t size, uint64_t align, Result *rc)
{
    void *heap = memalign(align, size);
    if (!heap) {
        *rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        return 0;
    }
    virtmemLock();
    void *code = virtmemFindCodeMemory(size, PAGE);
    *rc = svcMapProcessCodeMemory(self, (uint64_t)code, (uint64_t)heap, size);
    virtmemUnlock();
    if (R_FAILED(*rc)) return 0;
    *rc = svcSetProcessMemoryPermission(self, (uint64_t)code, size, Perm_Rw);
    return R_FAILED(*rc) ? 0 : (uint64_t)code;
}

static Result view(uint64_t guest, uint64_t source, uint64_t size)
{
    return svcMapProcessMemory((void *)(base + guest), self, source, size);
}

/* ---- the fault handler: maps the prepared page at the faulting address, then jumps back ---- */

alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof __nx_exception_stack;
u32 __nx_exception_ignoredebug = 1;

static volatile uint64_t fault_address, fault_source;
static volatile uint32_t fault_count, fault_desc;
static volatile Result fault_rc;
static jmp_buf fault_return;

void __libnx_exception_handler(ThreadExceptionDump *dump)
{
    fault_count++;
    fault_desc = dump->error_desc;
    fault_address = dump->far.x;
    if (fault_count == 1)
        fault_rc = svcMapProcessMemory((void *)(dump->far.x & ~(PAGE - 1)), self, fault_source, PAGE);
    /* Written here, so the log says what happened even if the process ends in this handler. */
    if (log_file) {
        MemoryInfo info = {0};
        u32 page_info;
        svcQueryMemory(&info, &page_info, dump->far.x);
        fprintf(log_file, "      fault %u: kind 0x%x, address 0x%" PRIx64 ", pc 0x%" PRIx64 ", map 0x%x, type 0x%x, perm 0x%x\n",
                fault_count, dump->error_desc, dump->far.x, dump->pc.x, fault_rc, info.type, info.perm);
        fflush(log_file);
    }
    /* libnx's return path ends in svcBreak when a handler returns: it leaves by a jump. The kernel
       has already left the exception by the time the handler runs. */
    if (fault_count > 3) svcExitProcess();
    longjmp(fault_return, 1);
}

/* ---- fibers ---- */

#define FPCR_EE ((3ull << 22) | (1ull << 24))  /* RMode toward zero, FZ: fp_mode.c's HOST_FP_EE */

static uint64_t fpcr_read(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return v;
}

static void fpcr_write(uint64_t v) { __asm__ volatile("msr fpcr, %0\n\tisb" :: "r"(v) : "memory"); }

static uint32_t third_bits(void)
{
    volatile float one = 1.0f, three = 3.0f;
    float q = one / three;
    uint32_t bits;
    memcpy(&bits, &q, 4);
    return bits;
}

static uint32_t denormal_bits(void)
{
    volatile float tiny = 1e-30f, small = 1e-10f;  /* the product, 1e-40, is denormal */
    float p = tiny * small;
    uint32_t bits;
    memcpy(&bits, &p, 4);
    return bits;
}

static void *main_fiber, *other_fiber;
static volatile unsigned fiber_bad;
static volatile uint64_t fiber_switches;

void fiber_entry(void *arg)
{
    (void)arg;
    /* The new fiber starts with its creator's mode: EE. */
    if (fpcr_read() != FPCR_EE || third_bits() != 0x3EAAAAAA || denormal_bits() != 0) fiber_bad |= 1;
    fpcr_write(0);  /* this fiber is IEEE from here on */
    for (;;) {
        if (fpcr_read() != 0 || third_bits() != 0x3EAAAAAB || denormal_bits() == 0) fiber_bad |= 2;
        fiber_switches++;
        fiber_switch_asm(&other_fiber, main_fiber);
    }
}

static void test_fibers(void)
{
    enum { STACK = 0x40000, ROUNDS = 200000 };
    char *stack = memalign(16, STACK);
    fpcr_write(FPCR_EE);
    other_fiber = fiber_frame((uintptr_t)stack, (uintptr_t)stack + STACK, NULL);
    uint64_t start = now_ns();
    for (unsigned i = 0; i < ROUNDS; i++) {
        fiber_switch_asm(&main_fiber, other_fiber);
        if (fpcr_read() != FPCR_EE || third_bits() != 0x3EAAAAAA || denormal_bits() != 0) fiber_bad |= 4;
    }
    uint64_t took = now_ns() - start;
    fpcr_write(0);
    say("      %u round trips with float checks, %" PRIu64 " ns each, bad mask %u\n", ROUNDS, took / ROUNDS, fiber_bad);
    result("fibers keep FPCR; EE mode rounds toward zero and flushes denormals",
           !fiber_bad && fiber_switches == ROUNDS);
}

/* ---- the RAM views ---- */

/* guest_memory.c's mirror regions: three complete views and three low ones (they start 4 MiB in). */
static const struct { uint64_t at, size; } views[6] = {
    {0x00400000, 0x01C00000}, {0x08000000, 0x02000000}, {0x20400000, 0x01C00000},
    {0x28000000, 0x02000000}, {0x30400000, 0x01C00000}, {0x38000000, 0x02000000},
};

static uint64_t view_offset(unsigned v) { return views[v].at & (RAM_SIZE - 1); }

static volatile uint64_t *in_view(unsigned v, uint64_t offset)
{
    return (volatile uint64_t *)(base + (views[v].at & ~(RAM_SIZE - 1)) + offset);
}

static int test_views(void)
{
    uint64_t start = now_ns();
    Result rc;
    uint64_t ram = backing(RAM_SIZE, HUGE, &rc);
    if (!ram) {
        say("      backing: 0x%x\n", rc);
        result("32 MiB block as read/write code memory", 0);
        return 0;
    }
    result("32 MiB block as read/write code memory", 1);
    for (unsigned v = 0; v < 6; v++) {
        rc = view(views[v].at, ram + view_offset(v), views[v].size);
        if (R_FAILED(rc)) {
            say("      view %u at 0x%08" PRIx64 ": 0x%x\n", v, views[v].at, rc);
            result("the six RAM views map", 0);
            return 0;
        }
    }
    say("      six views in %" PRIu64 " us\n", (now_ns() - start) / 1000);
    result("the six RAM views map", 1);

    static const uint64_t offsets[] = {0x400000, 0x400FFC, 0x1000000, 0x1FFFFF8};
    unsigned wrong = 0, checks = 0;
    for (unsigned w = 0; w < 6; w++)
        for (unsigned o = 0; o < sizeof offsets / sizeof *offsets; o++) {
            uint64_t value = 0xD00D000000000000ull | (uint64_t)w << 32 | offsets[o];
            *in_view(w, offsets[o]) = value;  /* 0x400FFC: unaligned, across a page */
            for (unsigned r = 0; r < 6; r++) {
                checks++;
                if (*in_view(r, offsets[o]) != value) wrong++;
            }
        }
    say("      %u reads, %u wrong\n", checks, wrong);
    result("a write through any view reads back through all of them", !wrong);

    /* An offset below 4 MiB is in the complete views only; a low view's window there is unmapped. */
    *in_view(1, 0x10) = 0x1234;
    result("offsets under 4 MiB are shared by the three complete views",
           *in_view(3, 0x10) == 0x1234 && *in_view(5, 0x10) == 0x1234);

    /* What a mirror access costs against canonical RAM (no game data, a plain sum). */
    volatile uint64_t *canonical = in_view(1, 0), *mirror = in_view(5, 0);
    uint64_t sum = 0, t0 = now_ns();
    for (uint64_t i = 0; i < RAM_SIZE / 8; i++) sum += canonical[i];
    uint64_t t1 = now_ns();
    for (uint64_t i = 0; i < RAM_SIZE / 8; i++) sum += mirror[i];
    uint64_t t2 = now_ns();
    say("      32 MiB read: canonical %" PRIu64 " ms, mirror %" PRIu64 " ms (sum %" PRIx64 ")\n",
        (t1 - t0) / 1000000, (t2 - t1) / 1000000, sum);
    return 1;
}

/* ---- commits at fixed guest addresses ---- */

static int commit(uint64_t guest, uint64_t size)
{
    Result rc;
    uint64_t source = backing(size, PAGE, &rc);
    if (source) rc = view(guest, source, size);
    if (R_FAILED(rc)) {
        say("      commit 0x%09" PRIx64 " + 0x%" PRIx64 ": 0x%x\n", guest, size, rc);
        return 0;
    }
    volatile uint32_t *first = (volatile uint32_t *)(base + guest), *last = (volatile uint32_t *)(base + guest + size - 4);
    *first = 0xC0DE0001;
    *last = 0xC0DE0002;
    return *first == 0xC0DE0001 && *last == 0xC0DE0002;
}

static void test_commits(void)
{
    /* The port's own layout: the module (memory_layout.h, build_wasm.py), the scratchpad, the
       hardware windows, the bridge arena and all 64 EE stack slots with their guard pages. */
    enum { STACK_BASE = 0x06000000, STACK_BYTES = 0x40000, SLOTS = 64 };
    uint64_t start = now_ns();
    unsigned done = 0;
    int ok = commit(0x70100000, 0x00D00000) && commit(0x70000000, 0x00004000) &&
             commit(0x10000000, 0x00010000) && commit(0x11000000, 0x00010000) &&
             commit(0x12000000, 0x00010000) && commit(0x07000000, 0x00800000);
    done = ok ? 6 : 0;
    for (unsigned i = 0; ok && i < SLOTS; i++) {
        ok = commit(STACK_BASE + i * STACK_BYTES + HOST64_PAGE, STACK_BYTES - HOST64_PAGE);
        done += ok;
    }
    say("      %u commits, %" PRIu64 " us each\n", done, done ? (now_ns() - start) / 1000 / done : 0);
    result("the module, hardware windows, arena and 64 stack slots commit", ok);
}

/* ---- faults ---- */

static void test_fault(void)
{
    Result rc;
    uint64_t address = base + 0x50000123ull;  /* reserved, nothing mapped */
    MemoryInfo info;
    u32 page_info;
    fault_source = backing(PAGE, PAGE, &rc);
    if (!fault_source) {
        say("      backing: 0x%x\n", rc);
        result("a fault reaches the handler and the access retries", 0);
        return;
    }
    *(volatile uint32_t *)fault_source = 0xFACE0FF;
    svcQueryMemory(&info, &page_info, address);
    say("      before: address type 0x%x, size 0x%" PRIx64 "\n", info.type, info.size);
    say("      touching an unmapped address now; if this is the log's last line, the handler failed\n");
    volatile uint32_t *at = (volatile uint32_t *)(address & ~(PAGE - 1));
    uint32_t got = 0;
    if (!setjmp(fault_return)) got = *at;  /* faults: the handler jumps back with a nonzero setjmp */
    int first_read = got != 0;
    got = *at;  /* mapped by the handler */
    say("      faults %u, kind 0x%x, address 0x%" PRIx64 ", map 0x%x, then read 0x%x\n", fault_count, fault_desc,
        fault_address, fault_rc, got);
    result("a fault reaches the handler with the right address, and the program continues",
           !first_read && fault_count == 1 && fault_address == (address & ~(PAGE - 1)) && R_SUCCEEDED(fault_rc));
    result("the page the handler mapped works", got == 0xFACE0FF);
}

/* ---- the console ---- */

static void report_system(void)
{
    u64 total = 0, used = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    AppletType type = appletGetAppletType();
    say("Downhill Revived M0 memory test\n");
    say("      applet type %d (%s), memory %" PRIu64 " MiB (used %" PRIu64 " MiB)\n", (int)type,
        type == AppletType_Application ? "title takeover" : "applet mode: expect less memory", total >> 20, used >> 20);
    u64 aslr = 0, aslr_size = 0;
    svcGetInfo(&aslr, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&aslr_size, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0);
    say("      ASLR region 0x%" PRIx64 ", %" PRIu64 " GiB; own process handle %s\n", aslr, aslr_size >> 30,
        envGetOwnProcessHandle() != INVALID_HANDLE ? "yes" : "NO");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    socketInitializeDefault();
    if (__nxlink_host.s_addr) nxlink_on = nxlinkStdioForDebug() >= 0;
    {   /* the last run's end, in case it died in the fault test */
        char line[256];
        FILE *last = fopen("sdmc:/dhd_m0.log", "r");
        if (last) {
            while (fgets(line, sizeof line, last))
                if (strstr(line, "fault") || strstr(line, "M0 ") || strstr(line, "PASS") || strstr(line, "FAIL")) say("last run: %s", line);
            fclose(last);
        }
    }
    log_file = fopen("sdmc:/dhd_m0.log", "w");

    report_system();
    self = envGetOwnProcessHandle();

    virtmemLock();
    void *space = virtmemFindAslr(GUEST_SIZE + HUGE, PAGE);
    VirtmemReservation *reservation = space ? virtmemAddReservation(space, GUEST_SIZE + HUGE) : NULL;
    virtmemUnlock();
    base = ((uintptr_t)space + HUGE - 1) & ~(HUGE - 1);
    say("      guest base 0x%" PRIx64 "\n", (uint64_t)base);
    result("8 GiB address reservation", reservation != NULL);

    if (reservation && test_views()) test_commits();
    test_fibers();
    if (reservation) test_fault();

    say("\n%s: %u failed. Log: sdmc:/dhd_m0.log. Press + to exit.\n", failures ? "M0 FAILED" : "M0 PASSED", failures);
    if (log_file) fclose(log_file);
    log_file = NULL;
    uint64_t finished = now_ns();
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        if (nxlink_on && now_ns() - finished > 3000000000ull) break;  /* a remote run ends by itself */
        consoleUpdate(NULL);
    }
    socketExit();
    consoleExit(NULL);
    return 0;
}
