"use strict";

const CellDesignerModel = (() => {
    const columns = 12;
    const rows = 8;
    const maxControls = 256;
    const maxAssetBytes = 256 * 1024;
    const maxPanelAssetBytes = 1024 * 1024;
    const maxProjectBytes = 2 * 1024 * 1024;
    const maxDisplayCodes = 128;
    const maxDetentLabels = 32;
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
        indicator: {width: 3, height: 1, title: "Indicator"},
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
    const defaultLayers = {rectangle: 1, line: 2, image: 3};
    const layerPriority = control => control.layer ?? defaultLayers[control.kind] ?? 4;
    const layerOrder = controls => [...controls].sort((a, b) => layerPriority(a) - layerPriority(b));
    const isProgramControl = kind => ["bank", "program", "patch-name", "program-grid"].includes(kind);
    const hasDisplayCodes = kind => ["led", "stepper", "readout", "choice"].includes(kind);
    const hasDisplayArtwork = kind => ["led", "readout", "stepper", "meter"].includes(kind);
    const hasCapArtwork = kind => kind === "knob" || kind === "fader";
    const hasSwitchArtwork = kind => ["toggle", "momentary", "action"].includes(kind);
    const validArtworkMode = (kind, mode) => mode === "cap" ? hasCapArtwork(kind) : mode === "pointer-cap" && kind === "knob";
    const hasArtwork = kind => hasCapArtwork(kind) || kind === "image" || hasDisplayArtwork(kind) || hasSwitchArtwork(kind) || kind === "indicator";
    const hasBinding = kind => !["text", "action", "action-pair"].includes(kind) && !isPanelArt(kind) && !isProgramControl(kind);
    const hasParameter = kind => hasBinding(kind) && kind !== "meter";
    const isMeterBinding = control => control.kind === "meter" || (control.kind === "indicator" && control.indicatorSource === "meter");
    const hasParameterBinding = control => hasParameter(control.kind) && !isMeterBinding(control) &&
        (control.kind !== "led" || Boolean(control.key));
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
    // Standard glyphs use segments a–g and the decimal point (p).
    // Extended glyphs use strokes [x1, y1, x2, y2] in percent of a cell.
    const ledGlyphs = Object.freeze({
        " ": "", "-": "g", "?": "abegp",
        0: "abcdef", 1: "bc", 2: "abdeg", 3: "abcdg", 4: "bcfg", 5: "acdfg",
        6: "acdefg", 7: "abc", 8: "abcdefg", 9: "abcdfg",
        A: "abcefg", a: "abcdeg", B: "cdefg", b: "cdefg", C: "adef", c: "deg",
        D: "bcdeg", d: "bcdeg", E: "adefg", e: "abdefg", F: "aefg", f: "aefg",
        G: "acdef", H: "bcefg", h: "cefg", I: "bc", i: "c", J: "bcde",
        L: "def", l: "ef", n: "ceg", O: "abcdef", o: "cdeg", P: "abefg",
        q: "abcfg", r: "eg", S: "acdfg", t: "defg", U: "bcdef", u: "cde", y: "bcdfg",
        K: [[12, 8, 12, 92], [12, 50, 75, 8], [12, 50, 75, 92]],
        M: [[12, 8, 12, 92], [12, 8, 44, 51], [44, 51, 76, 8], [76, 8, 76, 92]],
        N: [[12, 8, 12, 92], [12, 8, 76, 92], [76, 8, 76, 92]],
        R: [[12, 8, 12, 92], [12, 8, 66, 8], [66, 8, 76, 18], [76, 18, 76, 37],
            [76, 37, 66, 47], [66, 47, 12, 47], [41, 47, 77, 92]],
        V: [[10, 8, 44, 92], [44, 92, 78, 8]],
        W: [[9, 8, 23, 92], [23, 92, 44, 51], [44, 51, 65, 92], [65, 92, 79, 8]],
        X: [[11, 8, 77, 92], [77, 8, 11, 92]],
        Y: [[10, 8, 44, 49], [78, 8, 44, 49], [44, 49, 44, 92]],
        Z: [[11, 8, 77, 8], [77, 8, 11, 92], [11, 92, 77, 92]],
        "+": [[44, 27, 44, 73], [11, 50, 77, 50]],
        "#": [[35, 13, 23, 87], [65, 13, 53, 87], [12, 37, 79, 37], [9, 63, 76, 63]]
    });

    function validLedText(text) {
        return typeof text === "string" && text.trim() && text.length <= 2 &&
            [...text].every(character => Object.hasOwn(ledGlyphs, character));
    }

    function ledSettings({kind, key, segmented, ledText, hideHeading}) {
        const hasLed = kind === "led" || (kind === "stepper" && segmented);
        if (ledText !== undefined || hideHeading !== undefined) {
            if (!hasLed) throw new Error("LED settings require an LED display or segmented stepper.");
            if (hideHeading !== undefined && typeof hideHeading !== "boolean")
                throw new Error("Invalid LED heading setting.");
        }
        if ((ledText !== undefined || (kind === "led" && !key)) && !validLedText(ledText))
            throw new Error("Enter one or two supported LED characters.");
        return {
            ...(ledText === undefined ? {} : {ledText}),
            ...(hideHeading ? {hideHeading: true} : {})
        };
    }

    function readDisplayCodes(entries) {
        if (!Array.isArray(entries) || entries.length > maxDisplayCodes)
            throw new Error(`Use up to ${maxDisplayCodes} display codes.`);
        const values = new Set();
        return entries.map(entry => {
            const {value, code} = entry ?? {};
            if (!Number.isFinite(value) || !validLedText(code) || code.includes(" "))
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

    function readDetentLabels(labels) {
        if (labels === "choices") return labels;
        if (!Array.isArray(labels) || labels.length > maxDetentLabels || !labels.some(label => typeof label === "string" && label.trim()) ||
            labels.some(label => typeof label !== "string" || label.length > 32 || /[\x00-\x1f\x7f]/.test(label)))
            throw new Error(`Enter up to ${maxDetentLabels} detent labels of up to 32 characters each.`);
        return labels.map(label => label.trim());
    }

    function readRotation(angle) {
        if (!Number.isFinite(angle)) throw new Error("Enter a finite rotation in degrees.");
        const degrees = angle % 360;
        return degrees >= 180 ? degrees - 360 : degrees < -180 ? degrees + 360 : degrees;
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

    const themePackSchema = "cell.ui-theme-pack.v1";
    const presentationSchema = "cell.ui-presentation.v1";
    const assetIdPattern = /^[a-z][a-z0-9_-]{0,63}$/;
    const appearanceKinds = ["knob", "slider", "fader", "button", "meter", "panel", "dropdown", "tab", "icon-button"];
    const appearanceLayers = ["background", "track", "face", "pointer", "highlight", "indicator", "foreground"];
    const appearanceStates = ["enabled", "hover", "pressed", "selected", "focus", "disabled",
        "bypassed", "automated", "modulated", "invalid", "warning"];
    const appearanceRoles = [...Object.keys(defaultTheme), "edge", "muted", "metal", "highlight", "shadow"];
    const appearanceColors = {...defaultTheme, edge: "#46565d", muted: "#a8b5bb", metal: "#35444a",
        highlight: "#9aabb4", shadow: "#101417"};
    const validId = value => typeof value === "string" && assetIdPattern.test(value);
    const appearanceError = cause => { throw new Error("Presentation: " + cause + "."); };
    const objectEntries = (input, max) => {
        if (input === undefined) return [];
        if (input === null || typeof input !== "object" || Array.isArray(input) || Object.keys(input).length > max)
            appearanceError("invalid or oversized table");
        return Object.entries(input);
    };
    const table = (input, max, read) => Object.fromEntries(objectEntries(input, max).map(([id, value]) => {
        if (!validId(id)) appearanceError("invalid ID");
        return [id, read(value, id)];
    }));
    const bounded = (value, min, max, fallback) => {
        if (value === undefined) return fallback;
        if (!Number.isFinite(value) || value < min || value > max) appearanceError("number outside its supported range");
        return value;
    };
    const displayName = value => {
        if (!validText(value)) appearanceError("expected a descriptive name");
        return value.trim();
    };
    const dictionaryHas = (dictionary, key) => typeof key === "string" && Object.hasOwn(dictionary, key);
    const checkReference = (dictionary, id) => {
        if (!dictionaryHas(dictionary, id)) appearanceError("unknown reference " + String(id));
        return id;
    };
    const colorToken = value => {
        if (typeof value !== "string" || (!/^#[0-9a-f]{6}([0-9a-f]{2})?$/i.test(value) &&
            !appearanceRoles.includes(value))) appearanceError("invalid color token");
        return value.toLowerCase();
    };

    function relativeAssetPath(path) {
        if (typeof path !== "string" || path.length > 240 ||
            !/^[\w.-]+(?:\/[\w.-]+)*$/.test(path) ||
            path.split("/").some(part => part === "." || part === ".."))
            appearanceError("unsafe relative asset path");
        if (!/\.(svg|png|webp|jpe?g)$/i.test(path)) appearanceError("unsupported relative asset file type");
        return path;
    }

    function packAssetPaths(text) {
        if (new TextEncoder().encode(text).length > maxProjectBytes) appearanceError("theme pack exceeds 2 MiB");
        const pack = JSON.parse(text);
        if (pack?.schema !== themePackSchema) appearanceError("unsupported theme pack schema");
        return [...new Set(objectEntries(pack.assets, 512).flatMap(([, asset]) =>
            asset?.path === undefined ? [] : [relativeAssetPath(asset.path)]))];
    }

    function readManifest(value, assets, themes, icons) {
        if (value === undefined) return undefined;
        objectEntries(value, 8);
        if (value.version !== 1) appearanceError("unsupported pack manifest version");
        const refs = (ids, dictionary) => {
            if (ids === undefined) return undefined;
            if (!Array.isArray(ids) || ids.length > 512 || new Set(ids).size !== ids.length)
                appearanceError("invalid pack manifest dependency list");
            return ids.map(id => checkReference(dictionary, id));
        };
        let template;
        if (value.template !== undefined) {
            objectEntries(value.template, 2);
            if (!dictionaryHas(themes, value.template.theme) || !validId(value.template.style))
                appearanceError("invalid pack manifest template");
            template = {theme: value.template.theme, style: value.template.style};
        }
        return {version: 1, name: displayName(value.name), assets: refs(value.assets, assets),
            themes: refs(value.themes, themes), icons: refs(value.icons, icons),
            previews: refs(value.previews, assets), fonts: refs(value.fonts, typographyFamilies), template};
    }

    function packageRasterPixels(data) {
        const match = data.match(imageData);
        const type = match?.[1];
        if (type !== "png" && type !== "webp") return 0;
        let bytes;
        try { bytes = Uint8Array.from(atob(match[2].slice(0, 48)), character => character.charCodeAt(0)); }
        catch { appearanceError("invalid image encoding"); }
        const bytesAt = (offset, length) => String.fromCharCode(...bytes.subarray(offset, offset + length));
        const little = (offset, length) => {
            let value = 0;
            for (let i = length - 1; i >= 0; --i) value = 256 * value + bytes[offset + i];
            return value;
        };
        let width, height;
        if (type === "png" && bytes.length >= 24 && bytes[0] === 137 && bytesAt(1, 3) === "PNG" &&
            bytesAt(12, 4) === "IHDR") {
            const view = new DataView(bytes.buffer);
            width = view.getUint32(16);
            height = view.getUint32(20);
        } else if (type === "webp" && bytesAt(0, 4) === "RIFF" && bytesAt(8, 4) === "WEBP") {
            const header = bytesAt(12, 4);
            if (header === "VP8X" && !(bytes[20] & 2)) {
                width = 1 + little(24, 3);
                height = 1 + little(27, 3);
            } else if (header === "VP8L" && bytes[20] === 0x2f) {
                const packed = little(21, 4);
                width = 1 + (packed & 0x3fff);
                height = 1 + ((packed >>> 14) & 0x3fff);
            } else if (header === "VP8 " && bytes[23] === 0x9d && bytes[24] === 1 && bytes[25] === 0x2a) {
                width = little(26, 2) & 0x3fff;
                height = little(28, 2) & 0x3fff;
            }
        }
        if (![width, height].every(value => Number.isInteger(value) && value > 0 && value <= 4096))
            appearanceError("invalid, animated, or oversized package raster");
        return width * height;
    }

    function readAssets(input, files) {
        return table(input, 512, asset => {
            let data = asset?.data;
            if (asset?.path !== undefined) {
                const path = relativeAssetPath(asset.path);
                if (data !== undefined) appearanceError("asset cannot contain both data and a path");
                data = files?.get(path);
                if (!data) appearanceError("missing relative asset " + path);
            }
            if (!validArtwork(data, maxPanelAssetBytes)) appearanceError("invalid embedded asset");
            const name = displayName(asset.name);
            return {name, data, license: asset.license === undefined ? undefined : displayName(asset.license),
                description: asset.description === undefined ? undefined : displayName(asset.description)};
        });
    }

    const choices = (value, allowed, fallback) => {
        if (value === undefined) return fallback;
        if (!allowed.includes(value)) appearanceError("unsupported option " + String(value));
        return value;
    };
    function numbers(value, count, min, max, fallback) {
        if (value === undefined) return fallback;
        if (!Array.isArray(value) || value.length !== count) appearanceError("invalid material dimensions");
        return value.map(item => bounded(item, min, max));
    }
    function readGradient(value) {
        if (value === undefined) return undefined;
        objectEntries(value, 4);
        const kind = choices(value.kind, ["solid", "linear", "radial", "conic"]);
        if (!kind || !Array.isArray(value.stops) || value.stops.length < (kind === "solid" ? 1 : 2) ||
            value.stops.length > (kind === "solid" ? 1 : 16))
            appearanceError("gradient stop count outside its supported range");
        let last = -1;
        const stops = value.stops.map(stop => {
            objectEntries(stop, 3);
            const at = bounded(stop.at, 0, 1, kind === "solid" ? 0 : undefined);
            if (at === undefined || at < last) appearanceError("gradient stops must be in position order");
            last = at;
            return {at, color: colorToken(stop.color), opacity: bounded(stop.opacity, 0, 1, 1)};
        });
        return {kind, angle: bounded(value.angle, -360, 360, 180),
            center: numbers(value.center, 2, 0, 1, [.5, .5]), stops};
    }

    function readSurfaceFinish(value, assets) {
        const result = {};
        if (value.gradient !== undefined) result.gradient = readGradient(value.gradient);
        if (value.corners !== undefined) result.corners = numbers(value.corners, 4, 0, 48);
        if (value.borders !== undefined) {
            if (!Array.isArray(value.borders) || value.borders.length > 4) appearanceError("up to four borders are supported");
            result.borders = value.borders.map(border => {
                objectEntries(border, 2);
                return {color: colorToken(border.color), width: bounded(border.width, 0, 16, 1)};
            });
        }
        if (value.bevel !== undefined) {
            const bevel = value.bevel;
            objectEntries(bevel, 5);
            result.bevel = {kind: choices(bevel.kind, ["raised", "recessed"], "raised"),
                width: bounded(bevel.width, 0, 12, 2), angle: bounded(bevel.angle, -360, 360, 135),
                light: colorToken(bevel.light ?? "#ffffff50"), dark: colorToken(bevel.dark ?? "#00000090")};
        }
        if (value.texture !== undefined) {
            const texture = value.texture;
            objectEntries(texture, 6);
            result.texture = {asset: checkReference(assets, texture.asset),
                fit: choices(texture.fit, ["stretch", "contain", "cover", "tile"], "contain"),
                scale: bounded(texture.scale, .125, 8, 1), offset: numbers(texture.offset, 2, -512, 512, [0, 0]),
                opacity: bounded(texture.opacity, 0, 1, 1), blend: choices(texture.blend, ["normal", "multiply", "screen"], "normal")};
        }
        if (value.lighting !== undefined) {
            const lighting = value.lighting;
            objectEntries(lighting, 3);
            result.lighting = {ambient: bounded(lighting.ambient, -1, 1, 0),
                direction: bounded(lighting.direction, -360, 360, 135),
                intensity: bounded(lighting.intensity, 0, 1, 0)};
        }
        if (value.finish !== undefined) result.finish = choices(value.finish, ["matte", "satin", "polished"]);
        if (value.slices !== undefined) {
            const slices = value.slices;
            objectEntries(slices, 5);
            if (!value.asset || slices.insets === undefined) appearanceError("slices require artwork and four insets");
            if (slices.transparentCenter !== undefined && typeof slices.transparentCenter !== "boolean")
                appearanceError("invalid transparent center flag");
            result.slices = {insets: numbers(slices.insets, 4, 0, 128),
                minimum: numbers(slices.minimum, 2, 0, 4096, [0, 0]),
                edges: choices(slices.edges, ["stretch", "tile"], "stretch"),
                center: choices(slices.center, ["stretch", "tile"], "stretch"),
                transparentCenter: Boolean(slices.transparentCenter)};
        }
        return result;
    }

    function vectorPath(value) {
        if (typeof value !== "string" || value.length > 1024 || !/^[MLHVCSAZ][MLHVCSAZeE0-9+., \-]*$/.test(value))
            appearanceError("invalid vector path");
        return value;
    }

    function readIcons(input) {
        return table(input, 256, icon => {
            objectEntries(icon, 9);
            const viewBox = numbers(icon.viewBox, 4, -4096, 4096, [0, 0, 24, 24]);
            if (viewBox[2] <= 0 || viewBox[3] <= 0) appearanceError("invalid icon view box");
            const keywords = icon.keywords ?? [];
            if (!Array.isArray(keywords) || keywords.length > 12) appearanceError("invalid icon keywords");
            const states = Object.fromEntries(objectEntries(icon.states, appearanceStates.length).map(([state, variant]) => {
                if (!appearanceStates.includes(state)) appearanceError("unknown icon state");
                objectEntries(variant, 2);
                return [state, {path: variant.path === undefined ? undefined : vectorPath(variant.path),
                    color: variant.color === undefined ? undefined : colorToken(variant.color)}];
            }));
            return {name: displayName(icon.name),
                category: choices(icon.category, ["module", "toolbar", "routing", "handle", "other"], "other"),
                keywords: keywords.map(displayName), viewBox, offset: numbers(icon.offset, 2, -256, 256, [0, 0]),
                path: vectorPath(icon.path), stroke: bounded(icon.stroke, .1, 6, 1.8),
                color: colorToken(icon.color ?? "text"), states};
        });
    }

    function iconDataUrl(icon, color) {
        const {path, stroke, viewBox = [0, 0, 24, 24], offset = [0, 0]} = icon;
        const markup = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="${viewBox.join(" ")}"><path d="${path}" fill="none" stroke="${color}" transform="translate(${offset.join(" ")})" stroke-width="${stroke}" stroke-linecap="round" stroke-linejoin="round"/></svg>`;
        return "data:image/svg+xml," + encodeURIComponent(markup);
    }

    function readMaterials(input, assets, icons) {
        return table(input, 64, value => {
            if (!value || typeof value !== "object") appearanceError("invalid material");
            if (!["rounded", "ellipse"].includes(value.shape ?? "rounded")) appearanceError("invalid corner geometry");
            const effects = {};
            for (const kind of ["shadow", "innerShadow", "glow"]) {
                const effect = value[kind];
                if (effect === undefined) continue;
                if (!effect || typeof effect !== "object") appearanceError("invalid effect");
                effects[kind] = {color: colorToken(effect.color), sigma: bounded(effect.sigma, 0, 18, 3),
                    x: bounded(effect.x, -16, 16, 0), y: bounded(effect.y, -16, 16, 0)};
            }
            const vector = value.vector;
            if (vector !== undefined && (!vector || typeof vector.path !== "string" ||
                vector.path.length > 1024 || !/^[MLHVCSAZ][MLHVCSAZeE0-9+., \-]*$/.test(vector.path)))
                appearanceError("invalid 24-unit vector path");
            return {fill: colorToken(value.fill ?? "#00000000"), end: colorToken(value.end ?? value.fill ?? "#00000000"),
                edge: colorToken(value.edge ?? "#00000000"), edgeWidth: bounded(value.edgeWidth, 0, 6, 0),
                highlight: colorToken(value.highlight ?? "#00000000"), radius: bounded(value.radius, 0, 48, 8),
                opacity: bounded(value.opacity, 0, 1, 1), shape: value.shape ?? "rounded",
                asset: value.asset === undefined ? undefined : checkReference(assets, value.asset),
                mask: value.mask === undefined ? undefined : checkReference(assets, value.mask),
                icon: value.icon === undefined ? undefined : checkReference(icons, value.icon),
                vector: vector ? {path: vector.path, stroke: bounded(vector.stroke, .1, 6, 1.8)} : undefined,
                ...effects, ...readSurfaceFinish(value, assets)};
        });
    }

    function readVisualLayers(input, materials) {
        return Object.fromEntries(objectEntries(input, appearanceLayers.length).map(([layer, material]) => {
            if (!appearanceLayers.includes(layer)) appearanceError("unknown visual layer");
            return [layer, checkReference(materials, material)];
        }));
    }

    function readStack(input, materials) {
        if (input === undefined) return undefined;
        if (!Array.isArray(input) || input.length > 24) appearanceError("visual stack may contain up to 24 layers");
        const ids = new Set();
        return input.map(layer => {
            objectEntries(layer, 13);
            if (!validId(layer.id) || ids.has(layer.id)) appearanceError("invalid or duplicate visual layer ID");
            ids.add(layer.id);
            const material = materials ? checkReference(materials, layer.material) : layer.material;
            if (!validId(material)) appearanceError("visual layer requires a material ID");
            const frame = layer.frame;
            if (frame !== undefined) {
                objectEntries(frame, 2);
                if (!Number.isInteger(frame.count) || frame.count < 2 || frame.count > 128 || layer.text ||
                    (materials && !materials[material].asset))
                    appearanceError("filmstrip needs artwork and 2–128 frames");
            }
            const motion = layer.motion;
            if (motion !== undefined) objectEntries(motion, 3);
            const visible = numbers(layer.visible, 2, 0, 1, [0, 1]);
            if (visible[0] > visible[1]) appearanceError("invalid layer visibility range");
            const state = choices(layer.state, appearanceStates);
            if (layer.clip !== undefined && typeof layer.clip !== "boolean") appearanceError("invalid layer clipping flag");
            const text = layer.text === undefined ? undefined : readLabel(layer.text);
            if (text === null) appearanceError("invalid layer text");
            const rect = numbers(layer.rect, 4, -2, 3, [0, 0, 1, 1]);
            if (rect[2] <= 0 || rect[3] <= 0) appearanceError("visual layer needs a positive size");
            return {id: layer.id, material, rect, text, state, visible, clip: Boolean(layer.clip),
                opacity: bounded(layer.opacity, 0, 1, 1),
                pivot: numbers(layer.pivot, 2, 0, 1, [.5, .5]),
                anchor: numbers(layer.anchor, 2, 0, 1, [0, 0]),
                transform: numbers(layer.transform, 5, -360, 360, [0, 0, 0, 1, 1]),
                motion: motion && {source: choices(motion.source, ["value", "indicator"], "value"),
                    target: choices(motion.target, ["rotate", "x", "y", "fill-x", "fill-y", "opacity"], "rotate"),
                    range: numbers(motion.range, 2, -360, 360, [0, 1])},
                frame: frame && {count: frame.count,
                    orientation: choices(frame.orientation, ["horizontal", "vertical"], "vertical")}};
        });
    }

    function readControlGeometry(value, kind) {
        if (value === undefined) return undefined;
        if (kind === "knob") {
            objectEntries(value, 7);
            const angles = numbers(value.angles, 2, -360, 360, [-135, 135]);
            if (angles[1] <= angles[0] || angles[1] - angles[0] > 360)
                appearanceError("rotary angles must describe a positive sweep up to 360 degrees");
            const ticks = bounded(value.ticks, 0, 64, 0);
            if (!Number.isInteger(ticks)) appearanceError("tick count must be an integer");
            return {angles, pivot: numbers(value.pivot, 2, 0, 1, [.5, .5]),
                cap: choices(value.cap, ["stationary", "rotating"], "stationary"),
                arc: choices(value.arc, ["none", "unipolar", "bipolar"], "none"),
                origin: bounded(value.origin, 0, 1, .5), ticks};
        }
        if (!["fader", "slider", "meter"].includes(kind))
            appearanceError("this component has no rotary or linear geometry");
        objectEntries(value, 4);
        const ticks = bounded(value.ticks, 0, 64, 0);
        if (!Number.isInteger(ticks)) appearanceError("tick count must be an integer");
        return {orientation: choices(value.orientation, ["horizontal", "vertical"], kind === "fader" ? "vertical" : "horizontal"),
            origin: bounded(value.origin, 0, 1, 0), unity: bounded(value.unity, 0, 1),
            ticks};
    }

    const traceRoles = ["spectrum", "frequency-response", "transfer", "gain-reduction", "waveform",
        "meter-track", "meter-fill", "peak-hold", "handle", "selected-band", "mid", "side", "sidechain"];

    function readAnalyticalStyles(styles) {
        return table(styles, 32, value => {
            objectEntries(value, 7);
            const traces = Object.fromEntries(objectEntries(value.traces, traceRoles.length).map(([id, paint]) => {
                if (!traceRoles.includes(id)) appearanceError("unknown analytical role " + id);
                objectEntries(paint, 3);
                return [id, {color: colorToken(paint.color), width: bounded(paint.width, .5, 8, 1.5),
                    mode: choices(paint.mode, ["line", "fill", "points"], "line")}];
            }));
            const axes = value.axes ?? {};
            objectEntries(axes, 3);
            const range = numbers(axes.range, 2, -1e9, 1e9, [0, 1]);
            if (range[1] <= range[0]) appearanceError("invalid analytical display range");
            const precision = bounded(axes.precision, 0, 6, 1);
            if (!Number.isInteger(precision)) appearanceError("axis precision must be an integer");
            const grid = numbers(value.grid, 3, 1, 16, [4, 4, 2]);
            if (!grid.every(Number.isInteger)) appearanceError("grid counts must be integers");
            return {background: colorToken(value.background ?? "background"),
                major: colorToken(value.major ?? "edge"), minor: colorToken(value.minor ?? "#939caa29"),
                labels: colorToken(value.labels ?? "muted"), grid, traces,
                axes: {range, precision, unit: axes.unit === undefined || axes.unit === "" ? "" : displayName(axes.unit)}};
        });
    }

    function readThemeDefinition(value, assets, sharedIcons) {
        const colors = {...appearanceColors};
        for (const [role, token] of objectEntries(value?.colors, appearanceRoles.length)) {
            if (!appearanceRoles.includes(role) || !/^#[0-9a-f]{6}$/i.test(token)) appearanceError("invalid semantic color");
            colors[role] = token.toLowerCase();
        }
        const typography = {};
        for (const [role, settings] of objectEntries(value?.typography, 2)) {
            if (!["label", "value"].includes(role)) appearanceError("unknown typography role");
            typography[role] = typographySettings(settings);
        }
        const icons = readIcons(mergeAppearance(sharedIcons, value.icons));
        const materials = readMaterials(value?.materials, assets, icons);
        const displays = readAnalyticalStyles(value.displays);
        const components = table(value?.components, 64, (style, id) => {
            if (!appearanceKinds.includes(style?.kind)) appearanceError("unknown component kind");
            const states = Object.fromEntries(objectEntries(style.states, appearanceStates.length).map(([state, layers]) => {
                if (!appearanceStates.includes(state)) appearanceError("unknown visual state");
                return [state, readVisualLayers(layers, materials)];
            }));
            return {name: displayName(style.name ?? id), kind: style.kind, layers: readVisualLayers(style.layers, materials), states,
                stack: readStack(style.stack, materials), geometry: readControlGeometry(style.geometry, style.kind),
                analysis: style.analysis === undefined ? undefined : checkReference(displays, style.analysis)};
        });
        const defaults = Object.fromEntries(objectEntries(value?.defaults, appearanceKinds.length).map(([kind, id]) => {
            if (!appearanceKinds.includes(kind) || components[checkReference(components, id)].kind !== kind)
                appearanceError("component default has the wrong kind");
            return [kind, id];
        }));
        let transition;
        if (value.transition !== undefined) {
            objectEntries(value.transition, 2);
            const easing = value.transition.easing ?? "ease-out";
            if (!["linear", "ease", "ease-out", "ease-in", "ease-in-out"].includes(easing))
                appearanceError("unsupported transition easing");
            transition = {duration: bounded(value.transition.duration, 0, 1500, 0), easing};
        }
        return {name: displayName(value?.name), colors, typography, materials, components, defaults, transition, icons, displays};
    }

    // Merge authored fragments only. Null drops an inherited property and
    // arrays replace as units, so ordered layers and stop positions stay intact.
    function mergeAppearance(parent, child) {
        const merged = new Map(Object.entries(parent ?? {}));
        for (const [key, value] of Object.entries(child ?? {})) {
            if (value === undefined) continue;
            if (value === null) merged.delete(key);
            else if (typeof value === "object" && !Array.isArray(value) &&
                merged.get(key) && typeof merged.get(key) === "object" && !Array.isArray(merged.get(key)))
                merged.set(key, mergeAppearance(merged.get(key), value));
            else merged.set(key, value);
        }
        return Object.fromEntries(merged);
    }

    const themeResolutions = new WeakMap();
    function readThemes(input, assets, icons) {
        const authored = table(input, 16, theme => {
            const name = displayName(theme?.name);
            if (theme.parent !== undefined && !validId(theme.parent)) appearanceError("invalid parent theme ID");
            return Object.fromEntries(Object.entries({name, parent: theme.parent, colors: theme.colors,
                typography: theme.typography, materials: theme.materials, components: theme.components,
                defaults: theme.defaults, transition: theme.transition, icons: theme.icons, displays: theme.displays}).filter(([, value]) => value !== undefined));
        });
        themeResolutions.set(authored, prepareThemes(authored, assets, icons));
        return authored;
    }

    function prepareThemes(themes, assets, icons) {
        const completed = new Map();
        const path = [];
        function visit(id) {
            if (completed.has(id)) return completed.get(id);
            if (path.includes(id)) appearanceError("theme inheritance cycle " + [...path, id].join(" → "));
            if (!dictionaryHas(themes, id)) appearanceError("missing parent theme " + id);
            path.push(id);
            const {parent, ...fields} = themes[id];
            const inherited = parent && visit(parent);
            const merged = mergeAppearance(inherited?.merged, fields);
            let definition;
            try {
                definition = readThemeDefinition(merged, assets, icons);
            } catch (error) {
                appearanceError(`theme ${id}: ${error.message}`);
            }
            path.pop();
            const result = {merged, definition,
                chain: [...(inherited?.chain ?? []), id]};
            completed.set(id, result);
            return result;
        }
        Object.keys(themes).forEach(visit);
        return completed;
    }

    function themeResolution(presentation, id = presentation?.activeTheme) {
        if (!id || !dictionaryHas(presentation.themes, id)) return undefined;
        const {themes, assets, icons} = presentation;
        let prepared = themeResolutions.get(themes);
        if (!prepared) {
            prepared = prepareThemes(themes, assets, icons);
            themeResolutions.set(themes, prepared);
        }
        return prepared.get(id);
    }

    function activeAppearanceTheme(presentation) {
        return themeResolution(presentation)?.definition;
    }

    function readLocalAppearance(local, kind) {
        if (local === undefined) return undefined;
        objectEntries(local, 7);
        if (local.detached !== undefined && typeof local.detached !== "boolean")
            appearanceError("invalid detached style flag");
        if (local.kind !== undefined && (!appearanceKinds.includes(local.kind) ||
            !matchesAppearance(kind, {kind: local.kind}))) appearanceError("incompatible local appearance kind");
        const layers = fields => Object.fromEntries(objectEntries(fields, appearanceLayers.length).map(([role, id]) => {
            if (!appearanceLayers.includes(role) || (id !== null && !validId(id))) appearanceError("invalid local visual layer");
            return [role, id];
        }));
        const result = {layers: layers(local.layers)};
        if (local.detached) result.detached = true;
        if (local.kind) result.kind = local.kind;
        if (local.stack !== undefined) result.stack = readStack(local.stack);
        if (local.geometry !== undefined) result.geometry = readControlGeometry(local.geometry, local.kind ?? kind);
        if (local.analysis !== undefined) {
            if (!validId(local.analysis)) appearanceError("invalid analytical display ID");
            result.analysis = local.analysis;
        }
        if (local.states !== undefined) {
            result.states = Object.fromEntries(objectEntries(local.states, appearanceStates.length).map(([state, value]) => {
                if (!appearanceStates.includes(state)) appearanceError("invalid local visual state");
                return [state, layers(value)];
            }));
        }
        return result;
    }

    function appearanceSource(control, presentation) {
        const resolution = themeResolution(presentation);
        if (!resolution) return null;
        const theme = resolution.definition;
        const kind = appearanceKind(control.kind);
        const standard = theme.defaults[kind];
        const id = dictionaryHas(theme.components, control.visualStyle) &&
            matchesAppearance(kind, theme.components[control.visualStyle]) ? control.visualStyle : undefined;
        const local = control.visualOverrides;
        const layers = {}, states = {}, sources = {}, diagnostics = [];
        let visualKind = kind, stack, geometry, analysis;
        function apply(target, paint, origin, track = false) {
            for (const [slot, reference] of Object.entries(paint ?? {})) {
                if (reference === null) {
                    delete target[slot];
                    if (track) delete sources[slot];
                } else if (!dictionaryHas(theme.materials, reference)) {
                    diagnostics.push(`Missing material ${reference} for ${slot} in ${theme.name}`);
                } else {
                    target[slot] = reference;
                    if (track) sources[slot] = origin;
                }
            }
        }
        function style(component, origin) {
            if (!component) return;
            visualKind = component.kind ?? visualKind;
            if (component.stack) stack = component.stack.filter(layer => {
                if (dictionaryHas(theme.materials, layer.material) && (!layer.frame || theme.materials[layer.material].asset))
                    return true;
                diagnostics.push(`Missing material or artwork ${layer.material} for layer ${layer.id} in ${theme.name}`);
                return false;
            });
            if (component.geometry) geometry = component.geometry;
            if (component.analysis) {
                if (dictionaryHas(theme.displays, component.analysis)) analysis = component.analysis;
                else diagnostics.push("Missing analytical display " + component.analysis);
            }
            apply(layers, component.layers, origin, true);
            for (const [state, paint] of Object.entries(component.states ?? {}))
                apply(states[state] ??= {}, paint, origin);
        }
        function themeStyle(componentId, label) {
            if (!componentId) return;
            const component = theme.components[componentId];
            style(component, label);
            for (const slot of Object.keys(component.layers)) {
                const origin = resolution.chain.findLast(id =>
                    Object.hasOwn(presentation.themes[id].components?.[componentId]?.layers ?? {}, slot));
                if (origin) sources[slot] = label + " in " + presentation.themes[origin].name;
            }
        }
        if (!local?.detached) {
            themeStyle(standard, "Theme default");
            if (id && id !== standard) themeStyle(id, "Style " + id);
            if (control.visualStyle && !id)
                diagnostics.push(`Style ${control.visualStyle} is unavailable in ${theme.name}`);
        }
        style(local, "Local override");
        if (!standard && !id && !local) return null;
        return {theme, kind: visualKind, layers, states, sources, diagnostics, stack, geometry, analysis};
    }

    function readPresentation(value) {
        if (value === undefined) return {schema: presentationSchema, assets: {}, themes: {}, icons: {}, activeTheme: ""};
        if (value?.schema !== presentationSchema) appearanceError("unsupported schema");
        const assets = readAssets(value.assets);
        const icons = readIcons(value.icons);
        const themes = readThemes(value.themes, assets, icons);
        const activeTheme = value.activeTheme ?? "";
        if (typeof activeTheme !== "string") appearanceError("invalid active theme ID");
        if (activeTheme) checkReference(themes, activeTheme);
        const previewAssets = value.previewAssets;
        if (previewAssets !== undefined && (!Array.isArray(previewAssets) || previewAssets.length > 16))
            appearanceError("invalid preview image list");
        return {schema: presentationSchema, assets, themes, icons, activeTheme,
            previewAssets: previewAssets?.map(id => checkReference(assets, id))};
    }

    function readThemePack(text, presentation = readPresentation(), files) {
        if (new TextEncoder().encode(text).length > maxProjectBytes) appearanceError("theme pack exceeds 2 MiB");
        const pack = JSON.parse(text);
        if (pack?.schema !== themePackSchema) appearanceError("unsupported theme pack schema");
        const assets = readAssets(pack.assets, files);
        let decodedPixels = 0;
        for (const data of new Set(Object.values(assets).map(asset => asset.data))) {
            decodedPixels += packageRasterPixels(data);
            if (decodedPixels > 16 * 1024 * 1024) appearanceError("package decoded raster budget exceeded");
        }
        const icons = readIcons(pack.icons);
        const themes = readThemes(pack.themes, assets, icons);
        if (!Object.keys(themes).length) appearanceError("theme pack is empty");
        const manifest = readManifest(pack.manifest, assets, themes, icons);
        if (manifest?.template && !themeResolutions.get(themes).get(manifest.template.theme).definition.components[manifest.template.style])
            appearanceError("missing manifest template style " + manifest.template.style);
        const next = readPresentation(presentation);
        const rename = new Map();
        for (const [id, asset] of Object.entries(assets)) {
            const existing = Object.keys(next.assets).find(key => next.assets[key].data === asset.data);
            const key = existing ?? availableId(next.assets, id);
            if (!existing) next.assets[key] = asset;
            rename.set(id, key);
        }
        const knownIcons = Object.assign(Object.create(null), next.icons, ...Object.values(next.themes).map(theme => theme.icons));
        const incomingIcons = new Set([...Object.keys(icons), ...Object.values(themes).flatMap(theme => Object.keys(theme.icons ?? {}))]);
        const iconIds = new Map();
        for (const id of incomingIcons) {
            const key = availableId(knownIcons, id);
            iconIds.set(id, key);
            knownIcons[key] = true;
            if (dictionaryHas(icons, id)) next.icons[key] = icons[id];
        }
        const themeIds = new Map();
        for (const id of Object.keys(themes)) {
            const key = availableId(next.themes, id);
            themeIds.set(id, key);
            next.themes[key] = null;
        }
        for (const [id, theme] of Object.entries(themes)) {
            const materials = Object.fromEntries(Object.entries(theme.materials ?? {}).map(([key, source]) => {
                if (!source) return [key, source];
                const material = {...source};
                for (const field of ["asset", "mask"])
                    if (material[field]) material[field] = rename.get(material[field]);
                if (material.texture?.asset)
                    material.texture = {...material.texture, asset: rename.get(material.texture.asset)};
                if (material.icon) material.icon = iconIds.get(material.icon);
                return [key, material];
            }));
            const key = themeIds.get(id);
            const themeIcons = theme.icons === undefined ? undefined :
                Object.fromEntries(Object.entries(theme.icons).map(([id, icon]) => [iconIds.get(id), icon]));
            next.themes[key] = {...theme, materials, icons: themeIcons, parent: theme.parent && themeIds.get(theme.parent)};
            if (!next.activeTheme) next.activeTheme = key;
        }
        if (manifest?.previews)
            next.previewAssets = [...new Set([...(next.previewAssets ?? []), ...manifest.previews.map(id => rename.get(id))])];
        return readPresentation(next);
    }

    function availableId(dictionary, proposal) {
        const base = proposal.slice(0, 56);
        if (!Object.hasOwn(dictionary, proposal)) return proposal;
        let counter = 2;
        while (Object.hasOwn(dictionary, base + "-" + counter)) ++counter;
        return base + "-" + counter;
    }

    function registerAsset(assets, data, name) {
        const previous = Object.keys(assets).find(key => assets[key].data === data);
        if (previous) return previous;
        name = name.replace(/[\x00-\x1f\x7f]+/g, " ").trim() || "Artwork";
        const slug = name.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "").slice(0, 48);
        const id = availableId(assets, slug && /^[a-z]/.test(slug) ? slug : "artwork");
        assets[id] = {name: name.slice(0, 80), data};
        return id;
    }

    function appearanceKind(kind) {
        if (["knob", "slider", "fader", "meter"].includes(kind)) return kind;
        if (["choice", "bank", "program"].includes(kind)) return "dropdown";
        if (["toggle", "momentary", "action", "action-pair", "stepper", "program-grid"].includes(kind)) return "button";
        return "panel";
    }

    function matchesAppearance(kind, style) {
        return kind === style.kind || (kind === "button" && ["tab", "icon-button"].includes(style.kind));
    }

    function resolveAppearance(control, presentation) {
        const style = appearanceSource(control, presentation);
        if (!style) return null;
        const {theme} = style;
        const colors = {...theme.colors, accent: control.accent || theme.colors.accent};
        const color = token => tokensHas(token) ? colors[token] : token;
        function resolve(layers, state) {
            return Object.fromEntries(Object.entries(layers ?? {}).map(([layer, id]) => {
                const material = {...theme.materials[id]};
                for (const field of ["fill", "end", "edge", "highlight"]) material[field] = color(material[field]);
                if (material.icon) {
                    const icon = theme.icons[material.icon], variant = icon.states[state];
                    material.vector = {path: variant?.path ?? icon.path, stroke: icon.stroke,
                        viewBox: icon.viewBox, offset: icon.offset};
                    material.fill = color(variant?.color ?? icon.color);
                }
                for (const effect of ["shadow", "innerShadow", "glow"])
                    if (material[effect]) material[effect] = {...material[effect], color: color(material[effect].color)};
                if (material.gradient) material.gradient = {...material.gradient,
                    stops: material.gradient.stops.map(stop => ({...stop, color: color(stop.color)}))};
                if (material.borders) material.borders = material.borders.map(border => ({...border, color: color(border.color)}));
                if (material.bevel) material.bevel = {...material.bevel, light: color(material.bevel.light), dark: color(material.bevel.dark)};
                return [layer, material];
            }));
        }
        const states = Object.fromEntries(appearanceStates.flatMap(state => {
            const icons = Object.fromEntries(Object.entries(style.layers).filter(([, id]) => {
                const icon = theme.materials[id].icon;
                return icon && theme.icons[icon].states[state];
            }));
            const layers = {...icons, ...style.states[state]};
            return Object.keys(layers).length ? [[state, resolve(layers, state)]] : [];
        }));
        const display = theme.displays[style.analysis];
        const analysis = display && {...display, background: color(display.background), major: color(display.major),
            minor: color(display.minor), labels: color(display.labels),
            traces: Object.fromEntries(Object.entries(display.traces).map(([id, paint]) => [id, {...paint, color: color(paint.color)}]))};
        return {kind: style.kind, layers: resolve(style.layers), states, analysis,
            typography: theme.typography, transition: theme.transition,
            stack: style.stack?.map(layer => ({...layer, material: resolve({paint: layer.material}).paint})),
            geometry: style.geometry};
    }

    const tokensHas = token => appearanceRoles.includes(token);
    const displayTheme = (legacy, presentation) => {
        const active = activeAppearanceTheme(presentation);
        return active ? {...legacy, ...active.colors} : legacy;
    };

    function writeThemePack(presentation, options = {}) {
        const {themes, assets, icons, previewAssets} = readPresentation(presentation);
        if (!Object.keys(themes).length) appearanceError("theme pack is empty");
        const dependencies = new Set();
        for (const theme of Object.values(themes))
            for (const material of Object.values(theme.materials ?? {}).filter(Boolean))
                for (const id of [material.asset, material.mask, material.texture?.asset]) if (id) dependencies.add(id);
        const previews = options.previews ?? previewAssets;
        for (const id of previews ?? []) dependencies.add(checkReference(assets, id));
        const shipped = Object.fromEntries([...dependencies].map(id => [id, assets[id]]));
        const fonts = [...new Set(Object.values(themes).flatMap(theme =>
            Object.values(theme.typography ?? {}).filter(Boolean).flatMap(role => role.family ?? [])))];
        const manifest = {version: 1, name: options.name ?? Object.values(themes)[0].name,
            themes: Object.keys(themes), assets: Object.keys(shipped), icons: Object.keys(icons), fonts,
            previews, template: options.template};
        return JSON.stringify({schema: themePackSchema, manifest, themes, icons, assets: shipped}, null, 2) + "\n";
    }

    function visualTemplate(control, presentation, name) {
        const look = appearanceSource(control, presentation);
        if (!look) appearanceError("choose a themed control first");
        const style = {name: displayName(name), kind: look.kind, layers: look.layers,
            states: look.states, stack: look.stack, geometry: look.geometry, analysis: look.analysis};
        const ids = new Set([...Object.values(style.layers),
            ...Object.values(style.states).flatMap(Object.values),
            ...(style.stack ?? []).map(layer => layer.material)]);
        const materials = Object.fromEntries([...ids].map(id => [id, look.theme.materials[id]]));
        const iconIds = new Set(Object.values(materials).flatMap(m => m.icon ?? []));
        const icons = Object.fromEntries([...iconIds].map(id => [id, look.theme.icons[id]]));
        return writeThemePack({schema: presentationSchema, icons, assets: presentation.assets, activeTheme: "template",
            themes: {template: {name: style.name, colors: look.theme.colors, typography: look.theme.typography,
                materials, displays: look.analysis ? {[look.analysis]: look.theme.displays[look.analysis]} : undefined,
                components: {template: style}, defaults: {[style.kind]: "template"}}}},
            {template: {theme: "template", style: "template"}});
    }

    function importVisualTemplate(text, presentation) {
        const data = JSON.parse(text);
        if (!data?.manifest?.template) appearanceError("this pack has no visual template");
        const incoming = readThemePack(text);
        const {theme, style} = data.manifest.template;
        const source = themeResolution(incoming, theme)?.definition;
        const template = source?.components[style];
        if (!template) appearanceError("visual template style is missing");
        const next = readPresentation(presentation);
        const active = activeAppearanceTheme(next);
        if (!active) appearanceError("choose a destination theme first");
        const activeId = next.activeTheme;
        const assetIds = new Map();
        for (const [id, asset] of Object.entries(incoming.assets))
            assetIds.set(id, registerAsset(next.assets, asset.data, asset.name));
        const iconIds = new Map(), materialIds = new Map();
        const icons = {...active.icons}, materials = {...active.materials};
        const changes = next.themes[activeId] = {...next.themes[activeId],
            displays: {...next.themes[activeId].displays},
            icons: {...next.themes[activeId].icons}, materials: {...next.themes[activeId].materials},
            components: {...next.themes[activeId].components}};
        for (const [id, icon] of Object.entries(source.icons)) {
            const key = availableId(icons, id);
            iconIds.set(id, key);
            icons[key] = icon;
            (changes.icons ??= {})[key] = icon;
        }
        for (const [id, material] of Object.entries(source.materials)) {
            const key = availableId(materials, id);
            materialIds.set(id, key);
            const rewritten = {...material};
            for (const field of ["asset", "mask"])
                if (material[field]) rewritten[field] = assetIds.get(material[field]);
            if (material.texture)
                rewritten.texture = {...material.texture, asset: assetIds.get(material.texture.asset)};
            if (material.icon) rewritten.icon = iconIds.get(material.icon);
            materials[key] = rewritten;
            (changes.materials ??= {})[key] = rewritten;
        }
        const rewrite = layers => Object.fromEntries(Object.entries(layers).map(([role, id]) => [role, materialIds.get(id)]));
        let analysis;
        if (template.analysis) {
            analysis = availableId(active.displays, template.analysis);
            changes.displays[analysis] = source.displays[template.analysis];
        }
        const templateId = availableId(active.components, style);
        (changes.components ??= {})[templateId] = {...template, analysis, layers: rewrite(template.layers),
            states: Object.fromEntries(Object.entries(template.states).map(([state, layers]) => [state, rewrite(layers)])),
            stack: template.stack?.map(layer => ({...layer, material: materialIds.get(layer.material)}))};
        return {presentation: readPresentation(next), id: templateId};
    }

    // The native backend consumes only resolved colors, material indices,
    // geometry and static bitmap resources. No browser is needed at runtime.
    async function nativeScene(controls, legacyTheme, presentation, {phone = false, values = [], catalog = [], visualState = ""} = {}, bake) {
        if (!presentation?.activeTheme) appearanceError("choose a theme for native preview");
        const colors = displayTheme(legacyTheme, presentation);
        const positions = phone ? phonePlacements(controls) : controls;
        const columns = phone ? phoneGrid.columns : wideGrid.columns;
        const rows = Math.max(1, Math.ceil(positions.reduce((bottom, box) => Math.max(bottom, box.y + box.height), 0)));
        const width = columns * 76, height = rows * 76;
        if (height > 4096) appearanceError("the native preview is taller than 4096 logical pixels");
        const materials = [], instances = [], materialIds = new Map(), images = new Map(), assetIds = new Map();
        const parameters = new Map((catalog ?? []).map(parameter => [parameter.key, parameter]));
        const current = new Map(values);
        const rank = new Map(layerOrder(controls).map((control, i) => [control, i]));
        function imageIndex(data) {
            if (!images.has(data)) images.set(data, images.size);
            return images.get(data);
        }
        function layerIndices(layers, control) {
            return Object.fromEntries(Object.entries(layers).map(([role, source]) => {
                const material = {...source};
                for (const property of ["asset", "mask"]) {
                    if (material[property] === undefined) continue;
                    material[property] = imageIndex(presentation.assets[material[property]].data);
                }
                if (material.texture) material.texture = {...material.texture,
                    asset: imageIndex(presentation.assets[material.texture.asset].data)};
                if (control.artwork && (control.kind === "image" ? role === "background" : role === "face")) {
                    material.asset = imageIndex(control.artwork);
                    delete material.slices;
                }
                const key = JSON.stringify(material);
                if (!materialIds.has(key)) {
                    materialIds.set(key, materials.length);
                    materials.push(material);
                }
                return [role, materialIds.get(key)];
            }));
        }
        function matchingRules(expression) {
            const groups = conditionRules(expression);
            return !groups.length || groups.some(group => group.every(({key, operator, value}) =>
                compareValue(current.get(key) ?? parameters.get(key)?.value, operator, value)));
        }
        for (const [index, control] of controls.entries()) {
            if (!matchingRules(control.visibleWhen)) continue;
            const look = resolveAppearance(control, presentation);
            if (!look) continue;
            const {x, y, width, height} = positions[index];
            const param = parameters.get(control.key);
            const raw = current.get(control.key) ?? param?.value;
            const value = control.kind === "meter" ? .64 : Number.isFinite(raw) && param?.maximum > param.minimum
                ? clamp((raw - param.minimum) / (param.maximum - param.minimum), 1) : .5;
            const label = control.typography ?? look.typography.label ?? {};
            instances.push({id: index, rank: rank.get(control), kind: look.kind,
                label: control.hideHeading || isPanelArt(control.kind) ? "" : control.label,
                rotation: control.rotation ?? 0, indicator: control.showGainReduction ? .25 : undefined,
                display: param?.choices?.[Math.round(raw - param.minimum)],
                activeStates: appearanceStates.includes(visualState) ? [visualState] : undefined,
                rect: [x * 76 + 4, y * 76 + 4, width * 76 - 8, height * 76 - 8], value,
                selected: control.kind === "toggle" && value === 1, disabled: Boolean(param?.readonly) || !matchingRules(control.enabledWhen),
                typography: label, layers: layerIndices(look.layers, control), geometry: look.geometry, analysis: look.analysis, transition: look.transition,
                stack: look.stack?.map(layer => ({...layer, material: layerIndices({paint: layer.material}, {}).paint})), states:
                    Object.fromEntries(Object.entries(look.states).map(([state, layers]) => [state, layerIndices(layers, control)]))});
        }
        if (legacyTheme.backgroundArtwork) assetIds.set("backgroundAsset", imageIndex(legacyTheme.backgroundArtwork));
        if (images.size > 48) appearanceError("the native preview supports up to 48 referenced bitmaps");
        let encodedBytes = 0;
        const assets = [], sourceScales = [];
        for (const data of images.keys()) {
            const baked = await bake(data);
            const png = typeof baked === "string" ? baked : baked.png;
            sourceScales.push(typeof baked === "string" ? [1, 1] : baked.sourceScale);
            encodedBytes += png.length;
            if (encodedBytes > 10 * 1024 * 1024) appearanceError("native preview bitmap budget exceeded");
            assets.push(png);
        }
        for (const material of materials) {
            if (!material.slices) continue;
            const [x, y] = sourceScales[material.asset];
            material.slices.source = material.slices.insets.map((inset, i) => inset * (i % 2 ? x : y));
        }
        return JSON.stringify({schema: "cell.ui-skin-scene.v1", width, height, colors, ...Object.fromEntries(assetIds),
            materials, assets, controls: instances.sort((a, b) => a.rank - b.rank)}, null, 2) + "\n";
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
        if (new TextEncoder().encode(text).length > maxProjectBytes) appearanceError("layout exceeds 2 MiB");
        const presentation = readPresentation(value.presentation);
        const artwork = (id, inline) => {
            if (id === undefined) return inline;
            if (inline) appearanceError("ambiguous inline artwork and asset reference");
            return presentation.assets[checkReference(presentation.assets, id)].data;
        };
        const controls = value.controls.map((control, index) => {
            if (!control) return readControl(control, index, value.schema === schema);
            const {assetId, ...fields} = control;
            return readControl({...fields, artwork: artwork(assetId, fields.artwork)}, index, value.schema === schema);
        });
        const {backgroundAsset, ...theme} = value.theme ?? {};
        theme.backgroundArtwork = artwork(backgroundAsset, theme.backgroundArtwork);
        const legacy = value.schema === "cell.ui-layout.v1";
        return {controls, presentation, theme: readTheme(theme), programs: legacy ? undefined : readPrograms(value.programs),
            catalog: legacy ? undefined : value.catalog === null || value.catalog === undefined
                ? null : readCatalog({type: "parameters", parameters: value.catalog})};
    }

    function indicatorSettings(control) {
        const {kind, indicatorSource, threshold} = control;
        if (kind !== "indicator") {
            if (indicatorSource !== undefined || threshold !== undefined) throw new Error("Only indicators can have a lamp source and threshold.");
            return;
        }
        const source = indicatorSource === undefined ? "parameter" : indicatorSource;
        const level = threshold === undefined ? (source === "meter" ? -18 : 1) : threshold;
        if (!["parameter", "meter"].includes(source) || !Number.isFinite(level))
            throw new Error("Choose a lamp source and a finite threshold.");
        return {indicatorSource: source, threshold: level};
    }

    function readControl(control, index, hasPhone) {
        if (Object.hasOwn(control ?? {}, "cells"))
            throw new Error(`Control ${index + 1} has an unsupported "cells" field. Segmented displays use exactly two cells.`);
        const {kind, label, key = "", x, y, width = kinds[kind]?.width, height = 1,
            accent = "", artwork = "", artworkMode, skin = "modern", phone, fit = "contain",
            direction = "horizontal", thickness = 2, displayCodes, segmented, detentLabels, showGainReduction, rotation = 0, layer} = control ?? {};
        const normalizedLabel = readLabel(label);
        if (typeof kind !== "string" || !Object.hasOwn(kinds, kind) || !normalizedLabel ||
            (hasBinding(kind) && !(kind === "led" && key === "") && !validText(key)) ||
            typeof skin !== "string" || !Object.hasOwn(skins, skin) ||
            !validBox({x, y, width, height}, wideGrid) ||
            (detentLabels !== undefined && kind !== "knob") ||
            (showGainReduction !== undefined && (kind !== "knob" || typeof showGainReduction !== "boolean")) ||
            typeof accent !== "string" || (accent && !color.test(accent)) ||
            (artwork !== "" && (!hasArtwork(kind) || !validArtwork(artwork, kind === "image" ? maxPanelAssetBytes : maxAssetBytes))) ||
            (artworkMode !== undefined && (!artwork || !validArtworkMode(kind, artworkMode))) ||
            (kind === "image" && !imageFits.includes(fit)) ||
            (segmented !== undefined && (kind !== "stepper" || typeof segmented !== "boolean")) ||
            (displayCodes !== undefined && (!hasDisplayCodes(kind) || (kind === "led" && !key))) ||
            (kind === "line" && (!["horizontal", "vertical"].includes(direction) ||
                !Number.isInteger(thickness) || thickness < 1 || thickness > 16)))
            throw new Error(`Invalid settings for control ${index + 1}.`);
        const result = {kind, label: normalizedLabel, key: hasBinding(kind) ? key : "", x, y, width, height, accent, artwork, skin};
        if (control.visualOverrides !== undefined)
            result.visualOverrides = readLocalAppearance(control.visualOverrides, appearanceKind(kind));
        if (control.visualStyle !== undefined) {
            if (!validId(control.visualStyle)) appearanceError("invalid component style ID");
            result.visualStyle = control.visualStyle;
        }
        if (artworkMode) result.artworkMode = artworkMode;
        if (layer !== undefined) {
            if (!Number.isInteger(layer) || layer < 1 || layer > maxControls)
                throw new Error(`Invalid layer for control ${index + 1}. Use an integer from 1 to ${maxControls}.`);
            result.layer = layer;
        }
        const angle = readRotation(rotation);
        if (angle) result.rotation = angle;
        if (detentLabels !== undefined) result.detentLabels = readDetentLabels(detentLabels);
        if (showGainReduction) result.showGainReduction = true;
        Object.assign(result, buttonSettings(kind, control));
        Object.assign(result, ruleSettings(control));
        Object.assign(result, ledSettings(control));
        Object.assign(result, indicatorSettings(control));
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

    function namedComponent(name, controls) {
        const label = readLabel(name);
        if (!label || label.length > 80) throw new Error("Name the component using 1–80 characters.");
        // Reuse the clipboard validator for every control and both placements.
        const component = {schema: "cell.ui-component.v1", name: label, controls: readClipboard(writeClipboard(controls))};
        clipboardSize(JSON.stringify(component));
        return component;
    }

    function readComponent(text) {
        clipboardSize(text);
        const data = JSON.parse(text);
        if (data?.schema !== "cell.ui-component.v1" || !Array.isArray(data.controls))
            throw new Error("Choose a Cell Designer component file.");
        return namedComponent(data.name, data.controls);
    }

    function instantiateComponent(component, parameterMap, actionMap) {
        const replace = (map, key) => map.get(key) ?? key;
        const rule = text => {
            const groups = conditionRules(text);
            if (!groups.flat().some(({key}) => replace(parameterMap, key) !== key)) return text;
            return groups.map(group => group.map(({key, operator, value}) =>
                `${JSON.stringify(replace(parameterMap, key))} ${operator} ${value}`).join(" && ")).join(" || ");
        };
        return component.controls.map((control, index) => {
            const placed = {...control};
            if (hasParameterBinding(control)) placed.key = replace(parameterMap, control.key);
            if (control.labelFrom)
                placed.labelFrom = {...control.labelFrom, key: replace(parameterMap, control.labelFrom.key)};
            if (control.visibleWhen) placed.visibleWhen = rule(control.visibleWhen);
            if (control.enabledWhen) placed.enabledWhen = rule(control.enabledWhen);
            for (const field of ["action", "minusAction", "plusAction", "chordAction"])
                if (control[field]) placed[field] = replace(actionMap, control[field]);
            return readControl(placed, index, true);
        });
    }

    function serialize(controls, theme, catalog, programs, presentation) {
        const packageData = {...readPresentation(), ...presentation, assets: {...presentation?.assets}};
        const projectControls = controls.map(control => {
            if (!control.artwork || typeof control.artwork !== "string" ||
                !imageData.test(control.artwork)) return control;
            const {artwork, ...fields} = control;
            return {...fields, assetId: registerAsset(packageData.assets, artwork, control.label)};
        });
        const {backgroundArtwork, ...projectTheme} = theme;
        if (backgroundArtwork) {
            if (typeof backgroundArtwork === "string" && imageData.test(backgroundArtwork))
                projectTheme.backgroundAsset = registerAsset(packageData.assets, backgroundArtwork, "Panel background");
            else projectTheme.backgroundArtwork = backgroundArtwork;
        }
        const hasPresentation = packageData.activeTheme || Object.keys(packageData.assets).length || Object.keys(packageData.themes).length;
        return JSON.stringify({schema, controls: projectControls, theme: projectTheme, catalog, programs: programs ?? undefined,
            presentation: hasPresentation ? packageData : undefined}, null, 2) + "\n";
    }

    function validBox(box, grid) {
        if (!box) return false;
        const {x, y, width, height} = box;
        return Number.isInteger(width) && width >= 1 && width <= grid.columns &&
            Number.isInteger(height) && height >= 1 && height <= grid.maxHeight &&
            Number.isFinite(x) && x >= 0 && x <= grid.columns - width &&
            Number.isFinite(y) && y >= 0 && y <= grid.rows - height;
    }

    function constrain(box, grid) {
        const width = Math.max(1, Math.min(box.width, grid.columns));
        const height = Math.max(1, Math.min(box.height, grid.maxHeight));
        return {x: clamp(box.x, grid.columns - width), y: clamp(box.y, grid.rows - height), width, height};
    }

    function overlaps(a, b) {
        const tolerance = 1e-9;
        return a.x + tolerance < b.x + b.width && b.x + tolerance < a.x + a.width &&
               a.y + tolerance < b.y + b.height && b.y + tolerance < a.y + a.height;
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
            const gap = i * space / (boxes.length - 1);
            result[index] = {...translate(box, 0, 0), [axis]: cursor + (Number.isInteger(space) ? Math.round(gap) : gap)};
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
        if ([...boxes, ...occupied].some(box => !Number.isInteger(box.x) || !Number.isInteger(box.y))) {
            const rows = Array.from({length: grid.rows}, () => []);
            for (const box of occupied)
                for (let y = Math.floor(box.y); y < Math.min(grid.rows, Math.ceil(box.y + box.height)); y++) rows[y].push(box);
            for (let y = 0; y <= grid.rows - height; y++) {
                for (let x = 0; x <= grid.columns - width; x++) {
                    const placed = boxes.map(box => translate(box, x - left, y - top));
                    const fits = placed.every(box => {
                        for (let row = Math.floor(box.y); row < Math.ceil(box.y + box.height); row++) {
                            if (rows[row]?.some(other => overlaps(box, other))) return false;
                        }
                        return true;
                    });
                    if (fits) return placed.map(box => constrain(box, grid));
                }
            }
            return null;
        }
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
            ...controls.filter(hasParameterBinding).map(control => control.key),
            ...controls.flatMap(ruleKeys)
        ])];
    }

    function meterKeys(controls) {
        return [...new Set(controls.flatMap(control => {
            if (isMeterBinding(control)) return [control.key];
            return control.kind === "knob" && control.showGainReduction ? ["gain-reduction"] : [];
        }))];
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
            if (!catalog || !hasParameterBinding(control)) return [];
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
            if (control.detentLabels) {
                const detents = stepCount(parameter) + 1;
                if (detents < 2 || detents > maxDetentLabels)
                    return [`${control.label}: detent labels require 2 to ${maxDetentLabels} discrete values`];
                const labels = control.detentLabels === "choices" ? parameter.choices : control.detentLabels;
                if (labels.length !== detents)
                    return [`${control.label}: supply one detent label for each of the ${detents} values`];
            }
            const outside = control.displayCodes?.find(({value}) => value < parameter.minimum || value > parameter.maximum);
            if (outside) return [`${control.label}: display code value ${outside.value} is outside ${control.key}'s range`];
            return [];
        });
    }

    function bindingContract(controls) {
        const edited = new Set(controls.filter(control => hasParameterBinding(control) && control.kind !== "indicator")
            .map(control => control.key));
        return {
            parameters: parameterKeys(controls).map(key => ({key, writable: edited.has(key)})),
            actions: actionBindings(controls)
        };
    }

    // Three-digit byte escapes make the header independent of source-file encoding.
    function cppBindings(controls) {
        const literal = key => {
            const bytes = new TextEncoder().encode(key);
            return '"' + [...bytes].map(byte => byte >= 32 && byte <= 126 && byte !== 34 && byte !== 92 && byte !== 63
                ? String.fromCharCode(byte) : "\\" + byte.toString(8).padStart(3, "0")).join("") + '"';
        };
        const entries = controls.filter(hasParameterBinding);
        const {parameters, actions} = bindingContract(controls);
        const keys = parameters.map(({key}) => key);
        const readOnly = parameters.filter(binding => !binding.writable).map(({key}) => key);
        return `// Generated by Cell UI Designer.
#pragma once
#include <array>
#include <string_view>
#include <cell/web_parameter_bridge.hpp>

${actions.length ? "#define CELL_DESIGNER_HAS_ACTIONS 1\n\n" : ""}${readOnly.length ? "#define CELL_DESIGNER_HAS_RULE_SOURCES 1\n\n" : ""}namespace cell::designer {
enum class ControlKind { slider, knob, fader, choice, toggle, readout, stepper, led, momentary, indicator };
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

    return {readPresentation, readThemePack, writeThemePack, packAssetPaths, relativeAssetPath,
        nativeScene, visualTemplate, importVisualTemplate, readLocalAppearance, presentationSchema, themePackSchema,
        appearanceStates, appearanceLayers, appearanceKind, activeAppearanceTheme, appearanceSource, iconDataUrl, matchesAppearance, registerAsset, resolveAppearance, displayTheme,
        wideGrid, phoneGrid, kinds, skins, defaultTheme, showcase, maxControls, maxAssetBytes, maxPanelAssetBytes,
        maxProjectBytes, imageType, validArtwork, isPanelArt, isProgramControl, clamp, validText, hasArtwork, hasDisplayArtwork, hasCapArtwork, hasSwitchArtwork, hasParameter,
        hasDisplayCodes, isMeterBinding, indicatorSettings, ledGlyphs, ledSettings, parseDisplayCodes, displayCode, numericLedCode, hasParameterBinding,
        maxDetentLabels, readDetentLabels, readRotation,
        buttonDefaults, buttonSettings, actionBindings,
        readLabel, typographyFamilies, typographySettings, silkscreen,
        ruleSettings, ruleKeys, conditionRules, compareValue, variantsDisjoint, parseLabelMap,
        hasBinding, readProject, readCatalog, readPrograms, serialize, overlaps,
        writeClipboard, readClipboard, namedComponent, readComponent, instantiateComponent,
        constrain, firstOpenPlace, moveBoxes, alignBoxes, distributeBoxes, findGroupPlacement,
        readingOrder, layerPriority, layerOrder, phoneSize, phonePlacements, bindingIssues,
        parameterKeys, meterKeys, twoState, stepCount, bindingContract, cppBindings};
})();
