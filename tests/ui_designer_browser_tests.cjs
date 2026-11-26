const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const path = require("node:path");
const os = require("node:os");
const {execFileSync} = require("node:child_process");
const {chromium} = require(process.env.CELL_PLAYWRIGHT_MODULE || "playwright");

const source = path.join(__dirname, "../examples/ui_designer");
const origin = "http://127.0.0.1:8451";
const project = {type: "parameters", parameters: [
    {key: "gain", name: "Gain", minimum: -60, maximum: 12, step: 0.01, value: -6, unit: "dB"},
    {key: "bypass", name: "Bypass", minimum: 0, maximum: 1, step: 1, value: 0, unit: "", choices: ["Active", "Bypassed"]},
    {key: "mode", name: "Mode", minimum: 0, maximum: 2, step: 1, value: 1, unit: "", choices: ["Clean", "Warm", "Bright"]}
]};

async function download(page, id, directory) {
    const pending = page.waitForEvent("download");
    await page.locator(id).click();
    const item = await pending;
    const file = path.join(directory, item.suggestedFilename());
    await item.saveAs(file);
    return fs.readFile(file, "utf8");
}

(async () => {
    const browser = await chromium.launch({executablePath: process.env.CELL_CHROMIUM || undefined});
    const temp = process.env.CELL_UI_DESIGNER_TEST_OUTPUT ||
        await fs.mkdtemp(path.join(os.tmpdir(), "cell-ui-designer-tests-"));
    try {
        const context = await browser.newContext({acceptDownloads: true, viewport: {width: 1280, height: 860}});
        const page = await context.newPage();
        const errors = [];
        page.on("pageerror", error => errors.push(error));
        page.on("dialog", dialog => dialog.accept());
        const serveDesigner = async route => {
            const name = new URL(route.request().url()).pathname.slice(1);
            if (!["index.html", "model.js", "fonts.js", "artwork.js", "runtime.js", "designer.js"].includes(name)) return route.abort();
            await route.fulfill({body: await fs.readFile(path.join(source, name)),
                contentType: name.endsWith(".js") ? "text/javascript" : "text/html; charset=utf-8"});
        };
        await page.route(origin + "/**", serveDesigner);
        await page.goto(origin + "/index.html");
        await page.getByRole("button", {name: "Add knob"}).dragTo(page.locator("#canvas"), {targetPosition: {x: 150, y: 80}});
        const knobTile = page.getByRole("button", {name: "Gain · knob"});
        const original = Number(await page.getByLabel("Column").inputValue());
        await knobTile.press("ArrowRight");
        assert.equal(Number(await page.getByLabel("Column").inputValue()), original + 1);
        await knobTile.press("Shift+ArrowDown");
        assert.equal(await page.getByLabel("Height").inputValue(), "3");
        await page.locator("#undo").click();
        assert.equal(await page.getByLabel("Height").inputValue(), "2");
        await page.locator("#redo").click();
        assert.equal(await page.getByLabel("Height").inputValue(), "3");

        const groupFixture = {schema: "cell.ui-layout.v3", controls: [
            {kind: "knob", label: "Drive", key: "gain", x: 1, y: 1, width: 2, height: 1,
                phone: {x: 0, y: 1, width: 2, height: 2}},
            {kind: "slider", label: "Tone", key: "gain", x: 4, y: 2, width: 2, height: 1,
                phone: {x: 2, y: 1, width: 2, height: 2}},
            {kind: "slider", label: "Mix", key: "gain", x: 8, y: 3, width: 1, height: 1}
        ]};
        await page.locator("#layout-file").setInputFiles({name: "group-layout.json", mimeType: "application/json",
            buffer: Buffer.from(JSON.stringify(groupFixture))});
        await page.getByText("Layout opened.", {exact: true}).waitFor();
        const driveTile = page.getByRole("button", {name: "Drive · knob"});
        const toneTile = page.getByRole("button", {name: "Tone · slider"});
        const mixTile = page.getByRole("button", {name: "Mix · slider"});
        const selectedTiles = page.locator('#canvas .tile[aria-pressed="true"]');
        const coordinates = () => page.locator("#canvas .tile").evaluateAll(tiles => tiles.map(tile => {
            const {style} = tile.parentElement;
            return [
                Math.round(parseFloat(tile.style.left) * Number(style.getPropertyValue("--columns")) / 100),
                Math.round(parseFloat(tile.style.top) * Number(style.getPropertyValue("--rows")) / 100)
            ];
        }));
        await driveTile.click();
        await toneTile.click({modifiers: ["Shift"]});
        assert.equal(await selectedTiles.count(), 2);
        assert.equal(await page.getByRole("button", {name: "Space horizontally"}).isDisabled(), true);
        await mixTile.press("Shift+Space");
        assert.equal(await selectedTiles.count(), 3);
        await page.getByRole("button", {name: "Align top"}).click();
        await page.getByRole("button", {name: "Space horizontally"}).click();
        assert.deepEqual(await coordinates(), [[1,1], [5,1], [8,1]]);
        await driveTile.press("ArrowDown");
        assert.deepEqual(await coordinates(), [[1,2], [5,2], [8,2]]);

        const tileBox = await driveTile.boundingBox();
        const wideCanvas = await page.locator("#canvas").boundingBox();
        const dragX = tileBox.x + tileBox.width / 2, dragY = tileBox.y + tileBox.height / 2;
        await page.mouse.move(dragX, dragY);
        await page.mouse.down();
        await page.mouse.move(dragX + wideCanvas.width / 12, dragY + wideCanvas.height / 8);
        await page.mouse.up();
        assert.deepEqual(await coordinates(), [[2,3], [6,3], [9,3]]);
        await page.locator("#undo").click();
        assert.deepEqual(await coordinates(), [[1,2], [5,2], [8,2]]);
        assert.equal(await selectedTiles.count(), 3);

        await page.getByRole("button", {name: "Duplicate selection"}).click();
        assert.equal(await page.locator("#canvas .tile").count(), 6);
        assert.equal(await selectedTiles.count(), 3);
        assert.equal(await page.locator("#overlap-status").isVisible(), false);
        await page.getByRole("button", {name: "Remove selection"}).click();
        assert.equal(await page.locator("#canvas .tile").count(), 3);
        await page.locator("#undo").click();
        assert.equal(await selectedTiles.count(), 3);
        await page.locator("#redo").click();
        assert.deepEqual(await coordinates(), [[1,2], [5,2], [8,2]]);

        await page.getByLabel("Edit layout").selectOption("phone");
        await driveTile.click();
        const multiSelectButton = page.getByRole("button", {name: "Select multiple"});
        await multiSelectButton.click();
        await toneTile.click();
        assert.equal(await selectedTiles.count(), 2);
        await driveTile.press("ArrowDown");
        assert.deepEqual((await coordinates()).slice(0, 2), [[0,2], [2,2]]);
        await page.getByRole("button", {name: "Duplicate selection"}).click();
        assert.deepEqual((await coordinates()).slice(3), [[0,4], [2,4]]);
        await page.getByRole("button", {name: "Remove selection"}).click();
        await multiSelectButton.click();
        await page.getByLabel("Edit layout").selectOption("wide");
        assert.deepEqual(await coordinates(), [[1,2], [5,2], [8,2]]);

        await page.getByRole("button", {name: "Load showcase"}).click();
        await page.locator("#parameter-file").setInputFiles({name: "parameters.json", mimeType: "application/json",
            buffer: Buffer.from(JSON.stringify(project))});
        await page.getByText("3 parameters loaded", {exact: true}).waitFor();
        const preview = page.frameLocator("#preview-panel iframe");
        await preview.locator('.fader input[aria-valuetext="-6.00 dB"]').waitFor();
        const fader = preview.getByRole("slider", {name: "Studio Fader"});
        assert.equal(await fader.inputValue(), "-6");
        await fader.press("ArrowUp");
        const knob = preview.getByRole("slider", {name: "Field Radio Gain"});
        let gain = Number(await knob.inputValue());
        assert.ok(gain > -6);
        assert.equal(await fader.getAttribute("aria-orientation"), "vertical");

        const knobRect = await knob.boundingBox();
        const x = knobRect.x + knobRect.width / 2, y = knobRect.y + knobRect.height / 2;
        await page.mouse.move(x, y);
        await page.mouse.down();
        await page.mouse.move(x, y - 38, {steps: 4});
        await page.mouse.up();
        assert.ok(Number(await fader.inputValue()) > gain);
        gain = Number(await fader.inputValue());

        const meter = preview.getByRole("meter", {name: "Broadcast Peak"});
        assert.equal(await meter.getAttribute("max"), "0");
        assert.ok(Number(await meter.getAttribute("value")) > -60);
        await page.getByLabel("Viewport").selectOption("phone");
        assert.ok((await page.locator("#preview-panel iframe").boundingBox()).width <= 390);

        await page.getByLabel("Edit layout").selectOption("phone");
        const studioTile = page.getByRole("button", {name: "Studio Fader · fader"});
        await studioTile.click();
        assert.equal(await page.getByLabel("Width").inputValue(), "2");
        await page.getByLabel("Column").fill("1");
        await page.getByRole("button", {name: "Apply", exact: true}).click();
        assert.equal(await page.locator("#overlap-status").isVisible(), true);
        const fieldTile = page.getByRole("button", {name: "Field Radio Gain · knob"});
        await fieldTile.press("ArrowRight");
        await fieldTile.press("ArrowRight");
        assert.equal(await page.getByLabel("Column").inputValue(), "3");
        assert.equal(await page.locator("#overlap-status").isVisible(), false);
        await preview.locator('main[data-layout="phone"] > .fader + .knob').waitFor();
        const knobCard = await preview.locator(".knob").boundingBox();
        const faderCard = await preview.locator(".fader").boundingBox();
        assert.ok(faderCard.x < knobCard.x && faderCard.y === knobCard.y, "phone preview must follow the authored grid");

        await fieldTile.scrollIntoViewIfNeeded();
        const phoneTile = await fieldTile.boundingBox();
        await page.mouse.move(phoneTile.x + phoneTile.width - 8, phoneTile.y + phoneTile.height - 8);
        await page.mouse.down();
        await page.mouse.move(phoneTile.x + phoneTile.width - 8, phoneTile.y + phoneTile.height - 62);
        await page.mouse.up();
        assert.equal(await page.getByLabel("Height").inputValue(), "2");
        await page.locator("#undo").click();
        assert.equal(await page.getByLabel("Height").inputValue(), "3");

        await studioTile.click();
        await page.getByRole("button", {name: "Auto-place on phone"}).click();
        assert.equal(await page.getByLabel("Width").inputValue(), "4");
        await page.locator("#undo").click();
        assert.equal(await page.getByLabel("Width").inputValue(), "2");
        await page.getByRole("button", {name: "Reset phone layout"}).click();
        assert.equal(await page.locator("#reset-phone-layout").isDisabled(), true);
        await page.locator("#undo").click();
        assert.equal(await page.locator("#reset-phone-layout").isDisabled(), false);
        await studioTile.click();
        await page.getByRole("button", {name: "Duplicate", exact: true}).click();
        assert.equal(await studioTile.count(), 2);
        assert.equal(await page.locator("#overlap-status").isVisible(), false);
        await page.getByRole("button", {name: "Remove", exact: true}).click();
        assert.equal(await studioTile.count(), 1);

        await page.getByLabel("Edit layout").selectOption("wide");
        await studioTile.click();
        assert.equal(await page.getByLabel("Column").inputValue(), "4");
        assert.equal(await page.getByLabel("Width").inputValue(), "3");

        await page.getByRole("button", {name: "Field Radio Gain · knob"}).click();
        await page.getByLabel("Label", {exact: true}).fill("Field level");
        await page.getByRole("button", {name: "Apply", exact: true}).click();
        await preview.getByRole("slider", {name: "Field level"}).waitFor();
        assert.equal(Number(await preview.getByRole("slider", {name: "Field level"}).inputValue()), gain);

        const png = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==", "base64");
        await page.locator("#artwork-file").setInputFiles({name: "dial.png", mimeType: "image/png", buffer: png});
        await page.getByText("Artwork embedded in the layout.", {exact: true}).waitFor();
        await page.getByRole("button", {name: "Add toggle"}).click();
        await preview.getByRole("button", {name: "Bypass"}).click();
        assert.equal(await preview.getByRole("button", {name: "Bypass"}).getAttribute("aria-pressed"), "true");
        await page.getByRole("button", {name: "Add choice"}).click();
        const choice = preview.getByRole("combobox", {name: "Mode"});
        assert.equal(await choice.inputValue(), "1");
        await choice.selectOption({label: "Bright"});
        assert.equal(await choice.inputValue(), "2");

        const saved = JSON.parse(await download(page, "#save", temp));
        assert.equal(saved.schema, "cell.ui-layout.v3");
        assert.equal(saved.catalog.length, 3);
        assert.deepEqual(saved.controls.find(control => control.kind === "knob").phone, {x:2, y:1, width:2, height:3});
        assert.deepEqual(saved.controls.find(control => control.kind === "fader").phone, {x:0, y:1, width:2, height:3});
        assert.equal(saved.controls.find(control => control.kind === "knob").artwork.startsWith("data:image/png;"), true);
        const header = await download(page, "#export-cpp", temp);
        assert.match(header, /std::array<cell::ParamId, 3>/);
        assert.match(header, /std::array<std::string_view, 2>/);
        const html = await download(page, "#export", temp);
        assert.ok(!html.includes("cell-preview-value"));
        assert.ok(html.includes("Field level"));

        await page.locator("#layout-file").setInputFiles(path.join(temp, "cell-ui-layout.json"));
        await page.getByText("Layout opened.", {exact: true}).waitFor();
        assert.equal(await page.getByRole("button", {name: "Field level · knob"}).count(), 1);
        assert.equal(await page.locator("#undo").isDisabled(), true);
        await page.getByLabel("Edit layout").selectOption("phone");
        await studioTile.click();
        assert.equal(await page.getByLabel("Width").inputValue(), "2");
        await page.setViewportSize({width:390, height:860});
        await page.getByLabel("Viewport").selectOption("wide");
        await preview.locator('main[data-layout="wide"]').waitFor();
        assert.equal(await page.getByLabel("Edit layout").inputValue(), "phone");

        const exportPage = await browser.newPage({viewport: {width: 390, height: 600}});
        exportPage.on("pageerror", error => errors.push(error));
        await exportPage.addInitScript(() => {
            window.sent = [];
            window.cell = {onMessage(callback) { window.receive = callback; }, postMessage(message) { window.sent.push(message); }};
        });
        await exportPage.route(origin + "/editor.html", route => route.fulfill({body: html, contentType: "text/html; charset=utf-8"}));
        await exportPage.goto(origin + "/editor.html");
        await exportPage.evaluate(({parameters}) => {
            window.receive({type: "parameters", parameters});
            window.receive({type: "meters", meters: [{key: "output-peak", minimum: -60, maximum: 0, value: -12.5, unit: "dBFS"}]});
        }, project);
        assert.equal(await exportPage.getByRole("meter", {name: "Broadcast Peak"}).getAttribute("value"), "-12.5");
        const exportedOrder = () => exportPage.locator(".fader .control-title, .knob .control-title").allTextContents();
        assert.deepEqual(await exportedOrder(), ["Studio Fader", "Field level"]);
        await exportPage.keyboard.press("Tab");
        assert.equal(await exportPage.locator(":focus").getAttribute("aria-label"), "Studio Fader");
        await exportPage.setViewportSize({width:480, height:600});
        await exportPage.locator('main[data-layout="phone"]').waitFor();
        await exportPage.setViewportSize({width:481, height:600});
        await exportPage.locator('main[data-layout="wide"]').waitFor();
        assert.deepEqual(await exportedOrder(), ["Field level", "Studio Fader"]);
        assert.equal(await exportPage.locator(":focus").getAttribute("aria-label"), "Studio Fader");
        await exportPage.setViewportSize({width:390, height:600});
        await exportPage.locator('main[data-layout="phone"]').waitFor();
        assert.deepEqual(await exportedOrder(), ["Studio Fader", "Field level"]);
        const slider = exportPage.getByRole("slider", {name: "Field level"});
        await slider.press("ArrowUp");
        const messages = await exportPage.evaluate(() => window.sent);
        assert.deepEqual(messages.map(message => message.type), ["begin", "parameter", "end"]);
        assert.equal(messages[1].key, "gain");
        await fs.writeFile(path.join(temp, "gestures.jsonl"), messages.map(message => JSON.stringify(message)).join("\n") + "\n");
        await exportPage.evaluate(() => { window.sent.length = 0; });
        await slider.scrollIntoViewIfNeeded();
        const drag = await slider.boundingBox();
        await exportPage.mouse.move(drag.x + drag.width / 2, drag.y + drag.height / 2);
        await exportPage.mouse.down();
        await exportPage.mouse.move(drag.x + drag.width / 2, drag.y + drag.height / 2 - 20);
        await exportPage.setViewportSize({width:700, height:600});
        await exportPage.locator('main[data-layout="wide"]').waitFor();
        await exportPage.mouse.up();
        assert.deepEqual(await exportPage.evaluate(() => window.sent.map(message => message.type)), ["begin", "parameter", "end"]);

        await page.context().grantPermissions(["clipboard-read", "clipboard-write"], {origin});
        await page.bringToFront();
        await page.setViewportSize({width:1280, height:860});
        await studioTile.click();
        await page.getByRole("button", {name: "Field level · knob"}).click({modifiers: ["Shift"]});
        await page.keyboard.press("Control+c");
        await page.getByText("2 controls copied.", {exact: true}).waitFor();
        const copied = await page.evaluate(() => navigator.clipboard.readText());
        const copiedControls = JSON.parse(copied).controls;
        assert.deepEqual(copiedControls.map(control => control.label), ["Field level", "Studio Fader"]);
        assert.equal(copiedControls[0].artwork, saved.controls.find(control => control.kind === "knob").artwork);
        await page.getByRole("button", {name: "Copy selection"}).click();
        await page.getByText("Selection copied to the clipboard.", {exact: true}).waitFor();

        const recipient = await page.context().newPage();
        recipient.on("pageerror", error => errors.push(error));
        recipient.on("dialog", dialog => dialog.accept());
        await recipient.route(origin + "/**", serveDesigner);
        await recipient.goto(origin + "/index.html");
        await recipient.bringToFront();
        const paste = recipient.getByRole("button", {name: "Paste controls"});
        const recipientTiles = recipient.locator("#canvas .tile");
        await paste.focus();
        await recipient.keyboard.press("Control+v");
        await recipient.getByText("2 controls pasted.", {exact: true}).waitFor();
        const transplant = JSON.parse(await download(recipient, "#save", temp));
        assert.deepEqual(transplant.controls, copiedControls);
        assert.equal(transplant.catalog, null);

        await paste.click();
        await recipientTiles.nth(3).waitFor();
        assert.equal(await recipientTiles.count(), 4);
        assert.equal(await recipient.locator("#overlap-status").isVisible(), false);
        await recipient.getByLabel("Edit layout").selectOption("phone");
        assert.equal(await recipient.locator("#overlap-status").isVisible(), false);
        await recipient.locator("#undo").click();
        assert.equal(await recipientTiles.count(), 2);
        await recipient.getByRole("button", {name: "Field level · knob"}).click();
        const labelInput = recipient.getByLabel("Label", {exact: true});
        await labelInput.fill("Native text copy");
        await labelInput.press("Control+a");
        await labelInput.press("Control+c");
        assert.equal(await recipient.evaluate(() => navigator.clipboard.readText()), "Native text copy");
        await labelInput.fill("");
        await labelInput.press("Control+v");
        assert.equal(await labelInput.inputValue(), "Native text copy");
        await paste.click();
        await recipient.getByText("The clipboard does not contain Cell controls.", {exact: true}).waitFor();
        assert.equal(await recipientTiles.count(), 2);

        await recipient.locator("#layout-file").setInputFiles({name: "full-layout.json", mimeType: "application/json",
            buffer: Buffer.from(JSON.stringify({schema: "cell.ui-layout.v3",
                controls: [{kind: "text", label: "Full panel", x: 0, y: 0, width: 12, height: 8}]}))});
        await recipient.getByText("Layout opened.", {exact: true}).waitFor();
        await recipient.evaluate(text => navigator.clipboard.writeText(text), copied);
        await paste.click();
        await recipient.getByText("Make room on the wide grid to paste these controls.", {exact: true}).waitFor();
        assert.equal(await recipientTiles.count(), 1);
        assert.equal(await recipient.locator("#undo").isDisabled(), true);

        const touch = await browser.newPage({viewport: {width:390, height:860}, isMobile: true, hasTouch: true});
        touch.on("pageerror", error => errors.push(error));
        await touch.addInitScript(() => {
            const denied = async () => { throw new DOMException("Clipboard access denied", "NotAllowedError"); };
            Object.defineProperty(navigator, "clipboard", {value: {readText: async () => "Existing text", writeText: denied}});
        });
        await touch.route(origin + "/**", serveDesigner);
        await touch.goto(origin + "/index.html");
        await touch.locator("#layout-file").setInputFiles({name: "touch-layout.json", mimeType: "application/json",
            buffer: Buffer.from(JSON.stringify(groupFixture))});
        await touch.getByText("Layout opened.", {exact: true}).waitFor();
        await touch.getByLabel("Edit layout").selectOption("phone");
        await touch.getByRole("button", {name: "Select multiple"}).tap();
        const touchTone = touch.getByRole("button", {name: "Tone · slider"});
        await touchTone.tap();
        assert.equal(await touch.locator('#canvas .tile[aria-pressed="true"]').count(), 2);
        await touchTone.tap();
        assert.equal(await touch.locator('#canvas .tile[aria-pressed="true"]').count(), 1);
        await touch.getByRole("button", {name: "Copy selection"}).tap();
        await touch.getByText("Selection copied in this tab.", {exact: false}).waitFor();
        await touch.getByRole("button", {name: "Paste controls"}).tap();
        await touch.getByText("Control pasted.", {exact: true}).waitFor();
        assert.equal(await touch.getByRole("button", {name: "Drive · knob"}).count(), 2);

        const panel = await context.newPage();
        panel.on("pageerror", error => errors.push(error));
        panel.on("dialog", dialog => dialog.accept());
        await panel.route(origin + "/**", serveDesigner);
        await panel.goto(origin + "/index.html");
        await panel.getByRole("button", {name: "Add stepper"}).click();
        await panel.getByRole("button", {name: "Live preview"}).click();
        const panelPreview = panel.frameLocator("#preview-panel iframe");
        await panelPreview.getByRole("button", {name: "Increase Code"}).click();
        assert.equal(await panelPreview.locator(".stepper output").textContent(), "01");
        const parameter = {key: "program", name: "Program", minimum: 0, maximum: 11, step: 1, value: 3, unit: "",
            choices: Array.from({length: 12}, (_, index) => "Preset " + index)};
        const fixture = {schema: "cell.ui-layout.v3", catalog: [parameter], controls: [
            {kind: "stepper", label: "Program", key: "program", x: 0, y: 0, width: 4, height: 1},
            {kind: "knob", label: "Selector", key: "program", skin: "field", x: 4, y: 0, width: 4, height: 2, detentLabels: "choices"}
        ]};
        await panel.locator("#layout-file").setInputFiles({name: "discrete-layout.json", mimeType: "application/json",
            buffer: Buffer.from(JSON.stringify(fixture))});
        await panel.getByText("Layout opened.", {exact: true}).waitFor();
        const selector = panelPreview.getByRole("slider", {name: "Selector"});
        assert.equal(await panelPreview.locator(".detents span").count(), 12);
        assert.deepEqual(await panelPreview.locator(".detent-captions span").allTextContents(), parameter.choices);
        assert.equal(await selector.inputValue(), "3");
        await panelPreview.getByRole("button", {name: "Increase Program"}).press("ArrowUp");
        assert.equal(await selector.inputValue(), "4");
        assert.equal(await panelPreview.locator(".stepper output").textContent(), "04");
        assert.equal(await panelPreview.locator(".stepper output").getAttribute("title"), "Preset 4");
        assert.equal(await panelPreview.locator(".detents span").nth(4).getAttribute("data-active"), "");
        assert.equal(await panelPreview.locator(".detent-captions [data-active]").textContent(), "Preset 4");
        await selector.press("ArrowDown");
        assert.equal(await panelPreview.locator(".stepper output").textContent(), "03");
        const captionOverlap = await panelPreview.locator(".detent-captions span").evaluateAll(elements => {
            const boxes = elements.map(element => element.getBoundingClientRect());
            return boxes.some((a, index) => boxes.slice(index + 1).some(b =>
                a.right > b.left && b.right > a.left && a.bottom > b.top && b.bottom > a.top));
        });
        assert.equal(captionOverlap, false);
        await panelPreview.locator(".control.knob").screenshot({path: path.join(temp, "detent-choice-legends.png")});
        const discreteHeader = await download(panel, "#export-cpp", temp);
        assert.match(discreteHeader, /ControlKind::stepper/);
        assert.match(discreteHeader, /std::array<cell::ParamId, 1>/);
        const discreteHtml = await download(panel, "#export", temp);

        const mod = {key: "mod", name: "MOD", minimum: 1, maximum: 37, step: 1, value: 25, unit: ""};
        const modLayout = {schema: fixture.schema, catalog: [mod], controls: [
            {...fixture.controls[1], label: "MOD", key: "mod", x: 0, detentLabels: undefined}
        ]};
        await panel.locator("#layout-file").setInputFiles({name: "mod-selector.json", mimeType: "application/json",
            buffer: Buffer.from(JSON.stringify(modLayout))});
        await panelPreview.getByRole("slider", {name: "MOD"}).waitFor();
        const modTicks = panelPreview.locator(".detents[data-dense] span");
        assert.equal(await modTicks.count(), 37);
        assert.equal(await modTicks.nth(24).getAttribute("data-active"), "");
        assert.equal(await modTicks.first().evaluate(mark => getComputedStyle(mark, "::before").width), "1px");
        assert.equal(await modTicks.nth(24).evaluate(mark => getComputedStyle(mark, "::before").width), "3px");

        const controlsPage = await browser.newPage({viewport: {width: 700, height: 540}, hasTouch: true});
        controlsPage.on("pageerror", error => errors.push(error));
        await controlsPage.addInitScript(() => {
            window.sent = [];
            window.cell = {onMessage(callback) { window.receive = callback; }, postMessage(message) { window.sent.push(message); }};
        });
        await controlsPage.route(origin + "/discrete.html", route =>
            route.fulfill({body: discreteHtml, contentType: "text/html; charset=utf-8"}));
        await controlsPage.goto(origin + "/discrete.html");
        const publish = value => controlsPage.evaluate(parameter => window.receive({type: "parameters", parameters: [parameter]}),
            {...parameter, value});
        await publish(3);
        const plus = controlsPage.getByRole("button", {name: "Increase Program"});
        const minus = controlsPage.getByRole("button", {name: "Decrease Program"});
        const code = controlsPage.locator(".stepper output");
        const messagesFor = () => controlsPage.evaluate(() => window.sent.map(message => message.value ?? message.type));
        await plus.tap();
        assert.deepEqual(await messagesFor(), ["begin", 4, "end"]);
        await publish(8);
        await controlsPage.evaluate(() => { window.sent.length = 0; });
        await plus.hover();
        await controlsPage.mouse.down();
        await controlsPage.waitForFunction(() => window.sent.some(message => message.value === 11));
        assert.equal(await plus.getAttribute("aria-disabled"), "true");
        await controlsPage.mouse.up();
        assert.equal(await code.textContent(), "11");
        assert.deepEqual(await messagesFor(), ["begin", 9, 10, 11, "end"]);
        await plus.click({force: true});
        assert.deepEqual(await messagesFor(), ["begin", 9, 10, 11, "end"]);

        await controlsPage.evaluate(() => { window.sent.length = 0; });
        await minus.hover();
        await controlsPage.mouse.down();
        await controlsPage.setViewportSize({width: 390, height: 540});
        await controlsPage.locator('main[data-layout="phone"]').waitFor();
        await controlsPage.mouse.up();
        assert.deepEqual(await messagesFor(), ["begin", 10, "end"]);

        await publish(3);
        await controlsPage.evaluate(() => { window.sent.length = 0; });
        await minus.focus();
        await controlsPage.keyboard.down("ArrowDown");
        await controlsPage.waitForFunction(() => window.sent.some(message => message.value === 0));
        await controlsPage.keyboard.up("ArrowDown");
        assert.deepEqual(await messagesFor(), ["begin", 2, 1, 0, "end"]);
        assert.equal(await code.textContent(), "00");
        assert.equal(await minus.getAttribute("aria-disabled"), "true");
        await controlsPage.evaluate(() => window.receive({type: "parameters", parameters: []}));
        assert.equal(await plus.isDisabled(), true);
        assert.equal(await code.textContent(), "—");

        const publishDense = maximum => controlsPage.evaluate(parameter =>
            window.receive({type: "parameters", parameters: [parameter]}), {...mod, key: parameter.key, maximum});
        const exportedKnob = controlsPage.getByRole("slider", {name: "Selector"});
        const exportedTicks = controlsPage.locator(".detents span");
        await publishDense(37);
        assert.equal(await exportedTicks.count(), 37);
        assert.equal(await exportedKnob.inputValue(), "24");
        await controlsPage.evaluate(() => { window.sent.length = 0; });
        await exportedKnob.press("ArrowUp");
        assert.equal(await exportedKnob.getAttribute("aria-valuetext"), "26.00");
        assert.equal(await exportedTicks.nth(25).getAttribute("data-active"), "");
        assert.deepEqual(await messagesFor(), ["begin", 26, "end"]);
        await publishDense(64);
        assert.equal(await exportedTicks.count(), 64);
        await publishDense(65);
        assert.equal(await exportedTicks.count(), 0);
        assert.equal(await exportedKnob.inputValue(), "25");

        const artwork = await context.newPage();
        artwork.on("pageerror", error => errors.push(error));
        artwork.on("dialog", dialog => dialog.accept());
        await artwork.route(origin + "/**", serveDesigner);
        await artwork.goto(origin + "/index.html");
        const svg = (name, content) => ({name, mimeType: "image/svg+xml",
            buffer: Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 120 40">${content}</svg>`)});
        const logo = svg("logo.svg", '<style>.mark{fill:url(#ink)}</style>' +
            '<defs><linearGradient id="ink"><stop stop-color="#ffdd99"/></linearGradient>' +
            '<path id="shape" d="M0,35L20,5L40,35Z"/></defs><use class="mark" href="#shape"/>');
        const background = svg("background.svg", '<rect width="120" height="40" fill="#254754"/>' +
            '<!--' + " ".repeat(270 * 1024) + '-->');
        const loadLayout = value => artwork.locator("#layout-file").setInputFiles({name: "layout.json",
            mimeType: "application/json", buffer: Buffer.from(value)});
        await artwork.getByRole("button", {name: "Add slider"}).click();
        await artwork.getByRole("button", {name: "Add rectangle"}).click();
        await artwork.getByRole("button", {name: "Add line"}).click();
        assert.equal(await artwork.getByLabel("Parameter key").isVisible(), false);
        await artwork.getByLabel("Direction").selectOption("vertical");
        await artwork.getByLabel("Thickness (px)").fill("6");
        await artwork.getByRole("button", {name: "Apply", exact: true}).click();
        await artwork.getByRole("button", {name: "Add image"}).click();
        await artwork.getByLabel("Label", {exact: true}).fill("Module logo");
        await artwork.getByLabel("Image fit").selectOption("fill");
        await artwork.getByRole("button", {name: "Apply", exact: true}).click();
        await artwork.locator("#artwork-file").setInputFiles(logo);
        await artwork.getByText("Artwork embedded in the layout.", {exact: true}).waitFor();
        await artwork.locator("#artwork-file").setInputFiles(svg("external.svg", '<use href="https://example.com/file.svg#shape"/>'));
        await artwork.getByText(/SVG artwork must be self-contained/).waitFor();
        await artwork.getByText("Panel background", {exact: true}).click();
        await artwork.locator("#background-file").setInputFiles(background);
        await artwork.getByText("Background embedded in the layout.", {exact: true}).waitFor();
        await artwork.getByLabel("Background fit").selectOption("contain");
        await artwork.getByRole("button", {name: "Remove background image"}).click();
        assert.equal(await artwork.locator("#canvas .panel-background").isVisible(), false);
        await artwork.locator("#undo").click();
        assert.equal(await artwork.locator("#canvas .panel-background").isVisible(), true);
        assert.equal(await artwork.getByLabel("Background fit").inputValue(), "contain");

        await artwork.getByRole("button", {name: "Edit panel artwork"}).click();
        await artwork.getByRole("button", {name: "Rectangle · rectangle"}).click();
        assert.equal(await artwork.getByLabel("Label", {exact: true}).inputValue(), "Rectangle");
        await artwork.getByRole("button", {name: "Live preview"}).click();
        const artPreview = artwork.frameLocator("#preview-panel iframe");
        const verifyArtwork = async frame => {
            assert.equal(await frame.locator(".panel-background").evaluate(img => img.decode().then(() => img.style.objectFit)), "contain");
            const image = frame.getByRole("img", {name: "Module logo"});
            assert.equal(await image.evaluate(img => img.decode().then(() => img.style.objectFit)), "fill");
            assert.equal(await frame.locator(".panel-art.line").evaluate(element =>
                getComputedStyle(element, "::before").width), "6px");
            assert.equal(await frame.locator(".panel-art.rectangle").evaluate(element =>
                getComputedStyle(element, "::before").backgroundColor), "rgb(188, 239, 144)");
        };
        await verifyArtwork(artPreview);
        await artPreview.getByRole("slider", {name: "Gain"}).click();
        await artwork.getByLabel("Viewport").selectOption("phone");
        await artPreview.locator("main[data-layout=phone]").waitFor();
        await verifyArtwork(artPreview);

        const artLayout = await download(artwork, "#save", temp);
        const settings = JSON.parse(artLayout);
        assert.equal(settings.controls[3].artwork, "data:image/svg+xml;base64," + logo.buffer.toString("base64"));
        assert.equal(settings.theme.backgroundFit, "contain");
        await loadLayout(artLayout);
        await artwork.getByText("Layout opened.", {exact: true}).waitFor();
        const artHeader = await download(artwork, "#export-cpp", temp);
        assert.match(artHeader, /array<Binding, 1>/);
        const artHtml = await download(artwork, "#export", temp);
        const standalone = await browser.newPage({viewport: {width: 800, height: 540}});
        standalone.on("pageerror", error => errors.push(error));
        await standalone.route(origin + "/artwork.html", route =>
            route.fulfill({body: artHtml, contentType: "text/html; charset=utf-8"}));
        await standalone.goto(origin + "/artwork.html");
        await verifyArtwork(standalone);
        await standalone.screenshot({path: path.join(temp, "panel-artwork-wide.png")});
        await standalone.setViewportSize({width: 390, height: 540});
        await standalone.locator("main[data-layout=phone]").waitFor();
        await verifyArtwork(standalone);
        await standalone.screenshot({path: path.join(temp, "panel-artwork-phone.png")});

        const active = svg("script.svg", "<script>throw 1</script>");
        settings.theme.backgroundArtwork = "data:image/svg+xml;base64," + active.buffer.toString("base64");
        await loadLayout(JSON.stringify(settings));
        await artwork.getByText("The layout has invalid background artwork.", {exact: true}).waitFor();
        await verifyArtwork(artPreview);

        const native = process.env.CELL_WEB_BRIDGE_TEST_BINARY;
        const nativeSnapshot = (mode, input) => JSON.parse(execFileSync(native, [mode], {input, encoding: "utf8"}));
        const patchName = (bank, program) => `${-60 + 2 * (bank * 6 + program)} dB Trim`;
        const memory = native ? nativeSnapshot("--parameters") : {
            ...project, programs: {type: "program-memory", active: {bank: 5, program: 0},
                banks: Array.from({length: 6}, (_, bank) => ({number: bank, name: "Bank " + "ABCDEF"[bank],
                    programs: Array.from({length: 6}, (_, program) => ({number: program, name: patchName(bank, program)}))}))}
        };
        const memoryDesigner = await context.newPage();
        memoryDesigner.on("pageerror", error => errors.push(error));
        memoryDesigner.on("dialog", dialog => dialog.accept());
        await memoryDesigner.route(origin + "/**", serveDesigner);
        await memoryDesigner.goto(origin + "/index.html");
        await memoryDesigner.getByRole("button", {name: "Add bank", exact: true}).click();
        await memoryDesigner.getByRole("button", {name: "Add program", exact: true}).click();
        await memoryDesigner.getByRole("button", {name: "Add patch name", exact: true}).click();
        await memoryDesigner.getByRole("button", {name: "Add program grid", exact: true}).click();
        await memoryDesigner.getByRole("button", {name: "Add slider", exact: true}).click();
        await memoryDesigner.getByRole("button", {name: "Live preview"}).click();
        const memoryPreview = memoryDesigner.frameLocator("#preview-panel iframe");
        await memoryPreview.getByRole("combobox", {name: "Program", exact: true}).waitFor();
        assert.equal(await memoryPreview.getByRole("combobox", {name: "Program", exact: true}).isDisabled(), true);
        await memoryDesigner.locator("#parameter-file").setInputFiles({name: "program-memory.json", mimeType: "application/json",
            buffer: Buffer.from(JSON.stringify(memory))});
        await memoryDesigner.getByText("6 banks, 36 programs", {exact: true}).waitFor();
        const previewPatch = memoryPreview.getByRole("status", {name: "Patch Name"});
        assert.equal(await previewPatch.textContent(), "0 dB Trim");
        assert.equal(await memoryPreview.locator(".program-buttons button").count(), 36);
        await memoryPreview.getByRole("combobox", {name: "Bank", exact: true}).selectOption("2");
        assert.equal(await previewPatch.textContent(), "0 dB Trim");
        await memoryPreview.getByRole("combobox", {name: "Program", exact: true}).selectOption("4");
        assert.equal(await previewPatch.textContent(), "-28 dB Trim");
        const memoryLayout = await download(memoryDesigner, "#save", temp);
        assert.deepEqual(JSON.parse(memoryLayout).programs, memory.programs);
        await memoryDesigner.locator("#layout-file").setInputFiles({name: "memory-layout.json", mimeType: "application/json",
            buffer: Buffer.from(memoryLayout)});
        await memoryDesigner.getByText("Layout opened.", {exact: true}).waitFor();
        assert.equal(await previewPatch.textContent(), "0 dB Trim");
        assert.match(await download(memoryDesigner, "#export-cpp", temp), /array<Binding, 1>/);
        const memoryHtml = await download(memoryDesigner, "#export", temp);
        const memoryEditor = await browser.newPage({viewport: {width: 390, height: 720}});
        memoryEditor.on("pageerror", error => errors.push(error));
        await memoryEditor.addInitScript(() => {
            window.sent = [];
            window.cell = {onMessage(callback) { window.receive = callback; }, postMessage(message) { window.sent.push(message); }};
        });
        await memoryEditor.route(origin + "/memory.html", route => route.fulfill({body: memoryHtml, contentType: "text/html"}));
        await memoryEditor.goto(origin + "/memory.html");
        const patch = memoryEditor.getByRole("status", {name: "Patch Name"});
        assert.equal(await patch.textContent(), "—");
        await memoryEditor.evaluate(snapshot => window.receive(snapshot), memory);
        const slot = memoryEditor.getByRole("button", {name: "Bank C, 05 -28 dB Trim", exact: true});
        await slot.click();
        assert.equal(await patch.textContent(), "0 dB Trim");
        assert.equal(await slot.getAttribute("aria-pressed"), "false");
        const recallMessages = await memoryEditor.evaluate(() => window.sent);
        assert.deepEqual(recallMessages, [{type: "recall-program", bank: 2, program: 4}]);
        const recalled = native ? nativeSnapshot("--apply-gestures", JSON.stringify(recallMessages[0]) + "\n") :
            {...memory, programs: {...memory.programs, active: {bank: 2, program: 4}}};
        await memoryEditor.evaluate(snapshot => window.receive(snapshot), recalled);
        assert.equal(await patch.textContent(), "-28 dB Trim");
        assert.equal(await slot.getAttribute("aria-pressed"), "true");
        assert.equal(await slot.evaluate(button => document.activeElement === button), true);
        if (native) assert.equal(await memoryEditor.getByRole("slider", {name: "Gain"}).inputValue(), "-28");
        await memoryEditor.screenshot({path: path.join(temp, "program-memory-phone.png"), fullPage: true});

        const digital = await context.newPage();
        digital.on("pageerror", error => errors.push(error));
        digital.on("dialog", dialog => dialog.accept());
        await digital.route(origin + "/**", serveDesigner);
        await digital.goto(origin + "/index.html");
        const choiceLabels = ["Algorithm One", "OFF", "LFO Seven", "AC Coupled", "Unknown", "Manual"];
        const mode = {key: "mode", name: "Module Mode", unit: "", minimum: 10, maximum: 15, step: 1, value: 11,
            choices: choiceLabels};
        const counter = {key: "count", name: "Slot", unit: "", minimum: -9, maximum: 100, step: 1, value: 7};
        const snapshot = (parameters = [mode, counter]) => ({type: "parameters", parameters});
        await digital.locator("#parameter-file").setInputFiles({name: "digital-parameters.json", mimeType: "application/json",
            buffer: Buffer.from(JSON.stringify(snapshot()))});
        await digital.getByText("2 parameters loaded", {exact: true}).waitFor();
        const displayCodes = "10 = A1\n11 = oF\n12 = L7\n13 = Ac\n14 = ?1";
        const codeField = digital.getByLabel("Display codes", {exact: true});
        const apply = digital.getByRole("button", {name: "Apply", exact: true});
        await digital.getByRole("button", {name: "Add led display", exact: true}).click();
        await digital.getByLabel("Label", {exact: true}).fill("Mode LED");
        await digital.getByText("Bound choices", {exact: true}).click();
        assert.deepEqual(await digital.locator("#display-choices dd").allTextContents(), choiceLabels);
        await codeField.fill("11 = OFF");
        await apply.click();
        await digital.getByText("Each display code needs a finite value and one or two supported characters.", {exact: true}).waitFor();
        await codeField.fill(displayCodes);
        await digital.getByLabel("Custom accent").check();
        await digital.getByLabel("Control accent").fill("#ff9748");
        await apply.click();
        await digital.getByRole("button", {name: "Add choice", exact: true}).click();
        await digital.getByLabel("Label", {exact: true}).fill("Mode menu");
        await apply.click();
        await digital.getByRole("button", {name: "Add stepper", exact: true}).click();
        await digital.getByLabel("Label", {exact: true}).fill("Mode stepper");
        await digital.getByLabel("Segmented digits").check();
        await codeField.fill(displayCodes);
        await apply.click();
        await digital.getByRole("button", {name: "Add led display", exact: true}).click();
        await digital.getByLabel("Label", {exact: true}).fill("Slot LED");
        await digital.getByLabel("Parameter key", {exact: true}).fill("count");
        await codeField.fill("7 = C");
        await apply.click();
        await digital.getByRole("button", {name: "Live preview"}).click();

        const digitalPreview = digital.frameLocator("#preview-panel iframe");
        const modeLed = digitalPreview.locator(".led output").first();
        const slotLed = digitalPreview.locator(".led output").nth(1);
        const modeMenu = digitalPreview.getByRole("combobox", {name: "Mode menu"});
        const modeStepper = digitalPreview.locator(".stepper output");
        const activeSegments = output => output.locator(".led-digit").evaluateAll(digits =>
            digits.map(digit => [...digit.querySelectorAll("[data-lit]")].map(segment => segment.dataset.segment).join("")));
        assert.equal(await modeLed.getAttribute("data-code"), "oF");
        assert.equal(await modeLed.getAttribute("aria-label"), "Mode LED value oF, OFF");
        assert.deepEqual(await activeSegments(modeLed), ["cdeg", "aefg"]);
        assert.equal(await modeLed.locator('[data-segment=a]:not([data-lit])').first().evaluate(segment =>
            Number(getComputedStyle(segment).opacity)), .17);
        assert.equal(await modeLed.locator("[data-lit]").first().evaluate(segment =>
            Number(getComputedStyle(segment).opacity)), 1);
        assert.equal(await slotLed.getAttribute("data-code"), " C");
        assert.deepEqual(await activeSegments(slotLed), ["", "adef"]);
        assert.deepEqual(await modeMenu.locator("option").allTextContents(), choiceLabels);
        await digitalPreview.getByRole("button", {name: "Increase Mode stepper"}).click();
        assert.equal(await modeStepper.getAttribute("data-code"), "L7");
        assert.equal(await modeLed.getAttribute("data-code"), "L7");
        assert.equal(await modeMenu.inputValue(), "12");
        for (const [value, code] of [["10", "A1"], ["13", "Ac"], ["14", "?1"]]) {
            await modeMenu.selectOption(value);
            assert.equal(await modeLed.getAttribute("data-code"), code);
            assert.equal(await modeStepper.getAttribute("data-code"), code);
        }
        assert.deepEqual(await activeSegments(modeLed), ["abegp", "bc"]);

        const ledLayoutText = await download(digital, "#save", temp);
        const leds = JSON.parse(ledLayoutText).controls;
        assert.deepEqual(leds[0].displayCodes.map(({code}) => code), ["A1", "oF", "L7", "Ac", "?1"]);
        assert.equal(leds.find(control => control.kind === "stepper").segmented, true);
        await digital.locator("#layout-file").setInputFiles({name: "led-layout.json", mimeType: "application/json",
            buffer: Buffer.from(ledLayoutText)});
        await digital.getByText("Layout opened.", {exact: true}).waitFor();
        await digitalPreview.locator('.led output[data-code="oF"]').waitFor();
        assert.equal(await modeLed.getAttribute("data-code"), "oF");
        assert.equal(await codeField.inputValue(), displayCodes);
        const ledHeader = await download(digital, "#export-cpp", temp);
        assert.match(ledHeader, /ControlKind::led/);
        assert.match(ledHeader, /std::array<cell::ParamId, 2>/);
        const ledEditorHtml = await download(digital, "#export", temp);
        const exportedLED = await browser.newPage({viewport: {width: 390, height: 520}});
        exportedLED.on("pageerror", error => errors.push(error));
        await exportedLED.addInitScript(() => {
            window.sent = [];
            window.cell = {onMessage(callback) { window.receive = callback; }, postMessage(message) { window.sent.push(message); }};
        });
        await exportedLED.route(origin + "/digital.html", route =>
            route.fulfill({body: ledEditorHtml, contentType: "text/html; charset=utf-8"}));
        await exportedLED.goto(origin + "/digital.html");
        const exportedMode = exportedLED.locator(".led output").first();
        const exportedSlot = exportedLED.locator(".led output").nth(1);
        assert.equal(await exportedMode.getAttribute("data-code"), "  ");
        assert.equal(await exportedMode.getAttribute("aria-label"), "Mode LED value Unavailable");
        const sendSnapshot = parameters => exportedLED.evaluate(message => window.receive(message), snapshot(parameters));
        await sendSnapshot([{...mode, value: 14}, {...counter, value: 8}]);
        assert.equal(await exportedMode.getAttribute("data-code"), "?1");
        assert.equal(await exportedSlot.getAttribute("data-code"), "08");
        await exportedLED.screenshot({path: path.join(temp, "segmented-display-phone.png"), fullPage: true});
        await sendSnapshot([{...mode, value: 15}, {...counter, value: 100}]);
        assert.equal(await exportedMode.getAttribute("data-code"), "15");
        assert.equal(await exportedSlot.getAttribute("data-code"), "--");
        await sendSnapshot([{...counter, value: -4}]);
        assert.equal(await exportedMode.getAttribute("data-code"), "  ");
        assert.equal(await exportedSlot.getAttribute("data-code"), "-4");
        assert.deepEqual(await exportedLED.evaluate(() => window.sent), []);

        const pedal = await context.newPage();
        pedal.on("dialog", dialog => dialog.accept());
        pedal.on("pageerror", error => errors.push(error));
        await pedal.route(origin + "/**", serveDesigner);
        await pedal.goto(origin + "/index.html");
        const pedalField = name => pedal.locator(`#properties [name="${name}"]`);
        const applyLed = () => pedal.getByRole("button", {name: "Apply", exact: true}).click();
        await pedal.getByRole("button", {name: "Add led display", exact: true}).click();
        await pedalField("label").fill("Patch legend");
        await pedalField("led-source").selectOption("static");
        assert.equal(await pedalField("key").isVisible(), false);
        await pedalField("led-text").fill("A1");
        await pedalField("hide-led-heading").check();
        await applyLed();
        await pedal.getByRole("button", {name: "Add led display", exact: true}).click();
        await pedalField("label").fill("Compression");
        await pedalField("led-source").selectOption("idle");
        assert.equal(await pedalField("key").isVisible(), true);
        await pedalField("led-text").fill("CP");
        await pedalField("hide-led-heading").check();
        await pedalField("display-codes").fill("1 = oF\n2 = #1");
        await applyLed();
        await pedal.getByRole("button", {name: "Add stepper", exact: true}).click();
        await pedalField("label").fill("Compression adjust");
        await pedalField("segmented").check();
        await pedalField("led-text").fill("CP");
        await pedalField("hide-led-heading").check();
        await applyLed();
        await pedal.getByRole("button", {name: "Live preview", exact: true}).click();
        const pedalPreview = pedal.frameLocator("#preview-panel iframe");
        const patchLegend = pedalPreview.locator(".led output").first();
        const idlePreview = pedalPreview.locator(".led output").nth(1);
        assert.equal(await patchLegend.getAttribute("data-code"), "A1");
        assert.equal(await patchLegend.getAttribute("aria-label"), "Patch legend code A1");
        assert.equal(await idlePreview.getAttribute("data-code"), "CP");
        await pedalPreview.getByRole("button", {name: "Increase Compression adjust", exact: true}).click();
        assert.equal(await idlePreview.getAttribute("data-code"), "oF");
        assert.equal(await pedalPreview.locator(".stepper output").getAttribute("data-code"), "01");
        assert.equal(await pedalPreview.locator(".control-title").count(), 0);

        const pedalLayout = await download(pedal, "#save", temp);
        assert.equal(JSON.parse(pedalLayout).controls[0].key, "");
        await pedal.locator("#layout-file").setInputFiles({name: "led-codes.json", mimeType: "application/json",
            buffer: Buffer.from(pedalLayout)});
        await pedal.getByText("Layout opened.", {exact: true}).waitFor();
        assert.equal(await pedalField("led-source").inputValue(), "static");
        assert.equal(await pedalField("led-text").inputValue(), "A1");
        const pedalHeader = await download(pedal, "#export-cpp", temp);
        assert.match(pedalHeader, /std::array<Binding, 2>/);
        assert.match(pedalHeader, /std::array<cell::ParamId, 1>/);
        assert.ok(!pedalHeader.includes('stableId("")'));
        const pedalHtml = await download(pedal, "#export", temp);
        const noBridge = await browser.newPage();
        noBridge.on("pageerror", error => errors.push(error));
        await noBridge.route(origin + "/static-led.html", route =>
            route.fulfill({body: pedalHtml, contentType: "text/html; charset=utf-8"}));
        await noBridge.goto(origin + "/static-led.html");
        assert.equal(await noBridge.locator(".led output").first().getAttribute("data-code"), "A1");
        assert.equal(await noBridge.locator(".led output").nth(1).getAttribute("data-code"), "CP");

        const hostPedal = await browser.newPage({viewport: {width: 390, height: 420}});
        hostPedal.on("pageerror", error => errors.push(error));
        await hostPedal.clock.install();
        await hostPedal.clock.pauseAt(new Date(Date.now() + 1000));
        await hostPedal.addInitScript(() => {
            window.sent = [];
            window.cell = {onMessage(callback) { window.receive = callback; }, postMessage(message) { window.sent.push(message); }};
        });
        await hostPedal.route(origin + "/pedal-led.html", route =>
            route.fulfill({body: pedalHtml, contentType: "text/html; charset=utf-8"}));
        await hostPedal.goto(origin + "/pedal-led.html");
        const idleOutput = hostPedal.locator(".led output").nth(1);
        const publishCode = value => hostPedal.evaluate(value => window.receive({
            type: "parameters", parameters: value === undefined ? [] : [{key: "code", minimum: 0, maximum: 99, value, step: 1}]
        }), value);
        await publishCode(0);
        assert.equal(await idleOutput.getAttribute("data-code"), "CP");
        await idleOutput.evaluate(element => { window.firstCell = element.querySelector(".led-digit"); });
        await publishCode(1);
        assert.equal(await idleOutput.getAttribute("data-code"), "oF");
        await hostPedal.clock.runFor(900);
        await publishCode(1);
        await hostPedal.clock.runFor(299);
        assert.equal(await idleOutput.getAttribute("data-code"), "oF");
        await hostPedal.clock.runFor(1);
        assert.equal(await idleOutput.getAttribute("data-code"), "CP");
        await publishCode(8);
        assert.equal(await idleOutput.getAttribute("data-code"), "08");
        await hostPedal.clock.runFor(1000);
        await publishCode(9);
        await hostPedal.clock.runFor(300);
        assert.equal(await idleOutput.getAttribute("data-code"), "09");
        assert.equal(await idleOutput.evaluate(element => element.querySelector(".led-digit") === window.firstCell), true);
        await publishCode(2);
        assert.equal(await idleOutput.getAttribute("data-code"), "#1");
        assert.equal(await idleOutput.locator(".led-stroke").count(), 4);
        await publishCode(3);
        assert.equal(await idleOutput.getAttribute("data-code"), "03");
        assert.equal(await idleOutput.locator(".led-stroke").count(), 0);
        await publishCode(undefined);
        assert.equal(await idleOutput.getAttribute("data-code"), "CP");
        assert.deepEqual(await hostPedal.evaluate(() => window.sent), []);
        await hostPedal.screenshot({path: path.join(temp, "led-codes-phone.png"), fullPage: true});
        await fs.writeFile(path.join(temp, "led-codes-layout.json"), pedalLayout);

        const glyphPairs = ["MR", "NK", "VW", "XY", "Z+", "#1", "dS", "oF", "Ac", "L7", "SF"];
        const extendedGlyphs = new Set("MNRKVWXYZ+#");
        const glyphLayout = {
            schema: "cell.ui-layout.v3",
            controls: glyphPairs.map((ledText, index) => ({
                kind: "led", label: ledText, ledText, hideHeading: true,
                x: (index % 4) * 3, y: Math.floor(index / 4), width: 3, height: 1
            }))
        };
        await pedal.locator("#layout-file").setInputFiles({name: "extended-glyphs.json", mimeType: "application/json",
            buffer: Buffer.from(JSON.stringify(glyphLayout))});
        await pedal.getByText("Layout opened.", {exact: true}).waitFor();
        await pedalPreview.locator('.led output[data-code="MR"]').waitFor();
        assert.equal(await pedalField("led-text").inputValue(), "MR");
        const invalidLed = {...glyphLayout.controls[0], cells: 2};
        await pedal.locator("#layout-file").setInputFiles({name: "invalid-led.json", mimeType: "application/json",
            buffer: Buffer.from(JSON.stringify({...glyphLayout, controls: [invalidLed]}))});
        await pedal.getByText(/^Control 1 has an unsupported "cells" field/).waitFor();
        assert.equal(await pedalPreview.locator(".led output").count(), glyphPairs.length);
        await pedal.evaluate(controls => navigator.clipboard.writeText(JSON.stringify({schema: "cell.ui-controls.v1", controls})),
            [glyphLayout.controls[0], invalidLed]);
        await pedal.getByRole("button", {name: "Paste controls", exact: true}).click();
        await pedal.getByText(/^Control 2 has an unsupported "cells" field/).waitFor();
        assert.equal(await pedalPreview.locator(".led output").count(), glyphPairs.length);
        const supported = (await pedal.locator("#display-glyphs").textContent()).split(" ");
        assert.ok([...extendedGlyphs].every(glyph => supported.includes(glyph)));
        const glyphHtml = await download(pedal, "#export", temp);
        await noBridge.route(origin + "/extended-led.html", route =>
            route.fulfill({body: glyphHtml, contentType: "text/html; charset=utf-8"}));
        await noBridge.goto(origin + "/extended-led.html");
        const glyphsShown = await noBridge.locator(".led output").evaluateAll(outputs => outputs.map(output => ({
            code: output.dataset.code,
            cells: [...output.querySelectorAll(".led-digit")].map(digit => ({
                character: digit.dataset.glyph,
                lit: [...digit.querySelectorAll(".led-segment[data-lit]")].map(segment => segment.dataset.segment).join(""),
                strokes: [...digit.querySelectorAll(".led-stroke")].map(stroke => getComputedStyle(stroke).clipPath)
            }))
        })));
        assert.deepEqual(glyphsShown.map(({code}) => code), glyphPairs);
        for (const {cells} of glyphsShown) for (const {character, lit, strokes} of cells) {
            if (extendedGlyphs.has(character)) {
                assert.ok(strokes.length >= 2);
                assert.ok(strokes.every(clip => clip.startsWith("polygon(")));
                assert.equal(lit, "");
            } else {
                assert.ok(lit.length);
                assert.equal(strokes.length, 0);
            }
        }
        assert.deepEqual(glyphsShown[7].cells.map(cell => cell.lit), ["cdeg", "aefg"]);
        await noBridge.screenshot({path: path.join(temp, "led-glyphs.png"), fullPage: true});
        await fs.writeFile(path.join(temp, "led-glyphs-layout.json"), JSON.stringify(glyphLayout, null, 2) + "\n");

        const commandDesigner = await context.newPage();
        commandDesigner.on("pageerror", error => errors.push(error));
        commandDesigner.on("dialog", dialog => dialog.accept());
        await commandDesigner.route(origin + "/**", serveDesigner);
        await commandDesigner.goto(origin + "/index.html");
        const commandField = name => commandDesigner.getByLabel(name, {exact: true});
        const saveControl = () => commandDesigner.getByRole("button", {name: "Apply", exact: true}).click();
        const addButton = async (kind, label) => {
            await commandDesigner.getByRole("button", {name: "Add " + kind, exact: true}).click();
            await commandField("Label").fill(label);
        };
        const addAction = async (label, key, trigger) => {
            await addButton("action", label);
            await commandField("Action key").fill(key);
            await commandDesigner.locator('select[name="trigger"]').selectOption(trigger);
            await saveControl();
        };
        await addAction("STORE", "store", "hold");
        await addAction("Factory Recall", "factory-recall", "tap");
        await addAction("Tuner", "tuner", "momentary");
        await addAction("Scan", "scan", "repeat");
        await addButton("action pair", "Patch");
        await commandField("Two-button action key").fill("bank-select");
        await saveControl();
        await addButton("momentary", "GATE");
        await saveControl();
        await addButton("toggle", "BYPASS");
        await saveControl();
        await commandDesigner.getByRole("button", {name: "Live preview"}).click();
        const commandPreview = commandDesigner.frameLocator("#preview-panel iframe");
        await commandPreview.getByRole("button", {name: "Factory Recall"}).click();
        await commandDesigner.getByText("Simulated action factory-recall (invoke)", {exact: true}).waitFor();
        const commandLayout = await download(commandDesigner, "#save", temp);
        const savedButtons = JSON.parse(commandLayout).controls;
        assert.equal(savedButtons[0].holdMs, 2000);
        assert.equal(savedButtons[4].chordMs, 1000);
        await commandDesigner.locator("#layout-file").setInputFiles({name: "commands.json", mimeType: "application/json",
            buffer: Buffer.from(commandLayout)});
        await commandDesigner.getByText("Layout opened.", {exact: true}).waitFor();
        assert.equal(await commandField("Hold time (ms)").inputValue(), "2000");
        const commandHeader = await download(commandDesigner, "#export-cpp", temp);
        assert.match(commandHeader, /std::array<cell::web::ActionBinding, 7>/);
        assert.match(commandHeader, /"tuner", cell::web::ActionMode::momentary/);
        assert.match(commandHeader, /std::array<cell::ParamId, 2>/);
        const commandHtml = await download(commandDesigner, "#export", temp);

        const commandEditor = await browser.newPage({viewport: {width: 780, height: 560}, hasTouch: true});
        commandEditor.on("pageerror", error => errors.push(error));
        await commandEditor.clock.install();
        await commandEditor.clock.pauseAt(new Date(Date.now() + 1000));
        await commandEditor.addInitScript(() => {
            window.sent = [];
            window.cell = {onMessage(callback) { window.receive = callback; }, postMessage(message) { window.sent.push(message); }};
        });
        await commandEditor.route(origin + "/commands.html", route => route.fulfill({body: commandHtml, contentType: "text/html"}));
        await commandEditor.goto(origin + "/commands.html");
        const button = name => commandEditor.getByRole("button", {name, exact: true});
        assert.equal(await button("STORE").isDisabled(), true);
        const availableActions = ["store", "factory-recall", "scan", "previous-patch", "next-patch", "bank-select"]
            .map(key => ({key, mode: "invoke"})).concat({key: "tuner", mode: "momentary"});
        await commandEditor.evaluate(snapshot => window.receive(snapshot), {type: "parameters", momentary: true,
            actions: availableActions,
            parameters: [{key: "gate", minimum: 0, maximum: 1, value: 0, step: 1}, project.parameters[1]]});
        const takeMessages = () => commandEditor.evaluate(() => window.sent.splice(0));
        const invoke = (key, phase = "invoke") => ({type: "action", key, phase});
        await button("Factory Recall").click({force: true});
        const factoryRecall = await takeMessages();
        assert.deepEqual(factoryRecall, [invoke("factory-recall")]);
        if (native) {
            const recalled = nativeSnapshot("--apply-gestures", [
                {type: "recall-program", bank: 2, program: 4}, ...factoryRecall
            ].map(message => JSON.stringify(message) + "\n").join(""));
            assert.equal(recalled.parameters[0].value, 0);
            assert.deepEqual(recalled.programs.active, {bank: 5, program: 0});
        }
        assert.equal(await button("BYPASS").textContent(), "BYPASS");
        await button("STORE").click({force: true});
        assert.deepEqual(await takeMessages(), []);
        await button("STORE").focus();
        await commandEditor.keyboard.down("Space");
        await commandEditor.clock.runFor(1999);
        assert.deepEqual(await takeMessages(), []);
        await commandEditor.clock.runFor(1);
        assert.deepEqual(await takeMessages(), [invoke("store")]);
        await commandEditor.clock.runFor(2000);
        await commandEditor.keyboard.up("Space");
        assert.deepEqual(await takeMessages(), []);
        await button("Scan").focus();
        await commandEditor.keyboard.down("Enter");
        await commandEditor.clock.runFor(350 + 2 * 85);
        await commandEditor.keyboard.up("Enter");
        await commandEditor.clock.runFor(400);
        assert.deepEqual(await takeMessages(), Array.from({length: 4}, () => invoke("scan")));
        await button("Tuner").focus();
        await commandEditor.keyboard.down("Space");
        assert.deepEqual(await takeMessages(), [invoke("tuner", "press")]);
        await commandEditor.evaluate(() => window.dispatchEvent(new Event("blur")));
        await commandEditor.keyboard.up("Space");
        assert.deepEqual(await takeMessages(), [invoke("tuner", "release")]);
        await button("GATE").focus();
        await commandEditor.keyboard.down("Space");
        await commandEditor.setViewportSize({width: 390, height: 620});
        await commandEditor.locator("main[data-layout=phone]").waitFor();
        await commandEditor.keyboard.up("Space");
        assert.deepEqual(await takeMessages(), ["momentary-press", "momentary-release"].map(type => ({type, key: "gate"})));
        const down = button("Decrease Patch"), up = button("Increase Patch");
        await down.focus();
        await commandEditor.keyboard.down("ArrowLeft");
        await commandEditor.keyboard.down("ArrowRight");
        await commandEditor.clock.runFor(500);
        await commandEditor.keyboard.up("ArrowRight");
        await commandEditor.clock.runFor(600);
        await commandEditor.keyboard.up("ArrowLeft");
        assert.deepEqual(await takeMessages(), []);
        await commandEditor.keyboard.down("ArrowLeft");
        await commandEditor.keyboard.down("ArrowRight");
        await commandEditor.clock.runFor(999);
        assert.deepEqual(await takeMessages(), []);
        await commandEditor.clock.runFor(1);
        await commandEditor.keyboard.up("ArrowLeft");
        await commandEditor.keyboard.up("ArrowRight");
        assert.deepEqual(await takeMessages(), [invoke("bank-select")]);
        await up.click({force: true});
        assert.deepEqual(await takeMessages(), [invoke("next-patch")]);

        const touchSession = await commandEditor.context().newCDPSession(commandEditor);
        const touchPoints = await Promise.all([down, up].map(async (locator, id) => {
            const box = await locator.boundingBox();
            return {x: box.x + box.width / 2, y: box.y + box.height / 2, id};
        }));
        await touchSession.send("Input.dispatchTouchEvent", {type: "touchStart", touchPoints});
        await commandEditor.clock.runFor(1000);
        await touchSession.send("Input.dispatchTouchEvent", {type: "touchEnd", touchPoints: []});
        assert.deepEqual(await takeMessages(), [invoke("bank-select")]);
        await commandEditor.screenshot({path: path.join(temp, "action-buttons-phone.png"), fullPage: true});

        const variants = await context.newPage();
        variants.on("pageerror", error => errors.push(error));
        variants.on("dialog", dialog => dialog.accept());
        await variants.route(origin + "/**", serveDesigner);
        await variants.goto(origin + "/index.html");
        const parameterField = (key, name, maximum, choices = []) =>
            ({key, name, unit: "", minimum: 0, maximum, step: 1, value: 0, choices});
        const variantCatalog = [
            parameterField("device", "Device", 1, ["505 II", "506 II"]),
            parameterField("dist_guitar", "Guitar distortion", 2, ["Clean", "Drive", "High gain"]),
            parameterField("dist_bass", "Bass distortion", 1, ["Bass clean", "Bass drive"]),
            parameterField("comp", "Compression", 3),
            parameterField("gate", "Gate", 1),
            parameterField("tuner", "Tuner", 1)
        ];
        const variantFixture = {schema: "cell.ui-layout.v3", catalog: variantCatalog, controls: [
            {kind: "choice", label: "Device", key: "device", x: 0, y: 0, width: 4, height: 1},
            {kind: "text", label: "Device panel", labelFrom: {key: "device", source: "choice"}, x: 4, y: 0, width: 8, height: 1},
            {kind: "choice", label: "DIST", key: "dist_guitar", visibleWhen: "device = 0", x: 0, y: 1, width: 6, height: 1},
            {kind: "choice", label: "DIST", key: "dist_bass", visibleWhen: "device = 1", x: 0, y: 1, width: 6, height: 1},
            {kind: "stepper", label: "COMP", key: "comp", x: 6, y: 1, width: 6, height: 1},
            {kind: "momentary", label: "Gate", key: "gate", visibleWhen: "device = 0", x: 0, y: 2, width: 4, height: 1}
        ]};
        const openVariants = data => variants.locator("#layout-file").setInputFiles({name: "shared-panel.json",
            mimeType: "application/json", buffer: Buffer.from(data)});
        await openVariants(JSON.stringify(variantFixture));
        await variants.getByText("Layout opened.", {exact: true}).waitFor();
        assert.equal(await variants.locator("#overlap-status").isVisible(), false);
        await variants.locator("#rule-element").selectOption("2");
        await variants.getByText("Conditional behavior", {exact: true}).click();
        const visibleWhen = variants.getByLabel("Visible when", {exact: true});
        assert.equal(await visibleWhen.inputValue(), "device = 0");
        await visibleWhen.fill("device = 0 &&");
        await variants.getByRole("button", {name: "Apply", exact: true}).click();
        await variants.getByText(/Use numeric conditions/).waitFor();
        await visibleWhen.fill("device = 0");
        await variants.getByRole("button", {name: "Apply", exact: true}).click();
        await variants.getByRole("button", {name: "COMP · stepper", exact: true}).click();
        await variants.getByLabel("Enabled when", {exact: true}).fill("tuner = 0 && device >= 0");
        await variants.getByLabel("Label from", {exact: true}).fill("device");
        await variants.locator('select[name="label-source"]').selectOption("map");
        await variants.getByLabel("Labels by value", {exact: true}).fill("0 = COMP\n1 = COMP/LIMIT");
        await variants.getByRole("button", {name: "Apply", exact: true}).click();
        await variants.getByRole("button", {name: "Live preview"}).click();
        const variantPreview = variants.frameLocator("#preview-panel iframe");
        const previewDevice = variantPreview.getByRole("combobox", {name: "Device", exact: true});
        const previewDist = variantPreview.getByRole("combobox", {name: "DIST", exact: true});
        await variantPreview.locator(".text .control-title").filter({hasText: "505 II"}).waitFor();
        assert.deepEqual(await previewDist.locator("option").allTextContents(), variantCatalog[1].choices);
        await previewDevice.selectOption("1");
        await variantPreview.getByRole("button", {name: "Increase COMP/LIMIT"}).waitFor();
        assert.deepEqual(await previewDist.locator("option").allTextContents(), variantCatalog[2].choices);
        assert.equal(await variantPreview.getByRole("button", {name: "Gate", exact: true}).count(), 0);
        await previewDevice.selectOption("0");
        assert.deepEqual(await previewDist.locator("option").allTextContents(), variantCatalog[1].choices);

        const variantSaved = await download(variants, "#save", temp);
        assert.equal(JSON.parse(variantSaved).controls[4].labelFrom.labels[1].label, "COMP/LIMIT");
        await openVariants(variantSaved);
        await variants.getByText("Layout opened.", {exact: true}).waitFor();
        const variantHeader = await download(variants, "#export-cpp", temp);
        assert.match(variantHeader, /array<cell::ParamId, 6> parameters/);
        assert.match(variantHeader, /array<cell::ParamId, 1> readOnlyParameters/);
        const variantHtml = await download(variants, "#export", temp);

        const variantEditor = await browser.newPage({viewport: {width: 390, height: 600}});
        variantEditor.on("pageerror", error => errors.push(error));
        await variantEditor.addInitScript(() => {
            window.sent = [];
            window.cell = {onMessage(callback) { window.receive = callback; }, postMessage(message) { window.sent.push(message); }};
        });
        await variantEditor.route(origin + "/variants.html", route => route.fulfill({body: variantHtml, contentType: "text/html"}));
        await variantEditor.goto(origin + "/variants.html");
        const activeDist = variantEditor.getByRole("combobox", {name: "DIST", exact: true});
        assert.equal(await activeDist.count(), 0);
        const updateVariant = (device, tuner = 0) => variantEditor.evaluate(message => window.receive(message), {
            type: "parameters", momentary: true,
            parameters: variantCatalog.filter(parameter => device !== undefined || parameter.key !== "device").map(parameter => ({
                ...parameter, value: ({device, tuner})[parameter.key] ?? parameter.value
            }))
        });
        await updateVariant(0);
        const guitarPosition = await activeDist.boundingBox();
        await activeDist.focus();
        await updateVariant(1);
        assert.deepEqual(await activeDist.locator("option").allTextContents(), variantCatalog[2].choices);
        const bassPosition = await activeDist.boundingBox();
        assert.equal(guitarPosition.y, bassPosition.y);
        assert.equal(guitarPosition.x, bassPosition.x);
        assert.equal(await activeDist.evaluate(element => document.activeElement === element), true);
        const compButton = variantEditor.getByRole("button", {name: "Increase COMP/LIMIT"});
        await compButton.focus();
        await variantEditor.keyboard.down("ArrowUp");
        await updateVariant(1, 1);
        await variantEditor.keyboard.up("ArrowUp");
        assert.equal(await compButton.isDisabled(), true);
        assert.deepEqual(await variantEditor.evaluate(() => window.sent.splice(0)), [
            {type: "begin", key: "comp"}, {type: "parameter", key: "comp", value: 1}, {type: "end", key: "comp"}
        ]);
        await updateVariant(0);
        const gate = variantEditor.getByRole("button", {name: "Gate", exact: true});
        await gate.focus();
        await variantEditor.keyboard.down("Space");
        await updateVariant(1);
        await variantEditor.keyboard.up("Space");
        assert.deepEqual(await variantEditor.evaluate(() => window.sent.splice(0)), [
            {type: "momentary-press", key: "gate"}, {type: "momentary-release", key: "gate"}
        ]);
        assert.equal(await compButton.isEnabled(), true);
        await variantEditor.screenshot({path: path.join(temp, "506-variant-phone.png"), fullPage: true});
        await updateVariant(0);
        await variantEditor.screenshot({path: path.join(temp, "505-variant-phone.png"), fullPage: true});
        await updateVariant(undefined);
        assert.equal(await activeDist.count(), 0);
        await variantEditor.getByText("Device panel", {exact: true}).waitFor();

        const typeDesigner = await context.newPage();
        typeDesigner.on("pageerror", error => errors.push(error));
        typeDesigner.on("dialog", dialog => dialog.accept());
        await typeDesigner.route(origin + "/**", serveDesigner);
        await typeDesigner.goto(origin + "/index.html");
        const typeField = name => typeDesigner.locator(`#properties [name="${name}"]`);
        const applyType = () => typeDesigner.getByRole("button", {name: "Apply", exact: true}).click();
        const legend = "Signal path\nInput processing";
        await typeDesigner.getByRole("button", {name: "Add text", exact: true}).click();
        await typeField("label").fill(legend);
        await typeDesigner.getByText("Typography", {exact: true}).click();
        await typeDesigner.getByRole("button", {name: "Silkscreen", exact: true}).click();
        assert.equal(await typeField("type-family").inputValue(), "panel-condensed");
        await applyType();
        const legendTile = typeDesigner.locator("#canvas .tile-label");
        const readType = locator => locator.evaluate(element => {
            const style = getComputedStyle(element);
            return [style.fontFamily, style.fontSize, style.fontWeight, style.letterSpacing,
                style.textAlign, style.textTransform, style.whiteSpace];
        });
        const tileType = await readType(legendTile);
        assert.match(tileType[0], /Cell Panel Condensed/);
        assert.deepEqual(tileType.slice(1), ["15px", "600", "1.2px", "left", "uppercase", "pre-wrap"]);
        assert.equal(await legendTile.textContent(), legend);
        await typeDesigner.getByRole("button", {name: "Reset typography", exact: true}).click();
        await applyType();
        assert.equal(await typeDesigner.locator("#canvas [data-type-family]").count(), 0);
        await typeDesigner.locator("#undo").click();
        assert.deepEqual(await readType(legendTile), tileType);

        await typeDesigner.getByRole("button", {name: "Add readout", exact: true}).click();
        await typeField("label").fill("Output trim");
        await typeField("type-family").selectOption("panel-sans");
        await typeField("type-size").fill("22");
        await typeField("type-weight").fill("725");
        await typeField("type-tracking").fill("-0.4");
        await typeField("type-alignment").selectOption("right");
        await applyType();

        await typeDesigner.getByRole("button", {name: "Add action", exact: true}).click();
        const caption = "Factory\nrecall";
        await typeField("label").fill(caption);
        await typeField("type-family").selectOption("system-mono");
        await typeField("type-size").fill("16");
        await typeField("type-alignment").selectOption("center");
        await typeField("type-letter-case").selectOption("uppercase");
        await applyType();
        await typeDesigner.getByRole("button", {name: "Live preview", exact: true}).click();
        const typePreview = typeDesigner.frameLocator("#preview-panel iframe");
        assert.deepEqual(await readType(typePreview.locator(".text .control-title")), tileType);
        assert.equal(await typePreview.locator(".action button").textContent(), caption);
        assert.equal((await readType(typePreview.locator(".action button"))).at(-1), "pre-wrap");
        const typeLayout = await download(typeDesigner, "#save", temp);
        const formattedControls = JSON.parse(typeLayout).controls;
        assert.equal(formattedControls[0].label, legend);
        assert.equal(formattedControls[0].typography.letterCase, "uppercase");
        assert.equal(formattedControls[1].typography.weight, 725);
        await typeDesigner.locator("#layout-file").setInputFiles({name: "typography-layout.json", mimeType: "application/json",
            buffer: Buffer.from(typeLayout)});
        await typeDesigner.getByText("Layout opened.", {exact: true}).waitFor();
        assert.equal(await typeField("label").inputValue(), legend);
        assert.equal(await typeField("type-family").inputValue(), "panel-condensed");

        const typeHtml = await download(typeDesigner, "#export", temp);
        assert.match(typeHtml, /data:font\/woff2;base64,/);
        assert.match(typeHtml, /SIL OPEN FONT LICENSE/);
        const typeExport = await browser.newPage({viewport: {width: 390, height: 540}});
        typeExport.on("pageerror", error => errors.push(error));
        const fontErrors = [];
        typeExport.on("console", message => { if (/font/i.test(message.text()) && message.type() === "warning") fontErrors.push(message.text()); });
        await typeExport.route(origin + "/typography.html", route => route.fulfill({
            body: typeHtml, contentType: "text/html; charset=utf-8",
            headers: {"content-security-policy": "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; font-src data:"}
        }));
        await typeExport.goto(origin + "/typography.html");
        assert.deepEqual(await readType(typeExport.locator(".text .control-title")), tileType);
        const trimType = await readType(typeExport.locator(".readout .control-title"));
        assert.match(trimType[0], /Cell Panel Sans/);
        assert.deepEqual(trimType.slice(1, 5), ["22px", "725", "-0.4px", "right"]);
        for (const request of ['600 15px "Cell Panel Condensed"', '725 22px "Cell Panel Sans"']) {
            assert.deepEqual(await typeExport.evaluate(async spec => (await document.fonts.load(spec)).map(font => font.status),
                request), ["loaded"]);
        }
        assert.deepEqual(fontErrors, []);
        assert.equal(await typeExport.locator(".text .control-title").evaluate(element => {
            const range = document.createRange();
            range.selectNodeContents(element);
            return new Set([...range.getClientRects()].filter(rect => rect.width > 0).map(rect => rect.y)).size;
        }), 2);
        await typeExport.screenshot({path: path.join(temp, "typography-phone.png"), fullPage: true});

        const placementDesigner = await context.newPage();
        placementDesigner.on("pageerror", error => errors.push(error));
        placementDesigner.on("dialog", dialog => dialog.accept());
        await placementDesigner.route(origin + "/**", serveDesigner);
        await placementDesigner.goto(origin + "/index.html");
        const modeParameter = project.parameters.find(parameter => parameter.key === "mode");
        const placementFixture = {schema: "cell.ui-layout.v3", catalog: [modeParameter], controls: [
            {kind: "knob", label: "Mode selector", key: "mode", x: 3, y: 0, width: 4, height: 2},
            {kind: "text", label: "Signal range", x: 8.5, y: 0.5, width: 2, height: 1, rotation: -22.5,
                phone: {x: 1.25, y: 3.5, width: 2, height: 1}}
        ]};
        const openPlacement = async data => {
            await placementDesigner.locator("#layout-file").setInputFiles({name: "detent-placement.json",
                mimeType: "application/json", buffer: Buffer.from(data)});
            await placementDesigner.getByText("Layout opened.", {exact: true}).waitFor();
        };
        await openPlacement(JSON.stringify(placementFixture));
        const selectorTile = placementDesigner.getByRole("button", {name: "Mode selector · knob"});
        await selectorTile.click();
        const editPlacement = name => placementDesigner.locator(`#properties [name="${name}"]`);
        const applyPlacement = () => placementDesigner.getByRole("button", {name: "Apply", exact: true}).click();
        await editPlacement("column").fill("4.5");
        await editPlacement("row").fill("1.25");
        await editPlacement("rotation").fill("367.5");
        await editPlacement("detent-label-mode").selectOption("choices");
        await applyPlacement();
        assert.equal(await editPlacement("rotation").inputValue(), "7.5");
        assert.equal(await selectorTile.evaluate(tile => tile.style.transform), "rotate(7.5deg)");
        assert.ok(Math.abs(await selectorTile.evaluate(tile => parseFloat(tile.style.left)) - (350 / 12)) < 0.001);
        await placementDesigner.getByRole("checkbox", {name: "Snap to grid"}).uncheck();
        const fineTile = await selectorTile.boundingBox();
        const fineCanvas = await placementDesigner.locator("#canvas").boundingBox();
        const fineX = fineTile.x + fineTile.width / 2, fineY = fineTile.y + fineTile.height / 2;
        await placementDesigner.mouse.move(fineX, fineY);
        await placementDesigner.mouse.down();
        await placementDesigner.mouse.move(fineX + fineCanvas.width / 12 * 0.2, fineY);
        await placementDesigner.mouse.up();
        assert.equal(Number(await editPlacement("column").inputValue()), 4.7);
        await selectorTile.press("ArrowLeft");
        assert.ok(Math.abs(Number(await editPlacement("column").inputValue()) - 4.6) < 1e-9);
        const resize = await selectorTile.evaluate(tile => {
            const rect = tile.getBoundingClientRect();
            const rotation = new DOMMatrix(getComputedStyle(tile).transform);
            const corner = rotation.transformPoint({x: tile.offsetWidth / 2 - 8, y: tile.offsetHeight / 2 - 8});
            const delta = rotation.transformPoint({x: tile.parentElement.clientWidth / 12, y: 0});
            return {x: rect.x + rect.width / 2 + corner.x, y: rect.y + rect.height / 2 + corner.y, dx: delta.x, dy: delta.y};
        });
        await placementDesigner.mouse.move(resize.x, resize.y);
        await placementDesigner.mouse.down();
        await placementDesigner.mouse.move(resize.x + resize.dx, resize.y + resize.dy);
        await placementDesigner.mouse.up();
        assert.equal(await editPlacement("width").inputValue(), "5");
        await placementDesigner.getByRole("button", {name: "Undo", exact: true}).click();
        assert.equal(await editPlacement("width").inputValue(), "4");
        await placementDesigner.getByRole("button", {name: "Live preview"}).click();
        const placementPreview = placementDesigner.frameLocator("#preview-panel iframe");
        const previewCaptions = placementPreview.locator(".detent-captions span");
        assert.deepEqual(await previewCaptions.allTextContents(), modeParameter.choices);
        assert.equal(await placementPreview.locator(".detent-captions [data-active]").textContent(), "Warm");
        await editPlacement("detent-label-mode").selectOption("custom");
        await editPlacement("detent-labels").fill("CL\n\nBR");
        await applyPlacement();
        await placementPreview.getByText("BR", {exact: true}).waitFor();
        assert.deepEqual(await previewCaptions.allTextContents(), ["CL", "", "BR"]);
        await placementPreview.getByRole("slider", {name: "Mode selector"}).press("ArrowUp");
        assert.equal(await placementPreview.locator(".detent-captions [data-active]").textContent(), "BR");
        assert.equal(await placementPreview.getByRole("slider", {name: "Mode selector"}).getAttribute("aria-valuetext"), "Bright");

        await placementDesigner.getByLabel("Edit layout").selectOption("phone");
        await editPlacement("column").fill("1.5");
        await editPlacement("row").fill("1.75");
        await editPlacement("width").fill("3");
        await applyPlacement();
        const savedPlacement = await download(placementDesigner, "#save", temp);
        const [savedKnob, savedLegend] = JSON.parse(savedPlacement).controls;
        assert.ok(Math.abs(savedKnob.x - 3.6) < 1e-9);
        assert.equal(savedKnob.y, 0.25);
        assert.deepEqual(savedKnob.phone, {x: 0.5, y: 0.75, width: 3, height: 2});
        assert.equal(savedKnob.rotation, 7.5);
        assert.deepEqual(savedKnob.detentLabels, ["CL", "", "BR"]);
        assert.equal(savedLegend.rotation, -22.5);
        await openPlacement(savedPlacement);
        assert.equal(await editPlacement("rotation").inputValue(), "7.5");
        assert.equal(await editPlacement("detent-labels").inputValue(), "CL\n\nBR");
        assert.equal(await editPlacement("column").inputValue(), "1.5");

        const placementHtml = await download(placementDesigner, "#export", temp);
        const placementExport = await browser.newPage({viewport: {width: 1000, height: 480}});
        placementExport.on("pageerror", error => errors.push(error));
        await placementExport.addInitScript(() => {
            window.cell = {onMessage(callback) { window.receive = callback; }, postMessage() {}};
        });
        await placementExport.route(origin + "/detent-placement.html", route =>
            route.fulfill({body: placementHtml, contentType: "text/html; charset=utf-8"}));
        await placementExport.goto(origin + "/detent-placement.html");
        await placementExport.evaluate(parameter => window.receive({type: "parameters", parameters: [parameter]}),
            {...modeParameter, value: 2});
        assert.deepEqual(await placementExport.locator(".detent-captions span").allTextContents(), ["CL", "", "BR"]);
        assert.equal(await placementExport.locator(".detent-captions [data-active]").textContent(), "BR");
        const checkPlacement = async (kind, box, angle) => {
            const measured = await placementExport.locator(`.control.${kind}`).evaluate(item => {
                const stage = getComputedStyle(item.parentElement);
                const matrix = new DOMMatrix(getComputedStyle(item).transform);
                return {columns: stage.gridTemplateColumns.split(" ").map(parseFloat),
                    rows: stage.gridTemplateRows.split(" ").map(parseFloat),
                    dx: matrix.e, dy: matrix.f, angle: Math.atan2(matrix.b, matrix.a) * 180 / Math.PI,
                    gridColumn: item.style.gridColumn, gridRow: item.style.gridRow};
            });
            assert.equal(measured.gridColumn, `${Math.floor(box.x) + 1} / span ${box.width}`);
            assert.equal(measured.gridRow, `${Math.floor(box.y) + 1} / span ${box.height}`);
            assert.ok(Math.abs(measured.dx - (box.x % 1) * (measured.columns[Math.floor(box.x)] + 8)) < 0.01);
            assert.ok(Math.abs(measured.dy - (box.y % 1) * (measured.rows[Math.floor(box.y)] + 8)) < 0.01);
            assert.ok(Math.abs(measured.angle - angle) < 0.001);
            return measured;
        };
        const wideSelection = await checkPlacement("knob", savedKnob, 7.5);
        await checkPlacement("text", savedLegend, -22.5);
        await placementExport.setViewportSize({width: 850, height: 480});
        await placementExport.waitForFunction(dx =>
            Math.abs(new DOMMatrix(getComputedStyle(document.querySelector(".knob")).transform).e - dx) > 1,
        wideSelection.dx);
        await checkPlacement("knob", savedKnob, 7.5);
        await placementExport.setViewportSize({width: 390, height: 540});
        await placementExport.locator('main[data-layout="phone"]').waitFor();
        await checkPlacement("knob", savedKnob.phone, 7.5);
        await checkPlacement("text", savedLegend.phone, -22.5);
        await placementExport.screenshot({path: path.join(temp, "detent-placement-phone.png"), fullPage: true});
        await placementExport.evaluate(parameter => window.receive({type: "parameters",
            parameters: [{...parameter, step: 0}]}), modeParameter);
        assert.equal(await placementExport.locator(".detent-captions span").count(), 0);
        await fs.writeFile(path.join(temp, "detent-placement-layout.json"), savedPlacement);

        const layerDesigner = await context.newPage();
        layerDesigner.on("pageerror", error => errors.push(error));
        layerDesigner.on("dialog", dialog => dialog.accept());
        await layerDesigner.route(origin + "/**", serveDesigner);
        await layerDesigner.goto(origin + "/index.html");
        const overlay = svg("overlay.svg", '<path d="M1 1H99V39H1Z" fill="#1ba8a0" fill-opacity=".22" stroke="#45ead0"/>');
        const sharedBox = {x: 2, y: 1, width: 8, height: 1, phone: {x: 0, y: 1, width: 4, height: 1}};
        const layerLayout = {schema: "cell.ui-layout.v3", catalog: project.parameters, controls: [
            {kind: "slider", label: "Volume", key: "gain", ...sharedBox},
            {kind: "rectangle", label: "Inset", accent: "#26373f", ...sharedBox},
            {kind: "image", label: "Overlay", artwork: "data:image/svg+xml;base64," + overlay.buffer.toString("base64"),
                fit: "fill", ...sharedBox},
            {kind: "line", label: "Divider", accent: "#838a92", direction: "vertical", width: 2,
                x: 1, y: 0, height: 2, phone: {x: 0, y: 0, width: 1, height: 1}}
        ]};
        const openLayers = async data => {
            await layerDesigner.locator("#layout-file").setInputFiles({name: "layer-order.json",
                mimeType: "application/json", buffer: Buffer.from(data)});
            await layerDesigner.getByText("Layout opened.", {exact: true}).waitFor();
        };
        await openLayers(JSON.stringify(layerLayout));
        await layerDesigner.getByText("Layers", {exact: true}).click();
        const layerList = layerDesigner.getByRole("listbox", {name: "Front to back"});
        const layerLabels = () => layerList.locator("option").allTextContents();
        const layerAction = name => layerDesigner.getByRole("button", {name, exact: true});
        const initialLayers = ["Volume · Slider", "Overlay · Image", "Divider · Line", "Inset · Rectangle"];
        assert.deepEqual(await layerLabels(), initialLayers);
        await layerList.selectOption({label: "Inset · Rectangle"});
        assert.equal(await layerDesigner.getByLabel("Label", {exact: true}).inputValue(), "Inset");
        assert.equal(await layerAction("Send backward").isDisabled(), true);
        await layerAction("Bring forward").click();
        assert.deepEqual(await layerLabels(), [initialLayers[0], initialLayers[1], initialLayers[3], initialLayers[2]]);
        await layerAction("Send backward").click();
        assert.deepEqual(await layerLabels(), initialLayers);
        await layerList.selectOption({label: "Overlay · Image"});
        await layerAction("Bring to front").click();
        const customLayers = [initialLayers[1], initialLayers[0], initialLayers[2], initialLayers[3]];
        assert.deepEqual(await layerLabels(), customLayers);
        await layerAction("Send to back").click();
        const backLayers = [initialLayers[0], initialLayers[2], initialLayers[3], initialLayers[1]];
        assert.deepEqual(await layerLabels(), backLayers);
        await layerAction("Undo").click();
        assert.deepEqual(await layerLabels(), customLayers);
        await layerAction("Redo").click();
        assert.deepEqual(await layerLabels(), backLayers);
        await layerAction("Undo").click();
        await layerList.selectOption({label: "Volume · Slider"});

        const styleLayer = locator => locator.evaluate(element => Number(getComputedStyle(element).zIndex));
        assert.equal(await styleLayer(layerDesigner.getByRole("button", {name: "Volume · slider"})), 3);
        assert.equal(await styleLayer(layerDesigner.getByRole("button", {name: "Overlay · image"})), 4);
        const savedLayers = await download(layerDesigner, "#save", temp);
        assert.deepEqual(JSON.parse(savedLayers).controls.map(control => control.layer), [3, 1, 4, 2]);
        await openLayers(savedLayers);
        assert.deepEqual(await layerLabels(), customLayers);
        await layerDesigner.getByRole("button", {name: "Live preview"}).click();
        const layerPreview = layerDesigner.frameLocator("#preview-panel iframe");
        assert.equal(await styleLayer(layerPreview.locator(".control.image")), 4);
        await layerPreview.getByRole("slider", {name: "Volume"}).click();
        const layeredHtml = await download(layerDesigner, "#export", temp);

        const layerExport = await browser.newPage({viewport: {width: 760, height: 320}});
        layerExport.on("pageerror", error => errors.push(error));
        await layerExport.addInitScript(() => {
            window.cell = {onMessage(callback) { window.receive = callback; }, postMessage() {}};
        });
        await layerExport.route(origin + "/layer-order.html", route =>
            route.fulfill({body: layeredHtml, contentType: "text/html; charset=utf-8"}));
        await layerExport.goto(origin + "/layer-order.html");
        await layerExport.evaluate(snapshot => window.receive(snapshot), project);
        const panelStack = async () => {
            for (const [kind, layer] of [["rectangle", 1], ["line", 2], ["slider", 3], ["image", 4]])
                assert.equal(await styleLayer(layerExport.locator(`.control.${kind}`)), layer);
        };
        await panelStack();
        await layerExport.setViewportSize({width: 390, height: 320});
        await layerExport.locator('main[data-layout="phone"]').waitFor();
        await panelStack();
        await layerExport.getByRole("slider", {name: "Volume"}).click();
        await layerExport.screenshot({path: path.join(temp, "layer-order-phone.png"), fullPage: true});
        await layerDesigner.getByRole("button", {name: "Add text"}).click();
        assert.deepEqual(await layerLabels(), ["Text · Text", ...customLayers]);
        await layerList.selectOption({label: "Volume · Slider"});
        await layerDesigner.getByRole("button", {name: "Overlay · image"}).press("Shift+Space");
        await layerAction("Copy selection").click();
        await layerDesigner.getByText("Selection copied to the clipboard.", {exact: true}).waitFor();
        await layerAction("Paste controls").click();
        await layerDesigner.getByText("2 controls pasted.", {exact: true}).waitFor();
        assert.deepEqual(await layerLabels(), [...customLayers.slice(0, 2), "Text · Text", ...customLayers]);
        await fs.writeFile(path.join(temp, "layer-order-layout.json"), savedLayers);
        const hardware = await context.newPage();
        hardware.on("pageerror", error => errors.push(error));
        await hardware.route(origin + "/**", serveDesigner);
        await hardware.goto(origin + "/index.html");
        await hardware.getByRole("button", {name: "Hardware showcase", exact: true}).click();
        const hardwarePreview = hardware.frameLocator("#preview-panel iframe");
        const dial = hardwarePreview.locator(".knob[data-hardware-artwork=cap] .dial");
        await dial.waitFor();
        assert.match(await dial.evaluate(element => getComputedStyle(element).backgroundImage), /^url\("data:image\/webp/);
        await hardwarePreview.getByRole("slider", {name: "Output level"}).press("ArrowUp");
        assert.equal(await hardwarePreview.getByRole("slider", {name: "Output level"}).getAttribute("aria-valuetext"), "-11.99 dB");
        const display = hardwarePreview.locator('.stepper[data-hardware-artwork=display] output');
        assert.match(await display.evaluate(element => getComputedStyle(element).backgroundImage), /^url\("data:image\/webp/);
        assert.equal(await display.locator(".led-code").textContent(), "A1");
        await hardwarePreview.locator(".stepper button").last().press("Space");
        assert.equal(await display.locator(".led-code").textContent(), "br");
        assert.ok(Number(await hardwarePreview.getByRole("meter", {name: "Output peak"}).getAttribute("value")) > -60);
        await hardwarePreview.locator("main").screenshot({path: path.join(temp, "hardware-preview-wide.png")});

        await hardware.locator("#control-artwork-library summary").click();
        const knobArt = hardware.getByRole("button", {name: "Knurled graphite knob"});
        await knobArt.click();
        assert.equal(await knobArt.getAttribute("aria-pressed"), "true");
        await hardware.locator("#undo").click();
        assert.equal(await hardware.getByRole("button", {name: "Brushed nickel knob"}).getAttribute("aria-pressed"), "true");
        await hardware.getByLabel("Artwork placement").selectOption("");
        await hardwarePreview.locator(".knob img.artwork").waitFor();
        await hardware.locator("#undo").click();
        await dial.waitFor();
        await hardware.locator("#panel-artwork-options").locator("..").locator("summary").click();
        await hardware.getByRole("button", {name: "Slate powder coat"}).click();
        const savedHardware = JSON.parse(await download(hardware, "#save", temp));
        assert.match(savedHardware.theme.backgroundArtwork, /^data:image\/webp;base64,/);
        assert.equal(savedHardware.controls[0].artworkMode, "cap");
        assert.equal(savedHardware.controls[2].artworkMode, "cap");
        await hardware.locator("#layout-file").setInputFiles({name: "hardware.json", mimeType: "application/json",
            buffer: Buffer.from(JSON.stringify(savedHardware))});
        await hardware.getByText("Layout opened.", {exact: true}).waitFor();
        assert.equal(await hardware.getByRole("button", {name: "Slate powder coat"}).getAttribute("aria-pressed"), "true");

        const hardwareHtml = await download(hardware, "#export", temp);
        const hardwareExport = await browser.newPage({viewport: {width: 390, height: 600}});
        hardwareExport.on("pageerror", error => errors.push(error));
        await hardwareExport.addInitScript(() => {
            window.cell = {onMessage(callback) { window.receive = callback; }, postMessage() {}};
        });
        await hardwareExport.route(origin + "/**", route => {
            assert.equal(new URL(route.request().url()).pathname, "/hardware.html");
            return route.fulfill({body: hardwareHtml, contentType: "text/html; charset=utf-8"});
        });
        await hardwareExport.goto(origin + "/hardware.html");
        await hardwareExport.evaluate(parameters => window.receive({type: "parameters", parameters}), savedHardware.catalog);
        await hardwareExport.locator('main[data-layout="phone"] .fader[data-hardware-artwork=cap]').waitFor();
        assert.equal(await hardwareExport.locator("main > .panel-background").getAttribute("src"), savedHardware.theme.backgroundArtwork);
        assert.equal(await hardwareExport.locator(".stepper .led-code").textContent(), "A1");
        await hardwareExport.screenshot({path: path.join(temp, "hardware-export-phone.png"), fullPage: true});
        await fs.writeFile(path.join(temp, "hardware-editor.html"), hardwareHtml);
        const hostedModes = ["Mono", "Tape", "Symphonic", "Studio"];
        await hardwareExport.evaluate(choices => window.receive({type: "parameters", revision: 1, supportsDeltas: true,
            parameters: [{key: "gain", name: "Delay algorithm", minimum: 0, maximum: 3, step: 1, value: 2,
                text: "Symphonic HQ", choices, display: {detents: choices}}]}), hostedModes);
        assert.equal(await hardwareExport.locator(".knob .detents span").count(), 4);
        assert.deepEqual(await hardwareExport.locator(".knob .detent-captions span").allTextContents(), hostedModes);
        assert.equal(await hardwareExport.locator(".readout output").textContent(), "Symphonic HQ");
        await hardwareExport.evaluate(() => window.receive({type: "parameter-delta", base: 4, revision: 5,
            parameters: [], removed: ["gain"]}));
        await hardwareExport.evaluate(choices => window.receive({type: "parameter-delta", base: 1, revision: 2,
            parameters: [{key: "gain", name: "Delay algorithm", minimum: 0, maximum: 3, step: 1, value: 3,
                choices, text: "Studio", readonly: true}], removed: []}), hostedModes);
        assert.equal(await hardwareExport.locator(".knob input").isDisabled(), true);
        assert.equal(await hardwareExport.locator(".readout output").textContent(), "Studio");

        for (const [family, label] of [["british", "British broadcast"], ["soviet", "Soviet-era laboratory"],
            ["german", "German precision"], ["field", "Field communications"]]) {
            await hardware.getByLabel("Hardware style").selectOption(family);
            await hardware.getByRole("button", {name: "Hardware showcase", exact: true}).click();
            await hardware.getByText(label + " showcase loaded.", {exact: true}).waitFor();
            await hardwarePreview.locator(".stepper[data-hardware-artwork=display] output").waitFor();
            const artwork = await hardware.evaluate(family => Object.fromEntries(["panel", "knob", "fader", "display"]
                .map(group => [group, CellDesignerArtwork[group].find(asset => asset.family === family).artwork])), family);
            const background = hardwarePreview.locator(".panel-background");
            assert.equal(await background.getAttribute("src"), artwork.panel);
            await background.evaluate(image => image.decode());
            for (const [kind, group] of [["knob", "knob"], ["fader", "fader"], ["stepper", "display"]]) {
                const control = hardwarePreview.locator(".control." + kind);
                assert.ok((await control.evaluate(element => element.style.getPropertyValue("--hardware-image"))).includes(artwork[group]));
            }
        }

        const bypassSwitch = hardwarePreview.locator(".toggle[data-hardware-artwork=switch] button");
        await bypassSwitch.click();
        assert.equal(await bypassSwitch.getAttribute("aria-pressed"), "true");
        await bypassSwitch.press("Space");
        assert.equal(await bypassSwitch.getAttribute("aria-pressed"), "false");
        await hardware.getByRole("button", {name: "Input trim · knob", exact: true}).click();
        const library = hardware.locator("#control-artwork-library");
        if (!(await library.evaluate(panel => panel.open))) await library.locator("summary").click();
        await hardware.getByRole("button", {name: "Amp chicken-head knob"}).click();
        const pointerDial = hardwarePreview.locator(".knob[data-hardware-artwork=pointer-cap] .dial");
        await pointerDial.waitFor();
        const pointerAngle = () => pointerDial.evaluate(element => getComputedStyle(element, "::after").transform);
        const before = await pointerAngle();
        await hardwarePreview.getByRole("slider", {name: "Input trim"}).press("ArrowUp");
        assert.notEqual(await pointerAngle(), before);

        await hardware.getByLabel("Hardware style").selectOption("field-indicators");
        await hardware.getByRole("button", {name: "Hardware showcase", exact: true}).click();
        await hardware.getByText("Field indicators showcase loaded.", {exact: true}).waitFor();
        const lampStatus = (view, name) => view.getByRole("status", {name: name + " status", exact: true});
        assert.equal(await lampStatus(hardwarePreview, "READY").textContent(), "On");
        const effectLamp = hardwarePreview.locator(".indicator").filter({hasText: "EFFECT ACTIVE"});
        await effectLamp.locator('output').filter({hasText: "Off"}).waitFor();
        const detonate = hardwarePreview.getByRole("button", {name: "DETONATE", exact: true});
        await detonate.focus();
        await hardware.keyboard.down("Space");
        await effectLamp.locator('output').filter({hasText: "On"}).waitFor();
        assert.equal(await effectLamp.getAttribute("data-lamp"), "on");
        await hardware.keyboard.up("Space");
        await effectLamp.locator('output').filter({hasText: "Off"}).waitFor();

        await hardware.getByRole("button", {name: "CAUTION · indicator", exact: true}).click();
        assert.equal(await hardware.getByLabel("Lamp source").inputValue(), "meter");
        await hardware.getByLabel("Light at or above").fill("-21");
        await hardware.getByRole("button", {name: "Apply", exact: true}).click();
        const fieldLayout = JSON.parse(await download(hardware, "#save", temp));
        assert.equal(fieldLayout.controls.find(control => control.label === "CAUTION").threshold, -21);
        const fieldHtml = await download(hardware, "#export", temp);
        const fieldEditor = await browser.newPage({viewport: {width: 840, height: 380}});
        fieldEditor.on("pageerror", error => errors.push(error));
        await fieldEditor.addInitScript(() => {
            window.sent = [];
            window.cell = {onMessage(callback) { window.receive = callback; }, postMessage(message) { window.sent.push(message); }};
        });
        await fieldEditor.route(origin + "/field-indicators.html", route => route.fulfill({body: fieldHtml, contentType: "text/html"}));
        await fieldEditor.goto(origin + "/field-indicators.html");
        assert.equal(await lampStatus(fieldEditor, "READY").textContent(), "Unavailable");
        await fieldEditor.evaluate(parameters => window.receive({type: "parameters", momentary: true, parameters}),
            fieldLayout.catalog);
        const sendPeak = value => fieldEditor.evaluate(value => window.receive({type: "meters",
            meters: [{key: "output-peak", value, minimum: -60, maximum: 0, unit: "dBFS"}]}), value);
        await sendPeak(-22);
        assert.equal(await lampStatus(fieldEditor, "CAUTION").textContent(), "Off");
        await sendPeak(-21);
        assert.equal(await lampStatus(fieldEditor, "CAUTION").textContent(), "On");
        assert.equal(await lampStatus(fieldEditor, "PEAK WARNING").textContent(), "Off");
        await sendPeak(-15);
        assert.equal(await lampStatus(fieldEditor, "PEAK WARNING").textContent(), "On");
        await fieldEditor.getByRole("button", {name: "LAUNCH", exact: true}).press("Space");
        assert.deepEqual(await fieldEditor.evaluate(() => window.sent),
            ["momentary-press", "momentary-release"].map(type => ({type, key: "audition"})));
        await fieldEditor.screenshot({path: path.join(temp, "field-indicators.png"), fullPage: true});
        await fieldEditor.evaluate(() => window.receive({type: "parameters", parameters: []}));
        assert.equal(await lampStatus(fieldEditor, "READY").textContent(), "Unavailable");

        await hardware.getByLabel("Hardware style").selectOption("tape");
        await hardware.getByRole("button", {name: "Hardware showcase", exact: true}).click();
        await hardware.getByText("Tape studio showcase loaded.", {exact: true}).waitFor();
        const cueStatus = lampStatus(hardwarePreview, "Cue active");
        await cueStatus.filter({hasText: "Off"}).waitFor();
        await hardwarePreview.getByRole("button", {name: "Cue", exact: true}).focus();
        await hardware.keyboard.down("Space");
        await cueStatus.filter({hasText: "On"}).waitFor();
        await hardware.keyboard.up("Space");
        await cueStatus.filter({hasText: "Off"}).waitFor();
        await hardwarePreview.locator("main").screenshot({path: path.join(temp, "tape-studio-preview.png")});
        if (!(await library.evaluate(panel => panel.open))) await library.locator("summary").click();
        const jogWheel = hardware.getByRole("button", {name: "Cassette jog wheel"});
        await jogWheel.locator("img").evaluate(image => image.decode());
        await jogWheel.click();
        const tapeLayout = JSON.parse(await download(hardware, "#save", temp));
        const tapeKnob = tapeLayout.controls.find(control => control.kind === "knob");
        assert.equal(tapeKnob.artwork, await jogWheel.locator("img").getAttribute("src"));
        assert.equal(tapeKnob.artworkMode, "cap");
        const tapeHtml = await download(hardware, "#export", temp);
        for (const kind of ["knob", "fader", "meter", "momentary", "indicator"]) {
            const control = tapeLayout.controls.find(control => control.kind === kind);
            assert.match(control.artwork, /^data:image\/webp;base64,/);
            assert.ok(tapeHtml.includes(control.artwork), `Export lost artwork for ${kind}`);
        }

        if (process.env.CELL_BANKED_FIXTURE) {
            const banked = await browser.newPage({viewport: {width: 850, height: 600}});
            banked.on("pageerror", error => errors.push(error));
            await banked.addInitScript(() => {
                window.sent = [];
                window.cell = {onMessage(callback) { window.receive = callback; }, postMessage(message) { window.sent.push(message); }};
            });
            const exported = await fs.readFile(path.join(__dirname, "../examples/sdk_banked/cell-plugin-editor.html"), "utf8");
            await banked.route(origin + "/banked.html", route => route.fulfill({body: exported, contentType: "text/html"}));
            await banked.goto(origin + "/banked.html");
            const snapshot = JSON.parse(execFileSync(process.env.CELL_BANKED_FIXTURE, ["--parameters"], {encoding: "utf8"}));
            await banked.evaluate(snapshot => window.receive(snapshot), snapshot);
            const patch = banked.getByRole("status", {name: "Patch name"});
            assert.equal(await patch.textContent(), "0 dB Trim");
            await banked.getByRole("combobox", {name: "Bank", exact: true}).selectOption("2");
            await banked.getByRole("combobox", {name: "Program", exact: true}).selectOption("4");
            assert.equal(await patch.textContent(), "0 dB Trim");
            await banked.evaluate(snapshot => {
                snapshot.programs.active = {bank: 2, program: 4};
                window.receive(snapshot);
            }, snapshot);
            assert.equal(await patch.textContent(), "-28 dB Trim");
            const factory = banked.getByRole("button", {name: "Factory recall", exact: true});
            await factory.focus();
            await banked.keyboard.press("Space");
            assert.deepEqual(await banked.evaluate(() => window.sent), [{type: "recall-program", bank: 2, program: 4}]);
            await banked.keyboard.down("Space");
            await banked.waitForFunction(() => window.sent.some(message => message.key === "factory-recall"));
            await banked.keyboard.up("Space");
            assert.deepEqual(await banked.evaluate(() => window.sent.at(-1)),
                {type: "action", key: "factory-recall", phase: "invoke"});
        }

        assert.deepEqual(errors, []);
        console.log("Designer passed layout editing, program recall, displays, actions, conditional panels, typography, detent placement, layer-order exports, hardware artwork, and indicator lamps.");
    } finally {
        await browser.close();
        if (!process.env.CELL_UI_DESIGNER_TEST_OUTPUT) await fs.rm(temp, {recursive: true, force: true});
    }
})().catch(error => { console.error(error); process.exitCode = 1; });
