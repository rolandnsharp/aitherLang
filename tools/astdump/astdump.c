/* Print a patch's syntax tree as parser.c reads it (compare with astdump.nim). */
#include <stdio.h>
#include <stdlib.h>
#include "../../parser.h"

int main(int argc, char** argv) {
  for (int f = 1; f < argc; f++) {
    FILE* fp = fopen(argv[f], "rb");
    if (!fp) { perror(argv[f]); return 1; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    char* src = malloc(n + 1);
    fread(src, 1, n, fp); src[n] = 0; fclose(fp);
    Arena* a = arena_new();
    char err[1024];
    Node* prog = parse_program(a, src, err, sizeof err);
    printf("== %s\n", argv[f]);
    if (!prog) printf("error: %s\n", err);
    else {
      char* buf = NULL; long len = 0, cap = 0;
      dump_node(prog, &buf, &len, &cap);
      printf("%.*s\n", (int)len, buf);
      free(buf);
    }
    arena_free(a); free(src);
  }
  return 0;
}
