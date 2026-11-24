# SDK feature inventory — r38 source in progress

This is the complete inventory of packaged public headers, grouped by the
capability they expose. It describes implemented source, not a promise that
every platform or use case has shipped. See the README and accompanying
revision-specific validation records for executed checks.

Header names below are relative to `include/cell/`; use `#include <cell/NAME.hpp>`.
The SDK has no dependency on the suite-only `studio_dsp.hpp`, which is excluded
from this independent handoff. `DSP` is a CMake interface target; most DSP
primitives are headers and some related services are compiled in Core.

Optional visual work can use [`AnalysisLease`](ANALYSIS-LEASE.md) to expire
capture after its editor stops polling. The fixed-storage callback helper has
no retry loop or heap operations. It and its new tests are source-only in r38.

r38 also adds popup lifetime tokens and immediate stale-row retirement, plus
exact-zero FIR processing compaction with unchanged declared tail duration.
These changes and their new regression cases are uncompiled. Directory work
uses the existing `assets::Browser`; no product/TONE3000 service enters the SDK.

## New original synthesis and interaction primitives

**Targets:** Core / header-only DSP; source changes are uncompiled.

`wavetable.hpp` imports cyclic WAV/WT and a documented baked VITALTABLE subset,
prepares harmonic-limited mip banks off audio and exports cycle-tagged float32
WAV. `synthesis.hpp` provides original oscillator/portamento and six-operator
FM graph primitives and sample-counted transition fades. `physical_model.hpp`
and `percussion.hpp` add fixed-storage modal instrument and drum voices.
See [SYNTHESIS.md](SYNTHESIS.md) for models, examples and current limits. `impulse_alignment.hpp` recommends delay/polarity from
caller-owned spectra. `resonance_mask.hpp` provides bounded reduce-only spectral
masking; [RESONANCE-MASK.md](RESONANCE-MASK.md) specifies focus, cut limits,
weighting and qualification boundaries. `knob_position.hpp` and `ui_defaults.hpp` hold shared interaction and
unity-pointer presentation policies without changing host normalization.

For example, import/build a `dsp::Wavetable` on a worker, publish it through
`LatestPreparedExchange`, and borrow it at block boundaries for `read()`.
Export `waveFile()` on a control thread for wavetable interchange. A product
can use `Portamento` and `FmVoice` with fixed voice storage, while keeping its
own MIDI, licensing, presets and effects graph outside the independent SDK.

These are reusable native C++ building blocks with explicit preparation and
ownership. They reduce repeated integration work when those contracts fit the
product. There is no measured claim that they outperform every other framework.
The complete commercial SYNTH module/UI and suite DSP are not part of Core.

**Public APIs:** [`wavetable.hpp`](../include/cell/wavetable.hpp),
[`synthesis.hpp`](../include/cell/synthesis.hpp),
[`physical_model.hpp`](../include/cell/physical_model.hpp),
[`percussion.hpp`](../include/cell/percussion.hpp),
[`impulse_alignment.hpp`](../include/cell/impulse_alignment.hpp),
[`resonance_mask.hpp`](../include/cell/resonance_mask.hpp),
[`knob_position.hpp`](../include/cell/knob_position.hpp),
[`ui_defaults.hpp`](../include/cell/ui_defaults.hpp).

## Processing and hosts

**Targets:** Core, Wrappers.

Prepared planar float processing; transport with tempo/loop availability; processor lifecycle, block segmentation, sample-offset parameter/MIDI handling, owned document-sized audio and denormal control. Host layouts and supported buses require format/product qualification.

**Public APIs:** [`audio.hpp`](../include/cell/audio.hpp), [`plugin.hpp`](../include/cell/plugin.hpp), [`wrapper.hpp`](../include/cell/wrapper.hpp), [`process_buffer.hpp`](../include/cell/process_buffer.hpp), [`owned_audio.hpp`](../include/cell/owned_audio.hpp), [`realtime.hpp`](../include/cell/realtime.hpp), [`sample_clock.hpp`](../include/cell/sample_clock.hpp), [`parameter_timeline.hpp`](../include/cell/parameter_timeline.hpp).

Contract: [TRANSPORT.md](TRANSPORT.md).

## Parameters and gestures

**Targets:** Core.

Stable keys/IDs, physical ranges/units/choices, atomic parameter values, bounded begin/value/end edit admission, restore policy and host-facing edits. Open gestures reserve closing events; one GUI producer owns edits.

**Public APIs:** [`parameters.hpp`](../include/cell/parameters.hpp), [`parameter_restore.hpp`](../include/cell/parameter_restore.hpp).

