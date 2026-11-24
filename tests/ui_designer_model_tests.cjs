const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const root = path.join(__dirname, "../examples/ui_designer");
const context = vm.createContext({ assert, TextEncoder });
for (const name of ["model.js", "runtime.js"])
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

const hostileLabel = "<" + "/script>";
const html = CellDesignerRuntime.html([{...project.controls[1], label: hostileLabel}], project.theme);
assert.ok(!html.includes('"' + hostileLabel + '"'));
assert.ok(!html.includes("cell-preview-value"));
assert.ok(html.includes('dataset.skin'));
assert.ok(html.includes('"phone":{"x":0,"y":1,"width":2,"height":3}'));
`, context);
console.log("Cell UI Designer migration, layout geometry, clipboard, assets, bindings, and export checks passed.");
