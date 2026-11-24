# Native ARA source access

`Cell::ARASource` uses the official ARA C audio-access interface directly. Enable
`CELL_SDK_ARA_SOURCE` and supply `CELL_ARA_SDK_PATH` with the official ARA headers.
This optional source-access component has no JUCE, device or editor dependency.
It is one part of a complete ARA integration; document/controller, instance-role,
format, rendering and host-lifecycle work remain separate.

Create/destroy a host reader only in host-permitted source-management callbacks
or `endEditing`. The source owner must call `close()` before the host source or
controller becomes invalid. Retained jobs/providers may own a shared proxy to
the reader, but must never extend the validity of borrowed host handles. Reads
belong on workers, using prepared PCM for real-time playback.

Capture `SourceSnapshot expected = reader.snapshot()` once when preparing a
provider or job. Pass the same snapshot and an explicit span of destination
channel pointers to every `read()`. Do not fetch a new snapshot immediately
before each read: that would let old work silently borrow replacement audio.
Snapshots contain 64-bit source positions/length metadata and a source-lifetime
revision. Reopening after a content change, host access revocation, format
change or take replacement invalidates prior snapshots, even if the new source
has the same channel layout and length. Separate readers receive separate
source-lifetime revisions.

Retire or rebuild streamed providers after host access or source Undo revokes
their reader. A provider must not remain cached with a permanently stale
snapshot after reactivation. Decoded immutable RAM audio can remain reusable
when the source content has not changed. Preparation/read failures must be
contained before crossing an ARA C callback; do not publish an incomplete group
as a successful render. Availability recovery requires the new source snapshot.

The destination span must have exactly the snapshot's channel count and valid
buffers for the requested frame count. Stale revisions and channel mismatches
fail before touching the host. Failure clears only the caller's destination
span, so a queued mono job cannot index a newly stereo/multichannel source.
The snapshot is metadata, not ownership of either the reader or its host source.

Reads use bounded 8192-frame host calls and no heap allocation. `close()` revokes
queued work, joins the current bounded host read, then destroys the host reader.
Every slice checks the captured revision. A failed or revoked request clears
its entire destination rather than returning a half-old/half-new page. The
quantum is a scheduling unit, not a session-duration or channel-count cap.
Invalid output clearing runs outside the reader lock. Source rates, channels
and frame lengths remain constrained by the host, addressing and resources.

The contract tests cover exact mono/stereo/32/64-channel data, source positions
beyond signed 32-bit frames, padding, failed host calls, concurrent revocation,
allocation-free worker reads, stale queued jobs, channel-layout replacement,
same-layout replacement and cross-source snapshot rejection. ASan/UBSan and the
actual Band Aide ARA host provide additional stage-specific evidence. These
checks do not establish physical device/display synchronization or complete
native document/format integration.

## Archive I/O

`cell/ara/archive_stream.hpp` wraps the official C archiving callbacks using
the same headers as `Cell::ARASource`. It also accepts official C++ ARA Library
archive readers and writers if an application already uses them. Borrow the
archive and controller handles only inside the host's permitted restore or
store callback. Input snapshots the archive length and bounds each read.
Failed I/O is sticky, so callers can reject an incomplete restore before
committing document state. The host-contract test covers roundtrip, header
rewrites, EOF, truncated data, invalid tables, and I/O failure.
