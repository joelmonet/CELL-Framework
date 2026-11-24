# Tilt dynamics

`cell/tilt_dynamics.hpp` supplies a small causal detector for signed tilt ranges.
Prepare once with the sample rate, then call `process(left, right, amounts,
split, thresholdDb, ratio, attackMs, releaseMs)` with equal-length spans. A mono
input can use an empty right span. Stereo mode links the louder input channel;
split mode uses independent `(L+R)/2` and `(L-R)/2` envelopes. Results preserve
each amount's sign and never exceed its absolute magnitude. Ratio 1 produces no
movement. Reset clears detector history.

The helper allocates no storage, adds no lookahead and does not design filters.
A product chooses its own tilt shapes and smoothing and publishes the amounts
actually applied to the response display. Studio uses prepared input storage,
64-sample minimum-phase slices and the existing linear-phase analysis cadence.
Do not interpret detector response as pitch tracking or calibrated loudness.

The independent test covers M/S isolation, linked stereo, signed bounds, unity
ratio, release, sample-rate behavior and audio-thread allocation guards.
