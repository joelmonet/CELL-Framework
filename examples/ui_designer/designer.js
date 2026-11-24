"use strict";

const model = CellDesignerModel;
const runtime = CellDesignerRuntime;
const {wideGrid, phoneGrid, kinds, skins} = model;
const query = selector => document.querySelector(selector);
const canvas = query("#canvas");
const canvasFrame = query("#canvas-frame");
const backgroundImage = document.createElement("img");
backgroundImage.className = "panel-background";
backgroundImage.alt = "";
canvas.append(backgroundImage);
const form = query("#properties");
const field = name => form.elements.namedItem(name);
const themeForm = query("#theme");
const status = query("#status");
const undoButton = query("#undo");
const redoButton = query("#redo");
const copyButton = query("#copy-controls");
const pasteButton = query("#paste-controls");
const multiSelect = query("#multi-select");
const editArt = query("#edit-art");
const artworkElement = query("#artwork-element");
const backgroundFit = query("#background-fit");
const clearBackground = query("#clear-background");
const selectionTools = query("#selection-tools");
const previewPanel = query("#preview-panel");
const previewFrame = previewPanel.querySelector("iframe");
const previewButton = query("#preview");
const controls = [];
const phoneBoxes = new Map();
const undoStack = [], redoStack = [];
const previewValues = new Map();
let previewProgram;
const encoder = new TextEncoder();
const historyLimit = 64;
let theme = {...model.defaultTheme};
let catalog = null;
let programs = null;
let selected;
let selection = [];
let localClipboard = "";
let preferLocalClipboard = false;
let grid = wideGrid;
let visibleRows = wideGrid.rows;
let generation = 0;
let layoutVersion = 0;
let saved = model.serialize([], theme, catalog, programs);

for (const [kind, spec] of Object.entries(kinds)) {
    const button = document.createElement("button");
    button.type = "button";
    button.dataset.kind = kind;
    button.draggable = true;
    button.textContent = spec.title;
    button.setAttribute("aria-label", "Add " + spec.title.toLowerCase());
    button.onclick = () => addControl(kind);
    button.ondragstart = event => event.dataTransfer.setData("text/plain", kind);
    query("#palette").append(button);
}
for (const [value, label] of Object.entries(skins)) field("skin").add(new Option(label, value));
for (const [key, family] of Object.entries(model.typographyFamilies)) field("type-family").add(new Option(family.label, key));
const typeStyles = document.createElement("style");
typeStyles.textContent = runtime.fontCss(Object.keys(model.typographyFamilies).map(family => ({family}))) +
    "\n" + runtime.typographyCss;
document.head.append(typeStyles);

const typeFields = {family: "type-family", size: "type-size", weight: "type-weight", tracking: "type-tracking",
    alignment: "type-alignment", letterCase: "type-letter-case"};
function showTypography(typography = {}) {
    for (const [key, name] of Object.entries(typeFields)) field(name).value = typography[key] ?? "";
}
function inspectTypography() {
    const settings = {};
    for (const [key, name] of Object.entries(typeFields)) {
        const input = field(name);
        if (input.value !== "") settings[key] = input.type === "number" ? input.valueAsNumber : input.value;
    }
    return model.typographySettings(settings);
}

query("#silkscreen").onclick = () => showTypography(model.silkscreen);
query("#reset-typography").onclick = () => showTypography();
query("#display-glyphs").textContent = Object.keys(model.ledGlyphs).filter(character => character !== " ").join(" ");

function controlsOnly() {
    return controls.map(({tile, ...control}) => control);
}

function documentText() {
    return model.serialize(controlsOnly(), theme, catalog, programs);
}

function paintImage(image, artwork, fit) {
    if (artwork && image.getAttribute("src") !== artwork) image.src = artwork;
    if (!artwork) image.removeAttribute("src");
    image.hidden = !artwork;
    image.style.objectFit = fit;
}

function refreshProject() {
    undoButton.disabled = !undoStack.length;
    redoButton.disabled = !redoStack.length;
    query("#project-state").textContent = documentText() === saved ? "" : "Unsaved changes";
    refreshCanvas();
    for (const name of Object.keys(model.defaultTheme)) {
        const value = theme[name];
        themeForm.elements.namedItem(name).value = value;
        canvas.style.setProperty("--" + name, value);
    }
    backgroundFit.value = theme.backgroundFit ?? "cover";
    clearBackground.disabled = !theme.backgroundArtwork;
    paintImage(backgroundImage, theme.backgroundArtwork, backgroundFit.value);
    query("#parameter-keys").replaceChildren(...(catalog ?? []).map(({key, name}) => new Option(name, key)));
    refreshDisplayChoices();
    query("#catalog-status").textContent = catalog ? `${catalog.length} parameters loaded` : "No parameter snapshot loaded";
    query("#program-status").textContent = programs
        ? `${programs.banks.length} banks, ${programs.banks.reduce((count, bank) => count + bank.programs.length, 0)} programs`
        : "";
    query("#clear-parameters").disabled = !catalog;
    if (!previewPanel.hidden) renderPreview();
}

function snapshot() {
    return {layout: controlsOnly(), theme, catalog, programs, selection: selection.map(control => controls.indexOf(control))};
}

function record(before) {
    if (model.serialize(before.layout, before.theme, before.catalog, before.programs) === documentText()) return;
    undoStack.push(before);
    if (undoStack.length > historyLimit) undoStack.shift();
    redoStack.length = 0;
    refreshProject();
}

