# Network audio receive buffer

Include `<cell/network_audio.hpp>` and link `Cell::Core`. Allocate a
`cell::network_audio::ReceiveBuffer<Channels, CapacityFrames>` before
starting the stream. Set the prefill in frames. For example, 960 frames
at 48 kHz represent 20 ms of buffered audio.

Feed decoded, interleaved float PCM frames in order from one producer
thread. A rejected batch leaves queued audio intact. Call `render` from
one audio callback with its output span. The callback emits silence
during startup and after underruns until enough frames are available
again. The return value counts received frames so callers can track
underruns. Enqueue and render use fixed storage and never block.

A transport adapter must handle sockets, connection setup, playout
deadlines, clock drift, and channel and sample-rate agreement. This
component provides the PCM handoff for remote monitoring. Direct
Audiomovers interoperability would require a supported integration path.

## Pair a live sender and monitor

Configure with `-DCELL_SDK_NETWORK_AUTH=ON` and an installed OpenSSL Crypto
library. SDK consumers can request `find_package(Cell CONFIG REQUIRED
COMPONENTS NetworkAudio)` and link `Cell::NetworkAudio` for
`cell/network_audio_session.hpp`. Enable `CELL_SDK_AUDIO_DEVICE` to build
the live device commands.

Create one 32-byte pairing key per link, keep it private, and copy it securely
to both machines. For example, on a POSIX shell:

```sh
(umask 077; openssl rand -out cell-monitor.key 32)
cell_network_audio_wave monitor 0.0.0.0 40100 continuous 20 --pair-key cell-monitor.key --stats-ms 500
cell_network_audio_wave send-input 192.168.1.20 40100 continuous --capture-only --pair-key cell-monitor.key --stats-ms 500
```

Use the monitor's IPv4 address in the second command. For a connection
over the internet, use a publicly reachable address for the monitor and
allow or forward the selected UDP port to it. A private VPN address also
works when the monitor is behind carrier NAT. Both endpoints must agree on
the sample rate. `send` and `receive` also accept `--pair-key FILE` for paired
WAV testing. Omitting the flag selects the unpaired, cleartext PCM protocol.

The CAS2 paired protocol authenticates the stream format with fresh challenge
nonces and HMAC-SHA-256. It derives separate session keys for audio and
acknowledgments. ChaCha20-Poly1305 encrypts each PCM datagram and authenticates
its header. The receiver rejects replayed, modified, or wrong-peer audio.
Both endpoints must run CAS2 for a paired connection. Packet timing, sizes,
and negotiated format metadata are visible on the wire. Authentication,
encryption, and UDP run on the transport thread. The audio callback only
consumes its prepared queue.

The sender re-pairs when acknowledgments stop, discarding capture blocks it
cannot send. A continuous monitor retires the old playout queue after its
idle timeout and accepts a fresh authenticated stream on the same socket
and output device. The monitor adjusts playback for clock drift and reports
`playback_correction_ppm` alongside packet age, queue depth and underruns.

Build with `CELL_SDK_TESTS=ON` to run `cell_network_audio_session_tests`.
Its default mode checks pairing, wrong-key and replay rejection, reconnect,
and scheduled loss and drift. Pass `--wall-clock` for a local UDP loopback
measurement of median, p95, p99 and maximum capture-to-playout software
latency. It reports three profiles with the same jitter of up to 9 ms and a
sender clock about 417 ppm fast. The profiles are a no-loss baseline, a drop
every nineteenth packet, and a 35 ms one-way delay with every thirty-first
packet dropped plus eight-packet bursts. The results include concealment
and underrun frame counts. The impairments are injected on loopback.
Use the hardware loopback procedure below to include device and converter delay.

## Timestamped PCM datagrams

Include `<cell/network_audio_packet.hpp>` on the transport thread. Encode one
packet per datagram. Pass the complete datagram and the negotiated sample
rate to `decodePacket`. Submit successfully decoded frames to the reorder
buffer below.

