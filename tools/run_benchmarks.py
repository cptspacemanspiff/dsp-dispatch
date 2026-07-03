#!/usr/bin/env python3
"""Run the dsp-dispatch per-backend benchmarks, collate their JSON output, and
emit human-readable tables plus a comparison graph.

Each backend has its own executable (build/bench/fft_bench_<backend> or
build/bench/fir_bench_<backend>). This script runs each one with JSON output,
parses Google Benchmark's results, and produces:

  - <out>/<suite>_<backend>.json  raw Google Benchmark output (one per backend)
  - <out>/<suite>_results.md      markdown tables
  - <out>/<suite>_results.csv     flat CSV of every data point
  - <out>/<suite>_latency.png     median latency graph
    For FIR, the image has one subplot per tap count.

Examples:
  tools/run_benchmarks.py
  tools/run_benchmarks.py --suite fir
  tools/run_benchmarks.py --build-dir build --filter execute --min-time 0.1s
  tools/run_benchmarks.py --no-run            # re-collate existing JSON only
"""
from __future__ import annotations

import argparse
import csv
import glob
import json
import os
import platform
import subprocess
import sys
from pathlib import Path


def cpu_model() -> str:
    """Best-effort CPU brand string.

    Google Benchmark's JSON context records num_cpus/mhz/host_name but not the
    CPU model, so we detect it here. run_benchmarks.py runs the executables
    locally, so the local CPU is the machine the numbers came from. Works on x86
    and Arm Linux (the CI runners) plus macOS.
    """
    # Linux: lscpu exposes "Model name" on both x86 and Arm.
    try:
        out = subprocess.run(["lscpu"], capture_output=True, text=True,
                             check=False).stdout
        for line in out.splitlines():
            if line.strip().lower().startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    # Linux fallback: x86 exposes "model name" in /proc/cpuinfo (Arm usually not).
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.lower().startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    # macOS.
    try:
        out = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                             capture_output=True, text=True, check=False).stdout.strip()
        if out:
            return out
    except OSError:
        pass
    return platform.processor() or platform.machine() or "unknown"


def discover_backends(bench_dir: Path, suite: str) -> list[tuple[str, Path]]:
    """Return [(backend_name, executable_path)] for each <suite>_bench_* binary."""
    found = []
    prefix = f"{suite}_bench_"
    for exe in sorted(glob.glob(str(bench_dir / f"{prefix}*"))):
        p = Path(exe)
        if p.is_file() and os.access(p, os.X_OK):
            found.append((p.name[len(prefix):], p))
    return found


def run_backend(name: str, exe: Path, out_dir: Path, args) -> Path:
    out_json = out_dir / f"{args.suite}_{name}.json"
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
    print(f"[run] {args.suite}/{name}: {' '.join(cmd)}", flush=True)
    subprocess.run(cmd, check=True)
    return out_json


def parse_bench_name(full_name: str) -> tuple[str, str | None]:
    """Extract operation and primary size from a Google Benchmark name."""
    base = full_name.split("/repeats:")[0]
    parts = base.split("/")
    op = parts[0]
    if "block" in parts:
        i = parts.index("block")
        if i + 1 < len(parts) and parts[i + 1].isdigit():
            block = parts[i + 1]
            if "taps" in parts:
                j = parts.index("taps")
                if j + 1 < len(parts) and parts[j + 1].isdigit():
                    return op, f"{block}x{parts[j + 1]}"
            return op, block
    length = next((p for p in reversed(parts) if p.isdigit()), None)
    return op, length


def load_results(json_path: Path) -> tuple[str, dict, dict]:
    """Return (backend_label, {(op, size): metrics}, context)."""
    data = json.loads(json_path.read_text())
    label = json_path.stem
    rows: dict[tuple[str, str], dict] = {}
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


