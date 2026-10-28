/* aither's parser (parser.h): tokenizer and recursive descent to a tree. */

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "parser.h"

/* ------------------------------------------------------------------ arena */

typedef struct Block { struct Block* next; size_t used, cap; char data[]; } Block;
struct Arena { Block* head; };

Arena* arena_new(void) { return calloc(1, sizeof(Arena)); }

void arena_free(Arena* a) {
  if (!a) return;
  for (Block* b = a->head; b;) { Block* n = b->next; free(b); b = n; }
  free(a);
}

static void* alloc(Arena* a, size_t n) {
  n = (n + 15) & ~(size_t)15;
  if (!a->head || a->head->used + n > a->head->cap) {
    size_t cap = n > 65536 ? n : 65536;
    Block* b = malloc(sizeof(Block) + cap);
    if (!b) abort();
    b->next = a->head; b->used = 0; b->cap = cap;
    a->head = b;
  }
  void* p = a->head->data + a->head->used;
  a->head->used += n;
  memset(p, 0, n);
  return p;
}

void* arena_alloc(Arena* a, unsigned long n) { return alloc(a, n); }

static const char* dup(Arena* a, const char* s, size_t n) {
  char* d = alloc(a, n + 1);
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}

/* A growable array of pointers, copied into the arena when done. */
typedef struct { void** v; int n, cap; } Vec;
static void push(Vec* x, void* p) {
  if (x->n == x->cap) { x->cap = x->cap ? x->cap * 2 : 8; x->v = realloc(x->v, x->cap * sizeof(void*)); }
  x->v[x->n++] = p;
}
static void** keep(Arena* a, Vec* x) {
  void** d = alloc(a, (x->n ? x->n : 1) * sizeof(void*));
  if (x->n) memcpy(d, x->v, x->n * sizeof(void*));
  free(x->v);
  x->v = NULL;
  return d;
}

/* ----------------------------------------------------------------- errors */

typedef struct {
  Arena* a;
  jmp_buf jump;
  char* err;
  int errlen;
} Ctx;

static void fail_at(Ctx* c, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(c->err, c->errlen, fmt, ap);
  va_end(ap);
  longjmp(c->jump, 1);
}

/* -------------------------------------------------------------- tokenizer */

typedef enum {
  tkNum, tkIdent, tkStateIdent, tkKeyword, tkOp, tkStr,
  tkLParen, tkRParen, tkLBracket, tkRBracket,
  tkComma, tkColon, tkSemi, tkAssign, tkAugAssign,
  tkNewline, tkEof
} TokKind;

static const char* tok_names[] = {
  "tkNum", "tkIdent", "tkStateIdent", "tkKeyword", "tkOp", "tkStr",
  "tkLParen", "tkRParen", "tkLBracket", "tkRBracket",
  "tkComma", "tkColon", "tkSemi", "tkAssign", "tkAugAssign",
  "tkNewline", "tkEof"
};

typedef struct { TokKind kind; const char* str; double num; int line, col; } Token;
typedef struct { Token* t; int n, cap; } Toks;

static void add_tok(Toks* ts, Token t) {
  if (ts->n == ts->cap) { ts->cap = ts->cap ? ts->cap * 2 : 256; ts->t = realloc(ts->t, ts->cap * sizeof(Token)); }
  ts->t[ts->n++] = t;
}

static const char* keywords[] = {"let", "def", "rec", "play", "if", "then", "else",
                                 "mod", "and", "or", "not", "import", NULL};

static int is_keyword(const char* s) {
  for (int i = 0; keywords[i]; i++) if (!strcmp(s, keywords[i])) return 1;
  return 0;
}

static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_word(char c) { return is_alpha(c) || is_digit(c) || c == '_'; }
static int eq(const char* a, const char* b) { return !strcmp(a, b); }

/* A statement can't end on this token: it still needs a right side. */
static int ends_open(const Token* t) {
  if (t->kind == tkOp || t->kind == tkAssign || t->kind == tkAugAssign) return 1;
  if (t->kind != tkKeyword) return 0;
  return eq(t->str, "mod") || eq(t->str, "and") || eq(t->str, "or") ||
         eq(t->str, "not") || eq(t->str, "then") || eq(t->str, "else");
}

static int continues_with(const char* s, long n, long i, const char* w) {
  long k = (long)strlen(w);
  return i + k <= n && !memcmp(s + i, w, k);
}

/* A statement can't begin at s[i]: an infix operator, `then` or `else`.
 * `-`, `not`, `(` and `[` can begin one; there is no unary `+`. */
