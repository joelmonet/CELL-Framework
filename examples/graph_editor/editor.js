// Original Cell Framework implementation; see LICENSE.
"use strict";

const byId = id => document.getElementById(id);
const waveforms = ["sine", "triangle", "square"];
const rateRanges = {hz: [.01, 100], beats: [.125, 128]};
const tempoLimits = {minimum: 1, maximum: 1000, count: 1024};
const lfoLimit = 16, routeLimit = 64;
const inRange = (value, minimum, maximum) => Number.isFinite(value) && value >= minimum && value <= maximum;
const targets = new Map([
    ["gain", ["gainDb", 72, "dB"]],
    ["saturator", ["driveDb", 36, "dB"]],
    ["pan", ["pan", 2, ""]],
    ["tremolo", ["depth", 1, ""]],
    ["azimuth", ["azimuth", 360, "deg"]],
]);
const automationRanges = new Map([
    ["gain", ["gainDb", -60, 12]],
    ["saturator", ["driveDb", 0, 36]],
    ["pan", ["pan", -1, 1]],
    ["tremolo", ["depth", 0, 1]],
    ["azimuth", ["azimuth", -180, 180]],
    ["lowpass", ["brightness", 0, 1]],
]);
const automationLimit = 32768;
const example = {
    schema: "cell.graph.v4", sampleRate: 48000,
    nodes: [{id: "level", type: "gain", gainDb: -6}, {id: "color", type: "saturator", driveDb: 12, oversampling: 4}],
    connections: [{from: "input", to: "color"}, {from: "color", to: "level"}, {from: "level", to: "output"}],
    automation: [], intents: [], tempoMap: [{sample: 0, bpm: 120}, {sample: 30000, bpm: 90}],
    lfos: [{id: "beat", beatsPerCycle: .5, waveform: "triangle", phase: .25, clock: "host"}],
    modulation: [{source: "beat", node: "level", parameter: "gainDb", depth: 4},
                 {source: "beat", node: "color", parameter: "driveDb", depth: -6}],
};
const historyLimit = 32;
const undoStack = [], redoStack = [];
let original, current, editingControl, graph, filename, selected = 0;
let maxWaveBytes = 0, selectedWave, renderedUrl, rendering = false, revision = 0;
let referenceUrl, activePreview, pendingSeek;
let maxProjectBytes = 0, openRequest = 0, savingProject = false;

function report(message, error = false) {
    byId("status").textContent = message;
    byId("status").toggleAttribute("data-error", error);
}

function refreshEditState() {
    const changed = original !== current;
    byId("undo").disabled = !undoStack.length;
    byId("redo").disabled = !redoStack.length;
    byId("revert").disabled = !changed;
    report(changed ? filename + " · Unsaved edits" : filename);
}

function markDirty(control) {
    const next = JSON.stringify(graph);
    if (next === current) return;
    if (!control || control !== editingControl) {
        undoStack.push(current);
        if (undoStack.length > historyLimit) undoStack.shift();
    }
    editingControl = control;
    redoStack.length = 0;
    current = next;
    refreshEditState();
    invalidateAudition();
}

function finishControlEdit(control) {
    if (editingControl !== control) return;
    editingControl = undefined;
    if (undoStack.at(-1) === current) {
        undoStack.pop();
        refreshEditState();
    }
}

function restoreGraph(snapshot) {
    const selection = graph.lfos[selected]?.id;
    graph = JSON.parse(snapshot);
    selected = Math.max(0, graph.lfos.findIndex(lfo => lfo.id === selection));
}

function moveInHistory(from, to) {
    if (!from.length) return;
    editingControl = undefined;
    to.push(current);
    current = from.pop();
    restoreGraph(current);
    render();
}

function load(data, name, embeddedWave) {
    if (data?.schema !== "cell.graph.v4" || !Array.isArray(data.lfos) ||
        !Array.isArray(data.modulation) || !Array.isArray(data.nodes) || !Array.isArray(data.automation)) {
        throw new Error("Choose a Cell v4 graph JSON file.");
    }
    if (!Number.isSafeInteger(data.sampleRate) || !inRange(data.sampleRate, 8000, 384000)) {
        throw new Error("The graph sample rate must be a whole number from 8,000 to 384,000 Hz.");
    }
    if ("tempoMap" in data) {
        const points = data.tempoMap;
        if (!Array.isArray(points) || points.length < 1 || points.length > tempoLimits.count ||
            points.some((point, index) => !Number.isSafeInteger(point?.sample) ||
                (index === 0 ? point.sample !== 0 : point.sample <= points[index - 1].sample) ||
                !inRange(point.bpm, tempoLimits.minimum, tempoLimits.maximum))) {
            throw new Error("The tempo map must start at sample zero and advance in sample order.");
        }
    }
    for (const lfo of data.lfos) {
        if (typeof lfo?.id !== "string" || !waveforms.includes(lfo.waveform ?? "sine") ||
            !Number.isFinite(lfo.phase ?? 0) || (lfo.phase ?? 0) < 0 || (lfo.phase ?? 0) > 1) {
            throw new Error("A graph LFO has invalid settings.");
        }
        const beats = "beatsPerCycle" in lfo;
        if (beats === ("rateHz" in lfo) || (beats && !data.tempoMap) ||
            ("clock" in lfo && (!beats || lfo.clock !== "host")) ||
            !inRange(beats ? lfo.beatsPerCycle : lfo.rateHz, ...rateRanges[beats ? "beats" : "hz"])) {
            throw new Error("An LFO has an invalid rate or clock.");
        }
    }
    for (const route of data.modulation) {
        if (!route || typeof route.source !== "string" || typeof route.node !== "string" ||
            typeof route.parameter !== "string" || !Number.isFinite(route.depth)) {
            throw new Error("A modulation route is incomplete.");
        }
    }
    clearReference();
    if (embeddedWave) {
        selectedWave = embeddedWave;
        byId("audio-name").textContent = embeddedWave.name;
    }
    original = current = JSON.stringify(data);
    undoStack.length = redoStack.length = 0;
    editingControl = undefined;
    graph = structuredClone(data);
    filename = name;
    selected = 0;
    timelineStart = 0;
    byId("timeline-range").value = "fit";
    byId("workspace").hidden = false;
    byId("save").disabled = false;
    render();
}

