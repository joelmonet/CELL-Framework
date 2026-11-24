"use strict";

const model = CellDesignerModel;
const {wideGrid, phoneGrid, kinds, skins} = model;
const query = selector => document.querySelector(selector);
const canvas = query("#canvas");
const canvasFrame = query("#canvas-frame");
const form = query("#properties");
const field = name => form.elements.namedItem(name);
const themeForm = query("#theme");
const status = query("#status");
const undoButton = query("#undo");
const redoButton = query("#redo");
const copyButton = query("#copy-controls");
const pasteButton = query("#paste-controls");
const multiSelect = query("#multi-select");
const selectionTools = query("#selection-tools");
const previewPanel = query("#preview-panel");
const previewFrame = previewPanel.querySelector("iframe");
const previewButton = query("#preview");
const controls = [];
const phoneBoxes = new Map();
const undoStack = [], redoStack = [];
const previewValues = new Map();
const encoder = new TextEncoder();
const historyLimit = 64;
let theme = {...model.defaultTheme};
let catalog = null;
let selected;
let selection = [];
let localClipboard = "";
let preferLocalClipboard = false;
let grid = wideGrid;
let visibleRows = wideGrid.rows;
let generation = 0;
let saved = model.serialize([], theme, catalog);

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

function controlsOnly() {
    return controls.map(({tile, ...control}) => control);
}

function documentText() {
    return model.serialize(controlsOnly(), theme, catalog);
}

function refreshProject() {
    undoButton.disabled = !undoStack.length;
    redoButton.disabled = !redoStack.length;
    query("#project-state").textContent = documentText() === saved ? "" : "Unsaved changes";
    refreshCanvas();
    for (const [name, value] of Object.entries(theme)) {
        themeForm.elements.namedItem(name).value = value;
        canvas.style.setProperty("--" + name, value);
    }
    query("#parameter-keys").replaceChildren(...(catalog ?? []).map(({key, name}) => new Option(name, key)));
    query("#catalog-status").textContent = catalog ? `${catalog.length} parameters loaded` : "No parameter snapshot loaded";
    query("#clear-parameters").disabled = !catalog;
    if (!previewPanel.hidden) renderPreview();
}

function snapshot() {
    return {layout: controlsOnly(), theme, catalog, selection: selection.map(control => controls.indexOf(control))};
}

function record(before) {
    if (model.serialize(before.layout, before.theme, before.catalog) === documentText()) return;
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
    query("#artwork").hidden = !model.hasArtwork(control.kind);
    query("#clear-artwork").disabled = !control.artwork;
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
    query("#overlap-status").hidden = !boxes.some((box, i) => boxes.slice(i + 1).some(other => model.overlaps(box, other)));
    query("#reset-phone-layout").hidden = !phone;
    query("#reset-phone-layout").disabled = !controls.some(control => control.phone);
    query("#phone-help").hidden = !phone;
    if (selected) inspectPlacement(selected);
}

function paint(control) {
    const {tile, label, kind, skin, accent, artwork} = control;
    tile.dataset.skin = skin;
    tile.style.setProperty("--tile-accent", accent || "var(--accent)");
    tile.setAttribute("aria-label", `${label} · ${kind}`);
    tile.querySelector(".tile-label").textContent = label;
    const photo = tile.querySelector("img");
    if (artwork) photo.src = artwork;
    else photo.removeAttribute("src");
    photo.hidden = !artwork;
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
    if (kind === "toggle") parameter = choices.find(model.twoState);
    if (!parameter) parameter = choices.find(entry => entry.key === "gain") ?? choices[0];
    const key = model.hasParameter(kind) ? (parameter?.key ?? (kind === "toggle" ? "bypass" : "gain"))
        : kind === "meter" ? "output-peak" : "";
    const label = model.hasParameter(kind) ? ((parameter?.name || (key === "bypass" ? "Bypass" : "Gain")).slice(0, 80))
        : kind === "meter" ? "Output Peak" : title;
    const skin = {knob: "field", fader: "studio", meter: "broadcast"}[kind] ?? "modern";
    return {kind, key, label, width, height, skin, accent: "", artwork: ""};
}

