/* The Switch's main thread, sdl_host.c's counterpart, and the program's start, android_main.c's: the
   game runs on a thread of its own and this one polls the console's events and the controllers
   every 4 ms. Everything the player keeps lives in sdmc:/switch/downhill-revived/: the disc image
   SCUS-97177.iso, dhd.ini, saves and dhd.log. */
#include <SDL3/SDL.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>
#include "host_api.h"
#include "dhd_settings.h"
#include "../../hle/hle.h"
#include "../../gs/present.h"

#define NX_FOLDER "sdmc:/switch/downhill-revived/"
#define NX_DISC NX_FOLDER "SCUS-97177.iso"
#define NX_INI NX_FOLDER "dhd.ini"

static SDL_AtomicInt game_running;
static Handle main_thread;
static Mutex slot_lock;
static void (*request_fn)(void *);
static void *request_arg;
static SDL_Semaphore *request_done;

static void run_request(void)
{
    mutexLock(&slot_lock);
    void (*fn)(void *) = request_fn;
    void *arg = request_arg;
    SDL_Semaphore *done = request_done;
    request_fn = NULL;
    mutexUnlock(&slot_lock);
    if (fn) {
        fn(arg);
        SDL_SignalSemaphore(done);
    }
}

typedef struct { int (*fn)(void *); void *arg; } GameStart;
static int game_thread(void *start)
{
    GameStart *g = start;
    int code = g->fn(g->arg);
    SDL_SetAtomicInt(&game_running, 0);
    return code;
}

/* ---- the end ----
   Any thread may end the game: exit() on the game's thread (the close at its next field, EXIT GAME)
   runs the game's atexit list and then last_exit_step, the first registered. Going back to the
   Homebrew Menu would unmap the code under the threads still running, on the wrong thread too, so
   the running game always ends the process: exit mode 1 is the system's own exit (libnx's
   _appletExitProcess), which is also the answer the system waits for after an exit request. */
extern u32 __nx_applet_exit_mode;  /* libnx's */
void gamepad_nx_hold(int hold);    /* gamepad_nx.c */
static int game_started, back_to_menu;
static volatile int leaving, main_parked, close_wanted;

static void flush_all(void)
{
    fflush(stdout);
    fflush(stderr);
    fsync(fileno(stderr));
    fsdevCommitDevice("sdmc");
}