static int starts_closed(const char* s, long n, long i) {
  static const char* ops[] = {"|>", "==", "!=", "<", ">", "+", "*", "/", "=>", NULL};
  static const char* words[] = {"mod", "and", "or", "then", "else", NULL};
  for (int k = 0; ops[k]; k++) if (continues_with(s, n, i, ops[k])) return 1;
  for (int k = 0; words[k]; k++) {
    long j = i + (long)strlen(words[k]);
    if (continues_with(s, n, i, words[k]) && (j >= n || !is_word(s[j]))) return 1;
  }
  return 0;
}

static double parse_float(Ctx* c, const char* s) {
  char* end;
  double v = strtod(s, &end);
  if (*s == 0 || *end != 0) fail_at(c, "invalid float: %s", s);
  return v;
}

static Toks tokenize(Ctx* c, const char* src) {
  Toks ts = {0};
  long n = (long)strlen(src), i = 0;
  int line = 1, col = 1, at_line_start = 1;
  /* Inside a `(...)` or `[...]` newlines are whitespace, so expressions
   * can split across lines. */
  int depth = 0;
  char buf[256];
#define EMIT(k, s, v) add_tok(&ts, (Token){(k), (s), (v), line, col})

  while (i < n) {
    if (at_line_start) {
      if (depth > 0) {
        while (i < n && src[i] == ' ') { i++; col++; }
        if (i < n && src[i] == '#') while (i < n && src[i] != '\n') i++;
        if (i < n && src[i] == '\n') { i++; line++; col = 1; continue; }
        at_line_start = 0;
      } else {
        /* Indentation means nothing: a block ends at its expression. */
        while (i < n && (src[i] == ' ' || src[i] == '\t')) { i++; col++; }
        if (i >= n || src[i] == '\n' || src[i] == '#') {
          if (i < n && src[i] == '#') while (i < n && src[i] != '\n') i++;
          if (i < n && src[i] == '\n') { i++; line++; col = 1; }
          continue;
        }
        /* A newline ends a statement unless the statement can't end there
         * or the next line can't begin one: then it's whitespace. */
        if (ts.n >= 2 && ts.t[ts.n - 1].kind == tkNewline &&
            (ends_open(&ts.t[ts.n - 2]) || starts_closed(src, n, i)))
          ts.n--;
        at_line_start = 0;
      }
    }
    char ch = src[i];
    if (ch == '\n') {
      if (depth == 0) EMIT(tkNewline, "", 0);
      at_line_start = 1;
      i++; line++; col = 1;
    } else if (ch == ' ' || ch == '\t' || ch == '\r') {
      i++; col++;
    } else if (ch == '#') {
      while (i < n && src[i] != '\n') i++;
    } else if (is_digit(ch) || (ch == '.' && i + 1 < n && is_digit(src[i + 1]))) {
      long st = i;
      while (i < n && (is_digit(src[i]) || src[i] == '.')) { i++; col++; }
      /* An exponent, only when digits follow: `2e` stays a number and a name. */
      if (i < n && (src[i] == 'e' || src[i] == 'E')) {
        long j = i + 1;
        if (j < n && (src[j] == '+' || src[j] == '-')) j++;
        if (j < n && is_digit(src[j])) {
          while (i < j) { i++; col++; }
          while (i < n && is_digit(src[i])) { i++; col++; }
        }
      }
      const char* s = dup(c->a, src + st, i - st);
      EMIT(tkNum, s, parse_float(c, s));
    } else if (ch == '.') {
      fail_at(c, "unexpected '.' at %d:%d", line, col);
    } else if (is_alpha(ch) || ch == '_') {
      long st = i;
      while (i < n && is_word(src[i])) { i++; col++; }
      const char* s = dup(c->a, src + st, i - st);
      EMIT(is_keyword(s) ? tkKeyword : tkIdent, s, 0);
    } else if (ch == '"') {
      /* A string: an import path, or text for a compile-time builtin. No
       * escapes; it ends at the next `"` on the same line. */
      int sl = line, sc = col;
      i++; col++;
      long st = i;
      while (i < n && src[i] != '"' && src[i] != '\n') { i++; col++; }
      if (i >= n || src[i] != '"') fail_at(c, "unterminated string starting at %d:%d", sl, sc);
      const char* s = dup(c->a, src + st, i - st);
      i++; col++;
      add_tok(&ts, (Token){tkStr, s, 0, sl, sc});
    } else if (ch == '(') { EMIT(tkLParen, "", 0); i++; col++; depth++; }
    else if (ch == ')') { EMIT(tkRParen, "", 0); i++; col++; if (depth > 0) depth--; }
    else if (ch == '[') { EMIT(tkLBracket, "", 0); i++; col++; depth++; }
    else if (ch == ']') { EMIT(tkRBracket, "", 0); i++; col++; if (depth > 0) depth--; }
    else if (ch == ',') { EMIT(tkComma, "", 0); i++; col++; }
    else if (ch == ':') { EMIT(tkColon, "", 0); i++; col++; }
    else if (ch == ';') { EMIT(tkSemi, "", 0); i++; col++; }
    else if (ch == '|') {
      if (i + 1 < n && src[i + 1] == '>') { EMIT(tkOp, "|>", 0); i += 2; col += 2; }
      else fail_at(c, "unexpected '|' at %d:%d", line, col);
    } else if (ch == '$') {
      /* `$name`: state, one token, the sigil hugging its name. */
      int dl = line, dc = col;
      i++; col++;
      if (i >= n || !(is_alpha(src[i]) || src[i] == '_'))
        fail_at(c, "`$` must be followed immediately by an identifier (no space) at %d:%d", dl, dc);
      long st = i;
      while (i < n && is_word(src[i])) { i++; col++; }
      add_tok(&ts, (Token){tkStateIdent, dup(c->a, src + st, i - st), 0, dl, dc});
    } else if (ch == '=') {
      if (i + 1 < n && src[i + 1] == '>') { EMIT(tkOp, "=>", 0); i += 2; col += 2; }
      else if (i + 1 < n && src[i + 1] == '=') { EMIT(tkOp, "==", 0); i += 2; col += 2; }
      else { EMIT(tkAssign, "", 0); i++; col++; }
    } else if (ch == '!') {
      if (i + 1 < n && src[i + 1] == '=') { EMIT(tkOp, "!=", 0); i += 2; col += 2; }
      else fail_at(c, "unexpected '!' at %d:%d", line, col);
    } else if (ch == '<') {
      if (i + 1 < n && src[i + 1] == '=') { EMIT(tkOp, "<=", 0); i += 2; col += 2; }
      else { EMIT(tkOp, "<", 0); i++; col++; }
    } else if (ch == '>') {
      if (i + 1 < n && src[i + 1] == '=') { EMIT(tkOp, ">=", 0); i += 2; col += 2; }
      else { EMIT(tkOp, ">", 0); i++; col++; }
    } else if (ch == '+' || ch == '-' || ch == '*' || ch == '/') {
      if (i + 1 < n && src[i + 1] == '=') {
        buf[0] = ch; buf[1] = '=';
        EMIT(tkAugAssign, dup(c->a, buf, 2), 0); i += 2; col += 2;
      } else {
        buf[0] = ch;
        EMIT(tkOp, dup(c->a, buf, 1), 0); i++; col++;
      }
    } else if (ch == '%') {
      fail_at(c, "'%%' is spelled `mod` in aither (`a mod b`) at %d:%d", line, col);
    } else {
      fail_at(c, "unexpected char '%c' at %d:%d", ch, line, col);
    }
  }
  EMIT(tkEof, "", 0);
#undef EMIT
  return ts;
}

