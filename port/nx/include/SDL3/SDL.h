/* The part of SDL3 the port calls, on libnx (port/nx/src/sdl_nx.c). Not SDL: the same names, for the
   host files that are shared with the other platforms. The controllers and the main loop are the
   Switch's own (gamepad_nx.c, nx_host.c), so their calls are not here. */
#ifndef DHD_NX_SDL_H
#define DHD_NX_SDL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define SDLCALL
typedef uint8_t Uint8; typedef uint16_t Uint16; typedef uint32_t Uint32; typedef uint64_t Uint64;
typedef int16_t Sint16; typedef int32_t Sint32; typedef int64_t Sint64;
#define SDL_zero(x) memset(&(x), 0, sizeof(x))
#define SDL_free free
#define SDL_strdup strdup
#define SDL_strcasecmp strcasecmp
#define SDL_strncasecmp strncasecmp
#define SDL_isalnum isalnum
#define SDL_toupper toupper

/* atomics */
typedef struct { int value; } SDL_AtomicInt;
typedef int SDL_SpinLock;
static inline int SDL_GetAtomicInt(SDL_AtomicInt *a) { return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST); }
static inline int SDL_SetAtomicInt(SDL_AtomicInt *a, int v) { return __atomic_exchange_n(&a->value, v, __ATOMIC_SEQ_CST); }
static inline int SDL_AddAtomicInt(SDL_AtomicInt *a, int v) { return __atomic_fetch_add(&a->value, v, __ATOMIC_SEQ_CST); }
static inline bool SDL_CompareAndSwapAtomicInt(SDL_AtomicInt *a, int old, int v)
{ return __atomic_compare_exchange_n(&a->value, &old, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }
#define SDL_MemoryBarrierAcquire() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define SDL_MemoryBarrierRelease() __atomic_thread_fence(__ATOMIC_RELEASE)
#define SDL_CPUPauseInstruction() __asm__ volatile("isb")
void nx_spinlock_sleep(void);
static inline void SDL_LockSpinlock(SDL_SpinLock *l)
{
    for (unsigned spin = 0; __atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE); spin++) {
        if (spin < 1024) __asm__ volatile("isb");
        else nx_spinlock_sleep();
    }
}
static inline void SDL_UnlockSpinlock(SDL_SpinLock *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

/* locks */
typedef struct SDL_Mutex SDL_Mutex;
typedef struct SDL_Condition SDL_Condition;
typedef struct SDL_Semaphore SDL_Semaphore;
SDL_Mutex *SDL_CreateMutex(void);
void SDL_DestroyMutex(SDL_Mutex *m);
void SDL_LockMutex(SDL_Mutex *m);
void SDL_UnlockMutex(SDL_Mutex *m);
SDL_Condition *SDL_CreateCondition(void);
void SDL_DestroyCondition(SDL_Condition *c);
void SDL_SignalCondition(SDL_Condition *c);
void SDL_BroadcastCondition(SDL_Condition *c);
void SDL_WaitCondition(SDL_Condition *c, SDL_Mutex *m);
bool SDL_WaitConditionTimeout(SDL_Condition *c, SDL_Mutex *m, Sint32 ms);
SDL_Semaphore *SDL_CreateSemaphore(Uint32 value);
void SDL_DestroySemaphore(SDL_Semaphore *s);
void SDL_SignalSemaphore(SDL_Semaphore *s);
void SDL_WaitSemaphore(SDL_Semaphore *s);
bool SDL_WaitSemaphoreTimeout(SDL_Semaphore *s, Sint32 ms);
Uint32 SDL_GetSemaphoreValue(SDL_Semaphore *s);

/* threads */
typedef struct SDL_Thread SDL_Thread;
typedef uint64_t SDL_ThreadID;
typedef int (*SDL_ThreadFunction)(void *);
typedef enum { SDL_THREAD_PRIORITY_LOW, SDL_THREAD_PRIORITY_NORMAL, SDL_THREAD_PRIORITY_HIGH, SDL_THREAD_PRIORITY_TIME_CRITICAL } SDL_ThreadPriority;
SDL_Thread *SDL_CreateThread(SDL_ThreadFunction fn, const char *name, void *arg);
/* The Switch's: with a stack of its own size (the game's). */
SDL_Thread *nx_thread_create(SDL_ThreadFunction fn, const char *name, void *arg, size_t stack);
void SDL_WaitThread(SDL_Thread *t, int *status);
void SDL_DetachThread(SDL_Thread *t);
bool SDL_SetCurrentThreadPriority(SDL_ThreadPriority p);
SDL_ThreadID SDL_GetCurrentThreadID(void);
typedef int SDL_TLSID;  /* 0 until first set */
typedef void (*SDL_TLSDestructorCallback)(void *value);
void *SDL_GetTLS(SDL_TLSID *id);
bool SDL_SetTLS(SDL_TLSID *id, const void *value, SDL_TLSDestructorCallback destructor);

/* time */
Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);
Uint64 SDL_GetTicks(void);
Uint64 SDL_GetTicksNS(void);
void SDL_Delay(Uint32 ms);
void SDL_DelayNS(Uint64 ns);
void SDL_DelayPrecise(Uint64 ns);
typedef Sint64 SDL_Time;
typedef struct { int year, month, day, hour, minute, second, nanosecond, day_of_week, utc_offset; } SDL_DateTime;
bool SDL_TimeToDateTime(SDL_Time t, SDL_DateTime *dt, bool local);
bool SDL_GetCurrentTime(SDL_Time *t);

