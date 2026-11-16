/* SDL3's calls the port makes, on libnx (port/nx/include/SDL3/SDL.h). */
#include <SDL3/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <switch.h>

/* ---- locks: SDL's mutexes are recursive; a condition wait gives all the depth back ---- */
struct SDL_Mutex { Mutex m; Handle owner; int depth; };
struct SDL_Condition { CondVar c; };
struct SDL_Semaphore { Mutex m; CondVar c; Uint32 value; };

static Handle thread_self(void) { return threadGetCurHandle(); }

SDL_Mutex *SDL_CreateMutex(void)
{
    SDL_Mutex *m = calloc(1, sizeof *m);
    if (m) mutexInit(&m->m);
    return m;
}
void SDL_DestroyMutex(SDL_Mutex *m) { free(m); }
void SDL_LockMutex(SDL_Mutex *m)
{
    Handle self = thread_self();
    if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) == self) { m->depth++; return; }
    mutexLock(&m->m);
    __atomic_store_n(&m->owner, self, __ATOMIC_RELAXED);
    m->depth = 1;
}
void SDL_UnlockMutex(SDL_Mutex *m)
{
    if (--m->depth) return;
    __atomic_store_n(&m->owner, INVALID_HANDLE, __ATOMIC_RELAXED);
    mutexUnlock(&m->m);
}
SDL_Condition *SDL_CreateCondition(void)
{
    SDL_Condition *c = calloc(1, sizeof *c);
    if (c) condvarInit(&c->c);
    return c;
}
void SDL_DestroyCondition(SDL_Condition *c) { free(c); }
void SDL_SignalCondition(SDL_Condition *c) { condvarWakeOne(&c->c); }
void SDL_BroadcastCondition(SDL_Condition *c) { condvarWakeAll(&c->c); }
static Result wait_on(SDL_Condition *c, SDL_Mutex *m, Sint64 ms)
{
    int depth = m->depth;
    Handle self = m->owner;
    m->depth = 0;
    __atomic_store_n(&m->owner, INVALID_HANDLE, __ATOMIC_RELAXED);
    Result r = ms < 0 ? condvarWait(&c->c, &m->m) : condvarWaitTimeout(&c->c, &m->m, (u64)ms * 1000000);
    __atomic_store_n(&m->owner, self, __ATOMIC_RELAXED);
    m->depth = depth;
    return r;
}
void SDL_WaitCondition(SDL_Condition *c, SDL_Mutex *m) { wait_on(c, m, -1); }
bool SDL_WaitConditionTimeout(SDL_Condition *c, SDL_Mutex *m, Sint32 ms) { return R_SUCCEEDED(wait_on(c, m, ms)); }

SDL_Semaphore *SDL_CreateSemaphore(Uint32 value)
{
    SDL_Semaphore *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    mutexInit(&s->m);
    condvarInit(&s->c);
    s->value = value;
    return s;
}
void SDL_DestroySemaphore(SDL_Semaphore *s) { free(s); }
void SDL_SignalSemaphore(SDL_Semaphore *s)
{
    mutexLock(&s->m);
    s->value++;
    condvarWakeOne(&s->c);
    mutexUnlock(&s->m);
}
bool SDL_WaitSemaphoreTimeout(SDL_Semaphore *s, Sint32 ms)
{
    u64 end = ms < 0 ? 0 : armGetSystemTick() + armNsToTicks((u64)ms * 1000000);
    bool got = true;
    mutexLock(&s->m);
    while (!s->value) {
        if (ms < 0) { condvarWait(&s->c, &s->m); continue; }
        u64 now = armGetSystemTick();
        if (now >= end) { got = false; break; }
        condvarWaitTimeout(&s->c, &s->m, armTicksToNs(end - now));
    }
    if (got) s->value--;
    mutexUnlock(&s->m);
    return got;
}
void SDL_WaitSemaphore(SDL_Semaphore *s) { SDL_WaitSemaphoreTimeout(s, -1); }
Uint32 SDL_GetSemaphoreValue(SDL_Semaphore *s) { return __atomic_load_n(&s->value, __ATOMIC_RELAXED); }

/* A spinlock's holder can be a lower priority on this same core, which spinning would never let run. */
void nx_spinlock_sleep(void) { svcSleepThread(50000); }

/* ---- threads: where each of the port's threads runs (three cores for an application) ---- */
struct SDL_Thread { Thread t; SDL_ThreadFunction fn; void *arg; int status; };

