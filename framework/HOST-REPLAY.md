# Reproduce CLAP host interactions with Doctor

Build the plug-in with this SDK, then launch a fresh DAW process with
recording enabled. Point the DAW at the .clap path printed by Doctor.
For a generated project, Doctor uses its CLAP artifact and plug-in ID.

```sh
cell doctor BankedTrim --build-dir BankedTrim/build \
  --record host-trace --record-host /path/to/reaper --host-arg=-newinst
```

Reproduce the issue, then unload the plug-in or quit the DAW cleanly.
Doctor waits for the launched process and lists the traces in its report.
Each instance writes a separate `instance-N/host.celltrace` file. Omit
`--record-host` to print the recording environment for a DAW launched
separately. Use `--plugin /path/to/Effect.clap` to reuse a built bundle
and skip the project build, and add `--plugin-id ID` to filter the DAW's
instances when recording from a prebuilt bundle.

The trace keeps the CLAP activation and reset sequence, process and
parameter-flush boundaries, block lengths, sample offsets, and host
steady time. It records parameter gestures and values, editor parameter
edits, MIDI 1.0, SysEx, MIDI 2.0 UMP, CLAP note events and transport.
It also records CLAP state loads, successful state saves, and Cell
CLAP preset recalls. Audio inputs replay as silence by default. Pass
`--record-audio` to include the original float input buffers. State,
MIDI, and audio can contain session data. Review what you recorded
before sharing a trace.

```sh
cell doctor BankedTrim --build-dir BankedTrim/build \
  --replay host-trace/instance-1/host.celltrace

cell doctor --plugin /path/to/BankedTrim.clap \
  --replay host-trace/instance-1/host.celltrace --realtime
```

Replay opens the bundle in Doctor's minimal CLAP host and uses the
recorded plug-in ID. It checks process return codes, state-load acceptance,
and saved state bytes. The first difference identifies the trace command
in `replay-clap.log`. Each run also writes `clap-replay.json` with callback
and event counts. Add `--replay-timeout SECONDS` for long traces.
The helper runs in a separate process so Doctor still reports a crash or
timeout.

The replay host services deferred CLAP main-thread callbacks between trace
commands. It reproduces the recorded plug-in calls with separate main
and audio threads. DAW GUI behavior, product-specific editor commands,
host-extension responses, and device behavior need the original host.
The recorder uses 32 MiB of preallocated space per instance. Capacity
overruns, unsupported event spaces, and concurrent host calls that prevent
ordering cause an incomplete trace that Doctor rejects. A DAW crash before
instance teardown can leave the recording directory empty. Capture shorter
sequences when audio input is included.

## Read the optional real-time report

`--realtime` paces recorded real-time blocks using each block's frame count
and sample rate. Percentiles use nearest rank. Timings measure wall time
inside the plug-in's CLAP `process` call and include scheduler preemption.
Trace decoding, file I/O, and report generation are outside that interval.

| Field | Meaning |
| --- | --- |
| `p99_9_callback_ns`, `worst_callback_ns` | Callback wall time in nanoseconds. |
| `deadline_misses` | Timed callbacks taking longer than their frame budget. |
| `allocations` | C++ `new` calls on the callback thread in the diagnostic plug-in, including array and aligned allocations. A null value means the plug-in exposes no allocation probe. C allocation APIs and other modules or threads are outside this counter's scope. |
| `underruns` | Simulated playback periods whose callback finished after the scheduled end or returned a processing error. The scheduler starts a fresh period after a miss. Offline and zero-frame callbacks are unpaced. |
| `device_underruns` | Null, since the replay host has no physical audio device. |

Doctor enables the allocation probe when rebuilding a generated project
for `--realtime`. For an existing generated build, configure with
`-DCELL_PROJECT_DOCTOR_ALLOCATIONS=ON`. Custom CMake targets can pass
`CLAP_DOCTOR_ALLOCATIONS` to `cell_add_clap_plugin`. Use the probe for
diagnostic builds. The ordinary allocation smoke check still exercises
`Engine::process`, and the graph profiler measures offline DSP by block
size. [Doctor checks](PROJECT-GENERATOR.md#check-a-project-with-cell-doctor)
· [Graph profiler](PORTABLE-GRAPH.md#profile-dsp-by-block-size)
