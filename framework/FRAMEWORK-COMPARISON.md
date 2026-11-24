# Choosing Cell

Cell's strongest advantage is fit to this project's existing C++ audio code,
session migration requirements and interaction rules. It is not an objectively
better choice for every plugin developer. The comparisons below concern the
frameworks' roles and tradeoffs, not an independently benchmarked ranking of
their latest releases or licenses. Verify the exact version and license before
adopting an external project.

| Choice | When Cell is the better fit | When the other choice may be better |
| --- | --- | --- |
| JUCE | You need an independently owned implementation, explicit dependencies, our prepared-state and interaction contracts, and reuse of Sound Cells' native C++ code without a JUCE dependency. | You need a mature broad application framework, many existing components, extensive documentation, platform coverage and commercial support now. Cell still has qualification and feature gaps. |
| iPlug2 | You want Cell's specific state/migration, shared native service and interaction APIs and are prepared to maintain them. | You want an established audio-plugin framework with an existing user community, examples and mature build paths. Cell does not have a blanket efficiency advantage over iPlug2. |
| Visage | You need Cell's audio/parameter/state/file/device/wrapper foundation as well as a selectable native or WebView editor. | A modern GPU vector UI is the immediate priority. Cell's current native Canvas uses software rendering; its new effect caches are not a replacement for Visage's entire renderer. |
| Melatonin Blur / Gin | You need independently implemented effects/services that integrate without a JUCE dependency. Cell supplies CPU blur, shadows/glows, bounded caches and shared controls directly. | You already use JUCE and want its established extension ecosystem. Cell does not claim parity with every helper, algorithm or visual treatment in those modules. |
| NIH-plug and Rust UI ecosystems | Existing DSP and product infrastructure are C++, and a Rust migration/interoperability layer would add work. | You prefer Rust, its type/lifetime model and the associated plugin/UI ecosystem. Writing C++20 does not provide Rust's compile-time ownership guarantees. |
| HISE | You need a general C++ framework for effects, routing, standalone devices and independently authored module interactions. | You want a higher-level instrument-authoring environment and its existing scripting, content workflows and tools. Cell's sample readers and KSP subset are not an equivalent authoring platform or a full Kontakt replacement. |
| Custom browser-only application | Native audio, sample-accurate host events and session compatibility must remain in C++, with an optional HTML/CSS editor. | Your product is mainly a web application and needs browser distribution or web-first tooling. Cell::WebView embeds system browsers inside native applications; it does not target WebAssembly or replace native format wrappers. |

## Concrete reasons to adopt Cell here

1. **Reuse the lessons from actual plugin defects.** Bounded gestures reserve
   their release events. Prepared replacements retain the old running graph
   until adoption and defer destruction. Windows surface presentation treats
   dimensions and row stride explicitly. Pooled reads preserve independent
   cursors. These are reusable contracts backed by targeted tests.
2. **Keep DSP independent of the interface.** Headless processing, native Canvas
   controls and an optional WebView share parameter identities and native
   processing. A UI redesign does not require replacing the audio engine.
3. **Keep dependencies explicit.** Default SDK builds fetch nothing. Renderer,
   audio-device backend, codecs and official format SDKs are separately supplied.
   A consumer can request only the installed components it needs.
4. **Preserve product continuity deliberately.** Helpers accept original format
   identities; state readers/migration and asynchronous snapshot services provide
   building blocks for original sessions and grouped routing. Each product's
   actual schema and asset migration must still be implemented and tested.
5. **Define our own interaction language.** Physical values, modifier hints,
   hover help, native browsing, cascading menus, focus, gesture cancellation and
   cached drawing can be shared across products without copying a branded skin.
6. **Control the original code's direction.** There is no JUCE dependency in
   this independent SDK. Cell's original framework uses Apache-2.0, and external
   SDKs/runtimes/fonts retain their own obligations.

## Performance and sound: what the claims mean

The architecture allows prepared allocation, bounded realtime communication,
separate display refresh and retained rendering work. It does not guarantee
lower CPU or latency than another framework. Browser editors have nontrivial
startup, process and memory cost; optional embedding is not a free GPU upgrade.
C++20 cleanup mainly improves ownership and maintainability unless a specific
hot path changes.

Measure matched builds with the same DSP, sample rate, block size, active voices,
channels, oversampling, assets and host. Record average and worst-case callback
time, xruns, reported/actual latency, GUI frame time, startup and memory, with the
editor open, closed and actively manipulated. Separate cold asset/cache cost
from steady state. Effect-cache and control-copy microbenchmarks only describe
those operations. Linux browser tests using a portable runtime/PRoot are
functional evidence, not a rendering performance benchmark.

Sound quality depends on the algorithms, sample handling, interpolation,
filter transitions, precision and smoothing. A new framework cannot make
unchanged DSP inherently sound better. Preserve reference renders where parity
is required and use controlled listening and signal tests for deliberate changes.

## Adoption cost and remaining gates

Cell currently trades ecosystem maturity for control and direct fit. Budget for
API evolution, native Windows/macOS host qualification, broader buses, text and
accessibility providers, device hardware tests, dependency deployment and your
own product integration. VST2/AAX remain source prototypes. Modern Kontakt
library playback is not supplied. See [FEATURES.md](FEATURES.md) and the README
for exact component scope. Choose a mature alternative when these unfinished
areas are immediate shipping requirements and cannot be funded here.
