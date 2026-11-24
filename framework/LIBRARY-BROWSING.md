# Library browsing

Use cell::ui::FileDialog::Request::directory for a native directory chooser. Leave save false. initial sets the starting directory; file-extension filters apply to file selection. Cancellation and completion remain asynchronous, and cancel() releases the request without blocking editor teardown.

cell::ui::BrowserLocations stores user-selected default and last-used directories by application-defined key. Supply an application-owned preferences file. Default locations win over last-used locations when available; missing directories fall back to the next available choice. Preferences stay outside plug-in state. File writes are atomic, and updates reread the current file so serialized edits from separate instances preserve other categories.

Use cell::assets::Browser for directory scans. Navigation cancels superseded work, poll() adopts completed immutable results, and workers never capture the editor. Present errors and truncated listings rather than silently blocking or omitting files.

PopupMenu roots keep the clicked location and scroll long command lists rather than relocating the entire root to a corner. Near an editor edge, keep the popup reachable within the viewport.

For a list whose rows select an item and also offer alternate destinations, set PopupMenu::Item::activateParent. A row click or Enter runs its command; hover/click the right-hand arrow or press Right for child actions. This is opt-in; ordinary submenu rows retain hover expansion. maximumWidth() allows long asset names without changing other menus. Keep remote/session ownership in the product controller, independent of temporary editor projections or audio graphs.


## Async menu ownership (r38 source, uncompiled)

Capture `PopupMenu::generation()` immediately after opening a pending menu.
`isCurrent(token)` works before the first paint, remains true during navigation
and becomes false on close/replacement. Check it before adopting a directory or
catalogue result. Module/resource owners are additional leases, not substitutes
for menu lifetime. Open clears old hit/accessibility rows immediately, preventing
old row pointers from surviving until the next paint.

Run `assets::Browser::scan` with extension filters and a presentation bound;
`poll` performs no directory IO. On completion, populate only the menu that owns
the captured token; otherwise cancel/drop the view operation. Keep native Browse
available for files beyond the menu bound. Empty/error/truncated listings must
be distinguished. Cancel scans on hide/teardown; workers retain only paths and
options, never the editor. The suites now apply this to shared CAB/NAM/IR/
wavetable lists and presets. Unicode labels use UTF-8 path conversion on Windows.
The new popup/editor regressions are written but unrun.
