# Portable processing graph prototype

A graph document stores stable node IDs, planar audio connections, initial
settings and automation. The same JSON feeds the headless player and an
example standalone process that hosts a Cell Engine and Processor. Both
process local audio files.

```sh
cmake -S framework -B build-graph -DCELL_SDK_EXAMPLES=ON -DCELL_SDK_TESTS=ON
cmake --build build-graph --target cell_graph_demo cell_portable_graph_tests
ctest --test-dir build-graph -R '^cell_portable_graph$' --output-on-failure
build-graph/cell_graph_demo headless examples/portable_graph/example.json input.wav headless.wav
build-graph/cell_graph_demo engine examples/portable_graph/example.json input.wav engine.wav
```

Use a 48 kHz mono or stereo input for the included graph. The automation
changes brightness after half a second and lowers gain after one second.
The demo writes 32-bit float WAV and requires a new destination path.

## Document and playback contract

- Schema `cell.graph.v1` describes one serial audio chain of up to 64 nodes.
  `input` and `output` are reserved endpoints. Node and connection declaration
  order is independent of signal-flow order. Each node preserves the channel
  layout. The player supports up to 64 channels with independent filter memory.
- `gainDb` is a physical level in dB. The low-pass node's normalized
  `brightness` maps exponentially between its explicit `minHz` and `maxHz`.
  The filter uses the SDK's Butterworth low-pass biquad.
- Automation uses absolute integer sample positions from the start of playback
  at the document's sample rate. Events are ordered by sample and applied in
  document order at ties. A change applies to its named sample as a step.
  Block size does not change event timing. Reset returns to sample zero.
- Document parsing, serialization, graph validation and coefficient design
  run off audio. The prepared player processes in place. A sample-rate change
  requires a new session.
- The current schemas omit transport sync,
  automation smoothing and rate conversion. Exact sample comparisons are
  tested within one build. Cross-platform bit identity is unspecified.

## Parallel routing

Use `cell.graph.v2` to split and merge paths. A connection copies planar audio
to its destination. When several routes meet at a node or at `output`, the
player sums them at unity before processing or writing the result. Use gain
nodes to set branch levels. Output is unclipped floating point.

V2 retains the gain, low-pass, and automation definitions from V1. It allows
up to 64 processing nodes and 256 distinct connections. Every processing node
must receive audio from `input` and reach `output`. Feedback cycles and
duplicate links are rejected during parsing. The prepared player allocates
one audio buffer per node and any delay storage off the audio thread, with a
combined 64 MiB sample-storage limit.
Topology and summing order are deterministic across JSON declaration order.

```sh
build-graph/cell_graph_demo headless examples/portable_graph/parallel.json input.wav parallel.wav
```

Routing edits between V2 documents crossfade in `LiveSession`, even if the same
nodes remain in place. Sample positions and automation continue during the
crossfade. Parallel paths share the document's sample rate.

### Fixed latency compensation

A V2 delay node such as `{"id":"lookahead","type":"delay","frames":128}`
adds 128 samples of fixed latency. Valid delay lengths are 1 through 1,048,576
samples per node. The graph aligns every input to the longest incoming path
at each merge, including the final output. Graph latency is available from
`Document::latency()` and `Player::latency()`. Automation positions remain
absolute sample indexes on the running processing clock.

```sh
build-graph/cell_graph_demo headless examples/portable_graph/compensated.json input.wav aligned.wav
```

The demo retains leading latency and flushes it together with the graph's
known finite oversampling tail. Arbitrary DSP tails require separate handling. Live edits must keep the session's output latency. Routing changes
warm the new delay paths under the old output for the graph's latency, then
crossfade. Parameter-only edits retain delay history. Dynamic latency changes
require a new session.

## Live editing

The standalone command-line demo applies a brightness point, changes signal
flow, undoes the graph replacement, and saves the restored document with the
brightness point intact. It renders the transition to a WAV for listening.

```sh
build-graph/cell_graph_demo live examples/portable_graph/example.json input.wav live.wav examples/portable_graph/reordered.json tone saved.json
build-graph/cell_graph_demo headless saved.json input.wav reopened.wav
```