Version 1 uses a 24-byte header. In order, it contains the four ASCII
bytes `CNA1`, a 32-bit sample rate in Hz, a 32-bit channel count, a 32-bit
frame count, and a 64-bit absolute starting sample on the sender’s
stream clock. All integers and IEEE 754 binary32 PCM samples are
little-endian. Samples are interleaved by frame in the agreed channel order.
The datagram limit is 1,200 bytes, which fits up to 147 stereo frames.
Use a separate session to agree on the channel order, sample rate, and
stream identity before sending PCM.

## Reordering and loss concealment

Start a `ReorderBuffer` at the agreed first sample. On the same transport
thread, call `insert` with each successfully decoded packet’s starting
sample and frame span. The fixed window rejects packets that are late,
outside the window, or overlap queued audio. Use one reorder buffer per stream.

Call `pump` with the playback buffer, a scratch span, and an exclusive sender
sample deadline. Contiguous audio is queued immediately. At a gap, samples
before the deadline become silence and later samples wait for another packet.
The caller derives the deadline from its playout schedule. Pumping honors
playback capacity so a full queue preserves pending frames for the next call.
The audio callback continues to use `ReceiveBuffer::render`.

## Playout deadlines

Construct a `PlayoutClock` from the negotiated first sample, sample rate,
and time budget for packet arrival. On each transport tick, pass the
elapsed time since the stream anchor from a monotonic clock to
`concealBefore`, then pass its result to `pump`:

```cpp
const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - anchor);
const auto drained = reorder.pump(playback, scratch, clock.concealBefore(elapsed));
```

The deadline stays at the first sample until the arrival budget ends and
then advances by the number of whole sample periods elapsed. Choose the
arrival budget and playback prefill together to fit the latency budget.
This clock uses the nominal sample rate. Long-running connections between
independent devices also need clock-drift correction.

## Playback clock drift

For monitoring between devices running at the same nominal sample rate,
pass a second argument to `ReceiveBuffer` to enable adaptive playback.
For example, `ReceiveBuffer<2, 4096> playback(960, 800)` permits up to
800 parts per million of correction in either direction. Choose a prefill
larger than the callback block and leave room for incoming packet bursts.

The audio callback uses queue depth to adjust its consumption rate
gradually. It interpolates between adjacent frames with one frame of
lookahead. After an underrun it resets the interpolation state and waits
for prefill. The default constructor behavior preserves exact PCM playback.
Adaptive playback is intended for drift around a shared nominal rate.
The transport still supplies the sender deadlines and session clock.

## Integrate a receiver

Include `<cell/network_audio_receiver.hpp>` to use the packet decoder,
reorder window, deadlines, and playback buffer together. Configure one
`Receiver` per negotiated stream. For stereo at 48 kHz, an app could use
`Receiver<2, 4096, 2048> receiver({firstSample, 48000, 20ms, 960, 800});`.

On the transport thread, pass incoming datagrams to `receive`. Call
`service` with monotonic time elapsed from the stream anchor, both after
handling arrivals and on timer ticks during gaps. Service transfers up to
one packet’s worth of frames per call. If it returns that many queued
frames, call it again at the same elapsed time to drain more. The audio
callback calls only `render` with its output span. Set `ReceiverConfig::startAtFirstPacket`
to anchor deadlines at the first admissible packet's sample. In that mode,
pass elapsed time measured from that packet's arrival. The application is
responsible for socket ownership, peer selection, and session negotiation.

## Run the UDP loopback example

On Windows, Linux, or macOS, configure with `CELL_SDK_EXAMPLES=ON` and
`CELL_SDK_TESTS=ON` to build and run the example with CTest:

```sh
cmake -S framework -B build-network -DCELL_SDK_EXAMPLES=ON -DCELL_SDK_TESTS=ON
cmake --build build-network --target cell_network_audio_loopback
ctest --test-dir build-network -R '^cell_network_audio_loopback$' --output-on-failure
```

The example uses an ephemeral loopback port. It sends stereo PCM out of
order, repeats a packet, and advances a synthetic playout clock past one
missing frame. It checks the rendered output sample by sample.

## Send audio from a callback

Include `<cell/network_audio_sender.hpp>` and construct a `Sender` with
the sample rate and first sample agreed with the receiver. Call
`capture` from one audio callback with interleaved PCM frames. The
fixed queue can drop a whole block if the transport falls behind. The
sender still advances its sample clock, so the receiver can identify the gap.

