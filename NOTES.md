# Gang scheduler project — working notes

Project root: `/home/diver202/VBD Sim/` (the VS Code workspace; **everything lives here**, nothing in `~/gang-sched` any more). The path contains a space, so quote it in shell commands. This file is the hand-off record between sessions; keep it up to date.

## Project context

Building a **gang scheduler** for Linux with **sched_ext** (BPF-based schedulers, mainline since 6.12).

Workload: a mass-spring cloth simulation using **Vertex Block Descent (VBD)**:
- Graph colouring of the vertices happens once, at setup.
- Each step, for each colour, a fixed pool of worker threads (one per core) updates that colour's vertices in parallel. All workers then meet at a barrier before the next colour starts.
- The gang scheduler's job is to make all workers run **at the same time** for each colour, so no single descheduled worker stalls the barrier.
- Gang threads are identified through a **BPF map of thread IDs** that the simulation registers.

Experiment:
- Schedulers: default Linux scheduler (EEVDF) vs. the gang scheduler. Suggested extra control: `scx_simple`, to separate the overhead of sched_ext itself from the effect of gang scheduling.
- Load conditions: idle, with stress-ng background load, and oversubscribed.
- Barrier types: spin and blocking.
- Metrics: step time (mean and p99), per-colour barrier wait, context switches, migrations, background-job throughput.
- The Pi is both the development machine and the benchmark machine (bare metal, no hypervisor noise).

Phases: **1 = environment + scx_simple (DONE 2026-11-17)**, **2 = VBD simulation (DONE 2026-11-17, see below)**, **3 = gang scheduler (DONE 2026-11-18)**, **4 = benchmarks (DONE 2026-11-19)**. See the sections below.

## Machine

| Item | Value |
|---|---|
| Board | Raspberry Pi 5, 8 GB RAM, 4× Cortex-A76 (arm64), single cluster / single L3 |
| OS | Ubuntu 26.10 "Stonking Stingray" (fresh flash), GNOME desktop |
| Kernel | `7.3.0-1003-raspi` |
| Storage | SD card `/dev/mmcblk0p2`, 234 GB |
| cpufreq | driver `cpufreq-dt`, 1.5–2.4 GHz in 100 MHz steps, governor **ondemand** (default), boost **disabled** |
| Governors available | conservative ondemand userspace powersave performance schedutil |
| Thermal | 55 °C idle; 60 °C after 10 s of 4-core stress-ng; `get_throttled=0x0`; PWM fan (~4500 rpm under load) |

## Kernel support (checked 2026-11-17)

```
/sys/kernel/sched_ext/state      -> disabled   (file exists; "disabled" = no BPF scheduler loaded)
/proc/config.gz                  -> missing; used /boot/config-7.3.0-1003-raspi
CONFIG_SCHED_CLASS_EXT=y
CONFIG_DEBUG_INFO_BTF=y
CONFIG_DEBUG_INFO_BTF_MODULES=y
CONFIG_BPF_SYSCALL=y, CONFIG_BPF_JIT=y, CONFIG_BPF_JIT_ALWAYS_ON=y
/sys/kernel/btf/vmlinux          -> present (6.1 MB)
```
Other sysfs files: `/sys/kernel/sched_ext/{enable_seq,hotplug_seq,nr_rejected,root/ops,state,switch_all}`.

## Packages / versions

| Package | Version |
|---|---|
| clang / LLVM / lld | 22.1.8 (Ubuntu); `llc --version` lists the `bpf` target |
| libbpf-dev / libbpf1 | 1.6.3-1ubuntu1 |
| bpftool | 7.8.0 (package `bpftool 7.8.0+7.3.0-10.10`, `/usr/sbin/bpftool`; statically uses libbpf 1.8) |
| perf / cpupower | 7.3.0 (from `linux-tools-7.3.0-1003-raspi` + `linux-tools-common`) |
| stress-ng | 0.22.00 |
| meson / ninja | 1.10.1 / 1.13.2 (installed but not used, see below) |
| cpufrequtils | 008 |
| libelf-dev | 0.196 |
| git | 2.55.0 |