`LiveSession` takes edits from one serialized control producer. The callback
adopts prepared edits at block boundaries. Parameter edits retain filter
memory when node order, IDs and types match. Structural edits crossfade
for 128 samples by default using preallocated scratch audio. Both sides
advance at the same position during the fade, and newer edits wait for
the fade to complete. Retired players are reclaimed on the control thread.

Schedule a brightness point ahead of `nextSample()` to allow time for
preparation and adoption. A late edit catches up to the value on the new
timeline at adoption. The sample clock advances across input loop boundaries.
`document()` returns the latest accepted revision, and `updatePending()`
indicates whether it has been adopted. The last 16 accepted edits can be
undone. Saving the document preserves its timeline. The WAV records the live
transition, which includes filter history and fades from the editing session.


## Inspect controls and write automation

`Document::controls()` returns owning control descriptions keyed by node ID
and parameter name. Gain is measured in dB. Brightness ranges from 0 to 1
and exposes the same exponential cutoff mapping used by playback. Fixed
latency nodes have no automatable control.

```sh
build-graph/cell_graph_demo describe examples/portable_graph/parallel.json
build-graph/cell_graph_demo automate examples/portable_graph/parallel.json edited.json \
  tone brightness 24000 0.65 wet gainDb 24000 -9
build-graph/cell_graph_demo headless edited.json input.wav edited.wav
```

`describe` emits a `cell.graph-controls.v1` JSON report with the graph schema,
sample rate, latency, control ranges and mappings, and existing automation.
For v4 graphs it also reports saved LFOs and modulation routes.
`describe-project` reports the same fields for a saved project.
`automate` takes one or more node, parameter, sample, and value groups. A
single invalid point rejects the whole edit. A successful edit writes a new
graph file using exclusive publication. At equal samples, new points follow
existing points in the order supplied.

Use `Document::withAutomation` for offline edits or
`LiveSession::setAutomation` to queue a batch as one undoable live edit.
Schedule ahead of the session's current sample and allow the prepared edit
to be adopted before its deadline. This interface is available in process
and through the local demo CLI.

## Musical intent controls

V3 keeps the routing and latency rules from V2 and adds an `intents` array.
Each intent has a stable ID, a display name, and bindings to node parameters.
A binding's `atZero` and `atOne` values use the target parameter's units.
They may rise or fall as the normalized control increases. Cell interpolates
linearly between those endpoints by default, then applies the target parameter's usual
DSP mapping. For example, the brightness target still maps exponentially to
cutoff Hz. Azimuth bindings can set `"interpolation": "shortestArc"` to
follow the shorter arc between the endpoints. A binding from +170 to −170
degrees then maps 0.5 to the rear at 180 degrees. For endpoints exactly 180
degrees apart, interpolation follows the sign of `atOne - atZero`.

The included `intent.json` maps **Clarity** to filter brightness and wet gain.
The saved timeline retains intent IDs and normalized values. Cell expands
the bindings during document validation so the audio callback uses the same
prepared events as direct automation. Intent points and direct parameter
points follow document order at equal sample positions. The latest point to
affect a target at that sample takes effect.

```sh
build-graph/cell_graph_demo describe examples/portable_graph/intent.json
build-graph/cell_graph_demo intent examples/portable_graph/intent.json edited.json clarity 48000 0.8
build-graph/cell_graph_demo headless edited.json input.wav edited.wav
```

Use `Document::intents()` to inspect bindings, `Document::withIntent()` to
save a point, or `LiveSession::setIntent()` to schedule one as an undoable
edit. V3 supports up to 32 intents, with up to 64 unique bindings each.
Expanded direct and intent points share the 32,768-event preparation limit.
Existing V1 and V2 documents remain readable.

## Preserve a graph and its source audio

A `.cellgraph` project stores the editable graph and the original bytes of
one native PCM/float WAVE source with 1–64 channels at the graph sample rate. The
source name and path are unnecessary when reopening the project. Graph
routing, automation, and musical intent bindings stay editable.

