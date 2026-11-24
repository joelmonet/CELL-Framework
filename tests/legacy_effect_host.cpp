// Original Cell Framework implementation; see LICENSE.
#include <cell/dynamic_library.hpp>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>

#if CELL_TEST_LV2
#include <lv2/core/lv2.h>
#else
#include <ladspa.h>
#if CELL_TEST_DSSI
#include <dssi.h>
#endif
#endif

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void render(const char* path, [[maybe_unused]] std::string_view expectedUri) {
    cell::DynamicLibrary library;
    check(library.open(cell::Text(path)), "Host could not load the module");
#if CELL_TEST_LV2
    auto entry = reinterpret_cast<const LV2_Descriptor* (*)(std::uint32_t)>(
        library.getFunction("lv2_descriptor"));
    check(entry && entry(0) && !entry(1), "Invalid LV2 descriptor");
    const auto* descriptor = entry(0);
    check(descriptor->URI && std::string_view(descriptor->URI) == expectedUri, "Wrong LV2 URI");
    auto handle = descriptor->instantiate(descriptor, 48000, "", nullptr);
    const auto connect = descriptor->connect_port;
#else
    auto entry = reinterpret_cast<LADSPA_Descriptor_Function>(library.getFunction("ladspa_descriptor"));
    check(entry && entry(0) && !entry(1), "Invalid LADSPA descriptor");
    const auto* descriptor = entry(0);
    check(descriptor->PortCount == 5, "Wrong LADSPA port count");
#if CELL_TEST_DSSI
    auto dssi = reinterpret_cast<const DSSI_Descriptor* (*)(unsigned long)>(library.getFunction("dssi_descriptor"));
    check(dssi && dssi(0) && !dssi(1) && dssi(0)->DSSI_API_Version == 1 &&
          dssi(0)->LADSPA_Plugin == descriptor, "Invalid DSSI effect descriptor");
#endif
    auto handle = descriptor->instantiate(descriptor, 48000);
    const auto connect = descriptor->connect_port;
#endif
    check(handle, "Host could not instantiate the module");
    struct Scope {
        decltype(descriptor) api;
        decltype(handle) instance;
        ~Scope() {
            if (api->deactivate)
                api->deactivate(instance);
            api->cleanup(instance);
        }
    } scope{descriptor, handle};

    std::array<float, 1025> left, right;
    left.fill(.25f);
    right.fill(-.125f);
    float gain = 0;
    connect(handle, 0, left.data());
    connect(handle, 1, right.data());
    connect(handle, 2, left.data());
    connect(handle, 3, right.data());
    connect(handle, 4, &gain);
#if defined(CELL_TEST_LV2_METER_PORT)
    float peak{}, rms{};
    connect(handle, CELL_TEST_LV2_METER_PORT, &peak);
    connect(handle, CELL_TEST_LV2_METER_PORT + 1, &rms);
#endif
    descriptor->activate(handle);
    descriptor->run(handle, 1025);
    check(left.back() == .25f && right.back() == -.125f, "In-place unity render failed");
#if defined(CELL_TEST_LV2_METER_PORT)
    const float expectedRms = std::sqrt((.25f * .25f + .125f * .125f) / 2);
    check(peak == .25f && std::abs(rms - expectedRms) < 1e-6, "Wrong LV2 output meter levels");
#endif

    gain = -6;
    descriptor->run(handle, 1025);
    const float attenuation = std::pow(10.0f, -.3f);
    check(left.front() < .25f && left.front() > .25f * attenuation, "Gain did not ramp");
    check(std::abs(left.back() - .25f * attenuation) < 1e-6 &&
          std::abs(right.back() + .125f * attenuation) < 1e-6, "Block splitting changed the result");
}
} // namespace

int main(int argc, char** argv) {
    try {
#if CELL_TEST_LV2
        check(argc == 2 || argc == 3, "Expected a module path and optional LV2 URI");
        const std::string_view expectedUri = argc == 3 ? argv[2] : "https://example.com/cell/gain";
#else
        check(argc == 2, "Expected a module path");
        const std::string_view expectedUri;
#endif
        render(argv[1], expectedUri);
        render(argv[1], expectedUri);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
