## aither parser — tokenizer + recursive descent → AST

import std/[strutils]

type
  TokKind* = enum
    tkNum, tkIdent, tkKeyword, tkOp,
    tkLParen, tkRParen, tkLBracket, tkRBracket,
    tkComma, tkColon, tkSemi, tkAssign, tkAugAssign,
    tkNewline, tkIndent, tkDedent, tkEof

  Token* = object
    kind*: TokKind
    str*: string
    num*: float64
    line*, col*: int

  NodeKind* = enum
    nkNum, nkIdent, nkBinOp, nkUnary, nkCall, nkIf,
    nkVar, nkLet, nkDef, nkAssign, nkArr, nkIdx, nkIdxAssign, nkBlock,
    nkPlay

  Node* = ref object
    kind*: NodeKind
    str*: string                # name / op
    num*: float64               # literal value
    params*: seq[string]        # def parameters
    kids*: seq[Node]            # children (interpretation per kind)
    line*: int

  ParseError* = object of CatchableError

const Keywords = ["var", "let", "def", "play", "if", "then", "else",
                  "mod", "and", "or", "not"]

# ----------------------------------------------------------------- tokenizer

proc tokenize*(source: string): seq[Token] =
  var indents = @[0]
  var line = 1
  var col = 1
  var i = 0
  var atLineStart = true
  let n = source.len

  template emit(k: TokKind; s: string = ""; v: float64 = 0.0) =
    result.add Token(kind: k, str: s, num: v, line: line, col: col)

  while i < n:
    if atLineStart:
      var indent = 0
      while i < n and source[i] == ' ':
        indent += 1; i += 1; col += 1
      # Skip blank or comment-only lines without changing indent stack
      if i >= n or source[i] == '\n' or source[i] == '#':
        if i < n and source[i] == '#':
          while i < n and source[i] != '\n': i += 1
        if i < n and source[i] == '\n':
          i += 1; line += 1; col = 1
        continue
      # Indent change
      if indent > indents[^1]:
        indents.add indent
        emit(tkIndent)
      else:
        while indents.len > 1 and indent < indents[^1]:
          discard indents.pop()
          emit(tkDedent)
      atLineStart = false

    let c = source[i]
    case c
    of '\n':
      emit(tkNewline)
      i += 1; line += 1; col = 1; atLineStart = true
    of ' ', '\t', '\r':
      i += 1; col += 1
    of '#':
      while i < n and source[i] != '\n': i += 1
    of '0'..'9':
      var s = ""
      while i < n and (source[i].isDigit() or source[i] == '.'):
        s.add source[i]; i += 1; col += 1
      emit(tkNum, s, parseFloat(s))
    of '.':
      if i + 1 < n and source[i+1].isDigit():
        var s = ""
        while i < n and (source[i].isDigit() or source[i] == '.'):
          s.add source[i]; i += 1; col += 1
        emit(tkNum, s, parseFloat(s))
      else:
        raise newException(ParseError, "unexpected '.' at " & $line & ":" & $col)
    of 'a'..'z', 'A'..'Z', '_':
      var s = ""
      while i < n and (source[i].isAlphaAscii() or source[i].isDigit() or source[i] == '_'):
        s.add source[i]; i += 1; col += 1
      if s in Keywords: emit(tkKeyword, s) else: emit(tkIdent, s)
    of '(':  emit(tkLParen); i += 1; col += 1
    of ')':  emit(tkRParen); i += 1; col += 1
    of '[':  emit(tkLBracket); i += 1; col += 1
    of ']':  emit(tkRBracket); i += 1; col += 1
    of ',':  emit(tkComma); i += 1; col += 1
    of ':':  emit(tkColon); i += 1; col += 1
    of ';':  emit(tkSemi); i += 1; col += 1
    of '|':
      if i + 1 < n and source[i+1] == '>':
        emit(tkOp, "|>"); i += 2; col += 2
      else:
        raise newException(ParseError, "unexpected '|' at " & $line & ":" & $col)
    of '=':
      if i + 1 < n and source[i+1] == '=':
        emit(tkOp, "=="); i += 2; col += 2
      else:
        emit(tkAssign); i += 1; col += 1
    of '!':
      if i + 1 < n and source[i+1] == '=':
        emit(tkOp, "!="); i += 2; col += 2
      else:
        raise newException(ParseError, "unexpected '!' at " & $line & ":" & $col)
    of '<':
      if i + 1 < n and source[i+1] == '=':
        emit(tkOp, "<="); i += 2; col += 2
      else:
        emit(tkOp, "<"); i += 1; col += 1
    of '>':
      if i + 1 < n and source[i+1] == '=':
        emit(tkOp, ">="); i += 2; col += 2
      else:
        emit(tkOp, ">"); i += 1; col += 1
    of '+', '-', '*', '/':
      if i + 1 < n and source[i+1] == '=':
        emit(tkAugAssign, c & "="); i += 2; col += 2
      else:
        emit(tkOp, $c); i += 1; col += 1
    else:
      raise newException(ParseError,
        "unexpected char '" & $c & "' at " & $line & ":" & $col)

  while indents.len > 1:
    discard indents.pop()
    result.add Token(kind: tkDedent, line: line, col: col)
  result.add Token(kind: tkEof, line: line, col: col)

