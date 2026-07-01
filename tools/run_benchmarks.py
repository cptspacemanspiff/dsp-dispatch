#!/usr/bin/env python3
"""Run the dsp-dispatch per-backend benchmarks, collate their JSON output, and
emit human-readable tables plus a comparison graph.

Each backend has its own executable (build/bench/fft_bench_<backend>). This
script runs each one with JSON output, parses Google Benchmark's results, and
produces:

  - <out>/<backend>.json   raw Google Benchmark output (one per backend)
  - <out>/results.md       markdown tables (execution latency, throughput, plan)
  - <out>/results.csv      flat CSV of every (backend, length) data point
  - <out>/latency.png      median latency + throughput vs length (if matplotlib)

Examples:
  tools/run_benchmarks.py
  tools/run_benchmarks.py --build-dir build --filter execute --min-time 0.1s
  tools/run_benchmarks.py --no-run            # re-collate existing JSON only
"""
from __future__ import annotations

import argparse
import csv
import glob
import json
import os
import subprocess
import sys
from pathlib import Path


def discover_backends(bench_dir: Path) -> list[tuple[str, Path]]:
    """Return [(backend_name, executable_path)] for each fft_bench_* binary."""
    found = []
    for exe in sorted(glob.glob(str(bench_dir / "fft_bench_*"))):
        p = Path(exe)
        if p.is_file() and os.access(p, os.X_OK):
            found.append((p.name[len("fft_bench_"):], p))
    return found