Install command used:
```bash
sudo apt-get update
sudo apt-get install -y build-essential clang llvm lld git meson ninja-build pkg-config \
  libbpf-dev libelf-dev zlib1g-dev libseccomp-dev bpftool linux-tools-common \
  linux-tools-$(uname -r) stress-ng cpufrequtils
```
Notes: `bpftool` is its own package on 26.10. `cpupower` and `perf` come from `linux-tools-*` (there is no `linux-cpupower` package). `linux-tools-raspi` is at 1004, newer than the running 1003 kernel, so install the `$(uname -r)` one.

## Repos

- `scx/` — https://github.com/sched-ext/scx @ `3d853e8` (2026-11-17). **Reference only.** This repo is now Rust/cargo only, with **no meson and no C schedulers**. Its `scheds/README.md` says the C examples moved to the kernel's `tools/sched_ext` and to `sched-ext/scx-c-examples`.
- `scx-c-examples/` — https://github.com/sched-ext/scx-c-examples @ `370b1ce` (2026-08-31). **This is what we build.** It contains scx_simple, qmap, central, flatcg, nest, pair, prev and userland, plus `scheds/include/scx/*.bpf.h` headers and a bundled `scheds/vmlinux/vmlinux.h` (arm64).
  - Its meson build is deprecated (it needs `-Dforce_meson=true`, and it would download bpftool/libbpf and build Rust unless told not to; the Rust option is `enable_rust`). The **Makefile** is the primary build and uses the system clang, bpftool and libbpf, so that's what we use.
  - The canonical upstream copies are in the kernel tree under `tools/sched_ext/`. `scx_central` and `scx_pair` are the most relevant examples for gang scheduling (cross-CPU kicking, and pairing tasks on sibling CPUs).

## Build

**Caveat since the move to `/home/diver202/VBD Sim` (path with a space):** the scx-c-examples Makefile does not quote paths, so `make` there now fails with `make[1]: *** /home/diver202/VBD: Is a directory.` (a symlink doesn't help; make resolves it). The binaries built before the move in `scx-c-examples/build/scheds/c/` still work. For the gang scheduler we write our own Makefile with quoted paths (or copy the needed `scheds/include` + `scheds/vmlinux` headers), so we don't depend on this one. The original command, valid only from a space-free path:

```bash
cd scx-c-examples
make -j4            # out-of-tree into ./build; binaries in build/scheds/c/
```
Per scheduler, this runs:
1. `clang -target bpf ... X.bpf.c -o X.bpf.o`
2. `bpftool gen skeleton X.bpf.o name X > X.bpf.skel.h`
3. `cc -std=gnu11 -I scheds/include -I scheds/vmlinux -I build/scheds/c scheds/c/X.c -o X -lbpf -lelf -lz -lzstd -lpthread`

All 8 build with no errors using the bundled vmlinux.h. If kernel-struct mismatches ever show up, regenerate it from the running kernel:
`bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h`.

## scx_simple test (works)

Script: load in background → wait for `enabled` → stress-ng → SIGINT → wait for `disabled`.
```bash
sudo "$HOME/VBD Sim/scx-c-examples/build/scheds/c/scx_simple" > "$HOME/VBD Sim/logs/scx_simple_test.log" 2>&1 &
cat /sys/kernel/sched_ext/state            # enabled
cat /sys/kernel/sched_ext/root/ops         # simple
grep ext.enabled /proc/self/sched          # 1  -> this task is in the ext class
stress-ng --cpu 4 --timeout 10s --metrics-brief
sudo pkill -INT -x scx_simple
cat /sys/kernel/sched_ext/state            # disabled
```
Results:
- `switch_all=1`: full-switch mode, all SCHED_NORMAL tasks were moved to the ext class.
- dmesg: `sched_ext: BPF scheduler "simple" enabled`, then `... disabled (unregistered from user space)`.
- scx_simple counters at the end: `local=805 global=28049`.
- stress-ng cpu ×4, 10 s, rough single-run comparison (not a benchmark):

  | Scheduler | bogo ops/s |
  |---|---|
  | scx_simple | 812 |
  | EEVDF | 806 |

Log: `logs/scx_simple_test.log`.

## Problems hit and fixes

1. **sudo needed a password**, so Claude couldn't run root commands. Fix (user's choice): `/etc/sudoers.d/90-gang-sched` containing `diver202 ALL=(ALL) NOPASSWD: ALL`. Undo with `sudo rm /etc/sudoers.d/90-gang-sched`.
2. **The scx repo no longer has meson or the C schedulers**, so the original plan ("meson, disable Rust") no longer applies. Fix: build the C examples from `scx-c-examples` with `make`.
3. **`vcgencmd` failed with "Can't open device file: /dev/vcio"** because `/dev/vcio*` is `root:root 0600`. Fix: run `sudo vcgencmd measure_temp` / `sudo vcgencmd get_throttled`; no mknod needed. Alternative without root: `/sys/class/thermal/thermal_zone0/temp` (millidegrees C).