/* one-time initialisation guard (SDL3's SDL_InitState): 0 not started, 1 one thread is at it, 2 done */
typedef struct { SDL_AtomicInt status; } SDL_InitState;
static inline bool SDL_ShouldInit(SDL_InitState *s)
{
    int expect = 0;
    if (__atomic_compare_exchange_n(&s->status.value, &expect, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return true;
    while (__atomic_load_n(&s->status.value, __ATOMIC_SEQ_CST) == 1) __asm__ volatile("isb");
    return false;
}
static inline void SDL_SetInitialized(SDL_InitState *s, bool done) { __atomic_store_n(&s->status.value, done ? 2 : 0, __ATOMIC_SEQ_CST); }
#include <math.h>
#define SDL_floorf floorf

/* the system */
typedef Uint32 SDL_InitFlags;
#define SDL_INIT_AUDIO 0x10u
#define SDL_INIT_VIDEO 0x20u
#define SDL_INIT_EVENTS 0x4000u
bool SDL_InitSubSystem(SDL_InitFlags f);
void SDL_Quit(void);
const char *SDL_GetError(void);
int SDL_GetNumLogicalCPUCores(void);
int SDL_GetSystemRAM(void);
bool SDL_SetHint(const char *name, const char *value);
#define SDL_HINT_APP_ID "app"
#define SDL_HINT_WINDOWS_INTRESOURCE_ICON "i"
#define SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL "s"
#define SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN "a"
#define SDL_HINT_ANDROID_BLOCK_ON_PAUSE "b"
#define SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES "f"
#define SDL_MESSAGEBOX_ERROR 0x10u
bool SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, void *window);
char *SDL_GetClipboardText(void);
bool SDL_SetClipboardText(const char *text);

/* files */
typedef enum { SDL_PATHTYPE_NONE, SDL_PATHTYPE_FILE, SDL_PATHTYPE_DIRECTORY, SDL_PATHTYPE_OTHER } SDL_PathType;
typedef struct { SDL_PathType type; Uint64 size; SDL_Time create_time, modify_time, access_time; } SDL_PathInfo;
typedef enum { SDL_ENUM_CONTINUE, SDL_ENUM_SUCCESS, SDL_ENUM_FAILURE } SDL_EnumerationResult;
typedef SDL_EnumerationResult (*SDL_EnumerateDirectoryCallback)(void *user, const char *dir, const char *name);
bool SDL_GetPathInfo(const char *path, SDL_PathInfo *info);
bool SDL_CreateDirectory(const char *path);
bool SDL_EnumerateDirectory(const char *path, SDL_EnumerateDirectoryCallback fn, void *user);

/* properties: only what the process call (launcher_link.c, off here) writes */
typedef Uint32 SDL_PropertiesID;
typedef struct SDL_Process SDL_Process;
#define SDL_PROP_PROCESS_CREATE_ARGS_POINTER "a"
#define SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN "b"
static inline SDL_PropertiesID SDL_CreateProperties(void) { return 1; }
static inline void SDL_DestroyProperties(SDL_PropertiesID p) { (void)p; }
static inline bool SDL_SetPointerProperty(SDL_PropertiesID p, const char *n, void *v) { (void)p; (void)n; (void)v; return true; }
static inline bool SDL_SetBooleanProperty(SDL_PropertiesID p, const char *n, bool v) { (void)p; (void)n; (void)v; return true; }
static inline void *SDL_GetPointerProperty(SDL_PropertiesID p, const char *n, void *d) { (void)p; (void)n; return d; }
static inline Sint64 SDL_GetNumberProperty(SDL_PropertiesID p, const char *n, Sint64 d) { (void)p; (void)n; return d; }
static inline SDL_Process *SDL_CreateProcessWithProperties(SDL_PropertiesID p) { (void)p; return NULL; }
static inline void SDL_DestroyProcess(SDL_Process *p) { (void)p; }

/* sound: one output stream over audout */
typedef Uint32 SDL_AudioDeviceID;
typedef enum { SDL_AUDIO_S16 = 0x8010 } SDL_AudioFormat;
typedef struct { SDL_AudioFormat format; int channels, freq; } SDL_AudioSpec;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef void (*SDL_AudioStreamCallback)(void *user, SDL_AudioStream *s, int additional, int total);
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK 0xFFFFFFFFu
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID id, const SDL_AudioSpec *spec, SDL_AudioStreamCallback fn, void *user);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *s);
bool SDL_PauseAudioStreamDevice(SDL_AudioStream *s);  /* silence; the game's callback is not asked */
bool SDL_ClearAudioStream(SDL_AudioStream *s);        /* drops what is queued */
bool SDL_SetAudioStreamGain(SDL_AudioStream *s, float gain);  /* only 0 (mute) and 1 are told apart */
void SDL_DestroyAudioStream(SDL_AudioStream *s);
bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *data, int bytes);
int SDL_GetAudioStreamQueued(SDL_AudioStream *s);
SDL_AudioDeviceID SDL_GetAudioStreamDevice(SDL_AudioStream *s);
bool SDL_GetAudioDeviceFormat(SDL_AudioDeviceID id, SDL_AudioSpec *spec, int *frames);
const char *SDL_GetCurrentAudioDriver(void);