On the transport thread, call `takePacket` until it returns an empty
vector. Each packet contains one contiguous range of samples. The
encoder splits packets at the datagram size limit and substitutes zero
for non-finite samples. Send the returned bytes as one datagram.
The UDP loopback example exercises the sender and receiver together.

## Receiver diagnostics

Poll `Receiver::stats()` from a UI or monitoring thread. It reports
accepted, invalid, late, outside-window, and overlapping datagrams.
Overlapping counts include duplicates that are still buffered.

`concealedSourceFrames` counts missing sender samples replaced with silence
and queued for playback. `unfilledOutputFrames` counts output frames filled
with silence by the audio callback while waiting for prefill or recovering
from starvation. Queued concealment only increments the source counter.
Counters wrap at the native unsigned word size and are sampled independently.

## Stream and capture a WAV between processes

Build `cell_network_audio_wave` with the SDK examples on Windows, Linux,
or macOS. For a Visual Studio build, use the executable in the configured
build directory such as `build-network/Release/cell_network_audio_wave.exe`.
Start the receiver first, then the sender. For a local 4,800-frame test:

```sh
./build-network/cell_network_audio_wave receive 127.0.0.1 39000 4800 captured.wav
./build-network/cell_network_audio_wave send 127.0.0.1 39000 input.wav
```

Use a nonempty stereo WAV. Set the receiver frame count to the input's
length and choose a new output path. On two machines on a trusted
LAN, use the receiving interface's IPv4 address in both commands.
The receiver selects the first peer that supplies a packet it can admit.
The sender uses the WAV's sample rate. Set the receiver to the same rate
with `--rate HZ` when it differs from the 48 kHz default. Rates from
8,000 to 96,000 Hz are supported. The receiver writes a 32-bit float WAV
at the stream rate and prints packet and concealment counts. The example
uses plain UDP and explicit addresses, frame count, and format.

The receiver accepts port `0` to select a free port and prints the port
when it is listening. With Python available and SDK tests enabled,
`ctest --test-dir build-network -R '^cell_network_audio_wave$'` checks
intact transfers, a deliberately dropped packet, and a packet delayed by
100 ms at the nominal sender rate. The delayed packet is concealed with a
60 ms arrival budget and recovered with a 150 ms budget. The test checks
the exact PCM, concealment count, and late packet count. On systems with
process stop and continue signals, the test also pauses the receiver
while datagrams queue in the socket. The receiver processes a bounded
batch of queued datagrams before evaluating playout deadlines.

Pass an optional arrival budget in milliseconds after the output path.
The range is 5–150 ms and the default is 20 ms. For example, append
`100` to give late packets 100 ms before the receiver fills their gaps.
WAV capture is scheduled at twice the arrival budget plus one block
from the first admitted packet. The additional lead gives the receiver
time to queue concealed frames before the capture reads them.

On a trusted LAN, select the intended sender with `--peer IPV4 PORT` on
the receiver, and append that source port to the sender command. This
matches the source IPv4 address and UDP port before admitting packets.
When omitted, the first admissible peer is used. Endpoint matching
provides peer selection for this plain UDP example.

After the first packet, the receiver allows up to two seconds without an
accepted packet. If the stream times out before capture finishes, the command
exits with an error and removes the temporary WAV. Shorter gaps still use
the configured packet-arrival budget.

## Monitor a stream on an audio output

Enable `CELL_SDK_AUDIO_DEVICE` and supply `CELL_MINIAUDIO_INCLUDE_DIR`
to build device monitoring into the WAV example. Start the monitor before
sending a 48 kHz stereo stream:

```sh
./build-network/cell_network_audio_wave monitor 127.0.0.1 39000 48000
./build-network/cell_network_audio_wave send 127.0.0.1 39000 input.wav
```

The monitor plays the requested frame count to the default stereo output
at the stream rate. Its callback pulls directly from the receive buffer with
adaptive drift correction. The socket and playout scheduler run on the
main thread. By default, the monitor selects the first admissible peer.
Add `--peer IPV4 PORT` to select a specific sender. For example, on
loopback, run the monitor with `--peer 127.0.0.1 39001` and append
`39001` as the source port to `send-input`.