## Phase 2: VBD cloth simulation (done 2026-11-17)

Code: `sim/` (C++20, `cd "$HOME/VBD Sim/sim" && make` → `./vbd`, `./vbd --help`).

| File | Contents |
|---|---|
| `src/vec.h` | Vec3 / Mat3, 3×3 solve (Cramer) |
| `src/cloth.{h,cpp}` | Grid cloth, springs, greedy colouring, VBD vertex solve |
| `src/barrier.h` | `SpinBarrier` (atomic counter + generation, `yield` hint) and `BlockingBarrier` (`pthread_barrier_t`, futex sleep) |
| `src/main.cpp` | Options, worker pool, timing, stats output |

Model:
- n×n grid, 1 m × 1 m, 0.2 kg, pinned at corner (0,0), starting horizontal.
- Springs: stretch + shear (k = 2000) and bend (2 apart, k = 50). With n=32 that's 5,766 springs.
- Greedy colouring over the spring graph gives **7 colours**.
- Per vertex: one Newton step on m/(2h²)|x−y|² + Σ spring energy, using a PSD-clamped spring Hessian k·nnᵀ + k·max(0, 1−L/l)(I−nnᵀ).
- Adaptive initial guess (VBD paper): x + h·v + h²·ã·g, where ã = clamp(a_prev·ĝ/|g|, 0, 1).
- Velocity damping is per second (`--damping`, default 1/s).

Threading:
- `--threads T` workers, each owning a static vertex range for predict/velocity and a static slice of every colour.
- Per step: predict → barrier → (iterations × colours: solve slice → barrier) → velocity → barrier. That's **72 barriers per step** by default.
- Thread 0 times the steps. Each thread times its own wait at every colour barrier.
- Per-thread switches/migrations are diffed from `/proc/thread-self/sched` (`nr_switches`, `nr_(in)voluntary_switches`, `se.nr_migrations`). These counters are scheduler-agnostic, so they work under sched_ext too.

Output:
- Human-readable summary plus one `RESULT key=value ...` line for scripts.
- `--csv FILE` writes per-step times.
- `--obj DIR --obj-every K` dumps OBJ frames.

Live view: `cd "$HOME/VBD Sim/sim" && make && ./vbd --view` opens an SDL2 window (`src/viewer.h`, needs `libsdl2-dev`). It runs in real time (paced by thread 0) until the window is closed (or Esc / Ctrl-C). Left/right arrows rotate. Optional extras still apply (`--threads`, `--barrier block`, `--n 48`, …). The main thread does the drawing, so it takes CPU from the workers: **never benchmark with `--view`**. Workers agree on the stop step through a shared `halt` flag that thread 0 sets before the last barrier of a step, so closing the window can't deadlock a barrier. Measured: about 93 % of real time at the defaults with the desktop running.

Defaults: `--n 32 --dt 1/600 --iterations 10 --steps 3000 --warmup 300 --barrier spin --threads 4`.

Problems hit:
1. **The cloth over-stretched badly at high resolution** (n=64, dt=1/60: strain 98×, cloth "falling" to y = −7 m). The cause is Gauss-Seidel non-convergence when k·h²/m is large (about 11,000): the corner can't support the cloth within 10 iterations. It's not a threading bug, since 1 thread does the same. Fixes:
   - adaptive init (98 → 2.0 at the same settings);
   - substeps: dt = 1/600 with n=32 gives about 2–5 % max strain.

   Visual check: falls, swings, settles hanging along the diagonal; lowest point −1.38 m vs. a 1.41 m diagonal.
