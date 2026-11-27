// SPDX-License-Identifier: Apache-2.0
"use strict";

// Optional sample data. The Designer imports these through the public pack reader.
const CellDesignerThemeExamples = (() => {
    const kinds = ["knob", "slider", "fader", "button", "meter", "panel", "dropdown", "tab", "icon-button"];
    function definition(name, colors, edging, shadow, artwork) {
        const face = {shape: "ellipse", fill: "metal", end: "shadow", edge: "edge", edgeWidth: edging,
            highlight: "highlight", shadow: {sigma: shadow, y: 3, color: "#0000007a"}};
        const materials = {
            card: {fill: "surface", end: "background", edge: "edge", edgeWidth: edging, radius: 11,
                innerShadow: {sigma: 2, y: 1, color: "#00000038"}},
            rail: {fill: "shadow", end: "background", edge: "edge", edgeWidth: edging, radius: 4},
            dial: {...face, asset: artwork},
            "dial-track": {shape: "ellipse", fill: "shadow", edge: "edge", edgeWidth: edging},
            cap: {...face, shape: "rounded", radius: 7, edgeWidth: edging},
            pointer: {fill: "text", radius: 2}, fill: {fill: "accent", radius: 4},
            arrow: {fill: "text", vector: {path: "M5 9 L12 16 L19 9", stroke: 1.8}},
            reset: {fill: "text", vector: {path: "M5 11 A7 7 0 1 1 8 18 M5 5 L5 11 L11 11", stroke: 1.8}},
            shine: {fill: "#ffffff12", end: "#ffffff00", radius: 8},
            selected: {fill: "surface", edge: "accent", edgeWidth: 1, radius: 8,
                glow: {sigma: 4, color: colors.accent + "3d"}},
            hover: {fill: "metal", end: "surface", edge: "highlight", edgeWidth: 1, radius: 8},
            pressed: {fill: "background", edge: "accent", edgeWidth: 1, radius: 8},
            focus: {edge: "accent", edgeWidth: 2, radius: 10},
            disabled: {fill: "background", opacity: .65, radius: 8}
        };
        const layers = {
            knob: {background: "card", track: "dial-track", face: "dial", pointer: "pointer", indicator: "fill"},
            fader: {background: "card", track: "rail", face: "cap", pointer: "pointer"},
            slider: {background: "card", track: "rail", face: "cap", indicator: "fill"},
            meter: {background: "card", track: "rail", indicator: "fill"},
            panel: {background: "card", foreground: "shine"},
            button: {background: "card", face: "cap", highlight: "shine"},
            dropdown: {background: "card", face: "cap", pointer: "arrow"},
            tab: {background: "card", face: "cap", indicator: "fill"},
            "icon-button": {background: "card", face: "cap", pointer: "reset"}
        };
        const components = Object.fromEntries(kinds.map(kind => [kind, {name: kind, kind, layers: layers[kind], states: {
            hover: {background: "hover"}, pressed: {background: "pressed"}, selected: {background: "selected"},
            focus: {foreground: "focus"}, disabled: {foreground: "disabled"}
        }}]));
        return {name, colors, typography: {label: {family: "panel-sans", weight: 600, tracking: .35},
            value: {family: "system-mono"}}, materials, components,
            defaults: Object.fromEntries(kinds.map(kind => [kind, kind]))};
    }
    return {schema: "cell.ui-theme-pack.v1", assets: {
        "graphite-face": {name: CellDesignerArtwork.knob[0].name, data: CellDesignerArtwork.knob[0].artwork}
    }, themes: {
        hdr: definition("HDR", {background: "#171615", surface: "#282624", text: "#ece1c8", accent: "#c7a46b",
            edge: "#645b4b", muted: "#ac9f87", metal: "#4a4742", highlight: "#baa68a", shadow: "#161514"}, 1, 4, "graphite-face"),
        modern: definition("MODERN", {background: "#151e25", surface: "#202c36", text: "#e0eef4", accent: "#63d5eb",
            edge: "#354654", muted: "#93aab8", metal: "#3b4a57", highlight: "#607684", shadow: "#111a22"}, .5, 3)
    }};
})();