function select(items) {
    selection = items;
    selected = items.length === 1 ? items[0] : undefined;
    copyButton.disabled = !items.length;
    form.hidden = !selected;
    query("#empty-inspector").hidden = Boolean(items.length);
    selectionTools.hidden = items.length < 2;
    canvas.classList.toggle("multi-selection", items.length > 1);
    query("#selection-count").textContent = `${items.length} controls selected`;
    for (const button of selectionTools.querySelectorAll("[data-distribute]")) button.disabled = items.length < 3;
    for (const item of controls) item.tile.setAttribute("aria-pressed", String(items.includes(item)));
    const control = selected;
    if (canvas.classList.contains("edit-art")) {
        artworkElement.replaceChildren(new Option("Choose artwork", ""),
            ...controls.flatMap((item, index) => model.isPanelArt(item.kind)
                ? [new Option(`${item.label} · ${kinds[item.kind].title}`, index)] : []));
        artworkElement.value = control && model.isPanelArt(control.kind) ? String(controls.indexOf(control)) : "";
    }
    refreshRulePicker();
    if (!control) return;
    for (const key of ["label", "key", "skin"])
        field(key).value = control[key];
    inspectPlacement(control);
    field("key").required = model.hasBinding(control.kind);
    query("#binding-field").hidden = !field("key").required;
    query("#binding-label").textContent = control.kind === "meter" ? "Meter source" : "Parameter key";
    field("key").setAttribute("list", control.kind === "meter" ? "meter-keys" : "parameter-keys");
    field("use-accent").checked = Boolean(control.accent);
    field("accent").value = control.accent || theme.accent;
    field("accent").disabled = !control.accent;
    showTypography(control.typography);
    field("visible-when").value = control.visibleWhen ?? "";
    field("enabled-when").value = control.enabledWhen ?? "";
    field("label-key").value = control.labelFrom?.key ?? "";
    field("label-source").value = control.labelFrom?.source ?? "choice";
    field("label-map").value = (control.labelFrom?.labels ?? []).map(({value, label}) => `${value} = ${label}`).join("\n");
    refreshLabelFields();
    field("action-key").value = control.action ?? "";
    field("trigger").value = control.trigger ?? "tap";
    field("hold-ms").value = control.holdMs ?? model.buttonDefaults.holdMs;
    field("repeat-delay").value = control.repeatDelay ?? model.buttonDefaults.repeatDelay;
    field("repeat-ms").value = control.repeatMs ?? model.buttonDefaults.repeatMs;
    field("minus-action").value = control.minusAction ?? "";
    field("plus-action").value = control.plusAction ?? "";
    field("chord-action").value = control.chordAction ?? "";
    field("chord-ms").value = control.chordMs ?? model.buttonDefaults.chordMs;
    refreshActionFields();
    query("#display-settings").hidden = !model.hasDisplayCodes(control.kind);
    query("#segmented-setting").hidden = control.kind !== "stepper";
    field("segmented").checked = control.segmented ?? false;
    field("display-codes").value = (control.displayCodes ?? []).map(({value, code}) => `${value} = ${code}`).join("\n");
    refreshDisplayChoices();
    query("#skin-field").hidden = model.isPanelArt(control.kind);
    query("#image-fit").hidden = control.kind !== "image";
    field("fit").value = control.fit ?? "contain";
    query("#line-options").hidden = control.kind !== "line";
    field("direction").value = control.direction ?? "horizontal";
    field("thickness").value = control.thickness ?? 2;
    field("thickness").disabled = control.kind !== "line";
    query("#artwork").hidden = !model.hasArtwork(control.kind);
    query("#clear-artwork").disabled = !control.artwork;
}

function refreshRulePicker() {
    const choices = controls.flatMap((control, index) => control.visibleWhen || control.enabledWhen || control.labelFrom
        ? [new Option([control.label, control.visibleWhen || control.enabledWhen || control.labelFrom.key].join(" · "), index)]
        : []);
    const picker = query("#rule-element");
    query("#rule-picker").hidden = !choices.length;
    picker.replaceChildren(new Option("Choose control", ""), ...choices);
    const index = String(controls.indexOf(selected));
    picker.value = choices.some(option => option.value === index) ? index : "";
}

function refreshLabelFields() {
    const source = Boolean(field("label-key").value.trim());
    query("#label-source-setting").hidden = !source;
    query("#label-map-setting").hidden = !source || field("label-source").value !== "map";
}

function refreshActionFields() {
    const action = selected?.kind === "action";
    const pair = selected?.kind === "action-pair";
    const hasChord = pair || selected?.kind === "stepper";
    const hold = action && field("trigger").value === "hold";
    const repeat = action && field("trigger").value === "repeat";
    query("#action-settings").hidden = !action;
    query("#hold-settings").hidden = !hold;
    query("#repeat-settings").hidden = !repeat;
    query("#action-pair-settings").hidden = !pair;
    query("#chord-settings").hidden = !hasChord;
    field("hold-ms").disabled = !hold;
    field("repeat-delay").disabled = field("repeat-ms").disabled = !repeat;
    field("chord-ms").disabled = !hasChord;
}

function refreshDisplayChoices() {
    const details = query("#display-choices");
    const parameter = selected && model.hasDisplayCodes(selected.kind)
        ? catalog?.find(parameter => parameter.key === field("key").value.trim()) : undefined;
    const choices = parameter?.choices ?? [];
    details.hidden = !choices.length;
    const fields = choices.slice(0, 128).flatMap((choice, index) => {
        const value = document.createElement("dt");
        const label = document.createElement("dd");
        value.textContent = String(parameter.minimum + index);
        label.textContent = choice;
        return [value, label];
    });
    details.querySelector("dl").replaceChildren(...fields);
}

