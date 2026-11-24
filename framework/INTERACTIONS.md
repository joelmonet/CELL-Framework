# Cell interaction services

The framework provides parameter menus, keyboard/focus contracts, hover help, asynchronous system file dialogs, cascading popup trees and concurrent gesture admission. Studio consumes these services through its native controls. SDK/core contract tests and product-level Linux/Windows-under-Wine checks are recorded in the 0.4.6 handoff; native Windows hardware and complete accessibility/visual qualification remain separate. See [AUTOMATION.md](AUTOMATION.md) for fixed host IDs, dynamic presentation and sample-accurate host-event hooks.

## Gestures

`Engine::beginEdits`, `editValues` and `endEdits` admit bounded batches from one GUI producer. Up to 32 parameters can have open gestures. Every open parameter reserves an end event; values and additional begins cannot consume those reservations. Duplicate IDs, overlapping ownership, invalid values and insufficient capacity reject a batch before it changes the parameter bank. One-shot transactions retain their existing requirement that interactive gestures are closed first.

`cell/ui_interaction.hpp` provides `ui::GestureBroker`: up to 16 pointer identities, each owning up to four parameters, within the engine's 32-parameter total. Identity includes device kind. Only the owning contact can write its parameters; another contact cannot take them. Cancellation ends at the last accepted value, preserving a newer host value. Restore/preparation generations invalidate stale writes. These are GUI services, with fixed broker storage; they add no callback, rendering or allocation to the audio process function.

Studio uses the broker for ordinary knobs/faders, linked SQUASH threshold/makeup, CAB blend and PAN surfaces. Each parameter receives a real host begin/value/end sequence. Overlapping contacts form one local undo step when the last contact closes. Mode changes, removal, collapse, restore, resize, focus loss and teardown close invalid captures. Specialized band/curve graphs retain their existing single-pointer paths; simultaneous EQ-node editing is subsequent integration work.

## Parameter menus and keys

Studio offers Reset, Copy value and Paste value on parameter controls. Copy is local to the editor, retains the physical value and descriptor unit, and enables paste only for a matching unit and an in-range destination value. Reset/paste use the ordinary undoable edit path. Module and band menus remain available on their existing background/graph targets.

When supported, a Host automation menu entry opens the host's native menu, preserving its hierarchy and commands:

| Format | Source bridge |
| --- | --- |
| VST3 | `IComponentHandler3::createContextMenu` with the exported parameter ID and current view. |
| CLAP | `clap.context-menu/1` host popup for a parameter target; availability is checked after attachment and again on use. |
| AAX | The documented command/control/option automation-popup gesture through `AAX_IViewContainer`; still an uncompiled AAX prototype. |
| VST2.4 | Cell actions remain available; this pass adds no private host extension for native menus. |

Native windows deliver modifier-aware `KeyEvent` input. Studio adds reverse Tab, a context-menu key/Shift-F10 path, Ctrl/Cmd copy/paste/reset and undo/redo, Home/End, fine adjustment, and discrete ratio/sequence keyboard steps. Focus identities survive normal redraws and orientation changes; a collapsed module falls back to its header control. The existing file browser and other specialized modal editors retain their separate keyboard paths.

## Native input and accessibility boundary

| Platform | Source input path |
| --- | --- |
| Windows | Mouse plus `WM_POINTER` touch/pen identities when targeting Windows 8 or newer. Promoted mouse events are suppressed to avoid duplicate edits. |
| Linux/X11 | Optional XI 2.2 touch and pointer events when `X11::Xi` is available and the server accepts selection; core X11 mouse input remains the fallback. Emulated mouse events are suppressed on the XI path. |
| macOS | Existing mouse input, modifier-aware keyboard input, focus cancellation and conversion from surface pixels to native view points. Indirect trackpad contacts are not mapped to controls in this pass. |

`ControlInfo` now carries separate control identity, accessible name, formatted value and numeric bounds. `Editor::accessibleAction` offers focus, activation and ordinary undoable value changes. All input, control inspection and accessibility action calls are GUI-thread operations. A native accessibility bridge must dispatch to that thread and copy its snapshot; it must not expose a borrowed control span across threads.

**VoiceOver, UI Automation and AT-SPI providers are not implemented by this pass.** Metadata and action contracts are foundations, not screen-reader qualification. Full keyboard access in every specialized modal view and graph also remains work.

## Verification scope

`tests/interaction_tests.cpp` contains admission, reservation, ownership, cancellation, restore generation, focus, clipboard, allocation-guard and concurrent-consumer cases. `tests/studio_panel_ui_tests.cpp` adds both-orientation UI cases. Those C++ tests are written but unrun.

The Python source/grammar audit, source packaging and unchanged compilation-policy checks can run during the hold. Grammar parsing does not preprocess SDK macros, check C++ types or establish native ABI/host correctness. Native compilation and host qualification remain deferred until the user's suite-port compilation hold is released.

