{.compile: "miniaudio_wrapper.c".}
{.passC: "-I.".}
{.passL: "-lpthread -lm -ldl".}

# `input` holds the captured frames for the period (interleaved like
# `output`), or nil when the device has no audio input.
type AudioCallback* = proc(output: ptr UncheckedArray[cfloat],
                            input: ptr UncheckedArray[cfloat],
                            frameCount: cuint,
                            userData: pointer) {.cdecl.}

# Returns 1 with audio input, 0 playback-only, -1 on failure.
proc aither_audio_init*(sr, ch, buf: cuint, cb: AudioCallback,
                         ud: pointer): cint {.importc, cdecl.}
proc aither_audio_start*(): cint {.importc, cdecl.}
proc aither_audio_stop*(): cint {.importc, cdecl.}
proc aither_audio_uninit*() {.importc, cdecl.}
