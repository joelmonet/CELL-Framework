#include <cell/assets.hpp>
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace cell::assets {
std::string pathText(const std::filesystem::path& path) {
    const auto value = path.u8string(); return {reinterpret_cast<const char*>(value.data()), value.size()};
}
std::string fold(std::string_view value) {
    std::string text(value);
    for (auto& ch : text) if (ch >= 'A' && ch <= 'Z') ch = char(ch + 'a' - 'A');
    return text;
}
namespace {
bool validText(std::string_view text, std::size_t maximum) {
    return !text.empty() && text.size() <= maximum && text.find('\0') == text.npos;
}
void writeText(ByteWriter& w, std::string_view text) {
    w.u32(unsigned(text.size()));
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    w.bytes.insert(w.bytes.end(), bytes.begin(), bytes.end());
}
bool readText(ByteReader& r, std::string& text, std::size_t maximum) {
    unsigned size{}; std::span<const std::byte> bytes;
    if (!r.u32(size) || !size || size > maximum || !r.raw(size, bytes)) return false;
    text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return validText(text, maximum);
}
Listing scanDirectory(std::filesystem::path directory, Filter filter, jobs::Context& context) {
    Listing listing; std::error_code error;
    listing.directory = std::filesystem::absolute(directory, error).lexically_normal();
    if (error) { listing.error = "Could not resolve this folder"; return listing; }
    std::set<std::string> extensions;
    for (auto extension : filter.extensions) { extension = fold(extension); if (!extension.starts_with('.')) extension.insert(extension.begin(), '.'); extensions.insert(std::move(extension)); }
    std::vector<std::filesystem::path> pending{listing.directory};
    std::size_t visited = 0;
    while (!pending.empty() && !context.cancelled() && !listing.truncated) {
        auto root = std::move(pending.back()); pending.pop_back();
        std::filesystem::directory_iterator it(root, error), end;
        if (error) { listing.error = "Could not read one or more folders"; error.clear(); continue; }
        for (; it != end; it.increment(error)) {
            if (error) { listing.error = "Folder changed or became unavailable"; error.clear(); break; }
            if (context.cancelled()) return listing;
            const auto name = pathText(it->path().filename());
            if (!filter.showHidden && name.starts_with('.')) continue;
            auto status = it->symlink_status(error);
            if (error) { error.clear(); continue; }
            // Cyclic directory links cannot trap a recursive catalog scan.
            if (std::filesystem::is_symlink(status)) continue;
            const bool directoryEntry = std::filesystem::is_directory(status);
            if (!directoryEntry && !std::filesystem::is_regular_file(status)) continue;
            auto extension = fold(pathText(it->path().extension()));
            if (!directoryEntry && !extensions.empty() && !extensions.contains(extension)) continue;
            if (listing.entries.size() == filter.maximumEntries) { listing.truncated = true; break; }
            Entry entry; entry.path = it->path(); entry.name = name; entry.sortKey = fold(name); entry.directory = directoryEntry;
            entry.identity = pathText(entry.path.lexically_normal()); entry.extension = std::move(extension);
            if (!directoryEntry) { entry.bytes = it->file_size(error); entry.available = !error; error.clear(); }
            if (directoryEntry && filter.recursive) pending.push_back(entry.path);
            listing.entries.push_back(std::move(entry));
            if (++visited % 64 == 0) context.progress(std::min(.95, double(visited) / filter.maximumEntries));
        }
    }
    std::sort(listing.entries.begin(), listing.entries.end(), [](const Entry& a, const Entry& b) {
        if (a.directory != b.directory) return a.directory;
        const auto& left = a.sortKey; const auto& right = b.sortKey;
        return left == right ? a.identity < b.identity : left < right;
    });
    return listing;
}
}
Browser::Browser(std::shared_ptr<jobs::Pool> pool) : pool_(std::move(pool)) {
    if (!pool_) throw std::invalid_argument("browser job pool");
}
bool Browser::scan(std::filesystem::path path, Filter filter) {
    if (path.empty() || !filter.maximumEntries || filter.maximumEntries > 1000000 || filter.extensions.size() > 128) return false;
    for (const auto& extension : filter.extensions) if (!validText(extension, 64)) return false;
    auto token = owner_.next(); job_.cancel(); result_.reset(); failure_ = {}; failure_.directory = path;
    job_ = pool_->submit(token, [path = std::move(path), filter = std::move(filter)](jobs::Context& context) mutable {
        return jobs::Result::success(scanDirectory(std::move(path), std::move(filter), context));
    });
    busy_ = bool(job_);
    if (!busy_) failure_.error = "Background queue is busy; try this folder again";
    return busy_;
}
bool Browser::poll() {
    if (!busy_) return false;
    auto result = job_.poll(); if (!result.finished()) return false;
    busy_ = false;
    if (result.status == jobs::Status::succeeded && result.get<Listing>()) result_ = std::move(result.payload);
    else failure_.error = result.message.empty() ? "Folder scan cancelled" : result.message;
    return true;
}
void Browser::cancel() { owner_.cancel(); job_.cancel(); busy_ = false; }
const Listing& Browser::listing() const noexcept {
    const auto* value = dynamic_cast<const jobs::Value<Listing>*>(result_.get()); return value ? value->data : failure_;
}
const Annotation* Annotations::find(std::string_view identity) const {
    const auto it = entries_.find(identity); return it == entries_.end() ? nullptr : &it->second;
}
bool Annotations::set(std::string identity, Annotation annotation) {
    if (!validText(identity, 8192) || annotation.tags.size() > 32) return false;
    std::set<std::string> seen;
    for (auto& tag : annotation.tags) { if (!validText(tag, 128) || !seen.insert(fold(tag)).second) return false; }
    const auto encodedSize = [](std::string_view key, const Annotation& value) {
        std::size_t size = 12 + key.size();
        for (const auto& tag : value.tags) size += 4 + tag.size();
        return size;
    };
    const auto old = entries_.find(identity);
    const auto prior = old == entries_.end() ? 0 : encodedSize(old->first, old->second);
    if (!annotation.favorite && annotation.tags.empty()) {
        if (old != entries_.end()) { encodedBytes_ -= prior; entries_.erase(old); }
        return true;
    }
    if (old == entries_.end() && entries_.size() >= 20000) return false;
    const auto next = encodedSize(identity, annotation);
    if (next > 16u * 1024 * 1024 - (encodedBytes_ - prior)) return false;
    const auto total = encodedBytes_ - prior + next;
    entries_.insert_or_assign(std::move(identity), std::move(annotation));
    encodedBytes_ = total; return true;
}
bool Annotations::matches(const Entry& entry, std::string_view search, bool favorites, std::span<const std::string> tags) const {
    const auto* annotation = find(entry.identity);
    if (favorites && (!annotation || !annotation->favorite)) return false;
    for (const auto& tag : tags) {
        if (!annotation || std::none_of(annotation->tags.begin(), annotation->tags.end(), [&](const auto& value) { return fold(value) == fold(tag); })) return false;
    }
    std::string haystack = fold(entry.name);
    if (annotation) for (const auto& tag : annotation->tags) haystack += ' ' + fold(tag);
    const auto needle = fold(search);
    // Every whitespace-separated word must match; folders remain navigable.
    std::size_t at = 0;
    while (at < needle.size()) {
        at = needle.find_first_not_of(" \t\r\n", at); if (at == needle.npos) break;
        auto end = needle.find_first_of(" \t\r\n", at); if (end == needle.npos) end = needle.size();
        if (haystack.find(needle.substr(at, end - at)) == haystack.npos) return false;
        at = end;
    }
    return true;
}
std::vector<std::byte> Annotations::save() const {
    ByteWriter w; w.u32(0x41435343); w.u32(1); w.u32(unsigned(entries_.size()));
    for (const auto& [identity, annotation] : entries_) {
        writeText(w, identity); w.u32(annotation.favorite ? 1 : 0); w.u32(unsigned(annotation.tags.size()));
        for (const auto& tag : annotation.tags) writeText(w, tag);
    }
    return std::move(w.bytes);
}
bool Annotations::load(std::span<const std::byte> bytes) {
    if (bytes.size() > 16u * 1024 * 1024) return false;
    ByteReader r(bytes); unsigned magic{}, version{}, count{};
    if (!r.u32(magic) || magic != 0x41435343 || !r.u32(version) || version != 1 || !r.u32(count) || count > 20000) return false;
    Annotations candidate;
    for (unsigned i = 0; i < count; ++i) {
        std::string identity; unsigned favorite{}, tags{};
        if (!readText(r, identity, 8192) || candidate.entries_.contains(identity) || !r.u32(favorite) || favorite > 1 || !r.u32(tags) || tags > 32) return false;
        Annotation annotation; annotation.favorite = favorite;
        for (unsigned n = 0; n < tags; ++n) { std::string tag; if (!readText(r, tag, 128)) return false; annotation.tags.push_back(std::move(tag)); }
        if ((!favorite && !tags) || !candidate.set(std::move(identity), std::move(annotation))) return false;
    }
    if (r.remaining()) return false;
    entries_.swap(candidate.entries_); std::swap(encodedBytes_, candidate.encodedBytes_); return true;
}
}
