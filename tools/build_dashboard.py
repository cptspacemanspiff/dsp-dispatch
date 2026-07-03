#!/usr/bin/env python3
"""Collate per-backend Google Benchmark JSON into a single data file that drives
the interactive benchmark dashboard on the docs site.

It reuses the JSON parsing from ``run_benchmarks.py`` (``load_results`` and the
name/size helpers) so the dashboard sees exactly the same numbers as the
markdown tables and matplotlib charts.

Input is one directory per architecture, each holding the collated
``<suite>_<backend>.json`` files (and optionally the ``<suite>_results.md`` the
runner wrote, which is parsed only for the CPU model string). Output is a small
JavaScript file assigning ``window.BENCH_DATA`` — shipped as JS rather than JSON
so the docs page can load it with a plain ``<script>`` tag, independent of
MkDocs' directory-URL setting and without a fetch() (works offline too).

Example (matches the CI publish job):

    tools/build_dashboard.py \\
        --arch x86:x86_64:results/x86 \\
        --arch arm:Arm64:results/arm \\
        --commit "$GITHUB_SHA" --out docs/assets/bench-data.js
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# Reuse the exact collation logic the markdown/CSV/PNG outputs use.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_benchmarks import load_results, size_sort_key, split_fir_size  # noqa: E402

# Normalize whatever time unit Google Benchmark reports into microseconds so the
# dashboard axis label ("median latency (µs)") is always correct.
_TO_US = {"ns": 1e-3, "us": 1.0, "ms": 1e3, "s": 1e6}


def _us(value: float | None, unit: str) -> float | None:
    if value is None:
        return None
    return value * _TO_US.get(unit, 1.0)


def cpu_from_results_md(arch_dir: Path, suite: str) -> str | None:
    """Best-effort CPU model, parsed from the runner's ``<suite>_results.md``.

    Google Benchmark's JSON context carries host/cpus/mhz but not the CPU brand
    string; run_benchmarks.py detects it and writes it into the markdown header
    as ``- cpu: `<model>` (<arch>)``. We recover it from there when present.
    """
    # Prefer this suite's own markdown, then fall back to any results.md in the
    # dir (both suites run on the same machine, so the CPU string is identical).
    candidates = [arch_dir / f"{suite}_results.md",
                  *sorted(arch_dir.glob("*_results.md"))]
    for md in candidates:
        if not md.is_file():
            continue
        m = re.search(r"^- cpu:\s*`([^`]+)`", md.read_text(), re.MULTILINE)
        if m:
            return m.group(1).strip()
    return None


def collate_arch(arch_dir: Path, suite: str) -> dict | None:
    """Build the per-arch payload for one suite, or None if it has no data."""
    json_files = sorted(arch_dir.glob(f"{suite}_*.json"))
    if not json_files:
        return None

    backends: dict[str, dict] = {}
    context: dict = {}
    for jf in json_files:
        label, rows, ctx = load_results(jf)
        context = context or ctx

        execute: list[dict] = []
        plan_create: list[dict] = []
        for (op, size), m in rows.items():
            unit = m.get("unit", "us")
            if op == "execute":
                point = {"size": str(size)}
                if suite == "fir":
                    parsed = split_fir_size(str(size))
                    if not parsed:
                        continue
                    point["block"], point["taps"] = parsed
                    point["x"] = parsed[0]
                else:
                    if not str(size).isdigit():
                        continue
                    point["x"] = int(size)
                point["median_us"] = _us(m.get("median"), unit)
                point["p95_us"] = _us(m.get("p95"), unit)
                point["p99_us"] = _us(m.get("p99"), unit)
                point["throughput"] = m.get("items_per_second")
                execute.append(point)
            elif op == "plan_create" and str(size).isdigit():
                plan_create.append({
                    "size": str(size), "x": int(size),
                    "time_us": _us(m.get("time"), unit),
                })

        if not execute and not plan_create:
            continue
        execute.sort(key=lambda p: (p["x"], p.get("taps", 0)))
        plan_create.sort(key=lambda p: p["x"])
        backends[label] = {"execute": execute, "plan_create": plan_create}

    if not backends:
        return None

    payload = {
        # Prefer the CPU recorded in the JSON (run_benchmarks.py stamps it into
        # context.cpu_model); fall back to scraping the markdown for older runs.
        "cpu": context.get("cpu_model") or cpu_from_results_md(arch_dir, suite),
        "arch": context.get("cpu_arch"),
        "host": context.get("host_name"),
        "num_cpus": context.get("num_cpus"),
        "mhz": context.get("mhz_per_cpu"),
        "build": context.get("library_build_type"),
        "backends": backends,
    }
    if suite == "fir":
        taps = sorted({p["taps"] for b in backends.values()
                       for p in b["execute"] if "taps" in p})
        payload["taps"] = taps
    return payload


def parse_arch_arg(spec: str) -> tuple[str, str, Path]:
    """Parse ``key:label:dir`` (label defaults to key if only key:dir given)."""
    parts = spec.split(":")
    if len(parts) == 3:
        key, label, path = parts
    elif len(parts) == 2:
        key, path = parts
        label = key
    else:
        raise argparse.ArgumentTypeError(
            f"--arch must be key:label:dir or key:dir (got {spec!r})")
    return key, label, Path(path)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--arch", action="append", type=parse_arch_arg, default=[],
                    metavar="KEY:LABEL:DIR",
                    help="architecture results dir, e.g. x86:x86_64:results/x86 "
                         "(repeatable)")
    ap.add_argument("--suite", action="append", choices=("fft", "fir"),
                    help="suites to include (default: fft and fir)")
    ap.add_argument("--commit", default="", help="commit SHA for the header")
    ap.add_argument("--generated", default="",
                    help="ISO timestamp for the header (optional)")
    ap.add_argument("--out", type=Path, default=Path("docs/assets/bench-data.js"),
                    help="output JS file (default: docs/assets/bench-data.js)")
    args = ap.parse_args()

    if not args.arch:
        ap.error("at least one --arch is required")
    suites = args.suite or ["fft", "fir"]

    suites_out: dict[str, dict] = {}
    for suite in suites:
        archs: dict[str, dict] = {}
        arch_labels: dict[str, str] = {}
        for key, label, path in args.arch:
            if not path.is_dir():
                print(f"[warn] {path} is not a directory; skipping "
                      f"{suite}/{key}", file=sys.stderr)
                continue
            payload = collate_arch(path, suite)
            if payload is None:
                print(f"[info] no {suite} data in {path}", file=sys.stderr)
                continue
            archs[key] = payload
            arch_labels[key] = label
        if archs:
            suites_out[suite] = {
                "label": suite.upper(),
                "arch_labels": arch_labels,
                "archs": archs,
            }

    data = {
        "commit": args.commit,
        "commit_short": args.commit[:9] if args.commit else "",
        "generated": args.generated,
        "suites": suites_out,
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    body = json.dumps(data, indent=1, sort_keys=True)
    args.out.write_text(
        "/* Generated by tools/build_dashboard.py — do not edit by hand. */\n"
        f"window.BENCH_DATA = {body};\n")

    n = sum(len(s["archs"]) for s in suites_out.values())
    print(f"[write] {args.out} ({len(suites_out)} suite(s), {n} suite×arch block(s))")
    if not suites_out:
        print("[warn] no benchmark data found for any suite/arch", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
