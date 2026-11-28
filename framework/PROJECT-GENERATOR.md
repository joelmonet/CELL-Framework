# Create a Cell project

## Build and open your first project

Put the installed Cell SDK's `bin` directory on PATH. The desktop starters
need CMake 3.24 or newer, a C++20 toolchain, and REAPER 7 with CLAP support.
The SDK carries the pinned CLAP headers. Build the Cell SDK with
`CELL_SDK_WEBVIEW=ON` to use the banked editor and install the matching system
WebView runtime.

```sh
cell new BankedTrim --template banked
cell run BankedTrim

cell new PolySine --template instrument
cell run PolySine
```

`cell run` configures, builds, and tests the project, then opens its compiled
plug-in on a track in a separate REAPER instance. The instrument opens with
a short MIDI chord ready to play. Use `--host /path/to/reaper` if REAPER is
outside its usual install location or PATH. On Linux the separate REAPER
profile starts with the dummy audio backend. Select an audio device in that
profile to listen. The run stages the plug-in under the project's `.cell-run`
directory and writes a JSON timing report there.

- **Banked** ships an editable Designer layout, six banks of six trim presets,
  a hold-to-recall button, and a CLAP preset catalog. Its tests cover panel
  messages, state restore, timestamped host automation, and a real SysEx bank
  query and reply. The example uses the non-commercial MIDI manufacturer ID
  `0x7D`; the local demo messages are documented below.
- **Instrument** ships an eight-voice sine synth with velocity, short attack
  and release, and channel-scoped MIDI note off and panic. Its smoke test
  compares overlapping notes with their isolated renders.
