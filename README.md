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

### Step 1 result: it works

Built from `deadwood2/AROS` at `5376f09b` and booted as a live CD under QEMU
(`patches/0001-*.patch`, two files, 18 lines). The patch compiles to exactly
one instruction in `cpu_Dispatch`:

```
23f:  65 48 89 04 25 10 00    mov    %rax,%gs:0x10
```

and the probe, run from the Startup-Sequence so no shell is needed, reports:

```
SysBase        = 0x1002990
%gs:0          = 0x1002990   MATCH - userspace %gs works
%gs:16         = 0x15def20   (ThisTask, once patched)
main: FindTask=0x15def20  %gs=0x15def20
  thread 0: FindTask=0x4be9e670  %gs=0x4be9e670  MATCH  drift=0/2000000
  thread 2: FindTask=0x4bf20190  %gs=0x4bf20190  MATCH  drift=0/2000000
  thread 3: FindTask=0x4bf61090  %gs=0x4bf61090  MATCH  drift=0/2000000
  thread 1: FindTask=0x4bedf3b0  %gs=0x4bedf3b0  MATCH  drift=0/2000000
```

Five threads, five matches, and zero drift over eight million reads in total.
`FindTask(NULL)` is now a single `movq %gs:0x10, %rax` from userspace.

Verified on AROS x86_64 under QEMU only. Not verified on real hardware, and
**not on an SMP build** — this kernel is UP, so the claim that a single load
closes the migration window is still an argument, not a measurement.

Step 1 is deliberately not enough for Go: it gives a thread *identity*, not
thread *storage*. It is what makes the dispatcher hook, the fixed offset and
the userspace read verifiable in one run.

## Step 2: storage, and it works too

A thread pointer needs storage, not just identity. `tc_UserData` cannot be it:
it is documented "for use by the task; no restrictions!" and drivers across the
tree — USB classes, trackdisk, dbus — genuinely use it, so a runtime claiming it
would collide with the very libraries its programs call.

No new API turned out to be needed. The dispatcher already fetches the ETask on
every switch, so it can publish the **address** of a private word rather than a
value. `IntETask` is kernel-private and already allocated, so the word costs no
extra allocation. Tasks without `TF_ETASK` get NULL, and the slot is cleared on
entry so such a task never inherits the previous one's pointer.

The dispatcher compiles to three stores:

```
23f:  65 48 89 04 25 10 00    mov    %rax,%gs:0x10   ; ThisTask
25d:  65 48 89 04 25 18 00    mov    %rax,%gs:0x18   ; ThisTaskTLS = NULL
2d1:  65 48 89 04 25 18 00    mov    %rax,%gs:0x18   ; ThisTaskTLS = &iet_TLSSlot
```

and the probe, with each thread writing a private value through the pointer and
re-checking it two million times:

```
%gs:16 = 0x15f8fb0  (ThisTask)
%gs:24 = 0x15d58b8  (ThisTaskTLS)
  thread 0: task=0x4be9e980 OK  tls=0x4bedf348 val=0xc0de0000 OK  drift=0 tlsbad=0
  thread 1: task=0x4bedf6c0 OK  tls=0x4bf20128 val=0xc0de0001 OK  drift=0 tlsbad=0
  thread 2: task=0x4bf204a0 OK  tls=0x4bf60f98 val=0xc0de0002 OK  drift=0 tlsbad=0
  thread 3: task=0x4bf61310 OK  tls=0x4bfa1e48 val=0xc0de0003 OK  drift=0 tlsbad=0
```

Four distinct slot addresses, each thread reading back exactly what it wrote,
zero drift and zero corruption over eight million reads in total.

So `MOVQ TLS, r` on AROS becomes `movq %gs:0x18, r`, with `g` at offset 0 — the
same shape as Plan 9, where the base comes from a global symbol instead of a
segment.

Caveats worth keeping: verified under QEMU on a **UP** kernel only. The claim
that a single load closes the SMP migration window is an argument, not a
measurement, and nothing here has been run against AROS's own test suite.


## Who else benefits

Not only Go. Real `thread_local` for C and C++ instead of a
`pthread_getspecific` per access; `has-thread-local: true` for the Rust target;
and Mesa could drop the list-walking `GetFromTLS` in `workbench/libs/mesa/tls.c`.
That is the case for sending this upstream rather than carrying it.

## Building and testing

The patches target `deadwood2/AROS` (the ABIv11 fork AROS One is built from),
whose `tls.h` is byte-identical to `aros-development-team/AROS`, so they apply
to both. Build with `build-aros.sh` in the sandbox repo, which carries the
macOS workarounds; `build-aros.sh bootiso` then produces a live CD.

To read the probe's output without a Shell — the live CD's `Tools` menu is
empty, and there is still no way to get a file out of the VM — put `C:tlsprobe`
into `workbench/s/Startup-Sequence` ahead of the Wanderer block and make that
block's condition unsatisfiable. The probe then runs at boot and its output
stays on the console for a screendump. Note that `bootiso` regenerates
`Startup-Sequence` from source, so editing the built copy has no effect.
