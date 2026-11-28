// Original Cell Framework implementation; see LICENSE.
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const source = path.join(__dirname, "../ui_designer");
const output = process.argv[2] || __dirname;
const context = vm.createContext({TextEncoder,
    layout: fs.readFileSync(path.join(__dirname, "layout.json"), "utf8")});
for (const name of ["model.js", "fonts.js", "runtime.js"])
    vm.runInContext(fs.readFileSync(path.join(source, name), "utf8"), context, {filename: name});
const [html, header] = vm.runInContext(`
    const {controls, theme} = CellDesignerModel.readProject(layout);
    [CellDesignerRuntime.html(controls, theme), CellDesignerModel.cppBindings(controls)];
`, context);
fs.mkdirSync(output, {recursive: true});
fs.writeFileSync(path.join(output, "cell-plugin-editor.html"), html);
fs.writeFileSync(path.join(output, "cell-ui-bindings.hpp"), header);