2. **Step time was 12 ms single-threaded at -O2.** `-O3 -mcpu=native` gives 3.9 ms with identical results; `-ffast-math` adds only another 8 %, so it's not used.
3. **Damping was originally per step**, so smaller dt silently meant more damping. Changed to a per-second rate.

First measurements (desktop + Firefox running, `ondemand` governor, so **noisy, not benchmark quality**). EEVDF, 1350 measured steps:

| Config | Step mean | p99 | Barrier wait / step | Notes |
|---|---|---|---|---|
| 1 thread | 3.9 ms | — | — | pure compute |
| 4 threads, spin | 1.35 ms | 4.4 ms | 0.35 ms | 0 voluntary switches, 0 migrations |
| 4 threads, block | 1.94 ms | 4.0 ms | 0.95 ms | 274k voluntary switches, 2.3k migrations |
| 8 threads, block | 2.54 ms | 6.5 ms | 2.0 ms | |
| 8 threads, spin | **275 ms** | 407 ms | 267 ms (97 %) | ~200× slower: spinners burn full EEVDF slices (~2.1 ms) while the thread everyone waits for is descheduled |

Granularity: at n=32, one colour slice is about 10–15 µs of work per thread, so scheduling latency matters a lot. Raise `--n` for coarser phases (n=64 needs smaller dt or more iterations to stay stiff).

### Open design question for phase 3: what "oversubscribed" means

A gang larger than the core count (8 threads on 4 cores) **cannot** be co-scheduled. Gang scheduling only helps when gang size ≤ cores and the extra load comes from elsewhere. Reasonable definitions:
- (a) two simulation instances × 4 threads (two gangs; the scheduler time-slices whole gangs, as in Ousterhout coscheduling);
- (b) 4 sim threads + more than 4 stress-ng hogs.

The 8-thread single gang can stay as a pathological EEVDF reference point.

## System clock fix (2026-11-18)

Symptom: the Pi booted with the date Oct 11 every time. Causes:
1. The Pi 5 has **no RTC battery** (rtc0 reads 1970 at boot), and systemd falls back to a build-time epoch (Oct 11).
2. Ubuntu's chrony uses **only NTS servers**, which need TLS certificate checks. At Oct 11 the certificates are "not yet valid", so chrony never synced (`TLS handshake ... certificate chain uses not yet valid certificate`).
3. Those NTS sources carry the chrony options `require` + `trust`, so chrony also refuses to use any other source until one of them works (`chronyc selectdata` shows the plain sources stuck at `W`, "waits for others").

