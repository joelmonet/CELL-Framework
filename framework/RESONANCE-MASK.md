# Gentle resonance analysis — r38 source, uncompiled

`cell/resonance_mask.hpp` supplies a fixed-storage, analysis-only mask for
persistent upper-mid harshness or low-mid congestion. It does not perform audio
filtering. Caller-supplied magnitude spectra determine positive spectral flux,
local prominence and persistence. A small neighbouring-bin kernel softens mask
steps. Low-mid detection protects coherent second/third harmonics, checking
neighbouring bins for off-bin leakage. No acoustic transparency claim is made.

Call prepare(sampleRate, hop) outside audio; use at least eight bins including
DC/Nyquist for a real FFT. Geometry and frequency-focus weights are cached on
preparation. analyze() allocates no storage. Magnitudes are sanitized, and a
relative floor limits decisions on noise far below the current spectrum peak.
Amounts/depth/selectivity are bounded and nonfinite controls become neutral.
Silent/zero-amount/invalid-size frames clear the output mask. These behaviors
have deferred test source; they have not been executed in this revision.

The upper-mid focus covers 1.2–10 kHz around 3.5 kHz; the low-mid focus covers
90 Hz–1 kHz around 300 Hz. Focus weights are not a substitute for the user's
separate boundary filters. The detector caps at 6/8 dB respectively. Apply
weightedResonanceCut() AFTER boundary and graphic weighting to retain that cap
and the user's smaller Depth. Ratio slope also applies there. Strong graph
boosts therefore change emphasis without multiplying the intended cap. Targets
are bounded; a smoothing release may take time to reach a lowered Depth.

The native Studio and studio-profile shared kernels apply that mask in their
existing STFT. Live uses its existing causal peak-filter renderer and retains
zero added audio buffering. Individual Live filter targets obey the cap, but
cascaded/overlapping filter responses are not a certified total-response bound.
Neither method is qualified as phase-transparent or artifact-free. M/S weights,
external detectors, transient behavior, harmonic protection and listening must
be tested against representative audio. The older spectral modes keep their
separate processing equations and do not use this new detector.

New work removes duplicate legacy prominence calculations from the gentle
mode branch. Live sample/filter smoothing poles are prepared once per rate;
the detector's logarithmic focus geometry is also prepared once rather than
recomputed each analysis frame. These reduce repeated work in source. They do
not establish a measured whole-plugin CPU saving or a faster native deadline.

Deferred tests: resonance_mask_tests and the expanded spectral_routing_tests.
The latter compares independent M/S lanes, external-detector routing, callback
storage and unchanged latency for modes 0–7. No compiler/runtime/listening or
CPU comparison was run; see VALIDATION.md.
