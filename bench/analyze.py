#!/usr/bin/env python3
"""Summarises one or more benchmark result folders.

Usage: python3 bench/analyze.py bench/results/<timestamp> [more folders...]
Writes summary.md and plots (step_time.png, background.png, step_cdf.png) into the first folder.
"""
import csv
import statistics as st
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker
import numpy as np

SCHEDULERS = ["eevdf", "rt", "simple", "gang"]
LABELS = {"eevdf": "EEVDF (default)", "rt": "EEVDF + SCHED_RR", "simple": "scx_simple", "gang": "scx_gang"}
COLOURS = {"eevdf": "#2a78d6", "rt": "#eb6834", "simple": "#1baf7a", "gang": "#eda100"}
LOADS = ["idle", "stress", "oversub"]
BARRIERS = ["spin", "block"]
SURFACE, INK, INK2, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"


def load_rows(folders):
    rows = []
    for folder in folders:
        with open(folder / "results.csv") as f:
            for r in csv.DictReader(f):
                r["folder"] = folder
                rows.append(r)
    return rows


def num(r, key):
    try:
        return float(r[key])
    except (TypeError, ValueError):
        return None


def per_run(rows):
    """Averages the simulation instances of each run: {(sched, load, barrier): [run dict, ...]}."""
    groups = defaultdict(list)
    for r in rows:
        if r["load"] != "bgonly" and r["status"] == "ok":
            groups[(r["folder"], r["rep"], r["scheduler"], r["load"], r["barrier"])].append(r)
    runs = defaultdict(list)
    for (_, _, sched, load, barrier), inst in groups.items():
        run = {}
        for key in ["step_mean_ms", "step_p99_ms", "wait_per_step_ms"]:
            run[key] = st.mean(num(r, key) for r in inst)
        measured_s = [num(r, "steps") * num(r, "step_mean_ms") / 1000 for r in inst]
        for key in ["migrations", "involuntary", "voluntary"]:
            run[key + "_per_s"] = st.mean(num(r, key) / s for r, s in zip(inst, measured_s))
        run["steps_per_s"] = st.mean(num(r, "steps") / s for r, s in zip(inst, measured_s))
        run["bg"] = num(inst[0], "bg_bogo_per_s")
        run["colour_us"] = np.mean([[float(x) for x in r["wait_colour_us"].split(",")] for r in inst], axis=0)
        runs[(sched, load, barrier)].append(run)
    return runs


def mean_sd(values):
    values = [v for v in values if v is not None]
    if not values:
        return None, None
    return st.mean(values), (st.stdev(values) if len(values) > 1 else 0.0)


def fmt(m, s, digits=2):
    if m is None:
        return "–"
    return f"{m:.{digits}f} ± {s:.{digits}f}"


def table(runs, key, digits=2, loads=LOADS):
    lines = ["| load | barrier | " + " | ".join(LABELS[s] for s in SCHEDULERS) + " |", "|---|---|" + "---|" * len(SCHEDULERS)]
    for load in loads:
        for barrier in BARRIERS:
            cells = [fmt(*mean_sd([r[key] for r in runs.get((s, load, barrier), [])]), digits) for s in SCHEDULERS]
            lines.append(f"| {load} | {barrier} | " + " | ".join(cells) + " |")
    return "\n".join(lines)


def style(ax):
    ax.set_facecolor(SURFACE)
    for side in ["top", "right"]:
        ax.spines[side].set_visible(False)
    for side in ["left", "bottom"]:
        ax.spines[side].set_color(GRID)
    ax.tick_params(colors=INK2, labelsize=9)
    ax.grid(color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)


def legend(fig):
    handles = [plt.Line2D([], [], marker="o", linestyle="", markersize=8, color=COLOURS[s], label=LABELS[s]) for s in SCHEDULERS]
    fig.legend(handles=handles, loc="upper center", ncol=4, frameon=False, fontsize=9, labelcolor=INK)