/* the window: one full screen; the renderer draws to libnx's default window */
typedef struct SDL_Window SDL_Window;
typedef Uint64 SDL_WindowFlags;
typedef Uint32 SDL_DisplayID;
typedef struct { int x, y, w, h; } SDL_Rect;
typedef struct { SDL_DisplayID displayID; int format, w, h; float pixel_density, refresh_rate; } SDL_DisplayMode;
typedef struct SDL_Surface SDL_Surface;
typedef struct SDL_IOStream SDL_IOStream;
#define SDL_WINDOW_FULLSCREEN 1ull
#define SDL_WINDOW_MINIMIZED 0x40ull
#define SDL_WINDOW_BORDERLESS 0x10ull
#define SDL_WINDOW_RESIZABLE 0x20ull
#define SDL_WINDOW_HIDDEN 8ull
#define SDL_WINDOW_OCCLUDED 0x1000ull
#define SDL_WINDOW_INPUT_FOCUS 0x200ull
#define SDL_WINDOW_HIGH_PIXEL_DENSITY 0x2000ull
SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags f);
SDL_DisplayID SDL_GetPrimaryDisplay(void);
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *w);
const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID d);
const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID d);
SDL_Window *SDL_GetWindowFromID(Uint32 id);
float SDL_GetWindowDisplayScale(SDL_Window *w);
SDL_DisplayMode **SDL_GetFullscreenDisplayModes(SDL_DisplayID d, int *count);
bool SDL_GetDisplayBounds(SDL_DisplayID d, SDL_Rect *r);
bool SDL_GetDisplayUsableBounds(SDL_DisplayID d, SDL_Rect *r);
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *w);
Uint32 SDL_GetWindowID(SDL_Window *w);
float SDL_GetWindowPixelDensity(SDL_Window *w);
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *w);
bool SDL_GetWindowSizeInPixels(SDL_Window *w, int *x, int *y);
bool SDL_GetWindowBordersSize(SDL_Window *w, int *a, int *b, int *c, int *d);
const char *SDL_GetCurrentVideoDriver(void);
/* The Switch's: the console's screen, 1920 x 1080 docked or 1280 x 720, as it was at the start. */
void nx_screen_size(int *w, int *h);
#define NX_WINDOW_NOOP(name, ...) static inline bool name(__VA_ARGS__) { return true; }
NX_WINDOW_NOOP(SDL_HideWindow, SDL_Window *w) NX_WINDOW_NOOP(SDL_ShowWindow, SDL_Window *w) NX_WINDOW_NOOP(SDL_RaiseWindow, SDL_Window *w)
NX_WINDOW_NOOP(SDL_SyncWindow, SDL_Window *w) NX_WINDOW_NOOP(SDL_SetWindowBordered, SDL_Window *w, bool b)
NX_WINDOW_NOOP(SDL_SetWindowFullscreen, SDL_Window *w, bool b) NX_WINDOW_NOOP(SDL_SetWindowPosition, SDL_Window *w, int x, int y)
NX_WINDOW_NOOP(SDL_SetWindowSize, SDL_Window *w, int x, int y) NX_WINDOW_NOOP(SDL_SetWindowTitle, SDL_Window *w, const char *t)
NX_WINDOW_NOOP(SDL_ShowCursor, void) NX_WINDOW_NOOP(SDL_HideCursor, void) NX_WINDOW_NOOP(SDL_StartTextInput, SDL_Window *w)
NX_WINDOW_NOOP(SDL_StopTextInput, SDL_Window *w)
static inline bool SDL_TextInputActive(SDL_Window *w) { (void)w; return false; }

