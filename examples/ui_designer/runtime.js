"use strict";

const CellDesignerRuntime = (() => {
    // Serialized into the standalone document along with the selected controls.
    function mount(controls, background, phoneBreakpoint, stepCount, isPanelArt, isProgramControl, readPrograms, led, compareValue, paintType, families) {
        const stage = document.querySelector("main");
        const items = [];
        const states = new Map();
        const bindings = new Map();
        const meters = new Map();
        const parameters = new Map();
        const owners = new Map();
        const stopDrags = [];
        const bridge = window.cell;
        const actionControls = [];
        let actions = new Set();
        let momentaryAvailable = false;
        const programControls = [];
        let memory = null;
        let bankNumber;
        let catalogSignature;
        if (background.artwork) {
            const image = document.createElement("img");
            image.className = "panel-background";
            image.alt = "";
            image.src = background.artwork;
            image.style.objectFit = background.fit;
            stage.append(image);
        }
        const format = (parameter, value) => {
            const index = value - parameter.minimum;
            const digits = parameter.step > 0 && parameter.step < 0.01 ? 4 : 2;
            return parameter.choices?.[index] ?? `${value.toFixed(digits)}${parameter.unit ? " " + parameter.unit : ""}`;
        };
        const labelOf = control => states.get(control).label;
        const interactive = control => states.get(control).interactive;
        const setAvailable = (control, element, available) => { element.disabled = !available || !interactive(control); };

        function nameElement(control, element, property = "aria-label", compose = value => value) {
            const apply = label => {
                if (property === "aria-label") element.setAttribute(property, compose(label));
                else element[property] = compose(label);
            };
            apply(labelOf(control));
            if (control.labelFrom) states.get(control).rename.push(apply);
        }

        function updateRules() {
            const focused = document.activeElement;
            const hiddenOrDisabled = [];
            let focusFrom, changed = false;
            const matches = groups => !groups.length || groups.some(group => group.every(({key, operator, value}) => {
                const parameter = parameters.get(key);
                return parameter && compareValue(parameter.value, operator, value);
            }));
            for (const state of items) {
                const {control, item} = state;
                if (control.labelFrom) {
                    const {key, source, labels} = control.labelFrom;
                    const parameter = parameters.get(key);
                    let label;
                    if (parameter) {
                        if (source === "name") label = parameter.name;
                        if (source === "choice") label = parameter.choices?.[parameter.value - parameter.minimum];
                        if (source === "value") label = format(parameter, parameter.value);
                        if (source === "map") label = labels.find(entry => entry.value === parameter.value)?.label;
                    }
                    if (typeof label !== "string" || !label.trim()) label = control.label;
                    if (label !== state.label) {
                        state.label = label;
                        state.rename.forEach(apply => apply(label));
                    }
                }
                if (!control.visibleRules.length && !control.enabledRules.length) continue;
                const visible = matches(control.visibleRules);
                const enabled = matches(control.enabledRules);
                const next = visible && enabled;
                if (next !== state.interactive) {
                    if (!next) {
                        hiddenOrDisabled.push(state);
                        if (item.contains(focused)) focusFrom = item;
                    }
                    state.interactive = next;
                    changed = true;
                }
                item.hidden = !visible;
                item.toggleAttribute("data-rule-disabled", !enabled);
            }
            // Update every rule before ending a gesture that may publish another snapshot.
            hiddenOrDisabled.forEach(state => state.stop?.());
            return {changed, focusFrom};
        }

        function moveFocus(from) {
            const replacement = items.find(({item, interactive}) => interactive &&
                item.style.gridRow === from.style.gridRow && item.style.gridColumn === from.style.gridColumn &&
                item.querySelector("button:enabled, select:enabled, input:enabled"));
            const candidate = replacement?.item.querySelector("button:enabled, select:enabled, input:enabled") ?? stage;
            candidate.focus({preventScroll: true});
        }
        const twoState = parameter => parameter.maximum > parameter.minimum && parameter.step > 0 &&
            Math.abs(parameter.minimum + parameter.step - parameter.maximum) <= Math.max(1e-9, parameter.step * 1e-6) &&
            (!parameter.choices?.length || parameter.choices.length === 2);
        const send = (type, key, value) => bridge.postMessage(value === undefined ? {type, key} : {type, key, value});
        const clampIndex = (index, count) => Math.min(count, Math.max(0, index));
        const stepIndex = (parameter, value, count) =>
            clampIndex(Math.round((value - parameter.minimum) / parameter.step), count);
        const stepValue = (parameter, index) => Math.min(parameter.maximum, parameter.minimum + index * parameter.step);
        function adjacentValue(parameter, value, direction, count) {
            if (!count) return undefined;
            const index = stepIndex(parameter, value, count);
            const position = (value - parameter.minimum) / parameter.step;
            const onStep = Math.abs(stepValue(parameter, index) - value) <= parameter.step * 1e-6;
            const target = onStep ? index + direction : direction > 0 ? Math.ceil(position) : Math.floor(position);
            if (target < 0 || target > count) return undefined;
            const next = stepValue(parameter, target);
            return next === value ? undefined : next;
        }

        function createLed(output) {
            output.classList.add("led-window");
            const text = document.createElement("span");
            text.className = "led-code";
            text.setAttribute("aria-hidden", "true");
            const digits = Array.from({length: 2}, () => {
                const digit = document.createElement("span");
                digit.className = "led-digit";
                digit.setAttribute("aria-hidden", "true");
                for (const name of "abcdefgp") {
                    const segment = document.createElement("span");
                    segment.className = "led-segment";
                    segment.dataset.segment = name;
                    digit.append(segment);
                }
                return digit;
            });
            output.replaceChildren(text, ...digits);
            return {text, digits};
        }

        function showLed(binding, code, detail) {
            const {output, control, ledDisplay} = binding;
            const pair = code.padStart(2, " ");
            if (output.dataset.code !== pair) {
                output.dataset.code = pair;
                ledDisplay.text.textContent = code;
                ledDisplay.digits.forEach((digit, index) => {
                    const lit = led.glyphs[pair[index]];
                    for (const segment of digit.children)
                        segment.toggleAttribute("data-lit", lit.includes(segment.dataset.segment));
                });
            }
            output.title = detail;
            output.setAttribute("aria-label", `${labelOf(control)} value ${code ? code + (detail && detail !== code ? ", " + detail : "") : detail}`);
        }

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
            if (!interactive(binding.control)) return;
            const previous = owners.get(binding.key);
            if (previous !== binding) {
                if (previous) {
                    previous.stop?.();
                    end(previous);
                }
                owners.set(binding.key, binding);
                send("begin", binding.key);
            }
            // The active control shows its edit until the host echoes or corrects it.
            binding.deferred = undefined;
            send("parameter", binding.key, value);
            const parameter = parameters.get(binding.key);
            if (parameter) display(binding, parameter, value);
        }

        function display(binding, parameter, value) {
            const {control, output, input, dial, ledDisplay} = binding;
            binding.value = value;
            const mapped = led.codeForValue(control.displayCodes, value, parameter.step);
            if (control.kind === "led") {
                showLed(binding, mapped ?? led.numericCode(value), format(parameter, value));
                return;
            }
            if (control.kind === "stepper" && binding.buttons) {
                const count = stepCount(parameter);
                const index = count ? stepIndex(parameter, value, count) : 0;
                const choice = parameter.choices?.[index];
                if (ledDisplay) showLed(binding, mapped ?? led.numericCode(value), choice ?? format(parameter, value));
                else {
                    const code = mapped ?? (Number.isInteger(value)
                        ? (value < 0 ? "−" : "") + String(Math.abs(value)).padStart(2, "0")
                        : format({...parameter, choices: []}, value));
                    output.textContent = code;
                    output.setAttribute("aria-label", labelOf(control) + " value " + code + (choice && choice !== code ? ", " + choice : ""));
                    output.title = choice ?? "";
                }
                for (const [i, button] of binding.buttons.entries()) {
                    setAvailable(control, button, Boolean(count));
                    const chord = control.chordAction && permits(control.chordAction, "invoke");
                    button.setAttribute("aria-disabled", String(!chord &&
                        adjacentValue(parameter, value, i === 0 ? -1 : 1, count) === undefined));
                }
                return;
            }
            const formatted = format(parameter, value);
            output.textContent = mapped ?? formatted;
            if (control.displayCodes?.length) {
                output.title = mapped ? formatted : "";
                output.setAttribute("aria-label", labelOf(control) + " value" +
                    (mapped && mapped !== formatted ? ` ${mapped}, ${formatted}` : ""));
            }
            if (input?.type === "range") input.setAttribute("aria-valuetext", output.textContent);
            if (control.kind === "toggle")
                input.setAttribute("aria-pressed", String(value === parameter.maximum));
            if (dial) {
                const span = parameter.maximum - parameter.minimum;
                const steps = binding.detentSteps;
                const index = steps ? stepIndex(parameter, value, steps) : 0;
                const proportion = steps ? index / steps : span > 0 ? (value - parameter.minimum) / span : 0;
                dial.style.setProperty("--angle", `${-135 + Math.min(Math.max(proportion, 0), 1) * 270}deg`);
                if (steps) {
                    binding.detentMarks.children[binding.activeDetent]?.removeAttribute("data-active");
                    binding.detentMarks.children[index]?.setAttribute("data-active", "");
                    binding.activeDetent = index;
                }
            }
        }

        function updateDetents(binding, parameter) {
            if (!binding.detentMarks) return 0;
            const count = stepCount(parameter);
            const steps = count < 32 ? count : 0;
            if (steps === binding.detentSteps) return steps;
            binding.detentSteps = steps;
            binding.detentMarks.replaceChildren();
            binding.dial.toggleAttribute("data-detented", steps > 0);
            for (let i = 0; steps && i <= steps; i++) {
                const mark = document.createElement("span");
                mark.style.setProperty("--detent-angle", `${-135 + i * 270 / steps}deg`);
                binding.detentMarks.append(mark);
            }
            return steps;
        }

        function update(binding, parameter) {
            const {input, control} = binding;
            if (input) {
                if (control.kind === "momentary") {
                    const available = momentaryAvailable && parameter.minimum < parameter.maximum;
                    if (!available) binding.stop();
                    setAvailable(control, input, available);
                } else if (control.kind === "choice") {
                    const choices = (parameter.choices ?? []).slice(0, Math.max(0, Math.floor(parameter.maximum - parameter.minimum) + 1));
                    const signature = JSON.stringify([parameter.minimum, choices]);
                    if (binding.signature !== signature) {
                        input.replaceChildren(...choices.map((label, i) => new Option(label, parameter.minimum + i)));
                        binding.signature = signature;
                    }
                    setAvailable(control, input, Boolean(choices.length));
                    input.value = parameter.value;
                } else if (control.kind === "toggle") {
                    setAvailable(control, input, twoState(parameter));
                } else {
                    const steps = updateDetents(binding, parameter);
                    input.min = steps ? 0 : parameter.minimum;
                    input.max = steps || parameter.maximum;
                    input.step = steps ? 1 : parameter.step > 0 ? parameter.step : "any";
                    input.value = steps ? stepIndex(parameter, parameter.value, steps) : parameter.value;
                    setAvailable(control, input, parameter.minimum < parameter.maximum);
                }
            }
            display(binding, parameter, parameter.value);
        }

        function rangeInput(binding, face) {
            const {input, key, control} = binding;
            input.type = "range";
            input.step = "any";
            input.oninput = () => {
                const parameter = parameters.get(key);
                if (parameter) write(binding, binding.detentSteps ? stepValue(parameter, input.valueAsNumber) : input.valueAsNumber);
            };
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
                const span = binding.detentSteps || (parameter.maximum - parameter.minimum);
                const scale = span / (event.shiftKey ? 1000 : 180);
                const next = drag.value + (drag.y - event.clientY + event.clientX - drag.x) * scale;
                const before = input.valueAsNumber;
                input.value = next;
                if (input.valueAsNumber !== before) input.oninput();
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
            binding.stop = stop;
            stopDrags.push(stop);
        }

        function wirePress(button, handlers, {keyboard = true, confine = false} = {}) {
            let active;
            function start(identity) {
                if (button.disabled || active || handlers.start(identity) === false) return;
                active = identity;
                button.setAttribute("data-held", "");
                if (identity.pointer !== undefined) {
                    try { button.setPointerCapture(identity.pointer); }
                    catch { stop(false); }
                }
            }
            function stop(commit) {
                if (!active) return;
                const identity = active;
                active = undefined;
                button.removeAttribute("data-held");
                if (identity.pointer !== undefined && button.hasPointerCapture(identity.pointer))
                    button.releasePointerCapture(identity.pointer);
                handlers.stop(identity, commit);
            }
            const inside = event => {
                const box = button.getBoundingClientRect();
                return event.clientX >= box.left && event.clientX <= box.right &&
                    event.clientY >= box.top && event.clientY <= box.bottom;
            };
            button.onpointerdown = event => {
                if (event.button !== 0 || button.disabled) return;
                event.preventDefault();
                if (event.isPrimary) button.focus({preventScroll: true});
                start({pointer: event.pointerId});
            };
            button.onpointerup = event => {
                if (active?.pointer === event.pointerId) stop(!confine || inside(event));
            };
            button.onpointercancel = button.onlostpointercapture = event => {
                if (active?.pointer === event.pointerId) stop(false);
            };
            if (confine) button.onpointermove = event => {
                if (active?.pointer === event.pointerId && !inside(event)) stop(false);
            };
            if (keyboard) {
                button.onkeydown = event => {
                    if (![" ", "Enter"].includes(event.key) || event.altKey || event.ctrlKey || event.metaKey) return;
                    event.preventDefault();
                    if (!event.repeat) start({key: event.code || event.key});
                };
                button.onkeyup = event => {
                    if (active?.key === (event.code || event.key)) {
                        event.preventDefault();
                        stop(true);
                    }
                };
                button.onblur = () => stop(false);
            }
            button.onclick = event => {
                if (event.detail === 0 && !button.disabled && !active) handlers.click();
            };
            return () => stop(false);
        }

        const permits = (key, mode) => actions.has(key + "/" + mode);
        const invoke = (key, phase = "invoke") => bridge.postMessage({type: "action", key, phase});

        function actionButton(control, item) {
            const {action, trigger, holdMs, repeatDelay, repeatMs} = control;
            const mode = trigger === "momentary" ? "momentary" : "invoke";
            const button = document.createElement("button");
            button.type = "button";
            nameElement(control, button, "textContent");
            button.disabled = true;
            if (trigger === "hold") button.title = "Hold for " + holdMs / 1000 + " seconds";
            if (trigger === "repeat") button.title = "Hold to repeat";
            item.append(button);
            if (!bridge) return;
            let timer;
            const stop = wirePress(button, {
                start() {
                    if (trigger === "momentary") invoke(action, "press");
                    if (trigger === "hold") timer = setTimeout(() => invoke(action), holdMs);
                    if (trigger === "repeat") {
                        invoke(action);
                        const repeat = () => {
                            invoke(action);
                            timer = setTimeout(repeat, repeatMs);
                        };
                        timer = setTimeout(repeat, repeatDelay);
                    }
                },
                stop(_identity, commit) {
                    clearTimeout(timer);
                    if (trigger === "momentary") invoke(action, "release");
                    if (trigger === "tap" && commit) invoke(action);
                },
                click() {
                    if (trigger === "momentary") {
                        invoke(action, "press");
                        invoke(action, "release");
                    } else if (trigger !== "hold") invoke(action);
                }
            }, {confine: true});
            actionControls.push(() => {
                const available = permits(action, mode);
                if (!available) stop();
                setAvailable(control, button, available);
            });
            states.get(control).stop = stop;
            stopDrags.push(stop);
        }

        function pairInput(control, face, output, advance, finish) {
            const {chordAction, chordMs} = control;
            const pressed = new Map();
            const keys = new Map();
            const stopPointers = [];
            let timer, suppress, deferred;
            const chordEnabled = () => Boolean(chordAction) && permits(chordAction, "invoke");
            const start = (direction, identity) => {
                if (pressed.has(direction) || (pressed.size && !chordEnabled())) return false;
                if (!pressed.size && !chordEnabled() && !advance(direction)) return false;
                pressed.set(direction, identity);
                if (suppress) return true;
                clearTimeout(timer);
                if (pressed.size === 2) {
                    suppress = true;
                    finish();
                    timer = setTimeout(() => {
                        if (pressed.size === 2 && chordEnabled()) invoke(chordAction);
                    }, chordMs);
                } else {
                    deferred = chordEnabled();
                    const repeat = () => {
                        deferred = false;
                        if (pressed.get(direction) === identity && advance(direction) && pressed.get(direction) === identity)
                            timer = setTimeout(repeat, 85);
                    };
                    timer = setTimeout(repeat, 350);
                }
                return true;
            };
            const release = (direction, identity, commit) => {
                if (pressed.get(direction) !== identity) return;
                pressed.delete(direction);
                clearTimeout(timer);
                if (!suppress && deferred && commit) advance(direction);
                finish();
                if (!pressed.size) suppress = deferred = false;
            };
            const stop = () => {
                clearTimeout(timer);
                pressed.clear();
                keys.clear();
                suppress = deferred = false;
                stopPointers.forEach(cancel => cancel());
                finish();
            };
            const buttons = [-1, 1].map(direction => {
                const button = document.createElement("button");
                button.type = "button";
                button.textContent = direction < 0 ? "−" : "+";
                nameElement(control, button, "aria-label", label => (direction < 0 ? "Decrease " : "Increase ") + label);
                button.disabled = true;
                stopPointers.push(wirePress(button, {
                    start: identity => start(direction, identity),
                    stop: (identity, commit) => release(direction, identity, commit),
                    click() { if (advance(direction)) finish(); }
                }, {keyboard: false}));
                return button;
            });
            face.onkeydown = event => {
                const direction = ["ArrowUp", "ArrowRight", "+", "="].includes(event.key) ? 1 :
                    ["ArrowDown", "ArrowLeft", "-"].includes(event.key) ? -1 :
                    [" ", "Enter"].includes(event.key) ? (event.target === buttons[0] ? -1 : 1) : 0;
                if (!direction || event.altKey || event.ctrlKey || event.metaKey || event.target.disabled) return;
                event.preventDefault();
                const key = event.code || event.key;
                if (event.repeat || keys.has(key)) return;
                const identity = {direction};
                if (start(direction, identity)) keys.set(key, identity);
            };
            face.onkeyup = event => {
                const key = event.code || event.key;
                const identity = keys.get(key);
                if (!identity) return;
                event.preventDefault();
                keys.delete(key);
                release(identity.direction, identity, true);
            };
            face.onfocusout = event => { if (!face.contains(event.relatedTarget)) stop(); };
            face.setAttribute("role", "group");
            nameElement(control, face);
            if (chordAction)
                face.setAttribute("aria-description", "Hold minus and plus together for " + chordMs / 1000 + " seconds. Keyboard: Left and Right arrows.");
            face.append(buttons[0]);
            if (output) face.append(output);
            face.append(buttons[1]);
            stopDrags.push(stop);
            return {buttons, stop};
        }

        function stepperInput(binding, face) {
            const advance = direction => {
                const parameter = parameters.get(binding.key);
                const next = adjacentValue(parameter, binding.value, direction, stepCount(parameter));
                if (next === undefined) return false;
                write(binding, next);
                return true;
            };
            Object.assign(binding, pairInput(binding.control, face, binding.output, advance, () => end(binding)));
        }

        function actionPair(control, item) {
            const face = document.createElement("div");
            face.className = "face";
            item.append(face);
            const entries = [control.minusAction, control.plusAction];
            const advance = direction => {
                const key = entries[direction < 0 ? 0 : 1];
                if (!permits(key, "invoke")) return false;
                invoke(key);
                return true;
            };
            const {buttons, stop} = pairInput(control, face, null, advance, () => {});
            actionControls.push(() => {
                const chord = control.chordAction && permits(control.chordAction, "invoke");
                buttons.forEach((button, index) => setAvailable(control, button, chord || permits(entries[index], "invoke")));
                if (buttons.every(button => button.disabled)) stop();
            });
            states.get(control).stop = stop;
        }

        const programNumber = number => String(number + 1).padStart(2, "0");
        const isActive = (bank, program) => memory?.active?.bank === bank && memory.active.program === program;
        function recall(bank, program) {
            cancel();
            bridge.postMessage({type: "recall-program", bank, program});
        }

        function programControl(control, item) {
            const {kind} = control;
            const element = document.createElement(kind === "bank" || kind === "program" ? "select" : "div");
            nameElement(control, element);
            if (kind === "patch-name") element.setAttribute("role", "status");
            if (kind === "program-grid") element.className = "program-banks";
            const data = {control, element, buttons: []};
            programControls.push(data);
            item.append(element);
            if (kind === "bank") element.onchange = () => {
                if (element.disabled) return;
                bankNumber = Number(element.value);
                updateProgramControls(false);
            };
            if (kind === "program") element.onchange = () => {
                if (element.disabled || element.value === "") return;
                const number = Number(element.value);
                element.value = memory?.active?.bank === bankNumber ? String(memory.active.program) : "";
                recall(bankNumber, number);
            };
        }

        function updateProgramControls(rebuild) {
            const banks = memory?.banks ?? [];
            const bank = banks.find(entry => entry.number === bankNumber);
            const active = memory?.active;
            const patch = banks.find(entry => entry.number === active?.bank)?.programs.find(entry => entry.number === active.program);
            for (const data of programControls) {
                const {control, element} = data;
                const {kind} = control;
                if (kind === "bank" || kind === "program") {
                    const entries = kind === "bank" ? banks : bank?.programs ?? [];
                    const options = entries.map(entry => [kind === "bank" ? entry.name : `${programNumber(entry.number)} ${entry.name}`, entry.number]);
                    if (kind === "program" || !entries.length)
                        options.unshift([entries.length ? "Select program" : "No programs available", ""]);
                    const signature = JSON.stringify(options);
                    if (signature !== data.signature) {
                        element.replaceChildren(...options.map(([label, number]) => new Option(label, number)));
                        data.signature = signature;
                    }
                    setAvailable(control, element, Boolean(entries.length));
                    if (kind === "program") element.options[0].disabled = true;
                    const selected = kind === "bank" ? bank?.number : active?.bank === bankNumber ? active?.program : undefined;
                    element.value = selected === undefined ? "" : String(selected);
                } else if (kind === "patch-name") {
                    const name = patch?.name ?? "—";
                    if (element.textContent !== name) element.textContent = name;
                } else {
                    if (rebuild) {
                        data.buttons.length = 0;
                        const columns = Math.min(6, Math.max(1, ...banks.map(bank => bank.programs.length)));
                        element.style.setProperty("--program-columns", columns);
                        element.replaceChildren(...banks.map(bank => {
                            const group = document.createElement("fieldset");
                            const legend = document.createElement("legend");
                            legend.textContent = bank.name;
                            const row = document.createElement("div");
                            row.className = "program-buttons";
                            row.append(...bank.programs.map(program => {
                                const button = document.createElement("button");
                                button.type = "button";
                                button.title = program.name;
                                button.setAttribute("aria-label", `${bank.name}, ${programNumber(program.number)} ${program.name}`);
                                button.textContent = programNumber(program.number);
                                button.onclick = () => { if (!button.disabled) recall(bank.number, program.number); };
                                data.buttons.push({button, bank: bank.number, program: program.number});
                                return button;
                            }));
                            group.append(legend, row);
                            return group;
                        }));
                        if (!data.buttons.length) element.textContent = "No programs available";
                    }
                    for (const {button, bank, program} of data.buttons) {
                        setAvailable(control, button, true);
                        button.setAttribute("aria-pressed", String(isActive(bank, program)));
                    }
                }
            }
        }

        function updateMemory(message, rulesChanged) {
            let next = null;
            try { next = readPrograms(message.programs); } catch {}
            const signature = JSON.stringify(next?.banks ?? []);
            const rebuild = signature !== catalogSignature;
            const activeChanged = next?.active?.bank !== memory?.active?.bank || next?.active?.program !== memory?.active?.program;
            if (!rebuild && !activeChanged) {
                if (rulesChanged) updateProgramControls(false);
                return;
            }
            if (activeChanged && next?.active || !next?.banks.some(bank => bank.number === bankNumber))
                bankNumber = next?.active?.bank ?? next?.banks.find(bank => bank.programs.length)?.number ?? next?.banks[0]?.number;
            memory = next;
            catalogSignature = signature;
            updateProgramControls(rebuild);
        }

        for (const control of controls) {
            const item = document.createElement("div");
            item.className = "control " + control.kind;
            item.dataset.skin = control.skin;
            if (control.typography) paintType(item, control.typography, families);
            if (control.accent) item.style.setProperty("--accent", control.accent);
            const state = {control, item, label: control.label, interactive: true, rename: []};
            states.set(control, state);
            items.push(state);
            if (isPanelArt(control.kind)) {
                item.classList.add("panel-art");
                if (control.kind === "image" && control.artwork) {
                    const image = document.createElement("img");
                    nameElement(control, image, "alt");
                    image.src = control.artwork;
                    image.style.objectFit = control.fit ?? "contain";
                    item.append(image);
                } else {
                    item.setAttribute("aria-hidden", "true");
                    if (control.kind === "line") {
                        item.dataset.direction = control.direction ?? "horizontal";
                        item.style.setProperty("--line-thickness", `${control.thickness ?? 2}px`);
                    }
                }
                continue;
            }
            const title = document.createElement("span");
            title.className = "control-title";
            nameElement(control, title, "textContent");
            if (!["action", "toggle", "momentary"].includes(control.kind)) item.append(title);
            if (control.kind === "text") continue;
            if (control.kind === "action") {
                actionButton(control, item);
                continue;
            }
            if (control.kind === "action-pair") {
                actionPair(control, item);
                continue;
            }
            if (isProgramControl(control.kind)) {
                programControl(control, item);
                continue;
            }

            const output = document.createElement("output");
            output.textContent = "—";
            nameElement(control, output, "aria-label", label => label + " value");
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
                nameElement(control, gauge);
                face.append(scale, gauge);
                item.append(face);
                if (!meters.has(key)) meters.set(key, []);
                meters.get(key).push({output, gauge, face, marks});
                continue;
            }
            const binding = {key, control, output};
            state.stop = () => {
                binding.stop?.();
                end(binding);
            };
            if (control.kind === "led" || (control.kind === "stepper" && control.segmented)) {
                binding.ledDisplay = createLed(output);
                showLed(binding, "", "Unavailable");
            }
            if (!bindings.has(key)) bindings.set(key, []);
            bindings.get(key).push(binding);
            if (!bridge || control.kind === "readout" || control.kind === "led") continue;

            const face = document.createElement("div");
            face.className = "face";
            item.append(face);
            if (control.kind === "stepper") {
                stepperInput(binding, face);
                continue;
            }
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
                binding.detentMarks = document.createElement("div");
                binding.detentMarks.className = "detents";
                binding.dial.append(binding.detentMarks);
                face.append(binding.dial);
            }
            const buttonKind = control.kind === "toggle" || control.kind === "momentary";
            const input = document.createElement(control.kind === "choice" ? "select" : buttonKind ? "button" : "input");
            binding.input = input;
            nameElement(control, input);
            input.disabled = true;
            face.append(input);
            if (control.kind === "choice") {
                input.onchange = () => {
                    if (input.disabled || input.selectedIndex < 0) return;
                    write(binding, Number(input.value));
                    end(binding);
                };
            } else if (control.kind === "toggle") {
                input.type = "button";
                nameElement(control, input, "textContent");
                input.setAttribute("aria-pressed", "false");
                input.onclick = () => {
                    const parameter = parameters.get(key);
                    if (!parameter || input.disabled) return;
                    write(binding, parameter.value === parameter.maximum ? parameter.minimum : parameter.maximum);
                    end(binding);
                };
            } else if (control.kind === "momentary") {
                input.type = "button";
                nameElement(control, input, "textContent");
                binding.stop = wirePress(input, {
                    start() { send("momentary-press", key); },
                    stop() { send("momentary-release", key); },
                    click() {
                        send("momentary-press", key);
                        send("momentary-release", key);
                    }
                });
                stopDrags.push(binding.stop);
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
        stage.tabIndex = -1;
        updateRules();
        arrange();
        compact.addEventListener("change", arrange);
        if (programControls.length) updateMemory({});
        if (!bridge) return;
        window.addEventListener("blur", cancel);
        window.addEventListener("pagehide", cancel);
        document.addEventListener("visibilitychange", () => { if (document.hidden) cancel(); });
        let pendingSnapshot, receiving = false;
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
            pendingSnapshot = message;
            if (receiving) return;
            receiving = true;
            try {
                while (pendingSnapshot) {
                    const snapshot = pendingSnapshot;
                    pendingSnapshot = undefined;
                    applyParameterSnapshot(snapshot);
                }
            } finally {
                receiving = false;
            }
        });

        function applyParameterSnapshot(message) {
            momentaryAvailable = message.momentary === true;
            actions = new Set((Array.isArray(message.actions) ? message.actions : []).flatMap(action =>
                typeof action?.key === "string" && ["invoke", "momentary"].includes(action.mode)
                    ? [action.key + "/" + action.mode] : []));
            parameters.clear();
            for (const parameter of message.parameters) {
                if (parameter && [parameter.value, parameter.minimum, parameter.maximum].every(Number.isFinite))
                    parameters.set(parameter.key, parameter);
            }
            const {changed, focusFrom} = updateRules();
            actionControls.forEach(update => update());
            if (programControls.length) updateMemory(message, changed);
            for (const [key, items] of bindings) {
                const parameter = parameters.get(key);
                for (const binding of items) {
                    if (!parameter) {
                        binding.stop?.();
                        end(binding);
                        if (binding.input) binding.input.disabled = true;
                        for (const button of binding.buttons ?? []) button.disabled = true;
                        if (binding.ledDisplay) showLed(binding, "", "Unavailable");
                        else {
                            binding.output.textContent = "—";
                            if (binding.buttons || binding.control.displayCodes?.length) {
                                binding.output.removeAttribute("title");
                                binding.output.setAttribute("aria-label", labelOf(binding.control) + " value");
                            }
                        }
                        updateDetents(binding, undefined);
                    } else if (owners.get(key) === binding) {
                        binding.deferred = parameter;
                        if (binding.buttons && !stepCount(parameter)) {
                            binding.stop?.();
                            end(binding);
                        }
                    } else {
                        update(binding, parameter);
                    }
                }
            }
            if (focusFrom) moveFocus(focusFrom);
        }
    }

    function simulate(controls, catalog, memory, values, generation, isProgramControl, isPanelArt, actions, keys) {
        const byKey = new Map((catalog ?? []).map(parameter => [parameter.key, parameter]));
        const parameters = new Map();
        const prior = new Map(values);
        const meterKeys = new Set();
        const stepperKeys = new Set(controls.filter(control => control.kind === "stepper").map(control => control.key));
        function addParameter(key, fallback) {
            if (parameters.has(key) || (catalog && !byKey.has(key))) return;
            const parameter = {...(byKey.get(key) ?? {key, value: 0, ...fallback})};
            const saved = prior.get(key);
            if (Number.isFinite(saved) && saved >= parameter.minimum && saved <= parameter.maximum) parameter.value = saved;
            parameters.set(key, parameter);
        }
        for (const control of controls) {
            const {kind, key} = control;
            if (kind === "meter") { meterKeys.add(key); continue; }
            if (["text", "action", "action-pair"].includes(kind) || isProgramControl(kind) || isPanelArt(kind) || parameters.has(key)) continue;
            const fallback = stepperKeys.has(key) ? {minimum: 0, maximum: 99, step: 1, unit: ""} :
                key === "gain" ? {minimum: -60, maximum: 12, step: 0.01, unit: "dB"} :
                {minimum: 0, maximum: 1, step: ["toggle", "momentary"].includes(kind) ? 1 : 0.01, unit: ""};
            addParameter(key, fallback);
        }
        keys.forEach(key => addParameter(key, {minimum: 0, maximum: 1, step: 1, unit: ""}));
        let listener;
        const publish = () => listener({type: "parameters", programs: memory, actions, momentary: true,
            parameters: [...parameters.values()].map(parameter => ({...parameter}))});
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
                if (message.type === "action") {
                    window.parent.postMessage({type: "cell-preview-action", generation, key: message.key, phase: message.phase}, "*");
                    return;
                }
                if (message.type === "recall-program") {
                    const {bank, program} = message;
                    if (memory?.banks.find(entry => entry.number === bank)?.programs.some(entry => entry.number === program)) {
                        memory.active = {bank, program};
                        publish();
                        window.parent.postMessage({type: "cell-preview-program", generation, bank, program}, "*");
                    }
                    return;
                }
                const parameter = parameters.get(message.key);
                if (parameter && ["momentary-press", "momentary-release"].includes(message.type)) {
                    parameter.value = message.type === "momentary-press" ? parameter.maximum : parameter.minimum;
                    publish();
                    if (message.type === "momentary-press") return;
                    window.parent.postMessage({type: "cell-preview-value", generation, key: message.key, value: parameter.value}, "*");
                    return;
                }
                if (message.type !== "parameter" || !parameter || !Number.isFinite(message.value)) return;
                parameter.value = message.value;
                publish();
                window.parent.postMessage({type: "cell-preview-value", generation, key: message.key, value: message.value}, "*");
            }
        };
    }

    const css = `
*{box-sizing:border-box}
[hidden]{display:none!important}
:root{color-scheme:dark}
body{margin:0;background:var(--background);color:var(--text);font:15px system-ui,sans-serif}
main{position:relative;isolation:isolate;display:grid;padding:12px;gap:8px;min-height:100dvh;grid-template-columns:repeat(12,minmax(0,1fr));grid-template-rows:repeat(var(--rows),minmax(64px,auto))}
.control{z-index:4;min-width:0;padding:8px;border-radius:10px;background:var(--surface);display:flex;flex-direction:column;gap:3px}
.control[data-rule-disabled]{opacity:.55}
.panel-background,.panel-art img{position:absolute;inset:0;width:100%;height:100%;pointer-events:none}
.control.panel-art{position:relative;z-index:1;padding:0;border:0;border-radius:0;background:transparent;overflow:hidden;pointer-events:none}
.control.panel-art.line{z-index:2}
.control.panel-art.image{z-index:3}
.panel-art.rectangle::before,.panel-art.line::before{content:"";position:absolute;inset:0;background:var(--accent)}
.panel-art.line::before{top:50%;bottom:auto;height:var(--line-thickness);transform:translateY(-50%)}
.panel-art.line[data-direction=vertical]::before{inset:0 auto 0 50%;height:auto;width:var(--line-thickness);transform:translateX(-50%)}
.control-title,output{overflow-wrap:anywhere}
.control-title{font-weight:600;white-space:pre-wrap}
output{font-variant-numeric:tabular-nums}
.face{position:relative;min-width:0;flex:1;display:flex;align-items:center;justify-content:center}
input,select,button{font:inherit;color:inherit;accent-color:var(--accent);margin:0}
input[type=range],select,button{width:100%;min-width:0;min-height:44px}
select,button{background:var(--background);border:1px solid currentColor;border-radius:7px;padding:4px 8px}
.action button,.toggle button,.momentary button{white-space:pre-wrap;overflow-wrap:anywhere}
button[aria-pressed=true]{border-color:var(--accent);box-shadow:inset 0 -3px var(--accent)}
button[data-held]{border-color:var(--accent);box-shadow:inset 0 0 0 2px var(--accent)}
.action button,.momentary button{touch-action:none}
.action-pair .face{display:grid;grid-template-columns:repeat(2,minmax(18px,1fr));gap:6px}
.action-pair button{padding-inline:0;touch-action:none}
:focus-visible,.face:has(:focus-visible){outline:2px solid var(--accent);outline-offset:2px}
.fader input{writing-mode:vertical-lr;direction:rtl;width:44px;height:100%;min-height:100px}
.knob .face{min-height:72px;touch-action:none}
.knob .face.dragging{cursor:grabbing}
.knob input{position:absolute;inset:0;opacity:0;height:100%;cursor:ns-resize;touch-action:none}
.dial{position:relative;width:min(100%,70px);aspect-ratio:1;border-radius:50%;border:1px solid currentColor;pointer-events:none}
.dial::after{content:"";position:absolute;inset:7%;background:linear-gradient(var(--accent),var(--accent)) center top/4px 23% no-repeat;transform:rotate(var(--angle,-135deg))}
.detents{position:absolute;inset:-9px;pointer-events:none}
.detents span{position:absolute;inset:0;transform:rotate(var(--detent-angle))}
.detents span::before{content:"";position:absolute;top:0;left:calc(50% - 1px);width:2px;height:5px;border-radius:2px;background:currentColor;opacity:.58}
.detents [data-active]::before{background:var(--accent);width:4px;left:calc(50% - 2px);height:7px;opacity:1}
.artwork{position:absolute;inset:0;width:100%;height:100%;object-fit:contain;pointer-events:none}
.stepper .face{display:grid;grid-template-columns:minmax(18px,1fr) minmax(2ch,auto) minmax(18px,1fr);gap:clamp(2px,1vw,6px)}
.stepper button{padding-inline:0;touch-action:none;font-size:1.2em;font-weight:650;cursor:pointer}
.stepper button[aria-disabled=true],.stepper button:disabled{opacity:.5;cursor:default}
.stepper output{display:grid;place-items:center;background:var(--background);border-radius:4px;min-height:32px;padding-inline:3px;color:var(--accent);font:600 1.15em/1 ui-monospace,monospace;white-space:nowrap}
.control .led-window{display:flex;position:relative;align-items:center;justify-content:center;gap:4%;width:min(100%,92px);min-width:0;
min-height:32px;padding:4px 5px;color:var(--accent);background:#171b1a;border-radius:4px;box-shadow:inset 0 1px 4px #000a}
.stepper .face:has(.led-window){grid-template-columns:minmax(18px,1fr) minmax(22px,2fr) minmax(18px,1fr)}
.stepper .led-window{justify-self:center}
.led-code{position:absolute;clip-path:inset(50%);width:1px;height:1px;overflow:hidden}
.led-digit{position:relative;width:43%;max-width:31px;aspect-ratio:3/4}
.led-segment{position:absolute;background:currentColor;opacity:.17}
.led-segment[data-lit]{opacity:1}
.led-segment:is([data-segment=a],[data-segment=d],[data-segment=g]){left:13%;width:59%;height:9%;
clip-path:polygon(0 50%,12% 0,88% 0,100% 50%,88% 100%,12% 100%)}
.led-segment[data-segment=a]{top:1%}
.led-segment[data-segment=g]{top:45.5%}
.led-segment[data-segment=d]{bottom:1%}
.led-segment:is([data-segment=b],[data-segment=c],[data-segment=e],[data-segment=f]){width:12%;height:39%;
clip-path:polygon(50% 0,100% 12%,100% 88%,50% 100%,0 88%,0 12%)}
.led-segment:is([data-segment=b],[data-segment=c]){left:68%}
.led-segment:is([data-segment=e],[data-segment=f]){left:6%}
.led-segment:is([data-segment=b],[data-segment=f]){top:8%}
.led-segment:is([data-segment=c],[data-segment=e]){bottom:8%}
.led-segment[data-segment=p]{right:0;bottom:1%;width:10%;aspect-ratio:1;border-radius:100%}
.patch-name [role=status]{overflow-wrap:anywhere;color:var(--accent);font-weight:600}
.program-banks{display:grid;gap:6px}
.program-banks fieldset{min-width:0;margin:0;padding:4px 8px 8px;border:1px solid currentColor;border-radius:6px}
.program-banks legend{padding:0 4px;overflow-wrap:anywhere}
.program-buttons{display:grid;gap:4px;grid-template-columns:repeat(var(--program-columns),minmax(0,1fr))}
.program-buttons button{padding-inline:0;font-variant-numeric:tabular-nums}
.meter-face{display:grid;align-content:center;position:relative;flex:1;min-height:44px}
.meter-face meter{width:100%;height:22px}
.meter-scale{display:flex;justify-content:space-between;font-size:11px;font-variant-numeric:tabular-nums}
.control[data-skin=field]{--accent:#f0d898;color:#f7eec6;background:#30392c;border:1px dashed #8c9278}
.control[data-skin=field] .dial{box-shadow:0 0 0 5px #465042,0 0 0 8px #252b22;background:radial-gradient(circle,#596151,#242d23)}
.control[data-skin=field] .dial::before{content:"";position:absolute;inset:-7px;border-radius:50%;background:repeating-conic-gradient(from -135deg,#e8d9a5 0deg 1.8deg,transparent 1.8deg 15deg);mask:radial-gradient(circle,transparent 65%,#000 66% 72%,transparent 73%)}
.control[data-skin=field] .dial[data-detented]::before{content:none}
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
    const typeProperties = {family: "font-family", size: "font-size", weight: "font-weight",
        tracking: "letter-spacing", alignment: "text-align", lettercase: "text-transform"};
    const typeTargets = ":is(.tile-label,.control-title,output:not(.led-window),button,select,legend,.patch-name [role=status],.meter-scale)";
    const typographyCss = Object.entries(typeProperties).map(([key, property]) =>
        `[data-type-${key}] ${typeTargets}{${property}:var(--type-${key})}`).join("\n");

    function paintTypography(element, typography, families) {
        const {family, size, weight, tracking, alignment, letterCase} = typography ?? {};
        const pixels = value => value === undefined ? undefined : value + "px";
        const styles = {family: families[family]?.stack, size: pixels(size), weight,
            tracking: pixels(tracking), alignment, lettercase: letterCase};
        for (const [key, value] of Object.entries(styles)) {
            element.toggleAttribute("data-type-" + key, value !== undefined);
            if (value === undefined) element.style.removeProperty("--type-" + key);
            else element.style.setProperty("--type-" + key, value);
        }
    }

    function fontCss(styles) {
        const used = new Map();
        for (const {family, weight} of styles) {
            if (!Object.hasOwn(CellDesignerFonts, family)) continue;
            if (!used.has(family)) used.set(family, new Set());
            used.get(family).add(String(weight));
        }
        return [...used].map(([family, weights]) => {
            const font = CellDesignerFonts[family];
            const exactFaces = font.faces.filter(face => weights.has(face.weight));
            const faces = exactFaces.length === weights.size ? exactFaces : font.faces;
            return "/*\n" + font.license + "\n*/\n" + faces.map(({weight, data}) =>
                `@font-face{font-family:"${font.family}";font-style:normal;font-weight:${weight};font-display:swap;src:url(data:font/woff2;base64,${data}) format("woff2")}`
            ).join("\n");
        }).join("\n");
    }

    const json = value => JSON.stringify(value).replace(/</g, "\\u003c");

    function html(controls, theme, preview) {
        const {phoneGrid, phonePlacements, readingOrder, stepCount, isPanelArt, isProgramControl,
            readPrograms, defaultTheme, ledGlyphs, displayCode, numericLedCode, actionBindings,
            conditionRules, compareValue, parameterKeys, typographyFamilies} = CellDesignerModel;
        const phone = phonePlacements(controls);
        const sorted = controls.map((control, index) => ({...control, phone: phone[index],
            visibleRules: conditionRules(control.visibleWhen), enabledRules: conditionRules(control.enabledWhen)})).sort(readingOrder);
        const simulation = preview
            ? `(${simulate.toString()})(controls,${json(preview.catalog)},${json(preview.programs ?? null)},${json(preview.values)},${preview.generation},isProgramControl,isPanelArt,${json(actionBindings(controls))},${json(parameterKeys(controls))});` : "";
        const colors = Object.keys(defaultTheme).map(name => `--${name}:${theme[name]}`).join(";");
        const background = {artwork: theme.backgroundArtwork, fit: theme.backgroundFit ?? "cover"};
        const rows = sorted.reduce((maximum, control) => Math.max(maximum, control.y + control.height), 1);
        const fonts = fontCss(sorted.flatMap(control => control.typography ? [control.typography] : []));
        return `<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Cell plug-in editor</title>
<style>${fonts}
:root{${colors};--rows:${rows};--phone-columns:${phoneGrid.columns}}${css}${typographyCss}</style></head>
<body><main></main><script>const controls = ${json(sorted)};
const isProgramControl = ${isProgramControl.toString()}, isPanelArt = ${isPanelArt.toString()};
${simulation}(${mount.toString()})(controls,${json(background)},${phoneGrid.breakpoint},${stepCount.toString()},isPanelArt,isProgramControl,${readPrograms.toString()},
{glyphs:${json(ledGlyphs)},codeForValue:${displayCode.toString()},numericCode:${numericLedCode.toString()}},${compareValue.toString()},
${paintTypography.toString()},${json(typographyFamilies)});</script></body></html>`;
    }

    return {html, fontCss, paintTypography, typographyCss};
})();
