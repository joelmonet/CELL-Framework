# Original synthesis primitives — r38 source, uncompiled

These Apache-2.0 headers are reusable independently of the commercial Sound
Cells SYNTH module, UI, presets and rack. They use fixed voice storage and
caller-supplied sample rates. Prepare/import tables off the audio thread.

| Header | Responsibility |
| --- | --- |
| `synthesis.hpp` | polyBLEP analog morph, portamento, six-operator FM graph/voice, sample-counted cubic transition fade |
| `wavetable.hpp` | worker-side cycle import/preparation and immutable harmonic-limited playback |
| `physical_model.hpp` | 24-resonator voice with 11 physical models: strings, bars, membrane, tube, bell, wood, tines, sitar, reed and brass |
| `percussion.hpp` | 23 drum/percussion models with pitch, tone, noise, transient, metallic and FM layers |
| `latest_prepared_exchange.hpp` | bounded worker-to-audio publication, optional retained previous borrow for a caller-controlled crossfade |

For a plucked-string instrument:

```cpp
#include <cell/physical_model.hpp>
cell::dsp::PhysicalModelVoice voice;
cell::dsp::PhysicalModelSettings model;
model.model = 0;
model.position = .23f;
model.decay = 2.4f;       // seconds
model.damping = .45f;
voice.noteOn(123);       // deterministic exciter seed, on audio
// Once per sample; hold=false removes ongoing bow/breath excitation but lets
// stored resonances decay. Use an external amplitude envelope for note release.
const auto stereo = voice.next(220., 48000., model, true);
```

For a drum voice:

```cpp
#include <cell/percussion.hpp>
cell::dsp::PercussionVoice voice;
cell::dsp::PercussionSettings drum;
drum.model = 0;          // kick
drum.sweep = 30.f;      // starting pitch drop, semitones
drum.sweepTime = .032f; // seconds, exponential pitch-envelope time constant
drum.decay = .65f;     // seconds to -60 dB for the base tonal envelope; model material scales it
voice.noteOn(123);
const auto stereo = voice.next(48., 48000., drum);
// next() takes the actual fundamental frequency. Product-level tune/keyTrack
// fields can be translated by the caller. active() ends the natural one-shot.
```

All public settings must be finite; model indices are 0–10 / 0–22 respectively.
The product constrains its UI/automation ranges. Reset voices before reuse;
constructed/reset voices are silent until noteOn. Percussion one-shot/gated
behavior and velocity are product-level policies around the primitive voice.
A physical model's exciter/pickup position, modal damping, stiffness and body
shape can produce novel instruments as well as acoustic references. This is a
modal synthesis implementation, not a detailed simulation of every named
acoustic instrument or an emulation of a third-party synthesizer.

The physical models retain their first five IDs and append bell, wooden bar,
kalimba, sitar, reed and brass. Bell/bar/tine modes use different eigenfrequency
families and modal damping. Sitar adds a bounded bridge-feedback exciter. Reed
and brass use driven nonlinear reed/lip excitation into the modal bank, with
pressure and embouchure controls. They are physical-inspired sound designs;
self-oscillating wind-bore calibration and reference acoustic matching are not
implemented or claimed. Modes above output Nyquist are excluded from these
new exciters' feedback. `physicalModelUsesPressure/Bow` report the relevant
performance controls; `physicalModelCount` is 11.

The percussion list retains the first six IDs and appends rim, cowbell, clave,
shaker, ride, woodblock, tambourine and taiko, followed by sub/punch/FM kicks,
acoustic/brush/electro snares and floor/rack/concert toms. Kicks have distinct
pitch/partial/FM behavior; snares use different resonances, noise bandwidth and
attack; toms use different partial ratios and damping. These vary engine
behavior, not just a preset's tuning. `percussionModelCount` is 23. The six-voice
partial storage remains fixed; shaker/jingle granulation uses bounded sample
counters and envelope arithmetic rather than allocated sample buffers.

Physical coefficients update at most once per 16 samples; parameter changes
and glide can trigger that bounded work. Percussion timing/filter/drive
coefficients update when their settings change. Control ramps, envelopes,
voice stealing, arbitrary FM feedback and mode/table transitions still require
sound, alias and worst-case callback measurements in each product. High drive
is not oversampled in the percussion primitive; use the SDK oversampling
services if that policy is required. No transparent/alias-free claim is made.

For resource crossfades, use three or more LatestPreparedExchange slots,
adopt with audioUpdateRetainingPrevious(), read current and previous only
on audio, then call audioReleasePrevious() when the fade finishes. Release
never deletes the old resource: the serialized producer's reclaim() does.
With a retained previous borrow, pending worker updates coalesce and adoption
waits. Two slots can retain a previous borrow but cannot also queue a third
resource until that borrow is released. No public lifetime extends across
shutdown; stop both participants before destroying the exchange.

The commercial SYNTH uses 16 main voices and 16 bounded retiring voices with
2 ms fades for ordinary steals and mode changes, plus 3 ms control ramps and
1 ms expression ramps. Excess overlapping transitions reuse a retiring slot;
this overload case still needs listening qualification. No lookahead or
block-size queue was added to the synthesis primitives.

Deferred test sources: physical_percussion_tests, wavetable_synthesis_tests,
latest_prepared_exchange_tests and product-specific original_synth_tests / Live
editor tests. No compiler or runtime tests were run for r38; see VALIDATION.md.
