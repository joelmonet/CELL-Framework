# Host-facing MIDI output

The plugin descriptor's `producesMidi` flag enables the CLAP, VST3 and AUv3 MIDI
outputs independently of MIDI input. It defaults to false. AUv3 effects with
MIDI build as `aumf` and send channel messages through the host's output
callback. See the [AUv3 build guide](AU.md#auv3-app-extension).

Studio and Live recall products use MIDI output so a rack containing only
KEYS/PADS and MIDI/FX can feed an instrument in the next host FX slot.

`Engine::process` already accepts a `midi::Output` destination. MIDI-transforming or generating processors override `routesMidi()` and `processMidi()`. Ordinary audio processors transparently forward incoming channel MIDI. A generator does not require an internal sampler or synth. UI controller messages enter the next available process callback; module timestamps remain sample offsets and are rebased across engine automation segments.

The shared RackGraph exports its final serial MIDI stream. Existing grouped routing is retained: parallel/M-S/L-R branches receive the incoming stream, and branch-local transforms do not overwrite siblings or the group's continuing MIDI stream. Put shared processing before a group or on the serial path after it when it should affect host MIDI output.

The VST3 adapter converts channel notes, poly pressure, CC, program change,
channel pressure and pitch bend to SDK events, preserving channel and
sample offset.

`midi::Delivery` performs bounded allocation-free delivery. Missing/rejecting host storage blocks the remaining batch and schedules a channel panic before later notes. A partially accepted panic resumes on the following block rather than restarting indefinitely. Lifecycle reset and rack replacement/bypass transitions release previously exported notes. DSP and internal MIDI processing continue when a host provides no output list.

Regression coverage includes a loaded Studio VST3 with an actual KEYS/PADS -> MIDI/FX rack, note-on/off correspondence, sample timing, channel 16, CC output, exact audio passthrough, missing output-list recovery and reactivation. Shared unit tests cover partial sink acceptance and allocation guards.

## Send and receive MIDI 1.0 SysEx

Set `PluginDescriptor::supportsSysEx` on a plug-in with `acceptsMidi`,
`producesMidi`, or both. CLAP, VST3 and AUv3 carry SysEx in either enabled
direction. For AUv3, use a component with MIDI capability such as a music
effect (`aumf`) or dedicated MIDI effect (`aumi`).

Check `MidiEvent::isSysEx()` before interpreting the three inline channel
bytes. The `sysEx` span contains the complete MIDI 1.0 message, including
`F0` and `F7`, and remains valid during the process call. Channel MIDI and
SysEx share one stream, preserving order and sample offsets. The default
engine path forwards the messages. A MIDI processor can inspect, transform,
or originate them through its `processMidi` override.

```cpp
constexpr std::array<std::uint8_t, 5> message{0xf0, 0x7d, 1, 2, 0xf7};
const bool accepted = output.push(cell::MidiEvent::systemExclusive(frame, message));
```

If a processor needs to hold payloads beyond a process call, copy them into
storage prepared off the audio thread. For example,
`midi::Buffer::prepare(eventCount, sysExBytes)` reserves event and payload
storage up front. The VST3 adapter keeps output bytes in its own prepared
storage through the end of the host block.

The adapters accept up to 256 KiB of SysEx data per block in each direction.
The AUv3 event ABI limits each message to 65,535 bytes. Incoming messages
with invalid framing or bytes are skipped. A rejected outbound SysEx
message returns false without triggering channel panic. Hosts must expose
SysEx routing on the plug-in's MIDI ports. MIDI 2.0 SysEx8 and reassembly of
separately delivered fragments are outside this path.

### Verify a bank dump through the plug-in ABI

Pass the raw binary output of a bank-dump CLI to either
`tests/sdk_transport_consumer` host probe. Use the matching transport fixture
plug-in built by that consumer project.

```sh
cell_transport_clap_host "Cell Transport Test.clap" --dump bank.syx
cell_transport_vst3_host "Cell Transport Test.vst3" --dump bank.syx
```

The file can contain one complete `F0 … F7` message or multiple concatenated
messages. The probe sends one message per block and checks that the processor
observed it at frame 23 and that the host received the exact bytes back. This
exercises ABI delivery. Bank decoding and import are product behavior.
Omit `--dump` to run the built-in transport checks alone.

## MIDI 2.0 UMP and per-note expression

Set `PluginDescriptor::supportsUmp` alongside the required MIDI input or
output flags. The core carries inline UMP words with the same sample offsets
as short MIDI 1.0 and byte-stream SysEx events. CLAP advertises MIDI 2.0
and MIDI 1.0 on opted-in ports. AUv3 uses Core MIDI event lists on macOS 12
and iOS 15 or later. An AU host selects MIDI 2.0 with `hostMIDIProtocol`
before installing `MIDIOutputEventListBlock` and allocating render resources.
Rebuild processors, adapters, and installed SDK consumers together, since
`MidiEvent` includes inline packet storage and the optional host `noteId`.

```cpp
if (const auto expression = cell::midi::perNoteExpression(event)) {
    // Address the voice by group, channel and note.
    // expression->value preserves the full 32-bit MIDI 2.0 value.
}
```

`MidiEvent::universalPacket` constructs a packet from one, two or four
native-order 32-bit words. Inspect `isUmp()` before reading the short
MIDI 1.0 fields. `midi::perNoteExpression` recognizes registered and
assignable per-note controllers, per-note pitch bend, and polyphonic pressure.
MIDI transforms pass valid UMP through unchanged. Delivery cleanup sends
channel resets to the UMP groups in which channel-voice messages were sent.
Processors still own the mapping from note addresses to their voices.

### VST3 note expression

VST3 maps opted-in native notes, polyphonic pressure, tuning, and numeric
registered and assignable note expressions to MIDI 2.0 channel voice events.
The event bus maps to group zero. Use `MidiEvent::noteId` to distinguish
overlapping notes on the same pitch and channel. An expression addressed by
host note ID follows its note-on and precedes its note-off.
`kTuningTypeID` uses the VST3 range of ±120 semitones over the UMP bend range.
The native polyphonic pressure field has float precision. VST3's existing
MIDI 1.0 controller path remains available. Raw SysEx8 packets, additional UMP
groups, and textual expression are outside the VST3 mapping.

### Android UMP devices

An Android SDK built for API 33 or later provides `Cell::AndroidMidi`.
Select a device with `MidiManager.getDevicesForTransport(TRANSPORT_UNIVERSAL_MIDI_PACKETS)`,
open it with `MidiManager.openDevice`, and pass the Java `MidiDevice` to
`cell::midi::AndroidUmpPort` through JNI. A source port on the device feeds
Cell. A destination port receives Cell's output.

Call `poll` with a `cell::midi::UmpDeviceQueue` on a device worker. The audio
callback uses `read` with its block start in the same monotonic nanosecond
clock domain as Android MIDI and passes the returned events to the engine.
Use scratch for at least `UmpDeviceQueue::capacity` events. Reset the engine's
note state before applying new events when the read reports `reset`. The
queue carries future events across blocks and puts late events at frame
zero. Supply each port's timestamps in order. Flush the queue on device
removal or reconnect. Use `AndroidUmpPort::send` on the device worker for
outbound packets.


## Clock availability and MPE event lifetimes (r38 source guidance)

`Transport::samplePosition` is meaningful as a running sample clock only when
`samplePositionValid` is true. Compare consecutive playing blocks only when
both clocks are valid; losing or regaining availability must not manufacture a
seek. A real playing-to-stopped transition still needs note release. Use checked
or saturating signed sample addition, and preserve validity when segmenting a
host block. The independent `midi::Tools` path already observes this rule; the
shared suite AUDIO/2/MIDI engine has been corrected to match it. Its new source
regressions have not been compiled/run.

MPE channel expression has a lifetime beyond the physical key press. Send zone
and bend-range configuration, initial bend and initial pressure before Note On.
Normal Note Off must not blindly center bend or zero pressure on a channel whose
release envelope may still be sounding. Initialize the next note explicitly and
prefer another free member where possible. Finite member pools and destination
release behavior still bound isolation. Panic/output-failure handling is a
separate recovery path; it must not silently drop releases or endlessly retry a
partial batch. Incoming MIDI on the same member channels needs explicit
collision handling and qualification by the consuming product.

These are reusable output contracts. The independent SDK does not acquire the
suites' pitch-tracking module, its detector tuning or a universal MPE instrument.
