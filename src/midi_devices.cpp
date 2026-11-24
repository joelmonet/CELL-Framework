#include <cell/files.hpp>
#include <cell/json.hpp>
#include <cell/midi_devices.hpp>
#include <chrono>
#include <cstdlib>
#include <functional>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace cell::midi {
namespace {
std::atomic<std::uint64_t> nextOwner{};
template <class T> bool unique(const T &notes) noexcept {
  std::array<bool, 128> seen{};
  for (auto n : notes) {
    if (n > 127 || seen[n])
      return false;
    seen[n] = true;
  }
  return true;
}
bool text(std::string_view s, std::size_t maximum) {
  return !s.empty() && s.size() <= maximum && s.find('\0') == s.npos;
}
using V = json::Value;
V array(const std::array<std::uint8_t, 16> &notes) {
  V::Array result;
  for (auto n : notes)
    result.emplace_back(double(n));
  return V(std::move(result));
}
unsigned number(const V &v, unsigned lo, unsigned hi) {
  const auto *n = v.number();
  if (!n || *n < lo || *n > hi || *n != std::floor(*n))
    throw std::invalid_argument("Invalid controller profile number");
  return unsigned(*n);
}
std::array<std::uint8_t, 16> notes(const V &v) {
  const auto *a = v.array();
  if (!a || a->size() != 16)
    throw std::invalid_argument("A controller map needs sixteen notes");
  std::array<std::uint8_t, 16> r;
  for (unsigned n = 0; n < 16; ++n)
    r[n] = std::uint8_t(number((*a)[n], 0, 127));
  return r;
}
constexpr json::Limits limits{512 * 1024, 16384, 256 * 1024, 8};
V mappingValue(const PadMapping &m) {
  return V(V::Object{{"schema", V("cell.midi.pad-map.v1")},
                     {"enabled", V(m.enabled)},
                     {"channel", V(double(m.channel))},
                     {"input", array(m.input)},
                     {"output", array(m.output)}});
}
class FileLock {
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  OVERLAPPED overlap_{};

public:
  explicit FileLock(const std::filesystem::path &p) {
    handle_ = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
      throw std::runtime_error("Cannot open controller preset lock");
    if (!LockFileEx(handle_,
                    LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1,
                    0, &overlap_)) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
      throw std::runtime_error("Controller presets are busy in another window");
    }
  }
  ~FileLock() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      UnlockFileEx(handle_, 0, 1, 0, &overlap_);
      CloseHandle(handle_);
    }
  }
#else
  int handle_ = -1;

