/* Can Go's get_tls be built on AROS x86_64?
 *
 * Go's `MOVQ TLS, r` must yield a per-thread pointer in a couple of
 * instructions, with no function call: it runs in signal trampolines and at
 * thread entry, where there is no g and no usable stack yet. FindTask(NULL) is
 * a library call, so it does not qualify.
 *
 * AROS puts a per-CPU tls_t behind %gs (arch/x86_64-pc/kernel/tls.h), and the
 * GDT descriptor for it has dpl=3 — so userspace may read it. Its layout is
 *
 *     0  struct ExecBase            *SysBase
 *     8  void                       *KernelBase
 *    16  struct X86SchedulerPrivate *ScheduleData   (only if __AROSEXEC_SMP__)
 *        apicid_t                    CPUNumber
 *
 * and X86SchedulerPrivate.RunningTask is at offset 0. So on an SMP build
 *
 *     movq %gs:16, %rax ; movq (%rax), %rax
 *
 * is the current Task. This probe asks four things the design rests on:
 *   1. does %gs read at all from userspace,
 *   2. is this an SMP build (slot 16 a pointer) or UP (a small integer),
 *   3. does the derived pointer equal FindTask(NULL),
 *   4. is each pthread a distinct Task, and is the value stable under load —
 *      the sequence is two instructions, so a preemption between them on a
 *      migrating scheduler would hand us another CPU's task.
 */
#include <proto/exec.h>
#include <exec/execbase.h>
#include <pthread.h>
#include <stdio.h>
#include <aros/debug.h>

/* Results go to the kernel debug channel as well as stdout. There is still no
   way to get a file out of the VM (the vvfat share corrupts guest writes and
   SER: never reaches QEMU's serial log), but kprintf lands on the raw serial
   port, so `-serial file:` captures it -- and that works with no shell, which
   means the probe can run straight from the Startup-Sequence. */
#define OUT(...) do { printf(__VA_ARGS__); kprintf(__VA_ARGS__); } while (0)

#define GS(off) ({ IPTR __v; \
    __asm__ volatile("movq %%gs:%P1,%0" : "=r"(__v) : "n"(off)); __v; })

/* After the aros-tls patch, tls_t slot 16 is `struct Task *ThisTask`. Before
   it, that slot is ScheduleData (SMP) or CPUNumber (UP), so an unpatched
   kernel simply reports a mismatch rather than misbehaving. */
#define TLS_THISTASK 16
#define TLS_TLSBASE  24         /* &IntETask->iet_TLSSlot: the thread pointer */

static struct Task *gs_task(void)
{
    return (struct Task *)GS(TLS_THISTASK);
}

/* This is the sequence Go's get_tls would compile to: one load for the base,
   then g at offset 0 of it. No call, no stack needed. */
static APTR *gs_tls(void)
{
    return (APTR *)GS(TLS_TLSBASE);
}

static void *worker(void *arg)
{
    struct Task *me = FindTask(NULL), *via_gs = gs_task();
    APTR *slot = gs_tls();
    long i, drift = 0, tls_bad = 0;
    APTR mine = (APTR)(0xC0DE0000 + (long)arg);

    /* Write a value only this thread should ever see, exactly as a runtime
       would store its `g`, then keep checking it survives. */
    if (slot)
        *slot = mine;

    for (i = 0; i < 2000000; i++)
    {
        if (gs_task() != me)
            drift++;
        if (gs_tls() != slot || (slot && *slot != mine))
            tls_bad++;
    }

    OUT("  thread %ld: task=%p %s  tls=%p val=%p %s  drift=%ld tlsbad=%ld\n",
        (long)arg, (void *)me, me == via_gs ? "OK" : "BAD",
        (void *)slot, slot ? *slot : NULL,
        (slot && *slot == mine) ? "OK" : "BAD", drift, tls_bad);
    return NULL;
}

int main(void)
{
    IPTR sb = GS(0), kb = GS(8), slot16 = GS(16), slot24 = GS(24);
    pthread_t t[4];
    long i;

    OUT("SysBase        = %p\n", (void *)SysBase);
    OUT("%%gs:0          = %p  %s\n", (void *)sb,
           sb == (IPTR)SysBase ? "MATCH - userspace %gs works" : "MISMATCH");
    OUT("%%gs:8          = %p  (KernelBase)\n", (void *)kb);

    OUT("%%gs:16         = %p  (ThisTask, once patched)\n", (void *)slot16);

    OUT("%%gs:24         = %p  (ThisTaskTLS, once patched)\n", (void *)slot24);
    OUT("main: FindTask=%p  %%gs=%p   tls slot @ %p\n",
        (void *)FindTask(NULL), (void *)gs_task(), (void *)gs_tls());

    for (i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, worker, (void *)i);
    for (i = 0; i < 4; i++)
        pthread_join(t[i], NULL);

    OUT("done\n");
    return 0;
}
