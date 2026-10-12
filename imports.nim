## `import "lib/voice.aither"`: splice a library file's defs and constant
## tables into the program, so instruments live in a library and a patch
## is just the arrangement. Paths are relative to the importing file;
## a file imported twice (or in a cycle) is included once. Library files
## may hold defs, constant `let`s (a number or a table of numbers) and
## further imports — plays, `$state` and sound belong in the patch.

import std/[os, sets, sequtils]
import parser

type ImportError* = object of CatchableError

proc isConstLet(n: Node): bool =
  n.kind == nkLet and (n.kids[0].kind == nkNum or
    (n.kids[0].kind == nkArr and n.kids[0].kids.len > 0 and
     n.kids[0].kids.allIt(it.kind == nkNum)))

proc expandImports*(ast: Node; fromPath: string; files: var seq[string];
                    seen: var HashSet[string]; asLibrary = false): Node =
  ## Replace every top-level `import` in `ast` (parsed from `fromPath`,
  ## "" for inline source) with the imported file's statements. `files`
  ## collects every file read, for watchers. Raises ImportError when a
  ## file is missing or holds more than a library may.
  let baseDir = if fromPath.len > 0: parentDir(fromPath) else: getCurrentDir()
  var kids: seq[Node]
  for s in ast.kids:
    if s.kind == nkImport:
      var path = s.str
      if not path.isAbsolute: path = baseDir / path
      path = path.normalizedPath
      let loc = (if fromPath.len > 0: fromPath else: "<inline>") & ":" & $s.line
      if not fileExists(path):
        raise newException(ImportError,
          "import: " & s.str & " not found (looked at " & path & ") " &
          "(" & loc & ")")
      let key = try: expandFilename(path) except OSError: path
      if key in seen: continue                 # once is enough; cycles end here
      seen.incl key
      files.add path
      var lib: Node
      try:
        lib = parseProgram(readFile(path))
      except ParseError as e:
        raise newException(ImportError, "in " & path & ": " & e.msg)
      setSource(lib, path)
      let expanded = expandImports(lib, path, files, seen, asLibrary = true)
      for k in expanded.kids: kids.add k
    else:
      if asLibrary and not (s.kind == nkDef or isConstLet(s)):
        raise newException(ImportError,
          "a library holds defs and constant tables only; this " &
          (case s.kind
           of nkPlay: "play"
           of nkVar, nkAssign, nkIdxAssign: "state"
           of nkLet: "let (its value isn't a constant)"
           else: "expression") &
          " belongs in the patch that imports it (" & fromPath & ":" &
          $s.line & ")")
      kids.add s
  Node(kind: nkBlock, kids: kids, line: ast.line, source: ast.source)

proc loadPatchFile*(path: string): tuple[ast: Node; files: seq[string]] =
  ## Parse a patch and splice its imports. `files` starts with the patch.
  var seen = initHashSet[string]()
  var files = @[path]
  let ast = parseProgram(readFile(path))
  setSource(ast, path)
  seen.incl(try: expandFilename(path) except OSError: path)
  (expandImports(ast, path, files, seen), files)

proc importedFiles*(path: string): seq[string] =
  ## The patch and every file it imports, for `aither watch`. Missing
  ## imports are left out rather than raised, so a watcher keeps going.
  try: loadPatchFile(path).files
  except CatchableError: @[path]
