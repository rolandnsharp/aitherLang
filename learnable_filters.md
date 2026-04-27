# Learnable filters in aither

A parked design note. Captures the thesis that aither could host
**learnable signal-processing systems**: filters trained from
input/output pairs, deployable as ordinary patches, hot-reloadable,
ride-able with knobs, owned by the composer.

This is not a roadmap. It's the design space staked out so we know
what we're building toward when the pieces start landing.

## The thesis

The substrate that aither already commits to — the DHO equation,
pair operations, mutable shared state, `f(state) → sample` —
**is also a substrate for filters whose parameters can be learned
from data**.

You give the system pairs `(input_sound, target_sound)`. It learns
a transformation that turns inputs of that class into outputs of
that class. The transformation IS a filter — but in the broadest
sense: an oscillator bank with learned `(ω, γ, β, drive-coupling)`
parameters, possibly with `cmul`-style cross-coupling, possibly
stacked into layers with attention for long-range context. Once
trained, **the filter is a regular aither voice**. You patch it
inline, you ride knobs over the learned parameters, you treat it
as one of your synthesis primitives.

This dissolves the boundary between "ML research" and "synthesis
tool." The training is the design phase; the deployed filter is
the instrument. The same language is both training surface and
deployment surface.

## What this unlocks

The interesting cases aren't *boring filters* (low-pass, EQ — we
have those analytically already). The interesting cases are the
ones that **can't be designed by hand**:

### Style transfer in audio

- Input: my voice. Output: a celebrity's voice.
- Input: my acoustic guitar. Output: a Stradivarius.
- Input: my drum kit. Output: a specific producer's drum sound.

Train a filter on a small corpus of paired data. The trained
filter is a vocal/instrument transform you can apply in real time
to any input of the same class. Commercial plugins do this with
heavy ML pipelines (Synthesizer V for voice, Neural DSP for amps);
aither's substrate is the lightweight version of the same machinery.

### Instrument modelling from recordings

- Input: a clean impulse or soft pluck.
- Output: the same input recorded through a Stradivarius / a
  vintage tube amp / a specific room.

The trained filter captures whatever makes the target distinctive
— body resonances, bowing nonlinearities, room acoustics, amp
tube saturation, microphone character. Applied to other inputs,
it imparts the character. This is **acoustic identity capture**:
a specific instrument's voice, a specific room's sound, a
specific player's tonal fingerprint, all reducible to a learned
filter that can be deployed as code.

### Reverb that learned a specific space

- Input: dry source.
- Output: same source recorded in Notre Dame.

Standard convolution reverb just learns the impulse response.
A learnable filter could capture the impulse response **plus
whatever nonlinear coupling the room has** — high-amplitude
saturation in the air at fortissimo, the way the room "breathes"
at low frequencies. The result is a richer simulator than
linear convolution can provide.

### Distortion characters that don't fit standard models

- Input: clean guitar.
- Output: a specific tube amp on a specific day, miked with a
  specific microphone in a specific room.

A parameterised nonlinearity learned from the pair, deployable
as a knob. Captures the *gestalt* of a specific signal chain
rather than just one component of it.

### The "make this sound like that" knob

- Pick any (input, output) pair where you want to capture the
  transformation.
- Learn it.
- Deploy as a knob in your patch.

This is the most general formulation. Anything you can demonstrate
with a before/after pair, you can train as a knob.

### Voice/instrument morphing as a continuous knob

Train multiple filters between input/output pairs at different
morph positions. Interpolate between them at runtime. You get a
continuous timbre-morph control that walks between learned
destinations.

This is what wavetable synthesis does for static spectra. The
learnable-filter version does it for *transformations* — morph
between "make it sound like A" and "make it sound like B" as a
single performance gesture.

### Noise reduction trained on YOUR room

- Input: clean speech + your specific room noise.
- Output: clean speech.

Most noise-reduction tools train on generic noise corpora and
work moderately well on everything. A filter trained on YOUR
specific room (the HVAC frequency, the streetcar that goes by,
the buzz from your specific power supply) works perfectly on
that room. Different filter for each environment.

### Source separation as a per-source filter

For each source in a mix, train a filter that isolates it.
Combine with the morphing knob and you get continuous re-mixing
of pre-recorded material — something current source-separation
tools (Spleeter, Demucs) do once at export time, but aither could
do continuously and live.