def run_backend(name: str, exe: Path, out_dir: Path, args) -> Path:
    out_json = out_dir / f"{name}.json"
    cmd = [
        str(exe),
        f"--benchmark_out={out_json}",
        "--benchmark_out_format=json",
        f"--benchmark_min_time={args.min_time}",
    ]
    if args.filter:
        cmd.append(f"--benchmark_filter={args.filter}")
    if args.repetitions:
        cmd.append(f"--benchmark_repetitions={args.repetitions}")
    print(f"[run] {name}: {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, check=True)
    return out_json


def parse_bench_name(full_name: str) -> tuple[str, int | None]:
    """'execute/c2c/f32/1024/repeats:15' -> ('execute', 1024)."""
    base = full_name.split("/repeats:")[0]
    parts = base.split("/")
    op = parts[0]
    length = next((int(p) for p in reversed(parts) if p.isdigit()), None)
    return op, length


def load_results(json_path: Path) -> tuple[str, dict, dict]:
    """Return (backend_label, {(op, length): metrics}, context)."""
    data = json.loads(json_path.read_text())
    label = json_path.stem
    rows: dict[tuple[str, int], dict] = {}
    for b in data.get("benchmarks", []):
        op, length = parse_bench_name(b["name"])
        if length is None:
            continue
        label = b.get("label") or label
        key = (op, length)
        agg = b.get("aggregate_name", "")
        if op == "plan_create":
            # No repetitions: a single iteration row carries the time.
            if b.get("run_type") == "iteration":
                rows.setdefault(key, {})["time"] = b["real_time"]
                rows[key]["unit"] = b.get("time_unit", "us")
        elif agg in ("median", "p95", "p99", "mean"):
            d = rows.setdefault(key, {})
            d[agg] = b["real_time"]
            d["unit"] = b.get("time_unit", "us")
            if "items_per_second" in b:
                d["items_per_second"] = b["items_per_second"]
    return label, rows, data.get("context", {})


def fmt_table(headers: list[str], rows: list[list[str]]) -> str:
    widths = [len(h) for h in headers]
    for r in rows:
        for i, c in enumerate(r):
            widths[i] = max(widths[i], len(c))
    line = lambda cells: "| " + " | ".join(c.ljust(widths[i]) for i, c in enumerate(cells)) + " |"
    sep = "|" + "|".join("-" * (w + 2) for w in widths) + "|"
    return "\n".join([line(headers), sep] + [line(r) for r in rows])


def build_tables(per_backend: dict[str, dict], baseline: str | None) -> tuple[str, list[list]]:
    """Return (markdown, csv_rows). per_backend: backend -> {(op,length): metrics}."""
    backends = list(per_backend.keys())
    if baseline not in backends:
        baseline = backends[0] if backends else None

    lengths = sorted({length for rows in per_backend.values()
                      for (op, length) in rows if op == "execute"})

    md_parts: list[str] = []
    csv_rows: list[list] = [["backend", "op", "length", "metric", "value", "unit"]]

    # --- Execution latency (median us) ---
    headers = ["N"] + [f"{b} median (us)" for b in backends]
    if baseline:
        headers += [f"speedup vs {baseline}"]
    table_rows = []
    for n in lengths:
        row = [str(n)]
        meds = {}
        for b in backends:
            m = per_backend[b].get(("execute", n), {})
            med = m.get("median")
            meds[b] = med
            row.append(f"{med:.3f}" if med is not None else "-")
            if med is not None:
                csv_rows.append([b, "execute", n, "median", f"{med:.6f}", m.get("unit", "us")])
                for k in ("p95", "p99", "items_per_second"):
                    if k in m:
                        csv_rows.append([b, "execute", n, k, f"{m[k]:.6f}",
                                         m.get("unit", "us") if k != "items_per_second" else "1/s"])
        if baseline and meds.get(baseline):
            best = min((v for v in meds.values() if v), default=None)
            base_v = meds[baseline]
            # speedup of the fastest backend relative to baseline
            row.append(f"{base_v / best:.2f}x" if best else "-")
        table_rows.append(row)
    md_parts.append("### Execution latency (lower is better)\n\n" + fmt_table(headers, table_rows))

    # --- Throughput (transforms/sec) ---
    headers = ["N"] + [f"{b} xforms/s" for b in backends]
    table_rows = []
    for n in lengths:
        row = [str(n)]
        for b in backends:
            m = per_backend[b].get(("execute", n), {})
            tps = m.get("items_per_second")
            row.append(f"{tps:,.0f}" if tps else "-")
        table_rows.append(row)
    md_parts.append("### Throughput (transforms/sec, higher is better)\n\n" + fmt_table(headers, table_rows))

    # --- Plan creation (us) ---
    plan_lengths = sorted({length for rows in per_backend.values()
                           for (op, length) in rows if op == "plan_create"})
    if plan_lengths:
        headers = ["N"] + [f"{b} plan (us)" for b in backends]
        table_rows = []
        for n in plan_lengths:
            row = [str(n)]
            for b in backends:
                m = per_backend[b].get(("plan_create", n), {})
                t = m.get("time")
                row.append(f"{t:.3f}" if t is not None else "-")
                if t is not None:
                    csv_rows.append([b, "plan_create", n, "time", f"{t:.6f}", m.get("unit", "us")])
            table_rows.append(row)
        md_parts.append("### Plan creation (one-time, us)\n\n" + fmt_table(headers, table_rows))

    return "\n\n".join(md_parts), csv_rows


def make_graph(per_backend: dict[str, dict], png_path: Path) -> bool:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:  # pragma: no cover
        print(f"[graph] skipped (matplotlib unavailable: {e})", file=sys.stderr)
        return False

    fig, (ax_lat, ax_tps) = plt.subplots(1, 2, figsize=(12, 5))
    for b, rows in per_backend.items():
        pts = sorted((n, m) for (op, n), m in rows.items() if op == "execute")
        if not pts:
            continue
        ns = [n for n, _ in pts]
        med = [m.get("median") for _, m in pts]
        tps = [m.get("items_per_second") for _, m in pts]
        ax_lat.plot(ns, med, marker="o", label=b)
        if all(t is not None for t in tps):
            ax_tps.plot(ns, tps, marker="o", label=b)

    ax_lat.set(xscale="log", yscale="log", xlabel="N (transform length)",
               ylabel="median latency (us)", title="Execution latency vs N")
    ax_lat.grid(True, which="both", alpha=0.3)
    ax_lat.legend()

    ax_tps.set(xscale="log", yscale="log", xlabel="N (transform length)",
               ylabel="transforms / sec", title="Throughput vs N")
    ax_tps.grid(True, which="both", alpha=0.3)
    ax_tps.legend()

    fig.tight_layout()
    fig.savefig(png_path, dpi=120)
    print(f"[graph] wrote {png_path}")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="build", type=Path,
                    help="CMake build directory (default: build)")
    ap.add_argument("--out-dir", default="bench_results", type=Path,
                    help="output directory for JSON/tables/graph (default: bench_results)")
    ap.add_argument("--backends", nargs="*",
                    help="restrict to these backend names (default: all discovered)")
    ap.add_argument("--baseline", default="portable",
                    help="backend to compute speedups against (default: portable)")
    ap.add_argument("--filter", default=None, help="--benchmark_filter passthrough")
    ap.add_argument("--min-time", default="0.1s", help="--benchmark_min_time (default: 0.1s)")
    ap.add_argument("--repetitions", type=int, default=None,
                    help="--benchmark_repetitions override")
    ap.add_argument("--no-run", action="store_true",
                    help="do not run benchmarks; re-collate existing JSON in --out-dir")
    args = ap.parse_args()

    bench_dir = args.build_dir / "bench"
    args.out_dir.mkdir(parents=True, exist_ok=True)

    if args.no_run:
        json_files = sorted(args.out_dir.glob("*.json"))
        if args.backends:
            json_files = [p for p in json_files if p.stem in args.backends]
        if not json_files:
            print(f"error: no JSON files in {args.out_dir}", file=sys.stderr)
            return 1
    else:
        backends = discover_backends(bench_dir)
        if args.backends:
            backends = [(n, e) for (n, e) in backends if n in args.backends]
        if not backends:
            print(f"error: no fft_bench_* executables in {bench_dir}. "
                  f"Build with -DFFT_ENABLE_BENCHMARKS=ON first.", file=sys.stderr)
            return 1
        print(f"[info] backends: {', '.join(n for n, _ in backends)}")
        json_files = [run_backend(n, e, args.out_dir, args) for n, e in backends]

    per_backend: dict[str, dict] = {}
    context = {}
    for jf in json_files:
        label, rows, ctx = load_results(jf)
        per_backend[label] = rows
        context = context or ctx

    md, csv_rows = build_tables(per_backend, args.baseline)

    # Human-readable to stdout.
    print()
    if context:
        cpu = context.get("host_name", "?")
        mhz = context.get("mhz_per_cpu", "?")
        ncpu = context.get("num_cpus", "?")
        print(f"# host={cpu}  cpus={ncpu}  mhz={mhz}")
    print(md)
    print()

    # Markdown file.
    header = "# dsp-dispatch benchmark results\n\n"
    if context:
        header += (f"- host: `{context.get('host_name','?')}`  "
                   f"cpus: {context.get('num_cpus','?')}  "
                   f"mhz: {context.get('mhz_per_cpu','?')}\n"
                   f"- build: {context.get('library_build_type','?')}\n\n")
    (args.out_dir / "results.md").write_text(header + md + "\n")
    print(f"[write] {args.out_dir / 'results.md'}")

    # CSV file.
    with (args.out_dir / "results.csv").open("w", newline="") as f:
        csv.writer(f).writerows(csv_rows)
    print(f"[write] {args.out_dir / 'results.csv'}")

    make_graph(per_backend, args.out_dir / "latency.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
