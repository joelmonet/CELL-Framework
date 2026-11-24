# Cell graphics effects — r36 foundation

This revision adds independently written CPU effects and reusable raster layers.
The existing Blend2D software canvas remains the renderer. It does not add a GPU
vector backend, shaders, backdrop blur or a replacement widget toolkit. Studio
0.4.19 was frozen and shipped before these additions; its UI is unchanged.

## Components

`Cell::Graphics` is dependency-free C++20: `cell/graphics_effects.hpp`. It is built
by the independent SDK even with UI disabled. `BlurWorkspace::alpha` and `argb`
filter tightly packed alpha8 or premultiplied ARGB32 images in place. Radius is
specified as approximate Gaussian standard deviation in physical pixels. Three
separable fractional-box passes interpolate continuously through integer radii.
Running sums and clipped line initialization make the work linear in image
pixels, independent of radius, including single-row and single-column images. Float scratch avoids repeated
8-bit intermediate rounding. This is a Gaussian approximation, not an exact
Gaussian convolution. Transparent and clamped boundary modes are explicit.

`Cell::UI` additionally supplies `cell/ui_effects.hpp`:

- `AlphaMask`: rasterize any Canvas path, text or geometry, capture a Surface's
  alpha, or upload a tight alpha8 span. Unchanged pixels preserve the revision.
- `EffectCache`: tinted drop shadows/glows and clipped inner shadows. Each editor
  owns its cache. Masks have stable identities and content revisions. Color,
  opacity and position changes reuse the blurred alpha; inner-offset changes
  recolor/recompose without blurring again. Glow is a shadow with zero offset.
- `CachedLayer`: repaint only when content version, logical size, DPI or explicit
  invalidation changes. Moving a layer does not repaint it. Transparent pixels
  are initialized. Fractional DPI uses ceil-rounded physical pixels and clipping,
  rather than stretching a rounded raster into a smaller logical rectangle.
  Failed callbacks leave the last completed layer available for a retry.

```cpp
#include <cell/ui_effects.hpp>
cell::ui::AlphaMask mask;
mask.rasterize(240,80,[](cell::ui::Canvas& c) {
    c.rounded({8,8,224,64},12,cell::ui::Color(0xffffffff));
});
cell::ui::EffectCache effects; // retain with the editor, not inside paint()
// Inside paint, at scale 1 (mask pixels already match the rendering DPI):
// effects.draw(canvas,mask,{20,30},{6,{0,4},cell::ui::Color(0x80000000)});
```

For DPI 2, rasterize the mask at twice the logical resolution and scale its
Canvas by 2, then pass `scale=2` to `draw`. Sigma and offsets remain logical units.
Existing canvas transforms, alpha and clips apply to the final effect image.

## Memory and thread rules

These services allocate and belong on the GUI thread or an owned worker. They
must never run in an audio callback. Concurrent access to a cache, mask, layer or
workspace requires caller synchronization. There is no global image cache.

Images are limited to 8192 pixels per edge and 16,777,216 pixels total. Blur sigma
must be finite and within 0..256 physical pixels; scale must be finite in (0,8].
The padded shadow image must also fit those image limits. Invalid requests throw
before mutating the input blur pixels. Memory allocation failure also preserves
those input pixels. Uploaded colors are straight-alpha tints; input ARGB blur
images must already be premultiplied. Positive kernels preserve premultiplication.

Default cache limits are 64 entries and 16 MiB of retained alpha/surface pixels.
LRU eviction enforces both. This limit excludes caller-owned masks/layers, vector
metadata, transient construction images and reusable blur scratch. Scratch is
reported separately and can retain up to two float images (8 bytes per work
pixel). A cold color build also temporarily needs a 4-byte-per-output-pixel upload
and a new surface. Use smaller masks/cache regions for large interfaces. The
maximum allowed image is a validation ceiling, not a recommended allocation.
`setLimits` evicts immediately. A zero budget or oversized entry renders without
retaining that entry; image-work bounds still apply. `clear`, `releaseScratch`,
and `CachedLayer::clear` explicitly release their respective storage. Statistics
are cumulative since construction and distinguish blur builds, cache hits,
color builds, uncached draws, evictions and retained/scratch bytes.

## Build and example

With UI enabled, link `Cell::UI`; the graphics library is transitive. Headless
consumers use `find_package(Cell CONFIG REQUIRED COMPONENTS Graphics)` and link
`Cell::Graphics`. No extra effects dependency is fetched. Enable
`CELL_SDK_TESTS=ON` for reference/filter/cache/DPI tests. With `CELL_SDK_UI=ON` and
`CELL_SDK_EXAMPLES=ON`, `cell_effects_gallery output.png` renders the standalone
gallery. `cell_effects_benchmark` reports a repeatable uncached/cached shadow
draw microbenchmark at DPI 1 and 2, with retained/scratch bytes and rebuild
counts. Timings describe this operation and machine, not overall plugin CPU.
Its CMake entry also works as an installed SDK consumer.

## Renderer direction and provenance

The public Canvas/Surface interface remains the renderer boundary. A future GPU
implementation must preserve the same geometry, clipping, transforms, alpha,
DPI and offscreen-image contracts, and must be tested against software readback.
No GPU renderer, partial native redraw scheduler or hardware performance claim
is included here. Replacing the suite's current rendering path is separate work.

Feature research included [Visage](https://github.com/VitalAudio/visage) and the
[Melatonin Blur documentation](https://melatonin.dev/manuals/melatonin-blur/).
No implementation code from either project, Gin, JUCE or other UI expansions was
copied. Existing SDK/Blend2D notices continue to apply. This is an effects and
caching foundation, not feature parity with either library. Benchmark results
for other libraries do not describe this implementation or overall plugin CPU.
