#pragma once
#include <cell/parameters.hpp>
namespace cell::designer {
inline constexpr auto parameters = [] {
    std::array<cell::ParamId, 92> ids{};
    for (unsigned index = 0; index < ids.size(); ++index) ids[index] = 17 + index;
    return ids;
}();
}
