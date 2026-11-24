# Prepared state and native pixel presentation

An editor must remain usable when its host stops calling process. Pending
complete DSP replacements must not occupy a new queue slot for every edit.
`cell::LatestPreparedExchange<T>` provides a single pending replacement with
bounded storage. Its serialized producer may replace an unclaimed candidate;
its audio consumer keeps ownership of the current object until the next
`audioUpdate`. Superseded and retired objects are destroyed on the producer.
The audio handoff allocates nothing, takes no mutex and destroys no objects.

Call `publishLatest(candidate, commit)` on the control thread after fully
preparing the candidate. The non-throwing commit receives the candidate and a
monotonic ticket before publication. The publisher consumes the unique pointer
only on success. Call `audioUpdate()` between uses of the previous audio borrow.
Stop both threads before destroying the exchange. Use `PreparedExchange` for
explicit request cancellation or retained-previous crossfades; this new class
does not change those existing contracts. Studio and Live recall racks opt in
to the latest-pending policy through their shared source.

`cell_latest_prepared_exchange` covers 10,000 stopped-consumer edits with two
and three storage slots, resumption with the newest candidate, preservation of
the active borrow, 50,000 concurrent publications, bounded storage, ordering,
zero rejection with the default three-slot pool, and audio allocation/deletion
guards. It also runs as a native ASan/UBSan test in this update's evidence.

Windows bitmap presentation must describe the renderer's physical row stride,
not assume `visibleWidth * 4`. The owned PRGB32 surface has positive, aligned
rows; GDI's bitmap storage width is `stride / 4`, while the blit width remains
the visible width. This avoids a row copy/allocation on every frame and prevents
diagonal corruption at unaligned window widths. Linux and macOS already pass
the physical stride to their presentation APIs.

With `CELL_SDK_UI=ON` and Windows tests enabled,
`cell_windows_surface_presentation` compares actual HWND/GDI pixels with a
reference raster across ten aligned/unaligned widths and five content scales,
including repeated resizing and 1475-pixel width. Geometry and pointer-coordinate
checks remain separate from pixel-presentation checks.

For interactive graphs, retain a copy of the complete captured target, its
value domain and relevant owner/generation for the duration of the gesture.
Painting may rebuild hit regions and reconcile keyboard focus, but must not
retarget an active gesture to an unrelated or parameterless button. Cancel when
the captured owner/schema disappears. Pure spectrum displays do not acquire
parameter-edit gestures. The product's Studio graph-focus correction lives in
its shared editor code; SDK component input already keeps capture distinct
from keyboard focus. Product-specific band planners are not bundled here.
