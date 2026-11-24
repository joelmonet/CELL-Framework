// Original Cell Framework implementation; see LICENSE.
#pragma once
#include <cell/text.hpp>
#include <array>
#include <span>
namespace cell {
// Control/worker-thread service. Uses the operating system's random provider;
// generation must not run in an audio callback.
void randomBytes(std::span<std::byte>);
class Uuid {
    std::array<std::byte,16>bytes_{};
public:
    Uuid(){randomBytes(bytes_);bytes_[6]=std::byte((std::to_integer<unsigned>(bytes_[6])&15u)|64u);bytes_[8]=std::byte((std::to_integer<unsigned>(bytes_[8])&63u)|128u);}
    Text toString()const;
    Text toDashedString()const;
    const auto&bytes()const noexcept{return bytes_;}
    bool operator==(const Uuid&)const=default;
};
}
