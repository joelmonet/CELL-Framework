// Original Cell Framework implementation; see LICENSE.
#include <array>
#include <cell/sample_clock.hpp>
#include <iostream>
void need(bool v, const char *why) {
  if (!v)
    throw std::runtime_error(why);
}
int main() {
  try {
    for (const double rate : {44100., 48000., 96000., 192000., 384000.})
      for (const double origin : {0., 137.125, 14400.123456789, 86400.054321}) {
        for (const std::int64_t frame :
             {0LL, 1LL, 257LL, 44100LL, 2147483648LL, 1099511627776LL}) {
          const auto time = double(static_cast<long double>(origin) +
                                   static_cast<long double>(frame) / rate);
          need(cell::audio::sampleBoundary(time, origin, rate) == frame,
               "Integral source frame changed at a nonzero or long origin");
        }
        for (const double fraction : {.25, .5, .75}) {
          const auto time = double(static_cast<long double>(origin) +
                                   (12345.L + fraction) / rate);
          need(cell::audio::sampleBoundary(time, origin, rate) == 12346,
               "A genuinely fractional boundary was rounded down");
        }
      }
    need(cell::audio::sampleBoundary(.9, 0, 48000) == 43200 &&
             cell::audio::sampleBoundary(-.1, 0, 48000) == -4800,
         "Decimal seconds chose the wrong sample side");
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
      bool rejected = false;
      try {
        cell::audio::sampleBoundary(bad, 0, 48000);
      } catch (const std::exception &) {
        rejected = true;
      }
      need(rejected, "Nonfinite sample clock was accepted");
    }
    for (const double rate :
         {0., -1., std::numeric_limits<double>::infinity()}) {
      bool rejected = false;
      try {
        cell::audio::sampleBoundary(1, 0, rate);
      } catch (const std::exception &) {
        rejected = true;
      }
      need(rejected, "Invalid sample rate was accepted");
    }
    bool overflow = false;
    try {
      cell::audio::sampleBoundary(std::numeric_limits<double>::max(), 0, 48000);
    } catch (const std::exception &) {
      overflow = true;
    }
    need(overflow, "Unrepresentable source clock was narrowed to an integer");
    std::cout << "PASS Cell sample clock: 44.1-384 kHz, negative/decimal "
                 "times, fractional boundaries, four-hour/day origins, 64-bit "
                 "frames, nonfinite/rate/precision rejection.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAIL Cell sample clock: " << e.what() << '\n';
    return 1;
  }
}
