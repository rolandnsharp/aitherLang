/* aither to C: a program's tree (stdlib first) to a translation unit TCC
 * or cc compiles, and what the engine needs beside it. */

#ifndef AITHER_CODEGEN_H
#define AITHER_CODEGEN_H

#include "parser.h"

/* A stateful call site's slots in the pool. Migration across a hot
 * reload matches regions by (scope, type, owner, scope_idx), then by
 * (type, owner, per_type_idx). */
typedef struct {
  const char* type_name;
  int per_type_idx;
  const char* scope;     /* the statement it sits in: `let bass`, `play drums/out` */
  int scope_idx;         /* its index among same-type calls there */
  const char* owner;     /* the defs it is inlined through, and `/$name` for state */
  long offset;           /* absolute, in slots */
  long size;
} Region;

typedef struct {
  const char* csrc;
  const char** var_names; int nvars;       /* file-level state, in order */
  const char** part_names; int nparts;     /* play blocks, in order */
  const char** unheard; int nunheard;      /* plays the output never reaches */
  Region* regions; int nregions;
} Generated;

/* Compile `program` (stdlib and imports already spliced in). 0 on
 * success; on an error, -1 with the message in `err`. Everything
 * returned lives in the arena. */
int generate_full(Arena* a, Node* program, const char* patch_path, double sr,
                  int hoist_on, Generated* out, char* err, int errlen);

#endif
