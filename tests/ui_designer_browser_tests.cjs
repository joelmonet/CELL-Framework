const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const path = require("node:path");
const os = require("node:os");
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
            if (!["index.html", "model.js", "runtime.js", "designer.js"].includes(name)) return route.abort();
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
        await preview.getByRole("slider", {name: "Field Radio Gain"}).waitFor();
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
        assert.deepEqual(errors, []);
        console.log("Designer passed group editing, cross-tab clipboard, responsive controls, assets, meters, and exports.");
    } finally {
        await browser.close();
        if (!process.env.CELL_UI_DESIGNER_TEST_OUTPUT) await fs.rm(temp, {recursive: true, force: true});
    }
})().catch(error => { console.error(error); process.exitCode = 1; });
