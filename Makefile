NIM   := nim
FLAGS := --mm:arc --threads:on --opt:speed -d:danger \
         --passC:"-O3 -march=native -flto -fno-plt -fno-stack-protector" \
         --passL:"-flto" --hints:off --warnings:off

all: aither

aither: engine.nim eval.nim parser.nim dsp.nim stdlib.aither miniaudio.nim miniaudio_wrapper.c miniaudio.h
	$(NIM) c $(FLAGS) --out:aither engine.nim

clean:
	rm -rf aither nimcache/ test_parser test_eval test_stdlib

.PHONY: all clean