typedef struct { const char *name; int priority, core; u32 mask; } Placement;
enum { ANY_CORE = 7 };
/* A lower number is a higher priority. Only 0x3B is time-sliced: at any other a busy thread keeps
   its core from threads of its priority and below. A woken thread does not preempt one of its own
   priority, so gs and native, which wait on each other, stand one above the other: the picture's
   thread first, as it is what frees the producer. A thread made for one core stays there; with
   ANY_CORE (core: only the one it prefers) the kernel moves it to a core that has gone idle, so
   these two also get what the game and vu leave free. Sound stays first on its core. */
static const Placement placements[] = {
    {"game", 0x2C, 0, 1 << 0}, {"vu", 0x2C, 1, 1 << 1}, {"native", 0x2D, 2, ANY_CORE}, {"gs", 0x2E, 2, ANY_CORE},
    {"nx audio", 0x2A, 2, 1 << 2}, {"movie", 0x3B, 2, ANY_CORE},
};

/* ---- DHD_NX_SAMPLE=first-last (a test tool): what each busy thread is doing, by the clock ----
   Between those fields a thread pauses each of them about every millisecond and notes its pc and lr
   (a thread blocked in the kernel shows as its svc stub, so waits count too), the time and the field.
   The result is samples.bin beside dhd.log, for port/tools/nx_samples.py. */
extern uint64_t kernel_vsync_count;
static void thread_entry(void *arg);

enum { SAMPLED = 4, SAMPLE_MAX = 400000 };
static const char *const sampled_names[SAMPLED] = {"game", "vu", "gs", "native"};
static Handle sampled[SAMPLED];
typedef struct { u32 tick, field; s32 pc, lr; u32 thread; } Sample;

static void sampler_register(const char *name, Handle handle)
{
    for (int i = 0; name && i < SAMPLED; i++)
        if (!strcmp(name, sampled_names[i])) sampled[i] = handle;
}

