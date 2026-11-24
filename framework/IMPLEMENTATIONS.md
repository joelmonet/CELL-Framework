# Implementation examples and product patterns

The examples use public SDK APIs. Runnable targets are identified below;
product patterns describe what a consumer can build and the integration still
required. They do not imply the complete Sound Cells suite is in this archive.
Commands run from the extracted `Cell-Framework` source directory after
[building/installing the SDK](README.md#build-the-independent-sdk).

## 1. A small effect with host automation and recall

**Included runnable example:** `examples/sdk_gain`.

```sh
cmake -S examples/sdk_gain -B build-gain \
  -DCMAKE_PREFIX_PATH="$PWD/install-sdk"
cmake --build build-gain --parallel 2
./build-gain/cell_sdk_gain_smoke
```

The processor owns a prepared smoother, attaches the shared parameter bank and
applies gain to a planar audio block. The smoke executable checks that an event
at sample 32 leaves preceding samples unchanged, the ramp settles, and saved
state recalls correctly in mono. This is a starting point for a utility,
filter, saturation effect or meter; replace its DSP while retaining the
parameter and lifecycle structure.

Optional native editor: build the SDK with `CELL_SDK_UI=ON` and an installed
Blend2D, then configure this consumer with `CELL_GAIN_UI=ON`. Optional wrapper
options are `CELL_GAIN_CLAP_SDK`, `CELL_GAIN_VST3_SDK` and
`CELL_GAIN_VST2_SDK`, pointing to separately supplied official SDKs. See the
consumer CMake file for complete source setup. Host qualification remains a
separate step from the smoke test.

## 2. An original native interface with reusable visual effects

**Included runnable examples:** `examples/control_gallery` and
`examples/effects_gallery`, requiring an SDK with `Cell::UI`.

```sh
cmake -S examples/effects_gallery -B build-effects \
  -DCMAKE_PREFIX_PATH="$PWD/install-sdk"
cmake --build build-effects --parallel 2
./build-effects/cell_effects_gallery effects.png
./build-effects/cell_effects_benchmark
```

The gallery exports a standalone image; the benchmark compares cold/repeated
shadow drawing at different DPI scales. It reports effect-cache behavior,
retained bytes and scratch separately. Those results are not a claim about
whole-plugin CPU or another renderer.

**Possible implementation:** an analytical EQ with dynamic curves and a
stylized creative effect can share the same parameter bindings, gestures,
focus, numeric entry, popup menus and hover descriptions while changing the
visual materials. Use Canvas for curves, EffectCache for repeated effects and
CachedLayer for expensive static sections. Invalidate when content or DPI
changes. Keep audio displays in versioned snapshots rather than reading mutable
DSP internals. The SDK supplies these building blocks, not either complete EQ.

## 3. An HTML/CSS editor over the same C++ DSP

**Included runnable example:** `examples/webview_editor`.
Build/install the SDK with `CELL_SDK_WEBVIEW=ON` first. The initial smoke target
does not open a browser or require an installed browser runtime:

```sh
cmake -S examples/webview_editor -B build-web-example \
  -DCMAKE_PREFIX_PATH="$PWD/install-sdk"
cmake --build build-web-example --parallel 2
./build-web-example/cell_web_bridge_example
```

It sends a fractional physical gain value through begin/value/end JSON messages,
checks the native Engine value and host-edit sequence, and prints the allowed
parameter snapshot. It links the exported WebView component to check its
installed consumer dependencies even though this smoke path uses only the
parameter bridge.

For a real browser editor, also install `Cell::UI`, configure the example with
`CELL_WEB_EXAMPLE_WINDOW=ON`, and run `cell_web_editor_example`. On Windows pass
an absolute writable application-owned profile directory as its first argument.
The native window displays the self-contained page in `page.hpp`; it contains
no external assets. Native code polls messages, applies the explicit parameter
allow-list and returns current values. This demo displays controls; it does not
open an audio device or implement a complete format wrapper's WebView teardown.

**Possible implementation:** an interactive modulation matrix, instrument
library view or animated analysis panel can use HTML/CSS while the DSP, file
imports, state and host automation remain native. Send bounded parameter edits
and decimated display messages, not per-sample audio over JSON. You must handle
DAW shortcuts, editor focus, resize, unavailable runtimes and asynchronous
creation/unload. [WEBVIEW.md](WEBVIEW.md) gives the lifecycle and deployment
contract. Existing suite editors are not switched to WebViews by this addition.

## 4. A sample library browser and drum instrument

**Included APIs:** Core assets/jobs/read-ahead, native FileDialog (UI),
AudioFiles, `.trg`, observed `.tci` and `.gog` readers, WAVE export, MIDI device
profiles and InstrumentController hooks. The sample codecs do not allocate a
complete extracted library for each view.

A consumer can implement this workflow:

1. Open a native file/folder dialog and retain a user-selected default directory.
2. Inspect mappings and sample dimensions on a worker under explicit budgets.
3. Preview from prepared/cache pages, with cancellation when selection changes.
4. Admit the completed asset only if its instrument/pad and request generation
   still match; never let an old preview replace a newer selection.
5. Store the original path, sample identity and mapping. Use `.trg` to author a
   portable library or explicit WAVE export to convert a selected sample.

An observed embedded-audio TCI sample can be exported on a worker:

```cpp
#include <cell/tci.hpp>
#include <cell/wave_export.hpp>
auto source = std::make_shared<cell::tci::Source>(instrumentPath);
cell::tci::WaveView wave(source, sampleIndex);
cell::audiofiles::exportWaveView(wave, outputWavePath);
```

The concrete installed consumers in `tests/sdk_trg_consumer`,
`tests/sdk_pooled_file_consumer` and `tests/sdk_audio_files_consumer` are
executable API examples. They exercise their documented contracts; consumers
must create/provide the inputs their CMake/tests require. A complete drum
instrument still needs voices, envelopes, velocity/round-robin selection,
trigger detection, kit import policy and UI. TCI/GOG input support does not
provide those formats' writers or all instrument features. The KSP subset does
not load modern Kontakt libraries.

## 5. A MIDI utility, virtual controller or audio-to-MIDI effect

Use `midi_tools.hpp` for transforms/routing, `midi_note_state.hpp` for held-note
ownership, and `midi_delivery.hpp` for output/recovery. Keep generated event
sample offsets in the processing block and match note releases on bypass,
removal, transpose changes and overflow. The host adapter and DAW must actually
support and route MIDI output.

A keyboard/pad view can feed the same event route as a hardware device. Device
profiles and pad learning are separate from the voices; the product chooses
where that source sits relative to MIDI tools and instruments. For an
audio-to-MIDI product, prepare `pitch_detection.hpp`, translate confidence-gated
pitch results to note/bend events and define voice identity and note hysteresis.
Pitch detection alone is not a complete transcription or MPE implementation.
Reference tests: `midi_tools_tests.cpp`, `midi_delivery_tests.cpp`,
`midi_devices_tests.cpp` and `pitch_detection_tests.cpp`.

## 6. A low-latency standalone processor or router

Enable AudioDevice with a separately supplied miniaudio header. Negotiate the
actual sample rate/block/channel format, prepare DSP and caches, then process
only ready buffers in the callback. Put device changes, file loads, metering,
TAP/UI and resource construction on control/worker threads. Retire replaced
graphs with the prepared-exchange services. Recover held notes on stop/device
loss and display negotiated formats/errors rather than assumed values.

`tests/sdk_audio_consumer` is a small installed native-device consumer.
`tests/run_owned_pulse_device.py` provides an owned PulseAudio device test
fixture where that backend is available. A finished standalone router, MIDI
port adapters, clock policy, physical hardware latency tests and driver support
remain product work. The full Live standalone is outside this independent
archive.

## 7. A session-compatible replacement for an existing plugin

Capture the original format IDs, state chunks, parameter identities and assets
before changing wrappers. Use explicit original VST3 UIDs rather than the
helper's new-product defaults. Decode bounded legacy state off audio, migrate
using a product schema and prepare a candidate graph. Publish only a complete
candidate; keep the existing running state if preparation/admission fails.

Test real sessions containing several instances, inactive modes, assets,
parallel/mid-side groups and save/reopen cycles. Preserve unknown state where
required. The generic migration APIs cannot infer an undocumented product
schema. `HOST-PROJECTS.md`, `PROJECT-STATE.md` and `PREPARED-STATE-AND-RASTER.md`
describe related services. Current suite migration tests are product evidence,
not proof that every third-party plugin state can be converted.

## 8. A timeline editor or ARA-assisted tool

Combine wide OwnedAudio views, waveform thumbnails, TimelineOverview,
project-state trees and action history. Optional ARASource binds queued reads
to a source revision and channel span so stale work cannot consume replacement
content. `tests/sdk_ara_source_consumer` demonstrates the installed export with
separately supplied official ARA headers. Native DAW document integration,
editing tools, timing semantics and the complete ARA plugin extension still
need implementation. This is a foundation for an editor, not an included DAW.

## Standards to carry into each implementation

Use stable physical-value parameter IDs; one source of truth; complete gesture
lifetimes; bounded worker results with identities; honest modifier hover text;
readable labels; native browsing for local files; measured retained/scratch
budgets; and sample-boundary DSP adoption. Test queue pressure, cancellation,
restore, stale requests, repeated editor open/close, extreme scaling and actual
host shortcuts. See [UI-STANDARDS.md](UI-STANDARDS.md) for the shared practices.


## An asynchronous asset menu

Use `assets::Browser` for the scan and `PopupMenu` for the pending/results view.
After `menu.open(...)`, record `const auto token = menu.generation();`. This
works before painting. On the GUI tick, cancel the scan when
`!menu.isCurrent(token)`; when `browser.poll()` publishes a listing, validate
module/resource ownership, then replace the menu at the captured anchor. Show
`listing.error` and `listing.truncated` explicitly and retain a native Browse
entry. A worker captures paths/options only, not `menu` or the editor.

This pattern can serve an IR pack, wavetable folder, preset picker or sample
library. The popup-generation API and current suite integrations are source
only in r38; compile and run the deferred tests before shipping a consumer.