With the same device-enabled build, use `send-input` to stream from the
default stereo input:

```sh
./build-network/cell_network_audio_wave monitor 127.0.0.1 39000 48000
./build-network/cell_network_audio_wave send-input 127.0.0.1 39000 48000
```

For a 44.1 kHz session, append `--rate 44100` to both commands. The
selected input and output must support the configured rate. For a
44.1 kHz WAV source, pass `--rate 44100` to `monitor` and use `send`
with the source file.

Run the commands in separate terminals and start the receiver first.
For two machines, use the receiving interface's IPv4 address. Use a
headphone output when the sender and receiver share a room. You can also
use `receive` to capture the incoming stream to a WAV file.

By default, the sender opens the stereo input and output in duplex mode
and writes silence to its local output. Add `--capture-only` to open
just the input, for example on a headless sender with no playback endpoint.
Add `--mono` for a single-channel source. The sender copies each mono sample
to both channels of the stereo stream. The device callback queues frames,
and the transport thread sends timestamped packets. The sender reports
silent frames substituted for a missing input buffer and frames dropped
when the transport queue fills. Append a source port to `send-input`
when the WAV receiver or monitor is configured with `--peer`.

### Select audio devices

Run `cell_network_audio_wave devices` to list input and output IDs for
the automatically selected backend. Specify `wasapi` on Windows,
`coreaudio` on macOS, or `alsa`, `pulse`, or `jack` on Linux to enumerate
a backend explicitly.

On the sender, pass `--input ID` to pick the capture endpoint and
`--output ID` to pick its silent duplex output. Use `--capture-only`
when the sender should open just the input. On the monitor, pass
`--output ID` to choose the playback endpoint. Use `--backend BACKEND`
to match the backend you used to list IDs. Omit an endpoint option to
use the system default. Place any existing positional source port or
arrival budget before these options.

```sh
./build-network/cell_network_audio_wave devices alsa
./build-network/cell_network_audio_wave send-input 127.0.0.1 39000 48000 --backend alsa --input "INPUT_ID"
./build-network/cell_network_audio_wave monitor 127.0.0.1 39000 48000 20 --backend alsa --output "OUTPUT_ID"
```

On Linux with examples, tests, and audio-device support enabled,
`ctest --test-dir build-network -R '^cell_network_audio_devices$' --output-on-failure`
runs the live path through named virtual ALSA devices. It checks exact
WAV capture and compares monitor output against a 44.1 kHz stereo test
signal with drift correction enabled. It also verifies that a valid datagram
from another port is ignored. The test uses a private ALSA configuration
with file and null devices. It skips when the ALSA runtime is unavailable.

On Windows, build with `CELL_SDK_AUDIO_DEVICE=ON` and point
`CELL_MINIAUDIO_INCLUDE_DIR` at miniaudio 0.11.23. In a Visual Studio
Release build, list WASAPI endpoints with:

```powershell
.\build-network\Release\cell_network_audio_wave.exe devices wasapi
```

Use `--backend wasapi` with `send-input` or `monitor` and pass an
endpoint ID from that listing. Windows CI builds both live modes and runs
the UDP tests with WAV input and output.

On macOS, use `--backend coreaudio` with either live mode. With the
Xcode generator, a Release build can list endpoint IDs with:

```sh
./build-network/Release/cell_network_audio_wave devices coreaudio
```

The macOS CI jobs build the device-enabled streamer on Apple Silicon
and Intel. CTest includes the UDP WAV tests and
`cell_network_audio_coreaudio_devices`, which enumerates endpoints
without opening an audio stream. Device capture and playback can be
checked separately on a Mac with two 48 kHz stereo endpoints.

### Run a continuous live session

Use `continuous` in place of the frame count in `monitor` or
`send-input`. Start the monitor first, then the sender:

```sh
./build-network/cell_network_audio_wave monitor 127.0.0.1 39000 continuous
./build-network/cell_network_audio_wave send-input 127.0.0.1 39000 continuous
```

