/* Interactive benchmark dashboard for dsp-dispatch.
 *
 * Reads window.BENCH_DATA (emitted by tools/build_dashboard.py) and renders an
 * ECharts line chart with suite / architecture / metric / taps controls. Kept
 * dependency-free apart from ECharts (loaded via extra_javascript). Designed to
 * cooperate with MkDocs Material instant navigation (re-inits on document$).
 */
(function () {
  "use strict";

  // Stable, reasonably colorblind-friendly categorical palette. Backends are
  // assigned colors by sorted name so a backend keeps its color across views.
  var PALETTE = [
    "#4e79a7", "#f28e2b", "#59a14f", "#e15759", "#b07aa1",
    "#76b7b2", "#edc948", "#ff9da7", "#9c755f", "#bab0ac"
  ];

  var METRICS = {
    latency: { key: "median_us", label: "Latency (median)", unit: "µs", better: "lower" },
    throughput: { key: "throughput", label: "Throughput", unit: "items/s", better: "higher" }
  };

  var chart = null;
  var resizeHandler = null;
  var themeObserver = null;

  function el(root, role) {
    return root.querySelector('[data-role="' + role + '"]');
  }

  function cssVar(name, fallback) {
    var v = getComputedStyle(document.body).getPropertyValue(name);
    return (v && v.trim()) || fallback;
  }

  function colorFor(backends, name) {
    var i = backends.indexOf(name);
    return PALETTE[(i < 0 ? 0 : i) % PALETTE.length];
  }

  function fmtSI(v) {
    if (v == null || !isFinite(v)) return "—";
    var abs = Math.abs(v), u = "", d = v;
    if (abs >= 1e9) { d = v / 1e9; u = "G"; }
    else if (abs >= 1e6) { d = v / 1e6; u = "M"; }
    else if (abs >= 1e3) { d = v / 1e3; u = "k"; }
    return (Math.abs(d) >= 100 ? d.toFixed(0) : d.toFixed(2)) + u;
  }

  function fmtNum(v) {
    if (v == null || !isFinite(v)) return "—";
    if (v >= 100) return v.toFixed(1);
    if (v >= 1) return v.toFixed(3);
    return v.toPrecision(3);
  }

  // Build [x, y] pairs for one backend under the current suite/metric/taps.
  function seriesData(points, metricKey, suite, taps) {
    var out = [];
    for (var i = 0; i < points.length; i++) {
      var p = points[i];
      if (suite === "fir" && taps != null && p.taps !== taps) continue;
      var y = p[metricKey];
      if (y == null || !isFinite(y) || y <= 0) continue;
      out.push([p.x, y, p.size]);
    }
    out.sort(function (a, b) { return a[0] - b[0]; });
    return out;
  }

  function makeButtons(container, items, current, onPick) {
    container.innerHTML = "";
    var buttons = [];
    items.forEach(function (item) {
      var b = document.createElement("button");
      b.type = "button";
      b.className = "bench-btn" + (item.value === current ? " is-active" : "");
      b.textContent = item.label;
      b.setAttribute("aria-pressed", item.value === current ? "true" : "false");
      b.addEventListener("click", function () {
        // Update the active highlight immediately, independent of whether the
        // callback rebuilds this group (metric/taps only re-render the chart).
        buttons.forEach(function (other) {
          var active = other === b;
          other.classList.toggle("is-active", active);
          other.setAttribute("aria-pressed", active ? "true" : "false");
        });
        onPick(item.value);
      });
      buttons.push(b);
      container.appendChild(b);
    });
    container.hidden = items.length === 0;
  }

  function metaLine(root, suiteData, archKey) {
    var meta = el(root, "meta");
    var a = suiteData.archs[archKey] || {};
    var bits = [];
    if (a.cpu) bits.push("<strong>CPU:</strong> " + a.cpu);
    if (a.host) bits.push("<strong>host:</strong> " + a.host);
    if (a.num_cpus) bits.push(a.num_cpus + " CPUs");
    if (a.build) bits.push("build: " + a.build);
    meta.innerHTML = bits.join(" · ");
  }

  function footnote(root, data) {
    var f = el(root, "footnote");
    if (!f) return;
    var parts = ["Shared-runner smoke numbers — not regression-grade."];
    if (data.commit_short) {
      parts.push('commit <code>' + data.commit_short + "</code>");
    }
    if (data.generated) parts.push(data.generated);
    f.innerHTML = parts.join(" · ");
  }

  function render(root, state) {
    var data = window.BENCH_DATA;
    var suiteData = data.suites[state.suite];
    var arch = suiteData.archs[state.arch];
    var metric = METRICS[state.metric];
    var backends = Object.keys(arch.backends).sort();

    metaLine(root, suiteData, state.arch);

    var series = backends.map(function (name) {
      return {
        name: name,
        type: "line",
        smooth: false,
        symbol: "circle",
        symbolSize: 7,
        lineStyle: { width: 2 },
        itemStyle: { color: colorFor(backends, name) },
        emphasis: { focus: "series" },
        data: seriesData(arch.backends[name].execute, metric.key, state.suite, state.taps)
      };
    }).filter(function (s) { return s.data.length > 0; });

    var fg = cssVar("--md-default-fg-color", "#000");
    var faint = cssVar("--md-default-fg-color--light", "rgba(0,0,0,.5)");
    var split = cssVar("--md-default-fg-color--lightest", "rgba(0,0,0,.1)");
    var xName = state.suite === "fir" ? "block size (samples)" : "N (transform length)";
    var yName = metric.label + " (" + metric.unit + ") — " +
                (metric.better === "lower" ? "lower is better" : "higher is better");

    var option = {
      color: series.map(function (s) { return s.itemStyle.color; }),
      textStyle: { color: fg, fontFamily: cssVar("--md-text-font-family", "sans-serif") },
      grid: { left: 8, right: 24, top: 48, bottom: 8, containLabel: true },
      legend: {
        type: "scroll", top: 8, textStyle: { color: fg },
        inactiveColor: faint
      },
      tooltip: {
        trigger: "axis", axisPointer: { type: "cross" },
        backgroundColor: cssVar("--md-default-bg-color", "#fff"),
        borderColor: split,
        textStyle: { color: fg },
        formatter: function (rows) {
          if (!rows.length) return "";
          var head = state.suite === "fir"
            ? "block " + rows[0].data[2]
            : "N = " + rows[0].data[2];
          var lines = rows.slice().sort(function (a, b) {
            return (b.data[1] || 0) - (a.data[1] || 0);
          }).map(function (r) {
            var val = state.metric === "throughput"
              ? fmtSI(r.data[1]) + " " + metric.unit
              : fmtNum(r.data[1]) + " " + metric.unit;
            return r.marker + r.seriesName + ": <strong>" + val + "</strong>";
          });
          return "<div style='font-weight:600;margin-bottom:4px'>" + head +
                 "</div>" + lines.join("<br>");
        }
      },
      xAxis: {
        type: "log", name: xName, nameLocation: "middle", nameGap: 30,
        nameTextStyle: { color: faint },
        axisLine: { lineStyle: { color: faint } },
        axisLabel: { color: faint, formatter: function (v) { return v; } },
        splitLine: { lineStyle: { color: split } }
      },
      yAxis: {
        type: "log", name: yName, nameLocation: "end", nameGap: 12,
        nameTextStyle: { color: faint, align: "left" },
        axisLine: { lineStyle: { color: faint } },
        axisLabel: {
          color: faint,
          formatter: function (v) {
            return state.metric === "throughput" ? fmtSI(v) : v;
          }
        },
        splitLine: { lineStyle: { color: split } }
      },
      series: series
    };

    chart.setOption(option, true);
    var empty = el(root, "empty");
    empty.hidden = series.length > 0;
  }

  function buildControls(root, state) {
    var data = window.BENCH_DATA;
    var suites = Object.keys(data.suites);

    makeButtons(el(root, "suite"),
      suites.map(function (s) { return { value: s, label: data.suites[s].label }; }),
      state.suite, function (v) { state.suite = v; onSuiteChange(root, state); });

    var suiteData = data.suites[state.suite];
    var archKeys = Object.keys(suiteData.archs);
    makeButtons(el(root, "arch"),
      archKeys.map(function (a) {
        return { value: a, label: suiteData.arch_labels[a] || a };
      }),
      state.arch, function (v) { state.arch = v; buildControls(root, state); render(root, state); });

    makeButtons(el(root, "metric"),
      Object.keys(METRICS).map(function (m) { return { value: m, label: METRICS[m].label }; }),
      state.metric, function (v) { state.metric = v; render(root, state); });

    var tapsGroup = el(root, "taps");
    if (state.suite === "fir" && suiteData.archs[state.arch].taps) {
      var taps = suiteData.archs[state.arch].taps;
      makeButtons(tapsGroup,
        taps.map(function (t) { return { value: t, label: t + " taps" }; }),
        state.taps, function (v) { state.taps = v; render(root, state); });
      tapsGroup.hidden = false;
    } else {
      tapsGroup.hidden = true;
    }
  }

  // Reconcile arch/taps selection when the suite changes, then rebuild + render.
  function onSuiteChange(root, state) {
    var suiteData = window.BENCH_DATA.suites[state.suite];
    var archKeys = Object.keys(suiteData.archs);
    if (archKeys.indexOf(state.arch) < 0) state.arch = archKeys[0];
    if (state.suite === "fir") {
      var taps = suiteData.archs[state.arch].taps || [];
      if (taps.indexOf(state.taps) < 0) state.taps = taps[0];
    }
    buildControls(root, state);
    render(root, state);
  }

  function teardown() {
    if (resizeHandler) { window.removeEventListener("resize", resizeHandler); resizeHandler = null; }
    if (themeObserver) { themeObserver.disconnect(); themeObserver = null; }
    if (chart) { chart.dispose(); chart = null; }
  }

  function init() {
    var root = document.getElementById("bench-dashboard");
    if (!root) return;                 // not the benchmarks page
    teardown();                        // guard against instant-nav double init

    var data = window.BENCH_DATA;
    var hasData = data && data.suites && Object.keys(data.suites).length > 0;
    if (typeof echarts === "undefined" || !hasData) {
      var empty = el(root, "empty");
      if (empty) empty.hidden = false;
      return;
    }

    var suites = Object.keys(data.suites);
    var state = { suite: suites[0], arch: null, metric: "latency", taps: null };
    var firstSuite = data.suites[state.suite];
    state.arch = Object.keys(firstSuite.archs)[0];
    if (state.suite === "fir") {
      var t = firstSuite.archs[state.arch].taps || [];
      state.taps = t[0];
    }

    var chartEl = el(root, "chart");
    var minH = parseInt(root.getAttribute("data-echarts-min-height"), 10) || 420;
    chartEl.style.minHeight = minH + "px";
    chart = echarts.init(chartEl, null, { renderer: "canvas" });

    buildControls(root, state);
    render(root, state);
    footnote(root, data);

    resizeHandler = function () { if (chart) chart.resize(); };
    window.addEventListener("resize", resizeHandler);

    // Re-theme when Material's light/dark palette toggles (it flips
    // data-md-color-scheme on <body>).
    themeObserver = new MutationObserver(function () { render(root, state); });
    themeObserver.observe(document.body, {
      attributes: true, attributeFilter: ["data-md-color-scheme"]
    });
  }

  // MkDocs Material exposes an RxJS document$ that fires on every instant
  // navigation; fall back to DOMContentLoaded when it isn't present.
  if (window.document$ && typeof window.document$.subscribe === "function") {
    window.document$.subscribe(init);
  } else if (document.readyState !== "loading") {
    init();
  } else {
    document.addEventListener("DOMContentLoaded", init);
  }
})();
