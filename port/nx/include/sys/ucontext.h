/* host.c's crash and dump signals: Horizon has none, so nothing here ever runs (sigaction is a
   no-op here; host_nx.c's exception handler is the crash report). Only the shape it compiles against. */
#ifndef DHD_NX_UCONTEXT_H
#define DHD_NX_UCONTEXT_H
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
typedef struct { unsigned long long regs[31], sp, pc; } nx_mcontext_t;
typedef struct { nx_mcontext_t uc_mcontext; } ucontext_t;
#define si_addr si_value.sival_ptr
#define SA_RESETHAND 0
#define SA_SIGINFO 0
/* newlib's struct has no sa_sigaction without its realtime extensions: the shape host.c writes */
struct nx_sigaction { void (*sa_sigaction)(int, siginfo_t *, void *); void (*sa_handler)(int); int sa_flags; sigset_t sa_mask; };
static inline int nx_sigaction(int s, const struct nx_sigaction *a, struct nx_sigaction *o)
{
    (void)s; (void)a;
    if (o) *o = (struct nx_sigaction){0};
    return 0;
}
#define sigaction nx_sigaction
#ifndef SIGUSR1
#define SIGUSR1 30
#endif
#ifndef SIGBUS
#define SIGBUS 10
#endif
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(e, m) setjmp(e)
#define siglongjmp longjmp
#endif
