# aither stream (aither.blah.dev)

The live stream's container: PulseAudio null sinks, the aither engine,
ffmpeg, icecast. `AITHER_SCOPE=1` runs a 5-channel engine (mix L/R plus
an oscilloscope beam X/Y/Z from a patch's `play scope:` / `play beam:`)
and adds `/scope.ogg` (all five, Ogg FLAC: lossless, because the beam
needs DC) beside the stereo `/live.mp3`.

**This container runs the `scope` branch's engine (image
`aither-scope:next4`).** `main`'s engine no longer has `AITHER_SCOPE`,
the 5-channel output or a sound-card backend: it writes float pairs to
stdout (see [SCOPE.md](../SCOPE.md), [notes/pair-output-2026-10-15.md](../notes/pair-output-2026-10-15.md)).
Moving the stream to `main` means `aither | ffmpeg -f f32le -ac 2 -ar
48000 -i - …` for the sound and a second engine for the beam, and the
page's scope reading a 2-channel stream. Don't rebuild the live
container from `main` until that's done.

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
