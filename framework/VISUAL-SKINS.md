# Theme packs and native skin previews

For inheritance, ordered visual layers, and scalable artwork, see
[Visual authoring Phase 2](VISUAL-AUTHORING-PHASE-2.md).

Open `examples/ui_designer/index.html`, expand **Theme and asset library** and
choose **Load HDR / MODERN comparison**. The two themes share the same controls,
bindings, and wide and portrait positions. Use **Theme** to switch between them
while the live preview is open. Preview values and focus stay in the existing
editor. A layout or binding edit still follows the normal export and reload
path.

Use **Search assets and styles** to find an imported image or a component style
in the active theme. Click an entry to apply it to the selected compatible
control. Drag it onto a control to apply it, or onto open space to place a new
control or image. Use **Selected control style** to clear an override. Edit a
shared material's colors, gradient, roundness, borders, effects, artwork or
mask under **Material**. Changes apply to each layer that references it.

## Layout migration

`cell.ui-layout.v3` still carries control kinds, parameter keys, actions, labels,
geometry, responsive positions, and the legacy background colors. v1, v2 and
existing v3 layouts are accepted. On save, inline artwork becomes `assetId` on
controls or `backgroundAsset` on the legacy theme. Those IDs reference one copy
of each image in the optional `presentation.assets` table. A loaded layout
without an active theme keeps its original rendering.

The optional `presentation` field uses `cell.ui-presentation.v1` and contains
`activeTheme`, `themes`, and `assets`. A control's optional `visualStyle` selects a
named style when the active theme defines a compatible entry. If the entry
doesn't exist in a theme, the control uses that theme's default for its kind.
Tabs and icon buttons are appearance variants of existing button behavior.
A tab does not introduce an implicit tab group or change the action binding.
Hosted HTML packages the referenced image data and fonts for offline use.
Changing the theme leaves the generated C++ binding header unchanged.

## Import a third-party pack

Select **Import theme pack** and choose a UTF-8 JSON file with schema
`cell.ui-theme-pack.v1`. Use **Import pack folder** when the manifest refers to
relative artwork files. The Designer rejects unknown
asset and material references, remote paths, external SVG loads, and scriptable
SVG content. Colliding IDs are renamed when importing, and their references are
rewritten together. The original pack file is reusable across projects.
Include the redistribution terms for any third-party assets with your pack.

The [JSON Schema](schemas/cell.ui-theme-pack.v1.schema.json) describes the data
structure. The Designer additionally checks references, image bounds, and SVG
content. A minimal pack for a single knob style looks like this:

```json
{
  "schema": "cell.ui-theme-pack.v1",
  "assets": {},
  "themes": {
    "workbench": {
      "name": "Workbench",
      "colors": {"surface": "#262a2c", "accent": "#dac291"},
      "typography": {"label": {"family": "panel-sans", "weight": 600}},
      "materials": {
        "cap": {"shape": "ellipse", "fill": "surface", "end": "shadow",
                "edge": "edge", "edgeWidth": 1, "highlight": "highlight"},
        "glow": {"glow": {"color": "accent", "sigma": 3}}
      },
      "components": {
        "encoder": {"name": "Bench encoder", "kind": "knob",
                    "layers": {"face": "cap"},
                    "states": {"focus": {"foreground": "glow"}}}
      },
      "defaults": {"knob": "encoder"}
    }
  }
}
```

- IDs start with a lowercase letter and use up to 64 lowercase letters,
  digits, hyphens, or underscores. Each asset entry is
  `"id":{"name":"Descriptive name","data":"data:image/png;base64,…"}`. Use
  PNG, WebP or an inert SVG data URL. JPEG remains readable for older artwork.
  The same asset ID can be used as `asset` or as a `mask` in multiple materials.
- Colors use six-digit sRGB hex. Materials may also use eight-digit
  `#RRGGBBAA` values or semantic names. The palette roles are `background`,
  `surface`, `text`, `accent`, `edge`, `muted`, `metal`, `highlight` and
  `shadow`. Typography assignments use the Designer's existing font families.
- Materials accept `fill`, gradient `end`, `edge`, `edgeWidth` (0–6 px), `highlight`,
  `opacity` (0–1), `shape` (rounded or ellipse) and `radius` (0–48 px).
  `shadow`, `innerShadow`, and `glow` accept a color, `sigma` (0–18 px), and
  `x` and `y` offsets (±16 px). For simple scalable glyphs, `vector` accepts a
  `path` on a 24-unit square using the native Canvas path command subset
  (`M L H V C S A Z`) and a `stroke` width (0.1–6 units).
- Component kinds are knob, slider, fader, button, meter, panel, dropdown,
  tab and icon-button. Bind named materials to applicable `background`,
  `track`, `face`, `pointer`, `highlight`, `indicator`, and `foreground` slots.
  State overrides merge in order: enabled, hover, pressed, selected, focus
  and disabled. Each state replaces the referenced materials for its slots.
- Layouts and imported packs are bounded to 2 MiB. An embedded asset may use up
  to 1 MiB. Legacy control-face uploads retain their smaller limit. Pack
  export collects the artwork and masks used by the theme definitions.

## Validate through Cell::UI

Use **Export native preview** in the chosen Designer layout mode and theme.
It produces a `cell.ui-skin-scene.v1` JSON file with resolved materials and
geometry. The Designer prepares PNG derivatives of imported artwork once for
this export, including full SVG and WebP artwork. Vector glyphs and component
geometry remain native vectors. The scene is visual data and keeps parameter
bindings and DSP in the original project.

Build and render the exported scene:

```sh
cmake -S framework -B build-skin -DCELL_SDK_UI=ON -DCELL_SDK_EXAMPLES=ON \
  -DCELL_SDK_TESTS=ON
cmake --build build-skin --target cell_skin_preview cell_ui_skin_scene_tests
build-skin/cell_skin_preview cell-hdr-wide.cellskin.json hdr-native.png 2
build-skin/cell_skin_preview cell-modern-wide.cellskin.json modern-native.png 2
ctest --test-dir build-skin -R '^cell_ui_skin_scene$' --output-on-failure
```

`cell::ui::skins::Scene` in `<cell/ui_skin_scene.hpp>` prepares the image resources
when constructed. It reuses `CachedLayer`, `AlphaMask` and `EffectCache` for static
materials, with bounded memory and size- and DPI-aware reuse. Call `paint` on
the GUI thread. The application owns interaction and accessibility. It can
use `controlBounds`, `setValue`, `setIndicator`, and `setState` to draw the scene alongside its
existing event routing. Value changes translate or rotate a cached face or
pointer, or clip a cached meter fill. A repaint after a value change does not
decode an image or rebuild a static effect already in cache.

The native preview uses Blend2D and Cell's graphics effects. Compare its PNG at
the intended scale with the Designer preview. Browser form widgets, type
metrics, and blur kernels can differ from Canvas. The preview is a renderer
check, and host interaction and assistive technology still need testing in
the application or plug-in that will ship.
