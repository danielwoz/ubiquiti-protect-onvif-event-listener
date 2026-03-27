CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread
CPPFLAGS := $(shell pkg-config --cflags libxml-2.0 libcurl openssl sqlite3) \
            -I$(shell pg_config --includedir)
LDFLAGS  := $(shell pkg-config --libs   libxml-2.0 libcurl openssl sqlite3) \
            -L$(shell pg_config --libdir) -lpq -pthread

TARGET := onvif_recorder
OBJS   := onvif_listener.o detection_recorder.o unifi_camera_config.o ubv_thumbnail.o main.o

.PHONY: all clean install-hooks

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

onvif_listener.o: onvif_listener.cpp onvif_listener.hpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

detection_recorder.o: detection_recorder.cpp detection_recorder.hpp onvif_listener.hpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

unifi_camera_config.o: unifi_camera_config.cpp unifi_camera_config.hpp onvif_listener.hpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

ubv_thumbnail.o: ubv_thumbnail.cpp ubv_thumbnail.hpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

main.o: main.cpp onvif_listener.hpp detection_recorder.hpp unifi_camera_config.hpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS) onvif_events_*.jsonl

install-hooks:
	git config core.hooksPath .githooks
	@echo "Git hooks installed. Pre-push hook will run lint, tests, and PGO bench."

# ---------------------------------------------------------------------------
# PGO + LTO benchmark workflow
#
# x86_64 (full instrumented PGO):
#   make pgo-bench-x86            # runs all 6 steps, prints before/after
#   make pgo-bench-x86 PGO_EVENTS=100000
#
# ARM64 (cross-PGO -- reuses the x86 profile, runs under QEMU):
#   sudo apt-get install qemu-user-static
#   make pgo-bench-x86            # collect x86 profile first
#   make pgo-bench-arm64
#
# Clean generated profile data:
#   make pgo-clean
# ---------------------------------------------------------------------------

BAZEL       := ~/.local/bin/bazel
PGO_EVENTS  ?= 50000
BENCH_JSONL := test/testdata/bench_onvif.jsonl
PROFRAW     := $(CURDIR)/pgo.profraw
PROFDATA    := $(CURDIR)/pgo.profdata

# ARM64 sysroot built by the arm64_sysroot Bazel repository rule.
# Evaluated lazily so non-ARM64 targets don't invoke bazel info.
arm64_sysroot = $(shell $(BAZEL) info output_base 2>/dev/null)/external/arm64_sysroot/sysroot

.PHONY: pgo-bench-x86
pgo-bench-x86:
	@echo "=== [1/6] Baseline: clang -O2, no PGO, no LTO ==="
	$(BAZEL) run --config=clang //test:bench_onvif_listener \
	    -- $(PGO_EVENTS) 2>/dev/null
	@echo
	@echo "=== [2/6] Build instrumented binary ==="
	$(BAZEL) build --config=clang --config=pgo_instrument \
	    //test:bench_onvif_listener
	@echo "=== [3/6] Collect profile ($(PGO_EVENTS) events) ==="
	LLVM_PROFILE_FILE=$(PROFRAW) \
	    bazel-bin/test/bench_onvif_listener \
	    $(BENCH_JSONL) $(PGO_EVENTS) 2>/dev/null
	@echo "=== [4/6] Merge profile ==="
	llvm-profdata-14 merge -output=$(PROFDATA) $(PROFRAW)
	@echo "=== [5/6] Build PGO + LTO optimised binary ==="
	$(BAZEL) build --config=clang --config=lto \
	    --copt=-fprofile-instr-use=$(PROFDATA) \
	    --linkopt=-fprofile-instr-use=$(PROFDATA) \
	    //test:bench_onvif_listener
	@echo "=== [6/6] PGO + LTO benchmark ==="
	$(BAZEL) run --config=clang --config=lto \
	    --copt=-fprofile-instr-use=$(PROFDATA) \
	    --linkopt=-fprofile-instr-use=$(PROFDATA) \
	    //test:bench_onvif_listener -- $(PGO_EVENTS) 2>/dev/null

.PHONY: pgo-bench-arm64
pgo-bench-arm64:
	@test -f $(PROFDATA) || \
	    (echo "Run 'make pgo-bench-x86' first to collect the profile." && false)
	@command -v qemu-aarch64-static >/dev/null || \
	    (echo "Install QEMU: sudo apt-get install qemu-user-static" && false)
	@echo "=== [1/4] Build baseline ARM64 binary ==="
	$(BAZEL) build --config=arm64 //test:bench_onvif_listener
	@echo "=== [2/4] Baseline ARM64 benchmark under QEMU ==="
	qemu-aarch64-static -L $(arm64_sysroot) \
	    bazel-bin/test/bench_onvif_listener \
	    $(BENCH_JSONL) $(PGO_EVENTS) 2>/dev/null
	@echo
	@echo "=== [3/4] Build ARM64 with cross-PGO + LTO ==="
	@echo "    (x86 profile applied to ARM64 build -- same LLVM IR structure)"
	$(BAZEL) build --config=arm64 --config=lto \
	    --copt=-fprofile-instr-use=$(PROFDATA) \
	    --linkopt=-fprofile-instr-use=$(PROFDATA) \
	    //test:bench_onvif_listener
	@echo "=== [4/4] ARM64 PGO + LTO benchmark under QEMU ==="
	qemu-aarch64-static -L $(arm64_sysroot) \
	    bazel-bin/test/bench_onvif_listener \
	    $(BENCH_JSONL) $(PGO_EVENTS) 2>/dev/null

.PHONY: pgo-clean
pgo-clean:
	rm -f $(PROFRAW) $(PROFDATA)