```sh
build-graph/cell_graph_demo pack examples/portable_graph/intent.json input.wav song.cellgraph
build-graph/cell_graph_demo verify-project song.cellgraph
build-graph/cell_graph_demo describe-project song.cellgraph
build-graph/cell_graph_demo render-project song.cellgraph restored.wav
build-graph/cell_graph_demo extract-graph song.cellgraph editable.json
build-graph/cell_graph_demo extract-wave song.cellgraph original.wav
build-graph/cell_graph_demo replace-graph song.cellgraph edited.json revised.cellgraph
```

`describe-project` verifies the archive and emits the same
`cell.graph-controls.v1` JSON report as `describe`, with a `source` object
for the embedded WAVE. The report includes source frames, sample rate,
channel count, channel mask, sample format, and Broadcast WAVE time
reference. A zero channel mask means the file does not specify speaker
assignments. A missing time reference appears as JSON `null`. The command
reads the project in place without extracting or rendering the audio.

`replace-graph` and `graph::saveProjectWithGraph` publish a new archive
from an edited graph and the original project's embedded WAVE bytes. The
SDK verifies the input archive and checks the new graph's sample rate and
channel requirements before publishing. The destination must be new.

The SDK functions are `graph::saveProject` and `graph::openProject` in
`<cell/graph_project.hpp>` from `Cell::AudioFiles`. They run on a worker
or control thread. Save publishes exclusively and discards partial output
on error or cancellation. Open verifies SHA-256 before returning the graph
and audio reader. The reader seeks directly into the embedded audio.
Source files should stay stable during packing and the project file should
stay stable while open.

The byte format starts with ASCII `CELLGP01`, followed by two unsigned
64-bit little-endian lengths for the UTF-8 graph JSON and native WAVE.
Those payloads follow in that order. The last 32 bytes are the raw SHA-256
digest of the header and both payloads. Readers reject trailing bytes,
invalid lengths, corrupt content, and unsupported graph schemas.

### Check a saved render

Keep a reference WAV with the project. The checker renders into bounded
working memory and compares the decoded float32 samples with the reference.

```sh
build-graph/cell_graph_demo check-render song.cellgraph reference.wav
build-graph/cell_graph_demo check-render song.cellgraph reference.wav 0.000001
```

The default is bit-exact, including the sign of zero. An explicit positive
tolerance allows a bounded absolute difference per sample. The JSON report
includes maximum and RMS error and the first bit difference, with zero-based
frame and channel indexes. A mismatch exits unsuccessfully. The reference
must have the render's sample rate, channel count, and frame count, including
the graph's flushed latency. The comparison is over audio samples. Container
metadata and encoding can differ.

SDK clients can use `graph::renderProject()` and
`graph::compareProjectRender()` from `<cell/graph_project.hpp>`.

### Profile DSP by block size

```sh
build-graph/cell_graph_demo profile-project song.cellgraph 64 128 512
```

With no block sizes supplied, the profiler runs at 512 frames. It makes one
pass through the project for each size and emits a JSON report with total DSP
time, slowest block location, average and peak percentages of the audio time
budget, and the count of blocks above budget. The budget for each block is
its actual frame count divided by the sample rate. Partial final blocks and
the declared latency flush are included. Supported block sizes range from
1 to 1,048,576 frames, subject to the graph's prepared-storage limit.

The timer measures elapsed wall time inside `Player::process`. Preparation,
archive verification, file reads, and report generation are outside the timed
region. Scheduler preemption can still affect the result. This is an offline
profile for comparing graph configurations and block sizes. Measurements of
a DAW callback or physical device need their own run.

SDK clients can call `graph::profileProject()` from
`<cell/graph_project.hpp>`.

### Oversampled saturation

V2 and V3 accept a `saturator` node with `driveDb` in [0, 36] and a fixed
`oversampling` factor of 1, 2, 4, 8 or 16. The circuit applies input drive
followed by a tanh waveshaper. For factors above 1, Cell uses its linear-phase
half-band FIR oversampling cascade and compensates parallel paths for its
integer group delay. The declared delays are 0, 32, 48, 56 and 60 samples,
respectively. Frequency response and phase around the filter cutoff are
properties of the selected DSP.

