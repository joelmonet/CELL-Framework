`Cell::Core` supplies `cell::files::ReadPool` and `PooledInputStream` in
`cell/pooled_file.hpp`. They provide read-only worker/preparation input streams
whose decoder state can outlive an OS file handle. Each stream has its own
logical 64-bit cursor. The default shared pool retains at most 16 native handles and
reopens the least recently used source when needed. This cache capacity does
not limit the number of prepared source streams. `ReadPool::create(n)` creates
an independent cache of 1–4096 handles; `limit()` reports its fixed budget.
TCI, GOG and HSV sources accept an optional caller pool, allowing a writer to
use its own eight-handle budget without evicting playback handles.

Construction captures native file identity, length and modification metadata
and verifies actual read access through the pool. An unavailable stream reports
`openedOk()==false`; a decoder factory must reject it before initialization.
After eviction, reopening verifies the captured identity and dimensions. A
replacement file is not silently substituted into an earlier prepared stream.
This is not a copy or immutable snapshot of an externally modified file's audio.

Handles, offsets and reads use native 64-bit OS operations. Read storage and
the fixed handle table are prepared outside processing; steady reads/seeks do
not allocate C++ storage. File cursor work is serialized by a pool mutex and
must stay on control/cache workers. Live audio consumes separately prepared
PCM/cache pages. Separate streams can be read concurrently; each logical
cursor belongs to its caller. An evicted asset must remain accessible at its
captured path, or the caller must reload/relink the source. This API does not make disk access safe for an audio callback.
The default shared pool survives while clients retain it; the final owner
releases all cached handles. `statistics()` is for worker/control inspection.

The independently built test exercises eviction/reopen, concurrent independent
cursors, sparse positions beyond 4 GiB, invalid/missing sources, replacement
identity rejection and final-owner cleanup. The installed Core consumer checks
the public header, implementation and transitive linkage. Platform code exists
for Windows, macOS and Linux; native runtime qualification is recorded
separately and must not be inferred from a source build on another platform.

The mixing suites adapt this service to their existing sampler decoder stream.
The sample-cache worker retains decoder state and prepared audio while sharing
native handles. Product resource/latency and full-suite release checks remain
outside the independent Core API.
