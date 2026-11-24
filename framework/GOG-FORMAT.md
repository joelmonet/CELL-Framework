`cell/gog.hpp` is the shared, independent reader for the `Gog_v3.5` primary PCM
layout. It exposes the container's sample names, dynamic-group levels, sample
flags, channels, rates, bit depths and byte ranges. It reads 16-bit and 24-bit
integer PCM, mono or stereo, with up to 384 records and 16 groups. Current
admission bounds are 2 GiB per container and 100 million frames per sample.
Other layouts, compressed extensions and source-specific triggering/effects
are not claimed.

```cpp
#include <cell/gog.hpp>

auto source = std::make_shared<cell::gog::Source>(path);
const auto& sample = source->program().samples.at(sampleIndex);
cell::gog::WaveView wave(source, sampleIndex);
std::array<std::byte, 4096> bytes;
auto count = wave.read(bytes); // A standard PCM WAVE header followed by PCM.
```

`WaveView` has an independent cursor, bounded seeking and WAVE padding for an
odd PCM byte count. It never crosses into another sample. Views share one open
container and serialize its reads. Decoders can consume a view through an
adapter, or use `Source::read` with the recorded byte ranges. No extraction,
normalization or sample conversion happens here. Keep the original asset path
and sample index when saving state.

All file opening, inspection, reading and view preparation belong on control or
cache-worker threads. The service is not an audio-callback streaming API. The
product sampler adapters prepare/cache audio separately and retain their
existing voice, envelope, MIDI and resource-admission policies. MIDI velocity
zones, alternative selection and drum-pad state are product decisions; raw
group levels and sample flags remain available to other SDK consumers.

The SDK fixtures are independently generated. Product qualification also checks
three provided files containing 41 samples and two byte-identical companion WAV
payloads. User audio is not included in the SDK or repository.