def plot_step_time(runs, out):
    conditions = [(l, b) for l in LOADS for b in BARRIERS]
    fig, axes = plt.subplots(1, 2, figsize=(11, 5), sharey=True, facecolor=SURFACE)
    for ax, key, title in zip(axes, ["step_mean_ms", "step_p99_ms"], ["Mean step time", "p99 step time"]):
        style(ax)
        ax.set_xscale("log")
        for i, (load, barrier) in enumerate(conditions):
            for j, s in enumerate(SCHEDULERS):
                vals = [r[key] for r in runs.get((s, load, barrier), [])]
                if not vals:
                    continue
                y = i + (j - 1.5) * 0.17
                ax.plot([min(vals), max(vals)], [y, y], color=COLOURS[s], linewidth=2, solid_capstyle="round")
                ax.plot(st.mean(vals), y, "o", color=COLOURS[s], markersize=8, markeredgecolor=SURFACE, markeredgewidth=2)
        ax.set_yticks(range(len(conditions)), [f"{l} · {b}" for l, b in conditions])
        ax.set_xlabel("ms per step (log scale) · dot = mean of runs, line = range", color=INK2, fontsize=9)
        ax.set_title(title, color=INK, fontsize=11, loc="left")
    axes[0].invert_yaxis()
    legend(fig)
    fig.tight_layout(rect=(0, 0, 1, 0.93))
    fig.savefig(out, dpi=150, facecolor=SURFACE)
    plt.close(fig)


def plot_background(runs, baseline, out):
    fig, ax = plt.subplots(figsize=(7, 4.2), facecolor=SURFACE)
    style(ax)
    ax.grid(axis="x", visible=False)
    width = 0.19
    for j, s in enumerate(SCHEDULERS):
        for i, barrier in enumerate(BARRIERS):
            m, sd = mean_sd([r["bg"] / baseline * 100 for r in runs.get((s, "stress", barrier), []) if r["bg"]])
            if m is None:
                continue
            x = i + (j - 1.5) * (width + 0.02)
            ax.bar(x, m, width, color=COLOURS[s], label=LABELS[s] if i == 0 else None, edgecolor=SURFACE, linewidth=2)
            ax.errorbar(x, m, yerr=sd, color=INK2, linewidth=1, capsize=3)
            ax.text(x, m + sd + 1.5, f"{m:.0f}%", ha="center", color=INK, fontsize=8)
    ax.set_xticks(range(len(BARRIERS)), [f"{b} barrier" for b in BARRIERS])
    ax.set_ylabel("stress-ng throughput, % of running alone", color=INK2, fontsize=9)
    ax.set_title("Background work done while the simulation runs (4 stress-ng hogs)", color=INK, fontsize=11, loc="left")
    ax.legend(frameon=False, fontsize=9, ncol=2, labelcolor=INK)
    ax.set_ylim(0, 110)
    fig.tight_layout()
    fig.savefig(out, dpi=150, facecolor=SURFACE)
    plt.close(fig)


def step_samples(folders, sched, load, barrier):
    samples = []
    for folder in folders:
        for path in (folder / "steps").glob(f"r*_{sched}_{load}_{barrier}_*.csv"):
            with open(path) as f:
                samples += [float(r["step_ms"]) for r in csv.DictReader(f)]
    return np.sort(samples)


