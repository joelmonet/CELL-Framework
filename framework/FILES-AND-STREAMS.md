# Native files, streams, and UUIDs

`cell/file.hpp`, `cell/streams.hpp`, and `cell/uuid.hpp` provide shared native
services for Cell products. They retain no JUCE file, text, stream, or UUID
objects. File access, enumeration, durable flushing, and random UUID generation
belong on control or worker threads, never an audio callback.

Files own `std::filesystem::path`; UTF-8 text paths use Unicode conversion, and
Windows OS calls use wide, extended-length paths without requiring a host process manifest. `File::nativePath()` retains the native prefix; display paths omit it. `files::windowsPath()` provides the same conversion to low-level callers. Do not convert a native Windows path through
`path.string()`. The empty File is an invalid sentinel, rather than the working
directory. Parent traversal preserves symlink semantics. On Windows a path containing '..' resolves the preceding actual directory handle before stepping to its parent; ordinary paths add no such filesystem traversal. Resolution occurs when the native path is prepared, rather than promising live link retargeting.

Stream positions and lengths are signed 64-bit. Native reads and writes use OS
handles. Output has a configurable buffer (64 KiB by default) so a codec's small
writes do not each become a system call. Seeking drains the buffer without a
durable flush. Explicit `flush()` drains and requests durable OS synchronization;
check `getStatus()` before publishing an asset. Destructors drain but cannot
report a write failure. A stream error is sticky.

Output creation defaults to exclusive `FileWriteMode::createNew`. Choose
`truncate` explicitly only for an owned or authorized replacement, and `append`
only for intentional appending. Append streams reject seeking. Native streams
support regular files, not pipes or device endpoints. The stream's input length
is captured when opened; coordinate edits with the source owner.

For final assets use `files::AtomicWriter` or `files::publish`. Publication is
exclusive unless replacement is explicitly selected. A failed exclusive
publication preserves the existing destination and source. Temporary files use
short UUID names beside the destination so a valid long final filename does not
require an even longer temporary name. `TemporaryFile` reserves its temporary
file; overwrite that owned file explicitly before committing it.

`fitChildFileName()` uses the destination filesystem's component capacity,
preserves the extension, and adds a deterministic hash when shortening is
necessary. If the filesystem cannot report a capacity, it leaves the sanitized
name for the OS to accept or reject; it invents no fallback cap. Keep the complete
user-facing name separately in project or MIDI metadata. A suggested nonexistent
filename is not a reservation; publish exclusively to handle concurrent writers.

UUIDs use the platform random source and set the RFC version 4 and variant bits.
Linux uses `getrandom`, Windows uses `BCryptGenRandom`, and macOS uses
`arc4random_buf`. Failure is reported, rather than substituting a clock or weak
PRNG. macOS source is provided but has not been compiled for this handoff.

Native MIDI/SMF and audio-file integration now use Cell streams directly; see
`MIDI-FILES.md` and `AUDIO-FILES.md`. Foreign stream adapters remain in independent
reference tests. Host, FX and device boundaries still require separate migration
and qualification.

Qualification covers Unicode filenames and UTF-16 text import, sparse offsets
beyond 5 GiB, EOF and seeking, exclusive and atomic publication, long filenames,
collisions, timestamps, symlinks, and module discovery. UUID checks cover version,
variant, formatting, and 10,000 generated values. Windows preflight also passes Unicode/long-name, timestamp, link-parent and sparse-offset contracts. Native truncate uses OPEN_ALWAYS/SetEndOfFile so an existing NTFS sparse marker survives; the product sparse 32-channel/four-hour RF64 fixture passes on Windows without allocating its logical hundreds of gigabytes. Actual device/host workloads remain separately qualified.
