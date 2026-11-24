#pragma once

#include <cell/files.hpp>
#include <cell/jobs.hpp>
#include <map>
#include <set>

namespace cell::assets {
std::string pathText(const std::filesystem::path&);
std::string fold(std::string_view);
struct Entry {
    std::filesystem::path path;
    std::string identity, name, extension, sortKey;
    bool directory{}, available = true;
    std::uintmax_t bytes{};
};
struct Filter {
    std::vector<std::string> extensions;
    bool showHidden{}, recursive{};
    std::size_t maximumEntries = 20000;
};
struct Listing {
    std::filesystem::path directory;
    std::vector<Entry> entries;
    std::string error;
    bool truncated{};
};

// A directory scan owns only paths/options; it never borrows its browser/editor.
// Re-navigation cancels the old generation. No directory IO runs in poll().
class Browser {
public:
    explicit Browser(std::shared_ptr<jobs::Pool> = jobs::Pool::shared());
    bool scan(std::filesystem::path, Filter = {});
    bool poll(); // true once per newly published result, including errors
    void cancel();
    bool busy() const noexcept { return busy_; }
    const Listing& listing() const noexcept;
private:
    std::shared_ptr<jobs::Pool> pool_;
    jobs::Owner owner_;
    jobs::Job job_;
    std::shared_ptr<const jobs::Payload> result_;
    Listing failure_;
    bool busy_{};
};

struct Annotation { bool favorite{}; std::vector<std::string> tags; };
// Metadata is independent of the source files and preserves entries for missing
// assets. Persistence is explicit; opening a browser does not write user files.
class Annotations {
public:
    const Annotation* find(std::string_view identity) const;
    bool set(std::string identity, Annotation);
    bool matches(const Entry&, std::string_view search, bool favoritesOnly = false,
                 std::span<const std::string> requiredTags = {}) const;
    std::vector<std::byte> save() const;
    bool load(std::span<const std::byte>); // validate then replace; failure retains old data
private:
    std::map<std::string, Annotation, std::less<>> entries_;
    std::size_t encodedBytes_ = 12;
};
}