Fix:
- `/etc/chrony/sources.d/plain-ntp.sources`: `pool ntp.ubuntu.com iburst maxsources 2 trust require` and `pool 2.pool.ntp.org iburst maxsources 2 trust require` (plain NTP, no certificates; `trust require` lets them satisfy the NTS sources' `require`). The existing `makestep 1 3` then steps the clock on the first updates. Tested by restarting chronyd: synced in about 10 s, stepped by −6.44 s, `timedatectl` reports synchronized. Trade-off: plain NTP isn't authenticated, so on a hostile network the time could be spoofed.
- Installed `fake-hwclock` (load at boot via `fake-hwclock-load.service`, save hourly and at shutdown to `/etc/fake-hwclock.data`), so an offline boot starts from roughly the last shutdown time instead of Oct 11. `fake-hwclock.service` itself is a masked alias; that's normal.
- Hardware fix (not done, needs a `config.txt` change so ask first): a rechargeable RTC battery on the Pi 5's J5 connector, with `dtparam=rtc_bbat_vchg=3000000` in `/boot/firmware/config.txt` to enable charging.
- Still needs the network (Wi-Fi) to come up to correct the time after a boot; until then the date can be roughly the last-saved one.

## Phase 3: gang scheduler (done 2026-11-18)

Code: `scheduler/` (C + BPF, about 330 lines).

| File | Contents |
|---|---|
| `gang_common.h` | Shared by BPF, loader and sim: pin path `/sys/fs/bpf/gang_tids`, `struct gang_member {gang, index}`, `MAX_GANGS 4` |
| `scx_gang.bpf.c` | The scheduler (BPF struct_ops) |
| `scx_gang.c` | Loader: sets parameters, loads, pins the map, attaches, prints stats every second, and on Ctrl-C unpins and detaches (so the system is back on EEVDF) |
| `Makefile` | Relative paths only (the folder name has a space). Uses headers and vmlinux.h from `../scx-c-examples/scheds` |

### How it works

- **Membership:** hash map `gang_tids`, thread id → `{gang, index}`. The simulation adds its own workers (`--gang G`) through the pinned map. The scheduler deletes entries in `ops.exit_task`, so exited threads clean up automatically.
- **Home CPUs:** gang worker *i* always runs on CPU *i % nr_cpus* (`select_cpu` returns it). It's queued in its own dispatch queue `GANG_DSQ(gang, cpu)`, one per gang per CPU. Result: no migrations, and no two members of a gang on the same core. A member whose home CPU isn't in its affinity mask is treated as background.
- **Background:** every other task goes in one shared weighted-vtime DSQ (the same logic as `scx_simple`).
- **Windows (the gang part):** a global `window` variable cycles gang 0 → gang 1 → … → others, driven by a BPF timer. Gang windows last `-g` ms (default 8) and the others window `-o` ms (default 2). On every switch the timer calls `scx_bpf_kick_cpu(cpu, SCX_KICK_PREEMPT)` on **all** CPUs, so the whole machine changes window together. A gang is therefore always run or descheduled as a whole, never core by core as EEVDF does. The others window is skipped when the shared DSQ is empty, so an idle machine never interrupts the gang.
- **`dispatch(cpu)` order:** the current gang's queue for this CPU → the shared queue → any other gang's queue. That makes it work-conserving: a core whose gang thread is blocked (blocking barrier) runs background work instead of idling.
- **Wake-up preemption:** when a gang thread is enqueued during its own window, it kicks its home CPU with `SCX_KICK_PREEMPT`, so a background task running there gives the core up immediately.
- **Mode:** full switch (no `SCX_OPS_SWITCH_PARTIAL`), so stress-ng and the desktop are scheduled by us too. That's necessary, because the ext class sits below fair: in partial mode, background tasks left in EEVDF would always win.
- **Safety:** the sched_ext watchdog ejects the scheduler if any runnable task waits more than 30 s, and SysRq-S does the same by hand. Background always gets the others window, so it isn't starved.

### Running it

```bash
cd "$HOME/VBD Sim/scheduler" && make           # builds build/scx_gang
sudo ./build/scx_gang                          # terminal 1: load; Ctrl-C = back to EEVDF
cd "$HOME/VBD Sim/sim" && sudo ./vbd --gang 0  # terminal 2: workers join gang 0
# watch it under the gang scheduler (root needs the desktop's display variables):
sudo --preserve-env=WAYLAND_DISPLAY,XDG_RUNTIME_DIR,DISPLAY,XAUTHORITY ./vbd --view --gang 0
```

- `sudo` is needed for `--gang` because `/sys/fs/bpf` is root-only (mode 700) and `unprivileged_bpf_disabled=2`.
- Without `--gang`, the simulation is just background work under scx_gang.
- Loader options: `-g GANG_MS -o OTHER_MS -n NR_GANGS -v`. For two simulations at once use `-n 2` and `--gang 0` / `--gang 1`.

### Tests (2026-11-18; desktop + Firefox running, `ondemand` governor, so noisy single runs, not phase-4 quality)

- Load → `state=enabled`, `ops=gang`; Ctrl-C → `disabled`, pin removed. dmesg shows only enable/disable lines.
- Registration: 4 gang threads while the sim runs, back to 0 after it exits. **0 migrations** for every gang thread. About 200 involuntary switches per thread per 1.7 s, which equals the number of others windows (desktop always has work).
- Same scheduler, no other load: registered as a gang gives p99 3.1 ms; unregistered gives p99 10.9 ms (mean 1.23 vs 1.41 ms).
- With 4 stress-ng CPU hogs (600 steps, 60 warmup):

| Scheduler | Barrier | Step mean | Step p99 | Barrier wait / step | Migrations |
|---|---|---|---|---|---|
| EEVDF | spin | 107 ms | 534 ms | 102 ms | 217 |
| EEVDF | block | 11.8 ms | 71 ms | 10.6 ms | 103 |
| gang | spin | **1.23 ms** | **3.1 ms** | 0.10 ms | 0 |
| gang | block | **2.85 ms** | **12.0 ms** | 1.6 ms | 0 |

- **Background share:** stress-ng run entirely inside a gang-spin run got 97.5 bogo-ops/s (about 12 % of its 806 alone; 3.1 s CPU over 8 s × 4 cores). Under EEVDF spin it got 394 (about half). The nominal 20 % others window is shared with the desktop. This is the trade-off: the `-g/-o` ratio sets it, and phase 4 should sweep it.
- `--view` under sudo with preserved display variables works, and runs at exactly real time under scx_gang.

### Known limitations / design notes

- **A gang larger than the core count** puts 2 members in one home queue; they then time-share and the spin barrier is as bad as under EEVDF. Gangs must have ≤ nr_cpus threads.
- **Two gangs** (`-n 2`) are time-sliced as whole gangs: gang 0 window, gang 1 window, others. This is Ousterhout-style coscheduling, and it's the natural "oversubscribed" scenario for phase 4 (two 4-thread sims), alongside 4 sim threads + more than 4 stress-ng hogs.
- Gang threads register at their start, so the first few steps can run before the scheduler sees them. The warmup steps cover this.
- Window switches kick every CPU (about 200 times a second while background work waits). That's cheap, but it costs each gang thread one context switch per window.
- `vmlinux.h` is the bundled v6.18 one. CO-RE relocation makes it work on 7.3. If a field ever fails to relocate, regenerate with `bpftool btf dump file /sys/kernel/btf/vmlinux format c`.

### Problems hit

1. `enum { ..., NR_STATS }` clashed with a kernel enum in vmlinux.h. Renamed it `NR_GANG_STATS`.
2. Running the viewer under sudo needs `--preserve-env=WAYLAND_DISPLAY,XDG_RUNTIME_DIR,DISPLAY,XAUTHORITY`.

## Phase 4: benchmarks (done 2026-11-19)

How to run them again: **`bench/README.md`** (build, quiet the machine, `python3 bench/run_bench.py`, then `python3 bench/analyze.py bench/results/<timestamp>`).

| File | Contents |
|---|---|
| `bench/run_bench.py` | The matrix runner |
| `bench/analyze.py` | Writes `summary.md` and 3 plots |
| `bench/results/20261118-234242/` | The first full dataset: `summary.md`, `step_time.png`, `background.png`, `step_cdf.png`, `results.csv`, per-step CSVs, logs |

Matrix:
- **Schedulers:**
  - EEVDF;
  - EEVDF + `chrt -r 10` (SCHED_RR, the "simple alternative");
  - scx_simple (the cost of sched_ext alone);
  - scx_gang (8 ms gang / 2 ms others windows).
- **Loads:** idle, stress (+ `stress-ng --cpu 4`), oversub (two simulations, 8 workers; `scx_gang -n 2`), plus stress-only baselines.
- **Barriers:** spin and block.
- **Repetitions:** 3, in random order. Each run is 2 s warmup + 10 s measured. 81 runs, all `ok`.

Conditions: performance governor at 2.4 GHz, Firefox closed, but the GNOME desktop, VS Code and Claude Code were still running. 52–62 °C, never throttled.

Simulation changes made for benchmarking:
- `--duration SEC` / `--warmup-sec SEC`: fixed wall-time runs. Thread 0 decides `measuring`/`halt` before the last barrier of a step, so all threads agree.
- Always completes at least one measured step.
- `RESULT` now also has `wall_s` and `wait_colour_us`.

### Results (mean step time in ms, mean of 3 runs; full tables in summary.md)

| Load · barrier | EEVDF | EEVDF + RR | scx_simple | scx_gang | gang vs EEVDF |
|---|---|---|---|---|---|
| idle · spin | 0.98 | 0.97 | 0.98 | 1.05 | 0.93× (slightly slower) |
| idle · block | 1.21 | 1.20 | 1.34 | 1.48 | 0.82× (slower) |
| stress · spin | 139 | 1.13 | 1377 | **1.21** | **115×** |
| stress · block | 48.9 | 1.34 | 4.38 | **2.16** | **23×** |
| oversub · spin | 116 | 4888 | 728 | **1.97** | **59×** |
| oversub · block | 2.58 | 2.49 | 2.70 | 2.91 | 0.88× (slightly slower) |

Background throughput with stress-ng (% of stress-ng alone; alone = 907 bogo-ops/s, the same under all schedulers):

| Barrier | EEVDF | EEVDF + RR | scx_simple | scx_gang |
|---|---|---|---|---|
| spin | 51 % | 4 % | 50 % | 20 % |
| block | 91 % | 7 % | 76 % | 31 % |

Migrations: **scx_gang 0 in every condition**. EEVDF 0–245 per second; RR up to 35k per second and scx_simple up to 53k per second (oversub · block).

### What the results mean (viva notes)

1. **Under contention, the gang scheduler fixes the barrier problem.** With 4 CPU hogs and a spin barrier, EEVDF takes 139 ms per step instead of about 1 ms. Each core time-slices a worker against a hog independently, so at almost every one of the 72 barriers per step some worker is descheduled while the other three spin. The barrier wait was about 1.9 ms per colour per thread. scx_gang runs the 4 workers together, so the wait drops to about 1 µs per colour, and the step is 1.21 ms (115× faster). The p99 drops from 306 ms to 3.0 ms.
2. **Compared with the simple alternative (real-time priority), it's similarly fast but fairer.** SCHED_RR is about as fast (1.13 ms with spin) but leaves the background only 4–7 % of its throughput. RT is strict priority, and only the kernel's fair-server reserve (5 %) lets other tasks run. scx_gang gives the background 20 % (spin) or 31 % (block), about 4–5× more, because of its guaranteed "others" window. The window ratio (`-g`/`-o`) is a tunable knob between simulation speed and background share, which RT priority doesn't offer. With two simulations and spin barriers, RR collapses (4.9 s per step, huge variance): two RT gangs time-share each core in uncoordinated 100 ms slices.
3. **sched_ext by itself is not the fix; the policy is.** scx_simple, with 20 ms slices and no preemption, is the worst of all with spin barriers (1.4 s per step, about 19 ms per colour barrier: a full slice each time). Yet stress-ng alone runs at the same speed under all three, so the BPF overhead itself is negligible for CPU-bound work.
4. **Coscheduling two gangs works as intended.** With two simulations and spin barriers, scx_gang gives each 511 steps/s, exactly half of one simulation alone (1024), with no barrier collapse. Every other scheduler is 50–2000× slower there.
5. **The step-time distribution shows the windows directly** (`step_cdf.png`). Under stress + spin, 88 % of gang steps take about 1.0 ms and 12 % about 3 ms: those are the steps that ran into a 2 ms others window. EEVDF's distribution is spread out from 1 ms to 300 ms.
6. **It costs something when it isn't needed.**
   - *Idle:* 7 % slower (spin) and 22 % slower (block). The desktop always has some background work, so the others window still preempts the gang about 100 times a second. Blocking wake-ups go through our custom queues and a kick instead of EEVDF's direct wake-up path.
   - *Oversub + block:* EEVDF already interleaves two blocking simulations well (2.58 ms). scx_gang's strict windows make each gang wait for the other's 8 ms window (p99 8.75 ms vs 4.10 ms), and the wake-up kicks cause about 34k involuntary switches per second.

   The honest conclusion: gang scheduling pays off when barrier-synchronised threads compete for cores, and is slightly worse when they don't.

### Caveats

- 3 repetitions. The EEVDF stress cases vary a lot between runs (e.g. stress · block 48.9 ± 38 ms), but every conclusion above holds in each individual run.
- The desktop was running. For final numbers, rerun from a text console.
- One window setting (8/2 ms) was measured.
- Barrier phases are fine-grained: about 10–15 µs of work per colour per thread at n=32. Coarser workloads (`--n 48/64`) should suffer less under EEVDF.

### Possible next steps

- Sweep the windows (`-g 4/8/16`, `-o 1/2/4`) to draw the simulation-speed vs. background-throughput trade-off curve.
- Run larger cloths.
- Fix the two weak spots, then re-measure:
  - only rotate windows when another gang or background task is actually waiting;
  - don't preempt another gang's thread for a wake-up when the core would otherwise idle soon.
- Rerun from a text console with more repetitions for the report.