# ------------------------------------------------------------------- parser

type Parser = object
  toks: seq[Token]
  idx: int

proc peek(p: Parser; off: int = 0): Token {.inline.} =
  if p.idx + off < p.toks.len: p.toks[p.idx + off]
  else: Token(kind: tkEof)

proc advance(p: var Parser): Token {.inline.} =
  result = p.toks[p.idx]; inc p.idx

proc fail(p: Parser; msg: string) {.noreturn.} =
  let t = p.peek()
  raise newException(ParseError, msg & " at " & $t.line & ":" & $t.col &
                                 " (got " & $t.kind & " '" & t.str & "')")

proc expect(p: var Parser; k: TokKind; what: string = ""): Token =
  if p.peek().kind != k:
    p.fail("expected " & (if what.len > 0: what else: $k))
  result = p.advance()

proc match(p: var Parser; k: TokKind): bool =
  if p.peek().kind == k:
    discard p.advance(); return true
  return false

proc matchOp(p: var Parser; op: string): bool =
  let t = p.peek()
  if t.kind == tkOp and t.str == op:
    discard p.advance(); return true
  return false

proc matchKw(p: var Parser; kw: string): bool =
  let t = p.peek()
  if t.kind == tkKeyword and t.str == kw:
    discard p.advance(); return true
  return false

proc isKw(p: Parser; kw: string): bool =
  let t = p.peek()
  t.kind == tkKeyword and t.str == kw

proc skipNewlines(p: var Parser) =
  while p.peek().kind in {tkNewline, tkSemi}: discard p.advance()

proc node(k: NodeKind; line: int = 0): Node =
  Node(kind: k, line: line)

# Forward decls
proc parseExpr(p: var Parser): Node
proc parseStmt(p: var Parser): Node

proc parseArgList(p: var Parser): seq[Node] =
  discard p.expect(tkLParen)
  if p.peek().kind != tkRParen:
    while true:
      result.add p.parseExpr()
      if not p.match(tkComma): break
  discard p.expect(tkRParen)

proc parseArrayLit(p: var Parser): Node =
  let line = p.peek().line
  discard p.expect(tkLBracket)
  var items: seq[Node]
  if p.peek().kind != tkRBracket:
    while true:
      items.add p.parseExpr()
      if not p.match(tkComma): break
  discard p.expect(tkRBracket)
  Node(kind: nkArr, kids: items, line: line)

proc parseIfExpr(p: var Parser): Node =
  let line = p.peek().line
  discard p.expect(tkKeyword)               # 'if'
  let cond = p.parseExpr()
  if not p.matchKw("then"): p.fail("expected 'then'")
  let thn = p.parseExpr()
  # Look for an optional 'else' that may sit on the next line
  let save = p.idx
  while p.peek().kind in {tkNewline, tkIndent, tkDedent}: discard p.advance()
  if p.isKw("else"):
    discard p.advance()
    let els = p.parseExpr()
    return Node(kind: nkIf, kids: @[cond, thn, els], line: line)
  p.idx = save
  Node(kind: nkIf, kids: @[cond, thn, nil], line: line)

proc parsePrimary(p: var Parser): Node =
  let t = p.peek()
  case t.kind
  of tkNum:
    discard p.advance()
    return Node(kind: nkNum, num: t.num, line: t.line)
  of tkLParen:
    discard p.advance()
    let e = p.parseExpr()
    discard p.expect(tkRParen)
    return e
  of tkLBracket:
    return p.parseArrayLit()
  of tkKeyword:
    if t.str == "if": return p.parseIfExpr()
    if t.str == "not":
      discard p.advance()
      return Node(kind: nkUnary, str: "not", kids: @[p.parsePrimary()], line: t.line)
    p.fail("unexpected keyword '" & t.str & "'")
  of tkIdent:
    discard p.advance()
    if p.peek().kind == tkLParen:
      let args = p.parseArgList()
      return Node(kind: nkCall, str: t.str, kids: args, line: t.line)
    return Node(kind: nkIdent, str: t.str, line: t.line)
  of tkOp:
    if t.str == "-":
      discard p.advance()
      return Node(kind: nkUnary, str: "-", kids: @[p.parsePrimary()], line: t.line)
    p.fail("unexpected operator '" & t.str & "'")
  else:
    p.fail("unexpected token")

