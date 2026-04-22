## Minimal Nim FFI to libtcc. Compile a C string in-memory, relocate
## into an executable buffer, fetch a function pointer, call it.
##
## Usage:
##   let s = tccNew()
##   s.setOutputType(OutputMemory)
##   s.addSymbol("n_lpf", cast[pointer](nLpf))
##   if s.compileString(csrc) < 0: raise ...
##   if s.relocate() < 0: raise ...
##   let fn = cast[TickFn](s.getSymbol("tick"))

{.passL: "-ltcc -ldl".}

type
  TccState* = distinct pointer
  ErrorFunc* = proc (opaque: pointer; msg: cstring) {.cdecl.}

const
  OutputMemory* = 1
  RelocateAuto* = cast[pointer](1)

proc tccNew*(): TccState {.importc: "tcc_new", cdecl.}
proc delete*(s: TccState) {.importc: "tcc_delete", cdecl.}
proc setOutputType*(s: TccState; ty: cint): cint
  {.importc: "tcc_set_output_type", cdecl.}
proc setOptions*(s: TccState; opts: cstring)
  {.importc: "tcc_set_options", cdecl.}
proc compileString*(s: TccState; src: cstring): cint
  {.importc: "tcc_compile_string", cdecl.}
proc addSymbol*(s: TccState; name: cstring; val: pointer): cint
  {.importc: "tcc_add_symbol", cdecl.}
proc getSymbol*(s: TccState; name: cstring): pointer
  {.importc: "tcc_get_symbol", cdecl.}
proc relocate*(s: TccState; ptrs: pointer = RelocateAuto): cint
  {.importc: "tcc_relocate", cdecl.}
proc setErrorFunc*(s: TccState; opaque: pointer; f: ErrorFunc)
  {.importc: "tcc_set_error_func", cdecl.}
proc addLibrary*(s: TccState; name: cstring): cint
  {.importc: "tcc_add_library", cdecl.}
