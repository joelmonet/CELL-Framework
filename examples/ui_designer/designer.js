"use strict";

const model = CellDesignerModel;
const runtime = CellDesignerRuntime;
const {wideGrid, phoneGrid, kinds, skins} = model;
const query = selector => document.querySelector(selector);
const canvas = query("#canvas");
const canvasFrame = query("#canvas-frame");
const snapPlacement = query("#snap-placement");
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
const panelArtwork = artworkPicker(query("#panel-artwork-options"),
    asset => applyArtwork(() => setBackgroundArtwork(asset.artwork, asset.name + " applied.")));
const controlArtwork = artworkPicker(query("#control-artwork-options"), (asset, group) => {
    if (selected && artworkGroup(selected.kind) === group)
        applyArtwork(() => setControlArtwork(selected, asset.artwork, model.hasCapArtwork(selected.kind) ? asset.mode ?? "cap" : undefined,
            asset.name + " applied."));
});

const selectionTools = query("#selection-tools");
const layerElement = query("#layer-element");
const layerButtons = [...document.querySelectorAll("[data-layer-move]")];
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

function artworkGroup(kind) {
    if (model.hasCapArtwork(kind)) return kind;
    if (model.hasDisplayArtwork(kind)) return "display";
    if (model.hasSwitchArtwork(kind)) return "switch";
    if (kind === "indicator") return "indicator";
    return kind === "image" ? "panel" : "";
}

function artworkPicker(element, choose) {
    let previous;
    return (group, current) => {
        const assets = CellDesignerArtwork[group] ?? [];
        if (group !== previous) {
            previous = group;
            element.dataset.group = group;
            element.replaceChildren(...assets.map(asset => {
                const button = document.createElement("button");
                button.type = "button";
                const image = document.createElement("img");
                image.src = asset.artwork;
                image.alt = "";
                const label = document.createElement("span");
                label.textContent = asset.name;
                button.append(image, label);
                button.onclick = () => choose(asset, group);
                return button;
            }));
        }
        for (const [index, button] of Array.from(element.children).entries())
            button.setAttribute("aria-pressed", String(assets[index].artwork === current));
    };
}

function refreshControlArtwork(control) {
    const group = artworkGroup(control.kind);
    query("#control-artwork-library").hidden = !group;
    controlArtwork(group, control.artwork);
    query("#clear-artwork").disabled = !control.artwork;
    query("#artwork-placement-option").hidden = !control.artwork || !model.hasCapArtwork(control.kind);
    const mode = query("#artwork-placement");
    mode.querySelector('[value="pointer-cap"]').hidden = control.kind !== "knob";
    mode.value = control.artworkMode ?? "";
}

