# Android native SDK

Build Cell as C++ libraries with the official Android NDK. The reference
configuration uses NDK r28c, API 28, and either `arm64-v8a` or `x86_64`.
Supply [fast_float](https://github.com/fastfloat/fast_float) v8.2.3 for decimal
parsing on the NDK toolchain. Its headers are private to the build.

```sh
cmake -S framework -B build-android-arm64 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release -DCELL_SDK_EXAMPLES=ON -DCELL_SDK_TESTS=ON \
  -DCELL_FAST_FLOAT_INCLUDE_DIR=/path/to/fast_float/include
cmake --build build-android-arm64 --target cell_graph_demo cell_portable_graph_tests cell_sdk_gain_smoke
cmake --build build-android-arm64
cmake --install build-android-arm64 --prefix "$PWD/install-android-arm64"
```

An Android app's native CMake project can then use
`find_package(Cell CONFIG REQUIRED COMPONENTS Core DSP AudioFiles)` with the
same ABI and C++ runtime. Set `Cell_DIR` to the directory containing the
target installation's `CellConfig.cmake` (typically `lib/cmake/Cell`).
The NDK toolchain restricts package searches to target roots. Pass sandboxed filesystem
paths from the app when opening graph documents and audio. Resolve
Storage Access Framework content through the Android app before handing it to
filesystem-based SDK code. Supply the app's UI and lifecycle integration.

## Optional audio output

Enable `CELL_SDK_AUDIO_DEVICE=ON` with `CELL_MINIAUDIO_INCLUDE_DIR` pointing
to miniaudio 0.11.23. The audio module exposes `Backend::aAudio` and
`Backend::openSL` as explicit choices, as well as automatic backend selection.
With examples enabled, build `cell_android_audio_probe`. The same probe can
also be built from `examples/android_audio` using the same NDK configuration
and `Cell_DIR`.
It opens playback,
writes silence, and checks for delivered callbacks. It requires no microphone
permission. Capture in an app requires Android's recording permission and
normal audio lifecycle management.

For a development device or emulator with a matching ABI, push the probe,
`cell_portable_graph_tests`, `cell_sdk_gain_smoke`, and the NDK's matching
`libc++_shared.so` to `/data/local/tmp/cell`. Use `adb shell` from that
directory and set `LD_LIBRARY_PATH=.` when invoking each executable.
In an SDK build the probe executable is under `android-audio-example`.
Run the audio probe with `aaudio` or `opensl` to check explicit backend
selection. Physical device routing, focus, and latency require separate
hardware validation.

### Tune USB playback latency

The Designer app uses `AAudioPlaybackMode::lowLatency` for playback.
It asks AAudio for exclusive sharing and retries in shared mode if the
request fails. On a granted low-latency stream, Cell reduces the software
buffer toward two bursts at the negotiated sample rate. Android may clamp
the buffer size, and the default endpoint follows Android's audio route.
If Android ends an optimized stream on a route change, restart playback
to negotiate the new route's sample rate and burst size.

Compare the default and optimized streams while a USB interface is
selected. The probe writes silence for about one second and reports the
endpoint ID, granted sharing and performance modes, native frames per
burst, live buffer size, and stream underruns when available.

```sh
adb shell 'cd /data/local/tmp/cell && LD_LIBRARY_PATH=. ./cell_android_audio_probe aaudio'
adb shell 'cd /data/local/tmp/cell && LD_LIBRARY_PATH=. ./cell_android_audio_probe aaudio-low-latency'
adb shell 'cd /data/local/tmp/cell && LD_LIBRARY_PATH=. ./cell_android_audio_probe aaudio-one-burst'
```

The one-burst mode requests a smaller buffer only when AAudio grants
exclusive low-latency playback. Compare underruns over longer sessions
before using it for performance. At 48 kHz, two 96-frame bursts amount
to 4 ms of software buffering and one amounts to 2 ms. The actual USB
round-trip delay also includes the input path and interface converters.
Use a physical loopback to measure it. [Android loopback measurement](https://source.android.com/docs/core/audio/latency/measure)

## Run the device checks together

With Android Platform Tools installed and USB debugging enabled, run this
command against a matching device or emulator. The SDK build must include
the graph demo, graph and gain tests, optional audio probe, and shared C++ runtime.

```sh
python3 tools/qualify_android.py --build-dir build-android-arm64 \
  --libcxx /path/to/ndk/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so
```

Use `--serial` when several devices are connected. The command checks that
all binaries and the device agree on ABI and that the device meets their
minimum API level. It also checks that every ELF load segment supports the
device page size, including 16 KB devices. The command runs the graph and gain tests
and checks both audio backends, including optimized AAudio playback.
It also packs and renders a stereo project
and compares the render against independently generated channel-swapped
reference samples. The command then removes its staging directory.

## Render projects inside an Android app

`examples/android_graph` is a JNI consumer of the installed Cell SDK.
Point the app's Android Gradle Plugin `externalNativeBuild.cmake.path` at
the example's `CMakeLists.txt` and pass `Cell_DIR` for a single-ABI build.
For a multi-ABI build, install each SDK under a directory named for its ABI
and pass their common parent as `-DCELL_ANDROID_SDK_ROOT=/path/to/cell`.
CMake selects the installation using `ANDROID_ABI`.
Include its `java` directory as a Java source directory and use the
`c++_shared` runtime used to build the SDK. If the app uses R8, retain the
`org.cell.examples.GraphRenderer` class and its native method name.

Call `GraphRenderer.render(projectFile, destinationFile)` from a worker
thread. The bridge verifies the archived graph and renders its embedded
audio to a new WAV file. It reports failures as `IOException` and uses
UTF-8 paths. Both files must be accessible through the app filesystem.
For a document selected through Android's Storage Access Framework, copy
the document to app storage before calling the bridge.

### Qualify the JNI renderer on the device

Build `examples/android_graph` against the same ABI, API level, and C++ runtime
as the SDK. From the SDK source root, compile the Java smoke test with a JDK
and convert the classes to DEX with Android SDK Build Tools.

```sh
mkdir -p build-android-jni/classes
javac --release 8 -d build-android-jni/classes \
  examples/android_graph/java/org/cell/examples/GraphRenderer.java \
  tests/android_graph/GraphRendererSmoke.java
d8 --min-api 28 --output build-android-jni/jni-smoke.zip \
  build-android-jni/classes/org/cell/examples/*.class
```

Add `--jni-library build-android-jni/libcell_android_graph.so --jni-dex
build-android-jni/jni-smoke.zip` to the qualification command above. The
runner includes the shared library in its ABI, minimum API and page-size
checks. It then starts Android's 64-bit Java runtime, calls the same Java
renderer an app uses, checks that an existing destination is rejected, and
compares the resulting audio sample for sample. This covers the renderer
under ADB. App packaging, lifecycle and audio routing have separate tests.

## Run a Designer layout in an Android app

Use [`cell new --format android`](PROJECT-GENERATOR.md#build-a-generated-android-app)
to create a project with the Android app, processor, and Designer assets
together. The generated app uses the same Gradle prerequisites as the example.

`examples/android_designer` is a standalone gain app for Android 9 and later.
It opens the same Designer HTML and C++ bindings used by the desktop plug-in
examples. The bundled showcase includes the field-radio knob, studio fader,
and broadcast peak and RMS meters. The layout switches between its phone and
wide arrangements with the viewport.

Build and install the SDK for both `arm64-v8a` and `x86_64` using NDK r28c,
`c++_shared`, `CELL_SDK_AUDIO_DEVICE=ON`, and miniaudio 0.11.23. Put the two
installations in subdirectories named for their ABI. With Android SDK Platform
35, Build Tools 35.0.0, CMake 3.31.6, JDK 17 and Gradle 8.11.1 installed, run
this from the source root:

```sh
gradle -p examples/android_designer -PcellSdkRoot=/path/to/cell-install assembleDebug
adb install -r examples/android_designer/build/outputs/apk/debug/CellAndroidDesigner-debug.apk
```

To supply a different layout for the gain processor, add
`-PcellEditorDir=/path/to/exports`, containing `cell-plugin-editor.html` and
`cell-ui-bindings.hpp` from the same Designer layout.

Open **Cell Designer Gain**, move either gain control, then tap **Play test
tone** to see the meters respond to real native audio output. Either control
updates the same DSP parameter. The app stops playback on backgrounding or
loss of audio focus and restores the gain value after rotation. The system
WebView serves the editor offline without Internet or microphone permissions.
