# Native handoff integration

This source reconciles the November 17 native-services branch with the r12 suite
SDK. Current MIDI pad/device, KSP, sample-format, named-choice, tempo-validity
and X11 host-focus behavior is retained. Independent Linux verification passes
52 SDK/example groups and 91 public-header units, 11 focused native/UI/lifecycle
sanitizer groups, real X11/XInput2 input checks and an owned virtual PulseAudio
capture. The four native/audio-file/ARA/CLAP-VST3 transport installed consumers
pass after relocation. Those checks precede the subsequent TRG naming change;
the complete 52-group suite, header build, TRG sanitizer and a fresh relocated
TRG consumer pass after it. No old-format header is installed in that prefix.

Matching headers and libraries must be rebuilt: `Transport` adds optional
loop-point fields after `tempoValid`. Adapters without host loop-point mapping
leave the optional fields unavailable. The portable library API is now
`cell/trg.hpp`, namespace `cell::trg`, extension `.trg`, signature `CELLTRG1`;
product mappings use `trgIndex`. There are no existing old-format files, so no
compatibility header, import alias or file migration is supplied.

Native shared work includes joined module-worker lifetimes, checked local X11
requests and transfers, cached native pixel uploads, window-manager resize
reconciliation, Windows file semantics, child processes and helper APIs.
`ProcessBuffer` rejects frame counts above the signed processing limit before
mutating its storage. The full native UI scaling check waits for an actual
mapped/scaled paint, retains its strict geometry assertion and closes native
test handles on failure.

Product integration separately passes 27 selected Release groups and six
focused sanitizer groups before the TRG rename, followed by ten product Release
groups and three focused sampler/browser/capture sanitizer groups after it.
Five final Linux VST3 candidates each pass all 47 official validator checks;
both exported Send/Hub host groups pass. These are scoped checks. Studio remains
uncompiled; complete product, platform, physical hardware and format
qualification remains separate. No whole-product performance claim follows.

The following original handoff is retained for provenance. Its executed counts
are not claims about this merged source.

---

# Cell SDK native handoff — 17 November 2026

This is the independent SDK source used by the completed Band Aide native replacement. It excludes product DSP, recordings, other suite/instrument source, generated binaries and build directories. The current Cell license is retained; no public license or public release is declared.

## New shared services

- Native Windows and X11 windows, vector controls, cached raster uploads, inherited native visuals, host-parent teardown, Unicode clipboard and native file drag.
- Unicode text/localization, bundled font roles, files/streams/UUIDs, MIDI/SMF, XML/state/history and installed component exports.
- Native PCM/float WAV, RIFX, RF64, selective multichannel reads and optional libsndfile formats.
- Direct ARA C source access with 64-bit source coordinates, captured source revisions, channel spans, revocation and cooperative bounded reads.
- Native audio device selection and playback using separately supplied miniaudio 0.11.23; Linux PulseAudio playback has a native writable-region path. Dummy devices are not a fallback.
- Safe module worker quiescence/reentry; generic CLAP/VST3 format lifecycle hooks; preallocated timed VST3 parameter queues and mapped MIDI expression preservation.

## Module lifetime contract

`cell::jobs::resumeModulePools()` opens admission at format entry. `quiesceModulePools()` cancels and joins module workers after instances and their borrowed services are retired. Call these outside the OS loader lock, outside audio callbacks and outside pool workers. Never substitute a static destructor or DllMain teardown: Windows thread/TLS exit may require the loader lock.

The registry owns retired pools and actual join handles through complete thread exit, even if a worker releases its pool's final public owner. No thread is detached to make unload appear complete. Partial startup failure rolls back already started workers. Quiescence is idempotent; valid reentry restarts idle pools. A product must also shut down its own observers/read-ahead services. Those product-specific services are not silently covered by the generic pool registry.