proc parsePostfix(p: var Parser): Node =
  var n = p.parsePrimary()
  while p.peek().kind == tkLBracket:
    let line = p.peek().line
    discard p.advance()
    let idx = p.parseExpr()
    discard p.expect(tkRBracket)
    n = Node(kind: nkIdx, kids: @[n, idx], line: line)
  n

proc parseUnary(p: var Parser): Node =
  let t = p.peek()
  if t.kind == tkOp and t.str == "-":
    discard p.advance()
    return Node(kind: nkUnary, str: "-", kids: @[p.parseUnary()], line: t.line)
  if t.kind == tkKeyword and t.str == "not":
    discard p.advance()
    return Node(kind: nkUnary, str: "not", kids: @[p.parseUnary()], line: t.line)
  p.parsePostfix()

proc parseMul(p: var Parser): Node =
  var n = p.parseUnary()
  while true:
    let t = p.peek()
    var op = ""
    if t.kind == tkOp and t.str in ["*", "/"]: op = t.str
    elif t.kind == tkKeyword and t.str == "mod": op = "mod"
    else: break
    discard p.advance()
    let rhs = p.parseUnary()
    n = Node(kind: nkBinOp, str: op, kids: @[n, rhs], line: t.line)
  n

proc parseAdd(p: var Parser): Node =
  var n = p.parseMul()
  while true:
    let t = p.peek()
    if t.kind == tkOp and t.str in ["+", "-"]:
      discard p.advance()
      let rhs = p.parseMul()
      n = Node(kind: nkBinOp, str: t.str, kids: @[n, rhs], line: t.line)
    else: break
  n

proc parseCmp(p: var Parser): Node =
  var n = p.parseAdd()
  let t = p.peek()
  if t.kind == tkOp and t.str in ["==", "!=", "<", ">", "<=", ">="]:
    discard p.advance()
    let rhs = p.parseAdd()
    n = Node(kind: nkBinOp, str: t.str, kids: @[n, rhs], line: t.line)
  n

proc parseAnd(p: var Parser): Node =
  var n = p.parseCmp()
  while p.isKw("and"):
    let t = p.peek(); discard p.advance()
    let rhs = p.parseCmp()
    n = Node(kind: nkBinOp, str: "and", kids: @[n, rhs], line: t.line)
  n

proc parseOr(p: var Parser): Node =
  var n = p.parseAnd()
  while p.isKw("or"):
    let t = p.peek(); discard p.advance()
    let rhs = p.parseAnd()
    n = Node(kind: nkBinOp, str: "or", kids: @[n, rhs], line: t.line)
  n

proc parsePipe(p: var Parser): Node =
  var n = p.parseOr()
  var sawPipe = false
  while p.matchOp("|>"):
    sawPipe = true
    # RHS must be a function call (or bare ident treated as call)
    let line = p.peek().line
    let id = p.expect(tkIdent, "function name after '|>'")
    var args: seq[Node] = @[n]
    if p.peek().kind == tkLParen:
      let extra = p.parseArgList()
      for a in extra: args.add a
    n = Node(kind: nkCall, str: id.str, kids: args, line: line)
  # If an arithmetic op follows a pipe chain, the user probably expected
  # `(x |> f()) * y` semantics. aither's pipe is low-precedence like
  # OCaml/Elixir/F#, so they need explicit parens or a `let` binding.
  if sawPipe:
    let t = p.peek()
    if t.kind == tkOp and t.str in ["+", "-", "*", "/"]:
      p.fail("pipe result used in arithmetic - wrap the chain in parens " &
             "(e.g. `(x |> f()) * y`) or bind with `let`")
  n

proc parseExpr(p: var Parser): Node = p.parsePipe()

