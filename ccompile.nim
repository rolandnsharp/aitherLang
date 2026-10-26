## The compiler in C (parser.c, imports.c, codegen.c), for the engine: a
## patch's source in, the C it compiles to and its state layout out, as
## codegen.generateFull gives them. Errors raise as the Nim path's do:
## ParseError, ImportError, or ValueError for a compile error.

import std/os
import parser, imports, codegen, dsp

{.compile: "parser.c".}
{.compile: "imports.c".}
{.compile: "codegen.c".}

const Dir = currentSourcePath().parentDir
const CodegenH = Dir / "codegen.h"
const ImportsH = Dir / "imports.h"

type
  CRegion {.importc: "Region", header: CodegenH, bycopy.} = object
    type_name, scope, owner: cstring
    per_type_idx, scope_idx: cint
    offset, size: clong
  CGenerated {.importc: "Generated", header: CodegenH, bycopy.} = object
    csrc: cstring
    var_names: pointer         # const char**
    nvars: cint
    part_names: pointer
    nparts: cint
    unheard: pointer
    nunheard: cint
    regions: ptr UncheckedArray[CRegion]
    nregions: cint

proc arena_new(): pointer {.importc, header: CodegenH.}
proc arena_free(a: pointer) {.importc, header: CodegenH.}
proc build_program(a: pointer; stdlibSrc, src, srcTag: cstring; seedSelf: cint;
                   kind: ptr cint; err: cstring; errlen: cint): pointer {.importc, header: ImportsH.}
proc generate_full(a: pointer; program: pointer; patchPath: cstring; sr: cdouble;
                   hoistOn: cint; o: ptr CGenerated; err: cstring; errlen: cint): cint {.importc, header: CodegenH.}
proc calls_input(n: pointer): cint {.importc, header: CodegenH.}

const BuildParse = 1     # imports.h's BUILD_PARSE, BUILD_IMPORT
const BuildImport = 2

type Compiled* = object
  csrc*: string
  varNames*, partNames*, unheard*: seq[string]
  regions*: seq[Region]
  callsInput*: bool

proc strings(p: pointer; n: cint): seq[string] =
  let a = cast[ptr UncheckedArray[cstring]](p)
  for i in 0 ..< int(n): result.add $a[i]

proc compileSource*(stdlibSrc, src, tag, patchPath: string; sr: float64;
                    seedSelf = false): Compiled =
  ## `tag`: the patch's path, or "<inline>". `seedSelf`: the patch counts
  ## as imported already (the engine's send does this).
  let a = arena_new()
  defer: arena_free(a)
  var err = newString(4096)
  var kind: cint
  let prog = build_program(a, stdlibSrc, src, tag, cint(seedSelf), addr kind, err.cstring, 4096)
  if prog == nil:
    let msg = $err.cstring
    if kind == BuildImport: raise newException(ImportError, msg)
    raise newException(ParseError, msg)
  result.callsInput = calls_input(prog) != 0
  var g: CGenerated
  let hoistOn = cint(getEnv("AITHER_HOIST", "1") != "0")
  if generate_full(a, prog, patchPath, sr, hoistOn, addr g, err.cstring, 4096) != 0:
    raise newException(ValueError, $err.cstring)
  result.csrc = $g.csrc
  result.varNames = strings(g.var_names, g.nvars)
  result.partNames = strings(g.part_names, g.nparts)
  result.unheard = strings(g.unheard, g.nunheard)
  for i in 0 ..< int(g.nregions):
    let r = g.regions[i]
    result.regions.add Region(typeName: $r.type_name, perTypeIdx: int(r.per_type_idx),
                              scope: $r.scope, scopeIdx: int(r.scope_idx), owner: $r.owner,
                              offset: int(r.offset), size: int(r.size))
