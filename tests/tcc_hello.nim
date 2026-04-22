## Step 1: TCC FFI hello-world. Compile a trivial C function in memory,
## get its pointer, call it, verify the result.

import ../tcc

proc errHandler(opaque: pointer; msg: cstring) {.cdecl.} =
  stderr.writeLine "TCC: ", $msg

let src = """
int hello(void) { return 42; }
"""

let s = tccNew()
if cast[pointer](s) == nil: quit "tcc_new failed"
s.setErrorFunc(nil, errHandler)
if s.setOutputType(OutputMemory) != 0: quit "set_output_type failed"
if s.compileString(src) < 0: quit "compile failed"
if s.relocate() < 0: quit "relocate failed"

type HelloFn = proc (): cint {.cdecl.}
let fn = cast[HelloFn](s.getSymbol("hello"))
if fn == nil: quit "symbol not found"

let r = fn()
echo "hello() = ", r
doAssert r == 42, "expected 42, got " & $r
echo "step 1 ok"
s.delete()
