"use strict";

const CellDesignerModel = (() => {
    const columns = 12;
    const rows = 8;
    const maxControls = 256;
    const maxAssetBytes = 256 * 1024;
    const maxProjectBytes = 2 * 1024 * 1024;
    const schema = "cell.ui-layout.v3";
    const clipboardSchema = "cell.ui-controls.v1";
    const wideGrid = {columns, rows, maxHeight: rows};
    const phoneGrid = {columns: 4, rows: 4096, maxHeight: 8, breakpoint: 480};
    const kinds = {
        slider: {width: 4, height: 1, title: "Slider"},
        knob: {width: 3, height: 2, title: "Knob"},
        fader: {width: 3, height: 3, title: "Fader"},
        choice: {width: 4, height: 1, title: "Choice"},
        toggle: {width: 3, height: 1, title: "Toggle"},
        meter: {width: 4, height: 2, title: "Meter"},
        readout: {width: 3, height: 1, title: "Readout"},
        text: {width: 4, height: 1, title: "Text"}
    };
    const defaultTheme = {background: "#111b20", surface: "#1b2a30", text: "#eef4ef", accent: "#bcef90"};
    const skins = {modern: "Modern", field: "1940s Field Radio", studio: "1960s Recording Studio",
        broadcast: "1950s Broadcast"};
    const showcase = [
        {kind: "text", label: "Field radio · recording studio · broadcast", x: 0, y: 0, width: 12, height: 1},
        {kind: "knob", label: "Field Radio Gain", key: "gain", x: 0, y: 1, width: 3, height: 3, skin: "field",
            phone: {x: 0, y: 1, width: 2, height: 3}},
        {kind: "fader", label: "Studio Fader", key: "gain", x: 3, y: 1, width: 3, height: 5, skin: "studio",
            phone: {x: 2, y: 1, width: 2, height: 3}},
        {kind: "meter", label: "Broadcast Peak", key: "output-peak", x: 6, y: 1, width: 6, height: 2, skin: "broadcast"},
        {kind: "meter", label: "Broadcast RMS", key: "output-rms", x: 6, y: 3, width: 6, height: 2, skin: "broadcast"}
    ];
    const imageType = /^image\/(png|jpeg|webp)$/;
    const imageData = /^data:image\/(png|jpeg|webp);base64,[a-z0-9+/]+={0,2}$/i;
    const color = /^#[0-9a-f]{6}$/i;
    const clamp = (value, upper) => Math.min(Math.max(value, 0), upper);
    const hasArtwork = kind => kind === "knob" || kind === "fader";
    const hasParameter = kind => kind !== "text" && kind !== "meter";
    const hasBinding = kind => kind !== "text";
    const validText = value => typeof value === "string" && value.trim() && value.length <= 80 && !/[\x00-\x1f\x7f]/.test(value);

    function readCatalog(value) {
        if (value?.type !== "parameters" || !Array.isArray(value.parameters))
            throw new Error("Choose a Cell parameter snapshot.");
        const keys = new Set();
        return value.parameters.map(parameter => {
            const {key, name, unit, minimum, maximum, step, value, choices = []} = parameter ?? {};
            if (!validText(key) || keys.has(key) || typeof name !== "string" || typeof unit !== "string" ||
                !Array.isArray(choices) || !choices.every(choice => typeof choice === "string") ||
                ![minimum, maximum, step, value].every(Number.isFinite) ||
                minimum > maximum || step < 0 || value < minimum || value > maximum)
                throw new Error("The snapshot contains an invalid or duplicate parameter.");
            keys.add(key);
            return {key, name, unit, minimum, maximum, step, value, choices};
        });
    }

    function readTheme(input) {
        const theme = {...defaultTheme};
        for (const key of Object.keys(theme)) {
            if (input?.[key] === undefined) continue;
            if (typeof input[key] !== "string" || !color.test(input[key])) throw new Error("The layout has an invalid theme color.");
            theme[key] = input[key].toLowerCase();
        }
        return theme;
    }

    function readProject(text) {
        const value = JSON.parse(text);
        if (![schema, "cell.ui-layout.v2", "cell.ui-layout.v1"].includes(value?.schema) ||
            !Array.isArray(value.controls) || value.controls.length > maxControls)
            throw new Error("Choose a supported Cell UI layout.");
        const controls = value.controls.map((control, index) => readControl(control, index, value.schema === schema));
        const legacy = value.schema === "cell.ui-layout.v1";
        return {controls, theme: readTheme(value.theme),
            catalog: legacy ? undefined : value.catalog === null || value.catalog === undefined
                ? null : readCatalog({type: "parameters", parameters: value.catalog})};
    }

    function readControl(control, index, hasPhone) {
        const {kind, label, key = "", x, y, width = kinds[kind]?.width, height = 1,
            accent = "", artwork = "", skin = "modern", phone} = control ?? {};
        if (typeof kind !== "string" || !Object.hasOwn(kinds, kind) || !validText(label) ||
            (hasBinding(kind) && !validText(key)) || typeof skin !== "string" || !Object.hasOwn(skins, skin) ||
            !validBox({x, y, width, height}, wideGrid) ||
            typeof accent !== "string" || (accent && !color.test(accent)) ||
            typeof artwork !== "string" ||
            (artwork && (!hasArtwork(kind) || artwork.length > Math.ceil(maxAssetBytes / 3) * 4 + 32 || !imageData.test(artwork))))
            throw new Error(`Invalid settings for control ${index + 1}.`);
        const result = {kind, label, key: hasBinding(kind) ? key : "", x, y, width, height, accent, artwork, skin};
        if (hasPhone && phone !== undefined) {
            if (!validBox(phone, phoneGrid)) throw new Error(`Invalid phone placement for control ${index + 1}.`);
            const {x, y, width, height} = phone;
            result.phone = {x, y, width, height};
        }
        return result;
    }

    function clipboardSize(text) {
        if (text.length > maxProjectBytes || new TextEncoder().encode(text).length > maxProjectBytes)
            throw new Error("The clipboard data exceeds 2 MiB.");
    }

    function writeClipboard(controls) {
        const text = JSON.stringify({schema: clipboardSchema, controls});
        clipboardSize(text);
        return text;
    }

    function readClipboard(text) {
        clipboardSize(text);
        let value;
        try {
            value = JSON.parse(text);
        } catch {
            throw new Error("The clipboard does not contain Cell controls.");
        }
        if (value?.schema !== clipboardSchema || !Array.isArray(value.controls) ||
            !value.controls.length || value.controls.length > maxControls)
            throw new Error("The clipboard does not contain a supported set of Cell controls.");
        return value.controls.map((control, index) => readControl(control, index, true));
    }

    function serialize(controls, theme, catalog) {
        return JSON.stringify({schema, controls, theme, catalog}, null, 2) + "\n";
    }

    function validBox(box, grid) {
        if (!box) return false;
        const {x, y, width, height} = box;
        return Number.isInteger(width) && width >= 1 && width <= grid.columns &&
            Number.isInteger(height) && height >= 1 && height <= grid.maxHeight &&
            Number.isInteger(x) && x >= 0 && x <= grid.columns - width &&
            Number.isInteger(y) && y >= 0 && y <= grid.rows - height;
    }

    function constrain(box, grid) {
        const width = Math.max(1, Math.min(box.width, grid.columns));
        const height = Math.max(1, Math.min(box.height, grid.maxHeight));
        return {x: clamp(box.x, grid.columns - width), y: clamp(box.y, grid.rows - height), width, height};
    }

    function overlaps(a, b) {
        return a.x < b.x + b.width && b.x < a.x + a.width &&
               a.y < b.y + b.height && b.y < a.y + a.height;
    }

    function firstOpenPlace({width, height}, controls, grid = wideGrid) {
        for (let y = 0; y <= grid.rows - height; y++) {
            for (let x = 0; x <= grid.columns - width; x++) {
                if (!controls.some(control => overlaps({x, y, width, height}, control))) return {x, y};
            }
        }
        return {x: 0, y: 0};
    }

    function bounds(boxes) {
        return {
            left: Math.min(...boxes.map(box => box.x)),
            top: Math.min(...boxes.map(box => box.y)),
            right: Math.max(...boxes.map(box => box.x + box.width)),
            bottom: Math.max(...boxes.map(box => box.y + box.height))
        };
    }

    function moveBoxes(boxes, grid, dx, dy) {
        const {left, top, right, bottom} = bounds(boxes);
        const x = Math.max(-left, Math.min(dx, grid.columns - right));
        const y = Math.max(-top, Math.min(dy, grid.rows - bottom));
        return boxes.map(box => translate(box, x, y));
    }

    function translate({x, y, width, height}, dx, dy) {
        return {x: x + dx, y: y + dy, width, height};
    }

    function alignBoxes(boxes, edge) {
        const {left, top, right, bottom} = bounds(boxes);
        return boxes.map(box => {
            const {x, y, width, height} = box;
            return {
                x: edge === "left" ? left : edge === "right" ? right - width : x,
                y: edge === "top" ? top : edge === "bottom" ? bottom - height : y,
                width, height
            };
        });
    }

    function distributeBoxes(boxes, axis) {
        if (boxes.length < 3) return null;
        const {left, top, right, bottom} = bounds(boxes);
        const start = axis === "x" ? left : top;
        const finish = axis === "x" ? right : bottom;
        const size = axis === "x" ? "width" : "height";
        const space = finish - start - boxes.reduce((total, box) => total + box[size], 0);
        if (space < 0) return null;
        const ordered = boxes.map((box, index) => ({box, index}))
            .sort((a, b) => a.box[axis] - b.box[axis]);
        const result = [];
        let cursor = start;
        ordered.forEach(({box, index}, i) => {
            result[index] = {...translate(box, 0, 0), [axis]: cursor + Math.round(i * space / (boxes.length - 1))};
            cursor += box[size];
        });
        return result;
    }

    function findGroupPlacement(boxes, occupied, grid) {
        if (boxes.every(box => occupied.every(other => !overlaps(box, other))))
            return boxes.map(box => translate(box, 0, 0));
        const {left, top, right, bottom} = bounds(boxes);
        const width = right - left;
        const height = bottom - top;
        const pattern = new Uint16Array(height);
        const taken = new Uint16Array(grid.rows);
        const mark = (mask, geometry, originX, originY) => {
            for (const box of geometry) {
                const bits = ((1 << box.width) - 1) << (box.x - originX);
                for (let y = box.y - originY; y < box.y - originY + box.height; y++)
                    mask[y] |= bits;
            }
        };
        mark(pattern, boxes, left, top);
        mark(taken, occupied, 0, 0);
        const usedRows = Array.from(pattern.entries()).filter(([, bits]) => bits !== 0);
        for (let y = 0; y <= grid.rows - height; y++) {
            for (let x = 0; x <= grid.columns - width; x++) {
                if (usedRows.every(([row, bits]) => !(taken[y + row] & (bits << x))))
                    return boxes.map(box => translate(box, x - left, y - top));
            }
        }
        return null;
    }

    const readingOrder = (a, b) => a.y - b.y || a.x - b.x;
    const phoneSize = kind => ({width: phoneGrid.columns, height: kinds[kind].height});

    function phonePlacements(controls) {
        const placements = controls.map(control => control.phone);
        const pinned = placements.filter(Boolean);
        const ordered = controls.map((control, index) => ({control, index}))
            .sort((a, b) => readingOrder(a.control, b.control));
        let nextRow = 0;
        for (const {control, index} of ordered) {
            if (placements[index]) continue;
            const box = {x: 0, y: nextRow, ...phoneSize(control.kind)};
            // Automatic controls span the full width. Skip pinned boxes before
            // continuing the wide layout's reading order below this control.
            for (let blocker; (blocker = pinned.find(other => overlaps(box, other)));)
                box.y = blocker.y + blocker.height;
            placements[index] = box;
            nextRow = box.y + box.height;
        }
        return placements;
    }

    function parameterKeys(controls) {
        return [...new Set(controls.filter(control => hasParameter(control.kind)).map(control => control.key))];
    }

    function meterKeys(controls) {
        return [...new Set(controls.filter(control => control.kind === "meter").map(control => control.key))];
    }

    function twoState(parameter) {
        const {minimum, maximum, step, choices = []} = parameter;
        const tolerance = Math.max(1e-9, step * 1e-6);
        return maximum > minimum && step > 0 && Math.abs(minimum + step - maximum) <= tolerance &&
            (!choices.length || choices.length === 2);
    }

    function bindingIssues(controls, catalog) {
        const keys = new Map(catalog.map(parameter => [parameter.key, parameter]));
        return controls.flatMap(control => {
            if (!hasParameter(control.kind)) return [];
            const parameter = keys.get(control.key);
            if (!parameter) return [`${control.label}: unknown key ${control.key}`];
            if (control.kind === "choice" && !parameter.choices.length)
                return [`${control.label}: ${control.key} has no choices`];
            if (control.kind === "toggle" && !twoState(parameter))
                return [`${control.label}: ${control.key} needs two discrete values`];
            return [];
        });
    }

    // Three-digit byte escapes make the header independent of source-file encoding.
    function cppBindings(controls) {
        const names = {slider: "slider", knob: "knob", fader: "fader", choice: "choice", toggle: "toggle", readout: "readout"};
        const literal = key => {
            const bytes = new TextEncoder().encode(key);
            return '"' + [...bytes].map(byte => byte >= 32 && byte <= 126 && byte !== 34 && byte !== 92 && byte !== 63
                ? String.fromCharCode(byte) : "\\" + byte.toString(8).padStart(3, "0")).join("") + '"';
        };
        const entries = controls.filter(control => hasParameter(control.kind));
        const keys = parameterKeys(controls);
        return `// Generated by Cell UI Designer.
#pragma once
#include <array>
#include <string_view>
#include <cell/web_parameter_bridge.hpp>

namespace cell::designer {
enum class ControlKind { slider, knob, fader, choice, toggle, readout };
struct Binding {
    ControlKind kind;
    std::string_view key;
    cell::ParamId parameter;
};
inline constexpr std::array<Binding, ${entries.length}> controls{{${entries.map(({kind, key}) =>
    `\n    {ControlKind::${names[kind]}, ${literal(key)}, cell::stableId(${literal(key)})},`).join("")}
}};
inline constexpr std::array<cell::ParamId, ${keys.length}> parameters{{${keys.map(key =>
    `\n    cell::stableId(${literal(key)}),`).join("")}
}};
inline constexpr std::array<std::string_view, ${meterKeys(controls).length}> meters{{${meterKeys(controls).map(key => `\n    ${literal(key)},`).join("")}
}};
inline cell::web::ParameterBridge bind(cell::Engine& engine) {
    return {engine, parameters};
}
} // namespace cell::designer
`;
    }

    return {wideGrid, phoneGrid, kinds, skins, defaultTheme, showcase, maxControls, maxAssetBytes, maxProjectBytes, imageType,
        clamp, validText, hasArtwork, hasParameter, hasBinding, readProject, readCatalog, serialize, overlaps,
        writeClipboard, readClipboard,
        constrain, firstOpenPlace, moveBoxes, alignBoxes, distributeBoxes, findGroupPlacement,
        readingOrder, phoneSize, phonePlacements, bindingIssues, parameterKeys, meterKeys, twoState, cppBindings};
})();
