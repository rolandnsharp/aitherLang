NIM   := nim
FLAGS := --mm:arc --threads:on --opt:speed -d:danger \
         --passC:"-O3 -march=native -flto -fno-plt -fno-stack-protector" \
         --passL:"-flto -ltcc -ldl -lasound" --hints:off --warnings:off

all: aither

aither: aither.nim engine.nim engine_types.nim cli_output.nim analysis.nim render.nim voice.nim codegen.nim tcc.nim parser.nim dsp.nim midi.nim alsa_midi.c stdlib.aither miniaudio.nim miniaudio_wrapper.c miniaudio.h
	$(NIM) c $(FLAGS) --out:aither aither.nim

clean:
	rm -rf aither nimcache/ test_parser test_eval test_stdlib

.PHONY: all clean