public:
  explicit FileLock(const std::filesystem::path &p) {
    handle_ = ::open(p.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (handle_ < 0)
      throw std::runtime_error("Cannot open controller preset lock");
    if (flock(handle_, LOCK_EX | LOCK_NB)) {
      ::close(handle_);
      handle_ = -1;
      throw std::runtime_error("Controller presets are busy in another window");
    }
  }
  ~FileLock() {
    if (handle_ >= 0) {
      flock(handle_, LOCK_UN);
      ::close(handle_);
    }
  }
#endif
};
} // namespace
bool valid(const PadProfile &p) noexcept {
  return text(p.id, 128) && text(p.name, 128) && p.channel >= 1 &&
         p.channel <= 16 && unique(p.notes) &&
         (p.format.empty() ||
          (p.format.size() >= 2 && p.format.size() <= 17 &&
           p.format[0] == '.' &&
           std::all_of(p.format.begin() + 1, p.format.end(), [](char c) {
             return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
           })));
}
std::string padFormat(std::string_view s) {
  if (s.empty())
    return {};
  if (s.front() == '.')
    s.remove_prefix(1);
  if (s.empty() || s.size() > 16)
    throw std::invalid_argument("Invalid library file extension");
  std::string result = ".";
  for (char c : s) {
    if (c >= 'A' && c <= 'Z')
      c += char('a' - 'A');
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
      throw std::invalid_argument("Invalid library file extension");
    result += c;
  }
  return result;
}
std::string PadProfiles::defaultFor(std::string_view format) const {
  const auto key = padFormat(format);
  const auto found = formatDefaults.find(key);
  return found == formatDefaults.end() ? defaultId : found->second;
}
bool valid(const PadMapping &m) noexcept {
  return m.channel >= 1 && m.channel <= 16 && (!m.enabled || unique(m.input)) &&
         std::all_of(m.input.begin(), m.input.end(),
                     [](auto n) { return n < 128; }) &&
         std::all_of(m.output.begin(), m.output.end(),
                     [](auto n) { return n < 128; });
}
std::string writePadMapping(const PadMapping &m) {
  if (!valid(m))
    throw std::invalid_argument("Invalid pad mapping");
  return json::write(mappingValue(m));
}
PadMapping readPadMapping(std::string_view s) {
  const auto v = json::read(s, limits);
  if (v["schema"].text() != "cell.midi.pad-map.v1" || !v["enabled"].boolean())
    throw std::invalid_argument("Invalid pad mapping schema");
  PadMapping m{*v["enabled"].boolean(), number(v["channel"], 1, 16),
               notes(v["input"]), notes(v["output"])};
  if (!valid(m))
    throw std::invalid_argument("Invalid pad mapping");
  return m;
}
PadInput::PadInput() : owner_(++nextOwner) {}
bool PadInput::mapping(const PadMapping &value) noexcept {
  if (!valid(value))
    return false;
  if (value == desired_)
    return true;
  if (!mappings_.push(value))
    return false;
  desired_ = value;
  return true;
}
bool PadInput::capture(std::uint64_t value) noexcept {
  if (!value)
    return false;
  auto old = token_.load(std::memory_order_acquire);
  if (old && old != value)
    return false;
  if (old == value)
    return true;
  overflow_.store(0, std::memory_order_relaxed);
  token_.store(value, std::memory_order_release);
  return true;
}
bool PadInput::cancel(std::uint64_t value) noexcept {
  if (!value)
    return false;
  return token_.compare_exchange_strong(value, 0, std::memory_order_acq_rel);
}
PadInputView PadInput::poll() {
  PadInputView v;
  v.owner = owner_;
  v.mapping = desired_;
  const auto token = token_.load(std::memory_order_acquire);
  v.overflow = token && overflow_.load(std::memory_order_acquire) == token;
  PadCapture e;
  unsigned count = 0;
  while (count++ < 64 && captures_.pop(e))
    if (e.token == token)
      v.captures.push_back(e);
  return v;
}
bool PadInput::active() const noexcept {
  return token_.load(std::memory_order_acquire) || audio_.enabled || voices_ ||
         mappings_.freeCapacity() != 8;
}
void PadInput::resetAudio() noexcept {
  held_.fill({});
  references_.fill(0);
  voices_ = 0;
  captureHeld_.fill(0);
  audioToken_ = 0;
}
bool PadInput::process(std::span<const MidiEvent> input,
                       std::span<MidiEvent> output, unsigned frames,
                       std::size_t &count) noexcept {
  count = 0;
  if (input.size() > output.size())
    return false;
  PadMapping next;
  unsigned changes = 0;
  while (changes++ < 8 && mappings_.pop(next))
    audio_ = next;
  const auto token = token_.load(std::memory_order_acquire);
  if (token != audioToken_) {
    audioToken_ = token;
    captureHeld_.fill(0);
  }
  for (auto event : input) {
    if (event.isSysEx() || event.isUmp()) {
      if (event.frame < frames && (!event.isUmp() || validUmp(event))) output[count++] = event;
      continue;
    }
    if (!valid(event) || event.frame >= frames)
      continue;
    const unsigned kind = event.data[0] & 0xf0, ch = event.data[0] & 15,
                   pitch = event.data[1], key = ch * 128 + pitch;
    const bool on = kind == 0x90 && event.data[2],
               off = kind == 0x80 || (kind == 0x90 && !event.data[2]);
    if (token && (on || off)) {
      if (on && !captureHeld_[key]) {
        if (!captures_.push({token, ch + 1, pitch, ++captureSequence_}))
          overflow_.store(token, std::memory_order_release);
      }
      captureHeld_[key] = on;
    }
    if (kind == 0xb0 && (pitch == 120 || pitch == 123)) {
      for (unsigned n = 0; n < 128; ++n) {
        const auto k = ch * 128 + n;
        if (held_[k].count) {
          held_[k] = {};
          --voices_;
        }
        references_[k] = 0;
        captureHeld_[k] = 0;
      }
    }
    auto &h = held_[key];
    if (on) {
      unsigned target = pitch;
      if (h.count)
        target = h.target;
      else if (audio_.enabled && ch + 1 == audio_.channel) {
        for (unsigned n = 0; n < 16; ++n)
          if (audio_.input[n] == pitch) {
            target = audio_.output[n];
            break;
          }
      }
      if (h.count == UINT16_MAX)
        continue;
      if (!h.count) {
        h.target = std::uint8_t(target);
        ++voices_;
      }
      ++h.count;
      ++references_[ch * 128 + target];
      event.data[1] = std::uint8_t(target);
    } else if (off && h.count) {
      event.data[1] = h.target;
      const auto target = ch * 128 + h.target;
      if (!--h.count)
        --voices_;
      if (references_[target] && --references_[target])
        continue;
    } else if (off && references_[key])
      continue;
    else if (kind == 0xa0 && h.count)
      event.data[1] = h.target;
    output[count++] = event;
  }
  return true;
}
void PadLearner::begin(std::uint64_t token) {
  if (!token)
    throw std::invalid_argument("Invalid learn token");
  token_ = token;
  stage_ = Stage::first;
  channel_ = count_ = 0;
  message_ = "Hit pad 1";
}
void PadLearner::individual() {
  if (stage_ == Stage::idle)
    return;
  stage_ = Stage::individual;
  count_ = channel_ = 0;
  message_ = "Hit pad 1 of 16";
}
bool PadLearner::feed(const PadCapture &e) {
  if (e.token != token_ || e.channel < 1 || e.channel > 16 || e.note > 127 ||
      stage_ == Stage::idle || stage_ == Stage::ready)
    return false;
  if (channel_ && e.channel != channel_) {
    message_ = "Use the same MIDI channel for all pads";
    return false;
  }
  if (stage_ == Stage::first) {
    notes_[0] = std::uint8_t(e.note);
    channel_ = e.channel;
    stage_ = Stage::last;
    message_ = "Hit pad 16";
    return true;
  }
  if (stage_ == Stage::last) {
    const int difference = int(e.note) - int(notes_[0]);
    if (difference != 15 && difference != -15) {
      message_ = "Irregular layout: choose LEARN ALL 16";
      return false;
    }
    for (unsigned n = 1; n < 16; ++n)
      notes_[n] =
          std::uint8_t(int(notes_[0]) + int(n) * (difference > 0 ? 1 : -1));
    stage_ = Stage::ready;
    message_ = "Mapping ready - APPLY, then save a device preset";
    return true;
  }
  if (std::find(notes_.begin(), notes_.begin() + count_, e.note) !=
      notes_.begin() + count_) {
    message_ = "That note is already assigned; hit a different pad";
    return false;
  }
  channel_ = e.channel;
  notes_[count_++] = std::uint8_t(e.note);
  if (count_ == 16) {
    stage_ = Stage::ready;
    message_ = "Mapping ready - APPLY, then save a device preset";
  } else
    message_ = "Hit pad " + std::to_string(count_ + 1) + " of 16";
  return true;
}
PadProfile PadLearner::profile(std::string id, std::string name) const {
  if (stage_ != Stage::ready)
    throw std::logic_error("Pad learn is incomplete");
  return {std::move(id), std::move(name), channel_, notes_};
}
std::filesystem::path PadProfileStore::userFile() {
  auto env = [](const char *key) {
    const auto *p = std::getenv(key);
    return p ? std::filesystem::u8path(p) : std::filesystem::path{};
  };
#ifdef _WIN32
  auto base = env("APPDATA");
#elif defined(__APPLE__)
  auto base = env("HOME");
  if (!base.empty())
    base /= "Library/Application Support";
#else
  auto base = env("XDG_CONFIG_HOME");
  if (!base.is_absolute()) {
    base = env("HOME");
    if (!base.empty())
      base /= ".config";
  }
#endif
  if (!base.is_absolute())
    throw std::runtime_error("User configuration directory is unavailable");
  return base / "SoundCell" / "controller-profiles.json";
}
PadProfiles PadProfileStore::read() const {
  PadProfiles result;
  if (!std::filesystem::exists(file_))
    return result;
  const auto b = files::readBounded(file_, limits.inputBytes);
  const auto root =
      json::read({reinterpret_cast<const char *>(b.data()), b.size()}, limits);
  if (root["schema"].text() != "cell.midi.device-profiles.v1" ||
      !root["default"].string() || !root["profiles"].array() ||
      root["profiles"].array()->size() > 128)
    throw std::invalid_argument("Invalid controller profile library");
  result.defaultId = std::string(root["default"].text());
  for (const auto &p : *root["profiles"].array()) {
    if (!p["format"].isNull() && !p["format"].string())
      throw std::invalid_argument("Invalid controller format");
    PadProfile profile{std::string(p["id"].text()),
                       std::string(p["name"].text()),
                       number(p["channel"], 1, 16), notes(p["notes"]),
                       std::string(p["format"].text())};
    if (!valid(profile) ||
        std::any_of(result.profiles.begin(), result.profiles.end(),
                    [&](const auto &q) { return q.id == profile.id; }))
      throw std::invalid_argument("Invalid or duplicate controller profile");
    result.profiles.push_back(std::move(profile));
  }
  if (!result.defaultId.empty() &&
      std::none_of(result.profiles.begin(), result.profiles.end(),
                   [&](const auto &p) {
                     return p.id == result.defaultId && p.format.empty();
                   }))
    throw std::invalid_argument("Missing default controller profile");
  if (!root["formatDefaults"].isNull()) {
    const auto *defaults = root["formatDefaults"].object();
    if (!defaults || defaults->size() > 128)
      throw std::invalid_argument("Invalid format defaults");
    for (const auto &[ext, id] : *defaults) {
      if (ext.empty() || padFormat(ext) != ext || !id.string() ||
          std::none_of(result.profiles.begin(), result.profiles.end(),
                       [&](const auto &p) {
                         return p.id == id.text() &&
                                (p.format.empty() || p.format == ext);
                       }))
        throw std::invalid_argument("Missing or incompatible format default");
      result.formatDefaults[ext] = std::string(id.text());
    }
  }
  return result;
}
void PadProfileStore::mutate(const std::function<void(PadProfiles &)> &change) {
  if (!file_.is_absolute())
    throw std::invalid_argument("Controller profile path must be absolute");
  std::filesystem::create_directories(file_.parent_path());
  auto lockPath = file_;
  lockPath += ".lock";
  FileLock lock(lockPath);
  auto library = read();
  change(library);
  V::Array profiles;
  for (const auto &p : library.profiles)
    profiles.emplace_back(V::Object{{"id", V(p.id)},
                                    {"name", V(p.name)},
                                    {"channel", V(double(p.channel))},
                                    {"notes", array(p.notes)},
                                    {"format", V(p.format)}});
  V::Object defaults;
  for (const auto &[ext, id] : library.formatDefaults)
    defaults[ext] = V(id);
  const auto encoded =
      json::write(V(V::Object{{"schema", V("cell.midi.device-profiles.v1")},
                              {"default", V(library.defaultId)},
                              {"formatDefaults", V(std::move(defaults))},
                              {"profiles", V(std::move(profiles))}}),
                  limits);
  files::atomicWrite(
      file_, std::as_bytes(std::span(encoded.data(), encoded.size())), true);
}
void PadProfileStore::save(const PadProfile &p, bool replace) {
  if (!valid(p))
    throw std::invalid_argument("Invalid device preset");
  mutate([&](PadProfiles &library) {
    auto found = std::find_if(library.profiles.begin(), library.profiles.end(),
                              [&](const auto &q) { return q.id == p.id; });
    if (found != library.profiles.end()) {
      if (!replace)
        throw std::runtime_error(
            "A device preset with that identity already exists");
      if (library.defaultId == p.id && !p.format.empty())
        throw std::invalid_argument(
            "A general default must apply to all formats");
      for (const auto &[extension, id] : library.formatDefaults)
        if (id == p.id && !p.format.empty() && p.format != extension)
          throw std::invalid_argument(
              "Preset is still the default for another file extension");
      *found = p;
    } else {
      if (library.profiles.size() >= 128)
        throw std::runtime_error("Controller preset library is full");
      library.profiles.push_back(p);
    }
  });
}
void PadProfileStore::makeDefault(std::string_view id,
                                  std::string_view format) {
  const auto key = padFormat(format);
  mutate([&](PadProfiles &library) {
    if (!id.empty() &&
        std::none_of(library.profiles.begin(), library.profiles.end(),
                     [&](const auto &p) {
                       return p.id == id && (key.empty() ? p.format.empty()
                                                         : p.format.empty() ||
                                                               p.format == key);
                     }))
      throw std::invalid_argument(
          "Unknown or incompatible default device preset");
    if (key.empty())
      library.defaultId = id;
    else if (id.empty())
      library.formatDefaults.erase(key);
    else
      library.formatDefaults[key] = id;
  });
}
std::string newPadProfileId() {
  return "device-" +
         std::to_string(
             std::chrono::system_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++nextOwner);
}
} // namespace cell::midi