Drive supports sample-timed automation and V3 intent bindings. A parameter
edit retains the oversampling filter history. An oversampling factor is a
structural setting. If changing it alters total graph latency, open a new
live session. The offline renderer, reference checker and profiler include
the declared FIR tail after the graph's latency.

```sh
build-graph/cell_graph_demo describe examples/portable_graph/saturation.json
build-graph/cell_graph_demo headless examples/portable_graph/saturation.json input.wav saturated.wav
```

The **Heat** control in this example increases drive and lowers the wet
branch. Profile larger factors on the intended hardware when choosing a
processing budget.

### Multichannel routing

V2 and V3 accept a `channelMap` node that declares one source per output
slot. Sources use zero-based channel indexes. A repeated index duplicates
a channel, and `null` writes silence. All maps in a graph use the same
width as the source audio. A graph without a map can process any supported
channel count.

For example, the six-channel map `[1, 0, 2, null, 4, 5]` exchanges the first
two channels and silences the fourth. With the standard WAVE 5.1 channel
order, those slots are front left, front right, and LFE. The graph keeps the
original output slot layout, speaker mask, and time reference. With an
unspecified speaker mask, channels are addressed by index.

```sh
build-graph/cell_graph_demo describe examples/portable_graph/surround.json
build-graph/cell_graph_demo pack examples/portable_graph/surround.json input-5.1.wav surround.cellgraph
build-graph/cell_graph_demo render-project surround.cellgraph routed-5.1.wav
```

`Document::requiredChannels()` reports the required width or zero for a graph
without a fixed width. Map edits crossfade in a live session. Changing the
channel count requires a fresh source and playback configuration.

### Stereo position

V2 and V3 accept a `pan` node that places one selected input channel in a
stereo field. Set `source` to 0 for the left input or 1 for the right input.
`pan` ranges from −1 (left) through 0 (center) to +1 (right).

The node replaces its branch with the panned signal. Use a parallel branch
to mix in another source. The left/right power weights are
`(1 - pan) / 2` and `(1 + pan) / 2`, so the selected signal has constant
total power throughout a move. Automation starts at the requested sample
and smooths over 10 ms. Live updates preserve a move in progress when the
routing and selected channel stay the same.

The stereo width is fixed at two channels. The example uses the left input
as the foreground source and binds the panner to an editable **Stereo position**
intent.

```sh
build-graph/cell_graph_demo headless examples/portable_graph/position.json stereo.wav positioned.wav
```

### Surround position

V2 and V3 accept an `azimuth` node that places one selected input channel in
a horizontal speaker layout. Set `source` to the zero-based input channel
and `speakers` to one entry per output channel, in the same order as the audio
channels. Angles are in degrees from −180 to +180, with 0 facing forward and
positive values toward the right. Use `null` to silence a slot such as LFE
on this branch. Provide at least three distinct directional speakers, with
no gap greater than 180 degrees around the circle.

The node distributes the selected signal across adjacent speakers with
equal-power weights. Azimuth automation starts at the scheduled sample and
smooths over 10 ms along the shortest angular path. A live update preserves
the move in progress when the selected source and speaker layout stay the
same. Use parallel branches to mix in other sources or route LFE separately.

Host tools can call `Document::azimuthLayouts()` to recover each azimuth
node's ID, zero-based input channel, and speaker angles in output channel
order. Angles reflect the rendered layout, with the rear normalized to
+180 degrees. Silent slots are empty. The CLI `describe` report exposes
the same information as `azimuthLayouts` when the graph has azimuth nodes,
with silent slots represented by JSON `null`.

The example uses WAVE 5.1 channel order and moves a source across the rear
from +170 through 180 to −170 degrees using a shortest-arc intent binding.

```sh
build-graph/cell_graph_demo headless examples/portable_graph/surround-position.json input-5.1.wav positioned-5.1.wav
```

### Listener orientation

A V3 document can declare `"listenerYaw": 0` at the root to use scene-relative
azimuths. Positive yaw turns the listener right. Every azimuth node subtracts
the same listener yaw from its source position before mapping to speakers.
For example, a source at 0° moves to the listener's left when the listener
turns +90°. Stereo `pan` controls retain their usual meaning.

