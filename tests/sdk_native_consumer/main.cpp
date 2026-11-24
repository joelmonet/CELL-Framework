// Original Cell Framework implementation; see LICENSE.
// Exercises installed public headers and exported transitive link dependencies.
#include <cell/ui.hpp>
#include <cell/audio_device.hpp>
#include <cell/ara/source_reader.hpp>
#include <cell/process_buffer.hpp>
#include <cell/state_tree.hpp>
#include <cell/hash.hpp>
#include <cell/dispatch.hpp>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        cell::MainThreadDispatcher::bindCurrentThread();
        cell::ProcessBuffer<float> buffer(2, 128);
        buffer.clear();
        buffer.setSample(1, 127, -.25f);
        if (buffer.getSample(1, 127) != -.25f)
            throw std::runtime_error("Installed process buffer changed samples");
        cell::state::Tree state("NativeConsumer");
        state.setProperty("position", std::int64_t(1) << 40);
        if (state["position"].integer() != (std::int64_t(1) << 40))
            throw std::runtime_error("Installed state lost a long sample position");
        cell::ui::Surface surface;
        surface.resize(63, 31);
        surface.render([](cell::ui::Canvas &canvas) {
            canvas.fill({0, 0, 63, 31}, cell::ui::Color(0xff294852));
            canvas.text({2, 20}, "Cell \xe2\x80\x94 \xc3\xa9", 12, cell::ui::Color(0xffffffff));
        });
        if (!surface.pixels() || surface.width() != 63 || surface.height() != 31)
            throw std::runtime_error("Installed native renderer did not create its surface");
        cell::audio::Device device;
        cell::ara::SourceReader reader;
        if (device.isOpen() || reader.isOpen())
            throw std::runtime_error("Constructing a service unexpectedly opened a device or source");
        cell::Sha256 hash;
        if (hash.hex() != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
            throw std::runtime_error("Installed hash contract changed");
        std::cout << "PASS relocated installed native SDK: public headers, renderer/fonts, audio device, ARA source, process buffer, 64-bit state and transitive dependencies\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
