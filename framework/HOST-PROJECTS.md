# REAPER project context and sample boundaries

`cell/reaper_project.hpp` exposes Cockos's three-method VST3 host extension and
`cell::vst3::projectIdentity(context)`. Include it with the separately supplied
official VST3 SDK include paths. This optional header is outside the Core-only
public-header compilation set.

Call the helper on a serialized host control thread. It queries the documented
interface, asks for parent selector 3 and releases the acquired interface. It
retains no host pointer. Zero means no project is available. A nonzero result
is an opaque identity within the current process: do not dereference it,
persist it, compare it between processes or derive it from track/media names.

REAPER may expose no project during initialization and supply one before first
audio activation. An adapter which owns an appropriate host context may query
again at that later control-thread boundary. This helper does not install an
ARA registry, cross-document editing or project-switch lifecycle policy. The
generic Cell VST3 adapter retains its existing initialization behavior. Product
adapters own their later project binding and isolation rules.

`cell/sample_clock.hpp` provides `cell::audio::sampleBoundary(time, origin,
rate)` for compiling half-open time ranges into signed 64-bit sample indices.
It rounds genuinely fractional boundaries upward, resolves representation
error near integral frames and rejects invalid or insufficiently precise
timestamps. Prepare integer ranges on a control or worker thread.

The project helper is installed with the public headers; the VST3 adapter
includes that public path. Its former private header forwards to it. Installed
VST3 consumers therefore use the same ABI declaration. The sample-clock
contract covers 44.1–384 kHz, negative and decimal times, long origins,
fractional boundaries and rejection cases. Neither service changes product
DSP or imports product code into the SDK.