/* events: the window's few (nx_host.c sends the lifecycle ones to the watch) */
enum { SDL_EVENT_QUIT = 0x100, SDL_EVENT_WILL_ENTER_BACKGROUND = 0x103, SDL_EVENT_DID_ENTER_FOREGROUND = 0x106,
       SDL_EVENT_WINDOW_RESIZED = 0x206, SDL_EVENT_WINDOW_MOUSE_LEAVE = 0x20D, SDL_EVENT_WINDOW_CLOSE_REQUESTED = 0x210,
       SDL_EVENT_WINDOW_FIRST = 0x202, SDL_EVENT_WINDOW_LAST = 0x21F, SDL_EVENT_TEXT_INPUT = 0x303,
       SDL_EVENT_FINGER_DOWN = 0x700, SDL_EVENT_FINGER_UP = 0x701, SDL_EVENT_FINGER_MOTION = 0x702, SDL_EVENT_FINGER_CANCELED = 0x703,
       SDL_EVENT_MOUSE_MOTION = 0x400, SDL_EVENT_MOUSE_BUTTON_DOWN = 0x401, SDL_EVENT_MOUSE_BUTTON_UP = 0x402, SDL_EVENT_MOUSE_WHEEL = 0x403 };
enum { SDL_BUTTON_LEFT = 1, SDL_BUTTON_MIDDLE = 2, SDL_BUTTON_RIGHT = 3, SDL_MOUSEWHEEL_FLIPPED = 1 };
typedef union SDL_Event {
    Uint32 type;
    struct { Uint32 type; Uint32 windowID; float x, y; } motion;
    struct { Uint32 type; Uint32 windowID; Uint8 button; bool down; float x, y; } button;
    struct { Uint32 type; Uint32 windowID; float y, mouse_y, mouse_x; int direction; float x; } wheel;
    struct { Uint32 type; Uint32 windowID; } window;
    struct { Uint32 type; Uint32 reserved; Uint64 timestamp; Uint64 touchID, fingerID; float x, y, dx, dy, pressure; Uint32 windowID; } tfinger;
    Uint8 padding[64];
} SDL_Event;
typedef bool (*SDL_EventFilter)(void *user, SDL_Event *e);
bool SDL_AddEventWatch(SDL_EventFilter fn, void *user);
bool SDL_PushEvent(SDL_Event *e);
void nx_send_event(Uint32 type);  /* to the watch, on the caller's thread */
#endif
