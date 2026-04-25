## Shared data types between engine.nim and cli_output.nim.
##
## Engine extracts state into these structs (under mtx); cli_output
## formats them for stdout. Putting them in a third module breaks the
## otherwise-circular `engine ↔ cli_output` import.

const EnvBins* = 20            # sparkline slots; matches engine.nim's stats ring

type
  VoiceState* = enum
    vsStopped, vsMuted, vsFadingOut, vsFadingIn, vsPlaying

  PartState* = enum
    psSilent, psFadingOut, psFadingIn, psPlaying

  PartInfo* = object
    name*: string
    state*: PartState
    gain*: float64

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

  MidiStatus* = object
    portInfo*: string            # "" if never connected
    active*: bool
