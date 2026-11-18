#!/usr/bin/env python3
"""Runs the benchmark matrix: scheduler x load x barrier x repetition.

Run as your normal user (it calls sudo itself). See bench/README.md.
Results go to bench/results/<timestamp>/ (results.csv, per-step CSVs, raw logs, meta.txt).
"""
import argparse
import csv
import datetime
import random
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VBD = ROOT / "sim" / "vbd"
SCX_GANG = ROOT / "scheduler" / "build" / "scx_gang"
SCX_SIMPLE = ROOT / "scx-c-examples" / "build" / "scheds" / "c" / "scx_simple"
STATE = Path("/sys/kernel/sched_ext/state")

SCHEDULERS = ["eevdf", "rt", "simple", "gang"]
LOADS = ["idle", "stress", "oversub"]
BARRIERS = ["spin", "block"]
SIM_FIELDS = ["steps", "step_mean_ms", "step_p50_ms", "step_p99_ms", "step_max_ms", "wait_per_step_ms",
              "switches", "voluntary", "involuntary", "migrations", "wall_s", "wait_colour_us"]
COLUMNS = ["time", "rep", "scheduler", "load", "barrier", "instance", "status"] + SIM_FIELDS + \
          ["bg_bogo_per_s", "temp_before", "temp_after", "throttled_before", "throttled_after"]