function additive(event) {
    return multiSelect.getAttribute("aria-pressed") === "true" || event.shiftKey || event.ctrlKey || event.metaKey;
}

function toggleControl(control) {
    select(selection.includes(control) ? selection.filter(item => item !== control) : [...selection, control]);
}

multiSelect.onclick = () => multiSelect.setAttribute("aria-pressed",
    String(multiSelect.getAttribute("aria-pressed") !== "true"));

function placement(control) {
    return grid === phoneGrid ? (control.phone ?? phoneBoxes.get(control)) : control;
}

function inspectPlacement(control) {
    const box = placement(control);
    if (!box) return;
    field("column").value = box.x + 1;
    field("row").value = box.y + 1;
    field("width").value = box.width;
    field("height").value = box.height;
    field("column").max = field("width").max = grid.columns;
    field("row").max = grid.rows;
    field("height").max = grid.maxHeight;
    query("#auto-phone-position").hidden = grid !== phoneGrid || !control.phone;
}

function setPlacements(items, boxes) {
    const next = boxes.map(box => model.constrain(box, grid));
    if (items.every((control, i) => {
        const current = placement(control);
        const box = next[i];
        return current.x === box.x && current.y === box.y && current.width === box.width && current.height === box.height;
    })) return;
    items.forEach((control, i) => {
        if (grid === phoneGrid) control.phone = next[i];
        else Object.assign(control, next[i]);
    });
}

function position(tile, box) {
    tile.style.left = `${100 * box.x / grid.columns}%`;
    tile.style.top = `${100 * box.y / visibleRows}%`;
    tile.style.width = `${100 * box.width / grid.columns}%`;
    tile.style.height = `${100 * box.height / visibleRows}%`;
}

function refreshCanvas() {
    const phone = grid === phoneGrid;
    const boxes = phone ? model.phonePlacements(controls) : controls;
    phoneBoxes.clear();
    if (phone) boxes.forEach((box, index) => phoneBoxes.set(controls[index], box));
    const extent = boxes.reduce((maximum, box) => Math.max(maximum, box.y + box.height), 0);
    visibleRows = phone ? Math.min(grid.rows, Math.max(8, extent + 3)) : grid.rows;
    canvas.style.setProperty("--columns", grid.columns);
    canvas.style.setProperty("--rows", visibleRows);
    canvasFrame.dataset.size = phone ? "phone" : "wide";
    boxes.forEach((box, index) => position(controls[index].tile, box));
    const functionalBoxes = boxes.flatMap((box, index) =>
        model.isPanelArt(controls[index].kind) ? [] : [{box, rule: controls[index].visibleWhen}]);
    query("#overlap-status").hidden = !functionalBoxes.some((entry, i) =>
        functionalBoxes.slice(i + 1).some(other => model.overlaps(entry.box, other.box) &&
            !model.variantsDisjoint(entry.rule, other.rule)));
    query("#reset-phone-layout").hidden = !phone;
    query("#reset-phone-layout").disabled = !controls.some(control => control.phone);
    query("#phone-help").hidden = !phone;
    if (selected) inspectPlacement(selected);
}

function paint(control) {
    const {tile, label, kind, skin, accent, artwork} = control;
    tile.dataset.skin = skin;
    if (kind === "line") {
        tile.dataset.direction = control.direction ?? "horizontal";
        tile.style.setProperty("--line-thickness", `${control.thickness ?? 2}px`);
    }
    tile.style.setProperty("--tile-accent", accent || "var(--accent)");
    tile.setAttribute("aria-label", `${label} · ${kind}`);
    tile.querySelector(".tile-label").textContent = label;
    runtime.paintTypography(tile, control.typography, model.typographyFamilies);
    paintImage(tile.querySelector("img"), artwork, kind === "image" ? control.fit ?? "contain" : "cover");
}

function cellAt(event) {
    const rect = canvas.getBoundingClientRect();
    return {
        x: Math.floor(grid.columns * (event.clientX - rect.left) / rect.width),
        y: Math.floor(visibleRows * (event.clientY - rect.top) / rect.height)
    };
}

function dragControl(control, startEvent) {
    if (startEvent.button !== 0 || !startEvent.isPrimary) return;
    startEvent.preventDefault();
    const {tile} = control;
    const pointer = startEvent.pointerId;
    const dragGrid = grid;
    const start = cellAt(startEvent);
    const extend = additive(startEvent);
    const items = selection.includes(control) ? selection : extend ? [...selection, control] : [control];
    const originals = items.map(placement);
    const original = originals[0];
    const rect = tile.getBoundingClientRect();
    const resizing = !extend && selected === control && rect.right - startEvent.clientX <= 17 &&
        rect.bottom - startEvent.clientY <= 17;
    let draft = null;
    tile.focus({preventScroll: true});
    tile.setPointerCapture(pointer);
    tile.onpointermove = event => {
        if (event.pointerId !== pointer || grid !== dragGrid) return;
        const next = cellAt(event);
        const dx = next.x - start.x, dy = next.y - start.y;
        if (!draft && !dx && !dy) return;
        draft = resizing ? [resizedBox(original, dx, dy)] : model.moveBoxes(originals, grid, dx, dy);
        items.forEach((item, i) => position(item.tile, draft[i]));
    };
    tile.onpointerup = event => {
        if (event.pointerId !== pointer || grid !== dragGrid || !tile.isConnected) return;
        if (!draft) {
            if (extend) toggleControl(control);
            else select([control]);
            return;
        }
        select(items);
        const before = snapshot();
        setPlacements(items, draft);
        record(before);
    };
    tile.onlostpointercapture = event => {
        if (event.pointerId !== pointer) return;
        tile.onpointermove = tile.onpointerup = null;
        for (const item of items) if (item.tile.isConnected) position(item.tile, placement(item));
    };
}

