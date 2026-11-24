# Filter slopes and rack viewport behavior

`cell/filter_slopes.hpp` defines stable persisted slope IDs, ascending menu order,
and bounded Butterworth section coefficients for 1–16 poles. Standard HP/LP
choices are 6, 12, 18, 24, 36, 48, 60, 72 and 96 dB/oct. Odd orders include their
real first-order pole. Coefficients, filter state storage and response displays
must all use the same order mapping; do not use `1 << choice` after expanding an
existing choice list. Existing IDs 0–3 retain 6/12/24/48, while newer IDs append.

The product graphic EQ retains its legacy 12/24/48/96 IDs and circuit voicings.
18/36/60/72 append to that bank. New HP/LP options use the corresponding real
Butterworth order. The finite-gain bell/notch 18 voice interpolates the two
neighboring owned circuits; a finite-gain EQ skirt is not an unbounded cutoff
stopband. Studio, Live and Immersive use matching response and processing code.

`cell/scroll_viewport.hpp` provides minimum movement to reveal newly committed
content, preserves an already visible viewport, shows the leading edge of
oversized content, and clamps stale scrolling after content shrinks. Use the
same measured geometry for rendering and focus reveal, including group spacing,
folded sizes and scale. Keep a pending reveal across asynchronous editor
rebuilds, rather than applying a second approximate camera move.

Popup menus choose above/below placement according to the space needed by the
whole list. Hardware-pad learning controls belong to the controller's pad view;
keyboard mode retains its existing playing/transport shortcuts.
