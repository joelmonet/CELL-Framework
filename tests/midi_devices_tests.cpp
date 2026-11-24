#include "allocation_watch.hpp"
#include <cell/files.hpp>
#include <cell/plugin.hpp>
#include <iostream>
#include <thread>
using namespace cell;
namespace {
void check(bool b, const char *why) {
  if (!b)
    throw std::runtime_error(why);
}
MidiEvent note(unsigned pitch, bool down = true, unsigned channel = 1,
               unsigned frame = 0) {
  return {frame,
          {std::uint8_t((down ? 0x90 : 0x80) | (channel - 1)),
           std::uint8_t(pitch), 100},
          3};
}
midi::PadMapping mapping() {
  midi::PadMapping m;
  m.enabled = true;
  for (unsigned n = 0; n < 16; ++n) {
    m.input[n] = std::uint8_t(60 + n);
    m.output[n] = std::uint8_t(36 + n);
  }
  return m;
}
void learning() {
  midi::PadLearner l;
  l.begin(1);
  check(!l.feed({2, 1, 60}) && l.stage() == midi::PadLearner::Stage::first,
        "Stale learn event accepted");
  check(l.feed({1, 10, 60}) && l.nextPad() == 16, "First pad learn failed");
  check(!l.feed({1, 1, 75}) && l.feed({1, 10, 75}),
        "Learn did not isolate the MIDI channel");
  auto p = l.profile("id", "Keyboard");
  check(p.channel == 10 && p.notes[0] == 60 && p.notes[15] == 75,
        "Learned offset differs from hardware");
  l.begin(2);
  l.feed({2, 1, 90});
  check(l.feed({2, 1, 75}) && l.profile("rev", "Reverse").notes[1] == 89,
        "Descending pad order failed");
  l.begin(3);
  l.feed({3, 1, 36});
  check(!l.feed({3, 1, 70}) && l.stage() == midi::PadLearner::Stage::last,
        "Irregular layout was silently interpolated");
  l.individual();
  for (unsigned n = 0; n < 16; ++n)
    check(l.feed({3, 2, (n * 7) % 128}), "Individual pad learn failed");
  check(l.profile("all", "Custom").notes[15] == 105,
        "Individual pad order was rearranged");
}
void realtime() {
  midi::PadInput in;
  auto m = mapping();
  check(in.mapping(m) && in.capture(12), "Mapper control admission failed");
  std::array<MidiEvent, 128> out{};
  std::size_t count{};
  auto run = [&](std::span<const MidiEvent> e) {
    cell_test::allocations = cell_test::deletions = 0;
    cell_test::audio = true;
    const auto ok = in.process(e, out, 64, count);
    cell_test::audio = false;
    check(ok && !cell_test::allocations && !cell_test::deletions,
          "Mapper allocated, reclaimed or rejected valid callback");
  };
  std::array<MidiEvent, 5> events{note(60, true, 1, 3), note(60, true, 1, 7),
                                  note(60, false, 1, 10), note(60, true, 2, 12),
                                  note(75, true, 1, 20)};
  run(events);
  check(count == 4 && out[0].data[1] == 36 && out[0].frame == 3 &&
            out[2].data[1] == 60 && out[2].data[0] == 0x91 &&
            out[3].data[1] == 51,
        "Mapping changed timestamps/channels or lost overlap ownership");
  auto view = in.poll();
  check(view.captures.size() == 3 && view.captures[0].note == 60 &&
            view.captures[1].channel == 2,
        "Learn captured repeats, note-offs or remapped notes");
  auto changed = m;
  changed.output[0] = 90;
  check(in.mapping(changed), "Map change rejected");
  const std::array<MidiEvent, 1> off{note(60, false)};
  run(off);
  check(count == 1 && out[0].data[1] == 36,
        "A held note-off changed destination after preset switch");
  run(std::array{note(60)});
  check(out[0].data[1] == 90, "New note did not use the selected map");
  check(!in.cancel(2) && in.cancel(12),
        "Stale UI cancelled another learn session");
  in.resetAudio();
  m.output[1] = m.output[0];
  check(in.mapping(m), "Duplicate target fixture failed");
  run(std::array{note(60), note(61), note(60, false), note(61, false)});
  check(count == 3 && out[2].data[0] == 0x80 && out[2].data[1] == 36,
        "Two inputs sharing a target cut off one another");
  in.resetAudio();
  m = mapping();
  check(in.mapping(m), "Mapping reset failed");
  run(std::array{note(60)});
  m.enabled = false;
  check(in.mapping(m), "Disable rejected");
  run(std::array{note(60, false), note(61)});
  check(count == 2 && out[0].data[1] == 36 && out[1].data[1] == 61,
        "Disabling lost a held destination or kept remapping new notes");
  in.resetAudio();
  check(in.capture(14), "Capture restart failed");
  std::array<MidiEvent, 128> flood;
  for (unsigned n = 0; n < 128; ++n)
    flood[n] = note(n);
  run(flood);
  check(in.poll().overflow, "Capture saturation was hidden");
  check(in.cancel(14), "Capture cancel failed");
  midi::PadInput full;
  for (unsigned n = 0; n < 8; ++n) {
    auto q = mapping();
    q.output[0] = std::uint8_t(n);
    check(full.mapping(q), "Config queue filled too early");
  }
  const auto before = full.mapping();
  auto q = before;
  q.output[0] = 99;
  check(!full.mapping(q) && full.mapping() == before,
        "Rejected config changed the saved desired state");
}
class Capture : public Processor {
public:
  std::vector<MidiEvent> seen;
  void prepare(const ProcessSpec &) override { seen.reserve(32); }
  void reset() noexcept override {}
  void process(AudioBlock<float>, const ParameterBank &, const Transport &,
               std::span<const MidiEvent> e) noexcept override {
    seen.insert(seen.end(), e.begin(), e.end());
  }
};
void engine() {
  PluginDescriptor d{"test.devices", "Devices", "Cell", "1",   2,   {},
                     false,          800,       600,    false, true};
  auto p = std::make_unique<Capture>();
  auto *observer = p.get();
  Engine e(d, std::move(p));
  e.prepare(48000, 64);
  check(e.padInput()->mapping(mapping()) && e.padInput()->capture(1),
        "Engine mapper unavailable");
  check(e.controllerMidi({1, 1, midi::ControlAction::open, {}}) &&
            e.controllerMidi({1, 2, midi::ControlAction::event, note(60)}),
        "Private keyboard fixture failed");
  AudioBuffer<float> a;
  a.resize(2, 64);
  e.process(a.block(), {}, {}, std::array{note(60, true, 1, 17)});
  check(observer->seen.size() == 2 && observer->seen[0].data[1] == 60 &&
            observer->seen[1].data[1] == 36 && observer->seen[1].frame == 17,
        "Hardware map changed private keyboard audition");
  check(e.padInput()->poll().captures.size() == 1,
        "Learn captured the on-screen keyboard");
}
void storage() {
  const auto dir =
      std::filesystem::temp_directory_path() / midi::newPadProfileId();
  std::filesystem::create_directories(dir);
  try {
    midi::PadProfile p{"keys", "My keys", 1, mapping().input, {}};
    midi::PadProfileStore a(dir / "devices.json"), b(dir / "devices.json");
    a.save(p);
    p.id = "pads";
    p.name = "My pads";
    p.format = ".xpm";
    b.save(p);
    a.makeDefault("keys");
    a.makeDefault("pads", ".xpm");
    const auto lib = b.read();
    check(lib.profiles.size() == 2 && lib.defaultFor("XPM") == "pads" &&
              lib.defaultFor(".adg") == "keys",
          "Per-format defaults or concurrent-instance merge failed");
    const auto before = files::readBounded(a.file(), 512 * 1024);
    bool rejected = false;
    try {
      a.save(p);
    } catch (...) {
      rejected = true;
    }
    check(rejected && files::readBounded(a.file(), 512 * 1024) == before,
          "Duplicate save overwrote a device preset");
    rejected = false;
    try {
      a.makeDefault("pads", ".adg");
    } catch (...) {
      rejected = true;
    }
    check(rejected, "Incompatible format default accepted");
    auto m = mapping();
    check(midi::readPadMapping(midi::writePadMapping(m)) == m,
          "Map persistence changed notes");
    p.notes[1] = p.notes[0];
    check(!midi::valid(p), "Duplicate controller notes accepted");
    std::filesystem::remove_all(dir);
  } catch (...) {
    std::filesystem::remove_all(dir);
    throw;
  }
}
} // namespace
int main() {
  try {
    learning();
    realtime();
    engine();
    storage();
    std::cout << "PASS hardware pad learn, arbitrary/reversed maps, held "
                 "releases, channel isolation, callback allocation guard, "
                 "capture backpressure, private keyboard bypass and "
                 "per-extension device defaults\n";
  } catch (const std::exception &e) {
    cell_test::audio = false;
    std::cerr << e.what() << '\n';
    return 1;
  }
}
