# Cell project

Run `cell run .` to build and open a generated CLAP project in a separate
REAPER instance. The mobile starter takes
`cell run . --android-cell-sdk /path/to/sdk-per-abi` and opens its debug app
on a connected Android device or emulator. Add `--build-only` for automation.

Build against an installed Cell SDK using CMake 3.24 or newer and a C++20 compiler.

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/installed-cell
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

For a gain effect, run the executable named after the project with an input WAV, a new output WAV
path, and gain in dB. It processes mono or stereo through the same processor
used by the plug-in targets.

For CLAP, set `CELL_PROJECT_CLAP=ON`. The installed SDK supplies pinned headers,
and `CELL_PROJECT_CLAP_SDK=/path/to/clap` selects an alternate checkout.
For VST3, set `CELL_PROJECT_VST3=ON`
and `CELL_PROJECT_VST3_SDK=/path/to/vst3sdk` with its submodules initialized.
For LV2, set `CELL_PROJECT_LV2=ON` and `CELL_PROJECT_LV2_SDK` to the SDK root
or prefix such as `/usr`. On macOS, a project created with `--au-subtype` and
`--au-manufacturer` can set `CELL_PROJECT_AU=ON` and `CELL_PROJECT_AU_SDK`
to the Apple AudioUnitSDK checkout. On macOS and iOS, set
`CELL_PROJECT_AUV3=ON` to build the AUv3 extension and containing app.
Formats selected at generation time are enabled by default.
Enable `CELL_PROJECT_UI` to build
the native CLAP and VST3 editor with an installed SDK that exports `Cell::UI`.

Projects created with `cell new --designer-dir` bundle the Designer HTML and
C++ bindings under `editor` and enable `CELL_PROJECT_DESIGNER`. The CLAP and
VST3, Linux X11 LV2, and macOS AUv2 targets use that pair with an installed
SDK that exports `Cell::WebView`. The AUv3 extension uses system WebKit
directly.
Replace both exports together or set `CELL_PROJECT_DESIGNER_DIR` to the new
directory. The `designer_bindings` test checks the exports' control bindings
against the processor descriptor. Choose one editor by enabling either
`CELL_PROJECT_UI` or `CELL_PROJECT_DESIGNER`.
Run `project_parameter_identity --designer-catalog > parameters.json` from a
build and import that file with **Load parameters** in the Designer.

Edit `processor.cpp` to implement your processing. Keep the plug-in ID and
parameter IDs stable once users have saved projects with the effect.

Run `cell doctor . --sdk /path/to/installed-cell` to configure, build, and
test in one command. Pass SDK paths for enabled plug-in formats with
`--cmake-arg=-DCELL_PROJECT_CLAP_SDK=/path/to/clap` or the VST3 equivalent.

The processor smoke test checks for C++ heap allocations and frees during
processing. Allocation hooks are linked only into the smoke test. Run
`ctest --test-dir build --output-on-failure` to see the processing conditions
and counts for a failing allocation check.

Add `--validator clap /path/to/clap-validator` or
`--validator vst3 /path/to/validator` to the Doctor command to check the built
bundle with the corresponding SDK validator. The reports and validator logs
are saved under `.cell-doctor`.

If generated with `--github-ci`, include the Cell SDK source at
`vendor/Cell-Framework` in your repository. Commit the source or use a Git
submodule pinned to the revision you develop against. The workflow installs
that SDK, builds the standalone renderer and processor, and runs CTest on
Windows, macOS, and Linux. It leaves optional UI and plug-in builds disabled
so the jobs can run without separate SDKs.

Keep `plugin-id.txt` in version control. CMake checks the wrapper ID against
this file, and the smoke test checks the compiled processor descriptor.
Preserve the ID after release so DAWs can reopen saved sessions.

Keep `lv2-uri.txt` under version control when shipping LV2. The build checks
the URI saved there against the module identity. The host discovers the ports
declared in `plugin.lv2.ttl.in`. Update it when changing the processor's ports
and set `CELL_PROJECT_LV2_METER_PORT` to the first free port index when using
the Designer meters. The `lv2_host` smoke test uses the starter gain processor's
port layout. Install the entire `.lv2` bundle in the host's search path.

For either Audio Unit format, commit `au-identity.txt` with `plugin-id.txt`.
CTest checks the packaged Audio Unit identity against those files.
On macOS, `auval` and the
generated `project_au_render_check` discover the installed component through
Core Audio. Use the Designer showcase layout with `project_au_cocoa_check`
to check the Cocoa editor after installing and signing the component.

For AUv3, build `project_auv3_app` using the matching macOS, iOS device, or
iOS simulator SDK. The extension bundle ID is the app ID plus `.auv3`.
The `auv3_bundle` test checks the embedded extension and its Designer HTML.
On iOS, install and launch the signed app to check discovery, audio rendering,
and the editor. It writes the result to `Documents/AUv3-result.txt`.
The command-line processor tests and WAV renderer use the desktop SDK.

If the project includes `android/`, build the app with Android SDK Platform
35, NDK r28c, CMake 3.31.6, JDK 17 and Gradle 8.11.1.

```sh
gradle -p android -PcellSdkRoot=/path/to/cell-android assembleDebug
```

The SDK root contains `arm64-v8a` and `x86_64` Cell installations with
`AudioDevice` enabled. The app compiles this project's processor and loads
the two files in `editor`. It takes its app ID from `plugin-id.txt`.
Configure its label and version in `android/cell-app.properties`.

`parameter-ids.tsv` records released parameter keys and numeric IDs. CTest
checks that each released key and ID is still present. Additions and
reordering are allowed. When preparing a release, run the built
`project_parameter_identity --snapshot` and review the additions before
committing the updated baseline.

New baselines also record the range, default, step, skew, and automation
flag. The tab-separated columns are key, numeric ID, minimum, maximum,
default, step, skew, and automatable (0 or 1). Existing two-column rows
still check identity. To add mapping checks for an existing release,
generate the snapshot from the released processor.