/* ------------------------------------------------------------------ parser */

/* State names seen in a scope: the first `$x = ...` there declares, the
 * rest assign. Top level and play bodies share one; defs and lambdas
 * push their own. */
typedef struct { const char** names; int n, cap; } Scope;

typedef struct {
  Ctx* c;
  Toks ts;
  int idx;
  Scope scopes[256];
  int nscopes;
  int body_depth;        /* def/play bodies we're in: `import` only at 0 */
  Vec top;               /* the program's statements so far */
  Vec extra;             /* bindings a `let [a, b] =` adds after its own */
} P;

static Token peek_at(P* p, int off) {
  if (p->idx + off < p->ts.n) return p->ts.t[p->idx + off];
  return (Token){tkEof, "", 0, 0, 0};
}
static Token peek(P* p) { return peek_at(p, 0); }
static Token advance(P* p) { return p->ts.t[p->idx++]; }

static void fail(P* p, const char* msg) {
  Token t = peek(p);
  fail_at(p->c, "%s at %d:%d (got %s '%s')", msg, t.line, t.col, tok_names[t.kind], t.str);
}

static Token expect(P* p, TokKind k, const char* what) {
  if (peek(p).kind != k) {
    char m[200];
    snprintf(m, sizeof m, "expected %s", what ? what : tok_names[k]);
    fail(p, m);
  }
  return advance(p);
}

static int match(P* p, TokKind k) { if (peek(p).kind == k) { advance(p); return 1; } return 0; }
static int is_op(P* p, const char* op) { Token t = peek(p); return t.kind == tkOp && eq(t.str, op); }
static int is_kw(P* p, const char* kw) { Token t = peek(p); return t.kind == tkKeyword && eq(t.str, kw); }
static int match_op(P* p, const char* op) { if (is_op(p, op)) { advance(p); return 1; } return 0; }
static int match_kw(P* p, const char* kw) { if (is_kw(p, kw)) { advance(p); return 1; } return 0; }

