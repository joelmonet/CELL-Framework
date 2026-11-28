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
            arrow: {icon: "chevron"},
            reset: {icon: "reset"},
            warning: {fill: "#5b302b", edge: "#e38e75", edgeWidth: 2, radius: 8},
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
            focus: {foreground: "focus"}, disabled: {foreground: "disabled"}, warning: {background: "warning"}
        }}]));
        components.knob.geometry = {angles: [-140, 140], pivot: [.5, .5], arc: "bipolar", origin: .5, ticks: 13};
        components.fader.geometry = {orientation: "vertical", origin: 0, unity: .62, ticks: 9};
        components.analysis = {name: "Response panel", kind: "panel", layers: {background: "card"}, analysis: "response"};
        const displays = {response: {background: "background", major: "edge", minor: "#839da123",
            axes: {range: [-18, 6], precision: 0, unit: "dB"},
            traces: {"frequency-response": {color: "accent", width: 2}, sidechain: {color: "highlight", width: 1}}}};
        return {name, colors, typography: {label: {family: "panel-sans", weight: 600, tracking: .35},
            value: {family: "system-mono"}}, materials, components, displays,
            defaults: Object.fromEntries(kinds.map(kind => [kind, kind]))};
    }
    const hdr = definition("HDR", {
        background: "#171615", surface: "#282624", text: "#ece1c8", accent: "#c7a46b",
        edge: "#645b4b", muted: "#ac9f87", metal: "#4a4742", highlight: "#baa68a", shadow: "#161514"
    }, 1, 4, "graphite-face");
    hdr.materials.card.bevel = {kind: "raised", width: 1.5, angle: 160, light: "#ffffff19", dark: "#00000058"};
    const modern = {
        name: "MODERN", parent: "hdr",
        colors: {background: "#151e25", surface: "#202c36", text: "#e0eef4", accent: "#63d5eb",
            edge: "#354654", muted: "#93aab8", metal: "#3b4a57", highlight: "#607684", shadow: "#111a22"},
        materials: {card: {bevel: null, edgeWidth: .5, corners: [13, 13, 9, 9]},
            dial: {asset: null, edgeWidth: .5, gradient: {kind: "radial", center: [.44, .35],
                stops: [{at: 0, color: "metal"},{at: .55, color: "surface"},{at: 1, color: "shadow"}]}},
            "dial-track": {edgeWidth: .5}, cap: {edgeWidth: .5}, selected: {glow: {color: "#63d5eb3d"}}}
    };
    return {schema: "cell.ui-theme-pack.v1", assets: {
        "graphite-face": {name: CellDesignerArtwork.knob[0].name, data: CellDesignerArtwork.knob[0].artwork}
    }, icons: {
        chevron: {name: "Disclosure chevron", category: "toolbar", keywords: ["open", "menu"], path: "M5 9 L12 16 L19 9"},
        reset: {name: "Reset", category: "toolbar", keywords: ["restore", "undo"], path: "M5 11 A7 7 0 1 1 8 18 M5 5 L5 11 L11 11",
            states: {warning: {color: "accent"}}}
    }, themes: {hdr, modern}};
})();
