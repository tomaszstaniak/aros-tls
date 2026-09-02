# aros-tls

**Real thread-local storage for AROS x86_64, in 38 lines of kernel.**

Two patches against the AROS kernel give userspace a per-thread pointer it can
read in a *single instruction*, with no library call. They also make
`FindTask(NULL)` one instruction. Both are measured on a booted system, not
just argued for.

```
movq %gs:0x10, %rax     # the running task            (was: FindTask(NULL), a library call)
movq %gs:0x18, %rax     # this thread's private word  (was: nothing like it existed)
```

## The problem

AROS x86_64 has no cheap per-thread pointer.

* `FindTask(NULL)` is a library call through a jump table.
* GCC for AROS is configured `--disable-tls`, so `__thread` lowers to
  **emulated** TLS: a call to `__emutls_get_address` plus a
  `pthread_getspecific` on *every access*.
* `struct ExecBase` has no `ThisTask` field — it went away with the SMP rework.
* Mesa carries its own `GetFromTLS` that walks a list keyed on the task.

That is enough for C, and enough for Rust's `thread_local!`, which is why both
work on AROS today — they just pay a function call per access. It is **not**
enough for a language runtime like Go's, whose thread-pointer read happens in
signal trampolines and at thread entry, where there is no stack to make a call
on.

## The insight

The mechanism was already there, unused.

AROS keeps a per-CPU block behind `%gs` (`arch/x86_64-pc/kernel/tls.h`), and
`kernel_startup.c` builds its GDT descriptor with **`dpl = 3`** — so userspace
is allowed to read it. There is no `swapgs` anywhere in the kernel and no
`MSR_FS_BASE`/`MSR_GS_BASE` write; the segment base is a plain GDT descriptor
set once at boot. Verified from userspace before writing any patch:
`movq %gs:0, %rax` returns `SysBase`.

But the block held only globals. The dispatcher knows which task it is about to
run and never wrote it anywhere userspace could see. So the fix is not a new
mechanism — it is a store.

## What the patches do

**`0001` — publish the running task.** Adds `struct Task *ThisTask` to `tls_t`
and one `TLS_SET` in `cpu_Dispatch`. The field sits *before* the SMP-only
member so its offset is identical in UP and SMP builds: consumers outside the
kernel need one constant, not one per build configuration.

**`0002` — give each task a private word.** Adds `APTR iet_TLSSlot` to
`struct IntETask` and publishes its **address**. `IntETask` is kernel-private
and already allocated, so this costs no extra allocation, and no new
kernel.resource API is needed. Tasks without `TF_ETASK` get NULL, and the slot
is cleared on entry so such a task never inherits the previous one's pointer.

`tc_UserData` was considered and rejected as the storage: it is documented "for
use by the task; no restrictions!" and drivers across the tree — USB classes,
trackdisk, dbus — genuinely use it, so a language runtime claiming it would
collide with the very libraries its own programs call.

Three stores in the dispatcher, and nothing else on the hot path:

```
23f:  65 48 89 04 25 10 00    mov    %rax,%gs:0x10   ; ThisTask
25d:  65 48 89 04 25 18 00    mov    %rax,%gs:0x18   ; ThisTaskTLS = NULL
2d1:  65 48 89 04 25 18 00    mov    %rax,%gs:0x18   ; ThisTaskTLS = &iet_TLSSlot
```

There is no SMP migration hazard. A two-instruction sequence that read a
per-CPU pointer and then dereferenced it could be preempted in between and end
up reading another core's task. Here a *single* load already yields our own
task, because at the instant it executes we are the task running on that core.

## Results

`probe/tlsprobe.c`, built from a patched tree and run on a live CD under QEMU.
Each thread writes a private value through the pointer and re-checks it two
million times:

```
%gs:16 = 0x15f8fb0  (ThisTask)
%gs:24 = 0x15d58b8  (ThisTaskTLS)
main: FindTask=0x15f8fb0  %gs=0x15f8fb0
  thread 0: task=0x4be9e980 OK  tls=0x4bedf348 val=0xc0de0000 OK  drift=0 tlsbad=0
  thread 1: task=0x4bedf6c0 OK  tls=0x4bf20128 val=0xc0de0001 OK  drift=0 tlsbad=0
  thread 2: task=0x4bf204a0 OK  tls=0x4bf60f98 val=0xc0de0002 OK  drift=0 tlsbad=0
  thread 3: task=0x4bf61310 OK  tls=0x4bfa1e48 val=0xc0de0003 OK  drift=0 tlsbad=0
```

Four distinct slot addresses, every thread reading back exactly what it wrote,
zero drift and zero corruption over eight million reads in total.

