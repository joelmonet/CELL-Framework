# Host transport availability

`Transport::tempoValid` distinguishes a host-supplied positive, finite tempo
from the numerical fallback (`120`). CLAP, VST3, VST2.4, AAX, AUv2 and AUv3
adapters set it from their host's availability flag or callback result. Missing,
nonpositive or nonfinite host values leave `tempoValid=false`. Offline mode and
the other timeline flags are independent.

Legacy C++ producers retain a default of `true`; a producer without a usable
tempo must explicitly set it to `false`. Consumers must not infer availability
from a BPM value alone. A product may resolve a held or manual clock and mark
the resolved transport valid while separately displaying host availability.
Host BPM values retain their fractional precision. Whole-number custom tempo
entry is a product policy, not a limitation of this API.

Rebuild consumers against matching headers and libraries: the transport layout
changed. Parameter-event segmentation and bounded runtime chunks preserve the
flag. The independently installed `tests/sdk_transport_consumer` fixture builds
CLAP/VST3 adapters using separately supplied official SDKs and checks real
exported callbacks, including missing contexts and invalid tempo values. It
does not qualify VST2.4/AAX hosts or a DAW audio device.

On macOS and iOS, AUv3 reads the host's musical context and transport once
per render block. A processor can receive the tempo, time signature,
project beat and sample positions, playback, recording, loop range, and offline
mode. Beat and sample positions can be zero or negative. Unreported positions
remain unavailable. The render timestamp is used for scheduling and the host's
transport callback supplies the project sample position.

The macOS installed SDK fixture also checks that AUv3 passes those values
through to the processor, advances positions at an automation split, and
handles missing or malformed context.