static void push_scope(P* p) {
  if (p->nscopes == 256) fail(p, "nested too deep");
  p->scopes[p->nscopes++] = (Scope){0};
}
static void pop_scope(P* p) { if (p->nscopes > 0) free(p->scopes[--p->nscopes].names); }
static int seen(P* p, const char* name) {
  if (p->nscopes == 0) return 0;
  Scope* s = &p->scopes[p->nscopes - 1];
  for (int i = 0; i < s->n; i++) if (eq(s->names[i], name)) return 1;
  return 0;
}
static void mark_seen(P* p, const char* name) {
  if (p->nscopes == 0) push_scope(p);
  Scope* s = &p->scopes[p->nscopes - 1];
  if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->names = realloc(s->names, s->cap * sizeof(char*)); }
  s->names[s->n++] = name;
}

static void skip_newlines(P* p) { while (peek(p).kind == tkNewline || peek(p).kind == tkSemi) advance(p); }

static Node* node(P* p, NodeKind k, int line) {
  Node* n = alloc(p->c->a, sizeof(Node));
  n->kind = k; n->line = line; n->str = ""; n->source = "";
  return n;
}
static Node* node1(P* p, NodeKind k, const char* str, Node* kid, int line) {
  Node* n = node(p, k, line);
  n->str = str;
  n->kids = alloc(p->c->a, sizeof(Node*));
  n->kids[0] = kid; n->nkids = 1;
  return n;
}
static Node* node2(P* p, NodeKind k, const char* str, Node* a, Node* b, int line) {
  Node* n = node(p, k, line);
  n->str = str;
  n->kids = alloc(p->c->a, 2 * sizeof(Node*));
  n->kids[0] = a; n->kids[1] = b; n->nkids = 2;
  return n;
}

static Node* parse_expr(P* p);

/* After `let`: `name = expr`, or `[a, b] = expr`, which names a pair's
 * two sides: a hidden let of the pair, and `a`, `b` as its [0] and [1]
 * (queued in p->extra; `_` names a side not wanted). */
static Node* parse_let_rest(P* p, int line) {
  if (peek(p).kind != tkLBracket) {
    const char* name = expect(p, tkIdent, "binding name").str;
    expect(p, tkAssign, NULL);
    return node1(p, nkLet, name, parse_expr(p), line);
  }
  advance(p);
  const char* names[2];
  int n = 0;
  for (;;) {
    const char* nm = expect(p, tkIdent, "a name for a side").str;
    if (n == 2) fail(p, "let [a, b] names a pair's two sides: two names");
    names[n++] = nm;
    if (peek(p).kind != tkComma) break;
    advance(p);
  }
  expect(p, tkRBracket, NULL);
  if (n != 2) fail(p, "let [a, b] names a pair's two sides: two names");
  expect(p, tkAssign, NULL);
  char buf[256];
  snprintf(buf, sizeof buf, "__pair_%s_%s_%d", names[0], names[1], line);
  const char* tmp = dup(p->c->a, buf, strlen(buf));
  Node* whole = node1(p, nkLet, tmp, parse_expr(p), line);
  for (int k = 0; k < 2; k++) {
    if (eq(names[k], "_")) continue;
    Node* id = node(p, nkIdent, line);
    id->str = tmp;
    Node* ix = node(p, nkNum, line);
    ix->num = k;
    push(&p->extra, node1(p, nkLet, names[k], node2(p, nkIdx, "", id, ix, line), line));
  }
  return whole;
}

/* Move the bindings a `let [a, b] =` queued onto `out`. */
static void drain_extra(P* p, Vec* out) {
  for (int i = 0; i < p->extra.n; i++) push(out, p->extra.v[i]);
  p->extra.n = 0;
}
static Node* node3(P* p, NodeKind k, Node* a, Node* b, Node* c3, int line) {
  Node* n = node(p, k, line);
  n->kids = alloc(p->c->a, 3 * sizeof(Node*));
  n->kids[0] = a; n->kids[1] = b; n->kids[2] = c3; n->nkids = 3;
  return n;
}
static void set_kids(P* p, Node* n, Vec* v) { n->nkids = v->n; n->kids = (Node**)keep(p->c->a, v); }

static Node* parse_expr(P* p);
static Node* parse_stmt(P* p);

static void parse_arg_list(P* p, Vec* out) {
  expect(p, tkLParen, NULL);
  if (peek(p).kind != tkRParen) {
    for (;;) {
      /* `name = expr`: a named argument. Inside parentheses it can't be an
       * assignment. */
      if (peek(p).kind == tkIdent && p->idx + 1 < p->ts.n && p->ts.t[p->idx + 1].kind == tkAssign) {
        Token name = advance(p);
        advance(p);
        push(out, node1(p, nkNamedArg, name.str, parse_expr(p), name.line));
      } else {
        push(out, parse_expr(p));
      }
      if (!match(p, tkComma)) break;
    }
  }
  expect(p, tkRParen, NULL);
}