## Why aither is well-positioned

Three reasons this is more natural in aither than in a typical ML
stack.

### 1. Once trained, the filter is just a function

A normal ML model is a serialised weights file you load into a
runtime — frozen, opaque, not editable. An aither-shaped trained
filter is a small block of generated code: DHO calls with specific
frequency arrays, `cmul` mixing with specific weight matrices, a
few attention layers if needed. **You ship it as an aither patch.**
The composer can read it, modify it, use it as a starting point
for variations, fork it into a new instrument. It's not a black
box — it's the same kind of artefact aither composers already
work with, just generated by training rather than written by hand.

This matters more than it sounds. Most ML deployment treats the
model as inviolate; the only knobs are the input prompts. Aither's
trained-filter deployment treats the model as ordinary code — every
parameter is in scope, every primitive is named, and the composer
can intervene at any layer.

### 2. The training surface and the deployment surface are the same language

A typical pipeline is *"train in PyTorch → export to ONNX → load
in C++ runtime."* Each stage is a different language with different
semantics, and the gaps between them are where bugs and performance
losses live.

Aither could train AND deploy in the same codebase. The training
loop generates patch code; the deployment is the patch running.
**No serialisation gap. No runtime mismatch. No format conversion.**

A consequence worth flagging: hot-reload of trained filters
mid-performance becomes conceivable. Train a new filter on the
fly during a live set, swap it into a running patch, hear the
result without stopping. This isn't possible in any current ML
audio pipeline I know of.

### 3. The "knob" abstraction extends naturally

A trained filter has parameters. Some of those parameters are
**physics-critical** (a learned vocal-tract model has specific
formant frequencies you should NOT touch). Some are
**coupling-tolerant** (mixer weights, gains, blend coefficients).
The Causal Oscillator LM submission already proved this split is
structural, not accidental — it gets a quantization advantage from
treating physics params as float16-precision and coupling params
as int8-compressible.

In aither this means: trained filters expose the coupling-tolerant
params as performance knobs, freeze the physics-critical ones. The
performer gets meaningful control without breaking the model. The
patch comments document which is which. The whole thing reads as
a normal aither patch with one section labeled "learned weights —
do not touch" and another labeled "performance controls — go wild."

## What it would take

Concretely, to enable "train a filter from input/output pairs" as
an aither workflow:

### 1. Offline training mode

Aither already has `audit` for offline render — it takes a patch,
ticks it through N samples, returns the buffer. Extend this to a
training loop:

- Load N input/output pairs from disk.
- For each pair: render the patch with the input, compute loss
  against the target output, accumulate gradient.
- Update learnable parameters via the gradient.
- Iterate to convergence.

Loss function: spectral L2, waveform L2, multi-resolution STFT loss
(the standard for audio), or any combination. Picking the right
loss is most of the design work for any specific application;
having the *infrastructure* to swap losses is what aither needs to
provide.

### 2. Designate parameters as learnable

Syntax sketch:

```
let cutoff = learnable(800, 50, 5000)
                       initial, min, max
```

The training loop adjusts `cutoff` over many input/output pairs,
clipping to the bounds. At normal patch evaluation time, the
current value is just used as a constant. After training completes,
the patch is rewritten with the trained value substituted for the
`learnable(...)` call.

For physics-critical parameters that need precision preservation,
a `learnable_physics(...)` variant could mark them for high-precision
storage during quantization.

### 3. Gradient computation

Two viable approaches:

**a. Numerical differentiation (cheap, works everywhere).** For
each learnable parameter, perturb it by a small epsilon, render
the patch, compute the loss difference. Gradient = `(loss_perturbed
- loss_baseline) / epsilon`. Trivially correct, no autograd
infrastructure needed, slow at scale (one full render per
parameter per gradient step).

**b. Autograd via a Nim wrapper around the patch evaluation.** Each
arithmetic operation in the codegen path produces both a forward
value and a backward gradient. More complex but much faster at
scale. Could borrow the discipline (and possibly some kernels)
from resonance-ocaml's Nim+CUDA implementation.

**Recommendation: ship numerical first.** It's enough to validate
the workflow on small models (~100 learnable parameters). Once the
training loop works end-to-end and produces useful filters, *then*
write the autograd layer to scale up.

### 4. Save trained patches

