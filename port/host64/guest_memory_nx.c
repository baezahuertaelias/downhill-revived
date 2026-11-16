/* Horizon mapping for guest_memory.c: the 8 GiB reservation is an address range libnx's virtual
   memory manager keeps free; nothing is mapped there until asked. A mapping is heap memory turned
   into process code memory (read/write), then mapped at the guest address with
   svcMapProcessMemory, which may be done several times: that is how the six RAM views share one
   block (port/nx/spike_memory proves each step on the console). Horizon has no mprotect for these
   pages, so a commit maps them, and a page that isn't committed faults into the reporter. */
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include "core_host.h"
#include "guest_memory_os.h"
#include "memory_layout.h"

#define GUEST_SIZE 0x200000000ull
#define RAM_SIZE 0x2000000ull
#define HUGE 0x200000ull
/* The bridge arena is committed page by page as it is used: all at once here, one kernel mapping. */
#define ARENA 0x07000000ull
#define ARENA_SIZE 0x00800000ull

static Handle self;
static uint64_t ram;
static uint32_t committed[(0x100000000ull / HOST64_PAGE) / 32];

void os_fail(const char *what)
{
    fprintf(stderr, "host64 memory: %s\n", what);
    fflush(stderr);
    abort();
}

/* size bytes of zeroed memory, mapped read/write somewhere of the kernel's choice; 0 on failure. */
static uint64_t backing(uint64_t size)
{
    void *heap = memalign(size >= HUGE ? HUGE : 0x1000, size);
    if (!heap) return 0;
    virtmemLock();
    void *code = virtmemFindCodeMemory(size, 0x1000);
    Result r = svcMapProcessCodeMemory(self, (uint64_t)code, (uint64_t)heap, size);
    virtmemUnlock();
    if (R_FAILED(r) || R_FAILED(r = svcSetProcessMemoryPermission(self, (uint64_t)code, size, Perm_Rw))) {
        fprintf(stderr, "host64 memory: backing of 0x%" PRIx64 " bytes: 0x%x\n", size, r);
        return 0;
    }
    memset((void *)code, 0, size);
    return (uint64_t)code;
}

uintptr_t os_reserve(void)
{
    self = envGetOwnProcessHandle();
    virtmemLock();
    void *space = virtmemFindAslr(GUEST_SIZE + HUGE, 0x1000);
    VirtmemReservation *held = space ? virtmemAddReservation(space, GUEST_SIZE + HUGE) : NULL;
    virtmemUnlock();
    if (!held || !(ram = backing(RAM_SIZE))) return 0;
    return ((uintptr_t)space + HUGE - 1) & ~(uintptr_t)(HUGE - 1);
}
int os_split(uintptr_t at, uint64_t size) { (void)at; (void)size; return 1; }
int os_map_ram(uintptr_t at, uint64_t size, uint64_t file_offset)
{
    Result r = svcMapProcessMemory((void *)at, self, ram + file_offset, size);
    if (R_FAILED(r)) fprintf(stderr, "host64 memory: RAM view at guest 0x%" PRIx64 ": 0x%x\n", (uint64_t)(at - guest_base), r);
    return R_SUCCEEDED(r);
}
int os_private(uintptr_t at, uint64_t size) { (void)at; (void)size; return 1; }

static int is_committed(uint64_t page) { return committed[page / 32] >> (page % 32) & 1; }

/* Commits what isn't committed yet in [at, at + size): contents already there stay. */
int os_commit(uintptr_t at, uint64_t size)
{
    uint64_t lo = at - guest_base, hi = lo + size;
    if (lo >= ARENA && hi <= ARENA + ARENA_SIZE) lo = ARENA, hi = ARENA + ARENA_SIZE;
    for (uint64_t page = lo / HOST64_PAGE; page < hi / HOST64_PAGE;) {
        if (is_committed(page)) { page++; continue; }
        uint64_t end = page + 1;
        while (end < hi / HOST64_PAGE && !is_committed(end)) end++;
        uint64_t bytes = (end - page) * HOST64_PAGE, source = backing(bytes);
        if (!source) return 0;
        Result r = svcMapProcessMemory((void *)(guest_base + page * HOST64_PAGE), self, source, bytes);
        if (R_FAILED(r)) {
            fprintf(stderr, "host64 memory: commit at guest 0x%" PRIx64 ": 0x%x\n", page * HOST64_PAGE, r);
            return 0;
        }
        for (uint64_t p = page; p < end; p++) committed[p / 32] |= 1u << (p % 32);
        page = end;
    }
    return 1;
}
void os_decommit(const void *at, uint64_t size) { (void)at; (void)size; }  /* failed growth only: stays mapped */
int os_unmap_view(void *at, uint64_t size) { (void)at; (void)size; return 1; }  /* the process is ending */
int os_release(void *at, uint64_t size) { (void)at; (void)size; return 1; }
void os_backing_close(void) {}
int os_fault_install(void) { return 1; }  /* the handler is libnx's hook, host_nx.c */
void os_fault_remove(void) {}