Schedule turns with an automation point such as
`{"sample": 24000, "listenerYaw": 90}`. The turn follows the shortest arc
over 10 ms. `Document::withListenerYaw` and `LiveSession::setListenerYaw`
schedule equivalent points from an app. To add the same turn from the CLI:

```sh
build-graph/cell_graph_demo listener-yaw scene.json edited.json 24000 90 48000 0
```

The batch API accepts a span of `ListenerYawPoint` values. Points are sorted
by sample, and the last supplied point at a given sample takes precedence.
Cell validates the full batch before publishing one live edit.

Set `"listenerYawCurve": "shortestArc"` at the root to make these points
keyframes. The initial yaw supplies the sample-zero keyframe, and an
explicit point at sample zero takes precedence. The listener reaches each
keyframe at its exact sample and holds the final heading. Editing the
curve during live playback uses the session's topology crossfade.

The CLI `describe` output includes the initial yaw, optional curve, and
saved automation. This model covers horizontal
orientation.

### Shared speaker layout

A V3 scene can put its speakers at the root with
`"speakerLayout": [-90, 0, 90, 180]`. Azimuth nodes that omit `speakers`
inherit this layout. The angles are in output channel order and use the
same validation and silent slots as per-node speaker lists. Explicit
per-node lists can still be used on branches with the same channel count.

`Document::withSpeakerLayout` retargets the shared layout while preserving
source definitions and automation. Moving to a different channel count
requires matching source audio and a new playback configuration. The CLI
`describe` report includes the shared layout and each node's effective layout.


From the CLI, save the replacement speaker angles as a JSON array in a
separate file. The output is a new graph document.

```sh
build-graph/cell_graph_demo retarget scene.json speakers.json retargeted.json
```

For an existing `.cellgraph` project, use `extract-graph` to obtain the
scene and `replace-graph` to save the retargeted graph with the original
embedded WAVE. The replacement must match the embedded channel count.

### Source distance

V3 azimuth nodes can add `"distanceMeters": 2`. The panner applies
`1 / max(1, distanceMeters)` to the source amplitude before steering it
between speakers. Two meters gives half amplitude. Sources at one meter
or closer use unity gain. The field defaults to one meter and accepts
values from zero through 100,000.

A live edit to distance smooths the gain over 10 ms when the routing and
layout match. Retargeting the speakers preserves source distance. This
model applies attenuation only, with no propagation delay or room model.

Declaring `distanceMeters` also enables a distance automation control on
that source. Use the normal `automate` command or bind a musical intent to
`distanceMeters` with endpoints in meters. A distance event changes the
attenuation over 10 ms from the scheduled sample. Distance and azimuth
have independent ramps, and a single intent can bind both.

### Source azimuth keyframes

A V3 azimuth node can set `"azimuthCurve": "shortestArc"` to treat its
azimuth automation as continuous keyframes. The initial azimuth supplies
sample zero, and the last point at any sample wins. The source reaches
each keyframe at its exact sample and holds its final heading. Listener
yaw is evaluated independently and subtracted before panning. Updating
a source curve in a live session uses the session crossfade.

### Distance keyframes

A V3 azimuth node with `distanceMeters` can set `"distanceCurve": "linear"`.
Its distance points become continuous keyframes, with the initial distance
at sample zero and the last point winning at each sample. Cell interpolates
in meters and applies inverse-distance attenuation to each rendered sample.
The source holds its final distance. Live edits use the session crossfade.

### Tremolo modulation

V2 and V3 support a `tremolo` node. For example,
`{"id":"pulse","type":"tremolo","rateHz":5,"depth":0.6}` applies a
5 Hz sine-shaped gain cycle to all channels on its branch. Rate may range
from 0.01 to 100 Hz. The normalized depth ranges from zero (unity throughout)
to one (unity at the top of the cycle and silence at the bottom). Depth
supports direct automation and musical intent bindings with 10 ms smoothing.

The oscillator starts at unity gain on reset. It advances once per sample,
and live edits with an unchanged topology retain its phase. The node adds
zero latency and has no tail.

Run the 48 kHz example with:

```sh
build-graph/cell_graph_demo headless examples/portable_graph/tremolo.json input.wav pulsed.wav
```