The trained patch is just the original patch with the `learnable(...)`
calls replaced by their trained values. No special file format,
no separate weights file — **the trained filter IS aither code**.
A composer reading a trained patch sees:

```
def vocal_transform(input):
  let formant1 = 825.0     # learned: was learnable(700, 200, 2000)
  let formant2 = 1180.0    # learned: was learnable(1000, 500, 3000)
  let formant3 = 2730.0    # learned: was learnable(2500, 1500, 5000)
  let damping1 = 0.08      # learned: was learnable_physics(0.1, 0.01, 0.5)
  ...
  input |> dho_pair(formant1, damping1) |> ...
```

Same patch DSL, same readability, same modifiability. The only
difference is that some constants came from a training run instead
of from human intuition.

### 5. A small library of trainable patch templates

Most composers won't write training-template patches from scratch.
Provide canonical templates:

- **DHO bank** — N parallel damped oscillators, learnable `(ω, γ)`
  per oscillator, learnable mixing matrix. Good for harmonic /
  resonant transformations.
- **Convolution + nonlinearity stack** — learnable FIR taps,
  learnable nonlinearity coefficients (e.g. polynomial expansion).
  Good for amp / cabinet / saturation modelling.
- **Multi-band processor** — split into N frequency bands via
  learnable crossovers, apply per-band learnable transforms,
  recombine. Good for EQ / compression / dynamic processing.
- **Attention-augmented bank** — DHO bank front-end + attention
  layer for long-range context. Good when temporal dependencies
  matter (vocal tracts, room reverbs).

Each template is a normal aither patch with `learnable(...)` calls
in well-chosen places. Composers pick a template, point at training
data, get a trained filter.

## Relationship to resonance and the parameter-golf submission

The work is already partially done elsewhere in your codebase
ecosystem.

**Resonance-ocaml** has the autograd infrastructure for the DHO
equation in its Nim+CUDA implementation. The training loop, the
gradient kernels, the AdamW optimizer — all exist. They're targeting
sequence prediction, not filter learning, but the underlying
machinery is reusable.

**The Causal Oscillator LM submission** demonstrated that the
substrate produces real results when trained against a real
benchmark (BPB 1.34 on FineWeb). It also demonstrated the clean
param split (physics float16, coupling int8) that learnable filters
in aither would inherit. And it acknowledged the audio-domain
application explicitly: *"The same codebase achieves 26.4 dB
causal speech continuation from oscillator states."*

What aither would add:

- The **filter framing** (input → transformation → output) rather
  than the **prediction framing** (sequence → next token). Same
  substrate, different objective function.
- The **live-coding deployment surface** — trained filters
  hot-reload into a running performance, ride-able with MIDI,
  composable with other patches.
- The **interpretability story** — trained filters are aither code,
  not weight files. Every parameter has a name, every layer is
  visible, every transformation is editable.

The convergence picture this paints is: **resonance-ocaml is the
research arm**, the parameter-golf submission is the **proof of
concept that the substrate works in production**, and aither is the
**deployment surface** where trained filters become musical
instruments. Three projects, one substrate, complementary roles.

## The cybernetic frame — automating the patch design step

This is the cybernetic-synthesis tradition's natural completion.
The tradition spends hours in the studio finding sweet spots by
ear. Learnable filters automate part of that loop: instead of
turning knobs to find a sweet spot, you provide examples of what
you want and let gradient descent find the parameter values.

But — crucially — **the human is still the composer at every level
that matters**:

- The human designs the patch *shape* (which template to use, where
  to put nonlinearities, what feedback structure).
- The human chooses the training data (which examples represent
  the target).
- The human plays the deployed result (which knobs to ride, which
  sections to emphasise).

What gets automated is the *parameter-tuning* step — the part of
patch design that's not creative judgement but hill-climbing
against an objective. The cybernetic-synthesis tradition's
"patcher as gardener" model survives: you still cultivate the
system, you just have a power tool for one of the gardening tasks.

This is also what distinguishes aither's approach from "AI music
generation" tools. Spotify's algorithmic playlists are *dead
algorithm* in Roland Kayn's sense — optimisation toward a metric
with no human in the loop. Aither's learnable filters are
*cybernetic in the live sense* — the human chooses the shape, the
data, the deployment context, and the performance gestures. The
optimisation is a tool the human reaches for, not a replacement
for them.

