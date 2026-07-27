# X OS — Build & Debug Notes

## Build
```
make          # builds x-os.iso and disk.img
make run      # boots in QEMU (8 CPUs, 512M RAM, virtio-gpu-gl)
```

## Terminal typing freeze — IPC circular deadlock
When typing, the key path is: composer → terminal (IPC) → zsh stdin (IPC)
→ zsh processes → zsh output → terminal bridge port (IPC).  If zsh's
stdin port is full (zsh busy writing output) AND the terminal's bridge
port is full (terminal busy in `on_key`/`send_shell_byte`), both processes
are stuck: terminal can't send to zsh, zsh can't send to terminal.

The fix: `send_shell_byte` calls `drain_bridge()` between retries, which
drains zsh's output and unblocks `bridge_write`.  Retries increased to 64.
`bridge_write` retries increased to 64, continues on failure instead of
aborting all remaining output.

## Scheduler
The scheduler uses **deferred preemption** (XNU AST / Linux TIF_NEED_RESCHED pattern):
- Timer/IPI handlers set `need_resched` flag (per-CPU, GS:128) — they do NOT
  call `sched_yield_try()` directly. This eliminates sched_lock contention
  from 8 CPUs × 1000Hz timer ticks and prevents context switches while
  holding IPC/scheduler locks.
- `need_resched` is checked at safe points:
  1. **Syscall return path** (`syscall_entry.S`): before `sysretq`
  2. **Interrupt return to userspace** (`irq_stubs.S`, `lapic_stubs.S`):
     checks CS=0x1B in IRETQ frame, then checks `need_resched`
  3. **Idle loop** (`sched.c`): checks `need_resched` after `hlt`
- `sched_yield_try()` still exists for voluntary yields but is no longer
  called from timer/IPI handlers.
- The composer yields voluntarily via `SYS_NSLEEP(1)` at end of each frame
- IPC locks use `spinlock_acquire_irqsave` (interrupts disabled), so
  preemption can't happen during a port_send/port_recv critical section

## Build gotcha: disk.img
`make` builds **both** `x-os.iso` and `disk.img`.  All userspace binaries
(composer, dock, menubar, terminal, zsh, cmds) live on `disk.img`, not in the
ISO — only the kernel and `init` are embedded in the ISO.  QEMU boots with
`disk.img` writable, so the guest bumps its mtime and make can consider the
image newer than freshly rebuilt binaries.  If a userspace change appears to
have no effect, `rm -f disk.img && make disk.img`.

## Scheduler: the idle loop is load-bearing
Timer and IPI handlers only raise `need_resched` (deferred preemption).
Nothing dispatches the ready queue on their behalf.  Every CPU must park in
`sched_idle_loop()` (kernel/sched/sched.c), which consumes the flag and calls
`sched_yield()`.  A bare `for(;;) hlt;` anywhere a CPU can come to rest will
wedge the entire system as soon as all processes happen to sleep at once —
the symptom is a frozen cursor and dead keyboard with a full ready queue.
`sched_dbg_dump()` prints every process's state, sleep deadline and ready
queue membership; call it from a timer handler to diagnose this class of bug.

## Debugging without a display
`make boottest` boots headless and dumps the serial log.  To drive input,
run QEMU with `-monitor unix:/tmp/xos.mon,server,nowait` and pipe HMP
commands into it: `mouse_move dx dy`, `sendkey <key>`.  This is how the
frozen-cursor bug was reproduced and verified.

## /sys
Live kernel state as files (kernel/fs/sysfs.c), generated on demand.
`cat /sys/kernel/info`, `cat /sys/proc` (works as ps), `cat /sys/cpu/info`.
Writable: `/sys/input/mouse` ("x y" warps the pointer),
`/sys/proc/<pid>/kill`, `/sys/proc/<pid>/prio`.  sysfs owns fds 512+ and is
routed by fd range in kernel/arch/x86_64/syscall.c.  Adding a node means
adding one generator function and one entry in `render_file()`/`dir_for()`.
