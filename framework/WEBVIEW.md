# Optional original system-WebView host

`Cell::WebView` embeds a system browser as a native GUI child without JUCE or
an embedded Chromium distribution. It is an original host/bridge implementation,
not a browser engine. It is optional and does not replace Canvas or change the
mixing suites' editors. DSP and audio-device processing stay native.

## Backends, requirements and actual qualification

| Platform | Backend / deployment | Current evidence |
| --- | --- | --- |
| Windows | Official WebView2 headers to build, WebView2Loader.dll and a compatible installed WebView2 runtime, writable application-owned absolute profile directory | Backend compiled using official SDK 1.0.4370.26 and LLVM-MinGW. Native Windows browser and DAW qualification remains required. |
| macOS | System WKWebView / Cocoa, main-thread NSView parent, private nonpersistent data store | SDK and CLAP example cross-built for Apple silicon and Intel. Native Mac qualification remains pending. |
| Linux | X11 GtkPlug child using GTK3 and WebKitGTK 4.1 loaded dynamically; private ephemeral context | Native X11 child lifetimes, two-way JSON, physical resize and visibility/focus calls checked in an owned display using a portable runtime. Native Wayland embedding is not implemented. |

The source archive does not distribute Microsoft headers/loader/runtime or
Linux GTK/WebKit libraries. Apply each deployment's own licenses. WebView2 is
not bundled into a product automatically. Linux missing libraries produce an
availability/open error; they do not select an unrelated renderer silently.
The native runtime must coexist with the host's GUI loop and GTK initialization.
Do not change GTK's thread/display backend after the host initializes it.

Build the component:

```sh
cmake -S framework -B build-sdk-web -DCELL_SDK_WEBVIEW=ON
cmake --build build-sdk-web --parallel 2
```

On Windows add `-DCELL_WEBVIEW2_INCLUDE_DIR=/path/to/official/include` pointing
to the directory containing WebView2.h. Consumers link the installed component:

```cmake
find_package(Cell CONFIG REQUIRED COMPONENTS Core WebView)
target_link_libraries(my_editor PRIVATE Cell::WebView)
```

The WebView component does not require Blend2D or Cell::UI. The optional example
uses Cell::UI only to supply a portable native parent window.

## Native lifetime and geometry

All View operations and destruction occur on its creating GUI thread. Pass a
live native HWND, X11 Window ID, or macOS NSView pointer as `Options::parent`.
Bounds use **physical parent-client pixels** by default. Set `Options::boundsUnits`
to `BoundsUnits::parentCoordinates` when a Cocoa host supplies points. On other
backends the parent coordinates are physical pixels. `parentPixelScale()` reports
the number of physical pixels per native parent unit. Pass offsets from the top
left of the parent and reapply bounds on host resize or DPI changes.

`availability()` probes whether the backend is available; it cannot guarantee
that asynchronous browser creation will succeed. `open()` can start creation;
watch `ready`, `loaded` and `error` events and the `ready()` state. `loadHtml()`
can queue content until readiness. Drain `poll()` from the host GUI loop, which
must keep dispatching its own native messages. The Linux backend additionally
services a bounded number of pending GTK iterations. Never block the audio
thread or invent a second GUI owner to make a WebView responsive.

Before replacing state or closing an editor, cancel owned parameter gestures,
close the browser and discard stale callbacks/results. **A dynamically loaded
plugin must keep its code loaded until `View::readyToUnload()` returns true on
the GUI thread.** In particular, WebView2 environment/controller creation can
finish after the editor closes. Close is not proof that all COM callbacks have
settled. macOS delegate runtime classes are disposed only after their instances
are gone. A format wrapper must explicitly integrate this teardown contract
before enabling a browser editor; the SDK addition alone does not retrofit that
into all current plugin wrappers.

Linux WebKit/GTK libraries are retained for process lifetime once loaded because
they register process-global types and workers. View resources and Cell signal
callbacks are released on close. A host may already retain those libraries too.
Do not equate a closed editor with unloading the system browser engine.

## Bounded messages and parameters

Page-side bridge, injected before document scripts:

```js
cell.postMessage({type: 'parameter', key: 'gain', value: -12.1});
const unsubscribe = cell.onMessage(message => {
  // Refresh visible controls from a native snapshot.
});
```

Native-side adapter:

