NIM   := nim
# The binary is C. No fused multiply-adds (-ffp-contract=off): dsp.c's
# natives must compute what TCC's code does, bit for bit (make exact).
CC     := cc
CFLAGS := -std=gnu23 -O3 -march=native -flto -fno-plt -fno-stack-protector \
          -fno-math-errno -ffp-contract=off -pthread
LIBS   := -ltcc -ldl -lasound -lm
SRC    := aither.c engine.c voice.c render.c analysis.c cli_output.c midi.c alsa_midi.c \
          realtime.c codegen.c parser.c imports.c dsp.c
HDR    := engine.h voice.h render.h analysis.h cli_output.h midi.h codegen.h parser.h \
          imports.h dsp.h

all: aither

# Nim decides whether to rebuild a test binary from its .nim sources
# alone: when only a C file it {.compile.}s changed, it reruns the old
# binary, and the tests run the old compiler (it bit on 2026-10-14 and
# again on 2026-10-28). So when any C source or header is newer than the
# last build, every test build cache goes.
fresh-c:
	@if [ ! -f nimcache/.c-stamp ] || [ -n "$$(find . -maxdepth 1 \( -name '*.c' -o -name '*.h' \) -newer nimcache/.c-stamp)" ]; then \
	  rm -rf nimcache/tests nimcache/dumpc; \
	  rm -f tests/all tests/corpus tests/exact_builds $(addprefix tests/,cli_output_format midi_resilience slot_sweep solo_mutes engine_live); \
	  mkdir -p nimcache && touch nimcache/.c-stamp; \
	fi; true
.PHONY: fresh-c

aither: $(SRC) $(HDR) stdlib.aither
	$(CC) $(CFLAGS) -o aither $(SRC) $(LIBS)

tools/play: tools/play.c realtime.c
	cc -O2 tools/play.c realtime.c -o tools/play -lasound

tools/card: tools/card.c realtime.c
	cc -O2 -Wall tools/card.c realtime.c -o tools/card -lasound

tools/pad: tools/pad.c
	cc -O2 tools/pad.c -o tools/pad -lasound

tools/scope: tools/scope.c tools/display.h
	cc -O2 -Wall tools/scope.c -o tools/scope $$(pkg-config --cflags --libs sdl3) -lEGL -lGLESv2 -lpthread -lm

tools/tv: tools/tv.c tools/display.h
	cc -O2 -Wall tools/tv.c -o tools/tv $$(pkg-config --cflags --libs sdl3) -lEGL -lGLESv2 -lpthread -lm

# One binary for every test that doesn't touch the engine's voice
# table (tests/all.nim), then the ones that do, each on its own;
# engine_live runs ./aither itself, so make builds it first.
# Each binary its own cache: binaries sharing one don't re-link when another
# has already rebuilt a C file they {.compile.} (codegen.c), and ran stale.
TESTFLAGS := --mm:arc --threads:on --hints:off --warnings:off \
             --passL:"-ltcc -ldl -lasound"
test: aither fresh-c
	$(NIM) c -r $(TESTFLAGS) --nimcache:nimcache/tests/all --out:tests/all tests/all.nim
	for t in cli_output_format midi_resilience slot_sweep solo_mutes engine_live; do \
	  $(NIM) c -r $(TESTFLAGS) --nimcache:nimcache/tests/$$t --out:tests/$$t tests/$$t.nim || exit 1; done

# Every tracked patch rendered and fingerprinted against tests/corpus.golden
# (tests/corpus.nim). After an intended change: make corpus-update.
corpus: fresh-c
	$(NIM) c -r $(TESTFLAGS) --nimcache:nimcache/tests/corpus --out:tests/corpus tests/corpus.nim

corpus-update: fresh-c
	$(NIM) c -r $(TESTFLAGS) --nimcache:nimcache/tests/corpus --out:tests/corpus tests/corpus.nim update

# Every patch rendered with TCC's code and with the cc -O2 build: the bits
# must match (tests/exact_builds.nim). About 10 minutes on the uConsole.
exact: fresh-c
	$(NIM) c -r $(TESTFLAGS) --out:tests/exact_builds tests/exact_builds.nim

clean:
	rm -rf aither nimcache/ test_parser test_eval test_stdlib tools/play tools/card tools/pad tools/scope tools/tv

.PHONY: all clean test corpus corpus-update exact