static void sampler_run(void *arg)
{
    const char *window = arg;
    unsigned long long first = 0, last = 0;
    if (sscanf(window, "%llu-%llu", &first, &last) != 2) return;
    Sample *samples = malloc(SAMPLE_MAX * sizeof *samples);
    if (!samples) return;
    u32 count = 0;
    u64 anchor = (u64)(uintptr_t)thread_entry;
    while (*(volatile uint64_t *)&kernel_vsync_count < first) svcSleepThread(20000000);
    u64 start = armGetSystemTick();
    for (;;) {
        u64 field = *(volatile uint64_t *)&kernel_vsync_count;
        if (field > last || count + SAMPLED > SAMPLE_MAX) break;
        for (u32 i = 0; i < SAMPLED; i++) {
            if (!sampled[i] || R_FAILED(svcSetThreadActivity(sampled[i], ThreadActivity_Paused))) continue;
            ThreadContext c;
            Result r = svcGetThreadContext3(&c, sampled[i]);
            svcSetThreadActivity(sampled[i], ThreadActivity_Runnable);
            if (R_SUCCEEDED(r))
                samples[count++] = (Sample){(u32)((armGetSystemTick() - start) / 192), (u32)field, (s32)(c.pc.x - anchor),
                                            (s32)(c.lr - anchor), i};
        }
        svcSleepThread(1000000);
    }
    FILE *f = fopen("sdmc:/switch/downhill-revived/samples.bin", "wb");
    if (f) {
        u64 header[2] = {0x31534d5044ull, anchor};  /* "DPMS1": the tick is in units of 10 us */
        fwrite(header, sizeof header, 1, f);
        fwrite(samples, sizeof *samples, count, f);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    fprintf(stderr, "nx sampler: %u samples\n", (unsigned)count);
    free(samples);
}

/* dhd.ini's lines arrive after the start, so nx_host.c asks once a second. */
void nx_sampler_poll(void)
{
    static int started;
    static Thread thread;
    const char *window = started ? NULL : getenv("DHD_NX_SAMPLE");
    if (!window || !*window) return;
    started = 1;
    /* above the threads it pauses; on the core with the least to lose */
    if (R_SUCCEEDED(threadCreate(&thread, sampler_run, (void *)window, NULL, 0x10000, 0x29, 2))) threadStart(&thread);
}

/* Thread-local slots with destructors (SDL_SetTLS): host_threads.c ends a named thread's entry
   from its destructor. They run on the thread itself, after its function returns. */
enum { TLS_SLOTS = 8 };
static int tls_next;
static __thread struct { const void *value; SDL_TLSDestructorCallback dtor; } tls[TLS_SLOTS];
void *SDL_GetTLS(SDL_TLSID *id)
{
    int slot = __atomic_load_n(id, __ATOMIC_ACQUIRE);
    return slot ? (void *)tls[slot - 1].value : NULL;
}
bool SDL_SetTLS(SDL_TLSID *id, const void *value, SDL_TLSDestructorCallback dtor)
{
    int slot = __atomic_load_n(id, __ATOMIC_ACQUIRE);
    if (!slot) {
        int made = __atomic_add_fetch(&tls_next, 1, __ATOMIC_ACQ_REL);
        int none = 0;
        if (made > TLS_SLOTS) return false;
        slot = __atomic_compare_exchange_n(id, &none, made, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? made : none;
    }
    tls[slot - 1].value = value, tls[slot - 1].dtor = dtor;
    return true;
}

static void thread_entry(void *arg)
{
    SDL_Thread *t = arg;
    t->status = t->fn(t->arg);
    for (int i = 0; i < TLS_SLOTS; i++)
        if (tls[i].dtor && tls[i].value) tls[i].dtor((void *)tls[i].value);
}

SDL_Thread *nx_thread_create(SDL_ThreadFunction fn, const char *name, void *arg, size_t stack)
{
    SDL_Thread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn, t->arg = arg;
    int priority = 0x3B, core = 2;
    u32 mask = ANY_CORE;
    for (size_t i = 0; i < sizeof placements / sizeof *placements; i++)
        if (name && !strcmp(name, placements[i].name)) priority = placements[i].priority, core = placements[i].core, mask = placements[i].mask;
    Result r = threadCreate(&t->t, thread_entry, t, NULL, stack, priority, core);
    if (R_SUCCEEDED(r) && mask != 1u << core) {
        Result widened = svcSetThreadCoreMask(t->t.handle, core, mask);
        if (R_FAILED(widened)) fprintf(stderr, "nx: thread %s stays on core %d: 0x%x\n", name ? name : "?", core, widened);
    }
    if (R_FAILED(r) || R_FAILED(threadStart(&t->t))) {
        fprintf(stderr, "nx: thread %s: 0x%x\n", name ? name : "?", r);
        free(t);
        return NULL;
    }
    sampler_register(name, t->t.handle);
    return t;
}
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *arg)
{
    return nx_thread_create(fn, name, arg, 1024 * 1024);
}
void SDL_WaitThread(SDL_Thread *t, int *status)
{
    if (!t) return;
    threadWaitForExit(&t->t);
    if (status) *status = t->status;
    threadClose(&t->t);
    free(t);
}
void SDL_DetachThread(SDL_Thread *t) { (void)t; }  /* its stack and record stay until the process ends */
bool SDL_SetCurrentThreadPriority(SDL_ThreadPriority p) { (void)p; return true; }  /* placements decide */
SDL_ThreadID SDL_GetCurrentThreadID(void) { return thread_self(); }

/* ---- time ---- */
Uint64 SDL_GetPerformanceCounter(void) { return armGetSystemTick(); }
Uint64 SDL_GetPerformanceFrequency(void) { return armGetSystemTickFreq(); }
Uint64 SDL_GetTicksNS(void) { return armTicksToNs(armGetSystemTick()); }
Uint64 SDL_GetTicks(void) { return SDL_GetTicksNS() / 1000000; }
void SDL_Delay(Uint32 ms) { svcSleepThread((s64)ms * 1000000); }
/* host_wait_wall sleeps to a millisecond before its time and spins the rest: most of that spin kept
   the game's core from gs and native for nothing. This sleep ends 0.2 ms before it instead. */
void SDL_DelayNS(Uint64 ns) { svcSleepThread((s64)ns + 800000); }
void SDL_DelayPrecise(Uint64 ns) { svcSleepThread((s64)ns); }
bool SDL_GetCurrentTime(SDL_Time *t) { *t = (SDL_Time)time(NULL) * 1000000000; return true; }
bool SDL_TimeToDateTime(SDL_Time t, SDL_DateTime *dt, bool local)
{
    (void)local;
    time_t seconds = (time_t)(t / 1000000000);
    struct tm *m = gmtime(&seconds);
    if (!m) return false;
    *dt = (SDL_DateTime){m->tm_year + 1900, m->tm_mon + 1, m->tm_mday, m->tm_hour, m->tm_min, m->tm_sec, 0, m->tm_wday, 0};
    return true;
}

/* ---- the system ---- */
bool SDL_InitSubSystem(SDL_InitFlags f) { (void)f; return true; }
void SDL_Quit(void) {}
const char *SDL_GetError(void) { return strerror(errno); }
int SDL_GetNumLogicalCPUCores(void) { return 3; }
int SDL_GetSystemRAM(void) { return 4096; }
bool SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return true; }
bool SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, void *window)
{
    (void)flags; (void)window;
    /* the error applet: what a player sees before the game ends */
    ErrorApplicationConfig config;
    errorApplicationCreate(&config, title, message);
    errorApplicationShow(&config);
    return true;
}
char *SDL_GetClipboardText(void) { return calloc(1, 1); }
bool SDL_SetClipboardText(const char *text) { (void)text; return false; }

