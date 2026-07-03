# Benchmarks

Interactive results from the CI benchmark suite. Every backend is measured by
its **own executable** built from one backend-agnostic harness (same buffers,
same normalization, same statistics), so the comparison is apples-to-apples.
Latency is the **median** over 15 repetitions.

!!! warning "Smoke signal, not regression-grade"
    These numbers come from **shared GitHub runners** of unknown CPU vendor and
    variable load. They confirm each backend compiles and runs correctly across
    a range of sizes — they are **not** reliable relative-performance numbers.
    Run `tools/run_benchmarks.py` on dedicated hardware for that.

<div class="bench-dashboard" id="bench-dashboard" data-echarts-min-height="460">
  <noscript>
    <p class="bench-empty">This dashboard needs JavaScript. The raw numbers are
    also uploaded as CSV/JSON artifacts on every CI run.</p>
  </noscript>
  <div class="bench-toolbar">
    <div class="bench-group" data-role="suite" role="tablist" aria-label="Suite"></div>
    <div class="bench-group" data-role="arch" aria-label="Architecture"></div>
    <div class="bench-group" data-role="metric" aria-label="Metric"></div>
    <div class="bench-group bench-group--taps" data-role="taps" aria-label="Taps" hidden></div>
  </div>
  <div class="bench-meta" data-role="meta"></div>
  <div class="bench-chart" data-role="chart"></div>
  <p class="bench-empty" data-role="empty" hidden>
    No benchmark data has been published yet — it appears after the first CI run
    on <code>main</code> completes.
  </p>
  <p class="bench-footnote" data-role="footnote"></p>
</div>

## Reproducing locally

`tools/run_benchmarks.py` runs every `*_bench_*` executable, collates their JSON,
and writes tables, a CSV, and matplotlib charts:

```sh
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFT_ENABLE_ALL_BENCHMARKS=ON -S . -B build-bench
cmake --build build-bench
tools/run_benchmarks.py --build-dir build-bench            # FFT
tools/run_benchmarks.py --build-dir build-bench --suite fir  # FIR
# -> bench_results/{<suite>_<backend>.json, <suite>_results.md, .csv, _latency.png}
```

The interactive dashboard above is generated from those same JSON files by
`tools/build_dashboard.py`.

!!! tip "Fair comparison (ISA)"
    KFR defaults to a conservative `sse2` baseline. Raise it (and match the
    portable side) for a meaningful comparison:
    `cmake -DKFR_ARCH=avx2 build-bench`.
