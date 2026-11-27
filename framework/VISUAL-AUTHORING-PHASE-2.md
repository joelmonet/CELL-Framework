# Visual authoring Phase 2

## Gap analysis

Baseline `0fa92e4` introduced theme packs, a shared asset table, interchangeable
HDR and MODERN sample styles, a material inspector, and a native Canvas preview.
Layout authors already have desktop and phone geometry, control groups, a
binding-independent skin selector, conditional control visibility, a WebView
preview and live publishing. Audio parameter mappings, gesture handling, host
automation, and accessibility belong to the existing control runtime.

| Area | Existing capability | Extension and affected files |
| --- | --- | --- |
| Materials and scalable artwork | Two-color linear fills, one outline, uniform corners, vector glyphs, raster artwork, masks, cached shadows and glow | Extend material records, Canvas fills and artwork drawing. Use `model.js`, `runtime.js`, `designer.js`, `src/ui/canvas.cpp`, `src/ui/skin_scene.cpp` and the pack schema. |
| Resolution, templates and states | Flat theme dictionaries, named styles, seven fixed visual slots, six state overrides, and layout components with remappable bindings | Resolve parent themes during model preparation. Let named visual instances inherit and override appearance independently of layout components. Expose provenance in the Designer. Extend the current state machinery and add opt-in presentation transitions. |
| Rotary, fader and layer geometry | Existing knob detents and gain reduction, fader interaction, and seven native scene slots | Generalize appearance geometry in the scene and WebView while keeping parameter, pointer, keyboard, automation, and accessibility paths in `runtime.js`. Add native scene tests for dynamic paints and cache reuse. |
| Icons and analytical displays | 24-unit Canvas vector paths, a themed gain reduction ring, live meters, native Canvas effects and existing DSP graph displays | Introduce shareable semantic icon metadata and display roles. Keep signal drawing in the live display APIs. Connect those APIs to authorable appearance without capturing signals in image assets. |
| Packaging and qualification | Versioned JSON, deduplicated data URLs, SVG sanitizing, bounded native bitmap decoding, and browser/native sample renders | Extend the pack manifest and dependency validation. Validate relative resources at import, then embed portable bytes. Reuse model, browser and native tests and renderer sample images across scales. |

## Migration strategy

Keep `cell.ui-layout.v3` and `cell.ui-presentation.v1` readable, including v1 and
v2 layout migration. Keep existing flat packs readable as `cell.ui-theme-pack.v1`.
Treat additional fields as opt-in. Old fills and control geometry retain their
current interpretation. Serialize authored definitions for round-trip editing
and export resolved, bounds-checked data to the native scene. Validate pack
dependencies before replacing any project data. Store artwork once under an
asset ID and rewrite references whenever imports rename an ID.

## Qualification sequence

1. Resolve and validate themes, named styles, local presentation overrides,
   and dependency graphs. Check that layouts and native binding headers
   remain identical during appearance changes.
2. Extend shared materials and scalable artwork. Qualify gradient, corner,
   shadow, texture, and slice geometry through the native rasterizer and
   Designer preview. Test at multiple DPI scales and during resize.
3. Add control and layer authoring, state handling, icons, and analytical
   style roles using the resolved material and geometry records.
4. Qualify reusable packages and the full workflow with native screenshots,
   layout migration, host input and recall tests, cache budgets, and timing.
   Keep WebView and native evidence identifiable.

## Authoring with Phase 2

Use **Theme and asset library → Load HDR / MODERN comparison** to open the
qualification gallery. MODERN inherits HDR's component definitions and
overrides palette and material properties. Use **Compare themes** for two
views with the same starting values, and **Preview visual state** to inspect
a state without issuing any control or host command.

### Resolution and templates

A theme can declare a `parent` in the same pack. Framework defaults provide
the baseline palette and legacy rendering. The parent chain resolves from
oldest to newest, then the control uses the active theme's default style,
its chosen style class, and local overrides. Applicable state overrides
apply last. Dictionaries and partial records merge, arrays replace, and
`null` removes inherited properties. Invalid or cyclic dependencies reject
an import with a diagnostic. A control-local material missing from a newly
selected theme falls back to its available layers and reports the missing
reference in **Local appearance and sources**.

Use named styles as linked visual templates. Editing their shared materials
updates each linked instance. Local appearance changes affect only the
selected controls. Detach captures a style's current layer references.
Copy, paste, import and export use a portable visual template with material,
icon and asset dependencies. Its format is a normal theme pack with a
`manifest.template` reference. Control positions, bindings and automation
are owned by the layout.

### Materials and artwork

Materials can define `gradient` with `kind`, `angle`, `center` and ordered
`stops`. Positions are normalized 0–1, each stop can specify opacity, and
angles use CSS degrees. `corners` are top left through bottom left clockwise.
`borders` are ordered from the outside in. `bevel`, `lighting`, and `finish`
are optional. A neutral surface can use a solid or gradient fill with these
fields omitted. `texture` sets the asset, fit, scale, offset, opacity, and
normal, multiply, or screen blending.

