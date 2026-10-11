# aither stream (aither.blah.dev)

The live stream's container: PulseAudio null sinks, the aither engine,
ffmpeg, icecast. `AITHER_SCOPE=1` runs a 5-channel engine (mix L/R plus
an oscilloscope beam X/Y/Z from a patch's `play scope:` / `play beam:`)
and adds `/scope.ogg` (all five, Ogg FLAC: lossless, because the beam
needs DC) beside the stereo `/live.mp3`.

- `Dockerfile`: builds on the `aither-stream` base image (nim, tcc,
  pulseaudio, ffmpeg, icecast) and rebuilds aither from `src/`.
- `entrypoint.sh`: starts everything; passwords are fresh per start.
- `icecast.xml`: template (`@SRCPW@`, `@ADMPW@`, `@PORT@`).
- `web/index.html`: the player. It decodes `/scope.ogg`, plays the mix,
  draws the beam as phosphor (WebGL2), and paints `/scene.paint`: a
  per-pixel aitherLang scene compiled to GLSL in the browser and
  recompiled when the file changes.
- `scenes/`: paint scenes; `rotate.sh` cycles them into `web/scene.paint`.

On lab: `~/aither-scope/{src,patches,web,scenes}`; the live container
mounts `patches` at `/work` and `web` as icecast's webroot, so scenes and
the page update without a rebuild.