function drawWave(lfo) {
    const canvas = byId("preview");
    const bounds = canvas.getBoundingClientRect();
    const scale = window.devicePixelRatio || 1;
    canvas.width = Math.round(bounds.width * scale);
    canvas.height = Math.round(bounds.height * scale);
    const context = canvas.getContext("2d");
    if (!context || !canvas.width || !canvas.height) return;
    const {width, height} = canvas;
    context.strokeStyle = "#34463e";
    context.beginPath();
    context.moveTo(0, height / 2);
    context.lineTo(width, height / 2);
    context.stroke();
    context.strokeStyle = "#cef487";
    context.lineWidth = 2 * scale;
    context.beginPath();
    for (let pixel = 0; pixel <= width; pixel++) {
        const cycle = (pixel / width + (lfo.phase ?? 0)) % 1;
        let sample = Math.sin(2 * Math.PI * cycle);
        if (lfo.waveform === "triangle") sample = cycle < .25 ? 4 * cycle : cycle < .75 ? 4 * (.5 - cycle) : 4 * (cycle - 1);
        if (lfo.waveform === "square") sample = cycle < .5 ? 1 : -1;
        const y = height * (.5 - .38 * sample);
        if (pixel === 0) context.moveTo(pixel, y);
        else context.lineTo(pixel, y);
    }
    context.stroke();
}

function renderSources() {
    const sourceList = byId("sources");
    sourceList.replaceChildren();
    for (const [index, lfo] of graph.lfos.entries()) {
        const button = document.createElement("button");
        const rate = document.createElement("small");
        button.type = "button";
        button.setAttribute("aria-pressed", String(index === selected));
        button.textContent = lfo.id;
        rate.textContent = lfo.beatsPerCycle !== undefined ? lfo.beatsPerCycle + (lfo.clock === "host" ? " beats · Host" : " beats") : lfo.rateHz + " Hz";
        button.append(rate);
        button.onclick = () => {
            selected = index;
            renderSources();
            sourceList.children[index].focus();
        };
        sourceList.append(button);
    }
    byId("add-lfo").hidden = graph.lfos.length >= lfoLimit;
    const lfo = graph.lfos[selected];
    byId("source-detail").hidden = !lfo;
    byId("preview").hidden = !lfo;
    if (!lfo) {
        sourceList.textContent = "No LFOs in this graph.";
        return;
    }
    const beats = lfo.beatsPerCycle !== undefined;
    byId("lfo-clock").value = beats ? (lfo.clock === "host" ? "host" : "map") : "hz";
    byId("lfo-rate-label").textContent = beats ? "Beats per cycle" : "Rate in Hz";
    const rate = byId("lfo-rate");
    [rate.min, rate.max] = rateRanges[beats ? "beats" : "hz"];
    rate.value = beats ? lfo.beatsPerCycle : lfo.rateHz;
    byId("waveform").value = lfo.waveform ?? "sine";
    byId("phase").value = lfo.phase ?? 0;
    const count = graph.modulation.filter(route => route.source === lfo.id).length;
    byId("remove-lfo").textContent = count ? "Remove LFO and " + count + (count === 1 ? " route" : " routes") : "Remove LFO";
    drawWave(lfo);
}

function renderRoutes() {
    const routes = byId("routes");
    routes.replaceChildren();
    if (!graph.modulation.length) {
        routes.textContent = "No routes in this graph.";
        routes.classList.add("empty");
        return;
    }
    routes.classList.remove("empty");
    const nodeTypes = new Map(graph.nodes.filter(node => node && typeof node.id === "string").map(node => [node.id, node.type]));
    for (const route of graph.modulation) {
        const row = document.createElement("div");
        row.className = "route";
        const title = document.createElement("h3");
        title.textContent = route.source + " → " + route.node + " / " + route.parameter;
        const heading = document.createElement("div");
        const remove = document.createElement("button");
        heading.className = "route-title";
        remove.type = "button";
        remove.textContent = "Remove";
        remove.setAttribute("aria-label", "Remove route " + title.textContent);
        remove.onclick = () => {
            graph.modulation.splice(graph.modulation.indexOf(route), 1);
            renderSources();
            renderRoutes();
            renderRouteForm();
            byId("route-source").focus();
            markDirty();
        };
        heading.append(title, remove);
        row.append(heading);
        const spec = targets.get(nodeTypes.get(route.node));
        if (!spec || spec[0] !== route.parameter || Math.abs(route.depth) > spec[1]) {
            const unavailable = document.createElement("span");
            unavailable.className = "muted";
            unavailable.textContent = "Saved depth " + route.depth;
            row.append(unavailable);
        } else {
            const controls = document.createElement("div");
            controls.className = "route-controls";
            const slider = document.createElement("input");
            const numeric = document.createElement("input");
            const unit = document.createElement("span");
            slider.type = "range";
            numeric.type = "number";
            slider.step = "0.01";
            numeric.step = "any";
            for (const input of [slider, numeric]) {
                input.min = String(-spec[1]);
                input.max = String(spec[1]);
                input.value = String(route.depth);
                input.setAttribute("aria-label", title.textContent + (input === slider ? " depth slider" : " depth value"));
            }
            unit.className = "route-unit";
            unit.textContent = spec[2];
            slider.oninput = () => {
                route.depth = slider.valueAsNumber;
                numeric.value = String(route.depth);
                markDirty(slider);
            };
            slider.onchange = slider.onblur = () => finishControlEdit(slider);
            numeric.onchange = () => {
                if (!numeric.checkValidity() || !Number.isFinite(numeric.valueAsNumber)) {
                    numeric.value = String(route.depth);
                    report("Enter a depth between " + -spec[1] + " and " + spec[1] + ".", true);
                    return;
                }
                route.depth = numeric.valueAsNumber;
                slider.value = String(route.depth);
                markDirty();
            };
            controls.append(slider, numeric, unit);
            row.append(controls);
        }
        routes.append(row);
    }
}