## Shared modulation

`cell.graph.v4` adds `lfos` and `modulation` to the v3 document.
Each LFO has a unique `id` and a `rateHz` from 0.01 to 100 Hz. The optional
`waveform` is `sine`, `triangle` or `square`. The optional `phase` sets
the starting point in cycles from 0 to 1, with 1 equivalent to 0.
Omitting both fields preserves the original zero-phase sine. The triangle
starts by rising from zero, and the square is positive for the first half
of the cycle and negative for the second. All shapes follow the absolute
sample clock.

Each route names its `source` LFO, target `node`, `parameter` and signed
`depth`. A gain node takes `gainDb` modulation in decibels (depth from −72
to +72 dB). A tremolo node takes `depth` modulation in normalized units
(−1 to +1). A stereo pan node takes `pan` modulation with a signed depth
from −2 to +2. The combined pan stays in [−1, +1] and uses the node’s
existing equal-power law on its selected source channel. Multiple LFOs can target a parameter. Their offsets are summed
with its automated base value, then clamped once per sample. The source
wave is shared across destinations and uses prepared scratch storage.

[modulation.json](../examples/portable_graph/modulation.json) routes one LFO
to gain, tremolo depth, and stereo pan with independent amounts and polarity.
Existing v1 through v3 documents retain their previous behavior.

Use `Document::withModulationDepths` or `LiveSession::setModulationDepths`
to change existing routes by source, node and parameter. The entire batch
validates before it is saved or published. A live edit uses the session's
crossfade and can be undone in one step. The CLI writes a new graph file.

```sh
build-graph/cell_graph_demo modulation-depth examples/portable_graph/modulation.json edited.json \
  motion level gainDb 3 motion position pan -0.5
```

An azimuth node also accepts `azimuth` modulation with signed depth
from −360 to +360 degrees. The summed offset follows the source's
automated position and wraps around the circle before listener yaw
and speaker mapping are applied. Distance and speaker layout retain
their existing controls.

[azimuth-modulation.json](../examples/portable_graph/azimuth-modulation.json)
adds a triangle LFO to an animated source in a four-channel speaker
layout. Use a 48 kHz, four-channel input for this example.

A saturator accepts `driveDb` modulation with signed depth from −36 to +36 dB.
The combined automated and modulated drive is clamped to [0, 36] dB.
At 2× through 16× oversampling, each host frame's drive is held across
its oversampled subframes. Latency and tail are unchanged.

### Beat-synced LFOs

Use `beatsPerCycle` in place of `rateHz` to follow the graph's saved
`tempoMap`. A beat means one quarter note. For example, 0.5 beats is an
eighth-note cycle. Values from 0.125 through 128 are supported.

The map starts at sample 0, with up to 1,024 strictly increasing sample
positions and tempos from 1 through 1,000 BPM. Each tempo holds until
the next point. The LFO changes speed at that sample while preserving
its accumulated phase. Free-running LFOs keep their specified Hz rate.
The map is stored in the graph and used in both live and offline playback.

[tempo-modulation.json](../examples/portable_graph/tempo-modulation.json)
includes a tempo change partway through an LFO cycle. Both inspector
commands report the saved map.

### Host transport

A beat-synced LFO can set `"clock": "host"`. Pass a `Transport` to
`Player::process` or `LiveSession::process` to follow the host's
quarter-note position and block-start tempo. Play advances phase, stop
holds the reported beat, and a seek or loop reanchors phase. The graph
uses the saved map when host beat position or tempo is unavailable.
The host clock applies to opted-in LFOs. Graph automation still uses
its own sample timeline.

The graph processor example forwards transport from the SDK engine.
CLAP, VST3, AAX, and VST2 adapters expose beat-position validity when
the host supplies it. Rebuild consumers with the updated `Transport`
header and matching SDK library.

### Graph editor

Open [the Graph Editor](../examples/graph_editor/index.html) in a browser
or open that file locally from the SDK archive. Load a v4 graph or use the
included example. The editor previews the LFO cycle and edits waveform,
phase, and route depths. Save a JSON copy for the graph CLI or replace
a graph in an existing `.cellgraph` project. The editor runs locally.

