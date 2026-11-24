// Original Cell Framework implementation; see LICENSE.
#include <cell/dynamic_library.hpp>
#include <acfunknown.h>
#include <acfresult.h>
#include <iostream>
#include <stdexcept>
#include <string_view>

int main(int argc, char** argv) {
    try {
        cell::DynamicLibrary library;
        const bool exercise = argc == 3 && std::string_view(argv[2]) == "--exercise";
        if ((argc != 2 && !exercise) || !library.open(cell::Text(argv[1])))
            throw std::runtime_error("Could not load the linked AAX test module");
        for (const char* name : {"ACFRegisterPlugin", "ACFRegisterComponent", "ACFGetClassFactory",
                                 "ACFCanUnloadNow", "ACFStartup", "ACFShutdown", "ACFGetSDKVersion"}) {
            if (!library.getFunction(name))
                throw std::runtime_error(std::string("Missing AAX SDK export: ") + name);
        }
        using GetVersion = ACFRESULT (ACFAPICALLTYPE*)(acfUInt64*);
        const auto getVersion = reinterpret_cast<GetVersion>(library.getFunction("ACFGetSDKVersion"));
        acfUInt64 version{};
        if (getVersion(&version) != ACF_OK || !version)
            throw std::runtime_error("AAX SDK version query failed");
        std::cout << "AAX module loaded; all seven ACF exports present; SDK version " << version << '\n';
        if (exercise) {
            const auto describe = reinterpret_cast<bool (*)()>(library.getFunction("cell_aax_test_describe"));
            if (!describe || !describe())
                throw std::runtime_error("AAX registration, state, or render probe failed");
            std::cout << "AAX registration, state recall, and mono/stereo rendering passed\n";
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