static Node* parse_array_lit(P* p) {
  int line = peek(p).line;
  expect(p, tkLBracket, NULL);
  Vec items = {0};
  if (peek(p).kind != tkRBracket) {
    for (;;) {
      push(&items, parse_expr(p));
      if (!match(p, tkComma)) break;
      if (peek(p).kind == tkRBracket) break;    /* a trailing comma */
    }
  }
  expect(p, tkRBracket, NULL);
  Node* n = node(p, nkArr, line);
  set_kids(p, n, &items);
  return n;
}

static Node* parse_if(P* p) {
  int line = peek(p).line;
  expect(p, tkKeyword, NULL);                  /* if */
  Node* cond = parse_expr(p);
  if (!match_kw(p, "then")) fail(p, "expected 'then'");
  Node* thn = parse_expr(p);
  /* An `else` may sit on the next line. */
  int save = p->idx;
  while (peek(p).kind == tkNewline) advance(p);
  if (is_kw(p, "else")) {
    advance(p);
    Node* els = parse_expr(p);
    return node3(p, nkIf, cond, thn, els, line);
  }
  p->idx = save;
  return node3(p, nkIf, cond, thn, NULL, line);
}

/* A lambda's body, past the `=>`: lets and $state in any order, ended by
 * an expression (or, for `each`, by the closing paren). */
static Node* parse_lambda_body(P* p, int line, Vec* params) {
  push_scope(p);
  Vec stmts = {0};
  Node* final = NULL;
  for (;;) {
    if (is_kw(p, "let")) {
      int ll = peek(p).line;
      advance(p);
      push(&stmts, parse_let_rest(p, ll));
      drain_extra(p, &stmts);
      while (peek(p).kind == tkSemi) advance(p);
    } else if (peek(p).kind == tkStateIdent && p->idx + 1 < p->ts.n && p->ts.t[p->idx + 1].kind == tkAssign) {
      int sl = peek(p).line;
      const char* name = advance(p).str;
      expect(p, tkAssign, NULL);
      Node* rhs = parse_expr(p);
      NodeKind k = seen(p, name) ? nkAssign : nkVar;
      if (k == nkVar) mark_seen(p, name);
      push(&stmts, node1(p, k, name, rhs, sl));
      while (peek(p).kind == tkSemi) advance(p);
    } else if (peek(p).kind == tkRParen && stmts.n > 0) {
      /* Statements only (an `each` body): the value is 0. */
      final = node(p, nkNum, line);
      break;
    } else {
      /* An element store or the final expression: only the left side
       * tells which. */
      int sl = peek(p).line;
      Node* lhs = parse_expr(p);
      Token nx = peek(p);
      if ((nx.kind == tkAssign || nx.kind == tkAugAssign) && lhs->kind == nkIdx) {
        advance(p);
        Node* rhs = parse_expr(p);
        if (nx.kind == tkAugAssign) rhs = node2(p, nkBinOp, dup(p->c->a, nx.str, 1), lhs, rhs, sl);
        push(&stmts, node3(p, nkIdxAssign, lhs->kids[0], lhs->kids[1], rhs, sl));
        while (peek(p).kind == tkSemi) advance(p);
      } else if (nx.kind == tkAssign || nx.kind == tkAugAssign) {
        fail(p, "only `$name = ...` or `name[i] = ...` can be assigned here");
      } else {
        final = lhs;
        break;
      }
    }
  }
  pop_scope(p);
  Node* body;
  if (stmts.n == 0) { free(stmts.v); body = final; }
  else {
    push(&stmts, final);
    body = node(p, nkBlock, line);
    set_kids(p, body, &stmts);
  }
  Node* lam = node1(p, nkLambda, "", body, line);
  lam->nparams = params->n;
  lam->params = (const char**)keep(p->c->a, params);
  return lam;
}

/* `(a, b) => body`, or NULL (and nothing consumed) when it isn't one. */
static Node* try_paren_lambda(P* p) {
  int saved = p->idx;
  int line = peek(p).line;
  advance(p);                                   /* ( */
  if (peek(p).kind != tkIdent) { p->idx = saved; return NULL; }
  Vec params = {0};
  push(&params, (void*)advance(p).str);
  while (peek(p).kind == tkComma) {
    advance(p);
    if (peek(p).kind != tkIdent) { free(params.v); p->idx = saved; return NULL; }
    push(&params, (void*)advance(p).str);
  }
  if (peek(p).kind != tkRParen ||
      p->idx + 1 >= p->ts.n || p->ts.t[p->idx + 1].kind != tkOp || !eq(p->ts.t[p->idx + 1].str, "=>")) {
    free(params.v); p->idx = saved; return NULL;
  }
  advance(p); advance(p);                       /* ) => */
  return parse_lambda_body(p, line, &params);
}