For slice artwork, set `slices.insets` in top, right, bottom, left order in
logical pixels. Zeroing two opposite insets yields a three-slice. Use
`minimum` to communicate the recommended minimum size without overriding the
control hit box. Use the Designer guide to inspect the source cut lines.
The native renderer pixel-aligns adjacent boundaries at the device scale,
preserves the corners, and supports independent edge and center tiling or
stretching. A transparent center can reveal the underlying material.
WebView preview uses CSS border images, whose center repeats follow the edge
mode. Native texture sampling and bevel geometry also differ from CSS.
Inspect the native image for exact artwork and geometry.

### Layers, geometry and display data

The visual inspector accepts an ordered `stack` and a `geometry` record.
Each stack layer has a material ID, a normalized rectangle, an optional text
label, anchor, pivot, opacity, transform and visibility. Transforms are
`[x, y, degrees, scaleX, scaleY]`. A value- or indicator-driven `motion`
uses a source, target and numeric range. `frame` selects the count and strip
orientation for filmstrip assets. Stack layers have no pointer handling.

Rotaries accept angular sweeps, pivot, cap rotation and unipolar or bipolar
arc settings. The **Rotary pivot** fields edit normalized coordinates for
the selected knob. Linear controls accept orientation, normalized fill origin,
unity marker and ticks. Use normalized origins to position zero or unity
correctly on asymmetric parameter ranges. The native renderer draws ticks
and unity markers from geometry. The WebView keeps its existing detents,
keyboard entry and interaction bounds.

The icon registry defines a name, category, search keywords, view box,
optical offset, vector path, semantic color and optional state variants.
Themes can override shared icons. Preview the icons at 12, 16, 20, 24, 32 or
48 logical pixels. Use a material's `icon` ID to reuse the vector.

A component may reference a named `displays` style with grid, axis formatting,
and trace roles. The native host calls `Scene::setTrace` with normalized
points. WebView hosts can send `{type:"analysis", traces:[{id,role,points}]}`.
IDs are the authored control indexes. Points are pairs `[x,y]` with 0 at the
bottom and 1 at the top. Line, filled-region and point traces use the same
live data path. Top-level meter and gain-reduction input remains available.
Large or invisible WebView displays should be updated only as often as their
visible data requires.

The WebView supports interruptible color, opacity and shadow transitions,
honoring the system's reduced-motion preference. Native skins crossfade
changed materials, continuing from their current mix when interrupted.
Native hosts can call `Scene::setReducedMotion` and schedule additional
paints while `Scene::needsFrame` returns true. WebView hosts can apply extended states using
`{type:"visual-states", controls:[{id,states}]}`. Native hosts use
`Scene::setState`. These states affect visuals only. Use `{type:"visual-values", controls:[{id,indicator}]}`
to feed a meter or modulation overlay independently of the parameter value.

### Package dependencies

Use **Import pack folder** for a directory containing `cell-theme-pack.json`
and relative assets. Paths are resolved against the explicitly selected
folder. Absolute paths, traversal, unsupported suffixes, remote lookups
and unresolved dependencies are rejected. Imported artwork is embedded on
export, and matching bytes are deduplicated. The manifest can list theme,
font, icon, asset, and preview dependencies. Expand **Artwork dependencies**
to see which resolved materials use each artwork ID. The SVG sanitization path remains
in use. Pack PNG and WebP headers are checked for dimensions and the total
raster pixel budget before rendering. Native scene assets retain their own
decoded memory budget.

**Watch theme file** can re-read a self-contained local JSON pack in browsers
with the File System Access API. Relative-asset packages use folder import.
Invalid updates leave the last valid appearance active. Editing and applying a
new pack can update the visual document without rebuilding the DSP.
For a starting manifest and knob style, see the
[minimal theme pack example](VISUAL-SKINS.md#import-a-third-party-pack).

### Native qualification

Export a wide or portrait scene and run the renderer with an optional frame
count. A multi-frame run draws the demonstration response trace and moves the
sample rotary. It measures cold and warm render times plus cache reuse. The
response samples are generated by the preview tool, independent of artwork.

```sh
cell_skin_preview cell-hdr-wide.cellskin.json hdr-125.png 1.25 120
cell_skin_preview cell-modern-wide.cellskin.json modern-200.png 2 120
cell_skin_preview cell-modern-wide.cellskin.json modern-damage.png 2 120 damage
```

For incremental updates, clip the Canvas to the host's dirty rectangle
before calling `Scene::paint`. Include any artwork, shadow or movement
outside the control bounds in that rectangle. The optional `damage` mode
measures this path on the gallery rotary. Cheap fills paint directly and
leave cache space for artwork, masks and blurred surfaces.

The model tests cover inheritance, local overrides, package dependency
rewrites, missing assets, template portability, and layout migration.
The native test checks shared nine-slice edges and transparent centers at
1×, 1.25× and 2×, ordered dynamic layers, and cache behavior during value
and trace updates. With the native preview binary enabled, the browser test
also compares the full and clipped final frames of each 2× rotary demo.
