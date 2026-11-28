# Cell framework SDK

Cell is an independently written C++20 audio-plugin and application framework
being developed for the Sound Cells Mixing Suites. It separates native audio,
parameters, state, MIDI, files and plugin wrappers from the presentation layer.
A product can use a native vector editor, an optional system WebView editor, or
headless processing without taking on the other UI path.

**Development revision: r38 unified / version 0.1.0.** The API is evolving. This is a
source SDK, not a declaration that every platform, format or product is release
qualified. The original Cell framework is licensed under [Apache 2.0](../LICENSE).
[Third-party notices](THIRD_PARTY_NOTICES.md) retain the terms of optional
dependencies. The Sound Cells products are distributed separately under their
commercial licenses.

## Start here

| Need | Read / run |
| --- | --- |
| Complete feature inventory, APIs and target ownership | [FEATURES.md](FEATURES.md) |
| Working examples and designs for new products | [IMPLEMENTATIONS.md](IMPLEMENTATIONS.md) |
| Create a standalone, plug-in, or mobile project | [PROJECT-GENERATOR.md](PROJECT-GENERATOR.md) |
| Portable processing graph and sample-timed brightness automation | [PORTABLE-GRAPH.md](PORTABLE-GRAPH.md) |
| Why choose Cell; comparison and tradeoffs | [FRAMEWORK-COMPARISON.md](FRAMEWORK-COMPARISON.md) |
| Optional original system-WebView integration | [WEBVIEW.md](WEBVIEW.md) |
| Optional visual analysis that expires when polling stops | [ANALYSIS-LEASE.md](ANALYSIS-LEASE.md) |
| Native controls, graph gestures, menus and layout standards | [UI-STANDARDS.md](UI-STANDARDS.md), [INTERACTIONS.md](INTERACTIONS.md) |
| Shadows, glows, blur and cached layers | [GRAPHICS-EFFECTS.md](GRAPHICS-EFFECTS.md) |
| Format adapters and remaining qualification gates | [wrappers.json](wrappers.json) |
| Build the AAX Native reference effect | [AAX.md](AAX.md) |
| Cross-compile and embed Cell on Android | [ANDROID.md](ANDROID.md) |
| Embed the SDK in an iOS app | [IOS.md](IOS.md) |
| Build LV2, LADSPA, DSSI and JSFX reference effects | [LEGACY-EFFECTS.md](LEGACY-EFFECTS.md) |
| Build a macOS AUv2 effect | [AU.md](AU.md) |
| Build standalone audio and plug-ins for Raspberry Pi | [RASPBERRY-PI.md](RASPBERRY-PI.md) |
| Revision-specific checks and limits | [VALIDATION.md](VALIDATION.md) |
| Exact packaged files and hashes | `../SOURCE-MANIFEST.json` in the source archive |

## What is included

- Native float-audio processing, sample-timed parameter and MIDI delivery,
  transport, stable parameter identities, gestures and host-adapter services.
- Prepared state/graph handoff, bounded messaging, background jobs, read-ahead,
  history, JSON/XML/project state, legacy-state reading and migration helpers.
- Original cycle-bank wavetable import/playback, polyBLEP oscillators, six-operator FM graphs, portamento, modal physical modeling and percussion synthesis. See [SYNTHESIS.md](SYNTHESIS.md).
- Shared editor interaction defaults, unity-centered knob geometry and impulse phase/polarity recommendations.
- DSP building blocks for filters, FFT/analysis, pitch tracking, delay,
  convolution, oversampling, envelope detection and character processing.
- MIDI transformation, note state, output recovery, device profiles, pad learning
  and Standard MIDI Files; a bounded optional KSP language-subset runtime.
- Native files/streams, pooled reads, audio interchange, thumbnails, asset
  services, and independent `.trg`, observed `.tci` and `.gog` sample containers.
- Optional native vector UI, widgets, menus, hover help, numeric entry,
  keyboard/pad controller, native file dialogs, input/scaling and window services.