function resizedBox(box, dx, dy) {
    return model.constrain({...box,
        width: Math.min(grid.columns - box.x, box.width + dx),
        height: Math.min(grid.rows - box.y, box.height + dy)
    }, grid);
}

function add(data, activate) {
    if (controls.length >= model.maxControls) {
        status.textContent = `A layout can contain up to ${model.maxControls} controls.`;
        return;
    }
    const tile = document.createElement("button");
    tile.type = "button";
    tile.className = "tile";
    tile.dataset.kind = data.kind;
    tile.toggleAttribute("data-panel-art", model.isPanelArt(data.kind));
    tile.innerHTML = '<img alt="" hidden><span class="tile-label"></span><span class="tile-kind"></span>';
    const control = {...data, ...model.constrain(data, wideGrid), tile};
    controls.push(control);
    canvas.append(tile);
    tile.querySelector(".tile-kind").textContent = kinds[control.kind].title;
    paint(control);
    tile.onclick = event => {
        if (event.detail !== 0 || event.pointerType) return;
        if (additive(event)) toggleControl(control);
        else select([control]);
    };
    tile.onpointerdown = event => dragControl(control, event);
    tile.onkeydown = event => controlKey(control, event);
    if (activate) {
        select([control]);
        tile.focus();
    }
    return control;
}

function controlKey(control, event) {
    const items = selection.includes(control) ? selection : [control];
    const key = event.key.toLowerCase();
    const command = event.ctrlKey || event.metaKey;
    if ((key === " " || key === "enter") && additive(event)) {
        event.preventDefault();
        if (!event.repeat) toggleControl(control);
    } else if (key === "delete" || key === "backspace") {
        event.preventDefault();
        remove(items);
    } else if (command && key === "d") {
        event.preventDefault();
        duplicate(items);
    } else if (command && key === "a") {
        event.preventDefault();
        select([...controls]);
    } else if (key === "escape") {
        select([]);
    } else {
        const direction = {arrowleft: [-1, 0], arrowright: [1, 0], arrowup: [0, -1], arrowdown: [0, 1]}[key];
        if (!direction || command || event.altKey) return;
        event.preventDefault();
        if (event.shiftKey && items.length > 1) return;
        select(items);
        const before = snapshot();
        const boxes = items.map(placement);
        setPlacements(items, event.shiftKey ? [resizedBox(boxes[0], ...direction)] : model.moveBoxes(boxes, grid, ...direction));
        record(before);
        if (grid === phoneGrid) control.tile.scrollIntoView({block: "nearest"});
    }
}

function starter(kind) {
    const {width, height, title} = kinds[kind];
    let parameter;
    const choices = catalog ?? [];
    if (kind === "choice") parameter = choices.find(entry => entry.choices.length > 2) ?? choices.find(entry => entry.choices.length);
    if (kind === "led") parameter = choices.find(entry => entry.choices.length) ??
        choices.find(entry => Number.isInteger(entry.minimum) && Number.isInteger(entry.step) && model.stepCount(entry));
    if (kind === "toggle" || kind === "momentary") parameter = choices.find(model.twoState);
    if (kind === "stepper") parameter = choices.find(entry => entry.choices.length && model.stepCount(entry)) ??
        choices.find(entry => Number.isInteger(entry.minimum) && Number.isInteger(entry.step) && model.stepCount(entry)) ??
        choices.find(entry => model.stepCount(entry));
    if (!parameter && kind !== "stepper") parameter = choices.find(entry => entry.key === "gain") ?? choices[0];
    const key = model.hasParameter(kind) ? (parameter?.key ?? ({toggle: "bypass", momentary: "gate", stepper: "code", led: "code"}[kind] ?? "gain"))
        : kind === "meter" ? "output-peak" : "";
    const label = model.hasParameter(kind) ? ((parameter?.name || ({bypass: "Bypass", gate: "Gate", code: "Code"}[key] ?? "Gain")).slice(0, 80))
        : kind === "meter" ? "Output Peak" : title;
    const skin = {knob: "field", fader: "studio", meter: "broadcast"}[kind] ?? "modern";
    const defaults = kind === "action" ? {action: "factory-recall", trigger: "tap"} :
        kind === "action-pair" ? {minusAction: "previous-patch", plusAction: "next-patch"} : {};
    return {kind, key, label, width, height, skin, accent: "", artwork: "", ...defaults};
}

function placeNew(data, point, phoneSize) {
    const occupied = controls.filter(control => model.isPanelArt(control.kind) === model.isPanelArt(data.kind));
    const widePoint = grid === wideGrid && point ? point : model.firstOpenPlace(data, occupied);
    const result = {...data, ...widePoint};
    if (grid === phoneGrid) {
        const origin = point ?? model.firstOpenPlace(phoneSize, occupied.map(placement), phoneGrid);
        result.phone = model.constrain({...origin, ...phoneSize}, phoneGrid);
    }
    add(result, true);
}

