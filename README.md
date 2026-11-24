# Cell Framework

[SDK session compatibility and release guarantees](framework/SESSION-COMPATIBILITY.md)

Cell is a C++20 SDK for audio plug-ins and standalone music software. It
provides audio processing, sample-timed parameters and MIDI, state recall,
portable audio graphs, and optional native and WebView editors.

This source tree is the r38 unified development revision of Cell Framework
0.1.0. See the [validation notes](framework/VALIDATION.md) for the tested
platforms and hosts.

## Build and test

Use CMake 3.24 or later and a C++20 compiler. The default SDK build uses
the standard library and platform APIs.

```sh
cmake -S framework -B build -DCMAKE_BUILD_TYPE=Release \
  -DCELL_SDK_TESTS=ON -DCELL_SDK_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix install
```

- [SDK overview and build options](framework/README.md)
- [Start a project and add plug-in targets](framework/PROJECT-GENERATOR.md)
- [Design a parameter editor](framework/WEBVIEW.md#design-a-parameter-editor)
- [Android](framework/ANDROID.md), [iOS](framework/IOS.md), and [Raspberry Pi](framework/RASPBERRY-PI.md)

## License

Original Cell Framework code is licensed under the [Apache License 2.0](LICENSE).
See [NOTICE](NOTICE) and the [third-party notices](framework/THIRD_PARTY_NOTICES.md)
for attribution and the terms of included third-party material.
