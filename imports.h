/* The program the compiler sees: the stdlib, then the patch with its
 * imports spliced in (imports.c). */

#ifndef AITHER_IMPORTS_H
#define AITHER_IMPORTS_H

#include "parser.h"

/* NULL on an error, with the message in `err`. `src_tag` is the patch's
 * path, or "<inline>" (imports then resolve against the current
 * directory). */
Node* build_program(Arena* a, const char* stdlib_src, const char* src, const char* src_tag,
                    char* err, int errlen);

#endif