```cpp
#include <cell/webview.hpp>
#include <cell/web_parameter_bridge.hpp>
const std::array allowed{cell::stableId("gain")};
cell::web::ParameterBridge parameters(engine, allowed); // engine outlives this
// After opening View with a native parent, from each GUI tick:
for (const auto& event : view.poll())
    if (event.kind == cell::web::Event::Kind::message)
        parameters.apply(event.text);
// Throttle/coalesce snapshots to the editor's actual refresh requirements:
if (view.ready()) view.send(parameters.snapshot());
```

`ParameterBridge` accepts only declared parameter keys on its explicit allow-list
(maximum 128). Messages are `begin`, `parameter` with a physical `value`, or
`end`; implicit one-shot edits are also supported. It validates types, finite
ranges and quantization. Native Engine gestures and host edits remain the source
of truth. On focus loss/cancel/close call `cancel()` so an abandoned drag cannot
retain a gesture. The bridge is GUI-thread code and must not be called from
native browser worker threads or the audio callback.

Messages are limited to 64 KiB; events are bounded to 128. Oversized/malformed
JSON and overflow report errors. HTML is limited to 8 MiB and evaluated scripts
to 1 MiB. Use small parameter/display messages rather than audio buffers,
complete sample libraries or full session snapshots. Applications should coalesce
refreshes and check send/apply results, not silently lose a gesture under load.

## Content, runtime cost and missing integrations

Offline HTML is the default. The loader prefixes a policy permitting inline
script/style and embedded data/blob images while blocking remote connections,
frames and forms. Remote HTTP(S) navigation needs explicit
`allowRemoteContent=true`; this is not a general RPC API. File import, state,
network services and model access must be separate explicit native features.
No arbitrary filesystem/process bridge is provided.

The Windows profile option applies only to WebView2. Linux/macOS currently
always use ephemeral stores. Developer tools default off. Default browser
behavior may consume keys differently from native controls: DAW space-bar and
shortcut policy, text entry, accessibility, focus, multiple views and
cross-platform styling need actual product/host qualification. A browser process
uses startup time and memory even while native DSP remains independent; measure
its cost before choosing it for every module.

`examples/webview_editor` contains a standalone native-window demo and a
command-line check for the parameter bridge. Plug-in integrations are described
below. DSP stays in the native processor.

## Design a parameter editor

Open `examples/ui_designer/index.html` in a browser. Drag knobs, faders,
sliders, steppers, toggles, menus, meters, readouts and labels onto the grid. Select
a control to edit its size, position, style and binding. Drag the selected
corner to resize, or use Shift+arrow keys. Arrow keys move the selected
control. Duplicate and Remove are available in the inspector.

Shift-click or press Shift+Space on a focused control to add it to the
selection or remove it. On a touchscreen, turn on **Select multiple**
and tap the controls. Drag any selected control or use the arrow keys
to move the group. The inspector aligns selected controls to any edge
and spaces three or more controls within the selection's bounds.
Duplicating a group finds room on the active grid for the same arrangement.
The designer flags any overlap on the wide grid when duplicating from the
phone grid. Use **Undo** to reverse a group edit in one step.

**Copy selection** and **Paste controls** transfer one or more controls
within a layout or between designer tabs. Ctrl/Cmd+C and Ctrl/Cmd+V
also work on the canvas. Copies include parameter bindings, artwork,
and the current arrangement on both grids. Paste keeps that arrangement
and moves it into open space when needed. Both grids need room for the
group. The destination's theme and
parameter catalog apply. If the browser declines clipboard access, the
toolbar can use a copy kept in the current tab.

**Load showcase** opens a layout with a 1940s field-radio knob, a 1960s
recording-studio fader and 1950s broadcast-style needle meters. Choose a
different style on any control. The styles are original interpretations.
Resize controls, set editor colors, or embed SVG, PNG, JPEG or WebP artwork on a
knob or fader. Face artwork can use up to 256 KiB per control and raster
dimensions up to 2048 × 2048 pixels.

### Design the panel

Drag **Image**, **Rectangle**, and **Line** onto the grid. Use **Image** for
logos, markings, and illustrations. Its label is the image's accessible
description. Use the accent color picker to set a rectangle or line color.
Lines can be horizontal or vertical and 1–16 pixels thick. Open **Panel
background** for an image that fills the entire editor. Images and the
background offer **Fit**, **Fill and crop**, and **Stretch**.

