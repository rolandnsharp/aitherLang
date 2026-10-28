## The engine's data as the Nim tests see it (engine.h's snapshots,
## render.h's PartLevel), until the tests are C too
## (notes/c-port-2026-10-28.md). cli_output.nim hands them to C.

const EnvBins* = 20            # sparkline slots; engine.h's ENGINE_ENV_BINS

# A single voice sample beyond ±8 (+18 dBFS) is never music — it's a
# patch that has blown up (engine.c's RUNAWAY_LIMIT).
const RunawayLimit* = 8.0

proc isRunaway*(l, r: float64): bool {.inline.} =
  abs(l) > RunawayLimit or abs(r) > RunawayLimit

type
  VoiceState* = enum
    vsStopped, vsMuted, vsParked, vsFadingOut, vsFadingIn, vsPlaying, vsRunaway

  PartState* = enum
    psSilent, psFadingOut, psFadingIn, psPlaying, psParked

  PartInfo* = object
    name*: string
    state*: PartState
    gain*: float64
    rms*, peak*: float64         # linear, the louder channel's level
    unheard*: bool               # the final expression never reaches it

  VoiceInfo* = object
    name*: string
    state*: VoiceState
    gain*: float64
    parts*: seq[PartInfo]

  StatsSnapshot* = object
    rmsL*, rmsR*: float64        # linear; cli_output does dB conversion
    peakL*, peakR*: float64
    clips*: int
    envRing*: array[EnvBins, float32]
    envBinIdx*: int

  VoiceStats* = object
    name*: string                # "master" for the master mix
    isMaster*: bool
    state*: VoiceState           # ignored when isMaster
    gain*: float64               # ignored when isMaster
    stats*: StatsSnapshot

  ScopeQuery* = object
    found*: bool                 # false when a named voice didn't exist
    snapshots*: seq[VoiceStats]

  PartsQuery* = object
    voiceName*: string
    found*: bool                 # false when the voice didn't exist
    parts*: seq[PartInfo]

  # What `send` prints a second after the swap: did it land, how loud
  # is it, which parts are too quiet to hear, did a guard fire.
  CheckQuery* = object
    found*: bool
    voice*: VoiceStats
    master*: StatsSnapshot
    parts*: seq[PartInfo]
    rolledBack*: bool
    stuck*: bool                 # the guard fired for a recursion that didn't return
    nanEvents*, runawayEvents*: int
    retrying*: bool              # runaway-muted, will fade back in

  # One play block's level over an offline render. `low` is the share
  # of its energy below 200 Hz, which laptop speakers can't play: a part
  # that is mostly `low` measures fine and still isn't heard.
  PartLevel* = object
    name*: string
    rms*, peak*, low*: float64
    gain*: float64               # its part gain (0 = muted)
    unheard*: bool               # the final expression never reaches it

  MidiStatus* = object
    portInfo*: string            # "" if never connected
    active*: bool
