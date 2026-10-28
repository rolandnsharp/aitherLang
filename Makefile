# The binary is C. No fused multiply-adds (-ffp-contract=off): dsp.c's
# natives must compute what TCC's code does, bit for bit (make exact).
CC     := cc
CFLAGS := -std=gnu23 -O3 -march=native -flto -fno-plt -fno-stack-protector \
          -fno-math-errno -ffp-contract=off -pthread
LIBS   := -ltcc -ldl -lasound -lm
ENGINE := engine.c voice.c render.c analysis.c cli_output.c midi.c alsa_midi.c realtime.c \
          codegen.c parser.c imports.c dsp.c stdlib.c
HDR    := engine.h voice.h render.h analysis.h cli_output.h midi.h codegen.h parser.h \
          imports.h dsp.h

all: aither

# Every binary is linked to a temporary name and renamed into place: a
# rename is atomic, so whatever is running, or starts during a build, gets
# the old binary or the new one whole, never a half-written file.
aither: aither.c $(ENGINE) $(HDR) stdlib.aither
	$(CC) $(CFLAGS) -o $@.tmp aither.c $(ENGINE) $(LIBS) && mv -f $@.tmp $@

tools/play: tools/play.c realtime.c
	cc -O2 tools/play.c realtime.c -o $@.tmp -lasound && mv -f $@.tmp $@

tools/card: tools/card.c realtime.c
	cc -O2 -Wall tools/card.c realtime.c -o $@.tmp -lasound && mv -f $@.tmp $@

tools/pad: tools/pad.c
	cc -O2 tools/pad.c -o $@.tmp -lasound && mv -f $@.tmp $@

tools/scope: tools/scope.c tools/display.h
	cc -O2 -Wall tools/scope.c -o $@.tmp $$(pkg-config --cflags --libs sdl3) -lEGL -lGLESv2 -lpthread -lm && mv -f $@.tmp $@

tools/tv: tools/tv.c tools/display.h
	cc -O2 -Wall tools/tv.c -o $@.tmp $$(pkg-config --cflags --libs sdl3) -lEGL -lGLESv2 -lpthread -lm && mv -f $@.tmp $@

# tests/run: every TEST in the C files under tests/, each in its own
# process (tests/check.h); `tests/run name ...` runs those. engine_live
# runs ./aither itself, so make builds it first. The corpus, exact and
# temporal_audit are programs of their own.
TESTCFLAGS := -std=gnu23 -O2 -Wall -ffp-contract=off -pthread
TOOLS  := tests/corpus.c tests/exact.c tests/temporal_audit.c tests/bench.c
TESTS  := $(filter-out $(TOOLS),$(wildcard tests/*.c))

tests/run: $(TESTS) tests/check.h $(ENGINE) $(HDR) stdlib.aither
	$(CC) $(TESTCFLAGS) -o $@.tmp $(TESTS) $(ENGINE) $(LIBS) && mv -f $@.tmp $@

tests/corpus tests/exact tests/temporal_audit tests/bench: tests/%: tests/%.c $(ENGINE) $(HDR) stdlib.aither
	$(CC) $(TESTCFLAGS) -o $@.tmp $< $(ENGINE) $(LIBS) && mv -f $@.tmp $@

test: aither tests/run
	tests/run

# Every tracked patch rendered and fingerprinted against tests/corpus.golden
# (tests/corpus.c). After an intended change: make corpus-update.
corpus: tests/corpus
	tests/corpus

corpus-update: tests/corpus
	tests/corpus update

# What sines cost in each shape a patch writes them, and how far the
# compiler's fast forms stray from sin() (tests/bench.c).
bench: tests/bench
	tests/bench

# Every patch rendered with TCC's code and with the cc -O2 build: the bits
# must match (tests/exact.c). About 10 minutes on the uConsole.
exact: tests/exact
	tests/exact

clean:
	rm -rf aither *.tmp tools/*.tmp tests/*.tmp nimcache/ tests/run tests/corpus tests/exact tests/temporal_audit tests/bench tools/play tools/card tools/pad tools/scope tools/tv

.PHONY: all clean test corpus corpus-update exact bench