def annotate_cpu(json_path: Path, cpu: str, arch: str) -> None:
    """Record the CPU model + arch into a benchmark JSON's ``context``.

    Google Benchmark's JSON context carries host/cpus/mhz but not the CPU brand
    string, so each raw result is otherwise anonymous. We inject it after the run
    (when the local machine *is* the one that produced the numbers), making every
    JSON self-describing — the dashboard and any downstream tooling read the CPU
    straight from the JSON instead of a side file.
    """
    try:
        data = json.loads(json_path.read_text())
    except (OSError, json.JSONDecodeError) as e:  # pragma: no cover
        print(f"[warn] could not annotate {json_path} with CPU: {e}",
              file=sys.stderr)
        return
    ctx = data.setdefault("context", {})
    ctx["cpu_model"] = cpu
    ctx["cpu_arch"] = arch
    json_path.write_text(json.dumps(data, indent=2))


def fmt_table(headers: list[str], rows: list[list[str]]) -> str:
    widths = [len(h) for h in headers]
    for r in rows:
        for i, c in enumerate(r):
            widths[i] = max(widths[i], len(c))
    line = lambda cells: "| " + " | ".join(c.ljust(widths[i]) for i, c in enumerate(cells)) + " |"
    sep = "|" + "|".join("-" * (w + 2) for w in widths) + "|"
    return "\n".join([line(headers), sep] + [line(r) for r in rows])


def size_sort_key(size: str) -> tuple[int, int, str]:
    parts = size.split("x", 1)
    if len(parts) == 2 and parts[0].isdigit() and parts[1].isdigit():
        return int(parts[0]), int(parts[1]), size
    if size.isdigit():
        return int(size), 0, size
    return 0, 0, size


def build_tables(per_backend: dict[str, dict], baseline: str | None) -> tuple[str, list[list]]:
    """Return (markdown, csv_rows). per_backend: backend -> {(op,size): metrics}."""
    backends = list(per_backend.keys())
    if baseline not in backends:
        baseline = backends[0] if backends else None

    sizes = sorted({size for rows in per_backend.values()
                    for (op, size) in rows if op == "execute"}, key=size_sort_key)

    md_parts: list[str] = []
    csv_rows: list[list] = [["backend", "op", "size", "metric", "value", "unit"]]

    # --- Execution latency (median us) ---
    headers = ["size"] + [f"{b} median (us)" for b in backends]
    speedup_backends = [b for b in backends if b != baseline] if baseline else []
    headers += [f"{b} vs {baseline}" for b in speedup_backends]
    table_rows = []
    for size in sizes:
        row = [str(size)]
        meds = {}
        for b in backends:
            m = per_backend[b].get(("execute", size), {})
            med = m.get("median")
            meds[b] = med
            row.append(f"{med:.3f}" if med is not None else "-")
            if med is not None:
                csv_rows.append([b, "execute", size, "median", f"{med:.6f}", m.get("unit", "us")])
                for k in ("p95", "p99", "items_per_second"):
                    if k in m:
                        csv_rows.append([b, "execute", size, k, f"{m[k]:.6f}",
                                         m.get("unit", "us") if k != "items_per_second" else "1/s"])
        if baseline and meds.get(baseline):
            base_v = meds[baseline]
            for b in speedup_backends:
                med = meds.get(b)
                if med:
                    speedup = base_v / med
                    row.append(f"{speedup:.2f}x")
                    csv_rows.append([b, "execute", size, f"speedup_vs_{baseline}",
                                     f"{speedup:.6f}", "x"])
                else:
                    row.append("-")
        table_rows.append(row)
    md_parts.append("### Execution latency (lower is better)\n\n" + fmt_table(headers, table_rows))

    # --- Throughput (transforms/sec) ---
    headers = ["size"] + [f"{b} items/s" for b in backends]
    table_rows = []
    for size in sizes:
        row = [str(size)]
        for b in backends:
            m = per_backend[b].get(("execute", size), {})
            tps = m.get("items_per_second")
            row.append(f"{tps:,.0f}" if tps else "-")
        table_rows.append(row)
    md_parts.append("### Throughput (items/sec, higher is better)\n\n" + fmt_table(headers, table_rows))

    # --- Plan creation (us) ---
    plan_sizes = sorted({size for rows in per_backend.values()
                         for (op, size) in rows if op == "plan_create"}, key=size_sort_key)
    if plan_sizes:
        headers = ["size"] + [f"{b} plan (us)" for b in backends]
        table_rows = []
        for size in plan_sizes:
            row = [str(size)]
            for b in backends:
                m = per_backend[b].get(("plan_create", size), {})
                t = m.get("time")
                row.append(f"{t:.3f}" if t is not None else "-")
                if t is not None:
                    csv_rows.append([b, "plan_create", size, "time", f"{t:.6f}", m.get("unit", "us")])
            table_rows.append(row)
        md_parts.append("### Plan creation (one-time, us)\n\n" + fmt_table(headers, table_rows))

    return "\n\n".join(md_parts), csv_rows


