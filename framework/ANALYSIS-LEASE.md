# Bounded visual analysis — r38 source, uncompiled

`cell/analysis_lease.hpp` supplies an Apache 2.0 `cell::AnalysisLease` for optional
meters, visual tuners and analyzers. A control thread renews a frame budget; the
single audio consumer advances it once per real callback. A missing final GUI
stop cannot leave capture enabled indefinitely when polling stops.

```cpp
cell::AnalysisLease lease;
// Editor/control thread, while the display is visible:
lease.renew(static_cast<std::uint32_t>(sampleRate * 0.5));
// Audio owner, every callback, even while the display is inactive:
if (lease.consume(block.frames))
    boundedSamples.publish(block); // caller-owned, nonblocking tap
// Editor/control thread, when hidden/closed:
lease.stop();
```

The sample transport and its background detector belong to the caller. The
helper provides neither a queue nor audio processing. Renewing or stopping does
not alter sound, routing or plugin latency. One admitted block can finish past
the deadline/stop; expiry is therefore bounded by the requested duration plus
at most a callback block. It is suitable for visual capture, not a permission
barrier or sample-accurate musical gate.

`consume(0)` returns false without advancing. `renew(0)` stops. `active()` is a
best-effort diagnostic. There is one audio consumer and one serialized control
owner. Do not replace or destroy the object while either thread uses it. The
audio cursor may wrap; comparisons use unsigned modular distances with the
32-bit budget and block bounds far below half the 64-bit clock range.

The callback uses a private cursor and lock-free atomic publication/load. It has
no retry loop, wall-clock calls, heap operations or mutex. This is a source
contract; native platform compilation and allocation/concurrency fixtures are
pending while compilation is held. No CPU improvement percentage is claimed.

The Live visual TUNE integration uses a half-second lease and an existing
bounded SPSC tap. Its background analysis carries module owner, topology,
stream sequence, sample rate and drop count. Replacing a module, losing samples
or hiding its display cancels old jobs/history. That commercial integration is
not included in the public framework archive; this independent helper, guide
and regression source are included in its source inventory.