Press Ctrl+C in each terminal to close its audio stream and print the
frame totals. The sender sends queued packets after the device stops.
The continuous monitor waits for its first sender. If no accepted packet
arrives for two seconds during playback, it pauses playback, clears the
session buffer, and listens on the same port. The selected audio output
stays open and resumes when the next stream is ready. The first admissible
packet sets the sample origin for each continuous session.
The monitor can join a sender already transmitting to that port.
Packets older than the new origin are late. An explicit `--peer`
persists across sessions, and the final report sums the sessions.
WAV capture still starts at sample zero and requires a finite frame count.

The continuous sender keeps capturing when the UDP stack reports a
temporarily unreachable peer or a full send buffer. It discards that
datagram and advances the sample clock, so the returning monitor can
join the current stream. The final `UDP send drops` count covers local
send errors. Packets accepted by the local UDP stack may still be lost
later in transit.

### Watch live diagnostics

Add `--stats-ms 1000` to `monitor` or `send-input` for a report every
second. The interval can be 100–60,000 ms. Each line begins with
`Stats ` followed by a JSON object, and the ordinary final report is
still printed.

The monitor reports the session number, active peer (`IPv4:port`), last
accepted packet age, and arrival budget. Its `transport_state` is
`waiting` before the first accepted packet, `active` while packets have
arrived in the last 250 ms, and `stalled` after 250 ms without an accepted
packet. The peer and packet age are JSON `null` while waiting. The existing
`state` field tracks the playback phase independently.

The monitor also reports queued playback in frames and whole milliseconds,
packet rejection counts, concealed source frames, and unfilled output
frames. Its stream counters restart with each sender session. The device
counters span the whole monitor run.

Both modes report the sample rate, whether the audio device is running or
interrupted, callback and frame counts, callback overruns, and maximum
callback duration. The sender also reports successful sends, local UDP send
drops, and missing input callbacks. A successful send means the local UDP
stack accepted the packet.

### Measure latency on hardware

Record the local reference on the left channel and the monitored return
on the right channel of the **same 48 kHz stereo WAV**. Use an isolated
broadband click or short noise burst. Choose the zero-based frame at the
start of a 256-frame reference window that contains the probe. Allow
enough capture after the probe for the maximum expected delay.

```sh
./build-network/cell_network_audio_wave measure-latency capture.wav 4800 250
```

The example searches for the corresponding return within 250 ms of
reference frame 4800. The command reports delay in frames and milliseconds,
correlation, and polarity. It rejects a weak or ambiguous match. The
allowed search limit is 1–2,000 ms.

The measurement includes all delay between the two recorder inputs,
including converter, device, and playback buffering on the chosen paths.
Use one recorder clock for both channels, and keep the capture routing
consistent when comparing builds or buffer settings.

Generate a repeatable 48 kHz stereo PCM16 stimulus with:

```sh
./build-network/cell_network_audio_wave make-latency-probe probe.wav
```

The file is 2.5 seconds long. Its tapered noise burst occupies a
256-frame window starting at frame 4800, at about −18 dBFS peak.
Both channels carry the same signal. Route a local copy to the recorder
and another copy through the sender and monitor. Use the start of the
probe window **in the recorded left channel** for `measure-latency`.
The exported probe does not overwrite an existing file.

When using the generated probe, you can let the analyzer find it in the
recorded left channel:

```sh
./build-network/cell_network_audio_wave measure-latency capture.wav auto 250
```

Include one complete burst in the first ten seconds of the recording.
The command prints the detected reference frame along with the return
delay. Supply the frame explicitly for another kind of test signal or
to select one of several repeated bursts.

### Compare latency across several probes

Generate a repeated stimulus by supplying a count of 1–32. The bursts
are 2.5 seconds apart:

```sh
./build-network/cell_network_audio_wave make-latency-probe probes.wav 3
```

After recording, provide the zero-based start of each 256-frame reference
window in the recorded left channel. List at least two starts in increasing
order. For example, if the recording has these reference positions:

```sh
./build-network/cell_network_audio_wave measure-latency-series capture.wav 250 4800 124800 244800
```

The series report includes each delay, the minimum, median, maximum,
and maximum-minus-minimum spread. Measure the reference positions in
each recording when playback and recording use separate device clocks.
