# Convenience wrapper around the CMake benchmark flow.
#
#   make benchmarks   # configure (Release, all backends), build, run, collate
#   make fir-benchmarks # configure/build/run portable + liquid/KFR/IPP/CMSIS FIR benchmarks
#   make build        # configure + build the bench executables only
#   make clean        # remove the build dir and bench_results/
#
# "All backends" is resolved by CMake's DSP_ENABLE_ALL_BENCHMARKS option, which
# enables every FFT and FIR benchmark backend compatible with this host (x86:
# portable, kfr, mkl, aocl, ipp, cmsis, liquid; Arm: portable, cmsis, liquid,
# and kfr under Clang). No backend list is hardcoded here -- choosing backends
# is CMake's job, so e.g. x86-only IPP is never fetched on an Arm host.
#
# Override any variable on the command line, e.g.:
#   make benchmarks BUILD_DIR=bench-out KFR_ARCH=avx2
#   make benchmarks BUILD_TYPE=RelWithDebInfo

BUILD_DIR  ?= build-bench
BUILD_TYPE ?= Release
# Prefer Ninja when present; fall back to Unix Makefiles otherwise.
GENERATOR  ?= $(shell command -v ninja >/dev/null 2>&1 && echo Ninja || echo "Unix Makefiles")

# Disable ASLR for the benchmark run so results are reproducible (silences
# Google Benchmark's "ASLR is enabled" warning). setarch -R sets the
# ADDR_NO_RANDOMIZE personality, which child processes inherit, so wrapping the
# runner covers every fft_bench_* it spawns. No-op if setarch is unavailable
# (e.g. non-Linux); pass NO_ASLR_DISABLE=1 to opt out.
ifndef NO_ASLR_DISABLE
ASLR_WRAP ?= $(shell command -v setarch >/dev/null 2>&1 && echo "setarch -R")
endif

# Optional: raise KFR's ISA baseline for a fair comparison (avx2|avx512|...).
# Only added to the configure line when set.
ifdef KFR_ARCH
KFR_ARCH_FLAG := -DKFR_ARCH=$(KFR_ARCH)
endif

.DEFAULT_GOAL := help
.PHONY: benchmarks fir-benchmarks configure configure-fir build build-fir run run-fir clean help

## benchmarks: configure + build all backends, then run and collate results
benchmarks: run

## fir-benchmarks: configure + build portable/liquid/KFR/IPP/CMSIS FIR backends, then run and collate results
fir-benchmarks: run-fir

## configure: run CMake with Release + every host-compatible benchmark backend
configure:
	cmake -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	      -DDSP_ENABLE_ALL_BENCHMARKS=ON \
	      $(KFR_ARCH_FLAG) \
	      -S . -B $(BUILD_DIR)

## configure-fir: alias for configure (same host-gated backend set; suite is
## selected at build/run time, not configure time)
configure-fir: configure

## build: build the configured FFT benchmark executables
build: configure
	cmake --build $(BUILD_DIR) --target fft_benchmarks

## build-fir: build the configured FIR benchmark executables
build-fir: configure-fir
	cmake --build $(BUILD_DIR) --target fir_benchmarks

## run: run every fft_bench_* and write bench_results/ (tables, CSV, graph)
run: build
	$(ASLR_WRAP) tools/run_benchmarks.py --build-dir $(BUILD_DIR)

## run-fir: run every fir_bench_* and write bench_results/
run-fir: build-fir
	$(ASLR_WRAP) tools/run_benchmarks.py --build-dir $(BUILD_DIR) --suite fir

## clean: remove the build directory and benchmark output
clean:
	rm -rf $(BUILD_DIR) bench_results

## help: list available targets
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  /'
