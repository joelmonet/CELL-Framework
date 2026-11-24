# C++ standards and ownership

Cell's supported baseline is C++20, including the compiler contract exported to consumers. Use C++23 facilities only when the supported compiler, standard library and deployment targets provide them, or behind a tested optional boundary. Raising the language-version flag does not establish correctness or improve audio performance by itself.

## Ownership and lifetimes

- Give exclusive C++ ownership to `std::unique_ptr`; transfer it explicitly with `std::move`. Borrow required objects by reference and optional objects by pointer. Use `std::span` for borrowed contiguous ranges, with a documented lifetime and channel/frame units.
- Avoid new ownership APIs that pair a raw pointer with an `owns` boolean. Keep required compatibility entry points as adapters into the typed implementation. Acquire transferred ownership before a later allocation or initialization can throw.
- Give OS handles a scoped owner. Function addresses and buffers returned by C, COM and plugin SDK boundaries remain borrowed or follow that boundary's explicit release protocol. A pointer's syntax alone does not tell us who owns it.
- Child-widget lists are observers: widgets may be stack members. Do not replace those entries with owning smart pointers. Use lifetime-checked observers across callbacks that can remove a widget.
- Keep deliberate iterative destruction for deep XML/state trees. Replacing it with default recursive deletion can reintroduce stack exhaustion. Keep ABI-required factories/refcount methods distinct from ordinary C++ ownership.
- Never introduce shared ownership, locking, reclamation, or heap allocation in an audio callback merely to make a raw pointer disappear. Prepare bindings and storage outside audio; retain the existing publication and cancellation protocols.

## Macros and dispatch

Prefer normal functions, scoped names, templates, `constexpr` data and explicit dispatch over function-like macros. A macro should not silently return from its caller, rename an operating-system function, acquire ownership, or hide mutable state. Native calls that need checked wrappers must name the checked wrapper at the call site.

Retain preprocessor use for platform/header availability, symbol visibility, separately supplied ABI constants, intrinsic compilation and third-party single-header configuration. Preserve public and saved parameter identities while changing implementation style.

A simple explicit specialization/switch is often clearer than a replacement metaprogram. Do not add complex type machinery solely to remove a small macro. Keep dispatch mappings auditable against the supported module catalog.

## Validation

Use a reproducible failure when a modernization fixes a defect. Check transfer and borrowing independently, allocation/initialization failure, failed reads, repeated teardown, and native-handle loss. Validate actual OS integration where changed; distinguish a successful library load from physical-device qualification. Compile cross-platform portable code with the shipping toolchains.

Make efficiency claims from measurements. A maintainability refactor can preserve performance without claiming to improve CPU use. Keep previously published archives immutable and identify which source revision a binary actually contains.


## Callback work that depends only on one analysis update

For bounded assignment/matching loops, prepare a small fixed-size table of
invariant distances once per update rather than evaluating logarithms for the
same pair in every pass. Preserve the original numeric expression, traversal
order and tie policy when parity matters. Keep the table local/prepared, with a
known upper bound; do not replace a few arithmetic operations with heap work or
locking. The suites' eight-voice MIDI matcher now follows this pattern in source.
The arithmetic reduction does not establish a whole-plugin CPU improvement;
measure callback deadlines and tracking behavior after compilation is allowed.
