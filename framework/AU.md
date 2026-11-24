# Build a macOS Audio Unit effect

## AUv2 component

Use `cell new --format au` to start from the gain processor with stable
component codes and optional Designer exports. [Generated AUv2 projects](PROJECT-GENERATOR.md#build-a-generated-audio-unit-component)

The AUv2 adapter builds a macOS `.component` from an ordinary Cell
processor library. It supports one matched stereo input and output
bus. Set `supportsMono` on the processor descriptor to add mono.
The host negotiates planar float audio. The adapter exposes stable
Cell parameter IDs, physical-value ranges, and block-rate edits.
It reports latency and tail in seconds and stores the Cell engine
state in the Audio Unit class info property.

Use Xcode with C++23 support and Apple's separate [AudioUnitSDK
1.4.0 checkout](https://github.com/apple/AudioUnitSDK/tree/AudioUnitSDK-1.4.0).
The reference build targets macOS 15.

```sh
cmake -S framework -B build-au -G Xcode \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCELL_SDK_EXAMPLES=ON -DCELL_SDK_TESTS=ON \
  -DCELL_GAIN_AU_SDK=/path/to/AudioUnitSDK \
  -DCELL_FAST_FLOAT_INCLUDE_DIR=/path/to/fast_float/include
cmake --build build-au --config Release --target CellGain_AU cell_au_gain_host
```

An installed SDK consumer can call the same helper:

```cmake
cell_add_au_plugin(MyGain_AU PROCESSOR my_processor
    ID com.example.mygain.au NAME "My Gain" COMPANY "My Company"
    AU_SDK "/path/to/AudioUnitSDK"
    AU_SUBTYPE MyGn AU_MANUFACTURER MyCo VERSION 1.2.3 VERSION_CODE 66051)
```

Use four-character component codes unique to the product. Preserve
these codes, the bundle identifier and the processor's parameter IDs
once projects use the plug-in. `VERSION_CODE` is the decimal value
of the AU version's three one-byte fields, with the major version
first.

The macOS CI jobs install and ad hoc sign the reference component in
the runner's user plug-in directory. They run `auval -v aufx CGan Cell`
and a Core Audio render check. The render check discovers the registered
component, restores the saved gain into fresh instances, renders mono
and stereo at 48 kHz, and verifies a gain update and rejection of invalid
state. The workflow publishes the validation and render logs.

### Bundle a Designer editor in AUv2

Use **Export WebView HTML** and **Export C++ bindings** in the
[Cell UI Designer](WEBVIEW.md#design-a-parameter-editor). Enable
`CELL_SDK_WEBVIEW=ON` in a source build. An installed SDK consumer enables
Objective-C++ and requests the `WebView` component.

```cmake
cell_add_au_plugin(MyGain_AU PROCESSOR my_processor
    ID com.example.mygain.au NAME "My Gain" COMPANY "My Company"
    AU_SDK "${AUDIO_UNIT_SDK}"
    AU_SUBTYPE MyGn AU_MANUFACTURER MyCo
    AU_EDITOR_HTML "${CMAKE_CURRENT_SOURCE_DIR}/cell-plugin-editor.html"
    AU_EDITOR_BINDINGS "${CMAKE_CURRENT_SOURCE_DIR}/cell-ui-bindings.hpp"
    AU_EDITOR_METERS)
```

The editor uses a private WKWebView inside the host's Cocoa view. It reads
host parameter changes and sends edits and automation gestures through Core
Audio. `AU_EDITOR_METERS` enables output peak and RMS while a view is active,
including when the effect is bypassed. The AUv2 builder also supports a DSP-only
component whose controls appear in the host's generic editor.

For the gain example, choose **Load showcase** in the Designer and export both files
into one directory. Add `-DCELL_SDK_WEBVIEW=ON` and
`-DCELL_GAIN_AU_EDITOR_DIR=/path/to/exports` to the reference configuration.
Build `CellGain_AU` and `cell_au_cocoa_check`. After installing and signing the
component, run `build-au/gain-example/Release/cell_au_cocoa_check` from a
logged-in Mac desktop. The check opens the editor through Core Audio and
exercises resizing, two-way gain, automation gestures, and live meters.

The component and Cocoa check cross-build for Apple silicon and Intel. Native
Cocoa and DAW qualification remain pending. Restart the DAW after replacing
a component whose editor was opened so WKWebView can release its delegates.

## AUv3 app extension

The shared AUv3 adapter builds an app extension for macOS and iOS with the system SDK.
Audio effects use planar Float32 with matched main input and output layouts.
The adapter supports the host's parameter editor, scheduled points and ramps,
bypass, latency and tail reporting, and Cell engine state.
Processors can also receive [host tempo and transport](TRANSPORT.md).

Declare symmetric pairs in `PluginDescriptor::mainBusLayouts` to offer mono,
stereo, 5.1, 7.1.2, 7.1.4 or 9.1.6. The descriptor's channel count selects the
preferred pair when one matches. The adapter maps each bus by speaker role to
Cell's processing order, including front-wide and height channels. The host
can use different channel orders on input and output for the same speaker layout.
Multichannel formats require speaker labels or a recognized Core Audio layout
tag or bitmap. Existing descriptors without named layouts use the stereo and
optional mono configuration.

Set `PluginDescriptor::supportsSidechain` to publish a second input bus named
`Sidechain`. The host can configure it as mono or stereo independently of the
main input. Configure and enable the bus before allocating render resources.
The format is planar Float32 at the main bus sample rate.

`Processor::processSidechain` receives the key signal at the same sample offset
as the main signal, including during parameter ramps. A disabled or disconnected
bus arrives as an empty block. A connected silent bus arrives with its
negotiated channel count and zeroed samples.

Pass `AU_TYPE aumf` to `cell_add_auv3_plugin` for an audio effect with MIDI
input or output. Set `PluginDescriptor::acceptsMidi` for input and
`PluginDescriptor::producesMidi` for output. The regular audio effect uses
`aufx` by default.

Each enabled MIDI direction exposes one cable, numbered `0`. The adapter
handles MIDI 1.0 channel messages including notes, control changes, pressure,
program changes and pitch bend. Core Audio can translate supported messages
through its event-list API. A render with more than 4096 scheduled events is
rejected and resets the processor's MIDI state.

Processors that generate or transform MIDI override `routesMidi()` and
`processMidi()`. MIDI output retains its sample offsets through parameter
automation. When the host rejects an event, the adapter clears sustain and
releases notes before sending later MIDI. Bypass forwards incoming MIDI
unchanged. Reset, state recall, and bypass changes schedule note release on
the next render with a valid timestamp and output callback.

For a dedicated MIDI processor, set `channels = 0` and `producesMidi = true`
and build with `AU_TYPE aumi`. Set `acceptsMidi` when the processor consumes
host MIDI. Leave the audio layouts, mono option and sidechain disabled.
The processor receives zero audio channels, a frame count and the host's
transport. AUv3 exposes a silent mono or stereo output bus for the host's
render clock. The sample rate of that bus sets the processor's rate.

For a MIDI instrument, pass `AU_TYPE aumu` to `cell_add_auv3_plugin`, set
`acceptsMidi = true`, set `channels` to the preferred output count, and leave
`supportsSidechain` disabled. The host sees one audio output and no audio input
busses. The processor receives a cleared planar output block and sample-timed
MIDI. Set `supportsMono` for an optional mono output on a stereo instrument.
For named layouts, `mainBusLayouts` supplies the available output layouts through
each pair's `output` member. Bypass mutes audio and continues processing MIDI
so that note-offs during bypass are applied.

The adapter advertises ramps for automatable parameters with a step below
`1` and no bypass flag. Ramps interpolate in physical parameter units and
continue across render blocks. A point edit replaces an active ramp for the
same parameter. Reset and successful state recall clear in-flight ramps.

On macOS 15 with Xcode, build the reference app and its embedded extensions:

```sh
cmake -S framework -B build-auv3 -G Xcode \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
  -DCELL_FAST_FLOAT_INCLUDE_DIR=/path/to/fast_float/include \
  -DCELL_SDK_AUV3_EXAMPLE=ON
cmake --build build-auv3 --config Release --target CellAUv3Host
```

The local reference test signs and registers the app and its three extensions with
macOS. It requests out-of-process instantiation and checks state recall,
scheduled points and ramps, and bypass in both stereo channels at 48 kHz.
It also checks the MIDI processor's silent mono clock bus at 44.1 kHz,
sample-accurate note timing across a transpose edit, and MIDI generation
without incoming events. The instrument check renders a note, a level edit,
and a note-off at exact sample positions in stereo and mono. It also checks
that instrument bypass mutes the output. The reference host requests Cell
Gain's custom editor after checking its audio and state.

```sh
app="$PWD/build-auv3/macos-auv3/Release/CellAUv3Host.app"
plugins="$app/Contents/PlugIns"
entitlements="$PWD/framework/cmake/CellAUv3MacSandbox.entitlements"
extensions=('Cell Gain.appex' 'Cell MIDI Probe.appex' 'Cell Instrument Probe.appex')
for extension in "${extensions[@]}"; do
  codesign --force --sign - --entitlements "$entitlements" "$plugins/$extension"
done
codesign --force --sign - --entitlements "$entitlements" "$app"
codesign --verify --deep --strict "$app"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$app"
for extension in "${extensions[@]}"; do
  pluginkit -a "$plugins/$extension"
done
"$app/Contents/MacOS/CellAUv3Host"
```

The Apple CI workflow runs this check on Apple Silicon and Intel and saves
the registration and render logs. The reference components are Cell Gain
(`aufx Cv3G Cell`), Cell MIDI Probe (`aumi CtFx Cell`), and Cell Instrument
Probe (`aumu CtIn Cell`).

Installed SDK consumers use the same `cell_add_auv3_plugin` helper and Xcode
embedding properties on both Apple platforms. On macOS the helper supplies
app sandbox entitlements for the extension. Use the same signing identity
for the containing app and extension when signing a release. The helper
and iOS build are documented in [Embed Cell on iOS](IOS.md#build-and-run-the-auv3-reference-app).

## Bundle a designer editor

Use **Export WebView HTML** and **Export C++ bindings** from the same layout in
[the Cell UI Designer](WEBVIEW.md#design-a-parameter-editor). Pass both files to
the AUv3 helper on macOS or iOS.

```cmake
cell_add_auv3_plugin(MyGain_AUv3 PROCESSOR my_processor
    ID com.example.mygain.effect NAME "My Gain" COMPANY "My Company"
    AU_SUBTYPE MyGn AU_MANUFACTURER MyCo
    AU_EDITOR_HTML "${CMAKE_CURRENT_SOURCE_DIR}/cell-plugin-editor.html"
    AU_EDITOR_BINDINGS "${CMAKE_CURRENT_SOURCE_DIR}/cell-ui-bindings.hpp"
    AU_EDITOR_METERS)
```

With `CELL_SDK_AUV3_EXAMPLE=ON`, set `CELL_GAIN_AUV3_EDITOR_DIR` to the folder
containing those two exports to try a layout in `CellAUv3Host`.

Cell packs the HTML into the app extension and serves it in a private
system WKWebView. The generated binding file supplies the allow-list of up to
128 distinct keys from the processor descriptor. Custom HTML can use
`AU_EDITOR_PARAMETERS` with an explicit key list in place of the bindings.
The editor receives physical
values, ranges, units, steps and choices through the same messages as the
standalone WebView example. Slider and choice changes go through the Audio
Unit parameter tree with touch, value and release events for host automation
recording. The view reflects host automation and recalled values. It ends
active gestures when hidden or closed. Add `AU_EDITOR_METERS` to expose
read-only peak and RMS from the plug-in output as `output-peak` and
`output-rms`. The extension captures these levels while the editor is visible.
For a meter-only view, supply `AU_EDITOR_METERS` with the HTML file.

The AUv3 helper links CoreAudioKit and WebKit for a custom editor. A target
with no editor arguments keeps the host's parameter editor. The built-in
bundle uses offline HTML with remote navigation and resource requests blocked.
The [Cell Gain editor export](../examples/auv3_gain/Cell%20Gain%20Editor.html) is
included in the standalone source package.
