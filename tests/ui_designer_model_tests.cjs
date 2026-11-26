const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const root = path.join(__dirname, "../examples/ui_designer");
const context = vm.createContext({ assert, TextEncoder });
for (const name of ["model.js", "fonts.js", "artwork.js", "runtime.js"])
    vm.runInContext(fs.readFileSync(path.join(root, name), "utf8"), context);

vm.runInContext(`
const model = CellDesignerModel;
const old = model.readProject(JSON.stringify({schema: "cell.ui-layout.v1", controls: [
    {kind: "slider", label: "Gain", key: "gain", x: 7, y: 7}
]}));
assert.equal(old.controls[0].width, 4);
assert.equal(old.controls[0].height, 1);
assert.equal(old.controls[0].skin, "modern");
assert.equal(old.catalog, undefined);
assert.equal(old.controls[0].phone, undefined);

const v2 = model.readProject(JSON.stringify({schema: "cell.ui-layout.v2", controls: old.controls,
    theme: model.defaultTheme, catalog: []}));
assert.deepEqual(v2.catalog, []);
assert.deepEqual(model.phonePlacements(v2.controls), [{x: 0, y: 0, width: 4, height: 1}]);

const project = model.readProject(model.serialize(model.showcase, model.defaultTheme, null));
assert.deepEqual(Object.fromEntries(project.controls.map(({kind,skin}) => [kind,skin])),
    {text:"modern", knob:"field", fader:"studio", meter:"broadcast"});
assert.equal(project.catalog, null);
assert.equal(model.parameterKeys(project.controls).join(","), "gain");
const component = model.namedComponent("Channel strip", [
    {...project.controls[1], visibleWhen: "enabled = 1", labelFrom: {key:"gain", source:"name"}},
    {kind:"action", label:"Recall", action:"reset-a", x:6, y:1, width:2, height:1}
]);
const copiedComponent = model.readComponent(JSON.stringify(component));
const remapped = model.instantiateComponent(copiedComponent,
    new Map([["gain","channel.b"],["enabled","enabled-b"]]), new Map([["reset-a","reset-b"]]));
assert.equal(remapped[0].key, "channel.b");
assert.equal(remapped[0].labelFrom.key, "channel.b");
assert.equal(model.conditionRules(remapped[0].visibleWhen)[0][0].key, "enabled-b");
assert.equal(remapped[1].action, "reset-b");
assert.deepEqual(remapped[0].phone, component.controls[0].phone);
assert.equal(component.controls[0].key, "gain");
assert.equal(model.bindingContract(remapped).parameters[1].writable, false);
assert.match(CellDesignerRuntime.liveHtml(remapped, project.theme), /^<!-- cell.designer.live.v1 /);
assert.throws(() => model.instantiateComponent(component, new Map([["gain",""]]), new Map()), /Invalid settings/);
const printed = {...project.controls[0], label: "Signal path\\r\\nPanel legend",
    typography: {...model.silkscreen, size: 18.5, tracking: 0}};
const printLayout = model.readProject(model.serialize([printed], project.theme, null));
assert.equal(printLayout.controls[0].label, "Signal path\\nPanel legend");
assert.deepEqual(printLayout.controls[0].typography, printed.typography);
assert.deepEqual(model.readClipboard(model.writeClipboard(printLayout.controls)), printLayout.controls);
assert.equal(model.typographySettings({}), undefined);
assert.equal(model.readLabel(Array(9).fill("line").join("\\n")), null);
assert.equal(model.readLabel("a\\tb"), null);
assert.equal(model.readLabel("a".repeat(241)), null);
assert.throws(() => model.readProject(model.serialize([{...printed, typography: {family: "url(external)"}}], project.theme, null)),
    /Invalid typography/);
assert.throws(() => model.typographySettings({tracking: -3}), /Invalid typography/);
const printHtml = CellDesignerRuntime.html(printLayout.controls, project.theme);
assert.ok(printHtml.includes("data:font/woff2;base64,"));
assert.match(printHtml, /Indian Type Foundry/);
assert.ok(!printHtml.includes("The Inter Project Authors"));
assert.equal((printHtml.match(/@font-face/g) ?? []).length, 1);
assert.ok(!CellDesignerRuntime.html(project.controls, project.theme).includes("@font-face"));
assert.equal(model.meterKeys(project.controls).join(","), "output-peak,output-rms");
assert.ok(!project.controls.some((control, i) => project.controls.slice(i + 1).some(other => model.overlaps(control, other))));
assert.throws(() => model.readProject(model.serialize([{...project.controls[1], artwork:"data:image/svg+xml;base64,AAAA"}], project.theme, null)));
assert.throws(() => model.readProject(model.serialize([{...project.controls[1], height:8}], project.theme, null)));
assert.throws(() => model.readProject(model.serialize([{...project.controls[1],
    phone:{x:3, y:0, width:2, height:1}}], project.theme, null)), /Invalid phone placement/);
assert.throws(() => model.readProject(model.serialize([{...project.controls[1],
    phone:{x:0, y:4095, width:2, height:2}}], project.theme, null)), /Invalid phone placement/);
assert.deepEqual(project.controls[2].phone, {x:2, y:1, width:2, height:3});
const phone = model.phonePlacements(project.controls);
assert.deepEqual(phone[0], {x:0, y:0, width:4, height:1});
assert.deepEqual(phone[3], {x:0, y:4, width:4, height:2});
assert.ok(!phone.some((control, i) => phone.slice(i + 1).some(other => model.overlaps(control, other))));

const boxes = [{x: 0, y: 3, width: 2, height: 1}, {x: 3, y: 4, width: 3, height: 2},
    {x: 9, y: 5, width: 1, height: 1}];
const shifted = model.moveBoxes(boxes, model.wideGrid, 12, -8);
assert.deepEqual(shifted.map(({x,y}) => [x,y]), [[2,0], [5,1], [11,2]]);
assert.deepEqual(model.alignBoxes(boxes, "bottom").map(box => box.y), [5,4,5]);
assert.deepEqual(model.alignBoxes(boxes, "right").map(box => box.x), [8,7,9]);
assert.deepEqual(model.distributeBoxes(boxes, "x").map(box => box.x), [0,4,9]);
assert.equal(model.distributeBoxes(boxes, "y"), null);

const pattern = [{x:0, y:0, width:1, height:1}, {x:2, y:0, width:1, height:1}];
const obstacle = {x:0, y:0, width:1, height:1};
assert.deepEqual(model.findGroupPlacement(pattern, [obstacle], model.phoneGrid),
    [{x:1, y:0, width:1, height:1}, {x:3, y:0, width:1, height:1}]);
assert.equal(model.findGroupPlacement(pattern, [{x:0, y:0, width:12, height:8}], model.wideGrid), null);

const fractionalPattern = [{x:0.5, y:0.25, width:1, height:1}, {x:2.25, y:0.75, width:1, height:1}];
assert.deepEqual(model.findGroupPlacement(fractionalPattern, [fractionalPattern[0]], model.phoneGrid),
    [{x:0, y:2, width:1, height:1}, {x:1.75, y:2.5, width:1, height:1}]);
assert.equal(model.findGroupPlacement(fractionalPattern, [{x:0, y:0, width:12, height:8}], model.wideGrid), null);
assert.deepEqual(model.distributeBoxes([obstacle, {...obstacle, x:2}, {...obstacle, x:4.5}], "x").map(box => box.x), [0, 2.25, 4.5]);
const tilted = {...project.controls[1], x:2.5, y:0.25, rotation:337.5, detentLabels:["A", "", "C"],
    phone:{x:0.5, y:1.25, width:3, height:2}};
const tiltedLayout = model.readProject(model.serialize([tilted], project.theme, null));
assert.deepEqual(tiltedLayout.controls, [{...tilted, rotation: -22.5}]);
assert.deepEqual(model.readClipboard(model.writeClipboard(tiltedLayout.controls)), tiltedLayout.controls);
assert.deepEqual(model.phonePlacements(tiltedLayout.controls), [tilted.phone]);
assert.throws(() => model.readClipboard(model.writeClipboard([{...tilted, rotation:"22"}])), /finite rotation/);
assert.throws(() => model.readClipboard(model.writeClipboard([{...tilted, x:"2.5"}])), /Invalid settings/);
assert.throws(() => model.readClipboard(model.writeClipboard([{...tilted, width:1.5}])), /Invalid settings/);
assert.throws(() => model.readClipboard(model.writeClipboard([{...tilted, detentLabels:["", " "]}])), /detent labels/);
assert.throws(() => model.readClipboard(model.writeClipboard([{...tilted, kind:"slider"}])), /Invalid settings/);

const clipboard = model.writeClipboard([project.controls[2], project.controls[1]]);
assert.equal(JSON.parse(clipboard).schema, "cell.ui-controls.v1");
assert.deepEqual(model.readClipboard(clipboard), [project.controls[2], project.controls[1]]);
assert.ok(!clipboard.includes('"catalog"'));
assert.throws(() => model.readClipboard("another application"), /does not contain Cell controls/);
assert.throws(() => model.readClipboard(model.serialize(project.controls, project.theme, null)), /supported set/);
assert.throws(() => model.readClipboard(model.writeClipboard([])), /supported set/);
assert.throws(() => model.readClipboard(model.writeClipboard([{...project.controls[1], accent: "url(a)"}])), /Invalid settings/);
assert.throws(() => model.readClipboard("x".repeat(model.maxProjectBytes + 1)), /exceeds 2 MiB/);
assert.deepEqual(model.findGroupPlacement([project.controls[2]], [], model.wideGrid),
    [{x:3, y:1, width:3, height:5}]);

const header = model.cppBindings(project.controls);
assert.match(header, /array<cell::ParamId, 1>/);
assert.match(header, /array<std::string_view, 2>/);
assert.equal((header.match(/ControlKind::/g) ?? []).length, 2);
assert.ok(!header.includes('cell::stableId("output-peak")'));

const stepper = {...project.controls[1], kind:"stepper", label:"Program", key:"program", height:1};
const stepped = {key:"program", minimum:0, maximum:11, step:1, choices:[]};
assert.equal(model.stepCount(stepped), 11);
assert.equal(model.stepCount({...stepped, maximum:2.8, step:1}), 3);
assert.equal(model.stepCount({...stepped, step:0}), 0);
const legends = {...tilted, label:"Mode", key:"program"};
assert.deepEqual(model.bindingIssues([legends], [{...stepped, maximum:2}]), []);
assert.deepEqual(model.bindingIssues([legends], [stepped]), ["Mode: supply one detent label for each of the 12 values"]);
assert.deepEqual(model.bindingIssues([legends], [{...stepped, step:0}]), ["Mode: detent labels require 2 to 32 discrete values"]);
assert.deepEqual(model.bindingIssues([{...legends, detentLabels:"choices"}],
    [{...stepped, maximum:2, choices:["Clean", "Warm", "Bright"]}]), []);
assert.deepEqual(model.bindingIssues([stepper], [stepped]), []);
assert.deepEqual(model.bindingIssues([stepper], [{...stepped, step:0}]),
    ["Program: program needs a stepped parameter"]);
assert.equal(model.readProject(model.serialize([stepper], project.theme, null)).controls[0].kind, "stepper");
assert.equal(model.readClipboard(model.writeClipboard([stepper]))[0].kind, "stepper");
assert.match(model.cppBindings([stepper]), /ControlKind::stepper/);

const store = {...stepper, kind: "action", label: "STORE", action: "store", trigger: "hold"};
const pair = {...stepper, kind: "action-pair", label: "Patch", minusAction: "previous-patch",
    plusAction: "next-patch", chordAction: "bank-select"};
const gate = {...stepper, kind: "momentary", label: "Tuner"};
const buttons = [store, pair, gate, {...stepper, chordAction: "bank-select"}];
const buttonLayout = model.readProject(model.serialize(buttons, project.theme, null)).controls;
assert.equal(buttonLayout[0].holdMs, 2000);
assert.equal(buttonLayout[1].chordMs, 1000);
assert.equal(buttonLayout[0].key, "");
assert.equal(buttonLayout[1].key, "");
assert.deepEqual(model.readClipboard(model.writeClipboard(buttonLayout)), buttonLayout);
assert.deepEqual(model.parameterKeys(buttonLayout), ["program"]);
assert.deepEqual(model.actionBindings(buttonLayout).map(({key}) => key),
    ["store", "previous-patch", "next-patch", "bank-select"]);
assert.match(model.cppBindings(buttonLayout), /ControlKind::momentary/);
assert.match(model.cppBindings(buttonLayout), /"store", cell::web::ActionMode::invoke/);
assert.throws(() => model.readProject(model.serialize([{...store, holdMs: 50}], project.theme, null)), /Hold time/);
assert.throws(() => model.readProject(model.serialize([{...pair, chordAction: "../invalid"}], project.theme, null)), /action key/);

const guitar = {...stepper, kind: "choice", label: "DIST", key: "dist_guitar", visibleWhen: "device = 0", phone: undefined};
const bass = {...guitar, key: "dist_bass", visibleWhen: "device = 1"};
const comp = {...stepper, label: "COMP", key: "comp", y: guitar.y + 1, phone: undefined,
    enabledWhen: 'tuner = 0 && "module active" != -1 || device >= 2e0',
    labelFrom: {key: "device", source: "map", labels: [{value: 0, label: "COMP"}, {value: 1, label: "COMP/LIMIT"}]}};
const variantControls = model.readProject(model.serialize([guitar, bass, comp], project.theme, null)).controls;
assert.deepEqual(model.readClipboard(model.writeClipboard(variantControls)), variantControls);
assert.deepEqual(model.parameterKeys(variantControls), ["dist_guitar", "dist_bass", "comp", "device", "tuner", "module active"]);
assert.deepEqual(model.ruleKeys(variantControls[2]), ["tuner", "module active", "device"]);
assert.deepEqual(model.conditionRules(comp.enabledWhen).map(group => group.map(rule => rule.value)), [[0, -1], [2]]);
assert.equal(model.variantsDisjoint(guitar.visibleWhen, bass.visibleWhen), true);
assert.equal(model.variantsDisjoint("device = 0 || device = 1", bass.visibleWhen), false);
const variantPhone = model.phonePlacements(variantControls);
assert.deepEqual(variantPhone[0], variantPhone[1]);
assert.equal(variantPhone[2].y, variantPhone[0].y + variantPhone[0].height);
assert.match(model.cppBindings(variantControls), /array<cell::ParamId, 3> readOnlyParameters/);
assert.match(model.cppBindings(variantControls), /CELL_DESIGNER_HAS_RULE_SOURCES/);
assert.deepEqual(model.bindingIssues([guitar], [], null), ["DIST: unknown rule or label source device"]);
assert.throws(() => model.conditionRules("device = 1 &&"), /numeric conditions/);
assert.throws(() => model.conditionRules("device = 1; alert(1)"), /numeric conditions/);
assert.throws(() => model.ruleSettings({labelFrom: {...comp.labelFrom, labels: [{value: 0, label: "A"}, {value: 0, label: "B"}]}}),
    /unique numeric values/);

const codes = model.parseDisplayCodes("10 = A1\\n11 = oF\\n12 = L7\\n13 = Ac\\n14 = ?1\\n.3 = C");
const led = {...stepper, kind: "led", displayCodes: codes};
const ledLayout = model.readProject(model.serialize([led, {...stepper, segmented: true}], project.theme, null));
assert.deepEqual(ledLayout.controls[0].displayCodes, codes);
assert.deepEqual(model.readClipboard(model.writeClipboard(ledLayout.controls)), ledLayout.controls);
assert.equal(ledLayout.controls[1].segmented, true);
assert.deepEqual(model.parameterKeys(ledLayout.controls), ["program"]);
assert.match(model.cppBindings(ledLayout.controls), /ControlKind::led/);
const staticLed = {kind: "led", label: "Patch legend", ledText: " C", hideHeading: true, x: 0, y: 0, width: 3, height: 1};
const previewLed = {...led, ledText: "CP", hideHeading: true};
const mixedLed = model.readProject(model.serialize([staticLed, previewLed], project.theme, null)).controls;
assert.equal(mixedLed[0].key, "");
assert.equal(mixedLed[0].ledText, " C");
assert.equal(mixedLed[0].hideHeading, true);
assert.deepEqual(model.readClipboard(model.writeClipboard(mixedLed)), mixedLed);
assert.deepEqual(model.parameterKeys(mixedLed), ["program"]);
assert.deepEqual(model.bindingIssues([mixedLed[0]], [], null), []);
assert.match(model.cppBindings([mixedLed[0]]), /array<Binding, 0>/);
assert.match(model.cppBindings(mixedLed), /array<Binding, 1>/);
for (const control of [staticLed, led, {...stepper, segmented: true}]) {
    for (const cells of [2, 4, null]) {
        const invalid = {...control, cells};
        assert.throws(() => model.readProject(model.serialize([invalid], project.theme, null)),
            /Control 1 has an unsupported "cells" field/);
        assert.throws(() => model.readClipboard(model.writeClipboard([staticLed, invalid])),
            /Control 2 has an unsupported "cells" field/);
    }
}
assert.throws(() => model.readProject(model.serialize([{...staticLed, ledText: undefined}], project.theme, null)),
    /supported LED characters/);
assert.throws(() => model.readProject(model.serialize([{...staticLed, ledText: "@1"}], project.theme, null)),
    /supported LED characters/);
assert.throws(() => model.readProject(model.serialize([{...stepper, ledText: "CP"}], project.theme, null)),
    /segmented stepper/);
assert.equal(model.readProject(model.serialize([{...stepper, segmented: true, ledText: "CP"}], project.theme, null))
    .controls[0].ledText, "CP");
assert.equal(model.displayCode(codes, 11, 1), "oF");
assert.equal(model.displayCode(codes, .1 + .2, .1), "C");
assert.equal(model.displayCode(codes, 9, 1), undefined);
assert.equal(model.numericLedCode(7), "07");
assert.equal(model.numericLedCode(-4), "-4");
assert.equal(model.numericLedCode(100), "--");
assert.equal(model.numericLedCode(.7), "--");
assert.notEqual(model.ledGlyphs.o, model.ledGlyphs.O);
assert.ok(model.ledGlyphs["?"].includes("p"));
assert.throws(() => model.parseDisplayCodes("1 = OFF"), /one or two supported/);
assert.throws(() => model.parseDisplayCodes("1 = @1"), /one or two supported/);
const newCodes = ["MR", "NK", "VW", "XY", "Z+", "#1"];
for (const [value, code] of newCodes.entries()) {
    assert.equal(model.parseDisplayCodes(value + " = " + code)[0].code, code);
    assert.equal(model.ledSettings({...staticLed, ledText: code}).ledText, code);
}
assert.throws(() => model.parseDisplayCodes("1 = MNR"), /one or two supported/);
assert.throws(() => model.parseDisplayCodes("0 = oF\\n0 = A1"), /more than one/);
assert.throws(() => model.readClipboard(model.writeClipboard([{...led, displayCodes: [{value: "11", code: "oF"}]}])),
    /finite value/);
assert.throws(() => model.readProject(model.serialize([{...led, segmented: true}], project.theme, null)),
    /Invalid settings/);

const memory = {type: "program-memory", banks: [{number: 4, name: "Bank E",
    programs: [{number: 5, name: "Distant"}]}], active: {bank: 4, program: 5}};
const selector = {kind: "program-grid", label: "Memory", x: 0, y: 0, width: 12, height: 4};
const programLayout = model.readProject(model.serialize([selector], project.theme, [], memory));
assert.deepEqual(programLayout.programs, memory);
assert.equal(model.readClipboard(model.writeClipboard(programLayout.controls))[0].kind, "program-grid");
assert.deepEqual(model.parameterKeys(programLayout.controls), []);
assert.deepEqual(model.bindingIssues(programLayout.controls, [], memory), []);
assert.deepEqual(model.bindingIssues(programLayout.controls, null, null), ["Memory: missing program memory snapshot"]);
assert.throws(() => model.readPrograms({...memory, active: {bank: 4, program: 6}}), /Invalid Cell program memory/);
assert.throws(() => model.readPrograms({...memory, banks: [memory.banks[0], memory.banks[0]]}), /Invalid Cell program memory/);

const panelArt = [
    {kind: "rectangle", label: "Module panel", x: 0, y: 0, width: 6, height: 3, accent: "#123456"},
    {kind: "line", label: "Module divider", x: 6, y: 0, width: 1, height: 3, direction: "vertical", thickness: 5},
    {kind: "image", label: "Logo", x: 0, y: 3, width: 3, height: 2, fit: "cover"}
];
const panelTheme = {...project.theme, backgroundFit: "fill", backgroundArtwork: "data:image/webp;base64,AAAA"};
const panel = model.readProject(model.serialize(panelArt, panelTheme, null));
assert.deepEqual(panel.theme, panelTheme);
assert.deepEqual(panel.controls.map(control => control.key), ["", "", ""]);
assert.deepEqual(model.readClipboard(model.writeClipboard(panel.controls)), panel.controls);
assert.deepEqual(model.parameterKeys(panel.controls), []);
assert.deepEqual(model.bindingIssues(panel.controls, []), []);
assert.match(model.cppBindings(panel.controls), /array<Binding, 0>/);
const mixedStack = [project.controls[1], ...panel.controls.slice().reverse()];
assert.deepEqual(model.layerOrder(mixedStack).map(control => control.kind), ["rectangle", "line", "image", "knob"]);
const layerLayout = model.readProject(model.serialize(mixedStack.map(control =>
    control.kind === "rectangle" ? {...control, layer: 8} : control), panelTheme, null));
assert.deepEqual(model.layerOrder(layerLayout.controls).map(control => control.kind), ["line", "image", "knob", "rectangle"]);
assert.deepEqual(model.readClipboard(model.writeClipboard(layerLayout.controls)), layerLayout.controls);
for (const layer of [0, 1.5, 257, null, "2"])
    assert.throws(() => model.readClipboard(model.writeClipboard([{...panel.controls[0], layer}])), /Invalid layer/);
assert.throws(() => model.readProject(model.serialize([{...panelArt[1], thickness: 17}], panelTheme, null)), /Invalid settings/);
assert.throws(() => model.readProject(model.serialize([], {...panelTheme, backgroundArtwork: "https://example.com/panel"}, null)),
    /invalid background artwork/);

for (const [group, assets] of Object.entries(CellDesignerArtwork)) {
    assert.ok(assets.length >= 3);
    for (const asset of assets)
        assert.ok(model.validArtwork(asset.artwork, group === "panel" ? model.maxPanelAssetBytes : model.maxAssetBytes));
}
for (const family of ["british", "soviet", "german", "field"])
    for (const group of ["panel", "knob", "fader", "display"])
        assert.equal(CellDesignerArtwork[group].filter(asset => asset.family === family).length, 1);

const cap = {...project.controls[1], artwork: CellDesignerArtwork.knob[0].artwork, artworkMode: "cap"};
const display = {kind: "led", label: "Code", ledText: "A1", x: 0, y: 4, width: 3, height: 2,
    artwork: CellDesignerArtwork.display[0].artwork};
const hardware = model.readProject(model.serialize([cap, display], panelTheme, null));
assert.equal(hardware.controls[0].artworkMode, "cap");
assert.equal(hardware.controls[1].artwork, display.artwork);
assert.deepEqual(model.readClipboard(model.writeClipboard(hardware.controls)), hardware.controls);
for (const invalid of [{...cap, artwork: ""}, {...cap, artworkMode: "other"}, {...display, artworkMode: "cap"}])
    assert.throws(() => model.readClipboard(model.writeClipboard([invalid])), /Invalid settings/);

const pointer = {...cap, artworkMode: "pointer-cap"};
assert.equal(model.readClipboard(model.writeClipboard([pointer]))[0].artworkMode, "pointer-cap");
assert.throws(() => model.readClipboard(model.writeClipboard([{...pointer, kind: "fader"}])), /Invalid settings/);
for (const kind of ["toggle", "momentary", "action"]) assert.equal(model.hasArtwork(kind), true);

const lamp = {kind: "indicator", label: "Ready", key: "ready", x: 0, y: 0, width: 3, height: 2,
    artwork: CellDesignerArtwork.indicator[0].artwork};
const meterLamp = {...lamp, label: "Peak", key: "output-peak", x: 3, indicatorSource: "meter"};
const indicators = model.readClipboard(model.writeClipboard([lamp, meterLamp, {...meterLamp, threshold: -12.5}]));
assert.deepEqual(indicators.map(({indicatorSource, threshold}) => [indicatorSource, threshold]),
    [["parameter", 1], ["meter", -18], ["meter", -12.5]]);
assert.equal(indicators[0].artwork, lamp.artwork);
assert.deepEqual(model.parameterKeys(indicators), ["ready"]);
assert.deepEqual(model.meterKeys(indicators), ["output-peak"]);
assert.deepEqual(model.bindingIssues(indicators.slice(1), []), []);
const lampBindings = model.cppBindings(indicators);
assert.ok(lampBindings.includes('{ControlKind::indicator, "ready", cell::stableId("ready")}'));
assert.ok(lampBindings.includes('array<std::string_view, 1> meters{{\\n    "output-peak"'));
assert.ok(lampBindings.includes('array<cell::ParamId, 1> readOnlyParameters{{\\n    cell::stableId("ready")'));
const shared = model.cppBindings([...indicators, {...lamp, kind: "toggle"}]);
assert.ok(shared.includes("array<cell::ParamId, 0> readOnlyParameters"));
for (const override of [{indicatorSource: "action"}, {threshold: "1"}, {threshold: null}, {kind: "toggle", threshold: 1}])
    assert.throws(() => model.readClipboard(model.writeClipboard([{...lamp, ...override}])), /lamp source/);

const hostileLabel = "<" + "/script>";
const html = CellDesignerRuntime.html([{...project.controls[1], label: hostileLabel}], project.theme);
assert.ok(!html.includes('"' + hostileLabel + '"'));
assert.ok(!html.includes("cell-preview-value"));
assert.ok(html.includes('dataset.skin'));
assert.ok(html.includes('"phone":{"x":0,"y":1,"width":2,"height":3}'));
`, context);
console.log("Cell UI Designer migration, layout geometry, clipboard, assets, bindings, and export checks passed.");