/* Whether the next token is on the line of the one before it. A call's
 * `(` and an index's `[` must be: inside brackets newlines are dropped,
 * and a lambda body's next line starting with `[a, b]` is a pair, not an
 * index into the line above. */
static int same_line(P* p) {
  return p->idx > 0 && peek(p).line == p->ts.t[p->idx - 1].line;
}

/* `-` on a number is a negative number, so `[0, -0.5, 1]` is a table. */
static Node* negate(P* p, Node* kid, int line) {
  if (kid->kind == nkNum) { Node* n = node(p, nkNum, line); n->num = -kid->num; return n; }
  return node1(p, nkUnary, "-", kid, line);
}

static Node* parse_primary(P* p) {
  Token t = peek(p);
  switch (t.kind) {
  case tkNum: { advance(p); Node* n = node(p, nkNum, t.line); n->num = t.num; return n; }
  case tkStr: { advance(p); Node* n = node(p, nkStr, t.line); n->str = t.str; return n; }
  case tkLParen: {
    Node* lam = try_paren_lambda(p);
    if (lam) return lam;
    advance(p);
    Node* e = parse_expr(p);
    expect(p, tkRParen, NULL);
    return e;
  }
  case tkLBracket: return parse_array_lit(p);
  case tkKeyword:
    if (eq(t.str, "if")) return parse_if(p);
    if (eq(t.str, "not")) { advance(p); return node1(p, nkUnary, "not", parse_primary(p), t.line); }
    { char m[200]; snprintf(m, sizeof m, "unexpected keyword '%s'", t.str); fail(p, m); }
    return NULL;
  case tkIdent:
    advance(p);
    if (peek(p).kind == tkLParen && same_line(p)) {
      Vec args = {0};
      parse_arg_list(p, &args);
      Node* n = node(p, nkCall, t.line);
      n->str = t.str;
      set_kids(p, n, &args);
      return n;
    }
    if (is_op(p, "=>")) {                       /* `n => body` */
      advance(p);
      Vec params = {0};
      push(&params, (void*)t.str);
      return parse_lambda_body(p, t.line, &params);
    }
    { Node* n = node(p, nkIdent, t.line); n->str = t.str; return n; }
  case tkStateIdent: {
    /* `$x` reads the state named x: a plain name to the compiler; the `$`
     * is for the reader. */
    advance(p);
    Node* n = node(p, nkIdent, t.line);
    n->str = t.str;
    return n;
  }
  case tkOp:
    if (eq(t.str, "-")) { advance(p); return negate(p, parse_primary(p), t.line); }
    { char m[200]; snprintf(m, sizeof m, "unexpected operator '%s'", t.str); fail(p, m); }
    return NULL;
  default:
    fail(p, "unexpected token");
    return NULL;
  }
}

static Node* parse_postfix(P* p) {
  Node* n = parse_primary(p);
  while (peek(p).kind == tkLBracket && same_line(p)) {
    int line = peek(p).line;
    advance(p);
    Node* idx = parse_expr(p);
    expect(p, tkRBracket, NULL);
    n = node2(p, nkIdx, "", n, idx, line);
  }
  return n;
}

static Node* parse_unary(P* p) {
  Token t = peek(p);
  if (t.kind == tkOp && eq(t.str, "-")) { advance(p); return negate(p, parse_unary(p), t.line); }
  if (t.kind == tkKeyword && eq(t.str, "not")) { advance(p); return node1(p, nkUnary, "not", parse_unary(p), t.line); }
  return parse_postfix(p);
}

static Node* parse_mul(P* p) {
  Node* n = parse_unary(p);
  for (;;) {
    Token t = peek(p);
    const char* op;
    if (t.kind == tkOp && (eq(t.str, "*") || eq(t.str, "/"))) op = t.str;
    else if (t.kind == tkKeyword && eq(t.str, "mod")) op = "mod";
    else break;
    advance(p);
    n = node2(p, nkBinOp, op, n, parse_unary(p), t.line);
  }
  return n;
}

static Node* parse_add(P* p) {
  Node* n = parse_mul(p);
  for (;;) {
    Token t = peek(p);
    if (t.kind == tkOp && (eq(t.str, "+") || eq(t.str, "-"))) {
      advance(p);
      n = node2(p, nkBinOp, t.str, n, parse_mul(p), t.line);
    } else break;
  }
  return n;
}