## The economic angle

Worth flagging even though it's not the main reason to build this.

**Trained filters are a market.** Custom amp models, custom voice
transforms, custom space simulators are commercial products people
pay real money for:

- Neural DSP sells amp models for €119 each.
- Synthesizer V sells voice models for €40-150 each.
- iZotope sells noise-reduction with custom-trained profiles for
  €399.
- Acustica Audio sells "sampled" hardware emulations for €99-499.

In every case the customer pays for a **trained filter they don't
own and can't modify**, locked into a vendor's runtime, requiring
a subscription or activation server, deprecated when the vendor
decides to sunset it.

An aither workflow that lets a composer:

1. Capture audio from a piece of hardware they own,
2. Train a filter that imparts that hardware's character,
3. Deploy the filter as readable code they fully own,
4. Modify, fork, share, or sell it as they wish,

— is a meaningful alternative to that ecosystem. Not the primary
reason to build it (the primary reason is creative possibility),
but a real secondary value that argues for the work.

For aither specifically, this also gives the project a path to
**community-shared sound libraries**. Today the `sounds/` directory
is hand-written voices. With learnable filters, the library could
include user-contributed *trained* filters: "this is the filter
that makes your input sound like a 1972 Wurlitzer recorded through
a Leslie cabinet in a wood-floored room." Each filter is a few
hundred lines of aither code. The library scales the same way the
aither stdlib scales — by accumulation of useful artifacts.

## Open questions

- **Loss function**: spectral L2 on STFT magnitudes works for most
  audio applications but loses phase information. Multi-resolution
  STFT is the modern standard. For tasks where phase matters
  (phase-coherent reverbs, transient shapers), waveform L2 with
  proper alignment is needed. Need to figure out which losses
  serve which template.
- **Training data scale**: what's the minimum useful amount of
  paired data? Voice transforms need maybe 30 minutes of paired
  speech. Amp modelling needs maybe 5 minutes of paired audio.
  Reverb impulse responses need a single ~10-second sample. The
  scale varies enormously by application; templates should
  document expected data requirements.
- **Real-time vs offline-only**: some learnable filters will be
  too computationally expensive to run in real time even after
  training (e.g. large attention stacks). Should we also support
  offline-only filter application — render a long input through
  a heavy filter for hours of compute, get a finished output? The
  current `audit` model could be extended to do this.
- **MIDI integration**: learnable filters with knobs need a way
  to expose those knobs to MIDI cleanly. The current `midi_cc(N)`
  pattern works, but trained filters should probably document
  their performance-control surface explicitly so composers know
  which knobs are safe to map and which would break the model.
- **Numerical differentiation gradient noise**: at small epsilon
  values, floating-point error dominates the gradient signal.
  Need to pick epsilon adaptively per parameter scale. This is
  well-known in the optimization literature but worth flagging
  before implementation.
- **Training stability for cybernetic-feedback patches**: the
  patches with the most musical character (al-Mukabala, Tudor
  neural networks, anything with strong feedback) are the
  hardest to train because the loss landscape has many local
  minima and the system can diverge during gradient updates.
  Resonance-ocaml's Xavier-init / logit-clamping discipline is
  directly applicable. Worth importing as a documented training
  practice.

## What I'd build first

If we wanted to land this incrementally:

1. **`learnable(initial, min, max)` syntax** + parser support.
   Trivial.
2. **Numerical-differentiation training loop** as a CLI command:
   `./aither train <patch> <pairs_dir> [steps]`. ~200 lines.
3. **One trainable patch template**: a DHO bank with N learnable
   frequencies and a learnable output mixer. Simplest possible
   harmonic-transformation filter. ~30 lines of patch.
4. **One reference training task**: train the template to make
   white noise sound like a specific bell recording. Single audio
   file, no need for paired data. Validates the loop end-to-end.
5. **Documentation** — extend COMPOSING.md with a "learnable
   filters" section that walks through training the bell example.

That's maybe a weekend of work. Once it's running for one task,
the rest is templates and applications — building outward, not
re-architecting.

The ML-grade autograd, the GPU acceleration, the attention
templates, the production amp/voice models — all of that comes
later. Step 1 is "can a composer point aither at a target sound
and get a filter that produces it." If that works for the bell
example, we know the substrate is real for filters the same way
the parameter-golf submission proved it's real for sequences.
