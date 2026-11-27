// SPDX-License-Identifier: Apache-2.0
#include "../examples/native_theme_editor/theme_editor_integration.hpp"
#include <cell/ui_palette.hpp>
#include <cell/ui_theme_editor.hpp>
#include <cstring>
#include <iostream>
#include <thread>
namespace t = cell::ui::themes;
using namespace cell::ui;
void check(bool ok, const char* why) {
    if (!ok)
        throw std::runtime_error(why);
}
template <class F> void rejects(F f) {
    bool threw = false;
    try {
        f();
    } catch (const std::exception&) {
        threw = true;
    }
    check(threw, "Invalid theme input was accepted");
}
int main(int argc, char** argv) try {
    const auto root = std::filesystem::absolute(argc > 1 ? argv[1] : "theme-test-data");
    std::filesystem::create_directories(root);
    auto classic = t::Theme{};
    classic.name = "CLASSIC";
    auto hdr = classic;
    hdr.name = "HDR";
    hdr.iconSet = "hdr";
    hdr.material = t::Material::sculpted;
    hdr.relief = .9;
    hdr.colors[1] = Color(0xff131618);
    auto modern = classic;
    modern.name = "MODERN";
    modern.iconSet = "modern";
    modern.material = t::Material::modern;
    for (auto theme : {classic, hdr, modern}) {
        theme.extensions["futureProperty"] = cell::json::Value("retain me");
        check(t::Theme::read(theme.serialize()) == theme,
              "Theme JSON round trip lost tokens or extensions");
    }
    check(t::Theme::parseColor("#123456").argb == 0xff123456 &&
              t::Theme::parseColor("#80123456").argb == 0x80123456,
          "RGB/ARGB color parsing differs");
    rejects([] { t::Theme::parseColor("#GG1122"); });
    rejects([] { t::Theme::read(std::string(65537, ' ')); });
    rejects([&] {
        auto bad = classic;
        bad.glow = std::numeric_limits<double>::quiet_NaN();
        bad.serialize();
    });
    rejects([&] {
        auto bad = classic;
        bad.name = "bad\nname";
        bad.serialize();
    });
    rejects([&] {
        auto bad = classic;
        bad.name = "invalid \xFF";
        bad.validate();
    });
    rejects([&] {
        auto bad = classic;
        bad.extensions["tooLarge"] = cell::json::Value(std::string(65536, 'X'));
        bad.serialize();
    });
    auto future = classic.serialize();
    const auto at = future.find("cell.ui-theme.v1");
    future.replace(at, 16, "cell.ui-theme.v9");
    rejects([&] { t::Theme::read(future); });
    t::Library library(root / "library", {classic, hdr, modern});
    library.select(0);
    const auto original = library.stock(0).serialize();
    auto modified = library.snapshot();
    modified.theme.colors[1] = Color(0xff445566);
    rejects([&] { library.preview(modified); });
    check(library.stock(0).serialize() == original, "Rejected preview changed stock theme");
    library.edit(modified.theme);
    check(library.snapshot().stockIndex < 0 && library.snapshot().theme.name == "CLASSIC Custom",
          "Editing stock did not detach a custom copy");
    const auto saved = library.saveNamed("CLASSIC");
    check(saved.parent_path() == root / "library/User" &&
              saved.filename() == "CLASSIC Custom.celltheme" &&
              library.stock(0).serialize() == original,
          "Named save could overwrite stock definitions");
    check(t::Theme::load(saved) == library.snapshot().theme, "User theme saved different values");
    library.exportFile(root / "export.celltheme");
    t::Library reopened(root / "library", {classic, hdr, modern});
    check(reopened.snapshot().theme == library.snapshot().theme,
          "Active user theme did not persist");
    reopened.select(2);
    reopened.importFile(root / "export.celltheme");
    check(reopened.snapshot().theme == library.snapshot().theme,
          "Theme import changed the saved tokens");
    check(reopened.userThemes().size() == 1, "User library discovery lost a saved theme");
    const auto imported = reopened.snapshot();
    rejects([&] { reopened.loadUser(root / "export.celltheme"); });
    check(reopened.snapshot().revision == imported.revision, "Rejected load changed the theme");
    reopened.loadUser(saved);
    check(reopened.snapshot().userFile == std::filesystem::canonical(saved),
          "Loading a saved theme lost its library path");
    std::array<PaletteReplacement, 1> map{{{Color(0xff112233), Color(0xffaabbcc)}}};
    check(PaletteScope::map(Color(0x55112233)).argb == 0x55112233,
          "Unscoped renderer changed colors");
    {
        PaletteScope scope(map);
        check(PaletteScope::map(Color(0x55112233)).argb == 0x55aabbcc,
              "Palette scope lost alpha or RGB");
        {
            PaletteScope inner(std::span<const PaletteReplacement>{});
            check(PaletteScope::map(Color(0xff112233)).argb == 0xff112233,
                  "Nested palette scope did not isolate colors");
        }
        check(PaletteScope::map(Color(0xff112233)).argb == 0xffaabbcc,
              "Nested scope failed to restore its parent");
        bool isolated = false;
        std::thread other(
            [&] { isolated = PaletteScope::map(Color(0xff112233)).argb == 0xff112233; });
        other.join();
        check(isolated, "Palette leaked across paint threads");
    }
    check(PaletteScope::map(Color(0xff112233)).argb == 0xff112233,
          "Palette scope leaked after paint");
    Surface direct;
    direct.resize(20, 20);
    direct.render([&](Canvas& c) {
        PaletteScope scope(map);
        c.fill({0, 0, 20, 20}, Color(0xff112233));
    });
    check(*static_cast<const std::uint32_t*>(direct.pixels()) == 0xffaabbcc,
          "Canvas palette integration is missing");
    t::PanelCache cache(65536);
    cache.theme(hdr, 1);
    Surface image;
    image.resize(400, 300);
    const auto draw = [&](Canvas& c) {
        c.fill({0, 0, 400, 300}, Color(0xff000000));
        cache.paint(c, {10, 10, 100, 100});
    };
    image.render(draw);
    const auto bytes = cache.bytes();
    check(bytes > 0 && bytes <= 65536, "Panel cache exceeded its memory budget");
    std::vector<std::byte> pixels(std::size_t(image.stride()) * image.height());
    std::memcpy(pixels.data(), image.pixels(), pixels.size());
    image.render(draw);
    check(bytes == cache.bytes() && std::memcmp(pixels.data(), image.pixels(), pixels.size()) == 0,
          "Cached panel changed pixels or grew on a cache hit");
    cache.clear();
    image.render([&](Canvas& c) { cache.paint(c, {0, 0, 80.25, 40}); });
    const auto fractionalBytes = cache.bytes();
    image.render([&](Canvas& c) { cache.paint(c, {0, 0, 80.75, 40}); });
    check(cache.bytes() == fractionalBytes * 2, "Distinct logical panel sizes shared a cache key");
    image.render([&](Canvas& c) { cache.paint(c, {0, 0, 180, 180}, 2); });
    check(cache.bytes() <= 65536, "Oversized panel escaped the cache budget");
    cache.theme(modern, 2);
    check(cache.bytes() == 0, "Theme change retained stale cached backplates");
    t::ThemeEditor editor(library);
    const auto before = library.snapshot();
    editor.open();
    for (const auto size : {Point{900, 760}, Point{620, 980}, Point{520, 420}}) {
        image.resize(int(size.x), int(size.y));
        image.render([&](auto& c) { editor.paint(c, {0, 0, size.x, size.y}); });
        for (const auto& control : editor.controls())
            check(control.bounds.x >= 0 && control.bounds.y >= 0 &&
                      control.bounds.x + control.bounds.width <= size.x &&
                      control.bounds.y + control.bounds.height <= size.y,
                  "Theme editor control escaped the viewport");
    }
    image.resize(900, 760);
    image.render([&](auto& c) { editor.paint(c, {0, 0, 900, 760}); });
    std::uint64_t hdrButton = 0;
    for (const auto& c : editor.controls())
        if (c.label == "THEME / HDR")
            hdrButton = c.identity;
    check(hdrButton && editor.accessible(hdrButton, AccessibleAction::activate, 0, 0),
          "Stock selector is not accessible");
    check(library.snapshot().stockIndex == 1, "Theme editor stock selection did not preview");
    check(!editor.key({32}), "Theme editor swallowed host transport Space without text entry");
    check(!editor.key({.key = 'S', .control = true}), "Theme editor swallowed a host shortcut");
    check(!editor.accessible(hdrButton + 50000, AccessibleAction::focus, 0, 0),
          "Accessibility claimed an unknown control");
    image.resize(520, 420);
    const auto paintSmall = [&] {
        image.render([&](auto& c) { editor.paint(c, {0, 0, 520, 420}); });
    };
    const auto identityFor = [&](std::string_view name) {
        for (const auto& control : editor.controls())
            if (control.label == name)
                return control.identity;
        return std::uint64_t{};
    };
    paintSmall();
    const auto nameInput = identityFor("THEME / NAME");
    check(nameInput && editor.accessible(nameInput, AccessibleAction::activate, 0, 0),
          "Name input is not accessible");
    editor.key({.text = U'日'});
    editor.key({.text = U'é'});
    editor.key({.key = 8});
    editor.key({.key = 13});
    check(library.snapshot().theme.name == cell::Text::charToString(U'日').utf8(),
          "Name input did not preserve Unicode");
    std::uint64_t textScale{};
    for (int tab = 0; tab < 40 && !textScale; ++tab) {
        check(editor.key({.key = 9}), "Theme editor skipped keyboard focus");
        paintSmall();
        const auto target = identityFor("THEME / TEXT SCALE");
        if (target && editor.focus() == target)
            textScale = target;
    }
    check(textScale && editor.key({.key = keys::end}) && library.snapshot().theme.fontScale == 1.5,
          "Keyboard focus did not reach and adjust the scrolled slider");
    check(!editor.accessible(textScale, AccessibleAction::setValue,
                             std::numeric_limits<double>::quiet_NaN(), 0),
          "Accessibility accepted a non-finite slider value");
    const auto reset = identityFor("THEME / RESET SELECTED STOCK");
    check(reset && editor.accessible(reset, AccessibleAction::activate, 0, 0) &&
              library.snapshot().stockIndex == 1,
          "Reset did not restore the selected stock after editing");
    editor.close(false);
    check(library.snapshot().theme == before.theme, "Cancel did not restore the previous theme");
    cell::examples::ThemeExampleEditor example(root / "example");
    image.resize(900, 760);
    example.openThemeEditor();
    image.render([&](auto& c) { example.paint(c, {0, 0, 900, 760}); });
    check(image.writePng((root / "theme-editor.png").string()), "Theme editor preview failed");
    std::cout << "PASS theme schema/round-trip, malformed inputs, immutable stock, named "
                 "save/import/export, active recall, thread-local Canvas palette, bounded cached "
                 "panels, responsive editor, accessibility and cancel/Space behavior\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
