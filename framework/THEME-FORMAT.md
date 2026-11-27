# .celltheme format, version 1

A UTF-8 JSON object with `schema: "cell.ui-theme.v1"`, `name` and `material` (`classic`, `sculpted`, or `modern`). The nine required `colors` keys are `background`, `panel`, `surface`, `edge`, `ink`, `muted`, `accent`, `graph`, and `gold`. Colors accept `#RRGGBB` or `#AARRGGBB`; export uses ARGB.

Optional numeric tokens: `glow`, `texture`, and `relief` in 0–1, `radius` in 0–24, and `fontScale` in 0.75–1.5. `iconSet` defaults to `classic`; this revision accepts `classic`, `hdr`, and `modern`. The host provides the corresponding vector artwork. Theme files do not embed fonts, images or executable code.

Names are nonempty, at most 96 UTF-8 bytes and single-line. Files are bounded to 64 KiB; JSON node/text/depth limits apply. Invalid data is rejected before changing the active theme. Unknown top-level properties survive a read/write round trip. A future schema version is rejected rather than silently interpreted as v1.

Stock/user ownership is held by `Library`, never trusted from file content. Imported themes become user themes. Stock definitions are in-memory immutable values; file operations write only the user directory and `active.celltheme`.

## Native SDK integration

Build the native UI with `CELL_SDK_UI=ON` and link `Cell::UI`. Create one
`cell::ui::themes::Library` for the application's preferences and keep it alive
while its `ThemeEditor` is open. Open the editor from Settings, paint it above
the application scene, and forward pointer, keyboard and accessibility events.
Invalidate after handled input, and poll `ThemeEditor::tick()` for native
dialog results. Surface the theme editor's controls and focus while it is open.
Tab and Shift+Tab move focus, Enter activates, and the arrow keys edit a focused
slider. Space stays available to host transport outside text entry.

Edits preview live. Apply persists the active selection and Cancel restores the
last committed selection. Save User Theme writes a named user copy and makes
it active. Load User Theme opens a saved theme from the `User` folder. Import
previews a theme from an external path.

Take one library snapshot per paint. Pass its theme and revision to
`PanelCache::theme()`, and pass the current native window scale to
`PanelCache::paint()`. Use one library revision stream per cache. Cache static
panel layers and paint live meters and controls separately.
`cell::ui::PaletteScope` maps exact legacy RGB colors during Canvas drawing,
preserving alpha. New controls can use semantic colors directly. The host
resolves icon sets and applies `fontScale` to its text measurement and layout.
Theme data, dialogs, and cached surfaces are for GUI and control threads.

With `CELL_SDK_EXAMPLES=ON`, build the `cell_native_theme_editor` target to try
the editor in a native window. Press T to reopen it. The UI-enabled SDK install
also includes the example in `share/cell/examples/native_theme_editor`. Configure
that directory with CMake and point `CMAKE_PREFIX_PATH` at the SDK and Blend2D.
Run `ctest -R cell_ui_theme_system --output-on-failure` in a build configured
with `CELL_SDK_UI=ON` and `CELL_SDK_TESTS=ON`.