def sh(cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout.strip()


def temperature():
    m = re.search(r"temp=([\d.]+)", sh(["sudo", "vcgencmd", "measure_temp"]))
    return float(m.group(1)) if m else float("nan")


def throttled():
    return sh(["sudo", "vcgencmd", "get_throttled"]).replace("throttled=", "")


def cool_down(max_temp, limit_s=180):
    start = time.time()
    while temperature() > max_temp and time.time() - start < limit_s:
        time.sleep(5)


GOVERNOR = Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")


def set_governor(name):
    for policy in Path("/sys/devices/system/cpu/cpufreq").glob("policy*"):
        subprocess.run(["sudo", "tee", str(policy / "scaling_governor")], input=name, text=True, capture_output=True)
    if GOVERNOR.read_text().strip() != name:
        raise RuntimeError(f"could not set the cpufreq governor to {name}")


def wait_state(want, timeout=10):
    end = time.time() + timeout
    while time.time() < end:
        if STATE.read_text().strip() == want:
            return True
        time.sleep(0.1)
    return False


def start_scheduler(name, nr_gangs, log):
    if name == "gang":
        cmd = ["sudo", str(SCX_GANG), "-n", str(nr_gangs)]
    elif name == "simple":
        cmd = ["sudo", str(SCX_SIMPLE)]
    else:
        return None
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
    if not wait_state("enabled"):
        stop_scheduler(proc)
        raise RuntimeError(f"{name} did not load (see {log.name})")
    return proc


def stop_scheduler(proc):
    if proc is None:
        return
    subprocess.run(["sudo", "pkill", "-INT", "-x", Path(proc.args[1]).name])
    proc.wait(timeout=30)
    if not wait_state("disabled"):
        raise RuntimeError("sched_ext did not return to disabled")


def parse_result(text):
    line = next((l for l in text.splitlines() if l.startswith("RESULT ")), None)
    if line is None:
        return None
    return dict(kv.split("=", 1) for kv in line.split()[1:])


def parse_bogo(text):
    m = re.search(r"metrc:.*\bcpu\s+\d+\s+[\d.]+\s+[\d.]+\s+[\d.]+\s+([\d.]+)", text)
    return float(m.group(1)) if m else None


def run_one(args, outdir, rep, sched, load, barrier):
    """One measurement. Returns a list of CSV rows (one per simulation instance, or one for bgonly)."""
    tag = f"r{rep}_{sched}_{load}_{barrier}"
    cool_down(args.max_temp)
    base = {"time": datetime.datetime.now().isoformat(timespec="seconds"), "rep": rep, "scheduler": sched,
            "load": load, "barrier": barrier, "temp_before": temperature(), "throttled_before": throttled()}

    nr_sims = 0 if load == "bgonly" else 2 if load == "oversub" else 1
    total = args.warmup + args.duration
    with open(outdir / "logs" / f"{tag}_sched.log", "w") as slog:
        proc = start_scheduler(sched, 2 if load == "oversub" else 1, slog)
        try:
            stress = None
            if load in ("stress", "bgonly"):
                stress = subprocess.Popen(["stress-ng", "--cpu", "4", "--timeout", f"{total:.0f}s", "--metrics-brief"],
                                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            sims = []
            for i in range(nr_sims):
                # prlimit: this session limits real-time tasks to 200 ms of CPU without blocking (RLIMIT_RTTIME),
                # which kills spinning RT threads, so lift it for this one process.
                rt = ["prlimit", "--rttime=unlimited", "chrt", "-r", "10"] if sched == "rt" else []
                cmd = ["sudo"] + rt + \
                      [str(VBD), "--barrier", barrier, "--duration", str(args.duration), "--warmup-sec", str(args.warmup),
                       "--csv", str(outdir / "steps" / f"{tag}_{i}.csv")] + (["--gang", str(i)] if sched == "gang" else [])
                sims.append(subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))

            outputs = []
            for sim in sims:
                try:
                    outputs.append((sim.communicate(timeout=total + args.overrun)[0], "ok"))
                except subprocess.TimeoutExpired:
                    subprocess.run(["sudo", "kill", "-9", str(sim.pid)])
                    outputs.append((sim.communicate()[0], "timeout"))

            bogo = None
            if stress:
                bogo = parse_bogo(stress.communicate(timeout=total + 30)[0])
        finally:
            stop_scheduler(proc)

    base.update(temp_after=temperature(), throttled_after=throttled(), bg_bogo_per_s=bogo)
    if nr_sims == 0:
        return [dict(base, instance=-1, status="ok")]

    rows = []
    for i, (text, status) in enumerate(outputs):
        (outdir / "logs" / f"{tag}_{i}.log").write_text(text)
        result = parse_result(text) or {}
        if not result and status == "ok":
            status = "no_result"
        rows.append(dict(base, instance=i, status=status, **{k: result.get(k) for k in SIM_FIELDS}))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--duration", type=float, default=10, help="measured seconds per run")
    ap.add_argument("--warmup", type=float, default=2, help="unmeasured seconds at the start of each run")
    ap.add_argument("--schedulers", nargs="+", default=SCHEDULERS, choices=SCHEDULERS)
    ap.add_argument("--loads", nargs="+", default=LOADS, choices=LOADS)
    ap.add_argument("--barriers", nargs="+", default=BARRIERS, choices=BARRIERS)
    ap.add_argument("--max-temp", type=float, default=60, help="wait for the CPU to cool below this before each run")
    ap.add_argument("--overrun", type=float, default=120, help="extra seconds before a slow run is killed")
    ap.add_argument("--keep-governor", action="store_true", help="don't switch the cpufreq governor to performance")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()

    if STATE.read_text().strip() != "disabled":
        sys.exit("a sched_ext scheduler is already loaded; stop it first")
    for binary in [VBD, SCX_GANG, SCX_SIMPLE]:
        if not binary.exists():
            sys.exit(f"missing {binary}; build it first (see bench/README.md)")
    if sh(["pgrep", "-x", "firefox"]):
        print("warning: Firefox is running and will add noise")

    seed = args.seed if args.seed is not None else random.randrange(1 << 30)
    rng = random.Random(seed)
    outdir = args.out or ROOT / "bench" / "results" / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    (outdir / "logs").mkdir(parents=True, exist_ok=True)
    (outdir / "steps").mkdir(exist_ok=True)

    old_governor = GOVERNOR.read_text().strip()
    if not args.keep_governor:
        set_governor("performance")

    runs = [(s, l, b) for s in args.schedulers for l in args.loads for b in args.barriers]
    if "stress" in args.loads:
        runs += [(s, "bgonly", "-") for s in args.schedulers if s != "rt"]

    (outdir / "meta.txt").write_text("\n".join([
        f"started {datetime.datetime.now().isoformat(timespec='seconds')}",
        f"args {vars(args)}", f"seed {seed}", f"kernel {sh(['uname', '-r'])}",
        f"governor {GOVERNOR.read_text().strip()} (was {old_governor})", f"cpu_freq_khz {sh(['cat', '/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq'])}",
        f"firefox_running {bool(sh(['pgrep', '-x', 'firefox']))}",
        f"load_average {sh(['cat', '/proc/loadavg'])}", f"temp {temperature()} throttled {throttled()}", ""]))

    print(f"{len(runs)} runs x {args.reps} reps -> {outdir}")
    try:
        with open(outdir / "results.csv", "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=COLUMNS)
            writer.writeheader()
            done = 0
            for rep in range(args.reps):
                order = runs[:]
                rng.shuffle(order)
                for sched, load, barrier in order:
                    done += 1
                    rows = run_one(args, outdir, rep, sched, load, barrier)
                    writer.writerows(rows)
                    f.flush()
                    r = rows[0]
                    print(f"[{done}/{len(runs) * args.reps}] rep {rep} {sched:6} {load:7} {barrier:5} "
                          f"step mean {r.get('step_mean_ms') or '-':>9} p99 {r.get('step_p99_ms') or '-':>9} "
                          f"bg {r.get('bg_bogo_per_s') or '-'}  {r['status']}  {r['temp_after']}C", flush=True)
    finally:
        subprocess.run(["sudo", "pkill", "-x", "vbd"])
        subprocess.run(["sudo", "pkill", "-x", "stress-ng"])
        subprocess.run(["sudo", "pkill", "-INT", "-x", "scx_gang"])
        subprocess.run(["sudo", "pkill", "-INT", "-x", "scx_simple"])
        if not args.keep_governor:
            set_governor(old_governor)
        print(f"governor restored to {old_governor}; results in {outdir}")


if __name__ == "__main__":
    main()
