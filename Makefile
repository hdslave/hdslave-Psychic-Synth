# Psychic: a CLAP generative synthesizer for the Akai MPC Sample (64-bit ARM Linux, Cortex-A55).
# Build inside a Linux arm64 image with ./build.sh, which runs `make all test`.
#   make            build/Psychic.clap
#   make test       the tests against the built plug-in (tests/host.cpp) and the sound's fingerprint
#                   (tests/fingerprint.cpp: the output to the bit, against tests/fingerprint.txt)
#   make bench      build/bench: renders a busy pattern; tools/bench.sh counts its instructions
CXX ?= g++
OUT := build
WARN := -Wall -Wextra -Werror
# The MPC's cores. -ffp-contract=fast lets the compiler fuse multiply-adds; nothing here relies on IEEE errno,
# traps or exceptions.
OPT := -O3 -mcpu=cortex-a55 -ffp-contract=fast -fno-math-errno -fno-trapping-math
CXXFLAGS := -std=c++17 $(OPT) $(WARN) -fno-exceptions -fno-rtti -fvisibility=hidden -fPIC -Ithird_party/clap/include -Isrc
# Only clap_entry is exported; libstdc++ and libgcc are linked in so the MPC needs nothing beyond libc and libm. -s
# leaves out the symbol table (the code is the same; the exported clap_entry stays).
LDFLAGS := -shared -s -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,-z,now -Wl,-O1 -Wl,--version-script=src/exports.map

SOURCES := src/psychic.cpp src/screen.cpp
HEADERS := $(wildcard src/*.h src/ui/*.h)

.PHONY: all test bench clean fingerprint-update
all: $(OUT)/Psychic.clap

$(OUT)/Psychic.clap: $(SOURCES) $(HEADERS) src/exports.map Makefile
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(SOURCES) $(LDFLAGS) -o $@

$(OUT)/host: tests/host.cpp tests/wav.h src/melodee_screen.h Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 $(WARN) -Ithird_party/clap/include tests/host.cpp -ldl -o $@

$(OUT)/fingerprint: tests/fingerprint.cpp tests/fingerprint_scenarios.h Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 $(WARN) -Ithird_party/clap/include tests/fingerprint.cpp -ldl -o $@

test: $(OUT)/Psychic.clap $(OUT)/host $(OUT)/fingerprint
	$(OUT)/host $(OUT)/Psychic.clap $(OUT)
	$(OUT)/fingerprint $(OUT)/Psychic.clap tests/fingerprint.txt > /dev/null

# The sound's hashes after a change meant to change the sound (see tests/fingerprint.cpp).
fingerprint-update: $(OUT)/Psychic.clap $(OUT)/fingerprint
	$(OUT)/fingerprint $(OUT)/Psychic.clap > tests/fingerprint.txt

$(OUT)/bench: tests/bench.cpp $(SOURCES) $(HEADERS) Makefile
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 $(OPT) $(WARN) -g -Ithird_party/clap/include -Isrc tests/bench.cpp src/screen.cpp -o $@

bench: $(OUT)/bench

clean:
	rm -rf $(OUT)
