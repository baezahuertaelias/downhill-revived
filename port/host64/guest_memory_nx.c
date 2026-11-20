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
static VirtmemReservation *held;

/* What each mapping took, undone by nx_guest_memory_release: a backing is heap memory become code
   memory, a view maps a backing's pages at a guest address. */
typedef struct { uint64_t code, size; void *heap; } Backing;
typedef struct { uint64_t at, source, size; } View;
static Backing *backings;
static View *views;
static size_t backing_count, view_count, backing_room, view_room;
static int untracked;  /* a record that could not be kept: the release can't be complete */

/* Room in *list for one more record; 0 when it could not grow. */
static int room_for(void **list, size_t count, size_t *room, size_t size)
{
    if (count < *room) return 1;
    size_t want = *room ? *room * 2 : 64;
    void *more = realloc(*list, want * size);
    if (!more) {
        untracked = 1;
        return 0;
    }
    *list = more, *room = want;
    return 1;
}
static void view_add(uint64_t at, uint64_t source, uint64_t size)
{
    if (room_for((void **)&views, view_count, &view_room, sizeof *views)) views[view_count++] = (View){at, source, size};
}
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
    if (room_for((void **)&backings, backing_count, &backing_room, sizeof *backings))
        backings[backing_count++] = (Backing){(uint64_t)code, size, heap};
    memset((void *)code, 0, size);
    return (uint64_t)code;
}

uintptr_t os_reserve(void)
{
    self = envGetOwnProcessHandle();
    virtmemLock();
    void *space = virtmemFindAslr(GUEST_SIZE + HUGE, 0x1000);
    held = space ? virtmemAddReservation(space, GUEST_SIZE + HUGE) : NULL;
    virtmemUnlock();
    if (!held || !(ram = backing(RAM_SIZE))) return 0;
    return ((uintptr_t)space + HUGE - 1) & ~(uintptr_t)(HUGE - 1);
}
int os_split(uintptr_t at, uint64_t size) { (void)at; (void)size; return 1; }
int os_map_ram(uintptr_t at, uint64_t size, uint64_t file_offset)
{
    Result r = svcMapProcessMemory((void *)at, self, ram + file_offset, size);
    if (R_FAILED(r)) fprintf(stderr, "host64 memory: RAM view at guest 0x%" PRIx64 ": 0x%x\n", (uint64_t)(at - guest_base), r);
    else view_add(at, ram + file_offset, size);
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
        view_add(guest_base + page * HOST64_PAGE, source, bytes);
        for (uint64_t p = page; p < end; p++) committed[p / 32] |= 1u << (p % 32);
        page = end;
    }
    return 1;
}
void os_decommit(const void *at, uint64_t size) { (void)at; (void)size; }  /* failed growth only: stays mapped */
int os_unmap_view(void *at, uint64_t size) { (void)at; (void)size; return 1; }  /* the process is ending */
int os_release(void *at, uint64_t size) { (void)at; (void)size; return 1; }
void os_backing_close(void) {}

/* EXIT GAME's way back to the Homebrew Menu (nx_host.c), once no thread runs guest code: every view
   unmapped, every backing heap memory again and freed, the range given back. The loader reloads the
   menu in this process and takes its heap back; code memory left there stopped it with
   InvalidCurrentMemory (finding 270). 0 when something stays mapped. */
int nx_guest_memory_release(void)
{
    int ok = !untracked;
    for (size_t i = view_count; i--;) {
        Result r = svcUnmapProcessMemory((void *)views[i].at, self, views[i].source, views[i].size);
        if (R_FAILED(r)) {
            fprintf(stderr, "host64 memory: unmap at 0x%" PRIx64 ": 0x%x\n", views[i].at, r);
            ok = 0;
        }
    }
    for (size_t i = backing_count; i--;) {
        Result r = svcUnmapProcessCodeMemory(self, backings[i].code, (uint64_t)backings[i].heap, backings[i].size);
        if (R_FAILED(r)) {
            fprintf(stderr, "host64 memory: backing at 0x%" PRIx64 ": 0x%x\n", backings[i].code, r);
            ok = 0;
            continue;
        }
        free(backings[i].heap);
    }
    if (held) {
        virtmemLock();
        virtmemRemoveReservation(held);
        virtmemUnlock();
        held = NULL;
    }
    free(views), free(backings);
    views = NULL, backings = NULL, view_count = backing_count = view_room = backing_room = 0;
    return ok;
}
int os_fault_install(void) { return 1; }  /* the handler is libnx's hook, host_nx.c */
void os_fault_remove(void) {}