function addControl(kind, point) {
    const before = snapshot();
    placeNew(starter(kind), point, model.phoneSize(kind));
    record(before);
}

function duplicate(items) {
    if (!items.length) return;
    if (controls.length + items.length > model.maxControls) {
        status.textContent = `A layout can contain up to ${model.maxControls} controls.`;
        return;
    }
    const phone = grid === phoneGrid;
    const boxes = model.findGroupPlacement(items.map(placement), controls.map(placement), grid);
    if (!boxes) {
        status.textContent = `Make room to duplicate the selection on the ${phone ? "phone" : "wide"} grid.`;
        return;
    }
    const wide = phone ? model.findGroupPlacement(items, controls, wideGrid) : boxes;
    const occupied = [...controls];
    let wideOverlap = false;
    const copies = items.map(({tile, phone: prior, ...data}, i) => {
        const box = wide?.[i] ?? {...data, ...model.firstOpenPlace(data, occupied)};
        wideOverlap ||= occupied.some(other => model.overlaps(box, other));
        occupied.push(box);
        const copy = {...data, x: box.x, y: box.y};
        if (phone) copy.phone = boxes[i];
        return copy;
    });
    if (!insertControls(copies)) return;
    status.textContent = wideOverlap ? "Duplicated. Check overlapping controls on the wide grid." :
        items.length === 1 ? "Control duplicated." : `${items.length} controls duplicated.`;
}

function insertControls(copies) {
    if (controls.length + copies.length > model.maxControls) {
        status.textContent = `A layout can contain up to ${model.maxControls} controls.`;
        return false;
    }
    if (encoder.encode(model.serialize([...controlsOnly(), ...copies], theme, catalog, programs)).length > model.maxProjectBytes) {
        status.textContent = "This edit would take the layout over 2 MiB.";
        return false;
    }
    const before = snapshot();
    const added = copies.map(copy => add(copy, false));
    select(added);
    record(before);
    added[0].tile.focus();
    return true;
}

function captureClipboard() {
    const phone = model.phonePlacements(controls);
    const chosen = new Set(selection);
    const copies = [];
    controls.forEach((control, index) => {
        if (!chosen.has(control)) return;
        const {tile, ...data} = control;
        copies.push({...data, phone: phone[index]});
    });
    try {
        localClipboard = model.writeClipboard(copies);
        return localClipboard;
    } catch (error) {
        status.textContent = error.message;
        return "";
    }
}

function pasteControls(text) {
    let items;
    try {
        items = model.readClipboard(text);
    } catch (error) {
        status.textContent = error.message;
        return;
    }
    const wide = model.findGroupPlacement(items, controls, wideGrid);
    const phone = model.findGroupPlacement(model.phonePlacements(items), model.phonePlacements(controls), phoneGrid);
    if (!wide || !phone) {
        status.textContent = `Make room on the ${wide ? "phone" : "wide"} grid to paste these controls.`;
        return;
    }
    const copies = items.map((control, i) => ({...control, ...wide[i], phone: phone[i]}));
    if (insertControls(copies)) status.textContent = items.length === 1 ? "Control pasted." : `${items.length} controls pasted.`;
}

function isTextInput(target) {
    return target instanceof HTMLElement && (target.isContentEditable || target.matches("input,select,textarea"));
}

document.addEventListener("copy", event => {
    if (!selection.length || !event.clipboardData || isTextInput(event.target) || window.getSelection()?.isCollapsed === false) return;
    const text = captureClipboard();
    if (!text) return;
    event.preventDefault();
    event.clipboardData.setData("text/plain", text);
    preferLocalClipboard = false;
    status.textContent = selection.length === 1 ? "Control copied." : `${selection.length} controls copied.`;
});

document.addEventListener("paste", event => {
    if (!event.clipboardData || isTextInput(event.target)) return;
    const text = event.clipboardData.getData("text/plain");
    if (!text) return;
    event.preventDefault();
    pasteControls(text);
});

copyButton.onclick = async () => {
    const text = captureClipboard();
    if (!text) return;
    try {
        if (navigator.clipboard?.writeText) {
            await navigator.clipboard.writeText(text);
            if (text === localClipboard) {
                preferLocalClipboard = false;
                status.textContent = "Selection copied to the clipboard.";
            }
            return;
        }
    } catch {
        // The tab clipboard remains available when the browser declines access.
    }
    if (text === localClipboard) {
        preferLocalClipboard = true;
        status.textContent = "Selection copied in this tab. Use Ctrl+C or Cmd+C to copy for other tabs.";
    }
};

pasteButton.onclick = async () => {
    if (preferLocalClipboard) {
        pasteControls(localClipboard);
        return;
    }
    let text;
    try {
        text = await navigator.clipboard?.readText();
    } catch {
        // Use the tab clipboard when the browser declines access.
    }
    if (text !== undefined) pasteControls(text);
    else if (localClipboard) pasteControls(localClipboard);
    else status.textContent = "Use Ctrl+V or Cmd+V to paste from the clipboard.";
};

function remove(items) {
    if (!items.length) return;
    const before = snapshot();
    const index = Math.min(...items.map(control => controls.indexOf(control)));
    for (const control of items) control.tile.remove();
    const kept = controls.filter(control => !items.includes(control));
    controls.splice(0, controls.length, ...kept);
    const next = controls[Math.min(index, controls.length - 1)];
    select(next ? [next] : []);
    (next?.tile ?? query("[data-kind]")).focus();
    record(before);
}