function fillSelect(select, choices) {
    const previous = select.value;
    select.replaceChildren(...choices.map(([value, label]) => new Option(label, value)));
    if (choices.some(([value]) => value === previous)) select.value = previous;
}

function routeTargets(source) {
    const occupied = new Set(graph.modulation.filter(route => route.source === source).map(route => route.node));
    return graph.nodes.filter(node => node && typeof node.id === "string" && targets.has(node.type) && !occupied.has(node.id));
}

function updateRouteDepth() {
    const node = graph.nodes.find(candidate => candidate?.id === byId("route-target").value);
    const spec = targets.get(node?.type);
    const depth = byId("route-depth");
    depth.disabled = !spec;
    if (!spec) return;
    depth.min = -spec[1];
    depth.max = spec[1];
    if (!depth.checkValidity()) depth.value = 0;
}

function updateRouteTargets() {
    const nodes = routeTargets(byId("route-source").value);
    fillSelect(byId("route-target"), nodes.map(node => [node.id, node.id + " / " + targets.get(node.type)[0]]));
    updateRouteDepth();
}

function renderRouteForm() {
    const sources = graph.lfos.filter(lfo => routeTargets(lfo.id).length);
    byId("add-route").hidden = !sources.length || graph.modulation.length >= routeLimit;
    fillSelect(byId("route-source"), sources.map(lfo => [lfo.id, lfo.id]));
    updateRouteTargets();
}

byId("route-source").onchange = updateRouteTargets;
byId("route-target").onchange = updateRouteDepth;
byId("add-route").onsubmit = event => {
    event.preventDefault();
    const source = byId("route-source").value;
    const node = routeTargets(source).find(candidate => candidate.id === byId("route-target").value);
    const depth = byId("route-depth").valueAsNumber;
    if (!node || graph.modulation.length >= routeLimit || !graph.lfos.some(lfo => lfo.id === source)) return;
    const [parameter, maximum] = targets.get(node.type);
    if (!inRange(depth, -maximum, maximum)) return;
    graph.modulation.push({source, node: node.id, parameter, depth});
    renderSources();
    renderRoutes();
    renderRouteForm();
    markDirty();
};

byId("add-lfo").onsubmit = event => {
    event.preventDefault();
    const input = byId("lfo-id");
    const id = input.value.trim();
    if (!id || new TextEncoder().encode(id).length > 64 || graph.lfos.some(lfo => lfo.id === id)) {
        report("Enter a unique LFO ID of up to 64 UTF-8 bytes.", true);
        return;
    }
    if (graph.lfos.length >= lfoLimit) return;
    selected = graph.lfos.length;
    graph.lfos.push({id, rateHz: 1});
    input.value = "";
    renderSources();
    renderRouteForm();
    byId("sources").children[selected].focus();
    markDirty();
};

byId("remove-lfo").onclick = () => {
    const removed = graph.lfos[selected];
    graph.lfos.splice(selected, 1);
    graph.modulation = graph.modulation.filter(route => route.source !== removed.id);
    selected = Math.max(0, Math.min(selected, graph.lfos.length - 1));
    renderSources();
    renderRoutes();
    renderRouteForm();
    (byId("sources").children[selected] ?? byId("lfo-id")).focus();
    markDirty();
};

function renderTempo() {
    const points = graph.tempoMap ?? [];
    const list = byId("tempo-points");
    list.replaceChildren();
    byId("create-map").hidden = points.length !== 0;
    byId("add-tempo").hidden = points.length === 0 || points.length >= tempoLimits.count;
    for (const [index, point] of points.entries()) {
        const row = document.createElement("div");
        row.className = "tempo-row";
        const fields = [
            ["sample", "Sample", index === 0 ? 0 : 1, Number.MAX_SAFE_INTEGER, "1"],
            ["bpm", "BPM", tempoLimits.minimum, tempoLimits.maximum, "any"],
        ];
        for (const [key, label, min, max, step] of fields) {
            const caption = document.createElement("label");
            caption.textContent = label;
            const input = document.createElement("input");
            input.type = "number";
            input.min = min;
            input.max = max;
            input.step = step;
            input.value = point[key];
            input.disabled = key === "sample" && index === 0;
            input.setAttribute("aria-label", label + " for tempo point " + (index + 1));
            input.onchange = () => {
                const value = input.valueAsNumber;
                const invalidOrder = key === "sample" &&
                    (!Number.isSafeInteger(value) || value <= points[index - 1].sample ||
                     (points[index + 1] && value >= points[index + 1].sample));
                if (!input.checkValidity() || !Number.isFinite(value) || invalidOrder) {
                    input.value = point[key];
                    report(key === "sample" ? "Enter a whole sample number between adjacent tempo points." :
                        "Enter a tempo from 1 to 1,000 BPM.", true);
                    return;
                }
                point[key] = value;
                markDirty();
            };
            caption.append(input);
            row.append(caption);
        }
        if (index > 0) {
            const remove = document.createElement("button");
            remove.type = "button";
            remove.textContent = "Remove";
            remove.setAttribute("aria-label", "Remove tempo point " + (index + 1));
            remove.onclick = () => {
                points.splice(index, 1);
                renderTempo();
                byId("tempo-sample").focus();
                markDirty();
            };
            row.append(remove);
        }
        list.append(row);
    }
}

