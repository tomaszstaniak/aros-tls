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

#define GS(off) ({ IPTR __v; \
    __asm__ volatile("movq %%gs:%P1,%0" : "=r"(__v) : "n"(off)); __v; })

static int smp;                 /* slot 16 looked like a pointer */

static struct Task *gs_task(void)
{
    return smp ? *(struct Task **)GS(16) : NULL;
}

static void *worker(void *arg)
{
    struct Task *me = FindTask(NULL), *via_gs = gs_task();
    long i, drift = 0;

    /* Stability: if the two-instruction sequence can be preempted onto
       another core, a long spin should eventually catch it disagreeing. */
    for (i = 0; i < 2000000; i++)
        if (gs_task() != me)
            drift++;

    printf("  thread %ld: FindTask=%p  %%gs=%p  %s  drift=%ld/2000000\n",
           (long)arg, (void *)me, (void *)via_gs,
           me == via_gs ? "MATCH" : "MISMATCH", drift);
    return NULL;
}

int main(void)
{
    IPTR sb = GS(0), kb = GS(8), slot16 = GS(16), slot24 = GS(24);
    pthread_t t[4];
    long i;

    printf("SysBase        = %p\n", (void *)SysBase);
    printf("%%gs:0          = %p  %s\n", (void *)sb,
           sb == (IPTR)SysBase ? "MATCH - userspace %gs works" : "MISMATCH");
    printf("%%gs:8          = %p  (KernelBase)\n", (void *)kb);

    /* A pointer here means ScheduleData, i.e. an SMP kernel; a small integer
       means CPUNumber, i.e. UP — and then there is no per-core RunningTask. */
    smp = slot16 > 0x10000;
    printf("%%gs:16         = %p  -> %s build\n", (void *)slot16,
           smp ? "SMP (ScheduleData)" : "UP (CPUNumber)");

    printf("%%gs:24         = %p\n", (void *)slot24);
    printf("main: FindTask=%p  %%gs=%p\n",
           (void *)FindTask(NULL), (void *)gs_task());

    for (i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, worker, (void *)i);
    for (i = 0; i < 4; i++)
        pthread_join(t[i], NULL);

    printf("done\n");
    return 0;
}