Contract: [AUTOMATION.md](AUTOMATION.md).

## Realtime communication and ownership

**Targets:** Core.

Bounded SPSC communication, acknowledgement and prepared replacement/retirement helpers. Latest-prepared exchange separates requests from sample-boundary adoption and off-audio destruction. Follow each helper's producer/consumer contract.

**Public APIs:** [`core.hpp`](../include/cell/core.hpp), [`callback_gate.hpp`](../include/cell/callback_gate.hpp), [`prepared_exchange.hpp`](../include/cell/prepared_exchange.hpp), [`latest_exchange.hpp`](../include/cell/latest_exchange.hpp), [`latest_prepared_exchange.hpp`](../include/cell/latest_prepared_exchange.hpp), [`operation_ack.hpp`](../include/cell/operation_ack.hpp).

Contract: [PREPARED-STATE-AND-RASTER.md](PREPARED-STATE-AND-RASTER.md).

## Processor/controller commands

**Targets:** Core.

Asynchronous bounded product queries, command acknowledgement and state snapshot transfer. Replies carry request/generation identity; GUI code polls rather than blocking for the processor.

**Public APIs:** [`commands.hpp`](../include/cell/commands.hpp), [`state_transfer.hpp`](../include/cell/state_transfer.hpp).

Contract: [SERVICES.md](SERVICES.md).

## History and state

**Targets:** Core; optional State/StateTree.

Archive/timeline and action history, project trees/XML, legacy binary/XML state reading, schema migration and FXP chunk programs. A product supplies its schema and original identities; no automatic universal session conversion.

**Public APIs:** [`history.hpp`](../include/cell/history.hpp), [`action_history.hpp`](../include/cell/action_history.hpp), [`state_tree.hpp`](../include/cell/state_tree.hpp), [`xml.hpp`](../include/cell/xml.hpp), [`legacy_state.hpp`](../include/cell/legacy_state.hpp), [`state_migration.hpp`](../include/cell/state_migration.hpp), [`fxp.hpp`](../include/cell/fxp.hpp).

Contract: [PROJECT-STATE.md](PROJECT-STATE.md).

## Data and result utilities

**Targets:** Core.

Bounded JSON, structured results, byte storage, memory streams, text lists and SHA-256. Heavy parsing/serialization belongs off audio.

**Public APIs:** [`json.hpp`](../include/cell/json.hpp), [`result.hpp`](../include/cell/result.hpp), [`byte_block.hpp`](../include/cell/byte_block.hpp), [`memory_stream.hpp`](../include/cell/memory_stream.hpp), [`text_list.hpp`](../include/cell/text_list.hpp), [`hash.hpp`](../include/cell/hash.hpp).

Contract: [SERVICES.md](SERVICES.md).

## Text, localization and identity

**Targets:** Core.

UTF-8 text and Unicode case mappings, localization catalogs/plurals/number symbols, OS UUIDs, color and utility helpers. Complete shaping, bidi/IME/font fallback and translations are not supplied.

**Public APIs:** [`text.hpp`](../include/cell/text.hpp), [`unicode.hpp`](../include/cell/unicode.hpp), [`localization.hpp`](../include/cell/localization.hpp), [`uuid.hpp`](../include/cell/uuid.hpp), [`colour.hpp`](../include/cell/colour.hpp), [`utility.hpp`](../include/cell/utility.hpp), [`memory.hpp`](../include/cell/memory.hpp).

Contract: [FONTS-AND-LOCALIZATION.md](FONTS-AND-LOCALIZATION.md).

## Files and system services

**Targets:** Core.

Native paths/files and streams, atomic publication, pooled read handles with independent cursors, dynamic libraries, child processes, dispatch and persistent browser locations. Blocking file/process operations stay off audio.

**Public APIs:** [`file.hpp`](../include/cell/file.hpp), [`files.hpp`](../include/cell/files.hpp), [`streams.hpp`](../include/cell/streams.hpp), [`pooled_file.hpp`](../include/cell/pooled_file.hpp), [`dynamic_library.hpp`](../include/cell/dynamic_library.hpp), [`process.hpp`](../include/cell/process.hpp), [`dispatch.hpp`](../include/cell/dispatch.hpp), [`browser_locations.hpp`](../include/cell/browser_locations.hpp).

Contract: [FILES-AND-STREAMS.md](FILES-AND-STREAMS.md).

## Jobs and assets

**Targets:** Core.

