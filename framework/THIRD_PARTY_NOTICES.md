# Framework SDK notices

The independently written Cell Framework is licensed under Apache License 2.0.
See the repository LICENSE and NOTICE. Sound Cells product sources are
distributed separately under their commercial licenses.

The default framework core, DSP headers and generic state readers use the C++ standard library and operating-system APIs. The SDK entry point does not compile or link Sound Cell product ports, neural models, Dexed, Vital, Surge XT, setBfree, or other product DSP dependencies.

Optional components retain their own terms:

| Component | Role | Notice |
| --- | --- | --- |
| System WebView runtimes | Optional original Cell host uses WebView2 / WKWebView / GTK3 + WebKitGTK; these browser engines are not Cell implementations | Microsoft SDK/loader/runtime and Linux GTK/WebKit libraries are supplied separately under their own terms; Apple WebKit is a system framework. No Microsoft or GTK/WebKit implementation is redistributed here. |
| Blend2D | Optional CPU vector renderer; supplied separately | `licenses/Blend2D.txt` |
| miniaudio 0.11.23 | Optional device I/O; header supplied separately | `licenses/miniaudio.txt` (upstream offers public-domain or MIT No Attribution terms) |
| libsndfile1.2.2 API | Native compressed/extended audio codec provider; official header included, runtime library supplied separately | `third_party/sndfile/COPYING` (LGPL2.1 or later); native PCM/float WAV and RF64 do not require this provider |
| Inter | Embedded UI font | `assets/OFL.txt` |
| DejaVu Sans Mono | Embedded UI font | `assets/DejaVu-LICENSE.txt` |
| ARA SDK 2.2.0 | Optional direct C source-access component; official headers supplied separately | Preserve the actual SDK ARA_API and ARA_Library license files; no SDK headers are redistributed in this independent archive |
| CLAP SDK | Plug-in ABI headers, pinned in `third_party/clap` | `licenses/CLAP.txt` and `third_party/clap/LICENSE` |
| VST3 SDK | Optional plugin ABI; supplied separately | `licenses/VST3.txt` |
| VST2.4 SDK | Optional legacy plugin ABI | Actual SDK and its license must be supplied separately; neither is redistributed here |
| Avid AAX SDK/AAXLibrary | Optional AAX Native ABI, library and export entry points | Actual SDK/library and their licenses must be supplied separately; none is redistributed here |

The optional SDKs/rendering library are not included in the framework source archive. Complete source and licenses for the separate Sound Cell products and their DSP engines remain in the full project and its original archives. Framework packaging must not relabel those product sources or dependencies.

Installed SDKs collect these notice files under `share/cell/licenses`.

The numeric half-band profiles were generated offline with Laurent de Soras’s HIIR designer using its published elliptic design. The framework package contains no HIIR runtime implementation. The full source package retains the pinned generator, source and WTFPL-2.0 notice under `tools/third_party/hiir/`.
Unicode case mappings in `src/unicode_case_data.hpp` are generated from the Unicode 15.0.0 database exposed by Python. Unicode data is Copyright © 1991-2026 Unicode, Inc. and used under Unicode License V3; see `licenses/UNICODE-LICENSE.txt`. The mapping lookup and UTF-8 text algorithms are original Cell code. Python is not a runtime dependency.

Brand faces: `assets/CellBrandGrotesk-SemiBold.ttf` is a renamed static weight600 instance derived from Space Grotesk, Copyright 2020 The Space Grotesk Project Authors, under SIL OFL1.1 (`assets/SpaceGrotesk-OFL.txt`). `assets/Rajdhani-SemiBold.ttf` is Copyright2014 Indian Type Foundry, under SIL OFL1.1 (`assets/Rajdhani-OFL.txt`). `assets/BRAND-FONTS.json` records source URLs, hashes and the static-instance modification. Font licenses are retained separately from original Cell code licensing.

The UI Designer's `fonts.js` bundles renamed WOFF2 subsets of Inter
(Cell Panel Sans) and Rajdhani (Cell Panel Condensed). The generated data
includes the original copyright and SIL OFL 1.1 text for each family.
The Designer carries those notices into HTML exports that embed the fonts.
`tools/build_designer_fonts.py` records the source revision and regeneration steps.


Android builds use separately supplied fast_float headers for locale-independent
floating-point parsing. The Cell source archive does not include these headers.
See the upstream [license files](https://github.com/fastfloat/fast_float/tree/v8.2.3)
for redistribution terms.
