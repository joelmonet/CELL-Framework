# r38 validation in progress — November 22, 2026

The r37.5 source delta is merged into the current r38 tree. This environment
compiled the independent Linux headless Core/State/KSP/AudioFiles/Graphics/
Wrappers configuration and all public-header checks, then passed 59 registered
groups. The newly added standalone Convolver regression target also passes,
bringing this configuration's current coverage to 60 groups. The final source
packager test verified 424 manifest entries and retained Apache-2.0 plus genuine
third-party notices. These are configuration-specific checks, not mobile,
all-format, DAW, GPU or all-platform certification.

The public source includes synthesis/wavetable/physical/percussion, resonance
mask, AnalysisLease, exact-zero convolution padding removal, and popup lifetime
and stale-row contracts. Product builds separately exercised the shared popup
and convolution tests plus suite interfaces. Product AUDIO/2/MIDI, CAB/T3K,
protocols and main module faces stay outside this independent SDK archive.
The paired 12-case Convolver microbenchmark retained identical output, declared
tail and zero algorithmic latency; padding cases used less CPU under concurrent
build load. It does not establish whole-plugin savings or device deadlines.

Suite tests and final binary evidence are kept separately in the 0.4.21 records.
Native Windows hardware, macOS/iOS, actual Android and Pro Tools/real service
qualification remain distinct. Incoming r37.5 handoff claims are not reruns in
this environment. Optional WebView remains disabled in the mixing suites.

## REAPER reference effect qualification

The qualification tool exercises Cell Gain CLAP, VST3 and JSFX in REAPER
on Linux or Windows x64. It gives each plug-in an isolated project, sets its
gain to −6 dB, lets REAPER play it, saves the project and reopens it for an
offline render in a fresh REAPER process. It checks the result against a
deterministic stereo stimulus at 48 kHz. The report records the REAPER
version, runtime, plug-in file hashes, frame counts and peak error on each
channel.

Build the Release examples with the official CLAP and VST3 SDKs supplied:

```sh
cmake -S framework -B build-reaper -DCMAKE_BUILD_TYPE=Release \
  -DCELL_SDK_EXAMPLES=ON -DCELL_GAIN_CLAP_SDK=/path/to/clap \
  -DCELL_GAIN_VST3_SDK=/path/to/vst3sdk
cmake --build build-reaper --config Release --target CellGain_CLAP CellGain_VST3
python3 tools/qualify_reaper.py --reaper /path/to/reaper \
  --build-dir build-reaper --output-dir reaper-qualification
```

On Windows, use the Windows x64 Release build and pass the path to
`reaper.exe`. On Linux, use an X11 display or Xvfb. For Windows REAPER
on Linux, pass `--wine /path/to/wine`, the path to `reaper.exe`, and the
Windows x64 Release build directory. The tool uses a private REAPER
profile and its Dummy Audio device. The new output directory holds the
projects, WAVs, logs and `report.json`. A failed format is reported
independently, and the command exits nonzero if any format fails.

The Linux x86-64 and Windows x64 runs used REAPER 7.86. The Windows run
used Wine 11.22 and an LLVM-MinGW build. That cross-build selected the
Windows allocator in a local copy of the Steinberg SDK and staged a flat
VST3 module in a standard Windows x64 bundle. Both runs cover effect
discovery, parameter editing, state recall and stereo offline rendering.
Native Windows, physical I/O, and native plug-in editors still need
runtime qualification.

### Move qualification projects between hosts

Use `--import-run` on the destination host to reopen projects from a
previous qualification directory. Bring its `report.json`, `input.wav`,
and the three `.rpp` files together. For example, to reopen a run made
on Linux in Windows REAPER under Wine:

```sh
python3 tools/qualify_reaper.py --wine /path/to/wine \
  --reaper /path/to/reaper.exe --build-dir build-reaper-windows \
  --import-run reaper-qualification --output-dir reopened-on-windows
```

The tool copies each project and changes its render path to point beside
the copy. The effect state stays intact. REAPER reads back the saved gain
before a separate process renders the copied project. The destination
report includes the original project hash and the source and destination
plug-in hashes. CLAP, VST3 and JSFX passed in both directions between native
Linux REAPER 7.86 and Windows x64 REAPER 7.86 under Wine 11.22.

### Qualify saved gain automation

Add `--automation` to the qualification command to write a square-step
gain envelope. It sets −6 dB at the start, +3 dB at 160 ms, and −12 dB at
320 ms. A fresh REAPER process checks the saved points, then a separate
process renders the project. The tool compares both channels against
the expected gain on each plateau, allowing 30 ms on either side of a
boundary for host scheduling and the gain effect's ramp. The report
records the steps, frame ranges, and peak error for each plateau.

Pass the resulting directory to `--import-run` to check the saved
envelope after moving the projects to another host. The import uses the
same automation setup automatically. The check uses the reference stereo
effect and covers settled gain for a saved stepped envelope.