Generic VST3 entry/exit and CLAP final deinit already use the hooks. CLAP nested entry references keep the module live until the final deinit. On Windows, native GUI owners close before `cell::ui::shutdownNativePlatformServices()` releases thread-local clipboard/OLE resources, also before loader teardown. All GUI calls remain on the GUI thread.

## Window and file fixes

Native dispatch retains lifetime ownership when input/paint closes a window. Windows geometry arithmetic avoids signed overflow for default maximum sizes. X11 requests use checked local error results without changing the host's global Xlib handler. Transfer event packets never overread small event structures. Pixel uploads handle odd row strides, native byte order, RGB/ARGB visuals and request-size slicing.

Linux keeps the newest requested size while the window manager acknowledges it; replies from another client's old resize cannot roll it back merely because they carry a current serial number. Actual OS constraints reconcile asynchronously without blocking. Stable windows add no geometry query. Equal-size layout is an actual no-op, including during painting; replacing a live painter's raster remains rejected.

Windows file paths use native extended prefixes without depending on a DAW manifest. Link/parent traversal resolves physical directory handles before '..', with no new traversal for ordinary paths. Stream positions are signed 64-bit. Sparse-file truncation preserves NTFS sparse flags. Filesystem timestamps retain actual absolute epochs. Use the dedicated file/stream documentation for publication, flushing and failure semantics.

## Format SDKs

The independent generic VST3 example is built against official VST3 3.8.0, root revision `9fad9770f2ae8542ab1a548a68c1ad1ac690abe0`, with matching submodules. It uses IDataExchange and the helper adds the official native module entry source. VSTGUI and automatic installation are disabled. CLAP uses separately supplied official headers. Linux example modules hide private implementation symbols.

Band Aide's direct ARA 2.2 VST3 wrapper is separate and uses official VST3 3.7.1. Do not mix those header trees. Its complete ARA document/renderer code is product source; `Cell::ARASource` is the shared source-reading component. VST2/AAX remain unbuilt prototypes, and macOS remains deferred.

## Executed evidence

- 43 independent SDK contracts and public header checks pass on Linux.
- Three separate installed consumers pass after moving the installation to a new prefix: native services, audio files and ARA sources. The consumer resolves no original installation path.
- Generic VST3 actual factory/audio/state/native windows, constrained sizes, hide/show, parent-first teardown and unload pass. Generic CLAP factory/create/init/destroy, nested entry references and repeated unload/reentry pass.
- Native Windows file/worker preflight passes; Band Aide separately runs 43 Windows and 43 Linux product checks and the full independent ARA host.
- Scoped ASan/UBSan checks pass for X11 transfers/controls, ARA sources and module workers; this is not a whole-product sanitizer claim.
- Independent virtual PulseAudio capture passes at 48 kHz with a requested 128-frame period, three start/stop cycles and six bit-exact windows. It does not measure physical speaker latency.
- 1,000 rapid Linux window lifecycle runs under three CPU load workers pass after fixing the resize race; deterministic stale-reply and OS-constraint regressions are retained.

The source package hashes every member and performs CRC/SHA256 readback. Build through `framework/`, or install it and use `find_package(Cell CONFIG ...)`; see README.md. Optional Blend2D, miniaudio and official format SDKs remain explicitly supplied dependencies. A passing shared component does not certify another product's migration, commercial plugins, physical hardware, native accessibility providers, Linux IME composition or every platform/format combination.


## Band Aide 0.5.13 file publication

On Windows, Cell uses the extended replace rename where the filesystem
supports it. Readers that already hold a delete-sharing handle keep reading
the old file, and new opens see the replacement. Exclusive publication still
fails if the destination exists. Where the extended operation is unsupported,
Cell falls back to the earlier Windows move and reports any failure.

The native file and pooled-reader contracts exercise the retained-reader
behavior. The pooled-reader test marks its Windows fixture sparse before
seeking beyond 4 GiB. Source packaging checks that literal framework CMake
inputs and the original framework's license and notices are included.
