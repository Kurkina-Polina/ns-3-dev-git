#!/usr/bin/env python3
""" Run a series of ns-3 simulations, collect the results and plot them.

Example:
    python3 tools/run_sweep.py --widths 80 160 320 --repeats 5

Results are stored under results/<timestamp>/ together with the parameters of the sweep
and the commit hash, so that a plot can always be traced back to the code that produced it
"""

import argparse
import csv
import datetime
import json
import pathlib
import re
import subprocess
import sys

NS3_ROOT = pathlib.Path(__file__).resolve().parent.parent
SCENARIO = "punct-stand"
RESULT_RE = re.compile(r"^RESULT,(.+)$", re.MULTILINE)


def git_hash():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"], cwd=NS3_ROOT, text=True
        ).strip()
    except Exception:
        return "unknown"


def run_one(params):
    """Run one simulation and return the throughput in Mbit/s."""
    args = " ".join(f"--{k}={v}" for k, v in params.items())
    cmd = ["./ns3", "run", f"{SCENARIO} {args}"]
    proc = subprocess.run(cmd, cwd=NS3_ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"ERROR for {args}\n", file=sys.stderr)
        print(proc.stderr[-2000:], file=sys.stderr)
        return None
    match = RESULT_RE.search(proc.stdout)
    if not match:
        print(f"RESULT is not found for {args}\n", file=sys.stderr)
        return None
    return float(match.group(1).split(",")[-1])

def plot(rows, outdir):
    import statistics
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    widths = sorted({r["width"] for r in rows})
    fig, ax = plt.subplots(figsize=(8, 5))

    for width in widths:
        subset = [r for r in rows if r["width"] == width]
        subchannels = sorted({r["subch"] for r in subset})
        means, errors = [], []
        for subch in subchannels:
            values = [r["throughputMbps"] for r in subset if r["subch"] == subch]
            means.append(statistics.fmean(values))
            errors.append(statistics.stdev(values) if len(values) > 1 else 0.0)
        labels = ["without interference" if s < 0 else str(s) for s in subchannels]
        ax.errorbar(labels, means, yerr=errors, marker="o", capsize=4,
                    label=f"{width} МГц")

    ax.set_xlabel("Subchannel occupied by interference")
    ax.set_ylabel("Throughput, Mbit/s")
    ax.set_title("Impact of Narrowband Interference on Throughput")
    ax.grid(True, alpha=0.3)
    ax.legend()
    fig.tight_layout()
    path = outdir / "plot.png"
    fig.savefig(path, dpi=150)
    print(f"plot path: {path}")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--widths", type=int, nargs="+", default=[80, 160, 320])
    parser.add_argument("--repeats", type=int, default=5,
                        help="number of runs per point with different RngRun")
    parser.add_argument("--with-punct", action="store_true",
                        help="exclude the interfered subchannel")
    parser.add_argument("--tag", default="", help="tag for the folder name")
    args = parser.parse_args()

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    name = f"{stamp}-{args.tag}" if args.tag else stamp
    outdir = NS3_ROOT / "results" / name
    outdir.mkdir(parents=True, exist_ok=True)

    (outdir / "meta.json").write_text(
        json.dumps(
            {"args": vars(args), "git": git_hash(), "scenario": SCENARIO, "when": stamp},
            indent=2, ensure_ascii=False,
        ),
        encoding="utf-8",
    )

    rows = []
    for width in args.widths:
        n_sub = width // 20
        subchannels = [-1] + list(range(n_sub))
        for subch in subchannels:
            for run in range(1, args.repeats + 1):
                params = {
                    "width": width,
                    "subch": subch,
                    "rngRun": run,
                }

                if args.with_punct and (subch > 0):
                    params["punct"] = subch

                print(f"width={width} subch={subch} run={run} ... ", end="", flush=True)
                value = run_one(params)
                print("skip" if value is None else f"{value:.1f} Мбит/с")
                if value is not None:
                    rows.append(
                       {
                       "width": width,
                       "subch": subch,
                       "punct": params.get("punct", -1),
                       "rngRun": run,
                       "throughputMbps": value,
                      }
                    )


    csv_path = outdir / "results.csv"
    with csv_path.open("w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(
            fh, fieldnames=["width", "subch", "punct", "rngRun", "throughputMbps"]
        )
        writer.writeheader()
        writer.writerows(rows)
    print(f"\ncsv path: {csv_path}")

    plot(rows, outdir)

if __name__ == "__main__":
    main()