/* ---- files ---- */
bool SDL_GetPathInfo(const char *path, SDL_PathInfo *info)
{
    struct stat st;
    if (stat(path, &st)) return false;
    if (info) {
        info->type = S_ISDIR(st.st_mode) ? SDL_PATHTYPE_DIRECTORY : S_ISREG(st.st_mode) ? SDL_PATHTYPE_FILE : SDL_PATHTYPE_OTHER;
        info->size = (Uint64)st.st_size;
        info->create_time = info->modify_time = info->access_time = (SDL_Time)st.st_mtime * 1000000000;
    }
    return true;
}
bool SDL_CreateDirectory(const char *path)
{
    char *copy = strdup(path);
    if (!copy) return false;
    for (char *p = strchr(copy, '/'); p; p = strchr(p + 1, '/')) {
        if (p == copy || p[-1] == ':') continue;  /* the device, sdmc:/ */
        *p = 0;
        mkdir(copy, 0777);
        *p = '/';
    }
    bool ok = !mkdir(copy, 0777) || errno == EEXIST;
    free(copy);
    return ok;
}
bool SDL_EnumerateDirectory(const char *path, SDL_EnumerateDirectoryCallback fn, void *user)
{
    DIR *d = opendir(path);
    if (!d) return false;
    bool ok = true;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        SDL_EnumerationResult r = fn(user, path, e->d_name);
        if (r != SDL_ENUM_CONTINUE) { ok = r == SDL_ENUM_SUCCESS; break; }
    }
    closedir(d);
    return ok;
}

/* ---- sound: a thread pulls the game's samples and queues them on audout ---- */
enum { AUDIO_RATE = 48000, AUDIO_FRAMES = 1024, AUDIO_BYTES = AUDIO_FRAMES * 4, AUDIO_BUFFERS = 3, AUDIO_RING = 1 << 16 };
struct SDL_AudioStream { SDL_AudioStreamCallback fn; void *user; volatile int started; };
static SDL_AudioStream audio_stream;
static uint8_t audio_ring[AUDIO_RING];
static volatile uint32_t ring_in, ring_out;

bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *data, int bytes)
{
    (void)s;
    uint32_t at = ring_in, room = AUDIO_RING - (at - ring_out), take = (uint32_t)bytes < room ? (uint32_t)bytes : room;
    uint32_t start = at & (AUDIO_RING - 1), first = take < AUDIO_RING - start ? take : AUDIO_RING - start;
    memcpy(audio_ring + start, data, first);
    memcpy(audio_ring, (const uint8_t *)data + first, take - first);
    ring_in = at + take;
    return true;
}
int SDL_GetAudioStreamQueued(SDL_AudioStream *s) { (void)s; return (int)(ring_in - ring_out); }