canvas.ondragover = event => event.preventDefault();
canvas.ondrop = event => {
    event.preventDefault();
    const kind = event.dataTransfer.getData("text/plain");
    if (Object.hasOwn(kinds, kind)) addControl(kind, cellAt(event));
};
canvas.onclick = event => { if (event.target === canvas) select([]); };
editArt.onclick = () => {
    const editing = canvas.classList.toggle("edit-art");
    editArt.setAttribute("aria-pressed", String(editing));
    query("#artwork-picker").hidden = !editing;
    select(selection.filter(control => model.isPanelArt(control.kind) === editing));
};
artworkElement.onchange = () => {
    select(artworkElement.value === "" ? [] : [controls[Number(artworkElement.value)]]);
};
for (const button of document.querySelectorAll("[data-duplicate]")) button.onclick = () => duplicate(selection);
for (const button of document.querySelectorAll("[data-remove]")) button.onclick = () => remove(selection);
query("#clear-selection").onclick = () => { select([]); multiSelect.focus(); };
for (const button of selectionTools.querySelectorAll("[data-align]")) button.onclick = () => {
    const before = snapshot();
    setPlacements(selection, model.alignBoxes(selection.map(placement), button.dataset.align));
    record(before);
};
for (const button of selectionTools.querySelectorAll("[data-distribute]")) button.onclick = () => {
    const boxes = model.distributeBoxes(selection.map(placement), button.dataset.distribute);
    if (!boxes) {
        status.textContent = "Move the selected controls farther apart to distribute them.";
        return;
    }
    const before = snapshot();
    setPlacements(selection, boxes);
    record(before);
};
field("use-accent").onchange = () => { field("accent").disabled = !field("use-accent").checked; };
field("key").oninput = refreshDisplayChoices;
field("trigger").onchange = refreshActionFields;
field("label-key").oninput = field("label-source").onchange = refreshLabelFields;
query("#rule-element").onchange = event => {
    if (event.target.value === "") return;
    const control = controls[Number(event.target.value)];
    select([control]);
    control.tile.focus();
};
form.onsubmit = event => {
    event.preventDefault();
    if (!selected) return;
    const label = model.readLabel(field("label").value.trim());
    const key = model.hasBinding(selected.kind) ? field("key").value.trim() : "";
    if (!label || (model.hasBinding(selected.kind) && !model.validText(key))) {
        status.textContent = !label ? "Enter a label of up to 240 characters and eight lines." : "Enter a valid binding key.";
        return;
    }
    let displayCodes;
    let button;
    let rules;
    let typography;
    try {
        if (model.hasDisplayCodes(selected.kind)) displayCodes = model.parseDisplayCodes(field("display-codes").value);
        button = model.buttonSettings(selected.kind, {
            action: field("action-key").value.trim(), trigger: field("trigger").value,
            holdMs: field("hold-ms").valueAsNumber, repeatDelay: field("repeat-delay").valueAsNumber,
            repeatMs: field("repeat-ms").valueAsNumber,
            minusAction: field("minus-action").value.trim(), plusAction: field("plus-action").value.trim(),
            chordAction: field("chord-action").value.trim(), chordMs: field("chord-ms").valueAsNumber
        });
        const key = field("label-key").value.trim();
        const source = field("label-source").value;
        const labelFrom = key ? {key, source, labels: source === "map" ? model.parseLabelMap(field("label-map").value) : undefined} : undefined;
        rules = model.ruleSettings({visibleWhen: field("visible-when").value, enabledWhen: field("enabled-when").value, labelFrom});
        typography = inspectTypography();
    } catch (error) {
        status.textContent = error.message;
        if (!displayCodes && model.hasDisplayCodes(selected.kind)) field("display-codes").focus();
        return;
    }
    const before = snapshot();
    setPlacements([selected], [{
        x: field("column").valueAsNumber - 1, y: field("row").valueAsNumber - 1,
        width: field("width").valueAsNumber, height: field("height").valueAsNumber
    }]);
    Object.assign(selected, {
        label, key, skin: field("skin").value,
        accent: field("use-accent").checked ? field("accent").value : ""
    });
    for (const name of ["action", "trigger", "holdMs", "repeatDelay", "repeatMs", "minusAction", "plusAction", "chordAction", "chordMs"])
        delete selected[name];
    for (const name of ["visibleWhen", "enabledWhen", "labelFrom"]) delete selected[name];
    Object.assign(selected, button, rules);
    if (typography) selected.typography = typography;
    else delete selected.typography;
    if (selected.kind === "image") selected.fit = field("fit").value;
    if (selected.kind === "line") {
        selected.direction = field("direction").value;
        selected.thickness = field("thickness").valueAsNumber;
    }
    if (displayCodes?.length) selected.displayCodes = displayCodes;
    else delete selected.displayCodes;
    if (selected.kind === "stepper" && field("segmented").checked) selected.segmented = true;
    else delete selected.segmented;
    paint(selected);
    select([selected]);
    record(before);
    status.textContent = "Control updated.";
};
themeForm.onchange = () => {
    const before = snapshot();
    theme = {...theme, ...Object.fromEntries(new FormData(themeForm))};
    record(before);
};
query("#reset-theme").onclick = () => {
    const before = snapshot();
    theme = {...theme, ...model.defaultTheme};
    record(before);
};

