# Build Cell on Raspberry Pi

Use 64-bit Raspberry Pi OS with an arm64 compiler and CMake 3.24 or newer.
The SDK builds natively on the Pi. For releases, build on the same OS
version as the target to match its glibc and C++ runtime.

Install the Linux development dependencies and provide miniaudio 0.11.23,
the official CLAP SDK and a recursive checkout of the official VST3 SDK.

```sh
sudo apt install build-essential cmake pkg-config lv2-dev ladspa-sdk dssi-dev libasound2-dev
cmake -S framework -B build-pi -DCMAKE_BUILD_TYPE=Release \
  -DCELL_SDK_EXAMPLES=ON -DCELL_SDK_TESTS=ON -DCELL_SDK_AUDIO_DEVICE=ON \
  -DCELL_MINIAUDIO_INCLUDE_DIR=/path/to/miniaudio \
  -DCELL_GAIN_LV2_SDK=/usr -DCELL_GAIN_LADSPA_INCLUDE_DIR=/usr/include \
  -DCELL_GAIN_DSSI=ON -DCELL_GAIN_CLAP_SDK=/path/to/clap \
  -DCELL_GAIN_VST3_SDK=/path/to/vst3sdk -DSMTG_CREATE_PLUGIN_LINK=OFF
cmake --build build-pi --parallel 2
ctest --test-dir build-pi --output-on-failure
```

Run the standalone gain processor with a stereo input and output device.
Use headphones and start at low gain. For output-only systems, the tone
mode plays a quiet 440 Hz test signal through the same processor.

```sh
build-pi/gain-example/cell_gain_standalone duplex alsa -18 30
build-pi/gain-example/cell_gain_standalone tone alsa -12 5
```

The command accepts `auto`, `alsa`, `pulse`, and `jack` backends.
A capture interface is required for duplex. Ctrl+C stops the audio stream.
The standalone reference is headless.

Install complete LV2 and VST3 bundles, preserving their internal directories.
Put the CLAP binary in a host CLAP search directory. Install LADSPA and
DSSI binaries into the host's corresponding search directories. Each
host and plug-in must use the same architecture. Cell puts Linux arm64 VST3
binaries in `Contents/aarch64-linux`, including when cross-compiling. REAPER's JSFX reference
is a script and can run in an arm64 REAPER build with JSFX support.
See [the legacy effect instructions](LEGACY-EFFECTS.md) for the bundle
and format details.

To build the gain example with its Designer editor, add `-DCELL_SDK_WEBVIEW=ON`
and `-DCELL_GAIN_LV2_EDITOR_DIR=/path/to/designer-exports` to the CMake command.
Run the editor in an X11 desktop session with GTK 3, WebKitGTK 4.1 and an LV2
host that embeds X11 editors. The [LV2 editor instructions](WEBVIEW.md#bundle-a-designer-editor-in-lv2)
cover the export files, host requirements and meter ports.

To start your own project, install the SDK built on the Pi and use
`cell new PiGain --format lv2 --designer-dir /path/to/exports`. The generated
project builds a native `.lv2` bundle and a standalone WAV renderer. Doctor
checks the module and its meter ports locally. [LV2 project generator instructions](PROJECT-GENERATOR.md#build-a-generated-lv2-bundle)

The [Linux arm64 workflow](../.github/workflows/raspberry-pi-sdk.yml) builds
all five native plug-in formats, exercises an ALSA virtual playback endpoint,
and runs the plug-in host contracts on arm64 Linux. Raspberry Pi OS,
physical audio devices, and individual DAWs require their own runtime checks.
