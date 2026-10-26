/* The program the compiler sees: the stdlib, then the patch with its
 * imports spliced in (imports.c). */

#ifndef AITHER_IMPORTS_H
#define AITHER_IMPORTS_H

#include "parser.h"

/* NULL on an error, with the message in `err` and in `*kind` which:
 * BUILD_PARSE (the patch or the stdlib) or BUILD_IMPORT. `src_tag` is the
 * patch's path, or "<inline>" (imports then resolve against the current
 * directory). `seed_self`: the patch counts as already imported, so a
 * library importing it back stops there. */
enum { BUILD_OK, BUILD_PARSE, BUILD_IMPORT };
Node* build_program(Arena* a, const char* stdlib_src, const char* src, const char* src_tag,
                    int seed_self, int* kind, char* err, int errlen);

#endif