function restore(state) {
    ++layoutVersion;
    controls.length = 0;
    canvas.replaceChildren(backgroundImage);
    theme = state.theme;
    catalog = state.catalog;
    programs = state.programs;
    previewProgram = undefined;
    for (const data of state.layout) add(data, false);
    select(state.selection.map(index => controls[index]).filter(Boolean));
    refreshProject();
}
query("#layout-size").onchange = event => {
    const mode = event.target.value;
    grid = mode === "phone" ? phoneGrid : wideGrid;
    canvasFrame.scrollTop = 0;
    refreshCanvas();
    setPreviewSize(mode);
};
query("#auto-phone-position").onclick = () => {
    const before = snapshot();
    delete selected.phone;
    record(before);
};
query("#reset-phone-layout").onclick = () => {
    const before = snapshot();
    for (const control of controls) delete control.phone;
    record(before);
    status.textContent = "Phone layout reset to automatic placement.";
};
query("#showcase").onclick = () => {
    const before = snapshot();
    const project = model.readProject(model.serialize(model.showcase, model.defaultTheme, null));
    previewValues.clear();
    restore({layout: project.controls, theme: project.theme, catalog: null, programs: null, selection: [1]});
    record(before);
    togglePreview(true);
    status.textContent = "Historical controls showcase loaded.";
};
function stepHistory(source, destination) {
    if (!source.length) return;
    const focusedTile = canvas.contains(document.activeElement);
    destination.push(snapshot());
    restore(source.pop());
    if (focusedTile) (selection[0]?.tile ?? query("[data-kind]")).focus();
}
undoButton.onclick = () => stepHistory(undoStack, redoStack);
redoButton.onclick = () => stepHistory(redoStack, undoStack);
document.addEventListener("keydown", event => {
    if (event.defaultPrevented || event.altKey || !(event.ctrlKey || event.metaKey) ||
        isTextInput(event.target)) return;
    const key = event.key.toLowerCase();
    if (key !== "z" && key !== "y") return;
    event.preventDefault();
    (key === "y" || event.shiftKey ? redoButton : undoButton).click();
});

function download(content, type, filename) {
    const url = URL.createObjectURL(new Blob([content], {type}));
    const link = document.createElement("a");
    link.href = url;
    link.download = filename;
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
}
function bindFile(button, input, action) {
    button.onclick = () => input.click();
    input.onchange = async () => {
        const file = input.files[0];
        if (!file) return;
        button.disabled = true;
        try { await action(file); }
        catch (error) { status.textContent = error instanceof SyntaxError ? "This file is not valid JSON." : error.message; }
        finally { input.value = ""; button.disabled = false; }
    };
}
query("#save").onclick = () => {
    const text = documentText();
    if (encoder.encode(text).length > model.maxProjectBytes) {
        status.textContent = "This layout exceeds 2 MiB. Remove one or more artwork files before saving.";
        return;
    }
    download(text, "application/json", "cell-ui-layout.json");
    saved = text;
    query("#project-state").textContent = "";
    status.textContent = "Layout downloaded.";
};
bindFile(query("#open"), query("#layout-file"), async file => {
    if (file.size > model.maxProjectBytes) throw new Error("The layout file exceeds 2 MiB.");
    const project = model.readProject(await file.text());
    if (documentText() !== saved && !window.confirm("Replace the layout and discard unsaved changes?")) return;
    undoStack.length = redoStack.length = 0;
    previewValues.clear();
    restore({layout: project.controls, theme: project.theme,
        catalog: project.catalog === undefined ? catalog : project.catalog,
        programs: project.programs === undefined ? programs : project.programs, selection: [0]});
    saved = documentText();
    query("#project-state").textContent = "";
    status.textContent = "Layout opened.";
});

bindFile(query("#load-parameters"), query("#parameter-file"), async file => {
    if (file.size > 512 * 1024) throw new Error("The parameter snapshot exceeds 512 KiB.");
    const value = JSON.parse(await file.text());
    const incoming = model.readCatalog(value);
    const memory = model.readPrograms(value.programs);
    const before = snapshot();
    catalog = incoming;
    programs = memory;
    previewProgram = undefined;
    previewValues.clear();
    record(before);
    status.textContent = `Loaded ${catalog.length} parameters.`;
});
query("#clear-parameters").onclick = () => {
    const before = snapshot();
    catalog = null;
    programs = null;
    previewProgram = undefined;
    previewValues.clear();
    record(before);
};
async function readArtwork(file, panel) {
    const type = /\.svg$/i.test(file.name) && (!file.type || file.type === "application/octet-stream")
        ? "image/svg+xml" : file.type;
    const limit = panel ? model.maxPanelAssetBytes : model.maxAssetBytes;
    const dimension = panel ? 4096 : 2048;
    if (!model.imageType.test(type) || !file.size || file.size > limit)
        throw new Error(`Choose SVG, PNG, JPEG or WebP artwork up to ${panel ? "1 MiB" : "256 KiB"}.`);
    const artwork = await new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = () => resolve(reader.result);
        reader.onerror = () => reject(new Error("The artwork could not be read."));
        reader.readAsDataURL(type === file.type ? file : new Blob([file], {type}));
    });
    if (!model.validArtwork(artwork, limit))
        throw new Error("Choose valid artwork. SVG artwork must be self-contained and use static elements.");
    const image = new Image();
    image.src = artwork;
    try { await image.decode(); } catch { throw new Error("The artwork could not be decoded."); }
    if (type !== "image/svg+xml" && (image.naturalWidth > dimension || image.naturalHeight > dimension))
        throw new Error(`Raster artwork must be at most ${dimension} × ${dimension} pixels.`);
    return artwork;
}

