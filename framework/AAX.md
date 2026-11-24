# AAX Native build

The reference gain example builds an unsigned `CellGain_AAX` bundle on Windows
x64 or 64-bit macOS. Pass the separately supplied Avid SDK root and CMake
compiles AAXLibrary with the plug-in's toolchain and configuration.

```sh
cmake -S framework -B build-aax -DCELL_SDK_EXAMPLES=ON -DCELL_SDK_TESTS=ON -DCMAKE_BUILD_TYPE=Release -DCELL_GAIN_AAX_SDK=/path/to/AAX_SDK
cmake --build build-aax --config Release --target CellGain_AAX cell_aax_link_host
ctest --test-dir build-aax -C Release -R '^cell_aax_(exports|runtime)$' --output-on-failure
```

Run the same configure, build, and export check in one command from the
SDK source tree:

```sh
python tools/qualify_aax.py --aax-sdk /path/to/AAX_SDK --build-dir build-aax
```

For Windows x64 with Visual Studio 2022, select the generator and architecture
explicitly:

```powershell
py -3 tools/qualify_aax.py --aax-sdk "C:/SDKs/AAX" --build-dir build-aax-win-x64 --cmake-arg=-G "--cmake-arg=Visual Studio 17 2022" --cmake-arg=-A --cmake-arg=x64
```

For an Apple Silicon build add `--cmake-arg=-DCMAKE_OSX_ARCHITECTURES=arm64`.
Use a separate build directory for each architecture or toolchain. The
command fails if the expected CTest check is missing. It also checks the
generated bundle's executable location and the Windows x64 PE header or
macOS bundle metadata. The separate `CellGain_AAX_Test` module runs
the simulated registration, state recall, automation retry, and audio
rendering checks. The release bundle contains only the SDK entry points.

Use the MSVC toolchain or an MSVC-compatible compiler on Windows and the
native Apple toolchain on macOS. The export check loads the module, resolves
the seven ACF entry points and calls the SDK version function.

The SDK path also works when configuring `examples/sdk_gain` against an
installed Cell SDK. A prebuilt archive is optional. Pass
`CELL_GAIN_AAX_LIBRARY` and, for multiple configurations, optionally
`CELL_GAIN_AAX_DEBUG_LIBRARY` to use matching archives built with Visual
Studio or Xcode. On Windows the upstream Release x64 project produces
`Libs/Release/AAXLibrary_x64.lib` with the DLL runtime. On macOS use the
`libAAXLibrary_libcpp.a` output for the selected architecture or universal
slice set.

On Windows the bundle is under the build tree at
`gain-example/aax/Release/Cell Gain Example.aaxplugin` when building Release
from the SDK. On macOS use the resulting `.aaxplugin` module bundle in the
example build directory for the selected generator configuration.

For your processor use `cell_add_aax_plugin` with its processor target, the
Avid SDK path. You can pass an existing AAXLibrary target or let Cell build
one from sources. On macOS, enable `OBJCXX` in the calling CMake directory
when building AAXLibrary from sources. Supply the
product's stable manufacturer, product, mono, stereo and chunk identifiers.
The reference gain IDs are reserved for the example.

A successful build produces an unsigned Native bundle. The next validation
step is to load it in a Pro Tools developer build and exercise mono and stereo,
automation, state recall, sample-rate changes, and latency notifications.
Distribution requires the applicable Avid and PACE release workflow.

On Linux, `tests/sdk_aax_link_consumer` builds against an installed Cell SDK and
the supplied official AAX SDK (`CELL_TEST_AAX_SDK`). It links the adapter and
Avid sources into an ELF test module with undefined symbols disallowed and runs
the same exported-entry-point check. The Linux probe does not produce a
Pro Tools bundle.

The Linux probe also sends the gain adapter through a simulated SDK description
host. It creates the registered parameter object, saves its default state
through the AAX chunk callbacks, verifies the chunk identity, and checks
that a corrupted payload leaves the state intact and a valid edited payload restores. It also checks
package metadata and initializes the simulated controller at 48 kHz stereo
and 96 kHz mono. Both layouts render through the registered callback, retry
a rejected automation packet, and verify the resulting gain change. The
registration checks also cover distinct mono and
stereo Native IDs, matching input/output stems, algorithm context fields, and
propagation of a rejected Native process registration. The adapter supplies
the full package name and, when needed, an alias of at most 16 bytes on a UTF-8
boundary, as required by AAX package registration.


Run the Linux probe from the same source tree using a matching installed
Cell SDK:

```sh
python3 tools/qualify_aax.py --aax-sdk /path/to/AAX_SDK --build-dir build-aax-probe --link-probe /path/to/installed-cell
```
