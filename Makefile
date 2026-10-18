NIM   := nim
FLAGS := --mm:arc --threads:on --opt:speed -d:danger \
         --passC:"-O3 -march=native -flto -fno-plt -fno-stack-protector" \
         --passL:"-flto -ltcc -ldl -lasound" --hints:off --warnings:off

all: aither

aither: aither.nim engine.nim engine_types.nim cli_output.nim analysis.nim render.nim voice.nim codegen.nim tcc.nim parser.nim imports.nim dsp.nim midi.nim alsa_midi.c stdlib.aither
	$(NIM) c $(FLAGS) --out:aither aither.nim

tools/play: tools/play.c
	cc -O2 tools/play.c -o tools/play -lasound

tools/card: tools/card.c
	cc -O2 -Wall tools/card.c -o tools/card -lasound

tools/pad: tools/pad.c
	cc -O2 tools/pad.c -o tools/pad -lasound

tools/scope: tools/scope.c tools/display.h
	cc -O2 -Wall tools/scope.c -o tools/scope $$(pkg-config --cflags --libs sdl3) -lEGL -lGLESv2 -lpthread -lm

tools/tv: tools/tv.c tools/display.h
	cc -O2 -Wall tools/tv.c -o tools/tv $$(pkg-config --cflags --libs sdl3) -lEGL -lGLESv2 -lpthread -lm

# One binary for every test that doesn't touch the engine's voice
# table (tests/all.nim), then the ones that do, each on its own;
# engine_live runs ./aither itself, so make builds it first.
TESTFLAGS := --mm:arc --threads:on --hints:off --warnings:off \
             --nimcache:nimcache/tests --passL:"-ltcc -ldl -lasound"
test: aither
	$(NIM) c -r $(TESTFLAGS) --out:tests/all tests/all.nim
	for t in cli_output_format midi_resilience slot_sweep solo_mutes engine_live; do \
	  $(NIM) c -r $(TESTFLAGS) --out:tests/$$t tests/$$t.nim || exit 1; done

# Every tracked patch rendered and fingerprinted against tests/corpus.golden
# (tests/corpus.nim). After an intended change: make corpus-update.
corpus:
	$(NIM) c -r $(TESTFLAGS) --out:tests/corpus tests/corpus.nim

corpus-update:
	$(NIM) c -r $(TESTFLAGS) --out:tests/corpus tests/corpus.nim update

# Every patch rendered with TCC's code and with the cc -O2 build: the bits
# must match (tests/exact_builds.nim). About 10 minutes on the uConsole.
exact:
	$(NIM) c -r $(TESTFLAGS) --out:tests/exact_builds tests/exact_builds.nim

clean:
	rm -rf aither nimcache/ test_parser test_eval test_stdlib tools/play tools/card tools/pad tools/scope tools/tv

.PHONY: all clean test corpus corpus-update exact
