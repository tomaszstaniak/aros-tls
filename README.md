# aros-tls — a one-instruction thread pointer for AROS x86_64

AROS x86_64 has no per-thread pointer that userspace can read cheaply.
`FindTask(NULL)` is a library call, and GCC for AROS is built `--disable-tls`,
so `__thread` lowers to `__emutls_get_address` — a call plus a
`pthread_getspecific` on **every access**. That is enough for C and for Rust's
`thread_local!`, which is why both work today. It is not enough for a Go
runtime, whose `get_tls` runs in signal trampolines and at thread entry where
there is no `g` and no callable stack yet.

This repository is the patch that fixes it, and the probe that proves it.

## Why it can be small

AROS already keeps a per-CPU block behind `%gs`
(`arch/x86_64-pc/kernel/tls.h`), and `kernel_startup.c` builds its GDT
descriptor with **`dpl = 3`** — userspace may read it. There is no `swapgs`
anywhere in the kernel and no `MSR_FS_BASE`/`MSR_GS_BASE` write, so the segment
base is a plain GDT descriptor set once at boot.

So the missing piece is not a mechanism. It is one store: the dispatcher
already knows which task it is about to run, and never writes it anywhere
userspace can see.

## Measured before writing any patch

`probe/tlsprobe.c`, run on AROS One (x86_64, ABIv11) under QEMU:

```
SysBase        = 0x1002950
%gs:0          = 0x1002950   MATCH - userspace %gs works
%gs:8          = 0x0         (KernelBase)
%gs:16         = 0x0         -> UP (CPUNumber) build
main:      FindTask=0x5ab99360
thread 0:  FindTask=0x4ad7c490     thread 2:  FindTask=0x4da97960
thread 1:  FindTask=0x4bca1810     thread 3:  FindTask=0x4da98790
```

* `movq %gs:0, %rax` returns `SysBase` from userspace, in one instruction.
* `KernelBase` reads 0 — nothing ever calls `TLS_SET(KernelBase, …)`.
* `%gs:16` is 0, i.e. `CPUNumber`, so this is a **UP build**: `__AROSEXEC_SMP__`
  is off and there is no per-core `RunningTask`, even under `qemu -smp 2`.
* Every pthread is a **distinct Exec Task**, so a per-task slot is the right
  home for thread-private data.

## The patch, step 1 (minimal, no ABI questions)

Put the current task pointer in the per-CPU block and set it at dispatch:

* `arch/x86_64-pc/kernel/tls.h` — add `struct Task *ThisTask` **before** the
  `#if defined(__AROSEXEC_SMP__)` field, so its offset is identical in UP and
  SMP builds. Userspace needs one constant, not one per build config.
* `arch/x86_64-pc/kernel/kernel_cpu.c` — one `TLS_SET(ThisTask, task)` in
  `cpu_Dispatch`, right after `core_Dispatch()` returns the task to run.

`FindTask(NULL)` then becomes `movq %gs:16, %rax` — and note there is no
migration race even on SMP. A two-instruction sequence that read a per-CPU
pointer and then dereferenced it could be preempted in between and end up
reading another core's task. Here the single load already yields *our* task,
because at the instant it executes we are the task running on that core.

Step 1 is deliberately not enough for Go: it gives a thread *identity*, not
thread *storage*. It is what makes the dispatcher hook, the fixed offset and
the userspace read verifiable in one run.

## Step 2 (what Go actually needs)

Go needs one writable word per thread to hold `g`. `tc_UserData` cannot be it —
it is documented "for use by the task; no restrictions!" and drivers and
applications across the tree genuinely use it, so a language runtime claiming
it would collide. The slot belongs in `struct IntETask` (`rom/exec/etask.h`),
which is kernel-private, with the dispatcher caching it into the `%gs` block.
Tasks without `TF_ETASK` get NULL and simply have no TLS.

## Who else benefits

Not only Go. Real `thread_local` for C and C++ instead of a
`pthread_getspecific` per access; `has-thread-local: true` for the Rust target;
and Mesa could drop the list-walking `GetFromTLS` in `workbench/libs/mesa/tls.c`.
That is the case for sending this upstream rather than carrying it.

## Building and testing

The patch targets `deadwood2/AROS` (the ABIv11 fork AROS One is built from),
whose `tls.h` is byte-identical to `aros-development-team/AROS`, so it applies
to both. The build tree lives on the `arosbuild` sparse image next to the
sandbox checkout; it is configured
(`--target=pc-x86_64 --with-aros-toolchain-install=…`) and its crosstools are
built, but AROS itself has never been built from it — that comes first.