- Original CPU blur/shadow/glow effects, bounded LRU caches and DPI-aware layers.
- Optional system-browser hosting through WebView2, WKWebView and WebKitGTK,
  plus an explicit parameter/gesture bridge. Browser work stays outside audio.
- CLAP/VST3 adapter sources and build helpers, a VST2.4 prototype, and an AAX
  Native build path. AAX uses a separately supplied SDK and explicit product
  IDs, with Linux link and export smoke coverage.
- Optional native audio-device I/O and direct ARA source-access services.

[The inventory](FEATURES.md) states the exact scope and limitations of each
area. Accepting a sample container is not equivalent to emulating its original
instrument. A wrapper source is not evidence of qualification in every DAW.

## Why use it for Sound Cells and related products

Cell lets us keep existing C++ DSP and put the interaction and state contracts
we have developed into shared code: stable identities, complete gesture
lifetimes, useful menus, physical parameter values, bounded prepared-state
ownership, native browsing and reusable UI effects. Future products can reuse
those contracts rather than solve the same problems again.

The core does not depend on JUCE, and the SDK entry point does not compile the
suite's separate neural models or instrument engines. Optional UI, device and
WebView dependencies are explicit. We control the original implementation and
its development instead of tying it to another framework's commercial plan.
Third-party libraries retain their own license terms.

Cell is a better fit **when these integration and ownership requirements matter
more than a mature general-purpose ecosystem**. It is not yet a universal
replacement for all JUCE functionality. Other frameworks offer advantages in
platform maturity, GPU rendering, instrument authoring, tooling and community.
See [the comparison](FRAMEWORK-COMPARISON.md) for specific tradeoffs. No
whole-plugin CPU, latency or sound-quality superiority is claimed without
matched measurements.

## Build the independent SDK

Requires CMake 3.24+, a C++20 compiler and platform development tools. The default
configuration fetches nothing and does not configure the Sound Cells products.
Run from the extracted `Cell-Framework` directory:

```sh
cmake -S framework -B build-sdk \
  -DCELL_SDK_TESTS=ON -DCELL_SDK_EXAMPLES=ON \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PWD/install-sdk"
cmake --build build-sdk --parallel 2
ctest --test-dir build-sdk --output-on-failure
cmake --install build-sdk
cmake -S examples/sdk_gain -B build-gain \
  -DCMAKE_PREFIX_PATH="$PWD/install-sdk"
cmake --build build-gain --parallel 2
```

On Windows use a matching compiler/architecture for the SDK and consumer;
replace the shell syntax as appropriate and specify `--config Release` for
multi-configuration generators. Rebuild matching headers and libraries after
API changes. A handoff revision is not a stable binary-ABI guarantee.

An installed consumer requests the components it actually uses:

```cmake
find_package(Cell CONFIG REQUIRED COMPONENTS Core DSP)
add_library(my_processor STATIC processor.cpp)
target_link_libraries(my_processor PUBLIC Cell::Core Cell::DSP)
```

| Option | Default | Dependency / purpose |
| --- | --- | --- |
| `CELL_SDK_STATE` | ON | Standard-library legacy readers, migration and project state |
| `CELL_SDK_KSP` | ON | Independent bounded script-subset compiler/runtime |
| `CELL_SDK_UI` | OFF | Separately installed Blend2D; platform window libraries; Python 3 at build time for font embedding  [Native Theme Editor](THEME-FORMAT.md) |
| `CELL_SDK_WEBVIEW` | OFF | System browser runtime; Windows additionally needs `CELL_WEBVIEW2_INCLUDE_DIR` |
| `CELL_SDK_AUDIO_DEVICE` | OFF | Supplied `CELL_MINIAUDIO_INCLUDE_DIR` containing `miniaudio.h` |
| `CELL_SDK_NETWORK_AUTH` | OFF | OpenSSL Crypto for paired UDP audio sessions in `Cell::NetworkAudio` |
| `CELL_SDK_ARA_SOURCE` | OFF | Supplied `CELL_ARA_SDK_PATH` containing `ARA_API/ARAInterface.h` |
| `CELL_SDK_EXAMPLES` | OFF | Gain smoke and portable graph; web bridge when WebView is enabled; control/effects galleries when UI is enabled |
| `CELL_SDK_TESTS` | OFF | Contract tests and public-header checks; native UI tests need a display |

