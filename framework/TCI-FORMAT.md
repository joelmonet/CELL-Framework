# Embedded TCI audio

`cell/tci.hpp` exposes an independently written reader for the observed
version-4 `TRIGGER COMPRESSED INSTRUMENT` container. `Cell::Core` supplies its
implementation. It reads the little-endian record structure, embedded sample
labels, rate and channel dimensions, and indexes the integer audio packing.
Unknown mapping/articulation records remain addressable in the original file.
This audio reader does not emulate the source instrument's detector or effects.

The observed audio uses MSB-first blocks of at most 201 interleaved scalar
values. Each block starts with an eight-bit width; values use sign-magnitude
integer packing. Indexing buffers input in 64 KiB regions. A shared source owns
one file and the block indices; each `WaveView` has a small fixed PCM cache and
independent cursor. It supplies seekable PCM24 WAVE bytes without extracting
samples or retaining the whole decompressed instrument in memory. File access,
indexing and decoding belong on preparation/cache workers, never an audio
callback. Concurrent views serialize file reads, not the decoding operation.

In the provided files the declared scalar count includes one final scalar that
has no encoded bits. The view supplies zero for that absent value. All stored
values match the paired reference PCM in the tested mono/stereo libraries;
the absent scalar cannot be recovered from this container. Declared frame and
channel dimensions stay intact, and odd RIFF payloads receive a zero pad byte.

Container/record lengths, sample dimensions, aggregate bit counts, block widths
and label extents are checked before indexing. Explicit record/sample/label
admission budgets are configurable. Unsupported versions, packing and oversized
RIFF views report errors. A WAV folder beside a TCI is not evidence that the TCI
references those files: paired libraries tested here still embed packed audio.
Other signatures and external-reference layouts need their own actual fixtures.

Synthetic tests cover a fixed bit fixture, sign extrema, zero-width magnitude,
mono/stereo order, full/partial blocks, byte seeks, concurrent sample views,
padding, truncation, budgets and mutated inputs. Private paired-library checks
remain separate evidence; no library audio is distributed in the SDK.

`cell/wave_export.hpp` adds explicit `audiofiles::exportWaveView` for TCI and GOG
views. It streams a PCM WAVE to an atomic output, with cancellation checks and
exclusive publication by default. Replacement requires an explicit flag. Failed
or cancelled exports preserve the previous destination and discard temporary
output. Rate, channels and stored sample values are retained; normalization,
resampling and trimming are not implicitly applied. The absent TCI scalar uses
the same zero fill as playback. Exporting does not change the source container.

On a worker thread, an explicit single-sample export is:

```cpp
#include <cell/tci.hpp>
#include <cell/wave_export.hpp>

auto source = std::make_shared<cell::tci::Source>(instrumentPath);
cell::tci::WaveView wave(source, sampleIndex);
cell::audiofiles::exportWaveView(wave, outputWavePath);
```

Create a separate view for export when another worker is reading playback data;
their shared source retains one file while their cursors stay independent.

The reader is an audio service. Full instrument mapping, suite import/export UI,
external-reader qualification and TCI writing remain separate work. Conversion
to another instrument format must also preserve its velocity and round-robin
mapping. Portable native library authoring and GOG writing are separate from
this audio-view exporter; accepting an input format does not provide its writer.
