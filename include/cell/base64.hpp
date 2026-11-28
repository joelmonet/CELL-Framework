// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cell::encoding {
inline constexpr std::string_view base64Digits =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string encodeBase64(std::span<const std::byte> bytes) {
    std::string result;
    result.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto remaining = bytes.size() - i;
        const auto a = std::to_integer<unsigned>(bytes[i]);
        const auto b = remaining > 1 ? std::to_integer<unsigned>(bytes[i + 1]) : 0;
        const auto c = remaining > 2 ? std::to_integer<unsigned>(bytes[i + 2]) : 0;
        result += base64Digits[a >> 2];
        result += base64Digits[((a & 3) << 4) | (b >> 4)];
        result += remaining > 1 ? base64Digits[((b & 15) << 2) | (c >> 6)] : '=';
        result += remaining > 2 ? base64Digits[c & 63] : '=';
    }
    return result;
}

inline std::optional<std::vector<std::byte>> decodeBase64(std::string_view text,
                                                          std::size_t maxBytes) {
    if (text.size() % 4 || text.size() > maxBytes / 3 * 4)
        return {};
    std::vector<std::byte> result;
    result.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const auto a = base64Digits.find(text[i]), b = base64Digits.find(text[i + 1]);
        const auto c = base64Digits.find(text[i + 2]), d = base64Digits.find(text[i + 3]);
        if (a == base64Digits.npos || b == base64Digits.npos)
            return {};
        const bool final = i + 4 == text.size();
        if (c == base64Digits.npos &&
            (!final || text[i + 2] != '=' || text[i + 3] != '=' || (b & 15)))
            return {};
        if (c != base64Digits.npos && d == base64Digits.npos &&
            (!final || text[i + 3] != '=' || (c & 3)))
            return {};
        result.push_back(std::byte((a << 2) | (b >> 4)));
        if (c != base64Digits.npos)
            result.push_back(std::byte(((b & 15) << 4) | (c >> 2)));
        if (d != base64Digits.npos)
            result.push_back(std::byte(((c & 3) << 6) | d));
    }
    return result;
}
} // namespace cell::encoding