function automationTargets() {
    const choices = [];
    for (const node of graph.nodes) {
        const range = automationRanges.get(node?.type);
        if (!range || typeof node.id !== "string") continue;
        choices.push([node.id, ...range]);
        if (node.type === "azimuth" && "distanceMeters" in node)
            choices.push([node.id, "distanceMeters", 0, 100000]);
    }
    return choices;
}

function sortAutomation() {
    // Stable sort retains the existing order of points at the same sample.
    graph.automation.sort((a, b) => a.sample - b.sample);
}

let automationPlot, automationDrag, timelineStart = 0;
const timelinePadding = {left: 58, right: 18, top: 14, bottom: 28};

function timelineBounds() {
    const range = byId("timeline-range").value;
    if (range === "fit") {
        const end = Math.max(graph.sampleRate, automationPlot.points.at(-1)?.sample ?? 0);
        return {start: 0, end, span: end};
    }
    const span = Number(range) * graph.sampleRate;
    const start = Math.min(timelineStart, Number.MAX_SAFE_INTEGER - span);
    return {start, end: start + span, span};
}

function updateTimelineNavigation() {
    const fit = byId("timeline-range").value === "fit";
    byId("timeline-nav").hidden = fit;
    if (fit) return;
    const {start, span} = timelineBounds();
    const input = byId("timeline-start");
    input.max = Number.MAX_SAFE_INTEGER - span;
    input.value = start;
    byId("timeline-back").disabled = start === 0;
    byId("timeline-forward").disabled = start >= Number(input.max);
}

function changeTimelineStart(sample) {
    const {span} = timelineBounds();
    timelineStart = Math.max(0, Math.min(Number.MAX_SAFE_INTEGER - span, sample));
    refreshTimelineWindow();
}

function updatePointNavigation() {
    const points = automationPlot?.points;
    const sample = byId("automation-sample").valueAsNumber;
    const hasSample = Number.isSafeInteger(sample) && sample >= 0;
    byId("timeline-prev-point").disabled = !points?.length || (hasSample && points[0].sample >= sample);
    byId("timeline-next-point").disabled = !points?.length || (hasSample && points.at(-1).sample <= sample);
}

function jumpToAutomationPoint(forward) {
    const points = automationPlot.points;
    const input = byId("automation-sample");
    const sample = input.valueAsNumber;
    const hasSample = Number.isSafeInteger(sample) && sample >= 0;
    const point = forward
        ? points.find(point => !hasSample || point.sample > sample)
        : points.findLast(point => !hasSample || point.sample < sample);
    if (point) revealAutomationSample(point.sample);
}

function revealAutomationSample(sample) {
    byId("automation-sample").value = sample;
    updatePointNavigation();
    const {start, end, span} = timelineBounds();
    if (sample < start || sample > end) {
        changeTimelineStart(sample - Math.floor(span / 2));
    } else {
        drawAutomationTimeline();
    }
}

function timelineGeometry(canvas) {
    const rect = canvas.getBoundingClientRect();
    const {left, right, top, bottom} = timelinePadding;
    const plotWidth = rect.width - left - right;
    const plotHeight = rect.height - top - bottom;
    const {start, end, span} = timelineBounds();
    const {minimum, maximum} = automationPlot;
    return {rect, plotWidth, plotHeight, start, end, span,
        x: sample => left + (sample - start) / span * plotWidth,
        y: value => top + (maximum - value) / (maximum - minimum) * plotHeight};
}

function drawAutomationTimeline() {
    if (!automationPlot || byId("automation").hidden) return;
    const canvas = byId("automation-timeline");
    const {rect, plotWidth, plotHeight, start, end, x, y} = timelineGeometry(canvas);
    const {width, height} = rect;
    if (plotWidth <= 0 || plotHeight <= 0) return;
    const scale = window.devicePixelRatio || 1;
    canvas.width = Math.round(width * scale);
    canvas.height = Math.round(height * scale);
    const context = canvas.getContext("2d");
    if (!context) return;
    context.scale(scale, scale);

    const {minimum, maximum, points} = automationPlot;
    const {left, top} = timelinePadding;
    context.strokeStyle = "#34463e";
    context.fillStyle = "#a8bcb0";
    context.font = "11px system-ui";
    context.textAlign = "right";
    context.textBaseline = "middle";
    for (const value of [minimum, (minimum + maximum) / 2, maximum]) {
        const position = y(value);
        context.beginPath();
        context.moveTo(left, position);
        context.lineTo(left + plotWidth, position);
        context.stroke();
        context.fillText(String(value), left - 7, position);
    }

    context.textBaseline = "alphabetic";
    context.textAlign = "left";
    context.fillText(start.toLocaleString(), left, height - 7);
    context.textAlign = "right";
    context.fillText(end.toLocaleString() + " samples", left + plotWidth, height - 7);
    const selectedSample = byId("automation-sample").valueAsNumber;
    if (Number.isSafeInteger(selectedSample) && selectedSample >= start && selectedSample <= end) {
        const position = x(selectedSample);
        context.strokeStyle = "#a8bcb0";
        context.setLineDash([4, 4]);
        context.beginPath();
        context.moveTo(position, top);
        context.lineTo(position, top + plotHeight);
        context.stroke();
        context.setLineDash([]);
    }

    const previewPoints = automationDrag
        ? points.map(point => point === automationDrag.point ? automationDrag.preview : point)
            .sort((first, second) => first.sample - second.sample)
        : points;
    context.strokeStyle = "#91a89b";
    context.setLineDash([3, 3]);
    context.beginPath();
    for (let index = 1; index < previewPoints.length; ++index) {
        const first = previewPoints[index - 1];
        const second = previewPoints[index];
        if (second.sample < start) continue;
        if (first.sample > end) break;
        if (first.sample === second.sample) {
            context.moveTo(x(first.sample), y(first.value));
            context.lineTo(x(second.sample), y(second.value));
            continue;
        }
        const visibleStart = Math.max(start, first.sample);
        const visibleEnd = Math.min(end, second.sample);
        const valueAt = sample => first.value + (second.value - first.value) *
            ((sample - first.sample) / (second.sample - first.sample));
        context.moveTo(x(visibleStart), y(valueAt(visibleStart)));
        context.lineTo(x(visibleEnd), y(valueAt(visibleEnd)));
    }
    context.stroke();
    context.setLineDash([]);

    const dot = (point, radius) => {
        context.beginPath();
        context.arc(x(point.sample), y(point.value), radius, 0, 2 * Math.PI);
        context.fill();
    };
    context.fillStyle = "#cef487";
    for (const point of points) {
        if (point.sample > end) break;
        if (point.sample >= start && point !== automationDrag?.point) dot(point, 4);
    }
    if (automationDrag) dot(automationDrag.preview, 6);
}