function placeNew(data, point, phoneSize) {
    const widePoint = grid === wideGrid && point ? point : model.firstOpenPlace(data, controls);
    const result = {...data, ...widePoint};
    if (grid === phoneGrid) {
        const origin = point ?? model.firstOpenPlace(phoneSize, [...phoneBoxes.values()], phoneGrid);
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
    if (encoder.encode(model.serialize([...controlsOnly(), ...copies], theme, catalog)).length > model.maxProjectBytes) {
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
form.onsubmit = event => {
    event.preventDefault();
    if (!selected) return;
    const label = field("label").value.trim();
    const key = field("key").value.trim();
    if (!model.validText(label) || (model.hasBinding(selected.kind) && !model.validText(key))) {
        status.textContent = "Enter a label and a valid binding key.";
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
    paint(selected);
    select([selected]);
    record(before);
    status.textContent = "Control updated.";
};
themeForm.onchange = () => {
    const before = snapshot();
    theme = Object.fromEntries(new FormData(themeForm));
    record(before);
};
query("#reset-theme").onclick = () => {
    const before = snapshot();
    theme = {...model.defaultTheme};
    record(before);
};

function restore(state) {
    controls.length = 0;
    canvas.replaceChildren();
    theme = state.theme;
    catalog = state.catalog;
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
    restore({layout: project.controls, theme: project.theme, catalog: null, selection: [1]});
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
        catalog: project.catalog === undefined ? catalog : project.catalog, selection: [0]});
    saved = documentText();
    query("#project-state").textContent = "";
    status.textContent = "Layout opened.";
});

bindFile(query("#load-parameters"), query("#parameter-file"), async file => {
    if (file.size > 512 * 1024) throw new Error("The parameter snapshot exceeds 512 KiB.");
    const incoming = model.readCatalog(JSON.parse(await file.text()));
    const before = snapshot();
    catalog = incoming;
    previewValues.clear();
    record(before);
    status.textContent = `Loaded ${catalog.length} parameters.`;
});
query("#clear-parameters").onclick = () => {
    const before = snapshot();
    catalog = null;
    previewValues.clear();
    record(before);
};
bindFile(query("#load-artwork"), query("#artwork-file"), async file => {
    const control = selected;
    if (!control || !model.hasArtwork(control.kind)) return;
    if (!model.imageType.test(file.type) || file.size > model.maxAssetBytes)
        throw new Error("Choose PNG, JPEG or WebP artwork up to 256 KiB.");
    const artwork = await new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = () => resolve(reader.result);
        reader.onerror = () => reject(new Error("The artwork could not be read."));
        reader.readAsDataURL(file);
    });
    const image = new Image();
    image.src = artwork;
    try { await image.decode(); } catch { throw new Error("The artwork could not be decoded."); }
    if (image.naturalWidth > 2048 || image.naturalHeight > 2048) throw new Error("Artwork must be at most 2048 × 2048 pixels.");
    if (!controls.includes(control)) return;
    const before = snapshot();
    control.artwork = artwork;
    if (encoder.encode(documentText()).length > model.maxProjectBytes) {
        control.artwork = before.layout[controls.indexOf(control)].artwork;
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

function renderPreview() {
    previewFrame.srcdoc = CellDesignerRuntime.html(controlsOnly(), theme,
        {catalog, values: [...previewValues], generation: ++generation});
}

function togglePreview(show) {
    previewPanel.hidden = !show;
    previewButton.setAttribute("aria-expanded", String(show));
    if (show) renderPreview();
    else {
        previewFrame.removeAttribute("srcdoc");
        previewValues.clear();
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
    if (!previewPanel.hidden && event.source === previewFrame.contentWindow && message?.type === "cell-preview-value" &&
        message.generation === generation && Number.isFinite(message.value) &&
        controls.some(control => model.hasParameter(control.kind) && control.key === message.key))
        previewValues.set(message.key, message.value);
});

function exportIssues() {
    const issues = catalog ? model.bindingIssues(controls, catalog) : [];
    if (model.parameterKeys(controls).length > 128) issues.push("Cell’s parameter bridge supports up to 128 distinct bindings");
    return issues;
}
query("#check-bindings").onclick = () => {
    const issues = exportIssues();
    status.textContent = issues.length ? issues.join(". ") + "." :
        !catalog ? "Load a parameter snapshot to verify parameter bindings." :
        !controls.length ? "Add a control to check its binding." : "All parameter bindings match the snapshot.";
};
function exportEditor(bindings) {
    if (!controls.length) {
        status.textContent = "Add a control before exporting.";
        return;
    }
    if (model.parameterKeys(controls).length > 128) {
        status.textContent = "Reduce the layout to 128 distinct parameter bindings before exporting.";
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