static int claim_leaving(void)
{
    int expected = 0;
    return __atomic_compare_exchange_n(&leaving, &expected, 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

static void park(void)
{
    for (;;) svcSleepThread(INT64_MAX);
}

static void last_exit_step(void)
{
    if (!claim_leaving()) park();  /* the main thread's force_leave has it */
    if (game_started && !back_to_menu) {
        if (threadGetCurHandle() != main_thread)  /* it stops using hid before libnx closes it */
            for (int i = 0; i < 100 && !main_parked; i++) svcSleepThread(1000000);
        __nx_applet_exit_mode = 1;
    }
    flush_all();
}

/* Main thread, when the game hasn't reached its next field in time (a long load, a stuck wait). */
static void force_leave(void)
{
    if (!claim_leaving()) park();
    fprintf(stderr, "nx: the game didn't stop in time; ending the process\n");
    __nx_applet_exit_mode = 1;
    flush_all();
    _Exit(0);
}

/* EXIT GAME (present.c), any thread; the system's exit request comes the same way. */
void nx_request_close(void) { close_wanted = 1; }

static void close_pass(void)
{
    enum { CLOSE_SECONDS = 2 };
    static u64 deadline;
    u64 now = armGetSystemTick();
    if (close_wanted && !deadline) {
        deadline = now + CLOSE_SECONDS * armGetSystemTickFreq();
        fprintf(stderr, "nx: closing\n");
        gamepad_nx_hold(1);
        gs_window_event(SDL_EVENT_QUIT);  /* the game's exit() at its next field */
    }
    if (deadline && now > deadline) force_leave();
}

/* ---- the home menu, sleep and applets ----
   Out of focus the game waits at its next field (present.c's watch and app_background_wait), so no
   more pictures reach the GPU. In the home menu and in sleep the system then suspends the process;
   SuspendHomeSleepNotify (main) tells this loop first. The sound stops here, for applets as well. */
static void focus_pass(void)
{
    static int away, sound_stopped;
    int focused = appletGetFocusState() == AppletFocusState_InFocus;
    if (focused == !away) return;
    away = !focused;
    fprintf(stderr, "nx: %s\n", away ? "out of focus" : "in focus");
    nx_send_event(away ? SDL_EVENT_WILL_ENTER_BACKGROUND : SDL_EVENT_DID_ENTER_FOREGROUND);
    gamepad_nx_hold(away);
    if (away) {
        sound_stopped = serviceIsActive(audoutGetServiceSession_AudioOut()) && R_SUCCEEDED(audoutStopAudioOut());
        flush_all();  /* the process may never run again */
    } else if (sound_stopped) {
        audoutStartAudioOut();
        sound_stopped = 0;
    }
}

/* ---- DHD_NX_HEARTBEAT and DHD_NX_EXIT (test tools; dhd.ini's DHD_* lines arrive after the start,
   so both are looked for each second) ---- */
extern volatile const char *nx_render_stage;
extern uint64_t kernel_vsync_count;
int nx_gpu_error(void);
/* The renderer's cumulative timers (dhd_render_nx.c). */
#define NX_TIMERS(X) X(record) X(submit) X(gpuwait) X(acquire) X(present)
#define NX_TIMER_DECLARE(name) extern volatile uint64_t nx_t_##name##_ns __attribute__((weak));
NX_TIMERS(NX_TIMER_DECLARE)

/* The clocks really running, for the heartbeat: a sys-clk profile only applies under its conditions.
   0 when the service is closed to this process. */
static unsigned clock_mhz(PcvModuleId module)
{
    static int opened;
    if (!opened) opened = R_SUCCEEDED(clkrstInitialize()) ? 1 : -1;
    if (opened < 0) return 0;
    ClkrstSession session;
    u32 hz = 0;
    if (R_FAILED(clkrstOpenSession(&session, module, 3))) return 0;
    if (R_FAILED(clkrstGetClockRate(&session, &hz))) hz = 0;
    clkrstCloseSession(&session);
    return hz / 1000000;
}

/* A line a second on the card, even if the game hangs: fields/s, each thread's share of a core, and
   the renderer's ms per field, all since the line before. */
static void heartbeat_line(double now)
{
    enum { THREADS = 16 };
    static double last, last_cpu[THREADS];
    static uint64_t last_field;
#define NX_TIMER_LAST(name) static uint64_t last_##name;
    NX_TIMERS(NX_TIMER_LAST)
    uint64_t field = *(volatile uint64_t *)&kernel_vsync_count;
    uint64_t fields = field - last_field;
    double elapsed = now - last, per_field = fields ? 1e6 * (double)fields : 1e30;  /* no field: 0.00 */
    const char *names[THREADS];
    double cpu[THREADS];
    unsigned threads = host_thread_cpu(names, cpu, THREADS);
    char line[512];
    int n = snprintf(line, sizeof line, "nx: fields/s %.1f gpu error %d renderer: %s; cpu%%", last ? fields / elapsed : 0.0, nx_gpu_error(), nx_render_stage);
    for (unsigned i = 0; i < threads && n > 0 && (size_t)n < sizeof line; i++) {
        n += snprintf(line + n, sizeof line - (size_t)n, " %s %.0f", names[i], last ? (cpu[i] - last_cpu[i]) / elapsed * 100.0 : 0.0);
        last_cpu[i] = cpu[i];
    }
    if (n > 0 && (size_t)n < sizeof line) n += snprintf(line + n, sizeof line - (size_t)n, "; ms/field");
#define NX_TIMER_PRINT(name) \
    { \
        uint64_t total = &nx_t_##name##_ns ? nx_t_##name##_ns : 0; \
        if (n > 0 && (size_t)n < sizeof line) \
            n += snprintf(line + n, sizeof line - (size_t)n, " " #name " %.2f", (double)(total - last_##name) / per_field); \
        last_##name = total; \
    }
    NX_TIMERS(NX_TIMER_PRINT)
    if (n > 0 && (size_t)n < sizeof line)
        n += snprintf(line + n, sizeof line - (size_t)n, "; MHz cpu %u gpu %u mem %u", clock_mhz(PcvModuleId_CpuBus),
                      clock_mhz(PcvModuleId_GPU), clock_mhz(PcvModuleId_EMC));
    fprintf(stderr, "%s\n", line);
    fflush(stderr);
    fsync(fileno(stderr));
    last = now, last_field = field;
}

/* A --pgo-generate build: the counters to the card before anything can go wrong on the way out. */
extern void __gcov_dump(void) __attribute__((weak));
static void nx_profile_dump(void)
{
    if (!__gcov_dump) return;
    fprintf(stderr, "nx: writing the profile\n");
    fflush(stderr);
    __gcov_dump();
    fprintf(stderr, "nx: profile written\n");
    fflush(stderr);
    fsync(fileno(stderr));
}

static void second_pass(void)
{
    static u64 next;
    static double end = -1;
    u64 tick = armGetSystemTick();
    if (tick < next) return;
    next = tick + armGetSystemTickFreq();
    double now = tick / (double)armGetSystemTickFreq();
    void nx_sampler_poll(void);
    nx_sampler_poll();
    const char *beat = getenv("DHD_NX_HEARTBEAT");
    if (beat && *beat && strcmp(beat, "0")) heartbeat_line(now);
    if (end < 0) {
        const char *s = getenv("DHD_NX_EXIT");
        if (s && atof(s) > 0) end = now + atof(s);
    }
    /* Seconds later: exit on this thread, keeping the old way back to the Homebrew Menu that the
       test runs start from. A player's game never takes it: see "the end" above. */
    if (end >= 0 && now > end) {
        back_to_menu = 1;
        gamepad_nx_hold(1);
        nx_profile_dump();
        exit(0);
    }
}

int sdl_host_main(int (*game)(void *), void *arg)
{
    enum { GAME_STACK = 64 << 20 };
    host_thread_name("sdl");
    main_thread = threadGetCurHandle();
    /* above the game's 0x2C: at equal priority a busy game thread would keep this one off core 0 */
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
    gamepad_thread_start();  /* hid is ready (libnx's start); from here only this thread touches it */
    GameStart start = {game, arg};
    game_started = 1;
    SDL_SetAtomicInt(&game_running, 1);
    SDL_Thread *thread = nx_thread_create(game_thread, "game", &start, GAME_STACK);
    if (!thread) return 1;
    while (SDL_GetAtomicInt(&game_running)) {
        if (leaving) {  /* exit() on another thread, which waits for this before hid and fs close */
            main_parked = 1;
            park();
        }
        run_request();
        if (!appletMainLoop()) nx_request_close();  /* the system asks the program to close */
        close_pass();
        focus_pass();
        second_pass();
        if (!close_wanted && appletGetFocusState() == AppletFocusState_InFocus) gamepad_thread_update();
        gs_window_tick();
        svcSleepThread(4000000);
    }
    int code = 0;
    SDL_WaitThread(thread, &code);
    return code;
}

int sdl_host_start(void) { return 1; }

int sdl_host_run(void (*fn)(void *), void *arg)
{
    if (threadGetCurHandle() == main_thread) { fn(arg); return 1; }
    SDL_Semaphore *done = SDL_CreateSemaphore(0);
    for (;;) {
        mutexLock(&slot_lock);
        if (!request_fn) break;
        mutexUnlock(&slot_lock);
        svcSleepThread(1000000);
    }
    request_fn = fn, request_arg = arg, request_done = done;
    mutexUnlock(&slot_lock);
    SDL_WaitSemaphore(done);
    SDL_DestroySemaphore(done);
    return 1;
}

/* ---- the start ---- */
int dhd_launcher_main(int argc, char **argv);

static int console_stdout = -1;  /* stdout before the log took it: the console draws what reaches it */

static int stop_with(const char *message)
{
    fprintf(stderr, "nx: %s\n", message);
    consoleInit(NULL);
    if (console_stdout >= 0) dup2(console_stdout, STDOUT_FILENO);
    printf("Downhill Revived\n\n%s\n\nPress + to leave.\n", message);
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    for (;;) {
        if (!appletMainLoop()) {  /* the system's exit request is answered by the system's exit */
            __nx_applet_exit_mode = 1;
            break;
        }
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    main_thread = threadGetCurHandle();
    atexit(last_exit_step);  /* the first registered runs last */
    socketInitializeDefault();
    mkdir(NX_FOLDER, 0777);
    chdir(NX_FOLDER);
    if (__nxlink_host.s_addr) nxlinkStdio();
    else if (freopen(NX_FOLDER "dhd.log", "w", stderr)) {  /* stdout shares the open file: two would keep two offsets */
        console_stdout = dup(STDOUT_FILENO);
        dup2(fileno(stderr), STDOUT_FILENO);
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    fprintf(stderr, "nx: Downhill Revived, applet type %d\n", (int)appletGetAppletType());
    if (appletGetAppletType() != AppletType_Application)
        return stop_with("Start the Homebrew Menu by holding R while starting a game:\nthe album's applet mode has too little memory.");
    appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
    /* a new dhd.ini, or a player's without these: the disc, and what this console can draw at full speed */
    static const struct { const char *key, *value; } first_run[] = {
        {"iso", NX_DISC}, {"scale", "1"}, {"window", "fullscreen"}, {"skip_movies", "1"}, {"auto_update", "off"},
        /* Performance Mode as finding 263 gives weak phones: shadows, lens flare and particles off. Only a new
           dhd.ini gets it (finding 267; its gain is not yet measured on a console). */
        {"shadows", "0"}, {"lens_flare", "0"}, {"particles", "0"},
    };
    char disc[512];
    if (!dhd_settings_read(NX_INI, "iso", disc, sizeof disc)) {
        for (size_t i = 0; i < sizeof first_run / sizeof *first_run; i++)
            if (!dhd_settings_read(NX_INI, first_run[i].key, disc, sizeof disc)) dhd_settings_save(NX_INI, first_run[i].key, first_run[i].value);
        snprintf(disc, sizeof disc, "%s", NX_DISC);
    }
    struct stat st;
    if (stat(disc, &st)) {
        char message[640];
        snprintf(message, sizeof message, "No disc image. Copy your own Downhill Domination (USA) disc, SCUS-97177, to\n%s", disc);
        return stop_with(message);
    }
    /* In the hand, normal resolution whatever dhd.ini says: its race ran at 54 fields/s even at 460 MHz,
       against 48 at scale 2 and 768 MHz. The setting itself stays as written, for the dock. */
    char scale[16];
    if (appletGetOperationMode() == AppletOperationMode_Handheld && dhd_settings_read(NX_INI, "scale", scale, sizeof scale) &&
        atoi(scale) > 1) {
        setenv("DHD_SCALE", "1", 1);
        fprintf(stderr, "nx: in the hand, drawing at scale 1 (scale = %s is for the dock)\n", scale);
    }
    /* the handheld GPU's official fast mode (a homebrew starts at 307.2 MHz): 460.8 MHz, no overclock */
    if (R_SUCCEEDED(apmInitialize())) {
        apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, 0x92220008);
        apmExit();
    }
    char *args[] = {(char *)NX_FOLDER "dhd64", NULL};
    return dhd_launcher_main(1, args);
}