const timeline = byId("automation-timeline");
timeline.onpointerdown = event => {
    if (!automationPlot || automationDrag || event.button !== 0) return;
    const view = timelineGeometry(timeline);
    if (view.plotWidth <= 0 || view.plotHeight <= 0) return;
    const x = event.clientX - view.rect.left, y = event.clientY - view.rect.top;
    let hit, radius = 10;
    for (const point of automationPlot.points) {
        if (point.sample > view.end) break;
        if (point.sample < view.start) continue;
        const distance = Math.hypot(view.x(point.sample) - x, view.y(point.value) - y);
        if (distance <= radius) {
            hit = point;
            radius = distance;
        }
    }
    if (!hit) {
        const relative = x - timelinePadding.left;
        if (relative < 0 || relative > view.plotWidth) return;
        byId("automation-sample").value = view.start + Math.round(relative / view.plotWidth * view.span);
        updatePointNavigation();
        drawAutomationTimeline();
        return;
    }
    automationDrag = {point: hit, preview: {...hit}, pointer: event.pointerId,
        x: event.clientX, y: event.clientY, view, moved: false};
    timeline.setPointerCapture(event.pointerId);
    drawAutomationTimeline();
};

function moveAutomationDrag(event) {
    const drag = automationDrag;
    if (!drag || drag.pointer !== event.pointerId) return;
    const dx = event.clientX - drag.x, dy = event.clientY - drag.y;
    if (!drag.moved && Math.hypot(dx, dy) < 3) return;
    drag.moved = true;
    const {point, preview, view} = drag;
    const {minimum, maximum} = automationPlot;
    const relative = point.sample - view.start + dx * view.span / view.plotWidth;
    const value = point.value - dy * (maximum - minimum) / view.plotHeight;
    preview.sample = view.start + Math.round(Math.max(0, Math.min(view.span, relative)));
    preview.value = dy ? Math.max(minimum, Math.min(maximum, Number(value.toFixed(4)))) : point.value;
    drawAutomationTimeline();
}

timeline.onpointermove = moveAutomationDrag;
timeline.onpointerup = event => {
    if (automationDrag?.pointer !== event.pointerId) return;
    moveAutomationDrag(event);
    const {point, preview} = automationDrag;
    automationDrag = undefined;
    if (point.sample === preview.sample && point.value === preview.value) {
        drawAutomationTimeline();
        return;
    }
    point.sample = preview.sample;
    point.value = preview.value;
    sortAutomation();
    renderAutomationPoints();
    markDirty();
};
timeline.onpointercancel = timeline.onlostpointercapture = event => {
    if (automationDrag?.pointer !== event.pointerId) return;
    automationDrag = undefined;
    drawAutomationTimeline();
};
byId("automation-sample").oninput = () => {
    updatePointNavigation();
    drawAutomationTimeline();
};
byId("timeline-prev-point").onclick = () => jumpToAutomationPoint(false);
byId("timeline-next-point").onclick = () => jumpToAutomationPoint(true);
function refreshTimelineWindow() {
    automationDrag = undefined;
    updateTimelineNavigation();
    drawAutomationTimeline();
    if (byId("automation-window-only").checked) renderAutomationList();
}

byId("timeline-range").onchange = refreshTimelineWindow;
byId("automation-window-only").onchange = renderAutomationList;
byId("timeline-start").onchange = event => {
    const sample = event.target.valueAsNumber;
    if (!Number.isSafeInteger(sample) || sample < 0) {
        updateTimelineNavigation();
        report("Enter a nonnegative whole sample number.", true);
        return;
    }
    changeTimelineStart(sample);
};
byId("timeline-back").onclick = () => {
    const {start, span} = timelineBounds();
    changeTimelineStart(start - span);
};
byId("timeline-forward").onclick = () => {
    const {end} = timelineBounds();
    changeTimelineStart(end);
};

function renderAutomationPoints() {
    automationDrag = undefined;
    const target = byId("automation-target").value;
    if (!target) {
        automationPlot = undefined;
        renderAutomationList();
        updatePointNavigation();
        return;
    }
    const [node, parameter] = JSON.parse(target);
    const valueField = byId("automation-value");
    const [minimum, maximum] = automationTargets().find(item => item[0] === node && item[1] === parameter).slice(2);
    valueField.min = minimum;
    valueField.max = maximum;
    if (!valueField.checkValidity())
        valueField.value = graph.nodes.find(item => item?.id === node)[parameter];

    const points = graph.automation.filter(point => point?.node === node && point.parameter === parameter);
    automationPlot = {points, minimum, maximum};
    updatePointNavigation();
    updateTimelineNavigation();
    drawAutomationTimeline();
    renderAutomationList();
}

