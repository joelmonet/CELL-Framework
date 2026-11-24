// Original Cell Framework implementation; see LICENSE.
#include <cell/web_parameter_bridge.hpp>
#if defined(CELL_PROJECT_CHECK_DESIGNER)
#include "cell-ui-bindings.hpp"
#endif
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>

namespace {
int exportDesignerCatalog() {
    using V = cell::json::Value;
    V::Array fields;
    for (const auto& parameter : cell::plugin::descriptor().parameters)
        fields.push_back(cell::web::detail::parameterSnapshotField(parameter, parameter.clamp(parameter.defaultValue)));
    const auto text = cell::json::write(V(V::Object{{"type", V("parameters")}, {"parameters", V(std::move(fields))}}));
    if (text.size() >= 512 * 1024) {
        std::cerr << "The parameter catalog exceeds the Designer's 512 KiB import limit\n";
        return 1;
    }
    std::cout << text << '\n';
    return std::cout ? 0 : 1;
}

#if defined(CELL_PROJECT_CHECK_DESIGNER)
int checkDesignerBindings() {
    const auto& descriptor = cell::plugin::descriptor();
    for (const auto& control : cell::designer::controls) {
        const auto field = std::find_if(descriptor.parameters.begin(), descriptor.parameters.end(),
            [&](const cell::Parameter& parameter) {
                return parameter.key == control.key && parameter.id == control.parameter;
            });
        if (field == descriptor.parameters.end() ||
            std::find(cell::designer::parameters.begin(), cell::designer::parameters.end(), control.parameter) ==
                cell::designer::parameters.end()) {
            std::cerr << "Designer control has no matching processor binding: " << control.key << '\n';
            return 1;
        }
    }
    try {
        cell::Engine engine(descriptor, cell::plugin::createProcessor());
        const cell::web::ParameterBridge bridge(engine, cell::designer::parameters);
        (void)bridge.snapshot();
    } catch (const std::exception& error) {
        std::cerr << "Invalid Designer bindings: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
#endif
}

int main(int argc, char** argv) {
#if defined(CELL_PROJECT_CHECK_DESIGNER)
    if (argc == 2 && std::string_view(argv[1]) == "--designer-bindings") return checkDesignerBindings();
#endif
    std::map<std::string, const cell::Parameter*> parameters;
    std::set<cell::ParamId> ids;
    for (const auto& parameter : cell::plugin::descriptor().parameters) {
        if (parameter.key.empty() || parameter.key.find_first_of("\t\r\n") != std::string::npos ||
            !parameters.emplace(parameter.key, &parameter).second || !ids.insert(parameter.id).second) {
            std::cerr << "Invalid or duplicate parameter identity for " << parameter.key << '\n';
            return 1;
        }
    }
    if (argc == 2 && std::string_view(argv[1]) == "--designer-catalog") return exportDesignerCatalog();

    std::map<std::string, std::string> current;
    for (const auto& [key, parameter] : parameters) {
        std::ostringstream mapping;
        mapping.imbue(std::locale::classic());
        mapping << std::setprecision(std::numeric_limits<double>::max_digits10)
                << '\t' << parameter->minimum << '\t' << parameter->maximum
                << '\t' << parameter->defaultValue << '\t' << parameter->step
                << '\t' << parameter->skew << '\t' << parameter->automatable;
        current.emplace(key + '\t' + std::to_string(parameter->id), mapping.str());
    }

    if (argc == 2 && std::string_view(argv[1]) == "--snapshot") {
        for (const auto& [identity, mapping] : current)
            std::cout << identity << mapping << '\n';
        return std::cout ? 0 : 1;
    }
    if (argc != 2) {
        std::cerr << "Provide a released parameter baseline, --snapshot, or --designer-catalog\n";
        return 1;
    }

    std::ifstream baseline(argv[1]);
    if (!baseline) {
        std::cerr << "Cannot read parameter baseline: " << argv[1] << '\n';
        return 1;
    }
    bool compatible = true;
    for (std::string entry; std::getline(baseline, entry);) {
        if (!entry.empty() && entry.back() == '\r')
            entry.pop_back();
        const auto separator = entry.find('\t');
        const auto mappingStart = separator == std::string::npos ? separator : entry.find('\t', separator + 1);
        const auto identity = entry.substr(0, mappingStart);
        const auto parameter = current.find(identity);
        if (parameter == current.end()) {
            std::cerr << "Released parameter missing, changed, or duplicated: " << identity << '\n';
            compatible = false;
            continue;
        }
        if (mappingStart != std::string::npos && entry.substr(mappingStart) != parameter->second) {
            std::cerr << "Released parameter mapping changed: " << identity
                      << "\n  expected: " << entry
                      << "\n  actual:   " << identity << parameter->second << '\n';
            compatible = false;
        }
        current.erase(parameter);
    }
    return compatible && baseline.eof() && !baseline.bad() ? 0 : 1;
}
