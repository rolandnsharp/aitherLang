{.compile: "miniaudio_wrapper.c".}
{.passC: "-I.".}
{.passL: "-lpthread -lm -ldl".}

type AudioCallback* = proc(output: ptr UncheckedArray[cfloat],
                            frameCount: cuint,
                            userData: pointer) {.cdecl.}

proc aither_audio_init*(sr, ch, buf: cuint, cb: AudioCallback,
                         ud: pointer): cint {.importc, cdecl.}
proc aither_audio_start*(): cint {.importc, cdecl.}
proc aither_audio_stop*(): cint {.importc, cdecl.}
proc aither_audio_uninit*() {.importc, cdecl.}