Owned background-job pools, cancellation/result admission, prepared read-ahead, asset listings/annotations and multiresolution waveform thumbnails. Consumers retain asset lifetime and choose cache/memory budgets.

**Public APIs:** [`jobs.hpp`](../include/cell/jobs.hpp), [`read_ahead.hpp`](../include/cell/read_ahead.hpp), [`assets.hpp`](../include/cell/assets.hpp), [`thumbnails.hpp`](../include/cell/thumbnails.hpp).

Contract: [LIBRARY-BROWSING.md](LIBRARY-BROWSING.md).

## Audio files and sample export

**Targets:** AudioFiles; Core views.

Built-in PCM/float WAV, RIFX/RF64 and metadata/interchange; optional extended/compressed formats through separately deployed libsndfile. Streaming atomic WAVE export for supported sample views preserves rate/channels/sample values.

**Public APIs:** [`audio_file.hpp`](../include/cell/audio_file.hpp), [`wave_export.hpp`](../include/cell/wave_export.hpp).

Contract: [AUDIO-FILES.md](AUDIO-FILES.md).

## Portable drum libraries

**Targets:** Core.

Independent .trg mapping plus complete embedded WAVE assets, 64-bit seekable sample views and streaming atomic writing. Playback views do not extract files. Product velocity/round-robin/voice policies are separate.

**Public APIs:** [`trg.hpp`](../include/cell/trg.hpp).

Contract: [TRG-FORMAT.md](TRG-FORMAT.md).

## Observed third-party drum containers

**Targets:** Core.

Read observed version-4 embedded-audio .tci and Gog_v3.5 primary PCM .gog layouts with bounds and seekable WAVE views. Not all versions/layouts; no TCI/GOG writer or original detector/effects emulation is supplied.

**Public APIs:** [`tci.hpp`](../include/cell/tci.hpp), [`gog.hpp`](../include/cell/gog.hpp).

Contract: [TCI-FORMAT.md](TCI-FORMAT.md).

## MIDI messages, timing and delivery

**Targets:** Core.

Bounded MIDI buffers/output, message helpers, held-note state, controller input, delivery/overflow recovery and Standard MIDI File reading/writing/time maps. Host MIDI output capability is format/host dependent.

**Public APIs:** [`midi.hpp`](../include/cell/midi.hpp), [`midi_message.hpp`](../include/cell/midi_message.hpp), [`midi_note_state.hpp`](../include/cell/midi_note_state.hpp), [`midi_delivery.hpp`](../include/cell/midi_delivery.hpp), [`midi_control.hpp`](../include/cell/midi_control.hpp), [`smf.hpp`](../include/cell/smf.hpp).

Contract: [MIDI-OUTPUT.md](MIDI-OUTPUT.md).

## MIDI processing and hardware profiles

**Targets:** Core.

Configurable transforms/channel-controller routing and bounded note tracking; two-endpoint/per-pad learning, note maps and atomic user device profiles/defaults by extension. Instrument voices and kit-format importers stay in products.

**Public APIs:** [`midi_tools.hpp`](../include/cell/midi_tools.hpp), [`midi_devices.hpp`](../include/cell/midi_devices.hpp).

Contract: [MIDI-FILES.md](MIDI-FILES.md).

## Instrument scripting subset

**Targets:** optional KSP.

Independent source compiler and bounded event VM with voice/event identity, groups, polyphonic state and scheduled callbacks. Unknown commands reject. Requires a sampler/DSP adapter; not an NKI/NICNT loader or complete Kontakt emulator.

**Public APIs:** [`ksp.hpp`](../include/cell/ksp.hpp).

Contract: [README.md](README.md).

## DSP fundamentals

**Targets:** DSP (headers), Core.

Smoothing, delay, biquads, prepared complex/real FFTs, vector helpers and bounded analysis snapshots/mailboxes. FFT and scratch preparation occurs outside processing.

**Public APIs:** [`dsp.hpp`](../include/cell/dsp.hpp), [`vector_math.hpp`](../include/cell/vector_math.hpp), [`analysis.hpp`](../include/cell/analysis.hpp).

Contract: [SERVICES.md](SERVICES.md).

## Filters, envelopes and dynamics helpers

**Targets:** DSP (headers).

Inline-state all-pass/TPT filters, envelope/follower/trigger helpers, selectable slope tables, shared tilt dynamics and mono reference tools. Musical controls and full processors remain product code.