`Cell::Core`, `Cell::DSP`, `Cell::AudioFiles`, `Cell::Wrappers` and
`Cell::Graphics` are exported by the default SDK. Optional exports are
`Cell::State`, `Cell::LegacyState`, `Cell::StateMigration`, `Cell::StateTree`,
`Cell::KSP`, `Cell::UI`, `Cell::WebView`, `Cell::AudioDevice`,
`Cell::NetworkAudio`, `Cell::AndroidMidi` (Android API 33+), and
`Cell::ARASource`. A missing optional component causes `find_package(...
REQUIRED COMPONENTS ...)` to fail rather than silently substituting another
implementation.

## Export a plugin

Implement `cell::plugin::descriptor()` and `createProcessor()` in your processor
library. Implement `createEditor()` only for a UI-enabled plugin. The gain
example includes the complete processor and optional editor.

```cmake
cell_add_clap_plugin(MyGain_CLAP
    PROCESSOR my_processor ID com.example.gain NAME "My Gain")
```

Cell uses its pinned CLAP headers by default. Set `CLAP_SDK` to an alternate
official checkout when needed. Add `CLAP_INSTRUMENT` for a MIDI instrument
with audio outputs and no audio inputs.

For VST3, add the supported official SDK to the consuming project first and use
`cell_add_vst3_plugin`. `examples/sdk_gain/CMakeLists.txt` shows the complete
configuration. `UI` selects an editor-enabled adapter. No helper installs into
a DAW scan directory automatically.

For an existing VST3 product, provide both `VST3_PROCESSOR_UID` and
`VST3_CONTROLLER_UID`, each 32 hexadecimal digits in the SDK FUID constructor's
four-word order. Verify against the original source and host captures; do not
guess byte order from a displayed GUID. New-product deterministic IDs must not
replace existing identities. State migration also needs actual original chunks,
assets and routing checks; matching the plugin name alone is insufficient.

VST2.4 requires the original SDK, license and explicit `VST2_ID`. AAX requires
the Avid SDK, matching library, explicit manufacturer/product/type/chunk IDs,
native host tests and applicable signing. Their source adapters are prototypes;
see [wrappers.json](wrappers.json). No proprietary SDK or signing service is
included.

## Threading and ownership

The audio callback receives prepared buffers, parameter/MIDI events and
transport. It must not allocate, access files/network, wait on locks or destroy
retired graphs. Prepare and decode on workers, adopt completed state at a
sample boundary, and retire it outside audio. File reads and compressed sample
views are worker APIs, not realtime streaming callbacks.

Editors, native windows, dialogs, caches and WebViews belong to their specified
GUI/worker threads. GUI feedback and parameter delivery are separate from full
session serialization. ProductQueries/state snapshots support asynchronous
processor/controller communication without blocking the GUI. A host format's
thread, latency and lifetime contracts still require actual host tests.

## Validation and unfinished work

The r35 bugfix-only and r36 graphics archives remain separate records. Earlier
r36 evidence includes 66 Linux SDK test groups, focused Windows-under-Wine
checks, sanitizer checks and extracted/installed consumers. Those results apply
to those exact archives, not automatically to a later modified revision.

During r37 development, the Linux WebKitGTK backend passed three native child
open/close cycles, two-way JSON, physical resize and visibility/focus calls in
an owned X11 environment. Its Windows backend compiled with official WebView2
headers. macOS source exists but has not been built or runtime qualified here.
The revision's accompanying validation records must identify the final files
actually tested. Wine is not a substitute for native Windows qualification.

Remaining areas include a native GPU vector renderer, full native
accessibility providers, complex text shaping/IME/font fallback, native Wayland
WebView embedding, broad surround/multi-bus adapters, plugin hosting, complete
ARA document/format integration, full instrument/library emulation and native
platform/host coverage. Physical-device latency and CPU deadlines require
hardware measurements. These are development requirements, not features
silently delegated to JUCE.
