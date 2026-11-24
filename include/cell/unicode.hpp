// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <array>
#include <cstdint>

namespace cell::unicode {
enum class Case {lower,upper,fold};
struct CaseMapping {std::array<char32_t,3>code{};unsigned char count{};};
// Full, context-independent Unicode mappings. No locale/global state, runtime
// database, allocation or OS service. Original text is not normalized.
CaseMapping mapCase(char32_t,Case)noexcept;
char32_t foldSingle(char32_t)noexcept;
// Letters, combining marks, numbers and underscore for Unicode-aware names.
bool isWordCharacter(char32_t)noexcept;
}
