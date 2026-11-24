# Embed Cell on iOS

On macOS with Xcode installed, supply the [fast_float](https://github.com/fastfloat/fast_float)
v8.2.3 headers for floating-point parsing at the iOS 17 deployment target.
Configure a headless iOS device build:

```sh
cmake -S framework -B build-ios -G Xcode \
  -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 \
  -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED=NO \
  -DCELL_FAST_FLOAT_INCLUDE_DIR=/path/to/fast_float/include \
  -DCELL_SDK_UI=OFF -DCELL_SDK_WEBVIEW=OFF
cmake --build build-ios --config Release
cmake --install build-ios --config Release --prefix "$PWD/install-ios"
```

For Apple Silicon simulator builds use a separate build directory and
`-DCMAKE_OSX_SYSROOT=iphonesimulator`. Link the matching installation into
an iOS app and supply its audio session, render callback, and presentation
layer. `ChildProcess::start` returns false on iOS. This build path targets
embedded SDK libraries.

## Build and run the AUv3 reference app

To create your own effect and containing app from the SDK, use
[`cell new --format auv3`](PROJECT-GENERATOR.md#build-a-generated-auv3-app-extension).

Add `-DCELL_SDK_AUV3_EXAMPLE=ON` to the Xcode configuration above.
For a simulator build, use an independent build directory with
`-DCMAKE_OSX_SYSROOT=iphonesimulator`. Build the containing app:

```sh
cmake --build build-ios --config Release --target CellAUv3Host
```

`CellAUv3Host.app` contains Cell Gain, Cell MIDI Probe, and Cell Instrument Probe
in `PlugIns`. The app discovers them through Core Audio. For Cell Gain it sets −6 dB, saves its state,
and restores it into a new instance. It renders stereo at 48 kHz, checks
both output channels before and after a scheduled gain edit to +3 dB and
a ramp to −12 dB. It also verifies that bypass passes the input through.
For Cell MIDI Probe it checks a silent 44.1 kHz clock bus, timed notes
before and after a scheduled transpose edit, and MIDI generation without input.
For Cell Instrument Probe it verifies note and parameter timing in stereo and
mono and checks that bypass mutes the output.
The app reports the result on screen and writes `Documents/AUv3-result.txt`.
It also requests the custom editor bundled with Cell Gain. Set
`CELL_GAIN_AUV3_EDITOR_DIR` to try your own Designer HTML and bindings in the
reference app. The same exports work on both Apple platforms. See
[Bundle a designer editor](AU.md#bundle-a-designer-editor).

The effect supports a matched stereo pair and optional mono through
`supportsMono`. Other effects can declare matched 5.1 and immersive
layouts through `mainBusLayouts`. The host supplies planar Float32 audio. The adapter
publishes physical-valued parameters, accepts scheduled points and ramps,
forwards [host tempo and transport](TRANSPORT.md), and saves Cell engine state.
Processors that enable `supportsSidechain` can receive a separate mono or stereo
key input. Music effects build with `AU_TYPE aumf` and enable `acceptsMidi`
for MIDI input or `producesMidi` for MIDI output. See the
[AUv3 configuration](AU.md#auv3-app-extension).

Dedicated MIDI processors use `AU_TYPE aumi` and receive a zero-channel
render quantum. They use the sample rate and timing of the host's silent
render clock bus.
MIDI instruments use `AU_TYPE aumu` and receive a cleared audio output block
with sample-timed MIDI and parameter automation.

An installed SDK consumer can enable Objective-C++ and add the
extension target to its containing app:

```cmake
cell_add_auv3_plugin(MyGain_AUv3 PROCESSOR my_processor
    ID com.example.mygain.effect NAME "My Gain" COMPANY "My Company"
    AU_SUBTYPE MyGn AU_MANUFACTURER MyCo)
set_target_properties(MyApp PROPERTIES
    XCODE_EMBED_APP_EXTENSIONS MyGain_AUv3
    XCODE_EMBED_APP_EXTENSIONS_CODE_SIGN_ON_COPY ON)
add_dependencies(MyApp MyGain_AUv3)
```

Use the containing app's bundle ID as the prefix for the extension
bundle ID. Set the extension target's `MACOSX_BUNDLE_BUNDLE_VERSION`
to the app's build number when it differs from the default of `1`.
For physical devices, configure the signing team and provisioning for
both targets and build with code signing enabled.

The macOS effect bundle and its validation are documented in
[Build a macOS Audio Unit effect](AU.md).


## Apple build checks

The source archive includes [the Apple SDK workflow](../.github/workflows/apple-sdk.yml).
In a GitHub checkout with the standalone SDK at the repository root, the
workflow builds an extracted source archive on macOS. It installs and links
an independent `Cell::AudioFiles` consumer for macOS arm64 and x86_64, iOS arm64 devices,
and iOS arm64 simulators. All Apple jobs build and embed the AUv3 reference
extensions and validate an independent Designer app created by the installed
project generator. The macOS and simulator jobs ad hoc sign the reference app and extensions,
register or install the app, and capture its Core Audio self-test. The
device build is an unsigned build check.

The macOS jobs publish separate SDK and installed-consumer JUnit reports
for each architecture. The reports capture SDK contract results from the
native runner. DAW scan, plugin state recall, and device runs require
qualification in the target host.

The macOS matrix also builds the optional CoreAudio backend and the live
network-audio example. It runs the UDP contracts and a device-enumeration
check as part of the native SDK test suite.