function renderAutomationList() {
    const list = byId("automation-points");
    list.replaceChildren();
    const count = byId("automation-count");
    count.textContent = "";
    if (!automationPlot) return;
    const {points, minimum, maximum} = automationPlot;
    const windowOnly = byId("automation-window-only").checked;
    const {start, end} = timelineBounds();
    let visibleCount = 0;
    for (const [index, point] of points.entries()) {
        if (windowOnly && point.sample < start) continue;
        if (windowOnly && point.sample > end) break;
        ++visibleCount;
        const row = document.createElement("div");
        row.className = "tempo-row";
        for (const [key, title, min, max, step] of [
            ["sample", "Sample", 0, Number.MAX_SAFE_INTEGER, "1"],
            ["value", "Value", minimum, maximum, "any"],
        ]) {
            const label = document.createElement("label");
            const input = document.createElement("input");
            label.textContent = title;
            input.type = "number";
            input.min = min;
            input.max = max;
            input.step = step;
            input.value = point[key];
            input.setAttribute("aria-label", title + " for automation point " + (index + 1));
            input.onchange = () => {
                const value = input.valueAsNumber;
                if (!input.checkValidity() || !Number.isFinite(value) ||
                    (key === "sample" && !Number.isSafeInteger(value))) {
                    input.value = point[key];
                    report("Enter a " + title.toLowerCase() + " between " + min + " and " + max + ".", true);
                    return;
                }
                point[key] = value;
                if (key === "sample") {
                    sortAutomation();
                    renderAutomationPoints();
                } else {
                    drawAutomationTimeline();
                }
                markDirty();
            };
            label.append(input);
            row.append(label);
        }
        const actions = document.createElement("div");
        actions.className = "automation-point-actions";
        const locate = document.createElement("button");
        locate.type = "button";
        locate.textContent = "Locate";
        locate.setAttribute("aria-label", "Locate automation point " + (index + 1));
        locate.onclick = () => {
            revealAutomationSample(point.sample);
            timeline.focus({preventScroll: true});
            timeline.scrollIntoView({block: "nearest"});
        };
        const copy = document.createElement("button");
        copy.type = "button";
        copy.textContent = "Copy value";
        copy.setAttribute("aria-label", "Copy value from automation point " + (index + 1));
        copy.onclick = () => {
            byId("automation-value").value = point.value;
            byId("automation-sample").focus();
        };
        const remove = document.createElement("button");
        remove.type = "button";
        remove.textContent = "Remove";
        remove.setAttribute("aria-label", "Remove automation point " + (index + 1));
        remove.onclick = () => {
            graph.automation.splice(graph.automation.indexOf(point), 1);
            renderAutomationPoints();
            byId("automation-sample").focus();
            markDirty();
        };
        actions.append(locate, copy, remove);
        row.append(actions);
        list.append(row);
    }
    list.classList.toggle("empty", visibleCount === 0);
    if (!visibleCount) {
        list.textContent = points.length
            ? "No automation points in this timeline window."
            : "No automation points for this target.";
    }
    if (windowOnly) count.textContent = visibleCount + " of " + points.length + " points";
}

function renderAutomation() {
    const choices = automationTargets();
    byId("automation").hidden = !choices.length;
    fillSelect(byId("automation-target"), choices.map(([node, parameter]) =>
        [JSON.stringify([node, parameter]), node + " / " + parameter]));
    renderAutomationPoints();
}

byId("automation-target").onchange = () => {
    byId("automation-value").value = "";
    renderAutomationPoints();
};
byId("add-automation").onsubmit = event => {
    event.preventDefault();
    const [node, parameter] = JSON.parse(byId("automation-target").value);
    const input = byId("automation-sample");
    const sample = input.valueAsNumber;
    const value = byId("automation-value").valueAsNumber;
    if (!Number.isSafeInteger(sample) || !Number.isFinite(value)) return;
    const intentSizes = new Map((graph.intents ?? []).map(intent => [intent.id, intent.bindings.length]));
    const count = graph.automation.reduce((sum, point) => sum + (intentSizes.get(point?.intent) ?? 1), 0);
    if (count >= automationLimit) {
        report("This graph has reached the automation point limit.", true);
        return;
    }
    graph.automation.push({sample, node, parameter, value});
    sortAutomation();
    renderAutomationPoints();
    input.focus();
    markDirty();
};

function render(record = false) {
    renderSources();
    renderRoutes();
    renderRouteForm();
    renderTempo();
    renderAutomation();
    if (record) markDirty();
    else {
        refreshEditState();
        invalidateAudition();
    }
}

function ensureTempoMap() {
    graph.tempoMap ??= [{sample: 0, bpm: 120}];
}

byId("create-map").onclick = () => {
    ensureTempoMap();
    renderTempo();
    byId("tempo-points").querySelector('input:not([disabled])').focus();
    markDirty();
};
byId("add-tempo").onsubmit = event => {
    event.preventDefault();
    const input = byId("tempo-sample");
    const sample = input.valueAsNumber;
    const bpm = byId("tempo-bpm").valueAsNumber;
    const points = graph.tempoMap;
    if (!Number.isSafeInteger(sample) || sample < 1 || points.some(point => point.sample === sample)) {
        report("Choose a positive whole sample number that is not in the tempo map.", true);
        return;
    }
    if (points.length >= tempoLimits.count || !inRange(bpm, tempoLimits.minimum, tempoLimits.maximum)) return;
    points.push({sample, bpm});
    points.sort((a, b) => a.sample - b.sample);
    input.value = "";
    renderTempo();
    markDirty();
};