### What was *not* verified

* QEMU only — **not** real hardware.
* A **UP** kernel only. `__AROSEXEC_SMP__` was off (`%gs:16` read back as
  `CPUNumber`, not a `ScheduleData` pointer, even under `qemu -smp 2`). The
  single-load argument above is sound but remains an argument on SMP, not a
  measurement.
* Not run against AROS's own nightly test suite.

## Who this is for

Not only Go, which is what prompted it:

* **C and C++** get real `thread_local` instead of a `pthread_getspecific` per
  access.
* **Rust** could flip `has-thread-local` to true for the AROS target. Note this
  is an **optimisation, not an unblock**: Rust's `thread_local!` already works
  on AROS through its pthread-key fallback — tokio's runtime runs there today —
  so the gain is dropping a call per access, not enabling something impossible.
* **Mesa** could drop the list-walking `GetFromTLS` in
  `workbench/libs/mesa/tls.c`.
* Anything that calls `FindTask(NULL)` in a hot path.

A worked consumer: [go-aros](https://github.com/tomaszstaniak/go-aros) lowers
Go's `MOVQ TLS, r` to `movq %gs:24, r`. The resulting `g` access assembles
byte-identically to Plan 9's; only the base load differs, and this one is
cheaper — one segment-relative load with no relocation, where Plan 9 needs a
PC-relative load of a global.

## Applying and testing

The patches are against `deadwood2/AROS` (the ABIv11 fork), whose `tls.h` is
byte-identical to `aros-development-team/AROS`, so they apply to both.

```sh
git am patches/0001-*.patch patches/0002-*.patch
# configure with --target=pc-x86_64, then build, then:
make bootiso
```

To read the probe's output there is a wrinkle worth knowing: a freshly built
live CD has an **empty `Tools` menu**, and there is no reliable way to get a
file out of a QEMU guest. So put `C:tlsprobe` into
`workbench/s/Startup-Sequence` ahead of the Wanderer block and make that
block's condition unsatisfiable — the probe then runs at boot and its output
stays on the console for a screendump. Note that `bootiso` regenerates
`Startup-Sequence` from source, so editing the built copy has no effect.

Build the probe with the AROS SDK, linking libpthread in a group — the emulated
TLS helper lives in `libgcc.a`, which the driver places after it:

```sh
x86_64-aros-gcc -o tlsprobe probe/tlsprobe.c \
    -Wl,--start-group "$SDK/lib/libpthread.a" -lgcc -Wl,--end-group
```

## Design gap: hosted targets (from AROS developer feedback)

Posted on aros-exec; deadwood's review raised a point this README did not:
**the same x86_64 user binary must run on the native kernel and on the hosted
ones** (AROS as a Linux or Windows process). Those have their own kernels
without this patch, and on Windows-hosted `%gs` belongs to the host's TEB. A
binary that hard-codes `movq %gs:0x18` — which is what this README and the
go-aros lowering currently assume — would crash there. He is right.

The kernel-side store is fine as a per-target thing; it lives in
`arch/x86_64-pc/kernel` and always would. What has to change is that **user
code must not know the offset**. The revised shape:

* each kernel publishes the task's TLS word wherever it can — native via the
  existing `%gs` GDT block; linux-hosted plausibly via `arch_prctl(ARCH_SET_GS)`
  in its dispatcher (unverified); windows-hosted via a TEB TLS slot;
* a small portable call in `kernel.resource` returns the segment-relative
  offset once at startup; the program then reads `%gs:offset`. Still one load,
  no call on the hot path, no constant in the binary. This is precisely Go's
  Windows/Android model (`runtime.tls_g`), so the consumer side is a known
  shape;
* cross-arch, the discovery call is the portable part and the register is
  implied by the architecture (`TPIDR_EL0` on aarch64).

Also from the review: `FindTask(NULL)` itself could inline to the direct read
on x86_64-native and stay an LVO call elsewhere, giving ordinary programs the
speedup with no source change. That is a separate patch.

The patches in this repo are the native-only first cut and stand as measured;
the ABI they imply for user code is **not** the final one.

## Status

Posted to aros-exec; to be filed on the AROS GitHub issue tracker so more
developers see it. Native only, hosted untested. Offered here in the hope it is useful; feedback from AROS
developers on the placement of `ThisTaskTLS` and on SMP behaviour would be
especially welcome, since SMP is the part this cannot test.

## Licence

The patches modify AROS source and are offered under the same terms, the
[AROS Public License](https://aros.sourceforge.io/documentation/developers/licenses.php).
`probe/tlsprobe.c` is original and may be used under the APL as well.