**Public APIs:** [`allpass.hpp`](../include/cell/allpass.hpp), [`state_variable.hpp`](../include/cell/state_variable.hpp), [`ladder.hpp`](../include/cell/ladder.hpp), [`envelope.hpp`](../include/cell/envelope.hpp), [`envelope_filter.hpp`](../include/cell/envelope_filter.hpp), [`filter_slopes.hpp`](../include/cell/filter_slopes.hpp), [`tilt_dynamics.hpp`](../include/cell/tilt_dynamics.hpp), [`mono_reference.hpp`](../include/cell/mono_reference.hpp).

Contract: [FILTER-SLOPES.md](FILTER-SLOPES.md).

## Rate, convolution and character helpers

**Targets:** DSP (headers).

Prepared half-band/polyphase oversampling, convolution, non-owning guarded delay history, intentional rate reduction, saturation and magnetic-character primitives. Oversampling is not free or automatically zero-latency.

**Public APIs:** [`oversampling.hpp`](../include/cell/oversampling.hpp), [`polyphase.hpp`](../include/cell/polyphase.hpp), [`halfband_profiles.hpp`](../include/cell/halfband_profiles.hpp), [`convolution.hpp`](../include/cell/convolution.hpp), [`delay_history.hpp`](../include/cell/delay_history.hpp), [`degradation.hpp`](../include/cell/degradation.hpp), [`ferro.hpp`](../include/cell/ferro.hpp).

Contract: [SERVICES.md](SERVICES.md).

## Pitch tracking

**Targets:** Core.

Prepared mono/polyphonic detector and bounded pitch results usable by tuners or audio-to-MIDI. Detection latency and harmonic errors depend on material, mode, window and tuning; no universal accuracy claim.

**Public APIs:** [`pitch_detection.hpp`](../include/cell/pitch_detection.hpp).

Contract: [PITCH-TRACKING.md](PITCH-TRACKING.md).

## Native vector rendering and windows

**Targets:** optional UI.

Canvas paths, transforms/clips, gradients, text/images and offscreen surfaces; native embedded/standalone Windows, X11 and Cocoa window sources, DPI/content scaling and input. Current Canvas backend uses separately supplied Blend2D software rendering.

**Public APIs:** [`ui.hpp`](../include/cell/ui.hpp), [`ui_scaling.hpp`](../include/cell/ui_scaling.hpp).

Contract: [UI-STANDARDS.md](UI-STANDARDS.md).

## Reusable controls and input

**Targets:** optional UI; several header-only contracts.

Semantic palette/layout/control geometry, button/knob drawing, unit parsing/numeric entry, owned gestures, dominant-axis rotary motion, fractional value drags, focus/accessibility metadata and viewport-follow resizing. Native screen-reader providers remain work.

**Public APIs:** [`ui_components.hpp`](../include/cell/ui_components.hpp), [`ui_interaction.hpp`](../include/cell/ui_interaction.hpp), [`ui_values.hpp`](../include/cell/ui_values.hpp), [`scroll_viewport.hpp`](../include/cell/scroll_viewport.hpp).

Contract: [INTERACTIONS.md](INTERACTIONS.md).

## Menus, help and native browsing

**Targets:** optional UI.

Cascading pointer-anchored menus, keyboard/focus handling, delayed hover help and asynchronous system file/folder dialogs. Linux needs a supported desktop dialog provider. File decode/import remains a worker task.

**Public APIs:** [`popup_menu.hpp`](../include/cell/popup_menu.hpp), [`hover_help.hpp`](../include/cell/hover_help.hpp), [`file_dialog.hpp`](../include/cell/file_dialog.hpp).

Contract: [LIBRARY-BROWSING.md](LIBRARY-BROWSING.md).

## Instrument controller and timeline

**Targets:** optional UI.

Reusable floating keyboard/pad/controller UI hooks and waveform timeline overview/navigation. Device hooks, routed instruments, clip editing and branded product behavior are supplied by consumers.

**Public APIs:** [`instrument_controller.hpp`](../include/cell/instrument_controller.hpp), [`timeline.hpp`](../include/cell/timeline.hpp).

Contract: [NATIVE-HANDOFF.md](NATIVE-HANDOFF.md).

## Widget layer

**Targets:** optional UI.

Component hierarchy, control, drawing, geometry, focus/navigation and service contracts over native Canvas. These are reusable foundations, not a complete visual authoring application.