static Node* parse_cmp(P* p) {
  Node* n = parse_add(p);
  Token t = peek(p);
  if (t.kind == tkOp && (eq(t.str, "==") || eq(t.str, "!=") || eq(t.str, "<") ||
                         eq(t.str, ">") || eq(t.str, "<=") || eq(t.str, ">="))) {
    advance(p);
    n = node2(p, nkBinOp, t.str, n, parse_add(p), t.line);
  }
  return n;
}

static Node* parse_and(P* p) {
  Node* n = parse_cmp(p);
  while (is_kw(p, "and")) {
    Token t = advance(p);
    n = node2(p, nkBinOp, "and", n, parse_cmp(p), t.line);
  }
  return n;
}

static Node* parse_or(P* p) {
  Node* n = parse_and(p);
  while (is_kw(p, "or")) {
    Token t = advance(p);
    n = node2(p, nkBinOp, "or", n, parse_and(p), t.line);
  }
  return n;
}

static Node* parse_pipe(P* p) {
  Node* n = parse_or(p);
  int saw = 0;
  while (match_op(p, "|>")) {
    saw = 1;
    int line = peek(p).line;
    Token id = expect(p, tkIdent, "function name after '|>'");
    Vec args = {0};
    push(&args, n);
    if (peek(p).kind == tkLParen) parse_arg_list(p, &args);
    n = node(p, nkCall, line);
    n->str = id.str;
    set_kids(p, n, &args);
  }
  /* `x |> f() * y` reads as if it meant `(x |> f()) * y`: say so. */
  if (saw) {
    Token t = peek(p);
    if (t.kind == tkOp && (eq(t.str, "+") || eq(t.str, "-") || eq(t.str, "*") || eq(t.str, "/")))
      fail(p, "pipe result used in arithmetic - wrap the chain in parens (e.g. `(x |> f()) * y`) or bind with `let`");
  }
  return n;
}

static Node* parse_expr(P* p) { return parse_pipe(p); }

/* A statement made for its effect, not its value: a block reads on past it. */
static int is_binding(const Node* n) {
  return n->kind == nkLet || n->kind == nkVar || n->kind == nkAssign || n->kind == nkIdxAssign ||
         (n->kind == nkCall && eq(n->str, "each"));
}

/* A def or play body: its bindings, ended by the expression that is its
 * value; neither indentation nor a terminator is needed. */
static Node* parse_body(P* p, int line, const char* what) {
  p->body_depth++;
  skip_newlines(p);
  Vec stmts = {0};
  for (;;) {
    if (peek(p).kind == tkEof || is_kw(p, "def") || is_kw(p, "play")) {
      char m[300];
      snprintf(m, sizeof m, "%s has no expression: a body is its lets and $state, "
               "ended by the expression that is its value", what);
      free(stmts.v);
      fail(p, m);
    }
    Node* s = parse_stmt(p);
    push(&stmts, s);
    drain_extra(p, &stmts);
    if (!is_binding(s)) break;
  }
  p->body_depth--;
  if (stmts.n == 1) { Node* s = stmts.v[0]; free(stmts.v); return s; }
  Node* b = node(p, nkBlock, line);
  set_kids(p, b, &stmts);
  return b;
}

static Node* parse_def(P* p) {
  int line = peek(p).line;
  expect(p, tkKeyword, NULL);                   /* def */
  int is_rec = match_kw(p, "rec");
  const char* name = expect(p, tkIdent, "function name").str;
  expect(p, tkLParen, NULL);
  Vec params = {0}, defaults = {0};
  if (peek(p).kind != tkRParen) {
    for (;;) {
      push(&params, (void*)expect(p, tkIdent, "parameter name").str);
      push(&defaults, match(p, tkAssign) ? parse_expr(p) : NULL);   /* `knob = expr` */
      if (!match(p, tkComma)) break;
    }
  }
  expect(p, tkRParen, NULL);
  expect(p, tkColon, NULL);
  /* A def's `$x` is per call site, its own scope. */
  push_scope(p);
  char what[200];
  snprintf(what, sizeof what, "def %s", name);
  Node* body = parse_body(p, line, what);
  pop_scope(p);
  Node* n = node1(p, nkDef, name, body, line);
  n->is_rec = is_rec;
  n->nparams = params.n;
  n->params = (const char**)keep(p->c->a, &params);
  n->defaults = (Node**)keep(p->c->a, &defaults);
  return n;
}

static Node* parse_play(P* p) {
  int line = peek(p).line;
  expect(p, tkKeyword, NULL);                   /* play */
  const char* name = expect(p, tkIdent, "part name").str;
  expect(p, tkColon, NULL);
  char what[200];
  snprintf(what, sizeof what, "play %s", name);
  Node* body = parse_body(p, line, what);
  Node* n = node1(p, nkPlay, name, body, line);
  return n;
}

