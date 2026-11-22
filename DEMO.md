# Demo cheat sheet

All commands, in the order you would use them in the presentation.
Open the VS Code terminal with Ctrl+`. Use a second and third terminal with the `+` button.

Paths contain a space, so always keep the quotes around "$HOME/VBD Sim".

---

## 0. Before you start (once)

Close Firefox and anything heavy. Then check that no scheduler is loaded:

```bash
cat /sys/kernel/sched_ext/state
```
Expected: `disabled`. If it says `enabled`, stop the leftover scheduler:
```bash
sudo pkill -INT scx_gang
```

Build (only needed if you changed code, or the kernel was updated):
```bash
cd "$HOME/VBD Sim/sim" && make
cd "$HOME/VBD Sim/scheduler" && make
```

---

## 1. The simulation with EEVDF (the default Linux scheduler)

Terminal 1:
```bash
cd "$HOME/VBD Sim/sim"
```

With the window (arrow keys rotate, Esc quits):
```bash
./vbd --view
```

Numbers only, spin barrier (default):
```bash
./vbd --steps 3000 --barrier spin
```

Numbers only, blocking barrier:
```bash
./vbd --steps 3000 --barrier block
```

---

## 2. The simulation with the gang scheduler

### Terminal 1: start the scheduler, leave it running
```bash
cd "$HOME/VBD Sim/scheduler/build"
sudo ./scx_gang
```
It prints `gang scheduler loaded: 1 gang(s)...` and then one stats line per second.
Check from any other terminal: `cat /sys/kernel/sched_ext/state` should say `enabled`.

### Terminal 2: run the simulation as gang 0
```bash
cd "$HOME/VBD Sim/sim"
sudo ./vbd --gang 0 --steps 3000 --barrier spin
sudo ./vbd --gang 0 --steps 3000 --barrier block
```

With the window (the long `--preserve-env` part passes the display through sudo):
```bash
sudo --preserve-env=WAYLAND_DISPLAY,XDG_RUNTIME_DIR,DISPLAY,XAUTHORITY ./vbd --gang 0 --view
```

What to point out in Terminal 1 while it runs:
- `gang threads` goes from 0 to 4 (the scheduler found the 4 workers)
- `gang enqueues` keeps climbing
- `window switches` keeps climbing (the timer is flipping windows)

### Stop the scheduler
Press Ctrl-C in Terminal 1. The kernel goes straight back to EEVDF.

Optional scheduler settings (run these instead of plain `sudo ./scx_gang`):
```bash
sudo ./scx_gang -g 8 -o 2       # gang window 8 ms, others window 2 ms (the defaults)
sudo ./scx_gang -n 2            # two gangs (for two simulations at once)
```

---

## 3. The live comparison (the main demo)

You need a CPU hog that competes with the simulation. Terminal 3:
```bash
stress-ng --cpu 4 --timeout 120s
```
This starts 4 busy processes, one per core. Leave it running.

### A. EEVDF under load (no scheduler loaded; check `disabled` first)
Terminal 1:
```bash
cd "$HOME/VBD Sim/sim"
./vbd --steps 200 --barrier spin
./vbd --steps 200 --barrier block
```
Expect spin around 139 ms per step (very slow), block around 49 ms per step.
Use `--steps 200` here because these runs are slow.

### B. Gang under the same load
Terminal 1 (leave running):
```bash
cd "$HOME/VBD Sim/scheduler/build"
sudo ./scx_gang
```
Terminal 2:
```bash
cd "$HOME/VBD Sim/sim"
sudo ./vbd --gang 0 --steps 3000 --barrier spin
sudo ./vbd --gang 0 --steps 3000 --barrier block
```
Expect spin around 1.2 ms per step, block around 2.2 ms per step.

### C. Clean up
```bash
# Ctrl-C in Terminal 1 (scheduler), Ctrl-C in Terminal 3 (stress-ng) if still running
cat /sys/kernel/sched_ext/state      # should say: disabled
```

---

## 4. The benchmark results (already measured)

```bash
cd "$HOME/VBD Sim/bench/results/20261118-234242"
cat summary.md
```
Open these pictures from the VS Code file explorer, in that folder:
- `step_time.png`: mean step time per scheduler and load
- `background.png`: how much work the background programs got done
- `step_cdf.png`: distribution of step times

Numbers to remember (mean step time in ms, EEVDF then gang):

| Case | EEVDF | gang |
|---|---|---|
| stress, spin | 139 | 1.21 |
| stress, block | 48.9 | 2.16 |
| oversub (two simulations), spin | 116 | 1.97 |
| oversub, block | 2.58 | 2.91 |
| idle, spin | 0.98 | 1.05 |
| idle, block | 1.21 | 1.48 |

Cost of gang: the background program keeps only 20 % (spin) or 31 % (block) of its normal throughput.
Under EEVDF it keeps 51 % or 91 %. Gang threads had 0 migrations.

---

## 5. Re-running the benchmarks

Takes about 25 minutes. Close Firefox first, and do not use the machine while it runs.

```bash
cd "$HOME/VBD Sim"
python3 bench/run_bench.py
python3 bench/analyze.py bench/results/<the-new-folder>
```

Quick version, about 1 minute, to prove it works:
```bash
python3 bench/run_bench.py --reps 1 --duration 5 --schedulers eevdf gang --loads stress --barriers spin block
```

All options: `python3 bench/run_bench.py --help`, or see `bench/README.md`.

---

## 6. Troubleshooting

| Problem | Fix |
|---|---|
| `cannot open /sys/fs/bpf/gang_tids` | `scx_gang` is not running, or you forgot `sudo` on `./vbd` |
| `gang threads` stays at 0 | the simulation was started without `--gang 0`, or without `sudo` |
| State says `enabled` and nothing is running | `sudo pkill -INT scx_gang` |
| Cannot open the window under sudo | use the `--preserve-env=...` command from section 2 |
| Window will not open at all | check you are in the desktop session, not over plain SSH |
| Everything is slow even with gang | close Firefox, check `stress-ng` is not left running: `pkill stress-ng` |
| Fan or heat worries | `vcgencmd get_throttled` should say `0x0` |

---

## 7. The 6-sentence story

1. The cloth simulation runs 4 threads that wait for each other at a barrier, 72 times per step.
2. The default scheduler (EEVDF) does not know the 4 threads belong together, so one is often not running and the other 3 wait. Under load this is very slow.
3. sched_ext lets us write the scheduling rules as a small BPF program that the kernel verifies and runs, with no kernel rebuild.
4. Our rule is gang scheduling: the 4 threads each get their own CPU and run at the same time, in time windows (8 ms gang, 2 ms others), switched on all CPUs together by a timer.
5. The simulation registers its threads in a BPF map, which is how the scheduler knows who is in the gang.
6. Result: 139 ms to 1.2 ms per step under load. The cost is that background programs get less CPU.