bindFile(query("#load-artwork"), query("#artwork-file"), async file => {
    const control = selected;
    if (!control || !model.hasArtwork(control.kind)) return;
    const artwork = await readArtwork(file, control.kind === "image");
    if (!controls.includes(control)) return;
    const before = snapshot();
    const priorArtwork = control.artwork;
    control.artwork = artwork;
    if (encoder.encode(documentText()).length > model.maxProjectBytes) {
        control.artwork = priorArtwork;
        throw new Error("This artwork would take the layout over 2 MiB.");
    }
    paint(control);
    if (selected === control) select([control]);
    record(before);
    status.textContent = "Artwork embedded in the layout.";
});
query("#clear-artwork").onclick = () => {
    if (!selected) return;
    const before = snapshot();
    selected.artwork = "";
    paint(selected);
    select([selected]);
    record(before);
};

bindFile(query("#load-background"), query("#background-file"), async file => {
    const version = layoutVersion;
    const backgroundArtwork = await readArtwork(file, true);
    if (version !== layoutVersion) return;
    const next = {...theme, backgroundArtwork};
    if (encoder.encode(model.serialize(controlsOnly(), next, catalog, programs)).length > model.maxProjectBytes)
        throw new Error("This artwork would take the layout over 2 MiB.");
    const before = snapshot();
    theme = next;
    record(before);
    status.textContent = "Background embedded in the layout.";
});
backgroundFit.onchange = () => {
    const before = snapshot();
    theme = {...theme, backgroundFit: backgroundFit.value};
    record(before);
};
clearBackground.onclick = () => {
    const before = snapshot();
    const {backgroundArtwork, ...remaining} = theme;
    theme = remaining;
    record(before);
    status.textContent = "Background removed.";
};

function renderPreview() {
    previewFrame.srcdoc = CellDesignerRuntime.html(controlsOnly(), theme,
        {catalog, programs: programs && {...programs, active: previewProgram ?? programs.active},
            values: [...previewValues], generation: ++generation});
}

function togglePreview(show) {
    previewPanel.hidden = !show;
    previewButton.setAttribute("aria-expanded", String(show));
    query("#preview-action-status").hidden = true;
    if (show) renderPreview();
    else {
        previewFrame.removeAttribute("srcdoc");
        previewValues.clear();
        previewProgram = undefined;
    }
}
function setPreviewSize(size) {
    query("#preview-size").value = size;
    previewFrame.dataset.size = size;
}

previewButton.onclick = () => togglePreview(previewPanel.hidden);
query("#close-preview").onclick = () => { togglePreview(false); previewButton.focus(); };
query("#preview-size").onchange = event => setPreviewSize(event.target.value);
window.addEventListener("message", event => {
    const message = event.data;
    if (previewPanel.hidden || event.source !== previewFrame.contentWindow || message?.generation !== generation) return;
    if (message.type === "cell-preview-value" && Number.isFinite(message.value) &&
        controls.some(control => model.hasParameter(control.kind) && control.key === message.key))
        previewValues.set(message.key, message.value);
    if (message.type === "cell-preview-program" &&
        programs?.banks.find(bank => bank.number === message.bank)?.programs.some(program => program.number === message.program))
        previewProgram = {bank: message.bank, program: message.program};
    if (message.type === "cell-preview-action" && ["invoke", "press", "release"].includes(message.phase) &&
        model.actionBindings(controls).some(binding => binding.key === message.key)) {
        const action = query("#preview-action-status");
        action.hidden = false;
        action.textContent = `Simulated action ${message.key} (${message.phase})`;
    }
});

function exportIssues() {
    const issues = model.bindingIssues(controls, catalog, programs);
    if (model.parameterKeys(controls).length > 128) issues.push("Cell’s parameter bridge supports up to 128 distinct bindings");
    if (model.actionBindings(controls).length > 128) issues.push("Cell’s command bridge supports up to 128 action bindings");
    return issues;
}
query("#check-bindings").onclick = () => {
    const issues = exportIssues();
    status.textContent = issues.length ? issues.join(". ") + "." :
        !catalog ? "Load a parameter snapshot to verify parameter bindings." :
        !controls.length ? "Add a control to check its binding." : "All parameter bindings match the snapshot.";
};
function exportEditor(bindings) {
    if (!controls.length && (bindings || !theme.backgroundArtwork)) {
        status.textContent = bindings ? "Add a control before exporting bindings." : "Add a control or background image before exporting.";
        return;
    }
    if (model.parameterKeys(controls).length > 128) {
        status.textContent = "Reduce the layout to 128 distinct parameter bindings before exporting.";
        return;
    }
    if (model.actionBindings(controls).length > 128) {
        status.textContent = "Reduce the layout to 128 action bindings before exporting.";
        return;
    }
    const layout = controlsOnly();
    if (bindings) download(model.cppBindings(layout), "text/plain", "cell-ui-bindings.hpp");
    else download(CellDesignerRuntime.html(layout, theme), "text/html", "cell-plugin-editor.html");
    const issues = exportIssues();
    status.textContent = (bindings ? "C++ bindings exported." : "WebView HTML exported.") +
        (issues.length ? " " + issues.join(". ") + "." : "");
}
query("#export").onclick = () => exportEditor(false);
query("#export-cpp").onclick = () => exportEditor(true);
window.addEventListener("beforeunload", event => {
    if (documentText() === saved) return;
    event.preventDefault();
    event.returnValue = true;
});

select([]);
refreshProject();