If your browser restricts local HTML files, serve only the editor directory
with `python3 -m http.server 8765 --bind 127.0.0.1 --directory examples/graph_editor`
and open `http://localhost:8765`.

#### Audition a graph

Build the SDK examples, then start the local editor service:

```sh
python3 examples/graph_editor/serve.py --demo build-graph/cell_graph_demo
```

Open the printed URL, load a graph and choose a WAV at the graph's
sample rate and channel count. **Render preview** runs Cell's offline
renderer on the current graph. The player lets you listen and save the
rendered WAV. Rendering uses the graph's saved tempo map. The server
binds to loopback and removes the temporary render files when the
request ends. Audition accepts WAV files up to 32 MiB.

#### Compare two renders

Render a preview and select **Pin current as A**. Change the graph and
render again to produce B. **Listen A** and **Listen B** switch at the
same playback time and retain the play or pause state. These previews
use the same WAV and graph topology. A new graph or source WAV clears
the comparison. **Save rendered WAV** saves the version currently selected.

#### Edit tempo and LFO clocks

Use the LFO's **Clock** control to switch between a free-running Hz rate,
the saved tempo map, and host transport. The editor converts the starting
rate from the map's first tempo when possible, within the supported rate
ranges. A beat-synced clock creates a 120 BPM map when the graph has none.

Open **Saved tempo map** to edit BPM, insert a point at a sample, or remove
a later point. The first point stays at sample zero. The editor rejects
duplicate sample positions and keeps the points in order. Save a JSON
copy or render a new B preview to compare the edit against a pinned A.

#### Edit modulation routing

**Add LFO** creates a free-running source with a unique ID. Set its clock,
rate, waveform and phase in the source panel. **Add route** lists targets
that support modulation and are available for the selected source. Removing
a source also removes the linked routes shown in the button label.
`Revert edits` restores the graph as loaded.

**Undo** and **Redo** step through the last 32 graph edits. A depth slider
drag counts as one edit. Ctrl/Cmd+Z undoes and Ctrl/Cmd+Shift+Z redoes
when focus is outside a field. Editing after an undo clears the redo
branch. Opening a graph starts a fresh history. A pinned A render
stays available while undo and redo change the current graph.

#### Open and save Cell projects

With the local editor service running, **Open graph or project** accepts
a `.cellgraph` containing a v4 graph. Cell checks the project's digest
and loads the embedded WAV into audition. **Save project** downloads
a new `.cellgraph` with the edited graph and the original WAV bytes,
unless you explicitly choose a replacement WAV. **Save copy** still
downloads just the graph JSON. The editor accepts projects with up to
512 KiB of graph data and 32 MiB of embedded audio.

#### Edit parameter automation

Choose a target in **Parameter automation**, then add a sample position
and a value in that parameter's native units. Edit a point to move it,
change its value, or remove it. Cell applies points in sample order and
retains the order of points that share a sample. Undo and redo include
automation edits. Existing intent and listener-yaw events stay in the
timeline when you edit direct parameter points.

The automation timeline plots saved points for the selected parameter
against its native value range. Choose Fit points for an overview or select
a fixed range from 250 ms to 16 seconds. Enter a Start sample or use Previous
window and Next window to navigate. Click the plot to fill the sample field
for the next point, or enter an exact sample in the numeric field. Dashed
lines connect points in sample order, including when a segment spans the
visible window and both points are offscreen. Audio audition plays Cell’s
rendered automation with interpolation, smoothing, and modulation.
Previous point and Next point move the sample cursor between distinct
point positions for the selected parameter. A jump brings an offscreen
point into the visible window and only changes the view and sample field.
Use Locate on a point’s row to jump directly to its sample on the timeline.
Enable Show points in timeline window to filter the list as you page or zoom.
Previous point and Next point still navigate the full lane.
Use Copy value to fill the Add point form from a saved point. Pick a
destination sample and choose Add point to commit the copy.

Drag a plotted point to change its sample and value. The canvas previews
the move until you release it, then records one undoable edit. A canceled
pointer gesture leaves the point unchanged. The numeric fields remain
available for exact edits, including points that overlap visually.
