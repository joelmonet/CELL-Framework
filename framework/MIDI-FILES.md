# Native MIDI messages and files

`cell/midi_message.hpp` and `cell/smf.hpp` provide native message ownership and
Standard MIDI File I/O. They use no external framework MIDI objects. The native
memory streams in `cell/memory_stream.hpp` can encode or inspect in-memory data;
file streams remain buffered and use signed 64-bit positions.

Common channel messages and small metadata fit inline. Copies of large immutable
payloads share storage; changing a timestamp does not copy the payload or change
another snapshot. A generic packet constructor copies foreign bytes immediately,
without retaining the foreign object. Factory methods validate channels, 7-bit
values, 14-bit bend, and protocol field widths. Integer note velocities use0..127;
floating point note velocities use normalized0..1. Moved-from messages become
empty, retaining no length that could outlive their payload ownership.

File events use unsigned 64-bit cumulative ticks, independent of message display
timestamps. Reading buffers input in 64 KiB blocks, respects chunk boundaries,
supports running status, and retains unknown meta events and SysEx continuation
or escape packets. UTF-8 text payloads remain byte-exact. Invalid files fail with
an error; events after an end marker, missing end markers, broken lengths, and
invalid running status are not silently discarded or repaired.

Writing validates and sizes every track before the first output byte, then
streams events without a second whole-track data buffer. Large gaps use inert
sequencer events to stay within the format's four-byte delta field. Track count,
chunk length, payload length, and division checks follow the SMF field widths;
there is no additional event-count or performance-duration cap. Output errors
return false. Cancellation/error callbacks can throw and run between events and
64 KiB payload blocks. Publish an output only after its stream is finalized.

`SmfTimeMap` integrates tempo across tracks with indexed lookup and a wide
intermediate duration. SMPTE divisions support 24, 25, 29.97, and 30 fps; the
29.97 code uses 30000/1001. A type-2 file requires an explicit independent sequence
for its time map. Band Aide imports type-0 and type-1 files as one editable clip;
it does not combine unrelated type-2 sequences.

File I/O, message construction from untrusted/large packets, and final release
of large payloads belong on control or worker threads. The three-byte prepared
realtime event API remains separate. A zero-allocation small-message factory
does not make filesystem operations or large-payload destruction realtime-safe.

Qualification includes fixed external-style byte fixtures, both writer/reader
directions with independent reference codecs in the application, exact tempo and
controller/release values, truncation at every fixture byte, fragmented input,
large SysEx/meta/escape payloads, sparse 64-bit timelines, output failure and
cancellation. Sanitizer checks cover the native reader/writer independently of
the application wrappers. Host/device and Windows qualification remain separate.
