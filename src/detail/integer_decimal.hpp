#pragma once
#include <algorithm>
#include <limits>
#include <string>
namespace cell::detail {
// Preserve the full integer representation, including libc++'s 128-bit file
// clock ticks. Taking a remainder avoids negating the minimum signed integer.
template<class Integer> std::string integerDecimal(Integer value) {
    static_assert(std::numeric_limits<Integer>::is_integer);
    const bool negative = value < 0;
    std::string result;
    do {
        auto digit = value % 10;
        if (digit < 0) digit = -digit;
        result.push_back(char('0' + int(digit)));
        value /= 10;
    } while (value != 0);
    if (negative) result.push_back('-');
    std::reverse(result.begin(), result.end());
    return result;
}
}
