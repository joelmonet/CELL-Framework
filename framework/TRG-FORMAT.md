# TRG portable sample container

The canonical drum-trigger library extension is **`.trg`**. It is the only
portable trigger-library extension offered by the product loaders and savers.
The public SDK header is `cell/trg.hpp`; `cell::trg::fileExtension` supplies its
extension to independent consumers. No previous-format file compatibility is
needed for this initial format.

Cell's `.trg` version 1 is an independently written portable sample container.
Cell::Core provides `cell::trg::Source`, `WaveView` and `write` for control or
worker threads. It has no dependency on a product sampler or external codec.

All integer fields are unsigned little-endian. The 40-byte header contains
`CELLTRG1` (8 bytes), version (u32 = 1), flags (u32 = 0), metadata byte count
(u32), asset count (u32), first payload offset (u64), and total file size (u64).
UTF-8 JSON object metadata follows, then one `(offset:u64, bytes:u64)` record
per asset. Metadata is limited to 16 MiB; there are at most 65,536 assets.

The first payload and each subsequent asset start on an 8-byte boundary.
Padding is zero when written. Each asset contains a complete WAVE byte stream:
RIFF, RIFX, RF64 or BW64. Offsets are absolute; entries must be ordered,
non-overlapping and contiguous apart from alignment. Unindexed trailing data,
unknown flags/versions, malformed JSON and invalid/truncated ranges fail before
a source is exposed. Core checks the WAVE signature; the audio decoder remains
responsible for validating its format and chunks. No checksum or compression
is provided by container version 1.

Readers retain one open file and bounded index/metadata storage. Views share
that source but have independent cursors; file reads serialize off the audio
thread. Reads/seeks use 64-bit positions. Playback does not extract a WAV.
The writer streams 64 KiB chunks to an exclusive sibling, then atomically
publishes. Cancellation or failure retains an earlier destination. A caller
must prevent an output from aliasing any source file before writing.

Product mappings belong outside Core. The mixing-suite mapping uses native
drum-kit schema version 4 with `trgIndex` and the placeholder `self.trg`.
Import resolves each placeholder against the actual container, validates every
index and rejects external references. Kit export preserves pad/layer controls,
velocity zones, enabled states and weighted round-robin references, deduplicates
audio payloads, and rejects outputs that replace a source asset or hard link.
Ordinary WAV and embedded TCI/GOG/TRG WAVE bytes are copied; other supported
audio sources are explicitly converted to float32 WAV while building a library.
Foreign-format writers and capture-layer authoring are separate work.