def split_fir_size(size: str) -> tuple[int, int] | None:
    parts = size.split("x", 1)
    if len(parts) != 2 or not parts[0].isdigit() or not parts[1].isdigit():
        return None
    return int(parts[0]), int(parts[1])


def setup_matplotlib(out_dir: Path):
    try:
        os.environ.setdefault("MPLCONFIGDIR", str(out_dir / ".matplotlib"))
        Path(os.environ["MPLCONFIGDIR"]).mkdir(parents=True, exist_ok=True)
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as e:  # pragma: no cover
        print(f"[graph] skipped (matplotlib unavailable: {e})", file=sys.stderr)
        return None
    return plt


def make_fft_graph(per_backend: dict[str, dict], png_path: Path, plt) -> bool:
    fig, (ax_lat, ax_tps) = plt.subplots(1, 2, figsize=(12, 5))
    plotted = False
    for b, rows in per_backend.items():
        pts = sorted((int(n), m) for (op, n), m in rows.items()
                     if op == "execute" and str(n).isdigit())
        if not pts:
            continue
        plotted = True
        ns = [n for n, _ in pts]
        med = [m.get("median") for _, m in pts]
        tps = [m.get("items_per_second") for _, m in pts]
        ax_lat.plot(ns, med, marker="o", label=b)
        if all(t is not None for t in tps):
            ax_tps.plot(ns, tps, marker="o", label=b)

    if not plotted:
        print(f"[graph] skipped (no numeric x-axis values)")
        plt.close(fig)
        return False

    ax_lat.set(xscale="log", yscale="log", xlabel="N (transform length)",
               ylabel="median latency (us)", title="FFT execution latency")
    ax_lat.grid(True, which="both", alpha=0.3)
    ax_lat.legend()

    ax_tps.set(xscale="log", yscale="log", xlabel="N (transform length)",
               ylabel="transforms / sec", title="FFT throughput")
    ax_tps.grid(True, which="both", alpha=0.3)
    ax_tps.legend()

    fig.tight_layout()
    fig.savefig(png_path, dpi=120)
    print(f"[graph] wrote {png_path}")
    return True