proc parseDef(p: var Parser): Node =
  let line = p.peek().line
  discard p.expect(tkKeyword)               # 'def'
  let name = p.expect(tkIdent, "function name").str
  discard p.expect(tkLParen)
  var params: seq[string]
  if p.peek().kind != tkRParen:
    while true:
      params.add p.expect(tkIdent, "parameter name").str
      if not p.match(tkComma): break
  discard p.expect(tkRParen)
  discard p.expect(tkColon)

  var stmts: seq[Node]
  if p.peek().kind == tkNewline:
    discard p.advance()
    if p.peek().kind != tkIndent: p.fail("expected indented body after 'def'")
    discard p.advance()
    while p.peek().kind notin {tkDedent, tkEof}:
      stmts.add p.parseStmt()
    if p.peek().kind == tkDedent: discard p.advance()
  else:
    while true:
      stmts.add p.parseStmt()
      if p.peek().kind == tkSemi: discard p.advance()
      else: break

  let body = if stmts.len == 1: stmts[0]
             else: Node(kind: nkBlock, kids: stmts, line: line)
  Node(kind: nkDef, str: name, params: params, kids: @[body], line: line)

proc parsePlay(p: var Parser): Node =
  let line = p.peek().line
  discard p.expect(tkKeyword)                 # 'play'
  let name = p.expect(tkIdent, "part name").str
  # Modifiers (off, fade X, fade-in X, fade-out X) come after name, before ':'.
  # Parser captures them as a whitespace-joined string in `params[0]`; compiler
  # doesn't interpret yet. Phase 1 just tolerates them.
  var mods: seq[string]
  while p.peek().kind == tkIdent:
    mods.add p.advance().str
  discard p.expect(tkColon)

  var stmts: seq[Node]
  if p.peek().kind == tkNewline:
    discard p.advance()
    if p.peek().kind != tkIndent: p.fail("expected indented body after 'play'")
    discard p.advance()
    while p.peek().kind notin {tkDedent, tkEof}:
      stmts.add p.parseStmt()
    if p.peek().kind == tkDedent: discard p.advance()
  else:
    while true:
      stmts.add p.parseStmt()
      if p.peek().kind == tkSemi: discard p.advance()
      else: break

  let body = if stmts.len == 1: stmts[0]
             else: Node(kind: nkBlock, kids: stmts, line: line)
  Node(kind: nkPlay, str: name, params: mods, kids: @[body], line: line)

proc parseStmt(p: var Parser): Node =
  let t = p.peek()
  var n: Node
  if t.kind == tkKeyword and t.str == "var":
    discard p.advance()
    let name = p.expect(tkIdent).str
    discard p.expect(tkAssign)
    let v = p.parseExpr()
    n = Node(kind: nkVar, str: name, kids: @[v], line: t.line)
  elif t.kind == tkKeyword and t.str == "let":
    discard p.advance()
    let name = p.expect(tkIdent).str
    discard p.expect(tkAssign)
    let v = p.parseExpr()
    n = Node(kind: nkLet, str: name, kids: @[v], line: t.line)
  elif t.kind == tkKeyword and t.str == "def":
    n = p.parseDef()
  elif t.kind == tkKeyword and t.str == "play":
    n = p.parsePlay()
  else:
    let lhs = p.parseExpr()
    let nx = p.peek()
    if nx.kind == tkAssign:
      discard p.advance()
      let rhs = p.parseExpr()
      if lhs.kind == nkIdent:
        n = Node(kind: nkAssign, str: lhs.str, kids: @[rhs], line: t.line)
      elif lhs.kind == nkIdx:
        n = Node(kind: nkIdxAssign,
                 kids: @[lhs.kids[0], lhs.kids[1], rhs], line: t.line)
      else:
        p.fail("invalid assignment target")
    elif nx.kind == tkAugAssign:
      discard p.advance()
      let rhs = p.parseExpr()
      let op = $nx.str[0]      # '+', '-', '*', '/'
      if lhs.kind == nkIdent:
        let combined = Node(kind: nkBinOp, str: op,
                            kids: @[lhs, rhs], line: t.line)
        n = Node(kind: nkAssign, str: lhs.str,
                 kids: @[combined], line: t.line)
      elif lhs.kind == nkIdx:
        let combined = Node(kind: nkBinOp, str: op,
                            kids: @[lhs, rhs], line: t.line)
        n = Node(kind: nkIdxAssign,
                 kids: @[lhs.kids[0], lhs.kids[1], combined], line: t.line)
      else:
        p.fail("invalid assignment target")
    else:
      n = lhs
  # consume terminator
  while p.peek().kind in {tkNewline, tkSemi}: discard p.advance()
  n

proc parseProgram*(source: string): Node =
  let toks = tokenize(source)
  var p = Parser(toks: toks, idx: 0)
  var stmts: seq[Node]
  p.skipNewlines()
  while p.peek().kind != tkEof:
    stmts.add p.parseStmt()
    p.skipNewlines()
  Node(kind: nkBlock, kids: stmts, line: 1)
