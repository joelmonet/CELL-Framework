"use strict";

const CellDesignerModel = (() => {
    const columns = 12;
    const rows = 8;
    const maxControls = 256;
    const maxAssetBytes = 256 * 1024;
    const maxPanelAssetBytes = 1024 * 1024;
    const maxProjectBytes = 2 * 1024 * 1024;
    const maxDisplayCodes = 128;
    const schema = "cell.ui-layout.v3";
    const clipboardSchema = "cell.ui-controls.v1";
    const wideGrid = {columns, rows, maxHeight: rows};
    const phoneGrid = {columns: 4, rows: 4096, maxHeight: 8, breakpoint: 480};
    const kinds = {
        slider: {width: 4, height: 1, title: "Slider"},
        knob: {width: 3, height: 2, title: "Knob"},
        fader: {width: 3, height: 3, title: "Fader"},
        stepper: {width: 4, height: 1, title: "Stepper"},
        bank: {width: 4, height: 1, title: "Bank"},
        program: {width: 4, height: 1, title: "Program"},
        "patch-name": {width: 4, height: 1, title: "Patch Name"},
        "program-grid": {width: 12, height: 4, title: "Program Grid"},
        choice: {width: 4, height: 1, title: "Choice"},
        toggle: {width: 3, height: 1, title: "Toggle"},
        momentary: {width: 3, height: 1, title: "Momentary"},
        action: {width: 3, height: 1, title: "Action"},
        "action-pair": {width: 4, height: 1, title: "Action Pair"},
        meter: {width: 4, height: 2, title: "Meter"},
        readout: {width: 3, height: 1, title: "Readout"},
        led: {width: 3, height: 1, title: "LED Display"},
        text: {width: 4, height: 1, title: "Text"},
        image: {width: 4, height: 2, title: "Image"},
        rectangle: {width: 4, height: 2, title: "Rectangle"},
        line: {width: 4, height: 1, title: "Line"}
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
    const imageType = /^image\/(png|jpeg|webp|svg\+xml)$/;
    const imageData = /^data:image\/(png|jpeg|webp|svg\+xml);base64,([A-Za-z0-9+/]+={0,2})$/;
    const imageFits = ["contain", "cover", "fill"];
    const svgNamespace = "http://www.w3.org/2000/svg";
    const svgElements = new Set(("svg g defs symbol use path rect circle ellipse line polyline polygon " +
        "linearGradient radialGradient stop pattern clipPath mask marker text tspan textPath title desc style").split(" "));
    const localId = /^#[\w.:-]+$/;
    const color = /^#[0-9a-f]{6}$/i;
    const clamp = (value, upper) => Math.min(Math.max(value, 0), upper);
    const isPanelArt = kind => kind === "image" || kind === "rectangle" || kind === "line";
    const isProgramControl = kind => ["bank", "program", "patch-name", "program-grid"].includes(kind);
    const hasDisplayCodes = kind => ["led", "stepper", "readout", "choice"].includes(kind);
    const hasArtwork = kind => kind === "knob" || kind === "fader" || kind === "image";
    const hasBinding = kind => !["text", "action", "action-pair"].includes(kind) && !isPanelArt(kind) && !isProgramControl(kind);
    const hasParameter = kind => hasBinding(kind) && kind !== "meter";
    const validText = value => typeof value === "string" && value.trim() && value.length <= 80 && !/[\x00-\x1f\x7f]/.test(value);
    const plainNumber = "[+-]?(?:\\d+(?:\\.\\d*)?|\\.\\d+)(?:[eE][+-]?\\d+)?";
    const validAction = key => typeof key === "string" && /^[a-z0-9][a-z0-9._-]{0,79}$/i.test(key);
    const buttonDefaults = {holdMs: 2000, repeatDelay: 350, repeatMs: 85, chordMs: 1000};
    const typographyFamilies = {
        "panel-sans": {label: "Panel Sans", stack: '"Cell Panel Sans", system-ui, sans-serif'},
        "panel-condensed": {label: "Panel Condensed", stack: '"Cell Panel Condensed", "Arial Narrow", system-ui, sans-serif'},
        "system-sans": {label: "System sans", stack: "system-ui, sans-serif"},
        "system-serif": {label: "System serif", stack: "ui-serif, Georgia, serif"},
        "system-mono": {label: "System monospace", stack: "ui-monospace, monospace"}
    };
    const silkscreen = {family: "panel-condensed", size: 15, weight: 600, tracking: 1.2, alignment: "left", letterCase: "uppercase"};

    function readLabel(value) {
        if (typeof value !== "string") return null;
        const label = value.replace(/\r\n?/g, "\n");
        return label.trim() && label.length <= 240 && label.split("\n").length <= 8 &&
            !/[\x00-\x09\x0b-\x1f\x7f]/.test(label) ? label : null;
    }

    function typographySettings(value) {
        if (value === undefined) return undefined;
        if (!value || typeof value !== "object" || Array.isArray(value)) throw new Error("Invalid typography settings.");
        const {family, size, weight, tracking, alignment, letterCase} = value;
        const bounded = (number, min, max) => Number.isFinite(number) && number >= min && number <= max;
        if ((family !== undefined && (typeof family !== "string" || !Object.hasOwn(typographyFamilies, family))) ||
            (size !== undefined && !bounded(size, 8, 96)) ||
            (weight !== undefined && (!Number.isInteger(weight) || !bounded(weight, 100, 900))) ||
            (tracking !== undefined && !bounded(tracking, -2, 12)) ||
            (alignment !== undefined && !["left", "center", "right"].includes(alignment)) ||
            (letterCase !== undefined && !["none", "uppercase", "lowercase"].includes(letterCase)))
            throw new Error("Invalid typography settings.");
        const settings = Object.fromEntries(Object.entries({family, size, weight, tracking, alignment, letterCase})
            .filter(([, setting]) => setting !== undefined));
        return Object.keys(settings).length ? settings : undefined;
    }


    function conditionRules(expression = "") {
        const invalid = () => { throw new Error("Use numeric conditions such as device = 1, joined by && or ||."); };
        if (typeof expression !== "string" || expression.length > 512) invalid();
        if (!expression.trim()) return [];
        const start = /\s*("(?:[^"\\]|\\.)*"|[\w./:-]+)\s*(<=|>=|==|!=|=|<|>)\s*/;
        const clause = new RegExp(start.source + "(" + plainNumber + ")\\s*", "y");
        const groups = [[]];
        let offset = 0;
        for (let comparisons = 0; comparisons < 12; ++comparisons) {
            clause.lastIndex = offset;
            const match = clause.exec(expression);
            if (!match) invalid();
            let key = match[1];
            if (key.startsWith('"')) {
                try { key = JSON.parse(key); } catch { invalid(); }
            }
            const value = Number(match[3]);
            if (!validText(key) || !Number.isFinite(value)) invalid();
            groups.at(-1).push({key, operator: match[2] === "==" ? "=" : match[2], value});
            offset = clause.lastIndex;
            if (offset === expression.length) return groups;
            const join = expression.slice(offset, offset + 2);
            if (join === "||") groups.push([]);
            else if (join !== "&&") invalid();
            offset += 2;
        }
        throw new Error("A condition can contain up to 12 comparisons.");
    }

    function compareValue(left, operator, right) {
        switch (operator) {
            case "=": return left === right;
            case "!=": return left !== right;
            case "<": return left < right;
            case "<=": return left <= right;
            case ">": return left > right;
            case ">=": return left >= right;
            default: return false;
        }
    }

    function variantsDisjoint(first, second) {
        if (!first || !second) return false;
        const conflicts = (a, b) => a.key === b.key &&
            ((a.operator === "=" && !compareValue(a.value, b.operator, b.value)) ||
             (b.operator === "=" && !compareValue(b.value, a.operator, a.value)));
        return conditionRules(first).every(a => conditionRules(second).every(b =>
            a.some(left => b.some(right => conflicts(left, right)))));
    }

    function parseLabelMap(text) {
        const linePattern = new RegExp("^(" + plainNumber + ")\\s*=\\s*(.+)$");
        return text.split(/\r?\n/).map(line => line.trim()).filter(Boolean).map(line => {
            const match = line.match(linePattern);
            if (!match) throw new Error("Enter label mappings as one value = label per line.");
            return {value: Number(match[1]), label: match[2]};
        });
    }

    function ruleSettings({visibleWhen, enabledWhen, labelFrom}) {
        const result = {};
        if (conditionRules(visibleWhen).length) result.visibleWhen = visibleWhen.trim();
        if (conditionRules(enabledWhen).length) result.enabledWhen = enabledWhen.trim();
        if (labelFrom !== undefined) {
            const {key, source, labels} = labelFrom ?? {};
            if (!validText(key) || !["choice", "name", "value", "map"].includes(source))
                throw new Error("Choose a parameter key and label source.");
            result.labelFrom = {key, source};
            if (source === "map") {
                if (!Array.isArray(labels) || !labels.length || labels.length > 128)
                    throw new Error("Enter 1–128 label mappings.");
                const values = new Set();
                result.labelFrom.labels = labels.map(entry => {
                    const {value, label} = entry ?? {};
                    if (!Number.isFinite(value) || !validText(label) || values.has(value))
                        throw new Error("Label mappings need unique numeric values and labels of up to 80 characters.");
                    values.add(value);
                    return {value, label};
                });
            }
        }
        return result;
    }

    function ruleKeys(control) {
        return [...new Set([
            ...conditionRules(control.visibleWhen).flat().map(rule => rule.key),
            ...conditionRules(control.enabledWhen).flat().map(rule => rule.key),
            ...(control.labelFrom ? [control.labelFrom.key] : [])
        ])];
    }

    function buttonSettings(kind, input) {
        const {action, trigger = "tap", minusAction, plusAction, chordAction, holdMs = buttonDefaults.holdMs,
            repeatDelay = buttonDefaults.repeatDelay, repeatMs = buttonDefaults.repeatMs, chordMs = buttonDefaults.chordMs} = input;
        const duration = (value, minimum, maximum) => Number.isInteger(value) && value >= minimum && value <= maximum;
        if (kind === "action") {
            if (!validAction(action) || !["tap", "hold", "repeat", "momentary"].includes(trigger))
                throw new Error("Enter a valid action key and trigger.");
            if (trigger === "hold") {
                if (!duration(holdMs, 100, 10000)) throw new Error("Hold time must be 100–10000 ms.");
                return {action, trigger, holdMs};
            }
            if (trigger === "repeat") {
                if (!duration(repeatDelay, 100, 10000) || !duration(repeatMs, 40, 2000))
                    throw new Error("Repeat delay must be 100–10000 ms and interval must be 40–2000 ms.");
                return {action, trigger, repeatDelay, repeatMs};
            }
            return {action, trigger};
        }
        const settings = {};
        if (kind === "action-pair") {
            if (!validAction(minusAction) || !validAction(plusAction))
                throw new Error("Enter valid minus and plus action keys.");
            Object.assign(settings, {minusAction, plusAction});
        }
        if (["stepper", "action-pair"].includes(kind) && chordAction !== undefined && chordAction !== "") {
            if (!validAction(chordAction) || !duration(chordMs, 100, 10000))
                throw new Error("Enter a valid two-button action key and hold time of 100–10000 ms.");
            Object.assign(settings, {chordAction, chordMs});
        }
        return settings;
    }
    // Segments a–g and the decimal point (p), used as the dot in the question mark.
    const ledGlyphs = Object.freeze({
        " ": "", "-": "g", "?": "abegp",
        0: "abcdef", 1: "bc", 2: "abdeg", 3: "abcdg", 4: "bcfg", 5: "acdfg",
        6: "acdefg", 7: "abc", 8: "abcdefg", 9: "abcdfg",
        A: "abcefg", a: "abcdeg", B: "cdefg", b: "cdefg", C: "adef", c: "deg",
        D: "bcdeg", d: "bcdeg", E: "adefg", e: "abdefg", F: "aefg", f: "aefg",
        G: "acdef", H: "bcefg", h: "cefg", I: "bc", i: "c", J: "bcde",
        L: "def", l: "ef", n: "ceg", O: "abcdef", o: "cdeg", P: "abefg",
        q: "abcfg", r: "eg", S: "acdfg", t: "defg", U: "bcdef", u: "cde", y: "bcdfg"
    });

    function readDisplayCodes(entries) {
        if (!Array.isArray(entries) || entries.length > maxDisplayCodes)
            throw new Error(`Use up to ${maxDisplayCodes} display codes.`);
        const values = new Set();
        return entries.map(entry => {
            const {value, code} = entry ?? {};
            if (!Number.isFinite(value) || typeof code !== "string" || !code.length || code.length > 2 || code.includes(" ") ||
                ![...code].every(character => Object.hasOwn(ledGlyphs, character)))
                throw new Error("Each display code needs a finite value and one or two supported characters.");
            if (values.has(value)) throw new Error(`Value ${value} has more than one display code.`);
            values.add(value);
            return {value, code};
        });
    }

    function parseDisplayCodes(text) {
        const entries = text.split(/\r?\n/).map(line => line.trim()).filter(Boolean).map(line => {
            const match = line.match(/^([+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:e[+-]?\d+)?)\s*=\s*(\S+)$/i);
            if (!match) throw new Error("Enter display codes as one value = code pair per line.");
            return {value: Number(match[1]), code: match[2]};
        });
        return readDisplayCodes(entries);
    }

    function displayCode(entries, value, step) {
        if (!entries?.length) return undefined;
        const exact = entries.find(entry => entry.value === value);
        if (exact) return exact.code;
        // Decimal parameters can round-trip a few ulps from the authored value.
        let nearest;
        let distance = Infinity;
        for (const entry of entries) {
            const delta = Math.abs(entry.value - value);
            const tolerance = 4 * Number.EPSILON * Math.max(1, Math.abs(entry.value), Math.abs(value));
            if (delta < distance && delta <= tolerance && (!(step > 0) || delta < step / 4)) {
                nearest = entry;
                distance = delta;
            }
        }
        return nearest?.code;
    }

    function numericLedCode(value) {
        if (!Number.isInteger(value) || value < -9 || value > 99) return "--";
        return String(value).padStart(2, "0");
    }

    function safeSvgCss(value) {
        const withoutLocalUrls = value.replace(/url\(\s*(['"]?)(#[\w.:-]+)\1\s*\)/gi, "");
        return !/[\\@]|\/\*|\b(?:url|image(?:-set)?|paint|src)\s*\(/i.test(withoutLocalUrls);
    }

    function safeSvg(markup) {
        const document = new DOMParser().parseFromString(markup, "image/svg+xml");
        const root = document.documentElement;
        if (document.doctype || document.querySelector("parsererror") || root.localName !== "svg" ||
            [...document.childNodes].some(node => node.nodeType === 7)) return false;
        return [root, ...root.querySelectorAll("*")].every(element =>
            element.namespaceURI === svgNamespace && svgElements.has(element.localName) &&
            (element.localName !== "style" || safeSvgCss(element.textContent)) &&
            [...element.attributes].every(attribute => {
                const name = attribute.localName.toLowerCase();
                if (attribute.namespaceURI === "http://www.w3.org/2000/xmlns/") return true;
                if (name.startsWith("on") || name === "src" || name === "base") return false;
                if (name === "href") return localId.test(attribute.value);
                if (name === "style" || /url\s*\(/i.test(attribute.value)) return safeSvgCss(attribute.value);
                return true;
            }));
    }

    function validArtwork(artwork, limit) {
        if (typeof artwork !== "string" || artwork.length > Math.ceil(limit / 3) * 4 + 32) return false;
        const match = artwork.match(imageData);
        if (!match || match[2].length % 4) return false;
        const [, type, payload] = match;
        const bytes = payload.length * 3 / 4 - (payload.endsWith("==") ? 2 : Number(payload.endsWith("=")));
        if (bytes > limit) return false;
        if (type !== "svg+xml") return true;
        try {
            const raw = Uint8Array.from(atob(payload), byte => byte.charCodeAt(0));
            return safeSvg(new TextDecoder("utf-8", {fatal: true}).decode(raw));
        } catch {
            return false;
        }
    }

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

    // Also serialized into the editor, which validates incoming host snapshots.
    function readPrograms(memory) {
        if (memory == null) return null;
        const invalid = () => { throw new Error("Invalid Cell program memory snapshot."); };
        const slotNumber = (value, max) => Number.isInteger(value) && value >= 0 && value <= max;
        const encoder = new TextEncoder();
        const nameIsValid = name => typeof name === "string" && name.trim() &&
            name.length <= 80 && encoder.encode(name).length <= 80 && !/[\x00-\x1f\x7f]/.test(name);
        if (memory.type !== "program-memory" || !Array.isArray(memory.banks) || memory.banks.length > 64) invalid();
        const bankNumbers = new Set();
        let count = 0;
        const banks = memory.banks.map(bank => {
            if (!slotNumber(bank?.number, 16383) || !nameIsValid(bank.name) || bankNumbers.has(bank.number) ||
                !Array.isArray(bank.programs) || bank.programs.length > 128 || (count += bank.programs.length) > 512)
                invalid();
            bankNumbers.add(bank.number);
            const numbers = new Set();
            const programs = bank.programs.map(program => {
                if (!slotNumber(program?.number, 127) || !nameIsValid(program.name) || numbers.has(program.number)) invalid();
                numbers.add(program.number);
                return {number: program.number, name: program.name};
            });
            return {number: bank.number, name: bank.name, programs};
        });
        const current = memory.active;
        if (current != null && !banks.find(bank => bank.number === current.bank)?.programs
            .some(program => program.number === current.program)) invalid();
        const active = current == null ? null : {bank: current.bank, program: current.program};
        return {type: "program-memory", banks, active};
    }

    function readTheme(input) {
        const theme = {...defaultTheme};
        for (const key of Object.keys(theme)) {
            if (input?.[key] === undefined) continue;
            if (typeof input[key] !== "string" || !color.test(input[key])) throw new Error("The layout has an invalid theme color.");
            theme[key] = input[key].toLowerCase();
        }
        if (input?.backgroundArtwork !== undefined) {
            if (input.backgroundArtwork !== "" && !validArtwork(input.backgroundArtwork, maxPanelAssetBytes))
                throw new Error("The layout has invalid background artwork.");
            if (input.backgroundArtwork) theme.backgroundArtwork = input.backgroundArtwork;
        }
        if (input?.backgroundFit !== undefined) {
            if (!imageFits.includes(input.backgroundFit)) throw new Error("The layout has an invalid background image fit.");
            theme.backgroundFit = input.backgroundFit;
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
        return {controls, theme: readTheme(value.theme), programs: legacy ? undefined : readPrograms(value.programs),
            catalog: legacy ? undefined : value.catalog === null || value.catalog === undefined
                ? null : readCatalog({type: "parameters", parameters: value.catalog})};
    }

    function readControl(control, index, hasPhone) {
        const {kind, label, key = "", x, y, width = kinds[kind]?.width, height = 1,
            accent = "", artwork = "", skin = "modern", phone, fit = "contain",
            direction = "horizontal", thickness = 2, displayCodes, segmented} = control ?? {};
        const normalizedLabel = readLabel(label);
        if (typeof kind !== "string" || !Object.hasOwn(kinds, kind) || !normalizedLabel ||
            (hasBinding(kind) && !validText(key)) || typeof skin !== "string" || !Object.hasOwn(skins, skin) ||
            !validBox({x, y, width, height}, wideGrid) ||
            typeof accent !== "string" || (accent && !color.test(accent)) ||
            (artwork !== "" && (!hasArtwork(kind) || !validArtwork(artwork, kind === "image" ? maxPanelAssetBytes : maxAssetBytes))) ||
            (kind === "image" && !imageFits.includes(fit)) ||
            (segmented !== undefined && (kind !== "stepper" || typeof segmented !== "boolean")) ||
            (displayCodes !== undefined && !hasDisplayCodes(kind)) ||
            (kind === "line" && (!["horizontal", "vertical"].includes(direction) ||
                !Number.isInteger(thickness) || thickness < 1 || thickness > 16)))
            throw new Error(`Invalid settings for control ${index + 1}.`);
        const result = {kind, label: normalizedLabel, key: hasBinding(kind) ? key : "", x, y, width, height, accent, artwork, skin};
        Object.assign(result, buttonSettings(kind, control));
        Object.assign(result, ruleSettings(control));
        if (kind === "image") result.fit = fit;
        if (kind === "line") Object.assign(result, {direction, thickness});
        if (displayCodes !== undefined) result.displayCodes = readDisplayCodes(displayCodes);
        const typography = typographySettings(control.typography);
        if (typography) result.typography = typography;
        if (segmented) result.segmented = true;
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

    function serialize(controls, theme, catalog, programs) {
        return JSON.stringify({schema, controls, theme, catalog, programs: programs ?? undefined}, null, 2) + "\n";
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
        const slots = [];
        for (const {control, index} of ordered) {
            if (placements[index]) continue;
            const slot = control.visibleWhen && slots.find(({variants}) => variants.every(other =>
                ["x", "y", "width", "height"].every(key => control[key] === other[key]) &&
                variantsDisjoint(control.visibleWhen, other.visibleWhen)));
            if (slot) {
                placements[index] = {...slot.box};
                slot.variants.push(control);
                continue;
            }
            const box = {x: 0, y: nextRow, ...phoneSize(control.kind)};
            // Automatic controls span the full width. Skip pinned boxes before
            // continuing the wide layout's reading order below this control.
            for (let blocker; (blocker = pinned.find(other => overlaps(box, other)));)
                box.y = blocker.y + blocker.height;
            placements[index] = box;
            if (control.visibleWhen) slots.push({box, variants: [control]});
            nextRow = box.y + box.height;
        }
        return placements;
    }

    function parameterKeys(controls) {
        return [...new Set([
            ...controls.filter(control => hasParameter(control.kind)).map(control => control.key),
            ...controls.flatMap(ruleKeys)
        ])];
    }

    function meterKeys(controls) {
        return [...new Set(controls.filter(control => control.kind === "meter").map(control => control.key))];
    }

    function actionBindings(controls) {
        const bindings = new Map();
        const add = (key, mode = "invoke") => {
            if (key) bindings.set(key + "/" + mode, {key, mode});
        };
        for (const control of controls) {
            if (control.kind === "action") add(control.action, control.trigger === "momentary" ? "momentary" : "invoke");
            if (control.kind === "action-pair") {
                add(control.minusAction);
                add(control.plusAction);
            }
            if (control.kind === "stepper" || control.kind === "action-pair") add(control.chordAction);
        }
        return [...bindings.values()];
    }

    function twoState(parameter) {
        const {minimum, maximum, step, choices = []} = parameter;
        const tolerance = Math.max(1e-9, step * 1e-6);
        return maximum > minimum && step > 0 && Math.abs(minimum + step - maximum) <= tolerance &&
            (!choices.length || choices.length === 2);
    }

    // Match the parameter bank's snap-to-step and clamp rule.
    function stepCount(parameter) {
        if (!parameter || !Number.isFinite(parameter.step) || parameter.step <= 0) return 0;
        const count = Math.round((parameter.maximum - parameter.minimum) / parameter.step);
        return Number.isSafeInteger(count) && count > 0 ? count : 0;
    }

    function bindingIssues(controls, catalog, programs) {
        const keys = new Map((catalog ?? []).map(parameter => [parameter.key, parameter]));
        return controls.flatMap(control => {
            const unknownSource = catalog && ruleKeys(control).find(key => !keys.has(key));
            if (unknownSource) return [control.label + ": unknown rule or label source " + unknownSource];
            if (isProgramControl(control.kind))
                return programs?.banks.some(bank => bank.programs.length) ? [] : [`${control.label}: missing program memory snapshot`];
            if (!catalog || !hasParameter(control.kind)) return [];
            const parameter = keys.get(control.key);
            if (!parameter) return [`${control.label}: unknown key ${control.key}`];
            if (control.kind === "choice" && !parameter.choices.length)
                return [`${control.label}: ${control.key} has no choices`];
            if (control.kind === "toggle" && !twoState(parameter))
                return [`${control.label}: ${control.key} needs two discrete values`];
            if (control.kind === "momentary" && parameter.minimum >= parameter.maximum)
                return [`${control.label}: ${control.key} needs different press and release values`];
            if (control.kind === "stepper" && !stepCount(parameter))
                return [`${control.label}: ${control.key} needs a stepped parameter`];
            const outside = control.displayCodes?.find(({value}) => value < parameter.minimum || value > parameter.maximum);
            if (outside) return [`${control.label}: display code value ${outside.value} is outside ${control.key}'s range`];
            return [];
        });
    }

    // Three-digit byte escapes make the header independent of source-file encoding.
    function cppBindings(controls) {
        const literal = key => {
            const bytes = new TextEncoder().encode(key);
            return '"' + [...bytes].map(byte => byte >= 32 && byte <= 126 && byte !== 34 && byte !== 92 && byte !== 63
                ? String.fromCharCode(byte) : "\\" + byte.toString(8).padStart(3, "0")).join("") + '"';
        };
        const entries = controls.filter(control => hasParameter(control.kind));
        const keys = parameterKeys(controls);
        const edited = new Set(entries.map(control => control.key));
        const readOnly = keys.filter(key => !edited.has(key));
        const actions = actionBindings(controls);
        return `// Generated by Cell UI Designer.
#pragma once
#include <array>
#include <string_view>
#include <cell/web_parameter_bridge.hpp>

${actions.length ? "#define CELL_DESIGNER_HAS_ACTIONS 1\n\n" : ""}${readOnly.length ? "#define CELL_DESIGNER_HAS_RULE_SOURCES 1\n\n" : ""}namespace cell::designer {
enum class ControlKind { slider, knob, fader, choice, toggle, readout, stepper, led, momentary };
struct Binding {
    ControlKind kind;
    std::string_view key;
    cell::ParamId parameter;
};
inline constexpr std::array<Binding, ${entries.length}> controls{{${entries.map(({kind, key}) =>
    `\n    {ControlKind::${kind}, ${literal(key)}, cell::stableId(${literal(key)})},`).join("")}
}};
inline constexpr std::array<cell::ParamId, ${keys.length}> parameters{{${keys.map(key =>
    `\n    cell::stableId(${literal(key)}),`).join("")}
}};
inline constexpr std::array<std::string_view, ${meterKeys(controls).length}> meters{{${meterKeys(controls).map(key => `\n    ${literal(key)},`).join("")}
}};
inline constexpr std::array<cell::ParamId, ${readOnly.length}> readOnlyParameters{{${readOnly.map(key =>
    `\n    cell::stableId(${literal(key)}),`).join("")}
}};
inline constexpr std::array<cell::web::ActionBinding, ${actions.length}> actions{{${actions.map(({key, mode}) =>
    `\n    {${literal(key)}, cell::web::ActionMode::${mode}},`).join("")}
}};
inline cell::web::ParameterBridge bind(cell::Engine& engine) {
    return {engine, parameters, true, actions, readOnlyParameters};
}
} // namespace cell::designer
`;
    }

    return {wideGrid, phoneGrid, kinds, skins, defaultTheme, showcase, maxControls, maxAssetBytes, maxPanelAssetBytes,
        maxProjectBytes, imageType, validArtwork, isPanelArt, isProgramControl, clamp, validText, hasArtwork, hasParameter,
        hasDisplayCodes, ledGlyphs, parseDisplayCodes, displayCode, numericLedCode,
        buttonDefaults, buttonSettings, actionBindings,
        readLabel, typographyFamilies, typographySettings, silkscreen,
        ruleSettings, ruleKeys, conditionRules, compareValue, variantsDisjoint, parseLabelMap,
        hasBinding, readProject, readCatalog, readPrograms, serialize, overlaps,
        writeClipboard, readClipboard,
        constrain, firstOpenPlace, moveBoxes, alignBoxes, distributeBoxes, findGroupPlacement,
        readingOrder, phoneSize, phonePlacements, bindingIssues, parameterKeys, meterKeys, twoState, stepCount, cppBindings};
})();
