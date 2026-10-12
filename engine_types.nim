## Shared data types between engine.nim and cli_output.nim.
##
## Engine extracts state into these structs (under mtx); cli_output
## formats them for stdout. Putting them in a third module breaks the
## otherwise-circular `engine ↔ cli_output` import.

import std/[os, strutils]

const EnvBins* = 20            # sparkline slots; matches engine.nim's stats ring

# A single voice sample beyond ±8 (+18 dBFS) is never music — it's a
# patch that has blown up. The output tanh would turn it into a
# full-scale square wave, so the engine mutes the voice instead.
const RunawayLimit* = 8.0

proc envFloat*(name: string; default: float64): float64 =
  ## A numeric tuning knob from the environment, or `default`.
  let v = getEnv(name)
  if v.len == 0: return default
  try: parseFloat(v)
  except ValueError: default

# The per-part "share below" split in audit/try. 200 Hz is where small
# laptop speakers give up; studio monitors might want AITHER_LOW_HZ=40.
let LowSplitHz* = envFloat("AITHER_LOW_HZ", 200.0)

proc isRunaway*(l, r: float64): bool {.inline.} =
  abs(l) > RunawayLimit or abs(r) > RunawayLimit

# `play scope:` and `play beam:` draw the oscilloscope and are never
# heard, so the meters keep them out of the level comparison.
proc isScopePart*(name: string): bool {.inline.} =
  name == "scope" or name == "beam"

type
  VoiceState* = enum
    vsStopped, vsMuted, vsFadingOut, vsFadingIn, vsPlaying, vsRunaway

  PartState* = enum
    psSilent, psFadingOut, psFadingIn, psPlaying

  PartInfo* = object
    name*: string
    state*: PartState
    gain*: float64
    rms*, peak*: float64         # linear, the louder channel's level

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
    nanEvents*, runawayEvents*: int
    retrying*: bool              # runaway-muted, will fade back in

  # One play block's level over an offline render. `low` is the share
  # of its energy below 200 Hz, which laptop speakers can't play: a part
  # that is mostly `low` measures fine and still isn't heard.
  PartLevel* = object
    name*: string
    rms*, peak*, low*: float64
    gain*: float64               # its part gain (0 = muted)

  MidiStatus* = object
    portInfo*: string            # "" if never connected
    active*: bool
