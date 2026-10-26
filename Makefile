NIM   := nim
FLAGS := --mm:arc --threads:on --opt:speed -d:danger \
         --passC:"-O3 -march=native -flto -fno-plt -fno-stack-protector" \
         --passL:"-flto -ltcc -ldl -lasound" --hints:off --warnings:off

all: aither

aither: aither.nim engine.nim engine_types.nim cli_output.nim analysis.nim render.nim voice.nim codegen.nim tcc.nim parser.nim imports.nim dsp.nim dsp.c dsp.h midi.nim alsa_midi.c realtime.c stdlib.aither
	$(NIM) c $(FLAGS) --out:aither aither.nim

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

# parser.c against parser.nim: the tree of every tracked patch and the
# stdlib, printed by each, must be the same text.
parse-check:
	mkdir -p nimcache/parse-check
	cc -O2 -Wall -o nimcache/parse-check/astdump_c tools/astdump/astdump.c parser.c
	$(NIM) c --hints:off --warnings:off -d:release --nimcache:nimcache/parse-check \
	  --out:nimcache/parse-check/astdump_nim tools/astdump/astdump.nim
	files="$$(git ls-files '*.aither') stdlib.aither"; \
	  nimcache/parse-check/astdump_c $$files > nimcache/parse-check/c.txt && \
	  nimcache/parse-check/astdump_nim $$files > nimcache/parse-check/nim.txt && \
	  cmp nimcache/parse-check/c.txt nimcache/parse-check/nim.txt && \
	  echo "parse-check ok ($$(grep -c '^==' nimcache/parse-check/c.txt) files)"
.PHONY: parse-check

# codegen.c against codegen.nim: the C every tracked patch compiles to,
# and its state layout, written by each, must be the same text.
codegen-check:
	mkdir -p nimcache/codegen-check
	cc -O2 -Wall -Wno-unused-function -o nimcache/codegen-check/cgdump_c \
	  tools/codegen-check/cgdump.c codegen.c imports.c parser.c dsp.c -lm
	$(NIM) c --hints:off --warnings:off -d:release --mm:arc --threads:on \
	  --passL:"-ltcc -ldl -lasound" --nimcache:nimcache/codegen-check \
	  --out:nimcache/codegen-check/cgdump_nim tools/codegen-check/cgdump.nim
	files="$$(git ls-files '*.aither')"; \
	  nimcache/codegen-check/cgdump_c stdlib.aither $$files > nimcache/codegen-check/c.txt && \
	  nimcache/codegen-check/cgdump_nim stdlib.aither $$files > nimcache/codegen-check/nim.txt && \
	  cmp nimcache/codegen-check/c.txt nimcache/codegen-check/nim.txt && \
	  echo "codegen-check ok ($$(grep -c '^==' nimcache/codegen-check/c.txt) files)"
.PHONY: codegen-check