**Public APIs:** [`widgets/component.hpp`](../include/cell/widgets/component.hpp), [`widgets/controls.hpp`](../include/cell/widgets/controls.hpp), [`widgets/drawing.hpp`](../include/cell/widgets/drawing.hpp), [`widgets/geometry.hpp`](../include/cell/widgets/geometry.hpp), [`widgets/input.hpp`](../include/cell/widgets/input.hpp), [`widgets/navigation.hpp`](../include/cell/widgets/navigation.hpp), [`widgets/services.hpp`](../include/cell/widgets/services.hpp), [`widgets/text.hpp`](../include/cell/widgets/text.hpp).

Contract: [UI-STANDARDS.md](UI-STANDARDS.md).

## Blur, shadows, glows and layers

**Targets:** Graphics; UI integration.

Original bounded CPU alpha/ARGB blur, outer/inner shadow/glow, per-instance LRU effect caches and DPI-aware retained layers. Track retained versus scratch memory; no new GPU renderer or automatic suite integration.

**Public APIs:** [`graphics_effects.hpp`](../include/cell/graphics_effects.hpp), [`ui_effects.hpp`](../include/cell/ui_effects.hpp).

Contract: [GRAPHICS-EFFECTS.md](GRAPHICS-EFFECTS.md).

## Optional system-browser UI

**Targets:** optional WebView; Core bridge.

GUI-thread system browser child, offline HTML, bounded JSON messaging, readiness/error events, physical sizing and explicit parameter allow-list/gesture adapter. Windows WebView2, macOS WKWebView and Linux X11 WebKitGTK backends have different qualification levels.

**Public APIs:** [`webview.hpp`](../include/cell/webview.hpp), [`web_parameter_bridge.hpp`](../include/cell/web_parameter_bridge.hpp).

Contract: [WEBVIEW.md](WEBVIEW.md).

## Standalone device I/O

**Targets:** optional AudioDevice.

Explicit float32 duplex device enumeration/open/stop, negotiated format and statistics using separately supplied miniaudio. Hardware buffer deadlines, routing, MIDI device adapters and application UI require consumer implementation/qualification.

**Public APIs:** [`audio_device.hpp`](../include/cell/audio_device.hpp).

Contract: [NATIVE-HANDOFF.md](NATIVE-HANDOFF.md).

## Optional ARA source and REAPER context

**Targets:** optional ARASource; format-dependent Core header.

ARA archive/source-read adapters with revision and channel-span checks; REAPER host-context bridge with separately supplied official headers. Not complete ARA document integration, plugin hosting or universal DAW extensions.

**Public APIs:** [`ara/archive_stream.hpp`](../include/cell/ara/archive_stream.hpp), [`ara/source_reader.hpp`](../include/cell/ara/source_reader.hpp), [`reaper_project.hpp`](../include/cell/reaper_project.hpp).

Contract: [ARA-SOURCES.md](ARA-SOURCES.md).

## Format adapters and build helpers

| Format / helper | Included | Scope and requirements |
| --- | --- | --- |
| CLAP / `cell_add_clap_plugin` | Original adapter source | Supply official headers; Linux factory/lifecycle and consumer checks exist. Qualify native platforms/hosts for each product. |
| VST3 / `cell_add_vst3_plugin` | Original adapter source | Supply supported official SDK; processor/controller, state, GUI and MIDI services. Preserve original class IDs explicitly for replacements. Multi-bus/surround and host coverage need separate qualification. |
| VST2.4 / `cell_add_vst2_plugin` | Prototype source | Original SDK/license and explicit numeric ID required; no SDK bundled and no runtime qualification claimed. |
| AAX / `cell_add_aax_plugin` | Linked Linux test module and tested ACF exports | Avid SDK and stable product IDs. Windows/macOS bundle builds, Pro Tools runtime, latency adoption and signing require qualification. |
| Version/resource helper | CMake/resource templates | Consistent standalone version suffixes and Windows binary version metadata. Product naming and release packaging remain consumer responsibilities. |

See [wrappers.json](wrappers.json). Generic adapter evidence and product-specific
Windows/Linux VST3 checks are different scopes. User-owned original session
fixtures must verify module settings, assets and parallel/mid-side relationships.

## Deliberately outside the independent SDK

The archive does not contain the full Studio/Live/Immersive editors, every suite
DSP module, product sampler/synth engines, neural models, online-model services,
proprietary SDKs, user libraries, DAW licenses or release binaries. Their APIs
can use this framework, but that does not make those engines packaged SDK
features. Third-party library importers in a product are not universal SDK
format support. Modern Kontakt library playback and complete KSP behavior are
not supplied.

GUI GPU rendering, native accessibility providers, broad host/platform
qualification and complete ARA document integration remain development work.
The WebView module is an optional system browser, not an original browser engine
or a substitute for a native GPU Canvas backend.
