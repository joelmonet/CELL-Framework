// Original Cell Framework implementation; see LICENSE.
#pragma once

#include <ARA_API/ARAInterface.h>
#include <cell/streams.hpp>
#include <limits>
#include <stdexcept>

namespace cell::ara {

// Borrowed archive handles are valid only during the host's archive callback.
class ArchiveInput final : public InputStream {
public:
    ArchiveInput(const ARA::ARAArchivingControllerInterface& api,
                 ARA::ARAArchivingControllerHostRef controller, ARA::ARAArchiveReaderHostRef handle)
        : readHost_(api.structSize >= ARA::kARAArchivingControllerInterfaceMinSize ? api.readBytesFromArchive : nullptr), controller_(controller), handle_(handle) {
        if (!controller || !handle || api.structSize < ARA::kARAArchivingControllerInterfaceMinSize ||
            !api.getArchiveSize || !readHost_)
            throw std::invalid_argument("Invalid ARA archive reader");
        setLength(api.getArchiveSize(controller, handle));
    }

    // Also accepts the ARA Library HostArchiveReader without requiring that library.
    template<class Reader>
        requires requires(Reader& reader, ARA::ARASize size, ARA::ARAByte* bytes) {
            reader.getArchiveSize();
            reader.readBytesFromArchive(size, size, bytes);
        }
    explicit ArchiveInput(Reader& reader)
        : wrapper_(&reader), readWrapper_([](void* reader, ARA::ARASize at, ARA::ARASize size, ARA::ARAByte* data) {
            return bool(static_cast<Reader*>(reader)->readBytesFromArchive(at, size, data));
        }) {
        setLength(reader.getArchiveSize());
    }

    int read(void* destination, int count) override {
        if (failed_ || count < 0 || (!destination && count)) {
            failed_ = true;
            return 0;
        }
        const auto size = static_cast<int>(std::min<std::int64_t>(count, length_ - position_));
        if (!size)
            return 0;
        auto* bytes = static_cast<ARA::ARAByte*>(destination);
        const bool ok = readHost_
            ? readHost_(controller_, handle_, ARA::ARASize(position_), ARA::ARASize(size), bytes) == ARA::kARATrue
            : readWrapper_(wrapper_, ARA::ARASize(position_), ARA::ARASize(size), bytes);
        if (!ok) {
            failed_ = true;
            return 0;
        }
        position_ += size;
        return size;
    }

    std::int64_t getPosition() const override { return position_; }
    std::int64_t getTotalLength() const override { return length_; }
    bool setPosition(std::int64_t value) override {
        if (value < 0 || value > length_) {
            failed_ = true;
            return false;
        }
        position_ = value;
        return true;
    }
    bool isExhausted() const override { return position_ == length_; }
    bool failed() const noexcept { return failed_; }

    std::int32_t readInt() { return readInteger<std::int32_t>(); }
    std::int64_t readInt64() { return readInteger<std::int64_t>(); }
    Text readString() {
        std::string text;
        char value;
        while (read(&value, 1) == 1) {
            if (!value)
                return Text::fromUTF8(text.data(), static_cast<std::ptrdiff_t>(text.size()));
            text += value;
        }
        failed_ = true;
        return {};
    }
private:
    void setLength(ARA::ARASize size) {
        if (size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            throw std::length_error("ARA archive addressing overflow");
        length_ = static_cast<std::int64_t>(size);
    }

    template<class T> T readInteger() {
        std::array<std::uint8_t, sizeof(T)> bytes{};
        if (read(bytes.data(), static_cast<int>(bytes.size())) != static_cast<int>(bytes.size())) {
            failed_ = true;
            return 0;
        }
        std::make_unsigned_t<T> result{};
        for (std::size_t i = 0; i < bytes.size(); ++i)
            result |= std::make_unsigned_t<T>(bytes[i]) << (8 * i);
        return std::bit_cast<T>(result);
    }

    decltype(ARA::ARAArchivingControllerInterface::readBytesFromArchive) readHost_{};
    ARA::ARAArchivingControllerHostRef controller_{};
    ARA::ARAArchiveReaderHostRef handle_{};
    void* wrapper_{};
    bool (*readWrapper_)(void*, ARA::ARASize, ARA::ARASize, ARA::ARAByte*){};
    std::int64_t length_{}, position_{};
    bool failed_{};
};

class ArchiveOutput final : public OutputStream {
public:
    ArchiveOutput(const ARA::ARAArchivingControllerInterface& api,
                  ARA::ARAArchivingControllerHostRef controller, ARA::ARAArchiveWriterHostRef handle)
        : writeHost_(api.structSize >= ARA::kARAArchivingControllerInterfaceMinSize ? api.writeBytesToArchive : nullptr), controller_(controller), handle_(handle) {
        if (!controller || !handle || api.structSize < ARA::kARAArchivingControllerInterfaceMinSize || !writeHost_)
            throw std::invalid_argument("Invalid ARA archive writer");
    }

    template<class Writer>
        requires requires(Writer& writer, ARA::ARASize size, const ARA::ARAByte* bytes) {
            writer.writeBytesToArchive(size, size, bytes);
        }
    explicit ArchiveOutput(Writer& writer)
        : wrapper_(&writer), writeWrapper_([](void* writer, ARA::ARASize at, ARA::ARASize size, const ARA::ARAByte* data) {
            return bool(static_cast<Writer*>(writer)->writeBytesToArchive(at, size, data));
        }) {}

    bool write(const void* source, std::size_t count) override {
        if (failed_ || (!source && count) ||
            count > std::numeric_limits<ARA::ARASize>::max() ||
            count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() - position_) ||
            static_cast<std::uint64_t>(position_) > std::numeric_limits<ARA::ARASize>::max() - count) {
            failed_ = true;
            return false;
        }
        if (!count)
            return true;
        const auto* bytes = static_cast<const ARA::ARAByte*>(source);
        const bool ok = writeHost_
            ? writeHost_(controller_, handle_, ARA::ARASize(position_), ARA::ARASize(count), bytes) == ARA::kARATrue
            : writeWrapper_(wrapper_, ARA::ARASize(position_), ARA::ARASize(count), bytes);
        if (ok)
            position_ += static_cast<std::int64_t>(count);
        failed_ = !ok;
        return ok;
    }
    void flush() override {}
    std::int64_t getPosition() const override { return position_; }
    bool setPosition(std::int64_t value) override {
        if (value < 0 || static_cast<std::uint64_t>(value) > std::numeric_limits<ARA::ARASize>::max()) {
            failed_ = true;
            return false;
        }
        position_ = value;
        return true;
    }
    bool failed() const noexcept { return failed_; }
private:
    decltype(ARA::ARAArchivingControllerInterface::writeBytesToArchive) writeHost_{};
    ARA::ARAArchivingControllerHostRef controller_{};
    ARA::ARAArchiveWriterHostRef handle_{};
    void* wrapper_{};
    bool (*writeWrapper_)(void*, ARA::ARASize, ARA::ARASize, const ARA::ARAByte*){};
    std::int64_t position_{};
    bool failed_{};
};

} // namespace cell::ara
