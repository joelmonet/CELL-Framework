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