- **Mobile** ships an Android standalone gain app with a responsive Designer
  panel. Set up the Android SDK and both Cell ABIs as described in the
  [Android guide](ANDROID.md#run-a-designer-layout-in-an-android-app).

```sh
cell new PocketTrim --template mobile --id com.example.pockettrim
cell run PocketTrim --android-cell-sdk /path/to/sdk-per-abi
```

The mobile command uses installed Gradle and ADB to build a debug APK,
install it, and open its activity. Use `--serial` for multiple devices
and `--gradle` or `--adb` for tools outside PATH. Start an emulator or connect
an authorized Android device before running. Use `--build-only` for a
desktop bundle or debug APK without launching. Desktop runs also accept
`--sdk` for a different SDK installation and `--build-dir` to reuse a build.
`cell new Name` continues to generate a standalone gain renderer.

### Banked fixture messages

The template accepts `F0 7D 43 47 01 bank slot F7` to recall a slot and
`F0 7D 43 47 02 bank F7` to request a bank. Bank and slot numbers are
zero-based. The reply is `F0 7D 43 47 03 bank value0 … value5 F7`. Each
value is the signed gain in dB plus 60. The fixture ignores other SysEx.
To regenerate its checked-in Designer exports after editing the fixture,
run `node examples/sdk_banked/export.cjs` from the SDK source.

The installed SDK includes a Python 3 command named `cell` (`cell.cmd` on
Windows). Add the SDK's `bin` directory to your path or invoke it directly.
From a source checkout, run `python3 tools/cell.py new ...`.

```sh
cell new MyGain --id com.mycompany.mygain
cmake -S MyGain -B MyGain/build -DCMAKE_PREFIX_PATH=/path/to/installed-cell
cmake --build MyGain/build
ctest --test-dir MyGain/build --output-on-failure
MyGain/build/MyGain input.wav output.wav -6
```

If you omit the ID, the generator creates a unique one and writes it into
the project. Keep the selected ID after releasing the plug-in. `--output`
selects a new directory under an existing parent. Generation works offline
and refuses to overwrite an existing path.

The default project includes a gain processor, tests, and a standalone WAV
renderer. Add `--format clap`, `--format vst3`, `--format lv2`, `--format au`,
or `--format auv3` to enable a plug-in build by default. CLAP uses the headers
shipped with the SDK. Set `CELL_PROJECT_CLAP_SDK` during configure to use a
different checkout. For VST3, LV2, and AU, supply the format SDK location with
`CELL_PROJECT_VST3_SDK`, `CELL_PROJECT_LV2_SDK`, or `CELL_PROJECT_AU_SDK`.
Repeat `--format` to enable several. Add `--ui` to use the native editor for
CLAP and VST3 if your Cell installation includes the native UI component.
The generated CMake options allow changing these selections later.
Use `--format android` to add an Android app for the same processor.

## Start a project with a Designer layout

In the [UI Designer](WEBVIEW.md#design-a-parameter-editor), export WebView HTML
and C++ bindings from the same layout to one directory. Give that directory to
the generator when creating a CLAP, VST3, LV2, AUv2, or AUv3 project.

```sh
cell new StudioGain --id com.mycompany.studiogain --format clap --format vst3 \
  --designer-dir /path/to/exports
cell doctor StudioGain --sdk /path/to/installed-cell --build-dir StudioGain/build \
  --cmake-arg=-DCELL_PROJECT_CLAP_SDK=/path/to/clap \
  --cmake-arg=-DCELL_PROJECT_VST3_SDK=/path/to/vst3sdk
```

Build the installed Cell SDK with `CELL_SDK_WEBVIEW=ON` for CLAP, VST3, LV2,
and AUv2. The AUv3 extension uses the system WebKit directly.
The generator copies `cell-plugin-editor.html` and `cell-ui-bindings.hpp` into
the project's `editor` directory. Commit both files with the processor. The
gain starter exposes the `gain` parameter and supplies live peak and RMS.
The `designer_bindings` test checks parameter keys and IDs against the compiled
processor, including in builds with the plug-in targets disabled.

To design against a custom processor, export its parameter catalog from a
project build and open the JSON with **Load parameters** in the Designer.
The catalog includes the processor's keys, ranges, choices, and defaults.

```sh
StudioGain/build/project_parameter_identity --designer-catalog > studio-parameters.json
```

To update a layout, replace both exports in `editor` and rebuild. You can also
set `CELL_PROJECT_DESIGNER_DIR` to another directory. Use
`CELL_PROJECT_DESIGNER=OFF` to build without the Designer editor, or turn it off
and set `CELL_PROJECT_UI=ON` to use the native Cell editor.

## Build a generated LV2 bundle

```sh
cell new PiGain --format lv2 --lv2-uri https://plugins.example.org/lv2/pi-gain \
  --designer-dir /path/to/exports
cell doctor PiGain --sdk /path/to/installed-cell --build-dir PiGain/build \
  --cmake-arg=-DCELL_PROJECT_LV2_SDK=/usr
```

Use `/usr` with Debian's `lv2-dev` package or provide the official LV2 SDK
checkout. The optional Designer editor requires an SDK built with
`CELL_SDK_WEBVIEW=ON` and a Linux X11 host with GTK 3 and WebKitGTK 4.1.
Omit `--designer-dir` for a DSP-only bundle. Install the complete
`PiGain_LV2.lv2` directory in the host's LV2 search path. The build also
writes its absolute path to `cell-artifacts/<configuration>/lv2.txt`.

For a release, pass an LV2 URI in a namespace you control. The default URI
uses `https://example.com/lv2/` and the project ID. Keep `lv2-uri.txt` under
version control. The configured URI is used by the module and its Turtle
metadata, and the build checks it against the saved URI.

If you change the processor's ports, update `plugin.lv2.ttl.in` and the gain
smoke test together. The first meter port is `2 * channels + parameter_count`,
which is `5` for the starter. Set `CELL_PROJECT_LV2_METER_PORT` to the new index
or leave it empty to disable the meter outputs. Doctor's `lv2_host` test loads
the module, checks its URI and gain across blocks, and verifies the meter ports
when enabled. [Raspberry Pi build instructions](RASPBERRY-PI.md)

## Build a generated Audio Unit component

On macOS, supply the four-character subtype and manufacturer codes you use for
the product. Use the same codes in every release. The generator records them in
`au-identity.txt` alongside the bundle ID in `plugin-id.txt`.

```sh
cell new StudioAU --id com.mycompany.studioau --format au \
  --au-subtype StGn --au-manufacturer Acme --designer-dir /path/to/exports
cell doctor StudioAU --sdk /path/to/installed-cell --build-dir StudioAU/build \
  --cmake-arg=-DCELL_PROJECT_AU_SDK=/path/to/AudioUnitSDK
```

Use Apple's AudioUnitSDK 1.4.0. The Designer editor requires the SDK's
`WebView` component and a Cocoa host on macOS. Omit `--designer-dir` to use
the host's generic parameter editor. The generated target builds an AUv2 audio
effect in `StudioAU.component` and records its path in
`cell-artifacts/<configuration>/au.txt`. The AU component version uses the
three parts of the CMake project version, with one byte per part.

Doctor's `au_bundle` test checks the built component's identity, version, and
executable. On a Mac, install and sign the bundle before running the native
checks. With the build directory from the example above, run the following.

```sh
auval -v aufx StGn Acme
StudioAU/build/project_au_render_check
```

For the Designer showcase, build `project_au_cocoa_check` and run it from a
logged-in Mac desktop to exercise the embedded editor. [Audio Unit build and host testing instructions](AU.md#auv2-component)

## Build a generated AUv3 app extension

Generate an effect with your product's four-character Audio Unit codes and
app bundle ID. The generator records these in `au-identity.txt` and
`plugin-id.txt`. The extension's bundle ID is the app ID plus `.auv3`.

```sh
cell new MobileGain --id com.mycompany.mobilegain --format auv3 \
  --au-subtype MbGn --au-manufacturer MyCo --designer-dir /path/to/exports
cmake -S MobileGain -B MobileGain/build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DCMAKE_PREFIX_PATH=/path/to/installed-cell-ios \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM=YOUR_TEAM_ID
cmake --build MobileGain/build-ios --config Release --target project_auv3_app
ctest --test-dir MobileGain/build-ios -C Release -R '^auv3_bundle$' --output-on-failure
```

The matching [iOS Cell SDK build](IOS.md) can leave `CELL_SDK_UI` and
`CELL_SDK_WEBVIEW` off. Use a separate build and SDK installation for
`iphonesimulator`. For an unsigned build check, set
`CMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO`. To install on an iPhone
or iPad, provision the app and its extension for your team. If the project
also enables desktop formats, turn those options off in the iOS build.

The app path is recorded in `cell-artifacts/<configuration>/auv3.txt`.
The `auv3_bundle` test checks the app and embedded extension, their identities
and versions, and the packaged HTML when Designer is enabled. Launch the
installed app to test Core Audio discovery, gain state recall, automation,
bypass, and the custom editor on the starter processor. On iOS, it displays
the result and writes `Documents/AUv3-result.txt`. The desktop WAV renderer
and command-line processor tests can be built separately with a desktop SDK.

To build the AUv3 app for macOS, use a macOS Cell SDK and skip the iOS CMake
settings above. The generated app uses the sandbox entitlements in
`auv3-app.entitlements`. When building both AUv2 and AUv3 for the same
product, qualify each separately so Core Audio discovers the intended
component. [macOS signing and registration](AU.md#auv3-app-extension)

## Build a generated Android app

```sh
cell new PocketGain --id com.mycompany.pocketgain --format android \
  --designer-dir /path/to/exports
gradle -p PocketGain/android -PcellSdkRoot=/path/to/cell-android assembleDebug
adb install -r PocketGain/android/build/outputs/apk/debug/PocketGain-debug.apk
```

Put the installed Android Cell SDKs in `arm64-v8a` and `x86_64`
subdirectories of `cellSdkRoot`. Both need the `AudioDevice` component.
The app uses the parent project's `processor.cpp`, `editor` exports,
and ID in `plugin-id.txt`. Each Android ID segment starts with a letter
and can contain ASCII letters, digits, and underscores. Omit
`--designer-dir` to start with the SDK's gain showcase.

Use the app's test-tone button to exercise the starter gain processor
and meters. The Android build targets the standalone app. When building the
same project for desktop, the main CMake project uses the desktop formats
selected with `--format`.

Set the app label, version name, and Android version code in
`android/cell-app.properties`. For release packaging, use
`bundleRelease` and configure release signing with your Android signing
credentials. [Android SDK and toolchain setup](ANDROID.md#run-a-designer-layout-in-an-android-app)

## Qualify a release

Use `cell release` to compare a candidate with the previous build or a
saved baseline. Each starter ships a fixture for identity, automation,
presets, referenced assets, and offline audio. [Release compatibility guide](SESSION-COMPATIBILITY.md#release-checks-for-generated-projects)

## Check a project with Cell Doctor

```sh
cell doctor MyGain --sdk /path/to/installed-cell
```

Doctor configures, builds, and runs the project's CTest suite. Each invocation
writes separate logs, CTest XML, and a JSON summary under
`MyGain/.cell-doctor/run-*`. It returns a failure exit code on a failed stage
or when no tests complete successfully. Later stages are skipped after a build
or configuration failure.

Pass `--config Debug` to check a different configuration and
`--cmake-arg=-DCELL_PROJECT_CLAP_SDK=/path/to/clap` (repeat as needed) to
forward format SDK paths or other configure settings. Doctor uses a fresh
build directory by default. Use `--build-dir` to opt into an existing build.
Run Doctor on projects whose CMake files and tests you trust, because the build
and test steps execute project code.

The generated gain smoke test checks sample-offset automation, smoothing,
and state recall across three sample rates, five block sizes, and mono and
stereo. Doctor runs the project's own tests and optional plug-in validators.
Use [host replay](HOST-REPLAY.md) to capture an issue from a DAW and opt
into a paced replay with callback timings, deadline misses, and allocations.

### Audio-thread allocation checks

The generated smoke executable checks C++ `new` and `delete` during
`Engine::process`, including automation, smoothing, and the first block after
state recall. If a check fails, the test log names the phase, sample rate,
block size, channel count and operation counts. The executable checks the
instrumentation itself before running the processor. The smoke test also checks
subnormal input flushing on supported CPU architectures and denormal flag
restoration on SSE targets. Run [host replay](HOST-REPLAY.md) with
`--realtime` to count C++ allocations in a diagnostic plug-in build.

Coverage is for the executing thread and calls that reach the replaceable C++
allocation operators. Direct `malloc`/`free` calls, worker threads, lock
contention and host-loaded plug-in callbacks need separate instrumentation.

### Validate plug-in bundles

After building and testing, Doctor can pass the generated bundle paths to
the official validators. Enable the matching formats and supply the SDK and
validator locations. For example, on a generated project with both formats:

```sh
cell doctor MyGain --sdk /path/to/installed-cell \
  --cmake-arg=-DCELL_PROJECT_CLAP_SDK=/path/to/clap \
  --cmake-arg=-DCELL_PROJECT_VST3_SDK=/path/to/vst3sdk \
  --validator clap /path/to/clap-validator \
  --validator vst3 /path/to/validator
```

The validator executables are supplied separately. Doctor uses the configured
build's bundle paths, records separate exit statuses and logs, and fails if a
requested bundle is absent or a validator exits unsuccessfully. Requested
validators each run after successful project tests, so one format failure
does not prevent checking another. Set `--validator-timeout SECONDS` to change
the default 120-second timeout per validator.

Projects generated before validator support can add the matching
`file(GENERATE)` artifact declarations from the current template. Each
writes an absolute bundle path to `cell-artifacts/<configuration>/<format>.txt`
in the build directory. Passing these SDK validators gives format-level
coverage. DAW compatibility and platform-specific release checks are separate.

## Generate cross-platform CI

Run `cell new MyGain --github-ci` to include
`.github/workflows/cell.yml`. Commit the Cell source at
`vendor/Cell-Framework` or add a submodule pinned to your SDK revision.
The Windows, macOS, and Linux jobs build and install that SDK, build the
standalone renderer, and run the processor smoke test. The template
disables the UI and plug-in formats in CI, so those jobs require only
the Cell source and the runner's C++ toolchain.

## Preserve plug-in identity

Commit the generated `plugin-id.txt` with the project and keep its ID
stable after release. CMake checks the wrapper ID against this baseline on
every configure. The processor smoke test also checks the compiled
descriptor against the baseline, including in the generated CI workflow.
An ID change can prevent a DAW from reopening the CLAP plug-in or finding
the VST3 classes, whose IDs are derived from the plug-in ID.

Generated projects also keep released parameter keys and numeric IDs in
`parameter-ids.tsv`. The `parameter_identity` CTest checks these against
the compiled descriptor and rejects duplicate keys or IDs. Parameter
additions and reordering are allowed. Run the built
`project_parameter_identity --snapshot` to review the full list when
updating the baseline for a release.

New baselines also record the range, default, step, skew, and automation
flag. The tab-separated columns are key, numeric ID, minimum, maximum,
default, step, skew, and automatable (0 or 1). Existing two-column rows
still check identity. To add mapping checks for an existing release,
generate the snapshot from the released processor.

The SDK gain example also loads a fixed state captured from an earlier r37
build and checks the restored value and first stereo block. Generated
projects have their own plug-in IDs and use their own round-trip state test.