def plot_cdf(folders, out):
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2), sharey=True, facecolor=SURFACE)
    for ax, barrier in zip(axes, BARRIERS):
        style(ax)
        ax.set_xscale("log")
        for s in SCHEDULERS:
            x = step_samples(folders, s, "stress", barrier)
            if len(x):
                ax.plot(x, np.arange(1, len(x) + 1) / len(x), color=COLOURS[s], linewidth=2, label=LABELS[s])
        ax.xaxis.set_major_locator(matplotlib.ticker.LogLocator(subs=(1, 2, 5)))
        ax.xaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
        ax.set_title(f"Step time distribution · stress · {barrier} barrier", color=INK, fontsize=11, loc="left")
        ax.set_xlabel("ms per step (log scale)", color=INK2, fontsize=9)
    axes[0].set_ylabel("fraction of steps at or below", color=INK2, fontsize=9)
    legend(fig)
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(out, dpi=150, facecolor=SURFACE)
    plt.close(fig)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    folders = [Path(p) for p in sys.argv[1:]]
    rows = load_rows(folders)
    runs = per_run(rows)
    out = folders[0]

    bgonly = defaultdict(list)
    for r in rows:
        if r["load"] == "bgonly" and num(r, "bg_bogo_per_s"):
            bgonly[r["scheduler"]].append(num(r, "bg_bogo_per_s"))
    baseline = st.mean(bgonly["eevdf"]) if bgonly["eevdf"] else None

    failed = [r for r in rows if r["status"] != "ok"]
    throttled = {r["throttled_after"] for r in rows} - {"0x0"}
    counts = [len(v) for v in runs.values()]

    md = [f"# Benchmark summary\n", f"Folders: {', '.join(str(f) for f in folders)}  ",
          f"Runs per condition: {min(counts)}–{max(counts)}. Values are mean ± standard deviation across repetitions "
          f"(oversub: averaged over the two simulation instances first).  ",
          f"Failed runs: {len(failed)}. Throttling flags seen: {', '.join(sorted(throttled)) or 'none'}.  ",
          f"Temperature range: {min(num(r, 'temp_before') for r in rows):.1f}–{max(num(r, 'temp_after') for r in rows):.1f} °C.\n"]
    md += ["## Mean step time (ms)\n", table(runs, "step_mean_ms"), "",
           "## p99 step time (ms)\n", table(runs, "step_p99_ms"), "",
           "## Barrier wait per thread per step (ms)\n", table(runs, "wait_per_step_ms", 3), "",
           "## Simulation throughput (steps per second, per simulation)\n", table(runs, "steps_per_s", 0), "",
           "## Migrations per second (sum over the 4 workers)\n", table(runs, "migrations_per_s", 1), "",
           "## Involuntary context switches per second (sum over the 4 workers)\n", table(runs, "involuntary_per_s", 0), "",
           "## Voluntary context switches per second (sum over the 4 workers)\n", table(runs, "voluntary_per_s", 0), ""]

    if baseline:
        md += ["## Background throughput (stress-ng, 4 hogs)\n",
               f"stress-ng alone under EEVDF: {baseline:.1f} bogo-ops/s = 100 %. "
               "Alone under the others: " + ", ".join(f"{LABELS[s]} {st.mean(v):.1f}" for s, v in bgonly.items() if s != "eevdf") + ".\n",
               "| barrier | " + " | ".join(LABELS[s] for s in SCHEDULERS) + " |", "|---|" + "---|" * len(SCHEDULERS)]
        for barrier in BARRIERS:
            cells = [fmt(*mean_sd([r["bg"] / baseline * 100 for r in runs.get((s, "stress", barrier), []) if r["bg"]]), 1) for s in SCHEDULERS]
            md.append(f"| {barrier} | " + " | ".join(c + (" %" if c != "–" else "") for c in cells) + " |")
        md.append("")

    md += ["## Speed-up of scx_gang over EEVDF (mean step time ratio)\n", "| load | barrier | mean | p99 |", "|---|---|---|---|"]
    for load in LOADS:
        for barrier in BARRIERS:
            e, g = runs.get(("eevdf", load, barrier)), runs.get(("gang", load, barrier))
            if e and g:
                ratio = lambda k: st.mean(r[k] for r in e) / st.mean(r[k] for r in g)
                md.append(f"| {load} | {barrier} | {ratio('step_mean_ms'):.2f}× | {ratio('step_p99_ms'):.2f}× |")
    md.append("")

    md += ["## Barrier wait per colour, stress load (µs per thread per crossing)\n"]
    for barrier in BARRIERS:
        md.append(f"**{barrier} barrier**\n")
        md.append("| scheduler | " + " | ".join(f"colour {c}" for c in range(7)) + " |")
        md.append("|---|" + "---|" * 7)
        for s in SCHEDULERS:
            rs = runs.get((s, "stress", barrier))
            if rs:
                md.append(f"| {LABELS[s]} | " + " | ".join(f"{v:.1f}" for v in np.mean([r["colour_us"] for r in rs], axis=0)) + " |")
        md.append("")

    if failed:
        md += ["## Failed runs\n"] + [f"- rep {r['rep']} {r['scheduler']} {r['load']} {r['barrier']} instance {r['instance']}: {r['status']}" for r in failed] + [""]

    md += ["## Plots\n", "![step time](step_time.png)\n", "![background](background.png)\n", "![step CDF](step_cdf.png)\n"]
    (out / "summary.md").write_text("\n".join(md))

    plot_step_time(runs, out / "step_time.png")
    if baseline:
        plot_background(runs, baseline, out / "background.png")
    plot_cdf(folders, out / "step_cdf.png")
    print(f"wrote {out / 'summary.md'} and plots")


if __name__ == "__main__":
    main()
