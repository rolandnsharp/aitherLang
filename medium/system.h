/* The field core's way in from aither (notes/one-system-2026-11-11.md, "The
 * boundary"): a patch's field system, found from what reads what, solved by
 * the core, and stepped as one moment a sample, after everything else. */
#ifndef AITHER_MEDIUM_SYSTEM_H
#define AITHER_MEDIUM_SYSTEM_H

#include "../parser.h"

/* The program's statements from `from` on are the patch's. When the core
 * can hold its system (milestone 1: `line` media under a linear law, laws on
 * plain state, sources, at the file's level), it rewrites them in `v` and
 * returns 1; when it can't yet, it leaves them as they are and returns 0, and
 * the old lowering takes them; on an error, -1 with the reason in err.
 * AITHER_CORE=0 keeps every patch on the old lowering. `*v` may be replaced
 * (allocated with malloc, as the caller's list is). */
int system_pass(Arena* a, Node*** v, int* n, int* cap, int from, char* err, int errlen);

#endif