Continuous control drags must retain an unsnapped accumulator. Repeated fractional events must accumulate across parameter steps and redraws, and changing fine mode without moving must leave the value unchanged. ValueDrag in ui_interaction.hpp provides linear, logarithmic and stepped vertical scrubbing. Capture pointer ownership, initial values and geometry at the start; do not snap graph handles to an offset mouse-down point. Coalesce control delivery separately from UI feedback, and defer complete resource/session serialization until a gesture settles.

Windows wheel events use the Control/Shift flags captured in WM_MOUSEWHEEL rather than relying only on the current keyboard queue state after host forwarding. Bare modifier key-down events do not enter the host-shortcut forwarding path; chord keys still do.

## Windows mouse capture and keyboard focus

A completed click releases native capture without sending an extra cancellation into the editor. Losing capture during an unfinished press still cancels the gesture. This preserves computer-keyboard focus in floating instrument controllers after a normal click while releasing held controls if another window takes capture. The native Windows window test covers normal release, competing capture ownership and release after capture loss; the Studio host test checks generated notes and matched releases after an actual Windows click.

## Relative rotary motion

`RotaryMotion` locks a relative gesture to its initial dominant axis, preserving horizontal and vertical use without adding the two deltas. Preserve a fractional normalized accumulator even for stepped parameters. Commit the accumulator and pointer anchor only after the engine accepts the edit; rejected queue writes must not be counted twice on retry. Reuse bounded per-contact value storage rather than allocating for every pointer move. Absolute sliders and graph XY gestures retain their separate mappings.


Captured rack resizing uses `resizeViewportScroll(originalOffset, extent, content, trailingEdge)` from `cell/scroll_viewport.hpp`. Capture the original scroll and screen pointer once. Follow the trailing edge during pointer movement as well as painting, so releasing before the next frame cannot lose the reveal. Compute width from the original screen delta, excluding the scroll produced by that gesture. Shrinking in the same capture restores the original viewport where content bounds permit; releasing retains the final useful view.

Fader caps retain the value at the grabbed offset. A track click can still set the value immediately, followed by relative movement. Retain fractional normalized deltas across frames, apply fine adjustment to subsequent deltas, and never recompute a held gesture from a rounded display value. A fine-mode change without pointer movement must produce no parameter change.

Product control transports should distinguish ordinary scalar edits from prepared/resource/scene transactions. Studio validates bounded scalar batches against persistent module identity, topology, field metadata and its preparation policy, then updates the existing atomic bindings. It waits for a fresh processor snapshot after the gesture before recording remote-controller history. A pending structural command prevents overtaking it. Oversampling, lookahead, topology and resource changes retain acknowledged preparation.


## Repeated buttons and coupled graph controls

A semantic control identity must distinguish all simultaneously visible actions.
Include the module instance, action, parameter and selection discriminator:
previous/next buttons and adjacent tabs must never share an identity. Use that
same identity for painting, hit testing, keyboard focus and accessibility. A
parameter ID alone cannot identify several actions for one parameter. Validate
the persistent module owner before dispatching a stored hit or delayed popup
callback; an instance number can be reused after deleting a module.

Send both axes of an XY edit as one admitted scalar batch. Validate every target
before applying it, retain both UI values during capture, and record history
after the gesture settles. Use continuous delivery only when every field is
eligible and no structural/resource transaction is pending. If a write is
rejected, retain the last accepted pointer anchor and fractional accumulator.
This contract is useful for cabinet blending, spatial positions and future
multi-parameter instruments. Product-specific transaction helpers are not part
of the public framework API. Native host qualification remains required.


## Resource-backed instrument controls

A persistent module owner and the current sample-bank owner/generation identify
different lifetimes. Capture both alongside topology and instance in delayed
file/menu work. Validate both before changing a loaded library, pad or library
control; reject stale results instead of applying them to a reused instance.
Keep sample-bank edits on their prepared resource path. Ordinary scalar knobs
use existing atomic bindings where supported. Do not turn a sample replacement
into a series of unacknowledged continuous writes.

Fetch metadata only for visible views, cache pad banks by selection and resource
generation, and stop redundant display polling while a modal owns the screen.
Controller auditions need owned note release on hide, cancellation, replacement
and teardown as well as their normal timeout. Editing a selected pad's envelope
and opening a file chooser are distinct actions; preserve that choice through
the editor hooks. These practices are staged in the suites' private adapters;
their new GUI/native tests are unrun and no public sampler import API is added.

Resource-backed synthetic control identities also need the resource owner and
generation, or an equivalent captured lease checked at commit. A delayed numeric
entry must not resolve the same control index in a replacement library just
because the enclosing module still exists. Rebuild presentation descriptors when
the resource changes and retire obsolete synthetic values. The suite's private
sampler adapter now follows this rule; its runtime regressions remain unrun.