function applyArtwork(action) {
    try { action(); }
    catch (error) { status.textContent = error.message; }
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
    panelArtwork("panel", theme.backgroundArtwork);
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

function refreshBindingSource(control) {
    const meter = model.isMeterBinding(control);
    query("#binding-label").textContent = meter ? "Meter source" : "Parameter key";
    field("key").setAttribute("list", meter ? "meter-keys" : "parameter-keys");
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
    refreshLayers();
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
    field("rotation").value = control.rotation ?? "";
    query("#detent-settings").hidden = control.kind !== "knob";
    field("detent-label-mode").value = Array.isArray(control.detentLabels) ? "custom" : control.detentLabels ?? "none";
    field("detent-labels").value = Array.isArray(control.detentLabels) ? control.detentLabels.join("\n") : "";
    refreshDetentFields();
    query("#indicator-settings").hidden = control.kind !== "indicator";
    field("indicator-source").value = control.indicatorSource ?? "parameter";
    field("indicator-threshold").value = control.threshold ?? 1;
    refreshBindingSource(control);
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
    field("led-source").value = control.kind === "led" && !control.key ? "static" : control.ledText ? "idle" : "value";
    field("led-text").value = control.ledText ?? "";
    field("hide-led-heading").checked = control.hideHeading ?? false;
    field("display-codes").value = (control.displayCodes ?? []).map(({value, code}) => `${value} = ${code}`).join("\n");
    refreshDisplayFields();
    query("#skin-field").hidden = model.isPanelArt(control.kind);
    query("#image-fit").hidden = control.kind !== "image";
    field("fit").value = control.fit ?? "contain";
    query("#line-options").hidden = control.kind !== "line";
    field("direction").value = control.direction ?? "horizontal";
    field("thickness").value = control.thickness ?? 2;
    field("thickness").disabled = control.kind !== "line";
    query("#artwork").hidden = !model.hasArtwork(control.kind);
    refreshControlArtwork(control);
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

function refreshLayers() {
    const order = model.layerOrder(controls);
    order.forEach((control, index) => control.tile.style.setProperty("--element-layer", index + 1));
    const positions = new Map(controls.map((control, index) => [control, index]));
    layerElement.replaceChildren(...order.slice().reverse().map(control =>
        new Option(`${control.label} · ${kinds[control.kind].title}`, positions.get(control))));
    layerElement.disabled = order.length === 0;
    layerElement.value = positions.get(selected) ?? "";
    const index = order.indexOf(selected);
    for (const button of layerButtons) {
        const towardFront = ["front", "forward"].includes(button.dataset.layerMove);
        button.disabled = index < 0 || (towardFront ? index === order.length - 1 : index === 0);
    }
}

function numberLayers(order) {
    order.forEach((control, index) => { control.layer = index + 1; });
}

function moveLayer(command) {
    const layout = controlsOnly();
    const control = layout[controls.indexOf(selected)];
    const order = model.layerOrder(layout);
    const index = order.indexOf(control);
    const destination = {front: order.length - 1, forward: index + 1, backward: index - 1, back: 0}[command];
    if (index < 0 || destination === index || destination < 0 || destination >= order.length) return;
    order.splice(index, 1);
    order.splice(destination, 0, control);
    numberLayers(order);
    if (!fitsLayout(layout)) return;
    const before = snapshot();
    controls.forEach((item, i) => { item.layer = layout[i].layer; });
    refreshLayers();
    record(before);
    status.textContent = "Layer order updated.";
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

function refreshDisplayFields() {
    const kind = selected?.kind;
    const led = kind === "led";
    const segmented = kind === "stepper" && field("segmented").checked;
    const source = field("led-source").value;
    const staticLed = led && source === "static";
    const needsCharacters = led && source !== "value";
    const hasBinding = Boolean(kind && model.hasBinding(kind) && !staticLed);
    field("key").required = hasBinding;
    field("key").disabled = staticLed;
    query("#binding-field").hidden = !hasBinding;
    query("#led-source-setting").hidden = !led;
    query("#led-text-setting").hidden = !needsCharacters && !segmented;
    query("#led-heading-setting").hidden = !led && !segmented;
    field("led-text").required = needsCharacters;
    query("#bound-display-settings").hidden = staticLed;
    refreshDisplayChoices();
}

function refreshDetentFields() {
    const custom = selected?.kind === "knob" && field("detent-label-mode").value === "custom";
    query("#custom-detent-setting").hidden = query("#detent-help").hidden = !custom;
    field("detent-labels").disabled = !custom;
}

function refreshDisplayChoices() {
    const details = query("#display-choices");
    const parameter = selected && !field("key").disabled && model.hasDisplayCodes(selected.kind)
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
    field("column").value = Number((box.x + 1).toPrecision(15));
    field("row").value = Number((box.y + 1).toPrecision(15));
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
    visibleRows = phone ? Math.min(grid.rows, Math.max(8, Math.ceil(extent) + 3)) : grid.rows;
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
    tile.style.transform = control.rotation ? `rotate(${control.rotation}deg)` : "";
    if (kind === "line") {
        tile.dataset.direction = control.direction ?? "horizontal";
        tile.style.setProperty("--line-thickness", `${control.thickness ?? 2}px`);
    }
    tile.style.setProperty("--tile-accent", accent || "var(--accent)");
    tile.setAttribute("aria-label", `${label} · ${kind}`);
    tile.querySelector(".tile-label").textContent = label;
    runtime.paintTypography(tile, control.typography, model.typographyFamilies);
    const hardware = Boolean(artwork && (control.artworkMode || model.hasDisplayArtwork(kind) || model.hasSwitchArtwork(kind) || kind === "indicator"));
    tile.toggleAttribute("data-hardware-artwork", hardware);
    paintImage(tile.querySelector("img"), artwork, kind === "image" ? control.fit ?? "contain" : hardware ? "contain" : "cover");
}

function cellAt(event) {
    const rect = canvas.getBoundingClientRect();
    return {
        x: grid.columns * (event.clientX - rect.left - canvas.clientLeft) / canvas.clientWidth,
        y: visibleRows * (event.clientY - rect.top - canvas.clientTop) / canvas.clientHeight
    };
}

const finePosition = value => Math.round(value * 10) / 10;

function dragControl(control, startEvent) {
    if (startEvent.button !== 0 || !startEvent.isPrimary) return;
    startEvent.preventDefault();
    const {tile} = control;
    const pointer = startEvent.pointerId;
    const dragGrid = grid;
    const snap = snapPlacement.checked;
    const start = cellAt(startEvent);
    const extend = additive(startEvent);
    const items = selection.includes(control) ? selection : extend ? [...selection, control] : [control];
    const originals = items.map(placement);
    const original = originals[0];
    const rect = tile.getBoundingClientRect();
    const radians = (control.rotation ?? 0) * Math.PI / 180;
    const sine = Math.sin(radians);
    const cosine = Math.cos(radians);
    const localPoint = (x, y) => ({x: x * cosine + y * sine, y: y * cosine - x * sine});
    const corner = localPoint(startEvent.clientX - rect.left - rect.width / 2,
        startEvent.clientY - rect.top - rect.height / 2);
    const resizing = selected === control && !extend && items.length === 1 &&
        tile.offsetWidth / 2 - corner.x <= 17 && tile.offsetHeight / 2 - corner.y <= 17;
    let draft = null;
    tile.focus({preventScroll: true});
    tile.setPointerCapture(pointer);
    tile.onpointermove = event => {
        if (event.pointerId !== pointer || grid !== dragGrid) return;
        const next = cellAt(event);
        let dx = next.x - start.x, dy = next.y - start.y;
        if (resizing && control.rotation) {
            const delta = localPoint(event.clientX - startEvent.clientX, event.clientY - startEvent.clientY);
            dx = grid.columns * delta.x / canvas.clientWidth;
            dy = visibleRows * delta.y / canvas.clientHeight;
        }
        const quantize = resizing || snap ? Math.round : finePosition;
        dx = quantize(dx);
        dy = quantize(dy);
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
        width: Math.min(Math.floor(grid.columns - box.x), box.width + dx),
        height: Math.min(Math.floor(grid.rows - box.y), box.height + dy)
    }, grid);
}

function add(data) {
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
        const delta = snapPlacement.checked ? direction : direction.map(value => value / 10);
        setPlacements(items, event.shiftKey ? [resizedBox(boxes[0], ...direction)] : model.moveBoxes(boxes, grid, ...delta));
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
    if (["toggle", "momentary", "indicator"].includes(kind)) parameter = choices.find(model.twoState);
    if (kind === "stepper") parameter = choices.find(entry => entry.choices.length && model.stepCount(entry)) ??
        choices.find(entry => Number.isInteger(entry.minimum) && Number.isInteger(entry.step) && model.stepCount(entry)) ??
        choices.find(entry => model.stepCount(entry));
    if (!parameter && kind !== "stepper") parameter = choices.find(entry => entry.key === "gain") ?? choices[0];
    const key = model.hasParameter(kind) ? (parameter?.key ?? ({toggle: "bypass", indicator: "bypass", momentary: "gate", stepper: "code", led: "code"}[kind] ?? "gain"))
        : kind === "meter" ? "output-peak" : "";
    const label = model.hasParameter(kind) ? ((parameter?.name || ({bypass: "Bypass", gate: "Gate", code: "Code"}[key] ?? "Gain")).slice(0, 80))
        : kind === "meter" ? "Output Peak" : title;
    const skin = {knob: "field", fader: "studio", meter: "broadcast"}[kind] ?? "modern";
    const defaults = kind === "action" ? {action: "factory-recall", trigger: "tap"} :
        kind === "action-pair" ? {minusAction: "previous-patch", plusAction: "next-patch"} : {};
    const indicator = kind === "indicator" ? model.indicatorSettings({kind, threshold: parameter?.maximum}) : undefined;
    return {kind, key, label, width, height, skin, accent: "", artwork: "", ...defaults, ...indicator};
}

function placeNew(data, point, phoneSize) {
    const occupied = controls.filter(control => model.isPanelArt(control.kind) === model.isPanelArt(data.kind));
    const widePoint = grid === wideGrid && point ? point : model.firstOpenPlace(data, occupied);
    const result = {...data, ...widePoint};
    if (grid === phoneGrid) {
        const origin = point ?? model.firstOpenPlace(phoneSize, occupied.map(placement), phoneGrid);
        result.phone = model.constrain({...origin, ...phoneSize}, phoneGrid);
    }
    insertControls([result]);
}

function addControl(kind, point) {
    placeNew(starter(kind), point, model.phoneSize(kind));
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

function fitsLayout(layout) {
    if (encoder.encode(model.serialize(layout, theme, catalog, programs)).length <= model.maxProjectBytes) return true;
    status.textContent = "This edit would take the layout over 2 MiB.";
    return false;
}

function insertControls(copies) {
    if (controls.length + copies.length > model.maxControls) {
        status.textContent = `A layout can contain up to ${model.maxControls} controls.`;
        return false;
    }
    const layout = controlsOnly();
    const combined = [...layout, ...copies];
    const layered = combined.some(control => control.layer !== undefined);
    if (layered) numberLayers([...model.layerOrder(layout), ...model.layerOrder(copies)]);
    if (!fitsLayout(combined)) return false;
    const before = snapshot();
    if (layered) controls.forEach((control, index) => { control.layer = layout[index].layer; });
    const added = copies.map(add);
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
    const point = cellAt(event);
    const quantize = snapPlacement.checked ? Math.floor : finePosition;
    if (Object.hasOwn(kinds, kind)) addControl(kind, {x: quantize(point.x), y: quantize(point.y)});
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
layerElement.onchange = () => {
    if (layerElement.value !== "") select([controls[Number(layerElement.value)]]);
};
for (const button of layerButtons) button.onclick = () => {
    moveLayer(button.dataset.layerMove);
    if (button.disabled) layerElement.focus();
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
field("indicator-source").onchange = () => {
    if (selected?.kind !== "indicator") return;
    const source = field("indicator-source").value;
    const parameter = source === "parameter" ? catalog?.find(model.twoState) : undefined;
    field("key").value = source === "meter" ? "output-peak" : (parameter?.key ?? "bypass");
    field("indicator-threshold").value = model.indicatorSettings({
        kind: "indicator", indicatorSource: source, threshold: parameter?.maximum
    }).threshold;
    refreshBindingSource({kind: "indicator", indicatorSource: source});
};
field("use-accent").onchange = () => { field("accent").disabled = !field("use-accent").checked; };
field("key").oninput = refreshDisplayChoices;
field("detent-label-mode").onchange = refreshDetentFields;
field("led-source").onchange = field("segmented").onchange = refreshDisplayFields;
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
    const {kind} = selected;
    const key = field("key").required ? field("key").value.trim() : "";
    if (!label || (field("key").required && !model.validText(key))) {
        status.textContent = !label ? "Enter a label of up to 240 characters and eight lines." : "Enter a valid binding key.";
        return;
    }
    let displayCodes;
    let button;
    let rules;
    let typography;
    let indicator;
    let led;
    let detentLabels;
    let rotation;
    const segmented = kind === "stepper" && field("segmented").checked;
    const hasLed = kind === "led" || segmented;
    const hasLedText = segmented || (kind === "led" && field("led-source").value !== "value");
    try {
        rotation = model.readRotation(field("rotation").value === "" ? 0 : field("rotation").valueAsNumber);
        if (kind === "knob") {
            const mode = field("detent-label-mode").value;
            if (mode !== "none") detentLabels = model.readDetentLabels(mode === "choices"
                ? mode : field("detent-labels").value.split(/\r?\n/));
        }
        if (model.hasDisplayCodes(kind) && key) displayCodes = model.parseDisplayCodes(field("display-codes").value);
        const ledText = field("led-text").value;
        led = model.ledSettings({kind, key, segmented,
            ledText: hasLedText && (ledText || kind === "led") ? ledText : undefined,
            hideHeading: hasLed && field("hide-led-heading").checked ? true : undefined});
        button = model.buttonSettings(selected.kind, {
            action: field("action-key").value.trim(), trigger: field("trigger").value,
            holdMs: field("hold-ms").valueAsNumber, repeatDelay: field("repeat-delay").valueAsNumber,
            repeatMs: field("repeat-ms").valueAsNumber,
            minusAction: field("minus-action").value.trim(), plusAction: field("plus-action").value.trim(),
            chordAction: field("chord-action").value.trim(), chordMs: field("chord-ms").valueAsNumber
        });
        const labelKey = field("label-key").value.trim();
        const source = field("label-source").value;
        const labelFrom = labelKey ? {key: labelKey, source,
            labels: source === "map" ? model.parseLabelMap(field("label-map").value) : undefined} : undefined;
        rules = model.ruleSettings({visibleWhen: field("visible-when").value, enabledWhen: field("enabled-when").value, labelFrom});
        if (kind === "indicator") indicator = model.indicatorSettings({kind, indicatorSource: field("indicator-source").value,
            threshold: field("indicator-threshold").valueAsNumber});
        typography = inspectTypography();
    } catch (error) {
        status.textContent = error.message;
        if (!displayCodes && model.hasDisplayCodes(kind) && key) field("display-codes").focus();
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
    for (const name of ["visibleWhen", "enabledWhen", "labelFrom", "ledText", "hideHeading"]) delete selected[name];
    Object.assign(selected, button, rules, led, indicator);
    if (typography) selected.typography = typography;
    else delete selected.typography;
    if (selected.kind === "image") selected.fit = field("fit").value;
    if (selected.kind === "line") {
        selected.direction = field("direction").value;
        selected.thickness = field("thickness").valueAsNumber;
    }
    if (displayCodes?.length) selected.displayCodes = displayCodes;
    else delete selected.displayCodes;
    if (detentLabels) selected.detentLabels = detentLabels;
    else delete selected.detentLabels;
    if (rotation) selected.rotation = rotation;
    else delete selected.rotation;
    if (segmented) selected.segmented = true;
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
    for (const data of state.layout) add(data);
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
function loadShowcase(layout, colors, parameters, selection, message) {
    const before = snapshot();
    const project = model.readProject(model.serialize(layout, colors, parameters));
    previewValues.clear();
    restore({layout: project.controls, theme: project.theme, catalog: project.catalog, programs: null, selection});
    record(before);
    togglePreview(true);
    status.textContent = message;
}
query("#showcase").onclick = () => loadShowcase(model.showcase, model.defaultTheme, null, [1],
    "Historical controls showcase loaded.");
const hardwareStyles = {
    studio: {label: "Studio hardware", background: "#1d272e", surface: "#263039", ink: "#eef4ef", lamp: "#ffd38a"},
    british: {label: "British broadcast", background: "#182733", surface: "#243b4a", ink: "#eff6fa", lamp: "#f5dbad"},
    soviet: {label: "Soviet-era laboratory", background: "#282d24", surface: "#414638", ink: "#f1edd2", lamp: "#ffbf79"},
    german: {label: "German precision", background: "#252b2e", surface: "#363f44", ink: "#f1f4f4", lamp: "#a8e6f3"},
    field: {label: "Field communications", background: "#252b20", surface: "#363e2e", ink: "#f1e8ce", lamp: "#fae49e"}
};
const hardwareStyle = query("#hardware-style");
for (const [key, {label}] of Object.entries(hardwareStyles)) hardwareStyle.add(new Option(label, key));
hardwareStyle.add(new Option("Field indicators", "field-indicators"));

function loadFieldIndicators() {
    const switches = CellDesignerArtwork.switch;
    const [ruby, amber, green, red] = CellDesignerArtwork.indicator;
    const backgroundArtwork = CellDesignerArtwork.panel.find(asset => asset.family === "field").artwork;
    const parameters = model.readCatalog({type: "parameters", parameters: [
        {key: "monitor", name: "Monitor", minimum: 0, maximum: 1, step: 1, value: 1, unit: ""},
        {key: "audition", name: "Audition", minimum: 0, maximum: 1, step: 1, value: 0, unit: ""},
        {key: "effect-trigger", name: "Effect trigger", minimum: 0, maximum: 1, step: 1, value: 0, unit: ""}
    ]});
    const lamp = (label, key, x, asset, accent, indicatorSource, threshold) =>
        ({kind: "indicator", label, key, x, y: 0, width: 3, height: 2, artwork: asset.artwork,
            accent, indicatorSource, threshold});
    const button = (kind, label, key, x, name, accent) =>
        ({kind, label, key, x, y: 2, width: 4, height: 2, accent,
            artwork: switches.find(asset => asset.name === name).artwork});
    loadShowcase([
        lamp("READY", "monitor", 0, green, "#b6e56a", "parameter", 1),
        lamp("CAUTION", "output-peak", 3, amber, "#ffbd56", "meter", -23),
        lamp("PEAK WARNING", "output-peak", 6, ruby, "#ff6651", "meter", -15),
        lamp("EFFECT ACTIVE", "effect-trigger", 9, red, "#ff795b", "parameter", 1),
        button("toggle", "MONITOR", "monitor", 0, "Ivory console pushbutton", "#e8d7af"),
        button("momentary", "LAUNCH", "audition", 4, "Guarded crimson pushbutton", "#ff8765"),
        button("momentary", "DETONATE", "effect-trigger", 8, "Red rectangular pushbutton", "#ff735b")
    ], {...model.defaultTheme, background: "#252b20", surface: "#303627", text: "#f1e8ce", backgroundArtwork},
    parameters, [0], "Field indicators showcase loaded.");
}

query("#hardware-showcase").onclick = () => {
    const family = hardwareStyle.value;
    if (family === "field-indicators") {
        loadFieldIndicators();
        return;
    }
    const {label, background, surface, ink, lamp} = hardwareStyles[family];
    const pick = (group, fallback = 0) =>
        CellDesignerArtwork[group].find(asset => asset.family === family) ?? CellDesignerArtwork[group][fallback];
    const panel = pick("panel");
    const knob = pick("knob", 1);
    const fader = pick("fader", 1);
    const display = pick("display");
    const parameters = model.readCatalog({type: "parameters", parameters: [
        {key: "gain", name: "Input trim", minimum: -60, maximum: 12, step: 0.01, value: -6, unit: "dB"},
        {key: "level", name: "Output level", minimum: -60, maximum: 12, step: 0.01, value: -12, unit: "dB"},
        {key: "mode", name: "Mode", minimum: 0, maximum: 2, step: 1, value: 1, unit: "",
            choices: ["Clean", "Warm", "Bright"]},
        {key: "bypass", name: "Bypass", minimum: 0, maximum: 1, step: 1, value: 0, unit: ""}
    ]});
    const control = (kind, label, key, [x, y, width, height], asset, options) => ({
        kind, label, key, x, y, width, height, artwork: asset.artwork, ...options
    });
    const green = "#a0f5bb";
    loadShowcase([
        control("knob", "Input trim", "gain", [0, 0, 4, 3], knob, {artworkMode: "cap", accent: lamp}),
        control("readout", "Gain", "gain", [0, 3, 2, 1], display, {accent: lamp}),
        control("fader", "Output level", "level", [4, 0, 4, 4], fader, {artworkMode: "cap", accent: lamp}),
        control("stepper", "Mode", "mode", [8, 0, 4, 2], family === "studio" ? pick("display", 2) : display,
            {segmented: true, accent: green, displayCodes: [{value: 0, code: "CL"}, {value: 1, code: "A1"}, {value: 2, code: "br"}]}),
        control("meter", "Output peak", "output-peak", [8, 2, 4, 2], family === "studio" ? pick("display", 1) : display,
            {accent: green}),
        control("toggle", "Bypass", "bypass", [2, 3, 2, 1], pick("switch"), {accent: lamp})
    ], {...model.defaultTheme, background, surface, text: ink, backgroundArtwork: panel.artwork},
    parameters, [0], label + " showcase loaded.");
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

function setControlArtwork(control, artwork, artworkMode, message) {
    const before = snapshot();
    const priorArtwork = control.artwork;
    const priorMode = control.artworkMode;
    Object.assign(control, {artwork, artworkMode});
    if (encoder.encode(documentText()).length > model.maxProjectBytes) {
        Object.assign(control, {artwork: priorArtwork, artworkMode: priorMode});
        throw new Error("This artwork would take the layout over 2 MiB.");
    }
    paint(control);
    if (selected === control) refreshControlArtwork(control);
    record(before);
    status.textContent = message;
}

function setBackgroundArtwork(backgroundArtwork, message) {
    const next = {...theme};
    if (backgroundArtwork) next.backgroundArtwork = backgroundArtwork;
    else delete next.backgroundArtwork;
    if (encoder.encode(model.serialize(controlsOnly(), next, catalog, programs)).length > model.maxProjectBytes)
        throw new Error("This artwork would take the layout over 2 MiB.");
    const before = snapshot();
    theme = next;
    record(before);
    status.textContent = message;
}

bindFile(query("#load-artwork"), query("#artwork-file"), async file => {
    const control = selected;
    if (!control || !model.hasArtwork(control.kind)) return;
    const artwork = await readArtwork(file, control.kind === "image");
    if (!controls.includes(control)) return;
    setControlArtwork(control, artwork, control.artworkMode, "Artwork embedded in the layout.");
});
query("#artwork-placement").onchange = event => {
    if (selected?.artwork && model.hasCapArtwork(selected.kind))
        applyArtwork(() => setControlArtwork(selected, selected.artwork, event.target.value || undefined, "Artwork placement updated."));
};
query("#clear-artwork").onclick = () => {
    if (selected) setControlArtwork(selected, "", undefined, "Artwork removed.");
};

bindFile(query("#load-background"), query("#background-file"), async file => {
    const version = layoutVersion;
    const artwork = await readArtwork(file, true);
    if (version === layoutVersion) setBackgroundArtwork(artwork, "Background embedded in the layout.");
});
backgroundFit.onchange = () => {
    const before = snapshot();
    theme = {...theme, backgroundFit: backgroundFit.value};
    record(before);
};
clearBackground.onclick = () => setBackgroundArtwork("", "Background removed.");

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
        controls.some(control => model.hasParameterBinding(control) && control.key === message.key))
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
