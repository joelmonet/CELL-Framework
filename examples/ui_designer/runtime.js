"use strict";

const CellDesignerRuntime = (() => {
    // Serialized into the standalone document along with the selected controls.
    function mount(controls, phoneBreakpoint) {
        const stage = document.querySelector("main");
        const items = [];
        const bindings = new Map();
        const meters = new Map();
        const parameters = new Map();
        const owners = new Map();
        const stopDrags = [];
        const bridge = window.cell;
        const format = (parameter, value) => {
            const index = value - parameter.minimum;
            const digits = parameter.step > 0 && parameter.step < 0.01 ? 4 : 2;
            return parameter.choices?.[index] ?? `${value.toFixed(digits)}${parameter.unit ? " " + parameter.unit : ""}`;
        };
        const twoState = parameter => parameter.maximum > parameter.minimum && parameter.step > 0 &&
            Math.abs(parameter.minimum + parameter.step - parameter.maximum) <= Math.max(1e-9, parameter.step * 1e-6) &&
            (!parameter.choices?.length || parameter.choices.length === 2);
        const send = (type, key, value) => bridge.postMessage({type, key, value});

        function end(binding) {
            if (owners.get(binding.key) !== binding) return;
            owners.delete(binding.key);
            send("end", binding.key);
            if (binding.deferred) {
                const deferred = binding.deferred;
                binding.deferred = undefined;
                update(binding, deferred);
            }
        }

        function write(binding, value) {
            const previous = owners.get(binding.key);
            if (previous !== binding) {
                if (previous) end(previous);
                owners.set(binding.key, binding);
                send("begin", binding.key);
            }
            // The active control shows its edit until the host echoes or corrects it.
            binding.deferred = undefined;
            send("parameter", binding.key, value);
            display(binding, parameters.get(binding.key), value);
        }

        function display(binding, parameter, value) {
            const {control, output, input, dial} = binding;
            output.textContent = format(parameter, value);
            if (input?.type === "range") input.setAttribute("aria-valuetext", output.textContent);
            if (control.kind === "toggle")
                input.setAttribute("aria-pressed", String(value === parameter.maximum));
            if (dial) {
                const span = parameter.maximum - parameter.minimum;
                const proportion = span > 0 ? (value - parameter.minimum) / span : 0;
                dial.style.setProperty("--angle", `${-135 + Math.min(Math.max(proportion, 0), 1) * 270}deg`);
            }
        }

        function update(binding, parameter) {
            const {input, control} = binding;
            if (input) {
                if (control.kind === "choice") {
                    const choices = (parameter.choices ?? []).slice(0, Math.max(0, Math.floor(parameter.maximum - parameter.minimum) + 1));
                    const signature = JSON.stringify([parameter.minimum, choices]);
                    if (binding.signature !== signature) {
                        input.replaceChildren(...choices.map((label, i) => new Option(label, parameter.minimum + i)));
                        binding.signature = signature;
                    }
                    input.disabled = !choices.length;
                    input.value = parameter.value;
                } else if (control.kind === "toggle") {
                    input.disabled = !twoState(parameter);
                } else {
                    input.min = parameter.minimum;
                    input.max = parameter.maximum;
                    input.step = parameter.step > 0 ? parameter.step : "any";
                    input.value = parameter.value;
                    input.disabled = parameter.minimum >= parameter.maximum;
                }
            }
            display(binding, parameter, parameter.value);
        }

        function rangeInput(binding, face) {
            const {input, key, control} = binding;
            input.type = "range";
            input.step = "any";
            input.oninput = () => write(binding, input.valueAsNumber);
            input.onchange = input.onblur = input.onpointercancel = input.onlostpointercapture = () => end(binding);
            if (control.kind === "fader") input.setAttribute("aria-orientation", "vertical");
            if (control.kind !== "knob") return;

            let drag;
            input.onpointerdown = event => {
                if (input.disabled || event.button !== 0 || !event.isPrimary) return;
                event.preventDefault();
                input.focus({preventScroll: true});
                input.setPointerCapture(event.pointerId);
                drag = {id: event.pointerId, x: event.clientX, y: event.clientY, value: input.valueAsNumber};
                face.classList.add("dragging");
            };
            input.onpointermove = event => {
                const parameter = parameters.get(key);
                if (!parameter || drag?.id !== event.pointerId) return;
                const scale = (parameter.maximum - parameter.minimum) / (event.shiftKey ? 1000 : 180);
                const next = drag.value + (drag.y - event.clientY + event.clientX - drag.x) * scale;
                const before = input.valueAsNumber;
                input.value = next;
                if (input.valueAsNumber !== before) write(binding, input.valueAsNumber);
            };
            const stop = () => {
                if (!drag) return;
                const {id} = drag;
                drag = undefined;
                face.classList.remove("dragging");
                if (input.hasPointerCapture(id)) input.releasePointerCapture(id);
                end(binding);
            };
            const finish = event => { if (drag?.id === event.pointerId) stop(); };
            input.onpointerup = input.onpointercancel = input.onlostpointercapture = finish;
            stopDrags.push(stop);
        }

        for (const control of controls) {
            const item = document.createElement("div");
            item.className = "control " + control.kind;
            item.dataset.skin = control.skin;
            if (control.accent) item.style.setProperty("--accent", control.accent);
            const title = document.createElement("span");
            title.className = "control-title";
            title.textContent = control.label;
            item.append(title);
            items.push({control, item});
            if (control.kind === "text") continue;

            const output = document.createElement("output");
            output.textContent = "—";
            output.setAttribute("aria-label", control.label + " value");
            output.setAttribute("aria-live", "off");
            item.append(output);
            const {key} = control;
            if (control.kind === "meter") {
                const face = document.createElement("div");
                face.className = "meter-face";
                const scale = document.createElement("div");
                scale.className = "meter-scale";
                scale.setAttribute("aria-hidden", "true");
                const marks = [document.createElement("span"), document.createElement("span"), document.createElement("span")];
                scale.append(...marks);
                const gauge = document.createElement("meter");
                gauge.min = -60;
                gauge.max = 0;
                gauge.value = -60;
                gauge.setAttribute("aria-label", control.label);
                face.append(scale, gauge);
                item.append(face);
                if (!meters.has(key)) meters.set(key, []);
                meters.get(key).push({output, gauge, face, marks});
                continue;
            }
            const binding = {key, control, output};
            if (!bindings.has(key)) bindings.set(key, []);
            bindings.get(key).push(binding);
            if (!bridge || control.kind === "readout") continue;

            const face = document.createElement("div");
            face.className = "face";
            item.append(face);
            if (control.artwork) {
                const image = document.createElement("img");
                image.className = "artwork";
                image.src = control.artwork;
                image.alt = "";
                face.append(image);
            }
            if (control.kind === "knob") {
                binding.dial = document.createElement("div");
                binding.dial.className = "dial";
                binding.dial.setAttribute("aria-hidden", "true");
                face.append(binding.dial);
            }
            const input = document.createElement(control.kind === "choice" ? "select" : control.kind === "toggle" ? "button" : "input");
            binding.input = input;
            input.setAttribute("aria-label", control.label);
            input.disabled = true;
            face.append(input);
            if (control.kind === "choice") {
                input.onchange = () => {
                    if (input.selectedIndex < 0) return;
                    write(binding, Number(input.value));
                    end(binding);
                };
            } else if (control.kind === "toggle") {
                input.type = "button";
                input.textContent = "Toggle";
                input.setAttribute("aria-pressed", "false");
                input.onclick = () => {
                    const parameter = parameters.get(key);
                    if (!parameter || input.disabled) return;
                    write(binding, parameter.value === parameter.maximum ? parameter.minimum : parameter.maximum);
                    end(binding);
                };
            } else {
                rangeInput(binding, face);
            }
        }

        const cancel = () => {
            stopDrags.forEach(stop => stop());
            for (const binding of owners.values()) end(binding);
        };
        const compact = window.matchMedia(`(max-width: ${phoneBreakpoint}px)`);
        function arrange() {
            const focused = stage.contains(document.activeElement) ? document.activeElement : null;
            cancel();
            stage.dataset.layout = compact.matches ? "phone" : "wide";
            const placed = items.map(({control, item}) => ({
                box: compact.matches ? control.phone : control,
                item
            }));
            placed.sort((a, b) => a.box.y - b.box.y || a.box.x - b.box.x);
            for (const {box, item} of placed) {
                item.style.gridColumn = `${box.x + 1} / span ${box.width}`;
                item.style.gridRow = `${box.y + 1} / span ${box.height}`;
            }
            // Keep keyboard and screen-reader order in sync with the visible grid.
            stage.append(...placed.map(({item}) => item));
            if (focused) focused.focus({preventScroll: true});
        }
        arrange();
        compact.addEventListener("change", arrange);
        if (!bridge) return;
        window.addEventListener("blur", cancel);
        window.addEventListener("pagehide", cancel);
        document.addEventListener("visibilitychange", () => { if (document.hidden) cancel(); });
        bridge.onMessage(message => {
            if (message.type === "meters" && Array.isArray(message.meters)) {
                for (const signal of message.meters) {
                    if (!signal || ![signal.value, signal.minimum, signal.maximum].every(Number.isFinite) ||
                        signal.minimum >= signal.maximum) continue;
                    const value = Math.min(Math.max(signal.value, signal.minimum), signal.maximum);
                    for (const {output, gauge, face, marks} of meters.get(signal.key) ?? []) {
                        gauge.min = signal.minimum;
                        gauge.max = signal.maximum;
                        gauge.value = value;
                        output.textContent = `${value.toFixed(1)}${signal.unit ? " " + signal.unit : ""}`;
                        const levels = [signal.minimum, (signal.minimum + signal.maximum) / 2, signal.maximum];
                        marks.forEach((mark, i) => { mark.textContent = String(Math.round(levels[i])); });
                        face.style.setProperty("--needle", `${-50 + 100 * (value - signal.minimum) / (signal.maximum - signal.minimum)}deg`);
                    }
                }
                return;
            }
            if (message.type !== "parameters" || !Array.isArray(message.parameters)) return;
            parameters.clear();
            for (const parameter of message.parameters) {
                if (parameter && [parameter.value, parameter.minimum, parameter.maximum].every(Number.isFinite))
                    parameters.set(parameter.key, parameter);
            }
            for (const [key, items] of bindings) {
                const parameter = parameters.get(key);
                for (const binding of items) {
                    if (!parameter) {
                        if (binding.input) binding.input.disabled = true;
                        binding.output.textContent = "—";
                    } else if (owners.get(key) === binding) {
                        binding.deferred = parameter;
                    } else {
                        update(binding, parameter);
                    }
                }
            }
        });
    }

    function simulate(controls, catalog, values, generation) {
        const byKey = new Map((catalog ?? []).map(parameter => [parameter.key, parameter]));
        const parameters = new Map();
        const prior = new Map(values);
        const meterKeys = new Set();
        for (const control of controls) {
            const {kind, key} = control;
            if (kind === "meter") { meterKeys.add(key); continue; }
            if (kind === "text" || parameters.has(key)) continue;
            const fallback = key === "gain" ? {minimum: -60, maximum: 12, step: 0.01, unit: "dB"} :
                {minimum: 0, maximum: 1, step: kind === "toggle" ? 1 : 0.01, unit: ""};
            const parameter = {...(byKey.get(key) ?? {key, value: 0, ...fallback})};
            const saved = prior.get(key);
            if (Number.isFinite(saved) && saved >= parameter.minimum && saved <= parameter.maximum) parameter.value = saved;
            if (!catalog || byKey.has(key)) parameters.set(key, parameter);
        }
        let listener;
        const publish = () => listener({type: "parameters", parameters: [...parameters.values()].map(parameter => ({...parameter}))});
        window.cell = {
            onMessage(callback) {
                listener = callback;
                publish();
                if (meterKeys.size) {
                    const signal = () => {
                        const time = performance.now() / 690;
                        listener({type: "meters", meters: [...meterKeys].map((key, index) => ({
                            key, minimum: -60, maximum: 0, unit: "dBFS",
                            value: -24 - (key.endsWith("rms") ? 9 : 0) + 9 * Math.sin(time + index) + 4 * Math.sin(time * 2.3)
                        }))});
                    };
                    signal();
                    setInterval(signal, 100);
                }
            },
            postMessage(message) {
                const parameter = parameters.get(message.key);
                if (message.type !== "parameter" || !parameter || !Number.isFinite(message.value)) return;
                parameter.value = message.value;
                publish();
                window.parent.postMessage({type: "cell-preview-value", generation, key: message.key, value: message.value}, "*");
            }
        };
    }

    const css = `
*{box-sizing:border-box}
:root{color-scheme:dark}
body{margin:0;background:var(--background);color:var(--text);font:15px system-ui,sans-serif}
main{display:grid;padding:12px;gap:8px;min-height:100dvh;grid-template-columns:repeat(12,minmax(0,1fr));grid-template-rows:repeat(var(--rows),minmax(64px,auto))}
.control{min-width:0;padding:8px;border-radius:10px;background:var(--surface);display:flex;flex-direction:column;gap:3px}
.control-title,output{overflow-wrap:anywhere}
.control-title{font-weight:600}
output{font-variant-numeric:tabular-nums}
.face{position:relative;min-width:0;flex:1;display:flex;align-items:center;justify-content:center}
input,select,button{font:inherit;color:inherit;accent-color:var(--accent);margin:0}
input[type=range],select,button{width:100%;min-width:0;min-height:44px}
select,button{background:var(--background);border:1px solid currentColor;border-radius:7px;padding:4px 8px}
button[aria-pressed=true]{border-color:var(--accent);box-shadow:inset 0 -3px var(--accent)}
:focus-visible,.face:has(:focus-visible){outline:2px solid var(--accent);outline-offset:2px}
.fader input{writing-mode:vertical-lr;direction:rtl;width:44px;height:100%;min-height:100px}
.knob .face{min-height:72px;touch-action:none}
.knob .face.dragging{cursor:grabbing}
.knob input{position:absolute;inset:0;opacity:0;height:100%;cursor:ns-resize;touch-action:none}
.dial{position:relative;width:min(100%,70px);aspect-ratio:1;border-radius:50%;border:1px solid currentColor;pointer-events:none}
.dial::after{content:"";position:absolute;inset:7%;background:linear-gradient(var(--accent),var(--accent)) center top/4px 23% no-repeat;transform:rotate(var(--angle,-135deg))}
.artwork{position:absolute;inset:0;width:100%;height:100%;object-fit:contain;pointer-events:none}
.meter-face{display:grid;align-content:center;position:relative;flex:1;min-height:44px}
.meter-face meter{width:100%;height:22px}
.meter-scale{display:flex;justify-content:space-between;font-size:11px;font-variant-numeric:tabular-nums}
.control[data-skin=field]{--accent:#f0d898;color:#f7eec6;background:#30392c;border:1px dashed #8c9278}
.control[data-skin=field] .dial{box-shadow:0 0 0 5px #465042,0 0 0 8px #252b22;background:radial-gradient(circle,#596151,#242d23)}
.control[data-skin=field] .dial::before{content:"";position:absolute;inset:-7px;border-radius:50%;background:repeating-conic-gradient(from -135deg,#e8d9a5 0deg 1.8deg,transparent 1.8deg 15deg);mask:radial-gradient(circle,transparent 65%,#000 66% 72%,transparent 73%)}
.control[data-skin=studio]{--accent:#9bd6f2;color:#e8f3f7;background:#24333d;border-bottom:3px solid var(--accent)}
.control[data-skin=studio].fader .face{border-radius:6px;background:repeating-linear-gradient(to bottom,transparent 0 11px,#91a9b62a 11px 12px)}
.control[data-skin=studio].fader input{appearance:none;background:transparent;cursor:ns-resize}
.control[data-skin=studio].fader input::-webkit-slider-runnable-track{width:5px;height:100%;background:#172027;border-radius:3px;border:1px solid #657c87}
.control[data-skin=studio].fader input::-webkit-slider-thumb{appearance:none;width:34px;height:17px;margin-left:-15px;border:1px solid #172027;border-radius:3px;background:linear-gradient(#596d78 0 41%,var(--accent) 41% 53%,#2c3b45 53%);box-shadow:0 2px 3px #0008}
.control[data-skin=studio].fader input::-moz-range-track{width:5px;height:100%;background:#172027;border-radius:3px;border:1px solid #657c87}
.control[data-skin=studio].fader input::-moz-range-thumb{width:33px;height:16px;border:1px solid #172027;border-radius:3px;background:linear-gradient(#596d78 0 41%,var(--accent) 41% 53%,#2c3b45 53%);box-shadow:0 2px 3px #0008}
.control[data-skin=studio] .dial{border-radius:35%;background:linear-gradient(145deg,#455863,#1b2630)}
.control[data-skin=broadcast]{--accent:#9a311b;color:#372512;background:#e7d3a6;border:3px double #5b432a;color-scheme:light}
.control[data-skin=broadcast] .meter-face{min-height:62px;background:#f6e5bd;border:1px solid #634c37;border-radius:8px 8px 14px 14px;overflow:hidden}
.control[data-skin=broadcast] .meter-face meter{position:absolute;width:1px;height:1px;clip-path:inset(50%)}
.control[data-skin=broadcast] .meter-face::before{content:"";position:absolute;top:13%;left:7%;right:7%;height:8px;background:repeating-linear-gradient(to right,#634c37 0 1px,transparent 1px 5%);pointer-events:none}
.control[data-skin=broadcast] .meter-face::after{content:"";position:absolute;bottom:0;left:calc(50% - 1px);width:2px;height:72%;background:var(--accent);transform-origin:center bottom;transform:rotate(var(--needle,-50deg))}
.control[data-skin=broadcast] .meter-scale{margin-top:-24px;padding:0 7%;letter-spacing:.02em}
.control[data-skin=broadcast] button,.control[data-skin=broadcast] select{background:#f7e7c7;color:#372512}
.control[data-skin=broadcast] .dial{background:radial-gradient(circle,#f7e7c7,#ad8f62)}
.text{justify-content:center;background:transparent}
main[data-layout=phone]{grid-template-columns:repeat(var(--phone-columns),minmax(0,1fr));grid-template-rows:none;grid-auto-rows:minmax(52px,auto);align-content:start}
main[data-layout=phone] .fader .face{min-height:125px}
main[data-layout=phone] .meter-face{min-height:62px}
`;
    const json = value => JSON.stringify(value).replace(/</g, "\\u003c");

    function html(controls, theme, preview) {
        const {phoneGrid, phonePlacements, readingOrder} = CellDesignerModel;
        const phone = phonePlacements(controls);
        const sorted = controls.map((control, index) => ({...control, phone: phone[index]})).sort(readingOrder);
        const simulation = preview
            ? `(${simulate.toString()})(controls,${json(preview.catalog)},${json(preview.values)},${preview.generation});` : "";
        const colors = Object.entries(theme).map(([name, value]) => `--${name}:${value}`).join(";");
        const rows = sorted.reduce((maximum, control) => Math.max(maximum, control.y + control.height), 1);
        return `<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Cell plug-in editor</title>
<style>:root{${colors};--rows:${rows};--phone-columns:${phoneGrid.columns}}${css}</style></head>
<body><main></main><script>const controls = ${json(sorted)};${simulation}(${mount.toString()})(controls,${phoneGrid.breakpoint});</script></body></html>`;
    }

    return {html};
})();