Rectangles and lines sit above the background, followed by images and
interactive controls. Turn on **Edit panel artwork** to reach artwork behind
the controls. Artwork elements can have their own wide and phone positions.
The exported editor displays artwork offline and keeps its controls available
for pointer and keyboard input. Artwork has no parameter binding.

Each Image or background file can use up to 1 MiB, with raster dimensions
up to 4096 × 4096 pixels. Use self-contained SVGs for scalable artwork.
SVG paths, text, gradients, and local references are supported. Export the
artwork with scripts, animation, and external references removed. The layout
file can hold up to 2 MiB, including embedded artwork.

The native WebView reference can load an exported document with
`cell_web_editor_example --html cell-plugin-editor.html`. Build it with
`CELL_WEB_EXAMPLE_WINDOW=ON` and the SDK WebView and UI options enabled.
Use the `gain` key for this reference processor. Host-provided metadata
sets the range, step, units, and current value of each bound control.
Load HTML from authors you trust.

To bundle the same exports in a macOS or iOS AUv3 extension, pass
`AU_EDITOR_HTML` and `AU_EDITOR_BINDINGS` to the AUv3 helper. The host
opens the editor through Core Audio and records its edits through the Audio
Unit parameter tree. See [Bundle a designer editor](AU.md#bundle-a-designer-editor).

### Add hardware-style selectors

Add a **Stepper** for a module code or any stepped parameter. Integer values
display as `00`, `01`, and so on. A parameter's choice name is available
from the value's tooltip and accessible label. Use **−** or **+** to change
one step, or hold either button to repeat. Arrow keys also step when either
button has focus. Holding a button sends one host automation gesture and
stops at the parameter limit.

Bind a knob to a parameter with 2–32 discrete positions to show individual
detents and highlight the current position. Drag the knob or use the arrow
keys to select a stop. The knob retains its selected skin and artwork.
The same controls and parameter bindings work in live preview and exported
editors.

### Style panel text

Open **Typography** in the inspector to set a control's font family, size,
weight, tracking, alignment, and letter case. Add line breaks directly in
the **Label** field for text elements, control headings, and button captions.
Labels can contain up to 240 characters across eight lines.

**Silkscreen** fills the inspector with a condensed, uppercase style and
increased letter spacing. Adjust the fields and click **Apply**. Use
**Reset typography** and **Apply** to return the control to the panel defaults.

Panel Sans is based on Inter and includes variable weights. Panel Condensed
is based on Rajdhani and includes weights from 300 through 700. The HTML
export embeds the selected bundled fonts and their licenses. System font
choices use the device's own fonts. The seven-segment display keeps its
segment geometry while its heading follows the typography settings.

### Share a panel across device modes

Expand **Conditional behavior** for any control. Put `device = 0` in
**Visible when** for the row bound to `dist_guitar` and `device = 1` for
the row bound to `dist_bass`. Both rows can occupy the same grid slot.
When the wide boxes match and their rules are mutually exclusive, automatic
phone placement gives them the same slot. Use **Conditional control** above
the canvas to select a row covered by another row while editing.

Use **Enabled when** to leave a control visible while restricting input,
for example `tuner = 0 && device >= 0`. Conditions compare plain numeric
parameter values. Use `=`, `!=`, `<`, `<=`, `>`, and `>=`, with
`&&` evaluated before `||`. Use double quotes around keys with spaces.
Each condition supports up to 12 comparisons. A comparison with a missing
source is false.

**Label from** takes another parameter key. Use its current choice label,
parameter name, or formatted value, or map each value to your own label.
For example, map `0 = COMP` and `1 = COMP/LIMIT` on a control that gets
its own value from `comp`. The ordinary Label field supplies the fallback
when the source or a matching label is unavailable.

The exported editor evaluates rules from the host's parameter snapshots.
If a control becomes hidden or disabled during an edit, the editor ends
that gesture and releases momentary controls. The C++ export includes any
extra source parameters as read-only snapshot bindings.

### Add buttons and product commands

Use **Momentary** for a parameter that goes to its maximum on press and
returns to its minimum on release. The bridge reserves the release and
gesture end together before accepting the press. **Toggle** latches between
the two values. Both use the control's label as their caption.

Use **Action** for a named plug-in command. The inspector offers **Tap**,
**Press and release**, **Long press**, and **Hold to repeat**. For example,
bind a button labelled STORE to the product's `store` action and set
**Hold time** to 2000 ms. Releasing early cancels the command. The Designer
preview reports the command and phase so you can check the layout.

**Action Pair** gives minus and plus their own command keys. Add a
**Two-button action key** to fire a third command after both buttons have
been held for the specified time. A Stepper also supports this two-button
action while its individual buttons edit its parameter. With a two-button
action available, a single quick tap activates on release and a held
single button starts repeating after 350 ms. Starting both within that
interval suppresses the individual actions. Release either button early to
cancel the chord. Both touch points can participate. For keyboard use,
focus the pair and hold Left and Right together.

Implement `cell::web::ActionTarget` on the processor to expose product
actions. `supportsWebAction` declares a key and mode. `webAction`
accepts an invoke, press, or release on the control thread. A successful
press must leave its release available through editor cancellation.
The C++ export includes a separate `cell::designer::actions` allow-list.
Only actions that the layout lists and that the product supports are
enabled. The editor handles the hold timing. The processor owns any
product-specific validation and state changes.

The Cell Gain WebView example implements `factory-recall` and recalls its
default trim. CLAP and Android Designer carry action bindings to their
processing engines. The target product defines its own store, initialize,
tuner, and patch-navigation behavior.

### Show hardware display codes

Add an **LED Display** and bind its parameter key. The display has two
seven-segment character cells with dim unlit segments. Unmapped integer
values from 0 through 99 are zero-padded. Integers from −9 through −1 use
one cell for the minus sign. Other unmapped values show `--`.

In the inspector, enter one plain parameter value and code per line in
**Display codes**. **Bound choices** lists the values and labels from the
loaded parameter snapshot. The character mapping is case-sensitive and
supports codes such as `A1`, `oF`, `L7`, `Ac`, and `?1`. A single
character aligns to the right. **Supported characters** lists all available
glyphs, including the question mark with its dot.

```text
0 = A1
1 = oF
2 = L7
3 = Ac
4 = ?1
```

You can also add codes to a **Stepper**, **Readout**, or **Choice**. Turn on
**Segmented digits** on a Stepper to combine the display with its minus and
plus buttons. Codes live in the layout. Menus, host labels, and automation
keep the plug-in's chosen labels and values. On mapped outputs, the full
choice label is available to screen readers and in the tooltip.

### Bind a bank of programs

Add **Bank**, **Program**, and **Patch Name** for a split layout, or
**Program Grid** for direct access to every populated slot. Bank selects
which slots the Program control browses. Choosing a program or clicking a
grid button requests recall. The patch name and active slot reflect the
next snapshot from the plug-in.

Program controls bind to a separate memory catalog. Implement
`cell::ProgramMemory` on a processor to publish its banks, named slots,
and current address. Use `Engine::editTransaction` for parameter-only
patches. A processor whose patches include other state supplies its own
safe publication mechanism. The product owns patch contents, persistence,
and any slot editing. The GUI requests recall by bank and program index
using `{"type":"recall-program","bank":0,"program":5}`. The bridge accepts
only populated addresses from the published catalog.

The Cell Gain reference exposes six banks of six real gain patches.
Run `cell_web_bridge_example --parameters > parameters.json` and load that
snapshot in the Designer to preview its names and slot selection. Live
preview simulates the selection. Exported HTML uses the connected
instance's catalog and the reference applies the patch to its gain.

The Engine-backed CLAP and Android Designer bridges and the native
WebView reference can use this interface. Separate-controller VST3, LV2
UI, AU, and AUv3 editor adapters need a connection to their processing
instance's program endpoint. A bridge without a program catalog shows
the controls as unavailable.

### Start a project from Designer exports

Use `cell new MyGain --format clap --format vst3 --designer-dir /path/to/exports`
to copy both exports into a new project and wire its plug-in targets to the editor.
Add `--format lv2` to build an LV2 bundle with the same editor on Linux X11.
[Project generator instructions](PROJECT-GENERATOR.md#start-a-project-with-a-designer-layout)

### Bundle a designer editor in CLAP

Use **Export WebView HTML** and **Export C++ bindings** from the same layout.
Build the SDK with `CELL_SDK_WEBVIEW=ON` or request `WebView` from the installed SDK.
Pass both exports to the CLAP helper.

```cmake
cell_add_clap_plugin(MyPlugin
    PROCESSOR my_processor ID com.example.my-plugin NAME "My Plugin"
    CLAP_SDK "${CLAP_SDK}"
    CLAP_EDITOR_HTML "${CMAKE_CURRENT_SOURCE_DIR}/cell-plugin-editor.html"
    CLAP_EDITOR_BINDINGS "${CMAKE_CURRENT_SOURCE_DIR}/cell-ui-bindings.hpp")
```

The build uses Python 3 to embed the page in the plug-in. For the gain
example, set `CELL_GAIN_CLAP_SDK` to the CLAP checkout and
`CELL_GAIN_CLAP_EDITOR_DIR` to the folder with both exports. The host must
support embedded editors. The editor sends parameter changes and gestures
through CLAP and displays live `output-peak` and `output-rms` meters.

- On macOS, the `.clap` bundle embeds WKWebView in the host's main-thread
  `NSView` and sizes it in Cocoa points. The Cocoa main run loop supplies
  editor updates. Opening the editor keeps the plug-in image mapped until
  the DAW exits so detached WebKit delegates can finish safely. Restart the
  DAW after replacing a plug-in whose editor was opened.

- On Windows, ship the architecture-matched official `WebView2Loader.dll`
  beside the `.clap` file. The host needs the Evergreen WebView2 Runtime and
  an STA (single-threaded apartment) GUI thread with a Win32 parent window.
  The plug-in stores WebView2 data under
  `%LOCALAPPDATA%\Cell\WebView2\CLAP\plugin-<id>` and serves the embedded HTML
  from memory. Starting the editor keeps that plug-in image mapped until
  the DAW exits so WebView2 can finish pending callbacks after editor close.
  Restart the DAW after replacing a Windows plug-in whose editor was opened.
- On Linux, install GTK 3 and WebKitGTK 4.1 and run the host with an X11
  display.

Windows and Linux hosts provide editor ticks through `clap.timer-support`.

For a native macOS check, choose **Load showcase** in the designer before
exporting both files. With the gain example configured, build and run the
Cocoa qualification host from a logged-in macOS desktop. It checks Cocoa
resizing, two-way gain edits, closing during startup, and reopening in a
CLAP host without the timer extension.

```sh
cmake --build build-sdk-web --target cell_clap_cocoa_check
build-sdk-web/gain-example/cell_clap_cocoa_check \
  'build-sdk-web/gain-example/Cell Gain Example.clap'
```

The gain example was checked in REAPER 7.86 on Linux X11 for two-way gain
updates across editor close and reopen. An X11 test host also checked native
resize, peak and RMS levels from processed audio, and gesture cancellation.
The macOS plug-in and qualification host cross-build for Apple silicon and
Intel. The native Mac check and DAW qualification remain pending.
The Windows gain example cross-builds for x64 and Arm64. An x64 COM fixture
in Wine checked close during startup, the delivery of a page larger than
2 MiB, resize, parameter gestures, and missing-runtime handling. Testing
the browser in a DAW on Windows remains a release qualification step.

### Bundle a designer editor in VST3

On macOS, Windows, or Linux X11, enable `CELL_SDK_WEBVIEW` or request `WebView`
from the installed SDK. Add the two Designer exports to the VST3 target after
registering the official Steinberg SDK with CMake.

```cmake
cell_add_vst3_plugin(MyPlugin
    PROCESSOR my_processor ID com.example.my-plugin NAME "My Plugin"
    COMPANY "Example Audio"
    VST3_EDITOR_HTML "${CMAKE_CURRENT_SOURCE_DIR}/cell-plugin-editor.html"
    VST3_EDITOR_BINDINGS "${CMAKE_CURRENT_SOURCE_DIR}/cell-ui-bindings.hpp")
```

For the gain example, set `CELL_GAIN_VST3_SDK` and `CELL_GAIN_VST3_EDITOR_DIR`.
Use the embedded editor or the `UI` option for a given target.

- On macOS, the host supplies an `NSView` on its main thread. The browser
  uses Cocoa points for sizing and the Cocoa run loop for updates. Opening
  the editor keeps the plug-in module loaded until the DAW exits so WebKit
  can release detached delegates safely. Restart the DAW after replacing a
  plug-in whose editor was opened.
- On Windows, the host supplies an `HWND` on its STA GUI thread. Install the
  Evergreen WebView2 Runtime and place the matching official `WebView2Loader.dll`
  beside the binary in `Contents/x86_64-win` or `Contents/arm64-win`. For a flat
  plug-in, place the loader beside the `.vst3` file. WebView2 data lives under
  `%LOCALAPPDATA%\Cell\WebView2\VST3\plugin-<id>` and the browser uses the
  monitor's DPI. Opening the editor keeps the plug-in module mapped until the
  DAW exits so late WebView2 callbacks can finish safely. Restart the DAW
  after replacing a plug-in whose editor was opened.
- On Linux, the host supplies X11 embedding and an `IRunLoop` through
  `IPlugFrame` or the host context. GTK 3 and WebKitGTK 4.1 provide the browser.

The controller sends parameter edits and gestures through VST3 automation.
The editor reads parameter values from the controller and receives peak and
RMS from the separate processor through the VST3 connection between them.
Meter messages run on the GUI thread. The audio thread measures output only
while an editor requests readings.

For a native macOS check, choose **Load showcase** in the Designer and export
both files for the gain example. In a single-configuration Release build, run
the Cocoa test host with the generated VST3 bundle from a logged-in Mac desktop.
The host checks resizing, two-way gain edits, peak and RMS readings, and close
and reopen.

```sh
cmake --build build-sdk-web --target cell_vst3_cocoa_check
build-sdk-web/gain-example/cell_vst3_cocoa_check \
  'build-sdk-web/VST3/Release/Cell Gain Example.vst3'
```

The macOS example and test host cross-build for Apple silicon and Intel.
The native Mac check and DAW qualification remain pending.

The gain example was checked in REAPER 7.86 on Linux X11. Closing and reopening
the editor preserved host gain changes, and its two output meters matched the
peak and RMS of a played test tone. An X11 test host also checked physical
resize, balanced gestures, and closing from inside a host callback.
The Windows example cross-builds for x64 and Arm64. An x64 VST3 host under
Wine checked HWND resizing, parameter gestures, meter delivery, and early
close with a WebView2 COM fixture. The official loader was also checked
with the runtime absent. Native Windows DAW qualification remains pending.

### Bundle a designer editor in LV2

On Linux X11, enable `CELL_SDK_WEBVIEW` or request the installed `WebView`
component. Pass both Designer exports when declaring the LV2 target. This
adds a separate X11 editor module to the `.lv2` bundle.

```cmake
cell_add_lv2_plugin(MyPlugin
    PROCESSOR my_processor URI https://example.com/my-plugin
    LV2_SDK "${LV2_SDK_ROOT}"
    DESCRIPTION "${CMAKE_CURRENT_SOURCE_DIR}/plugin.ttl"
    LV2_EDITOR_HTML "${CMAKE_CURRENT_SOURCE_DIR}/cell-plugin-editor.html"
    LV2_EDITOR_BINDINGS "${CMAKE_CURRENT_SOURCE_DIR}/cell-ui-bindings.hpp"
    LV2_EDITOR_METER_PORT 5)
```

Set `CELL_GAIN_LV2_SDK` and `CELL_GAIN_LV2_EDITOR_DIR` to try this with the
gain example. Install the entire generated `.lv2` directory in the host's
LV2 search path.

The editor uses GTK 3 and WebKitGTK 4.1. The host supplies an X11 parent and
calls the LV2 idle interface. The editor exchanges parameter values in their
native units and uses host touch and resize support when offered. In a GTK 2
host the custom editor declines to open. The separate DSP module remains
available.

If the design contains output meters, set `LV2_EDITOR_METER_PORT` to the first
free LV2 port index after the audio and parameter ports. Calculate it as
`2 * channels + parameter_count`. Cell adds peak at that index and RMS at the
next index. These optional control outputs report linear amplitude over the
entire host block. The editor displays both readings in dBFS.

The gain example was checked with Jalv GTK 3 on Linux X11. A Designer fader
edit changed the gain measured on the JACK audio output. An X11 test host
also checked resize, host automation, meter readings and close and reopen.
The DSP and editor cross-build for Linux Arm64. The DSP runs under QEMU.
Raspberry Pi hardware and its desktop editor still need a runtime check.

### Bundle a designer editor in AUv2 or AUv3

Pass both Designer exports as `AU_EDITOR_HTML` and `AU_EDITOR_BINDINGS`.
Use `cell_add_au_plugin` for a macOS AUv2 component or `cell_add_auv3_plugin`
for a macOS or iOS app extension. Add `AU_EDITOR_METERS` for live peak and RMS.

- Set `CELL_GAIN_AU_EDITOR_DIR` to use your exports in the AUv2 gain component.
  [AUv2 build instructions](AU.md#bundle-a-designer-editor-in-auv2)
- Set `CELL_GAIN_AUV3_EDITOR_DIR` to use your exports in the AUv3 gain reference app.
  [AUv3 build instructions](AU.md#bundle-a-designer-editor)

### Run a Designer layout on Android

The standalone Android gain app uses the Designer's same HTML and generated
bindings. It supports phone and wide layouts, touch edits, and live peak and
RMS while its test tone plays. [Android build instructions](ANDROID.md#run-a-designer-layout-in-an-android-app)

### Save, preview and check layouts

Use **Save layout** to download editable JSON and **Open layout** to
resume editing it. The v3 file stores wide geometry, optional phone
positions and sizes, theme, styles, artwork and a parameter snapshot
when loaded. The designer opens v1 and v2 layouts with their original
wide positions and automatic phone placement.

Choose **Phone** in **Edit layout** to arrange the controls on a four-column
grid. Moving or resizing a control sets its phone position and size.
Automatic controls use the full width and flow around fixed controls.
**Auto-place on phone** clears the selected control's phone position.
**Reset phone layout** clears all phone positions, and Undo can restore them.
Both sizes share parameter bindings, styles and artwork.

**Export WebView HTML** produces one self-contained editor. It switches
to the phone grid at 480 CSS pixels or less, and the tab order follows
the visible layout.

Use **Undo** and **Redo** for editing controls, artwork, colors and the
parameter snapshot. Ctrl/Cmd+Z undoes and Ctrl/Cmd+Shift+Z redoes.
Opening a layout starts a fresh history. Loading the showcase is undoable.

Use **Live preview** to try the same renderer while you edit. Change
**Viewport** to compare sizes without switching the grid you're editing.
Preview retains simulated values
across layout edits until you close it. Controls bound to the same key
share a value. Preview meters use simulated signals.

Choose **Load parameters** to import a JSON snapshot from Cell's parameter
bridge. The reference exporter is `cell_web_bridge_example --parameters > params.json`.
The designer suggests stable parameter keys and uses the snapshot's
values, ranges, steps and units in preview. The exported editor reads live
metadata from its host. **Check bindings** reports unknown keys and choice
or toggle controls whose parameters have incompatible ranges.

**Export C++ bindings** downloads `cell-ui-bindings.hpp`. Include it
from a Cell C++ project, then call `cell::designer::bind(engine)` to
create a WebView parameter bridge using the generated allow-list. The
header also exposes each bound control's kind and key, and lists meter
sources separately so they stay read-only.

Meters consume a separate GUI-thread message. With the AUv3 helper's
`AU_EDITOR_METERS` option, the extension sends `output-peak` and
`output-rms` in dBFS from its latest audio output block while the view is
visible. The CLAP designer editor also sends these meters while visible.
Other WebView hosts can send the same wire format through
`View::send()`:

```json
{"type":"meters","meters":[{"key":"output-peak","minimum":-60,"maximum":0,"value":-12.5,"unit":"dBFS"}]}
```

For iterative desktop development, launch the native reference with
`cell_web_editor_example --watch-html cell-plugin-editor.html`. Replacing
the HTML on disk reloads the editor while preserving the processor's
parameter values. The reference still uses the `gain` parameter. Update
the bundled HTML and rebuild for signed AUv3 distribution.

For a headless integration check, record the editor's `postMessage`
payloads as newline-delimited JSON. Pass them to
`cell_web_bridge_example --apply-gestures < gestures.jsonl`.
The reference applies the messages to its native engine and prints the
resulting parameter snapshot. A rejected message reports its line number.

With SDK examples, tests, and WebView enabled, CTest runs
`cell_web_gesture_replay` when Python is available. It checks that
a valid UI gesture updates the native gain and that unknown, out-of-range,
and off-step parameter edits are rejected. No browser runtime is needed.
