# FreeRTOS on the NCAPPS boxes

Preemptive multitasking for the boxes' MIPS 24KEc (MIPS32r2 + DSP ASE, no
FPU), so NCAPPS can run things side by side: music in the background while
the launcher or an app runs, a USB monitor for hot-plugging sticks, sensor
polling, a network stack.

| Folder | What |
|---|---|
| `FreeRTOS-Kernel/` | FreeRTOS kernel V11.3.1, unchanged (MIT licence, `LICENSE.md`): the kernel sources, headers, `heap_3` / `heap_4` |
| `port/` | our port for the 24KEc: `portmacro.h`, `port.c`, `port_asm.S`, `port_ctx.h` |
| `qemu/` | test under QEMU (`-M malta -cpu 24KEc`): `sh rtos/qemu/build.sh run` |
| `test/` | `regcheck.S`: register and DSP-state integrity checks used by both tests |
| `../apps/rtostest/` | the same test as an NCAPPS app on the box (launcher entry "RTOS test") |

## How the port works

- **Interrupt modes** (picked when the scheduler starts):
  - *chained*, under U-Boot (Status.BEV = 0, Cause.IV = 1: vectored
    interrupts, 512-byte spacing): the current vector page is copied and
    only four entries are replaced: TLB refill (+0x000), general exceptions
    (+0x180), software interrupt 0 and the CP0 timer's vector. U-Boot's own
    line (IM4, its interrupt controller) keeps running U-Boot's handler,
    which saves registers below the current stack pointer: tasks need ~1.5
    KB of stack headroom.
  - *simple* (QEMU): Cause.IV = 0, everything enters at +0x180, other
    lines masked.

  EBase is changed with Status.BEV set, as the architecture requires;
  EBase, Status and Cause are put back when the scheduler ends.
- **Tick:** the CP0 timer (Count / Compare, interrupt line from
  IntCtl.IPTI, normally IP7), `configCPU_COUNT_HZ` = Count rate (324 MHz on
  the boxes = CPU / 2, 160 MHz on QEMU's malta). After writing Compare the
  port checks that Count has not passed it already (ticks missed while
  interrupts were masked, or Count passing it between the read and the
  write); if it has, the tick restarts from now. Without that check the
  next tick only comes when Count wraps: a 13 s freeze on the boxes. QEMU
  hits this often (its CPU thread can be paused by the host at any
  instruction); `ulPortTickResyncs` counts the restarts.
- **Context switch:** a task yields by raising software interrupt 0;
  the switch happens as soon as interrupts are on (at once, or when the
  critical section ends). The exception entry pushes the registers on the
  task's stack, stores the stack pointer in the TCB, runs the C handler on
  a separate exception stack, and restores whichever task is current.
- **Saved per task:** all general registers except zero / k0 / k1 / sp, HI /
  LO, DSP accumulators ac1-ac3, DSPControl, EPC, Status. SDK code is built
  with `-mdsp`, so the DSP state belongs to the task.
- **k0 is never touched:** U-Boot keeps its global data pointer there and
  the SDK still calls U-Boot (serial, USB). k1 is the only scratch register
  of the exception code.
- **Critical sections:** `di` / `ei`, nesting counted in the TCB.
- **Idle:** `vPortIdleSleep()` (from `vApplicationIdleHook`) runs the MIPS
  `wait` instruction and adds the time asleep to `ulPortIdleCount`, so the
  CPU really rests and a load meter can be built on it.
- **Crashes:** a non-interrupt exception calls `vPortFatalException(frame,
  cause, badvaddr)`; the application may change the frame (e.g. EPC) and
  return to resume the task somewhere else. The default stops.
- **Ending the scheduler:** `vTaskEndScheduler()` from a task jumps back
  into `xPortStartScheduler` (a small setjmp / longjmp), restores the CPU
  state and `vTaskStartScheduler()` returns, so an app can go back to the
  launcher.

## U-Boot's clock

U-Boot 2012.04's `get_timer()` keeps its milliseconds in CP0 Compare: it
moves Compare past Count one jiffy at a time and counts them. The tick
needs Compare, keeps it ahead of Count, and U-Boot's clock would stand
still (and a `get_timer` interrupted by the tick writes an old Compare
back: no tick for 13 s). Both boxes have the same code, so while a
scheduler runs `sdk_timer_own()` (sdk/runtime.c) patches the first four
words of U-Boot's `get_timer` into a jump to a Count-based clock that runs
with interrupts off and carries on from U-Boot's milliseconds;
`sdk_timer_tick()` (tick hook) keeps it going, `sdk_timer_release()` puts
U-Boot's code and its counter back. The code is checked word by word
first; on an unknown U-Boot the launcher stays without the system.

## Rules for NCAPPS code under FreeRTOS

- U-Boot is not reentrant. USB goes through one owner (the SDK's USB
  lock, a recursive mutex in the system); the rest (serial `printf`, keys)
  only from one task at a time.
- Wait with `vTaskDelay` (or `sdk_idle`, which the system turns into
  `vTaskDelay`) instead of busy waiting, so lower-priority tasks run.
- Nothing resident may use RAM the apps reuse (the shared heap): the system
  keeps everything in the launcher's 2 MB area.

## The NCAPPS system

With Multitasking ON (launcher settings, `system=1` in SETTINGS.TXT, after
a restart) the launcher runs under FreeRTOS (`launcher/system.c`): the menu
and the apps it starts run in the UI task (priority 1); a USB task (3)
notices the stick going and coming back (U-Boot `usb stop` / `usb start`,
remount, a storage generation apps see); a music task (4) plays `/MUSIC`
in the background with PLAY / PAUSE / STOP / NEXT on the remote anywhere,
and pauses while an app plays sound itself. HOME in the MUSIC app's player
hands the playing MP3 over (same spot, same volume, then the rest of its
folder; `music_play` in the table, version 2). Apps get the services through
`"@sys=<table>"` (sdk/sys.h). U-Boot ABI programs (the old `.bin` tools)
run alone as before: the scheduler is suspended and the tick masked while
they run.

## Status

- 2026-10-01, QEMU: PASS. Two equal-priority busy tasks advance equally,
  ~4800 register and ~4900 DSP checks across preemptions with 0 errors,
  queue in order, the tick on time, the scheduler ends and `main` continues.
- 2026-10-01, box (satellite box): PASS. Ran 14 s, 1863 register and 1863 DSP
  checks with 0 errors, queue 703 / 703 (50 per second: the 1 ms tick is
  right at configCPU_COUNT_HZ 324 MHz), the scheduler ended and the app
  returned to the launcher. The overlay showed CPU 100 % (the test keeps four
  tasks busy on purpose, and the meter only counts `sdk_idle`).

- 2026-10-01, QEMU, after the port upgrade (chained / simple vectors, idle
  `wait`): about half the runs showed a counter jumping to a huge value or
  "hung". Cause: Compare written behind Count (the host paused QEMU between
  the read and the write), so no tick until Count wrapped, and the task
  that happened to run spun alone for ~27 s. Fixed with the check after the
  write (see Tick). Since then 12 / 12 runs PASS (plus single-instruction
  TCG mode), idle 99 % while the busy tasks are suspended.
- 2026-10-01: U-Boot's `get_timer` found to use Compare (see U-Boot's
  clock); takeover added. RTOS test now also checks that U-Boot's
  milliseconds keep pace with the tick. Not yet run on the box.
- 2026-10-01, satellite box, Multitasking ON: the system runs; MUSIC app
  HOME hands the song to the background player, it plays on in the menu
  (header "> 5/20 title 0:43"), overlay CPU 1 %, audio queue ~178 ms.