byId("lfo-clock").onchange = event => {
    const lfo = graph.lfos[selected];
    const clock = event.target.value;
    const bpm = graph.tempoMap?.[0].bpm ?? 120;
    if (clock === "hz") {
        const [minimum, maximum] = rateRanges.hz;
        lfo.rateHz = Math.max(minimum, Math.min(maximum, bpm / (60 * lfo.beatsPerCycle)));
        delete lfo.beatsPerCycle;
    } else {
        ensureTempoMap();
        if (lfo.beatsPerCycle === undefined) {
            const [minimum, maximum] = rateRanges.beats;
            lfo.beatsPerCycle = Math.max(minimum, Math.min(maximum, bpm / (60 * lfo.rateHz)));
            delete lfo.rateHz;
        }
    }
    if (clock === "host") lfo.clock = "host";
    else delete lfo.clock;
    renderSources();
    renderTempo();
    markDirty();
};

byId("lfo-rate").onchange = event => {
    const input = event.target;
    const lfo = graph.lfos[selected];
    const key = lfo.beatsPerCycle === undefined ? "rateHz" : "beatsPerCycle";
    if (!input.checkValidity() || !Number.isFinite(input.valueAsNumber)) {
        input.value = lfo[key];
        report("Enter a rate between " + input.min + " and " + input.max + ".", true);
        return;
    }
    lfo[key] = input.valueAsNumber;
    renderSources();
    markDirty();
};

byId("open").onclick = () => byId("file").click();
function parseGraphText(text) {
    const data = JSON.parse(text, (key, value) => {
        if (typeof value === "number" && !Number.isFinite(value))
            throw new Error("Graph numbers must be finite.");
        return value;
    });
    // Browser JSON numbers cannot round-trip sample indices beyond 2^53 − 1.
    for (const [token] of text.matchAll(/"(?:\\.|[^"\\])*"|-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?/g)) {
        if (/^-?\d+$/.test(token) && !Number.isSafeInteger(Number(token))) {
            throw new Error("This graph has large sample indices. Edit it with cell_graph_demo.");
        }
    }
    return data;
}

byId("example").onclick = () => {
    openRequest++;
    load(example, "tempo-modulation.json");
};
byId("file").onchange = async event => {
    const file = event.target.files[0];
    event.target.value = "";
    if (!file) return;
    const request = ++openRequest;
    try {
        if (/\.cellgraph$/i.test(file.name)) {
            if (!maxProjectBytes) throw new Error("Start the local Graph Editor service to open Cell projects.");
            if (file.size > maxProjectBytes) throw new Error("Choose a smaller Cell project.");
            report("Opening Cell project…");
            const response = await postBinary("open-project", file);
            const graphBytes = Number(response.headers.get("X-Cell-Graph-Bytes"));
            const snapshot = await response.blob();
            if (!Number.isSafeInteger(graphBytes) || graphBytes <= 0 || snapshot.size - graphBytes < 12)
                throw new Error("The Cell project response is incomplete.");
            const text = await snapshot.slice(0, graphBytes).text();
            if (request !== openRequest) return;
            const embedded = new File([snapshot.slice(graphBytes)], file.name.replace(/\.cellgraph$/i, ".wav"),
                {type: "audio/wav"});
            load(parseGraphText(text), file.name, embedded);
        } else {
            const text = await file.text();
            if (request === openRequest) load(parseGraphText(text), file.name);
        }
    } catch (error) {
        if (request === openRequest) report(error.message, true);
    }
};
byId("waveform").onchange = event => {
    graph.lfos[selected].waveform = event.target.value;
    drawWave(graph.lfos[selected]);
    markDirty();
};
byId("phase").onchange = event => {
    const lfo = graph.lfos[selected];
    if (!event.target.checkValidity() || !Number.isFinite(event.target.valueAsNumber)) {
        event.target.value = lfo.phase ?? 0;
        report("Enter a phase between 0 and 1 cycles.", true);
        return;
    }
    lfo.phase = event.target.valueAsNumber;
    drawWave(lfo);
    markDirty();
};
byId("undo").onclick = () => moveInHistory(undoStack, redoStack);
byId("redo").onclick = () => moveInHistory(redoStack, undoStack);
byId("revert").onclick = () => {
    restoreGraph(original);
    render(true);
};
document.addEventListener("keydown", event => {
    if (!graph || event.defaultPrevented || event.altKey || !(event.ctrlKey || event.metaKey)) return;
    if (event.target.closest?.("input, select, textarea, [contenteditable=true]")) return;
    const key = event.key.toLowerCase();
    if (key !== "z" && key !== "y") return;
    event.preventDefault();
    if (key === "y" || event.shiftKey) moveInHistory(redoStack, undoStack);
    else moveInHistory(undoStack, redoStack);
});
byId("save").onclick = () => {
    const url = URL.createObjectURL(new Blob([JSON.stringify(graph, null, 2) + "\n"], {type: "application/json"}));
    const link = document.createElement("a");
    link.href = url;
    link.download = filename.replace(/\.(json|cellgraph)$/i, "") + "-edited.json";
    link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
};
window.addEventListener("resize", () => {
    if (graph?.lfos[selected]) drawWave(graph.lfos[selected]);
    drawAutomationTimeline();
});


function auditionStatus(message, error = false) {
    const status = byId("audition-status");
    status.textContent = message;
    status.toggleAttribute("data-error", error);
}

function updateAudition() {
    byId("audition").hidden = !graph || !maxWaveBytes;
    byId("render-wave").disabled = !graph || !selectedWave || rendering || savingProject;
    byId("save-project").hidden = !maxProjectBytes;
    byId("save-project").disabled = !graph || !selectedWave || rendering || savingProject;
    byId("pin-a").disabled = !renderedUrl || renderedUrl === referenceUrl;
    byId("compare").hidden = !referenceUrl;
    byId("listen-b").disabled = !renderedUrl;
    for (const choice of ["a", "b"])
        byId("listen-" + choice).setAttribute("aria-pressed", String(activePreview === choice));
}