def make_fir_graphs(per_backend: dict[str, dict], out_dir: Path, plt) -> list[Path]:
    taps_values = sorted({taps for rows in per_backend.values()
                          for (op, size) in rows
                          for parsed in [split_fir_size(str(size))]
                          if op == "execute" and parsed
                          for _, taps in [parsed]})
    if not taps_values:
        print("[graph] skipped (no FIR block/tap data)")
        return []

    cols = min(3, len(taps_values))
    grid_rows = (len(taps_values) + cols - 1) // cols
    fig, axes = plt.subplots(grid_rows, cols, figsize=(5 * cols, 3.8 * grid_rows), squeeze=False)
    plotted_any = False

    for idx, taps in enumerate(taps_values):
        ax = axes[idx // cols][idx % cols]
        for backend, backend_rows in per_backend.items():
            pts: list[tuple[int, dict]] = []
            for (op, size), metrics in backend_rows.items():
                parsed = split_fir_size(str(size))
                if op == "execute" and parsed and parsed[1] == taps:
                    pts.append((parsed[0], metrics))
            if not pts:
                continue
            pts.sort()
            plotted_any = True
            blocks = [b for b, _ in pts]
            med = [m.get("median") for _, m in pts]
            ax.plot(blocks, med, marker="o", label=backend)

        ax.set_xscale("log", base=2)
        ax.set_yscale("log")
        ax.set_xlabel("block size")
        ax.set_ylabel("median latency (us)")
        ax.set_title(f"taps={taps}")
        ax.grid(True, which="both", alpha=0.3)
        ax.legend()

    for idx in range(len(taps_values), grid_rows * cols):
        axes[idx // cols][idx % cols].axis("off")

    if not plotted_any:
        plt.close(fig)
        print("[graph] skipped (no FIR block/tap data)")
        return []

    fig.suptitle("FIR latency by tap count", y=1.02)
    fig.tight_layout()
    png_path = out_dir / "fir_latency.png"
    fig.savefig(png_path, dpi=120, bbox_inches="tight")
    plt.close(fig)
    print(f"[graph] wrote {png_path}")
    return [png_path]


def make_graphs(per_backend: dict[str, dict], out_dir: Path, suite: str) -> list[Path]:
    plt = setup_matplotlib(out_dir)
    if plt is None:
        return []
    if suite == "fir":
        return make_fir_graphs(per_backend, out_dir, plt)

    png_path = out_dir / f"{suite}_latency.png"
    return [png_path] if make_fft_graph(per_backend, png_path, plt) else []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build-dir", default="build", type=Path,
                    help="CMake build directory (default: build)")
    ap.add_argument("--suite", choices=("fft", "fir"), default="fft",
                    help="benchmark suite to run/discover (default: fft)")
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
        json_files = sorted(args.out_dir.glob(f"{args.suite}_*.json"))
        if args.backends:
            wanted = {f"{args.suite}_{b}" for b in args.backends}
            json_files = [p for p in json_files if p.stem in wanted]
        if not json_files:
            print(f"error: no JSON files in {args.out_dir}", file=sys.stderr)
            return 1
    else:
        backends = discover_backends(bench_dir, args.suite)
        if args.backends:
            backends = [(n, e) for (n, e) in backends if n in args.backends]
        if not backends:
            print(f"error: no {args.suite}_bench_* executables in {bench_dir}. "
                  f"Build with -DFFT_ENABLE_BENCHMARKS=ON first.", file=sys.stderr)
            return 1
        print(f"[info] backends: {', '.join(n for n, _ in backends)}")
        json_files = [run_backend(n, e, args.out_dir, args) for n, e in backends]
        # Stamp the CPU into each freshly-produced JSON. Skipped under --no-run,
        # where the local CPU may not be the one that produced the results.
        run_cpu, run_arch = cpu_model(), platform.machine()
        for jf in json_files:
            annotate_cpu(jf, run_cpu, run_arch)

    per_backend: dict[str, dict] = {}
    context = {}
    for jf in json_files:
        label, rows, ctx = load_results(jf)
        per_backend[label] = rows
        context = context or ctx

    md, csv_rows = build_tables(per_backend, args.baseline)

    # Prefer the CPU recorded in the JSON (from a prior run) so --no-run reports
    # the machine that actually produced the numbers; fall back to the local CPU.
    cpu = context.get("cpu_model") or cpu_model()
    arch = context.get("cpu_arch") or platform.machine()

    # Human-readable to stdout.
    print()
    print(f"# cpu={cpu} ({arch})")
    if context:
        mhz = context.get("mhz_per_cpu", "?")
        ncpu = context.get("num_cpus", "?")
        print(f"# host={context.get('host_name','?')}  cpus={ncpu}  mhz={mhz}")
    print(md)
    print()

    # Markdown file.
    header = "# dsp-dispatch benchmark results\n\n"
    header += f"- cpu: `{cpu}` ({arch})\n"
    if context:
        header += (f"- host: `{context.get('host_name','?')}`  "
                   f"cpus: {context.get('num_cpus','?')}  "
                   f"mhz: {context.get('mhz_per_cpu','?')}\n"
                   f"- build: {context.get('library_build_type','?')}\n")
    header += "\n"
    results_md = args.out_dir / f"{args.suite}_results.md"
    results_md.write_text(header + md + "\n")
    print(f"[write] {results_md}")

    # CSV file.
    results_csv = args.out_dir / f"{args.suite}_results.csv"
    with results_csv.open("w", newline="") as f:
        csv.writer(f).writerows(csv_rows)
    print(f"[write] {results_csv}")

    make_graphs(per_backend, args.out_dir, args.suite)
    return 0


if __name__ == "__main__":
    sys.exit(main())
