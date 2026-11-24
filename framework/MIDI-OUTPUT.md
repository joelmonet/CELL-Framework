# Host-facing MIDI output

The plugin descriptor's `producesMidi` flag enables the VST3 and AUv3 MIDI
outputs independently of MIDI input. It defaults to false. AUv3 effects with
MIDI build as `aumf` and send channel messages through the host's output
callback. See the [AUv3 build guide](AU.md#auv3-app-extension).

Studio and Live recall products use MIDI output so a rack containing only
KEYS/PADS and MIDI/FX can feed an instrument in the next host FX slot.

`Engine::process` already accepts a `midi::Output` destination. MIDI-transforming or generating processors override `routesMidi()` and `processMidi()`. Ordinary audio processors transparently forward incoming channel MIDI. A generator does not require an internal sampler or synth. UI controller messages enter the next available process callback; module timestamps remain sample offsets and are rebased across engine automation segments.

The shared RackGraph exports its final serial MIDI stream. Existing grouped routing is retained: parallel/M-S/L-R branches receive the incoming stream, and branch-local transforms do not overwrite siblings or the group's continuing MIDI stream. Put shared processing before a group or on the serial path after it when it should affect host MIDI output.

The VST3 adapter converts channel notes, poly pressure, CC, program change, channel pressure and pitch bend to SDK events, preserving channel and sample offset. This channel-event path does not implement arbitrary SysEx. Other format adapters must explicitly bind their host's output protocol; the descriptor flag by itself is not an implementation for a format.

`midi::Delivery` performs bounded allocation-free delivery. Missing/rejecting host storage blocks the remaining batch and schedules a channel panic before later notes. A partially accepted panic resumes on the following block rather than restarting indefinitely. Lifecycle reset and rack replacement/bypass transitions release previously exported notes. DSP and internal MIDI processing continue when a host provides no output list.

Regression coverage includes a loaded Studio VST3 with an actual KEYS/PADS -> MIDI/FX rack, note-on/off correspondence, sample timing, channel 16, CC output, exact audio passthrough, missing output-list recovery and reactivation. Shared unit tests cover partial sink acceptance and allocation guards.


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
