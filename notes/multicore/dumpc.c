/* The C a patch compiles to: notes/multicore/dumpc patch.aither > out.c
 *   cc -std=gnu23 -O1 -ffp-contract=off -pthread -I. -o notes/multicore/dumpc notes/multicore/dumpc.c \
 *      engine.c voice.c render.c analysis.c cli_output.c midi.c alsa_midi.c realtime.c codegen.c \
 *      parser.c imports.c dsp.c stdlib.c -ltcc -ldl -lasound -lm */
#include <stdio.h>
#include <stdlib.h>
#include "parser.h"
#include "codegen.h"
#include "imports.h"
extern const char aither_stdlib[];
int main(int argc, char** argv) {
  if (argc < 2) return 2;
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 1;
  fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
  char* src = calloc(n + 1, 1);
  if (fread(src, 1, n, f) != (size_t)n) return 1;
  fclose(f);
  char err[4096]; int kind;
  Arena* a = arena_new();
  Node* prog = build_program(a, aither_stdlib, src, argv[1], 0, &kind, err, sizeof err);
  if (!prog) { fprintf(stderr, "%s\n", err); return 1; }
  Generated g;
  if (generate_full(a, prog, argv[1], 48000.0, 1, &g, err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
  fputs(g.csrc, stdout);
  return 0;
}
