/* aither's parser: tokenizer and recursive descent to a syntax tree. */

#ifndef AITHER_PARSER_H
#define AITHER_PARSER_H

typedef enum {
  nkNum, nkIdent, nkBinOp, nkUnary, nkCall, nkIf,
  nkVar, nkLet, nkDef, nkAssign, nkArr, nkIdx, nkIdxAssign, nkBlock,
  nkPlay, nkLambda,
  nkImport,       /* `import "lib/voice.aither"`; str = the path */
  nkStr,          /* a string literal; str = its text */
  nkNamedArg      /* `name = expr` in a call; str = name, kids = [expr] */
} NodeKind;

typedef struct Node Node;
struct Node {
  NodeKind kind;
  const char* str;        /* name / op ("" when none) */
  double num;             /* literal value */
  const char** params;    /* def parameters, lambda parameters, play modifiers */
  Node** defaults;        /* def parameter defaults, parallel to params (NULL: none) */
  int nparams;
  int is_rec;             /* `def rec`: the def may call itself, and repeats */
  Node** kids;            /* children (interpretation per kind); a kid may be NULL */
  int nkids;
  int line;
  const char* source;     /* origin ("stdlib", "patches/foo.aither"), or "" */
};

/* An arena: every node, string and array of a parse lives in it and is
 * freed with it. */
typedef struct Arena Arena;
Arena* arena_new(void);
void arena_free(Arena* a);

/* Parse a program. NULL on a parse error, with the message in `err`
 * (as the Nim parser words it). */
Node* parse_program(Arena* a, const char* source, char* err, int errlen);

/* Does the program call `input()` anywhere? */
int calls_input(const Node* n);

/* Stamp every node of a subtree with its source. */
void set_source(Arena* a, Node* n, const char* source);

/* The tree as text, one form per node, for comparing parsers. */
void dump_node(const Node* n, char** buf, long* len, long* cap);

#endif
