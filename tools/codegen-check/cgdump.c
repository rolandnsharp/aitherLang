/* The C a patch compiles to, and its state layout, from codegen.c
 * (compare with cgdump.nim, the Nim compiler's). */
#include <stdio.h>
#include <stdlib.h>
#include "../../codegen.h"
#include "../../imports.h"

static char* slurp(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  char* s = malloc(n + 1);
  if (fread(s, 1, n, f) != (size_t)n) n = 0;
  s[n] = 0; fclose(f);
  return s;
}

int main(int argc, char** argv) {
  char* stdlib = slurp(argv[1]);
  for (int f = 2; f < argc; f++) {
    char* src = slurp(argv[f]);
    Arena* a = arena_new();
    char err[2048];
    printf("== %s\n", argv[f]);
    int kind;
    Node* prog = build_program(a, stdlib, src, argv[f], 0, &kind, err, sizeof err);
    Generated g;
    if (!prog) printf("error: %s\n", err);
    else if (generate_full(a, prog, "", 48000.0, 1, &g, err, sizeof err)) printf("error: %s\n", err);
    else {
      fputs(g.csrc, stdout);
      printf("-- vars");
      for (int i = 0; i < g.nvars; i++) printf(" %s", g.var_names[i]);
      printf("\n-- parts");
      for (int i = 0; i < g.nparts; i++) printf(" %s", g.part_names[i]);
      printf("\n-- unheard");
      for (int i = 0; i < g.nunheard; i++) printf(" %s", g.unheard[i]);
      printf("\n");
      for (int i = 0; i < g.nregions; i++) {
        Region* r = &g.regions[i];
        printf("-- region %s %d [%s] %d [%s] %ld %ld\n", r->type_name, r->per_type_idx, r->scope,
               r->scope_idx, r->owner, r->offset, r->size);
      }
    }
    arena_free(a);
    free(src);
  }
  return 0;
}
