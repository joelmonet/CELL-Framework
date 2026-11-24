# Cell unit UI standards

These standards apply to new Cell units. Existing Studio ports continue to reproduce their original controls and workflows first. The independent API is in `include/cell/ui_components.hpp` and `ui_values.hpp`; the optional gain editor in `examples/sdk_gain/editor.cpp` demonstrates authoring without a product UI reference or product DSP dependencies. Shared framework qualification does not compile the held Studio product.

## What the current interfaces teach us

The useful inherited ideas are a single parameter attachment per control, consistent numeric editing, controls grouped by the task, clear selected modes, component-relative layouts, dedicated portrait arrangements and separate display refresh. The Sound Cell interfaces add useful model-specific knob materials, meaningful control labels, compact filter sections and selected-band editing. Those behaviors are worth retaining as contracts. Their implementation belongs in reusable Cell services rather than being copied into every new panel.

## Required practices

1. **One source of parameter truth.** Bind by stable parameter ID. Store physical values in state and automation. Formatting, detents, units and visual materials must not renumber parameters or alter the DSP range. A detent affects pointer/wheel editing; typed fine values and host automation remain valid.
2. **Layout by structure.** A unit owns its header, body and grouped control tiles. Use explicit alternate arrangements for narrow and wide views. Labels and values center within the control they describe. Close and collapse stay at the upper right; further actions belong in a menu or body section. Include only actions the unit actually supports.
3. **Readable type before packing more controls.** Default labels use 11 logical pixels and numeric values use 12. Shorten or wrap labels and expose their full accessible names rather than shrinking text indefinitely. Use tabular numeric text for changing measurements.
4. **Shared interaction geometry.** Default spacing is 12 logical pixels. Pointer targets are at least 24 pixels; the touch metrics use 44-pixel targets. Numeric-entry and slider regions must be distinct and stay inside their tile. Adapt the layout when space runs out.
5. **Visible state.** Distinguish enabled, disabled, hover, press, selection, keyboard focus and invalid entry. Keep disabled labels readable. Use color together with text, shape or position. The palette is semantic: background, panel, surface, edge, ink, muted ink, accent and error.
6. **One interaction contract, several visual treatments.** Analytical and character presentations share binding, geometry, focus, numeric entry and gesture behavior. A model can change materials without creating a separate control implementation.
7. **Complete input lifetimes.** Use the gesture broker for owned parameter edits. Close gestures on release, cancellation, focus loss, hide, removal and state replacement. Use `Engine::beginMomentary` for press-and-hold actions: its release value and closing event are reserved together under queue pressure.
8. **Honest value text.** Parse the whole entry, validate its unit and reject nonfinite or out-of-range values. Keep a rejected entry available for correction. Accept compatible unit conversions such as kHz to Hz. Show silence or infinity only when the processor explicitly gives that endpoint the corresponding meaning.
9. **Keyboard and accessibility are part of the control.** Give every interactive face a stable editor identity, role, name, physical value, range, unit and enabled state. Support focus traversal, arrows, fine adjustment, numeric entry, reset and cancellation. Expose equivalent accessible actions. Native screen-reader providers are still a separate implementation requirement.
10. **Displays report the running engine.** Use bounded, versioned snapshots with instance identity. Separate requested settings from the prepared graph's actual latency and state. Mark unavailable/stale data instead of drawing invented activity. Never read mutable DSP internals from the editor.
11. **Keep work off audio.** Painting, FFT displays, text, file access, asset decoding and allocation belong outside the audio callback. Coalesce display work, cache layout/metadata and suspend hidden views. Publish completed work through bounded queues or prepared graph replacement. Rendering and background-job budgets need native measurements before qualification.
12. **Undo the user's action.** A drag, paired fader edit, patch recipe, band split or asset change should form one meaningful operation. Admit complete edits before changing state. Preserve inactive model memories, routes and assets. A full queue must leave a clear retry path rather than a partial edit.

On Linux, distinguish native focus entry from loss. X11 can send a normal NotifyPointer FocusOut before FocusIn when a host gives explicit focus to its embedded, pointer-focused child. Retain the click's edit only when the server now explicitly focuses that same child. Real external focus loss, queued genuine losses and keyboard grabs must still cancel active input. The native scaling fixture exercises those transitions with real X11 windows, alongside shortcut forwarding and fractional-scale input.

Context clicks also acquire native focus before opening an editor or menu that accepts typing. Check entry from both a host parent and a separate active host window; a value editor must continue receiving keys when the pointer moves away.

Apply the same policy to core X11 and XInput2 pointer events. The scaling test's optional `--native-input` fixture uses the separately supplied XTest runtime to exercise real button delivery, including XInput2 when the UI library is built with it. Synthetic core events alone do not qualify that path.

An embedded host may activate its frame after the first pointer focus request. Complete that handoff at most once within one second, only when the focused window is an actual ancestor and the pointer's child chain reaches the editor. Clear the request on typing, hiding, unmapping, closing or a genuine loss. Keyboard grabs and focus changes after the pointer leaves must still release active input.

## Starting a new unit

Define its processor parameters and state independently of its editor. Choose a `ValueFormat` for each physical dimension. Arrange `controlGrid`/`controlGeometry` tiles from the available body rectangle, paint with `parameterKnob` or the shared button primitive, and derive control metadata with `parameterInfo`. Supply pointer/keyboard/accessibility handlers through Cell's existing editor and gesture APIs.

The gain example offers an optional editor via `CELL_GAIN_UI`; configure the independent SDK with its UI component enabled and supply the renderer separately. Linux SDK UI tests require an X display. For an isolated Xvfb server, use `-noreset` so closing the last test window does not race a server reset before the next window opens.

## Qualification

Check narrow/wide and portrait/landscape arrangements, text centering, clipped labels, touch sizes, keyboard order, cancellation, queue pressure, invalid input, state recall and hidden-window cost. Measure the native interfaces on Windows, macOS and Linux. Source presence and grammar checks do not establish rendering, accessibility, host or performance qualification.

## Default interaction policy

`cell/ui_defaults.hpp` supplies the shared editor/interaction defaults. Reuse
`cell/knob_position.hpp` to place physical unity at noon even for asymmetric
ranges; it changes pointer geometry without renumbering automation values.
Bare Escape and Space return to the host after local menus, entry fields and
active drags have been dismissed. A plugin must not destroy its embedded host
window directly. File actions use native OS browsing; popup anchors use the
actual click position. Submenus open after the shared hover delay, while an
explicit selectable category may activate its first child.

Keep product startup chains and DSP presets in product policy code, separate
from these SDK defaults. Apply factory defaults only to new instances; never
silently overwrite a restored project or a saved user device configuration.
