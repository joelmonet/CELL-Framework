# Native audio files

Link `Cell::AudioFiles` and include `<cell/audio_file.hpp>`. An installed SDK
exports the `AudioFiles` component. These are control/worker-thread file APIs;
decoding, allocating buffers and waiting on a reader lock must happen outside
the real-time audio callback. Applications can retain decoded PCM in RAM or
provide their own page cache and read-ahead service.

The built-in reader handles PCM/IEEE WAV, big-endian RIFX, extensible WAV, and
RF64/BW64. It retains 64-bit frame/file positions, Broadcast Wave sample time
references and channel masks. A null channel destination skips that channel;
destinations beyond the file's channel count receive zeros. A shared reader
serializes its cursor. `read(double*)` preserves float64 precision and floating
point headroom; non-finite samples or values that cannot fit the destination
precision produce an error instead of silently normalizing them.

Native WAV writers support PCM 8/16/24/32 and IEEE float32/64. They exclusively
create their destination and promote to RF64 when the RIFF size fields would
overflow. Call `finish()` and check its result before publishing an asset. A
failed or cancelled write must be discarded by the caller; destruction alone
does not certify a completed asset. Integer outputs clip to their representable
range and round to the destination resolution. Floating output retains values
above unity. `waveFileHeader()` provides the same finalized layout for known
length interchange and sparse capture construction.

There is no product track or duration cap. Actual format fields, signed 64-bit
addressing, available memory, storage and codec restrictions determine valid
inputs. Native WAV transfer storage is normally 256 KiB, growing to one complete
sample frame if necessary. Selected channel decoding avoids allocating a whole
planar copy of a multichannel capture. This does not eliminate the disk I/O
needed to read interleaved frames.

Additional formats use the separately licensed libsndfile C ABI. The included
official 1.2.2 header and license have recorded provenance. The provider is loaded
beside the current module, then from the operating system's normal library
locations. Linux names are `libsndfile.so.1` or `libsndfile.so`; Windows names are
`libsndfile-1.dll` or `sndfile.dll`. Applications distributing it must package
the runtime and its dependencies and fulfill their licenses. No runtime binary
is included in the source SDK. Native PCM/float WAV/RF64 requires no provider.

FLAC output uses an exact integer conversion before encoding, so a lossless
round trip of PCM-resolution samples does not introduce a one-bit float
conversion error. AIFF, Ogg, MP3 and other decoding capabilities depend on the
installed provider build. MP3 encoding is a separate application integration.
`compressedAudioCodecsAvailable()` reports whether the C ABI can be loaded;
opening a specific format can still fail if its codec is unavailable.

The optional work callback may throw to cancel long operations. C callbacks
capture exceptions and propagate them after returning to C++. Cancellation can
invalidate internal codec state: a subsequent read rebuilds that decoder from
the same open source file before seeking, so a cancelled job does not poison
later playback or substitute a different file at the same pathname.

Qualification covers independent byte-level WAV fixtures, 32/64-channel capture,
wide frames beyond the former framework scratch buffer, float64 and RIFX,
sparse four-hour RF64 with markers beyond 32-bit addressing, metadata, bounded
working storage, concurrent reads, malformed inputs, exact FLAC16/24 and
cancellation recovery. Sparse-file tests exercise addressing; they do not prove
four hours of sustained physical device throughput. Windows provider deployment,
file locking and physical low-latency playback require platform qualification.
