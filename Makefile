# Convenience wrapper around the CMake benchmark flow.
#
#   make benchmarks   # configure (Release, all backends), build, run, collate
#   make fir-benchmarks # configure/build/run portable + liquid/KFR/IPP/CMSIS FIR benchmarks
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
.PHONY: benchmarks fir-benchmarks configure configure-fir build build-fir run run-fir clean help

## benchmarks: configure + build all backends, then run and collate results
benchmarks: run

## fir-benchmarks: configure + build portable/liquid/KFR/IPP/CMSIS FIR backends, then run and collate results
fir-benchmarks: run-fir

## configure: run CMake with Release + every host-compatible benchmark backend
configure:
	cmake -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	      -DFFT_ENABLE_ALL_BENCHMARKS=ON \
	      $(KFR_ARCH_FLAG) \
	      -S . -B $(BUILD_DIR)

## configure-fir: run CMake with Release + FIR benchmark backends
configure-fir:
	cmake -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	      -DFFT_ENABLE_BENCHMARKS=ON \
	      -DFIR_ENABLE_LIQUID_BENCHMARK=ON \
	      -DFIR_ENABLE_KFR_BENCHMARK=ON \
	      -DFIR_ENABLE_IPP_BENCHMARK=ON \
	      -DFIR_ENABLE_CMSIS_BENCHMARK=ON \
	      -S . -B $(BUILD_DIR)

## build: build the per-backend benchmark executables
build: configure
	cmake --build $(BUILD_DIR)

## build-fir: build the FIR benchmark executables
build-fir: configure-fir
	cmake --build $(BUILD_DIR) --target fir_bench_portable fir_bench_liquid fir_bench_kfr fir_bench_ipp fir_bench_cmsis

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