static Node* parse_stmt(P* p) {
  Token t = peek(p);
  Node* n;
  if (t.kind == tkStateIdent && p->idx + 1 < p->ts.n && p->ts.t[p->idx + 1].kind == tkAssign) {
    /* `$name = expr`: a declaration on first sight in its scope, then an
     * assignment. */
    advance(p);
    expect(p, tkAssign, NULL);
    Node* v = parse_expr(p);
    NodeKind k = seen(p, t.str) ? nkAssign : nkVar;
    if (k == nkVar) mark_seen(p, t.str);
    n = node1(p, k, t.str, v, t.line);
  } else if (t.kind == tkKeyword && eq(t.str, "let")) {
    advance(p);
    n = parse_let_rest(p, t.line);
  } else if (t.kind == tkKeyword && eq(t.str, "def")) {
    n = parse_def(p);
  } else if (t.kind == tkKeyword && eq(t.str, "play")) {
    n = parse_play(p);
  } else if (t.kind == tkKeyword && eq(t.str, "import")) {
    if (p->body_depth > 0) fail(p, "import belongs at the top of the file, not inside a def or play");
    advance(p);
    n = node(p, nkImport, t.line);
    n->str = expect(p, tkStr, "a quoted path after import").str;
  } else {
    Node* lhs = parse_expr(p);
    Token nx = peek(p);
    if (nx.kind == tkAssign) {
      advance(p);
      Node* rhs = parse_expr(p);
      if (lhs->kind == nkIdent) n = node1(p, nkAssign, lhs->str, rhs, t.line);
      else if (lhs->kind == nkIdx) n = node3(p, nkIdxAssign, lhs->kids[0], lhs->kids[1], rhs, t.line);
      else { fail(p, "invalid assignment target"); return NULL; }
    } else if (nx.kind == tkAugAssign) {
      advance(p);
      Node* rhs = parse_expr(p);
      Node* combined = node2(p, nkBinOp, dup(p->c->a, nx.str, 1), lhs, rhs, t.line);
      if (lhs->kind == nkIdent) n = node1(p, nkAssign, lhs->str, combined, t.line);
      else if (lhs->kind == nkIdx) n = node3(p, nkIdxAssign, lhs->kids[0], lhs->kids[1], combined, t.line);
      else { fail(p, "invalid assignment target"); return NULL; }
    } else {
      n = lhs;
    }
  }
  while (peek(p).kind == tkNewline || peek(p).kind == tkSemi) advance(p);
  return n;
}

Node* parse_program(Arena* a, const char* source, char* err, int errlen) {
  Ctx c;
  memset(&c, 0, sizeof c);
  c.a = a; c.err = err; c.errlen = errlen;
  P* p = calloc(1, sizeof(P));
  /* After a jump only what lives in *p is trusted; a parse error may leak
   * a little scratch, never the arena. */
  if (setjmp(c.jump)) {
    free(p->top.v);
    for (int i = 0; i < p->nscopes; i++) free(p->scopes[i].names);
    free(p->ts.t); free(p->extra.v);
    free(p);
    return NULL;
  }
  p->c = &c;
  p->ts = tokenize(&c, source);
  /* Top level's state scope, shared with play bodies. */
  push_scope(p);
  int expr_line = 0;          /* the top-level expression's line, once seen */
  skip_newlines(p);
  while (peek(p).kind != tkEof) {
    Node* s = parse_stmt(p);
    if (s->kind != nkDef && s->kind != nkPlay && s->kind != nkImport && !is_binding(s)) {
      if (expr_line > 0)
        fail_at(&c, "a second top-level expression at line %d (the first is at line %d). "
                "A def or play body ends at its expression, so a body with a second "
                "expression spills out here; a file has one expression, its output",
                s->line, expr_line);
      expr_line = s->line;
    }
    push(&p->top, s);
    drain_extra(p, &p->top);
    skip_newlines(p);
  }
  pop_scope(p);
  Node* prog = alloc(a, sizeof(Node));
  prog->kind = nkBlock; prog->line = 1; prog->str = ""; prog->source = "";
  prog->nkids = p->top.n;
  prog->kids = (Node**)keep(a, &p->top);
  free(p->ts.t); free(p->extra.v);
  free(p);
  return prog;
}

int calls_input(const Node* n) {
  if (!n) return 0;
  if (n->kind == nkCall && eq(n->str, "input")) return 1;
  for (int i = 0; i < n->nkids; i++) if (calls_input(n->kids[i])) return 1;
  if (n->defaults) for (int i = 0; i < n->nparams; i++) if (calls_input(n->defaults[i])) return 1;
  return 0;
}

void set_source(Arena* a, Node* n, const char* source) {
  if (!n) return;
  n->source = source;
  for (int i = 0; i < n->nkids; i++) set_source(a, n->kids[i], source);
  (void)a;
}
