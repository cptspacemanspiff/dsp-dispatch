# Convenience wrapper around the CMake benchmark flow.
#
#   make benchmarks   # configure (Release, all backends), build, run, collate
#   make build        # configure + build the bench executables only
#   make clean        # remove the build dir and bench_results/
#
# "All backends" is resolved by CMake's FFT_ENABLE_ALL_BENCHMARKS option, which
# enables every benchmark backend compatible with this host (x86: portable, kfr,
# mkl, aocl; Arm/Apple: portable, kfr). No backend list is hardcoded here.
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
.PHONY: benchmarks configure build run clean help

## benchmarks: configure + build all backends, then run and collate results
benchmarks: run

## configure: run CMake with Release + every host-compatible benchmark backend
configure:
	cmake -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	      -DFFT_ENABLE_ALL_BENCHMARKS=ON \
	      $(KFR_ARCH_FLAG) \
	      -S . -B $(BUILD_DIR)

## build: build the per-backend benchmark executables
build: configure
	cmake --build $(BUILD_DIR)

## run: run every fft_bench_* and write bench_results/ (tables, CSV, graph)
run: build
	$(ASLR_WRAP) tools/run_benchmarks.py --build-dir $(BUILD_DIR)

## clean: remove the build directory and benchmark output
clean:
	rm -rf $(BUILD_DIR) bench_results

## help: list available targets
help:
	@grep -E '^## ' $(MAKEFILE_LIST) | sed 's/^## /  /'