function clearPlayer() {
    const player = byId("player");
    activePreview = undefined;
    pendingSeek = undefined;
    player.onloadedmetadata = null;
    player.pause();
    player.removeAttribute("src");
    player.load();
    player.hidden = true;
    byId("download-wave").hidden = true;
}

function clearReference() {
    if (!referenceUrl) return;
    if (activePreview === "a") clearPlayer();
    if (referenceUrl !== renderedUrl) URL.revokeObjectURL(referenceUrl);
    referenceUrl = undefined;
}

function selectPreview(choice) {
    const url = choice === "a" ? referenceUrl : renderedUrl;
    if (!url) return;
    const player = byId("player");
    if (player.getAttribute("src") !== url) {
        const seek = pendingSeek ?? {time: player.currentTime, resume: !player.paused && !player.ended};
        pendingSeek = seek;
        player.pause();
        player.onloadedmetadata = () => {
            pendingSeek = undefined;
            player.onloadedmetadata = null;
            player.currentTime = Math.min(seek.time, player.duration);
            if (seek.resume && player.currentTime < player.duration) {
                player.play().catch(() => {
                    if (player.getAttribute("src") === url && player.paused)
                        auditionStatus("Press Play to resume the selected render.");
                });
            }
        };
        player.src = url;
    }
    activePreview = choice;
    player.hidden = false;
    const download = byId("download-wave");
    download.href = url;
    download.download = selectedWave.name.replace(/\.wav(e)?$/i, "") +
        (referenceUrl ? "-cell-" + choice.toUpperCase() : "-cell") + ".wav";
    download.hidden = false;
    updateAudition();
}

function invalidateAudition() {
    revision++;
    if (renderedUrl) {
        if (activePreview === "b") clearPlayer();
        if (renderedUrl !== referenceUrl) URL.revokeObjectURL(renderedUrl);
        renderedUrl = undefined;
    }
    auditionStatus(rendering ? "Settings changed. The current render will be discarded." :
        referenceUrl ? "A is pinned. Render the current graph to make B." :
        selectedWave ? "Ready to render the current graph." : "Choose a WAV at the graph’s sample rate.");
    updateAudition();
}

byId("pin-a").onclick = () => {
    if (activePreview === "a") selectPreview("b");
    clearReference();
    referenceUrl = renderedUrl;
    selectPreview("a");
    auditionStatus("A is pinned. Edit the graph, then render B.");
};
byId("listen-a").onclick = () => selectPreview("a");
byId("listen-b").onclick = () => selectPreview("b");

byId("choose-wave").onclick = () => byId("wave-file").click();
byId("wave-file").onchange = event => {
    const file = event.target.files[0];
    event.target.value = "";
    if (!file) return;
    if (file.size > maxWaveBytes) {
        auditionStatus("Choose a WAV up to " + maxWaveBytes / (1024 * 1024) + " MiB.", true);
        return;
    }
    clearReference();
    selectedWave = file;
    byId("audio-name").textContent = file.name;
    invalidateAudition();
};

async function postBinary(path, body, graphBytes) {
    const headers = {"Content-Type": "application/octet-stream"};
    if (graphBytes) headers["X-Cell-Graph-Bytes"] = String(graphBytes);
    const response = await fetch(path, {method: "POST", headers, body});
    if (!response.ok) {
        const failure = response.headers.get("Content-Type")?.includes("application/json") ?
            await response.json() : {};
        throw new Error(failure.error ?? "The Cell service could not complete the graph operation.");
    }
    return response;
}

function postGraphAndWave(path) {
    const data = new TextEncoder().encode(JSON.stringify(graph));
    return postBinary(path, new Blob([data, selectedWave]), data.length);
}

byId("save-project").onclick = async () => {
    const request = revision;
    const name = filename.replace(/\.(json|cellgraph)$/i, "");
    savingProject = true;
    updateAudition();
    report("Saving Cell project…");
    try {
        const response = await postGraphAndWave("save-project");
        const snapshot = await response.blob();
        if (request !== revision) return;
        const url = URL.createObjectURL(snapshot);
        const link = document.createElement("a");
        link.href = url;
        link.download = name + "-edited.cellgraph";
        link.click();
        setTimeout(() => URL.revokeObjectURL(url), 1000);
        report("Saved " + link.download);
    } catch (error) {
        if (request === revision) report(error.message, true);
    } finally {
        savingProject = false;
        updateAudition();
    }
};

byId("render-wave").onclick = async () => {
    invalidateAudition();
    const request = revision;
    const source = selectedWave;
    rendering = true;
    updateAudition();
    auditionStatus("Rendering through Cell…");
    try {
        const response = await postGraphAndWave("render");
        if (revision !== request) {
            await response.body?.cancel();
            return;
        }
        const audio = await response.blob();
        if (revision !== request) return;
        renderedUrl = URL.createObjectURL(audio);
        if (!referenceUrl || activePreview !== "a") selectPreview("b");
        auditionStatus(referenceUrl ? "B is ready. Switch between A and B to compare." :
            "Preview ready. Press Play to listen.");
    } catch (error) {
        if (revision === request) auditionStatus(error.message, true);
    } finally {
        rendering = false;
        if (revision !== request) invalidateAudition();
        else updateAudition();
    }
};

if (location.protocol !== "file:") {
    fetch("capabilities").then(response => response.ok ? response.json() : null).then(capabilities => {
        if (capabilities?.render && Number.isSafeInteger(capabilities.maximumWaveBytes)) {
            maxWaveBytes = capabilities.maximumWaveBytes;
            if (Number.isSafeInteger(capabilities.maximumProjectBytes)) {
                maxProjectBytes = capabilities.maximumProjectBytes;
                byId("file").accept += ",.cellgraph";
                byId("open").textContent = "Open graph or project";
            }
            updateAudition();
        }
    }).catch(() => {});
}
