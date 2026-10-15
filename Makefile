NIM   := nim
FLAGS := --mm:arc --threads:on --opt:speed -d:danger \
         --passC:"-O3 -march=native -flto -fno-plt -fno-stack-protector" \
         --passL:"-flto -ltcc -ldl -lasound" --hints:off --warnings:off

all: aither

aither: aither.nim engine.nim engine_types.nim cli_output.nim analysis.nim render.nim voice.nim codegen.nim tcc.nim parser.nim imports.nim dsp.nim midi.nim alsa_midi.c stdlib.aither
	$(NIM) c $(FLAGS) --out:aither aither.nim

tools/play: tools/play.c
	cc -O2 tools/play.c -o tools/play -lasound

# One binary for every test that doesn't touch the engine's voice
# table (tests/all.nim), then the four that do, each on its own.
TESTFLAGS := --mm:arc --threads:on --hints:off --warnings:off \
             --nimcache:nimcache/tests --passL:"-ltcc -ldl -lasound"
test:
	$(NIM) c -r $(TESTFLAGS) --out:tests/all tests/all.nim
	for t in cli_output_format midi_resilience slot_sweep solo_mutes; do \
	  $(NIM) c -r $(TESTFLAGS) --out:tests/$$t tests/$$t.nim || exit 1; done

clean:
	rm -rf aither nimcache/ test_parser test_eval test_stdlib tools/play

.PHONY: all clean test
