# SDK session compatibility

A candidate must reopen sessions saved by the previous published stable release
on Windows x64, Linux x64 and ARM64, and macOS Intel and Apple Silicon.
Both SDKs are installed in independent prefixes. The previous build saves
the sessions and offline renders before the candidate is installed. The
candidate opens the unchanged project from a different directory. The
qualification uses the shipped Cell Gain processor and an independent CLAP
host, together with the SDK's portable graph reader and renderer.

There is no published release tag yet. The first forward check uses the
public commit recorded in [baseline.json](../tests/session_compatibility/baseline.json)
as its bootstrap reference. For each later release, pin the previous published
stable release tag and the immutable commit it resolves to. The workflow
checks the pin against GitHub's releases and Git history before building.
The guarantee runs from that previous release to the candidate within the same
platform and architecture.

## Guaranteed session surfaces

| Public SDK surface | What a candidate must preserve |
| --- | --- |
| `Cell::Core` and `Cell::Wrappers` CMake targets and `cell_add_clap_plugin` | A rebuilt source consumer can build the reference CLAP product with the installed SDK. The CLAP factory continues to resolve the released descriptor ID. |
| `cell::stableId`, `PluginDescriptor`, `Parameter` and the CLAP parameter and state extensions | The published reference product's existing parameter IDs, names, modules, flags, ranges and defaults remain compatible. Saved plain values restore through the same IDs. CLAP automation continues to address those IDs at the saved sample offsets. |
| `Engine::saveState`, `Engine::loadState` and the default `CELL` version 1 state envelope | The same product reads its previous default state, preserving recognized parameter IDs and values. Product-specific `StateExtension` codecs and migrations are owned by the product. |
| `ProgramMemory`, `ProgramAddress` and the reference product's factory catalog | The reference bank and program numbers and names retain their meaning. The candidate recalls the benchmark factory slot, loads its saved state-backed preset through CLAP, and renders the preserved setting. |
| `graph::Document::parse` and `serialize` for `cell.graph.v1` through `cell.graph.v4` | Previously written documents retain node identities, routing, stored settings, automation, and the intents and modulation defined by their schema. |
| `graph::saveProject`, `openProject`, `extractProjectWave`, `renderProject` and `compareProjectRender` for `.cellgraph` (`CELLGP01`) | The candidate reopens the previous portable graph archives and extracts each original embedded WAVE byte for byte. The graph still renders from the archived audio. |
| `Cell::AudioFiles` WAVE reader and writer for the reference fixture | The stored relative stereo 48 kHz float32 WAVE resolves after relocation. Sample rate, channel count, frame count and finite audio are required. |

Each reference render must stay within a peak absolute sample error of
`1e-6` and RMS error of `1e-7`, measured over all channels and frames.
Sample rate, channel count, and frame count must match exactly. The host
exercises sample offsets inside processing blocks and renders the recalled
session and preset separately. The graph renders exercise each schema.
A missing external asset, a remapped automation lane, and a corrupt preset
must fail the gate.

These are source and forward session guarantees for the named surfaces.
C++ binary ABI, backward opening in an older SDK, bit-for-bit renders across
architectures, and product-specific state schemas remain outside this contract.
Coverage of commercial DAW project formats, other plug-in formats, OS-level
plug-in signing, mobile session recall, and additional audio file formats
requires separate fixtures. The qualification host's JSON and preset
filenames belong to the test fixture and are private to the harness.

## Release checks for generated projects

Every `cell new` project includes `release-fixture.json` and a native
compatibility target. Use the installed SDK's release command to capture
the outgoing stable build before rolling the project forward. Keep the
baseline outside a disposable build directory.

```sh
cell release BankedTrim --build-dir build/previous --capture releases/0.1.0-reference
cell release BankedTrim --build-dir build/candidate --previous releases/0.1.0-reference \
  --report compatibility.json
```

You can also compare a previous build directory or `.clap` directly. Use
`--previous-project` to locate the previous build's declared fixture and
assets. A previous build directory can supply the SDK's parameter identity
executable. A standalone CLAP bundle supplies the host-visible mappings.
Older generated CLAP projects can qualify using their existing build
receipts and processor identity executable.

```sh
cell release BankedTrim --previous /releases/previous-build \
  --previous-project /releases/previous-source --report compatibility.json
```

The command builds and tests the candidate, opens the released state in an
independent CLAP host, and checks the published plug-in ID and parameter
mappings. A released baseline preserves processor keys, IDs, ranges,
defaults, steps, skew, and automation flags. The host also checks CLAP's
published names, ranges, flags, and audio layout. New parameters and factory
slots can be added. Previous identities and factory load keys must still
resolve. The default fixture records two sample-offset edits, a saved
session, and a state-backed preset. The banked project also verifies the
factory catalog and recalls two bank slots through CLAP. The instrument
receives a MIDI note during its renders.

The fixture can declare these fields in addition to its schema.

| Field | Accepted values |
| --- | --- |
| `tolerances` | Required `peak` and `rms` absolute sample limits. The generated limits are `1e-6` and `1e-7`. |
| `assets` | Relative asset paths whose bytes must still match in the candidate project. |
| `input` | Relative input WAVE path. It is checked as an asset and copied into the portable baseline. The host generates a deterministic signal when this is omitted. |
| `automation` | Ordered objects with `sample`, numeric CLAP `parameter` ID, and a plain `value` aligned to the released parameter's step. |
| `session_state`, `preset_state` | Relative paths to optional raw CLAP state blobs from a product's host fixture. The previous build must open them. |
| `factory_presets` | Up to 32 embedded CLAP factory load keys to recall and render. |

The candidate resolves the released relative assets from its own project
directory. Missing or changed assets fail qualification. The portable
baseline locks the previous input WAVE, reference renders, and serialized
state with SHA-256 digests. Keep baseline state and reference audio private
when they contain session data. Ship the JSON report independently. It
contains the build fingerprints, asset digests, native check results,
and measured peak and RMS errors, with no state blobs or audio.

Run the gate on the same platform and architecture as the baseline. A
generated project that ships CLAP uses that bundle. Other templates use a
desktop CLAP reference built from the same processor, which the report
identifies as `processor-reference`. This covers shared processor behavior
for the Android starter and other wrappers. Qualify their device builds
and format-specific sessions separately. A baseline captured for a
shipping CLAP bundle and a processor reference are different scopes.

## Release evidence

The GitHub Actions `session-compatibility` job fails unless the release pin
and every native target pass. After it passes, trusted `main` and `v*` runs
package the previous and candidate CLAP binaries, independent hosts, original
sessions and renders, and a report with the two source commits and measured
errors. The `signed-session-reference-builds` workflow artifact contains
the five reference ZIPs and their Sigstore provenance bundle
(`cell-references.sigstore.jsonl`). The attestations cover the ZIP digests
and their build workflow. Platform
signatures such as Developer ID and Authenticode have a separate purpose.

Verify a downloaded ZIP against this repository and workflow:

```sh
gh attestation verify cell-session-reference-linux-x64.zip \
  --repo joelmonet/CELL-Framework \
  --signer-workflow joelmonet/CELL-Framework/.github/workflows/session-compatibility.yml
```

After extracting a package, run the candidate host on its saved session.
For Linux, run this from the extracted package directory:

```sh
candidate/bin/cell_session_reference verify candidate/plugins/CellGainReference.clap session
```

The host and plug-in paths for each platform are listed in
`candidate/paths.txt`.

Use the signed artifact from the candidate tag's successful workflow run
when promoting a release. Keep it with the release so future qualification
can identify the exact source revisions, fixtures, and render limits.