static int audio_thread(void *arg)
{
    SDL_AudioStream *s = arg;
    static AudioOutBuffer buffers[AUDIO_BUFFERS];
    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        buffers[i].next = NULL;
        buffers[i].buffer = memalign(0x1000, (AUDIO_BYTES + 0xFFF) & ~0xFFF);
        buffers[i].buffer_size = (AUDIO_BYTES + 0xFFF) & ~0xFFF;
        buffers[i].data_size = AUDIO_BYTES;
        buffers[i].data_offset = 0;
        memset(buffers[i].buffer, 0, buffers[i].buffer_size);
        audoutAppendAudioOutBuffer(&buffers[i]);
    }
    for (;;) {
        AudioOutBuffer *done = NULL;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&done, &count, UINT64_MAX)) || !done) continue;
        for (u32 n = 0; n < count; n++) {
            AudioOutBuffer *b = done + n;
            s->fn(s->user, s, AUDIO_BYTES, AUDIO_BYTES);
            uint8_t *out = b->buffer;
            uint32_t at = ring_out, have = ring_in - at, take = have < AUDIO_BYTES ? have : AUDIO_BYTES;
            uint32_t start = at & (AUDIO_RING - 1), first = take < AUDIO_RING - start ? take : AUDIO_RING - start;
            memcpy(out, audio_ring + start, first);
            memcpy(out + first, audio_ring, take - first);
            memset(out + take, 0, AUDIO_BYTES - take);
            ring_out = at + take;
            armDCacheFlush(out, AUDIO_BYTES);
            audoutAppendAudioOutBuffer(b);
        }
    }
    return 0;
}
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID id, const SDL_AudioSpec *spec, SDL_AudioStreamCallback fn, void *user)
{
    (void)id;
    if (spec->freq != AUDIO_RATE || spec->channels != 2) { errno = EINVAL; return NULL; }
    if (R_FAILED(audoutInitialize())) { errno = EIO; return NULL; }
    if (R_FAILED(audoutStartAudioOut())) { errno = EIO; return NULL; }
    audio_stream.fn = fn, audio_stream.user = user;
    return &audio_stream;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *s)
{
    if (!s->started) {
        s->started = 1;
        SDL_DetachThread(SDL_CreateThread(audio_thread, "nx audio", s));
    }
    return true;
}
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *s) { (void)s; return 1; }
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID id, SDL_AudioSpec *spec, int *frames)
{
    (void)id;
    if (spec) *spec = (SDL_AudioSpec){SDL_AUDIO_S16, 2, AUDIO_RATE};
    if (frames) *frames = AUDIO_FRAMES;
    return true;
}
const char *SDL_GetCurrentAudioDriver(void) { return "audout"; }

/* ---- the window ---- */
struct SDL_Window { int unused; };
static SDL_Window the_window;
static SDL_DisplayMode the_mode;
/* The screen for the whole run: the mode at the start, docked or handheld. */
void nx_screen_size(int *w, int *h)
{
    static int docked = -1;
    if (docked < 0) docked = appletGetOperationMode() == AppletOperationMode_Console;
    *w = docked ? 1920 : 1280;
    *h = docked ? 1080 : 720;
}
SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags f) { (void)title; (void)w; (void)h; (void)f; return &the_window; }
SDL_DisplayID SDL_GetPrimaryDisplay(void) { return 1; }
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *w) { (void)w; return 1; }
const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID d)
{
    (void)d;
    int screen_w, screen_h;
    nx_screen_size(&screen_w, &screen_h);
    the_mode = (SDL_DisplayMode){1, 0, screen_w, screen_h, 1.0f, 60.0f};
    return &the_mode;
}
SDL_DisplayMode **SDL_GetFullscreenDisplayModes(SDL_DisplayID d, int *count) { (void)d; if (count) *count = 0; return NULL; }
bool SDL_GetDisplayBounds(SDL_DisplayID d, SDL_Rect *r)
{
    (void)d;
    int screen_w, screen_h;
    nx_screen_size(&screen_w, &screen_h);
    *r = (SDL_Rect){0, 0, screen_w, screen_h};
    return true;
}
bool SDL_GetDisplayUsableBounds(SDL_DisplayID d, SDL_Rect *r) { return SDL_GetDisplayBounds(d, r); }
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *w) { (void)w; return SDL_WINDOW_FULLSCREEN; }
Uint32 SDL_GetWindowID(SDL_Window *w) { (void)w; return 1; }
float SDL_GetWindowPixelDensity(SDL_Window *w) { (void)w; return 1.0f; }
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *w) { (void)w; return 1; }
bool SDL_GetWindowSizeInPixels(SDL_Window *w, int *x, int *y)
{
    (void)w;
    int screen_w, screen_h;
    nx_screen_size(&screen_w, &screen_h);
    if (x) *x = screen_w;
    if (y) *y = screen_h;
    return true;
}
bool SDL_GetWindowBordersSize(SDL_Window *w, int *a, int *b, int *c, int *d) { (void)w; if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; if (d) *d = 0; return true; }
const char *SDL_GetCurrentVideoDriver(void) { return "nx"; }

/* ---- events ---- */
static SDL_EventFilter watch;
static void *watch_user;
bool SDL_AddEventWatch(SDL_EventFilter fn, void *user) { watch = fn, watch_user = user; return true; }
bool SDL_PushEvent(SDL_Event *e) { (void)e; return true; }
void nx_send_event(Uint32 type)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    if (watch) watch(watch_user, &e);
}
