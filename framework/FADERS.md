# Native image-backed fader caps

Include `<cell/ui_faders.hpp>` and link `Cell::UI`. The helper draws a caller's
cap artwork in existing thumb bounds. It supplies no parameter normalization,
gesture ownership, model-specific style selection or product state.

```cpp
// On the editor/worker thread, once per immutable artwork asset:
cell::ui::faders::PreparedCap cap(encodedPngBytes);

// In an editor paint callback, retaining cap's lifetime:
cap.paint(canvas, verticalThumbBounds);
cap.paint(canvas, horizontalThumbBounds, true);
cap.paint(canvas, disabledThumbBounds, false, false);
```

Preparation uses the existing bounded `decodeImage` and owns the decoded
Surface. The defaults allow 65,536 source pixels and a 256-pixel prepared edge;
pass explicit bounds when the asset needs a different budget. Decode failure
throws to the caller. Prepare/replace/retire images outside any paint that
borrows them. No file I/O or image decoding occurs in `paint()`.

`drawCap(canvas, bounds, callerOwnedSurface, horizontal, enabled)` supports an
already prepared image without taking ownership. Drawing preserves source
aspect ratio and rotates the artwork by a quarter turn for horizontal travel.
The caller still chooses thumb size, track endpoints, DPI transforms, hit areas,
hover/focus treatment and accessibility metadata. Empty/nonfinite bounds and
empty surfaces produce no drawing; enabled=false applies a dimming opacity.
Neither preparation nor drawing is an audio-callback operation.

This addition contains no bundled skins, product module identities, style
policy, preset schema, networking or DSP. Existing Designer artwork can be
exported by its owner and prepared through this API. The example accepts an
image filename to make that ownership boundary explicit.

```sh
cmake -S examples/native_fader -B build-fader -DCMAKE_PREFIX_PATH=/path/to/cell
cmake --build build-fader
./build-fader/cell_native_fader cap.png preview.png
```

This is a source addition. The separate product integration exercises real
native module painting; it does not certify every host/platform or promise
that native Canvas rendering never allocates internally.
