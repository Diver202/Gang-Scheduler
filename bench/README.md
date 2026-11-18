# Benchmarks

Compares how the VBD cloth simulation runs under different schedulers, loads and barrier types.

## What is compared

| Scheduler | What it is |
|---|---|
| `eevdf` | The default Linux scheduler, nothing loaded. |
| `rt` | Still EEVDF, but the simulation runs as real-time (`chrt -r 10`, SCHED_RR). This is the "simple alternative" a gang scheduler must beat. |
| `simple` | `scx_simple`: a minimal sched_ext scheduler. It measures what using sched_ext costs by itself. |
| `gang` | Our `scx_gang`, with the simulation registered as a gang (`--gang`). |

| Load | What runs |
|---|---|
| `idle` | One simulation (4 worker threads) alone. |
| `stress` | One simulation + `stress-ng --cpu 4` (one CPU hog per core). |
| `oversub` | Two simulations at once (8 workers on 4 cores). Under `gang` they are gangs 0 and 1 (`scx_gang -n 2`). |
| `bgonly` | Only `stress-ng --cpu 4`: the background-throughput reference (added automatically when `stress` is in the list). |

Each is run with the `spin` and the `block` barrier.

Every run lasts a fixed wall time: `--warmup` seconds unmeasured, then `--duration` seconds measured. stress-ng runs for the same total time, so its throughput is comparable between schedulers. Runs are done in a random order within each repetition, to spread out drift (temperature, background activity).

## Before you start

1. **Build** (needed after any code change or a kernel update):
   ```bash
   cd "$HOME/VBD Sim/sim" && make
   cd "$HOME/VBD Sim/scheduler" && make
   ```
   `scx_simple` is used pre-built from `scx-c-examples/build/scheds/c/scx_simple`. Its Makefile can't build inside a path with a space; see NOTES.md.
2. **Quiet the machine:** close Firefox and anything else heavy. Ideally log out of the desktop and run from a text console (Ctrl-Alt-F3) or over SSH. The runner warns if Firefox is open.
3. **No scheduler loaded:** `cat /sys/kernel/sched_ext/state` must say `disabled`.
4. The runner calls `sudo` itself (passwordless sudo is set up), and sets the cpufreq governor to `performance` for the run, restoring the old one at the end.

## Running

```bash
cd "$HOME/VBD Sim"
python3 bench/run_bench.py                 # full matrix, 3 reps, 10 s measured + 2 s warmup (~25 min)
python3 bench/analyze.py bench/results/<timestamp>
```

Useful options of `run_bench.py` (`--help` lists all):

| Option | Default | Meaning |
|---|---|---|
| `--reps N` | 3 | repetitions of the whole matrix |
| `--duration S` / `--warmup S` | 10 / 2 | measured / unmeasured seconds per run |
| `--schedulers ...` | `eevdf rt simple gang` | subset, e.g. `--schedulers eevdf gang` |
| `--loads ...` | `idle stress oversub` | subset |
| `--barriers ...` | `spin block` | subset |
| `--max-temp C` | 60 | wait until the CPU is below this before each run |
| `--keep-governor` | off | don't switch to `performance` |
| `--seed N` | random | fix the run order |
| `--out DIR` | `bench/results/<timestamp>` | output folder |

To change the gang scheduler's window lengths, edit `start_scheduler()` in `run_bench.py` (`scx_gang -g GANG_MS -o OTHER_MS`; defaults 8 and 2 ms).

Ctrl-C is safe: the runner kills the simulations and stress-ng, unloads any sched_ext scheduler and restores the governor. Results written so far are kept, because `results.csv` is flushed after every run.

`analyze.py` accepts several result folders and pools them, for example to add more repetitions later: `python3 bench/analyze.py bench/results/A bench/results/B`. Its output goes in the first folder.

## Output

`bench/results/<timestamp>/`:

| File | Contents |
|---|---|
| `meta.txt` | Arguments, seed, kernel, governor, CPU frequency, load average, temperature, whether Firefox was running |
| `results.csv` | One row per simulation instance per run (two rows for `oversub`, one row with `instance=-1` for `bgonly`) |
| `steps/r<rep>_<sched>_<load>_<barrier>_<instance>.csv` | Every measured step's time, for distributions |
| `logs/` | Full simulation output and scheduler output of every run |
| `summary.md`, `*.png` | Written by `analyze.py` |

`results.csv` columns:

| Column | Meaning |
|---|---|
| `status` | `ok`, `timeout` (killed after duration + `--overrun`) or `no_result` |
| `steps` | measured steps |
| `step_mean_ms`, `step_p50_ms`, `step_p99_ms`, `step_max_ms` | step time statistics |
| `wait_per_step_ms` | average time one worker spends waiting in barriers per step |
| `wait_colour_us` | mean wait at each colour's barrier (µs per thread per crossing), comma-separated |
| `switches`, `voluntary`, `involuntary`, `migrations` | summed over the 4 workers during the measured part, from `/proc/<tid>/sched` |
| `wall_s` | total wall time of the simulation process |
| `bg_bogo_per_s` | stress-ng throughput (bogo-ops per second of real time) |
| `temp_before/after`, `throttled_before/after` | from `vcgencmd`; `throttled` should be `0x0`, otherwise the run was affected by heat or power |

## Notes and gotchas

- **RT runs** use `prlimit --rttime=unlimited`. This desktop session limits real-time tasks to 200 ms of CPU without sleeping (`RLIMIT_RTTIME`), and the kernel SIGKILLs spinning RT threads that hit it. The limit is raised for the simulation process only.
- **Very slow configurations** (spin barrier + `oversub` under `eevdf`/`rt`) can take seconds per step. The simulation always completes at least one measured step, so such runs last longer than `--warmup + --duration`.
- Only one sched_ext scheduler can be loaded at a time. If a run crashes, check `cat /sys/kernel/sched_ext/state` and stop leftovers with `sudo pkill -INT scx_gang` / `sudo pkill -INT scx_simple`.
- `--view` is never used in benchmarks, because the window's drawing would compete with the workers for CPU.