## Ardour LV2 qualification

The headless Ardour check discovers Cell Gain by URI from a staged LV2
bundle. It saves a dry snapshot and two snapshots that set the plug-in
to −6 dB and +3 dB. A separate Ardour export process reopens each
snapshot and renders 24,000 stereo frames at 48 kHz. The check compares
both channels with the expected levels and writes the Ardour version,
the bundle hashes, and the measured peak errors to `report.json`.

```sh
cmake -S framework -B build-lv2 -DCMAKE_BUILD_TYPE=Release \
  -DCELL_SDK_EXAMPLES=ON -DCELL_GAIN_LV2_SDK=/usr
cmake --build build-lv2 --target CellGain_LV2
python3 tools/qualify_ardour_lv2.py \
  --bundle build-lv2/gain-example/CellGain_LV2.lv2 \
  --output-dir ardour-lv2-qualification
```

The tool defaults to `ardour8-lua` and `ardour8-export`. Pass
`--ardour-lua` and `--ardour-export` for a matching pair at other paths.
The published run used Ardour 8.12 on Linux x86-64 with its dummy
backend. The new output directory contains the session snapshots,
rendered WAVs, and Ardour logs.

The following evidence is historical and applies to immutable r37 artifacts.

# r37 development validation scope — 2026-11-21

This is a development SDK handoff, not a complete mixing-suite release or an
all-platform certification. The accompanying records archive identifies the
source manifest hashes and command logs used for the final checks.

| Area | Executed evidence | Limits |
| --- | --- | --- |
| Independent Linux SDK | Default native Core/State/KSP/AudioFiles/Graphics/Wrappers plus optional WebView build; 53 headless SDK/example groups and standalone public-header compilation pass. An installed web-bridge consumer also passes. | This configuration leaves UI, AudioDevice and ARASource off. Their older component evidence does not certify new products. |
| Linux browser backend | Three native X11 WebKitGTK child lifetimes, bidirectional JSON, physical resize to 520×320, visibility/focus calls, malformed JSON and blocked navigation checks pass using the built SDK library. | Portable WebKitGTK runtime in an isolated test display; native Wayland, host shortcuts, multiple editor windows and rendering performance are not qualified. |
| Windows backend | Official WebView2 1.0.4370.26 headers; LLVM-MinGW compilation and executable link. Parameter bridge and common WebView lifecycle/bounds/thread/error contracts run under Wine. | Actual WebView2 browser runtime/DAW behavior and native Windows hardware remain unqualified. A missing-loader check does not establish browser rendering. |
| macOS browser backend | Original WKWebView source included; explicit main-thread ownership, retained parent and delegate cleanup. | No Apple compile/runtime test executed in this environment. |
| Example and packaging | Installed consumer checks physical fractional values and begin/value/end host edits; source package CRC/hash/allow-list checks pass. | See accompanying records for final extracted/relocated consumer checks; examples are not complete production applications. |
| Existing graphics effects | r36 immutable records include 66 Linux groups, focused Windows-under-Wine checks, sanitizer and external consumer evidence. | Historical results are tied to r36. There is no new GPU Canvas backend or comparative whole-plugin performance result. |
| Format adapters | Existing CLAP/VST3 Linux generic consumer evidence documented in wrappers.json; product Windows/Linux VST3 evidence is separate. | VST2/AAX remain prototypes; broad host, multi-bus and native platform qualification remains work. |

The release records should distinguish compilation, contract tests, native
browser tests and native DAW/hardware tests. An implementation's presence does
not substitute for the last two. No actual user library, license key or private
host session is distributed in the independent SDK.

Physical-device latency, average/worst-case callback time, browser memory and
GUI frame cost need matched native measurements. A faster blur-cache operation
or smaller control-copy transaction does not establish whole-plugin CPU gains.

The CLAP consumer test selects symmetric named main-bus layouts and verifies
the 5.1, 7.1.2, and 7.1.4 channel maps. The adapter exposes a 16-channel
configuration for 9.1.6. The CLAP speaker enum lacks front-wide roles, so
that configuration has no speaker map.

The installed CLAP transport consumer also exercises subnormal sidechain
samples on SSE2 hosts. Two exact subnormal inputs would sum to a normal
sample with denormal handling disabled. The test requires a zero result,
unchanged input buffers, and restoration of the host's MXCSR register.
The clap-validator 0.3.2 denormal timing test includes input generation
and host work in its measurement. Treat that ratio as an end-to-end
diagnostic alongside the deterministic callback contract.

## Native Windows CI

The standalone archive includes [the Windows SDK workflow](../.github/workflows/windows-sdk.yml).
It builds the extracted archive with x64 MSVC, installs the SDK, and runs
the native file and pooled-reader regressions on Windows. The installed
SDK consumer builds a CLAP plug-in and opens it from a separate host
executable to exercise routing, transport, MIDI and subnormal handling.
The workflow's results apply to its Windows runner and SDK configuration.
