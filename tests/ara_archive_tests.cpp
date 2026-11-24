// Original Cell Framework implementation; see LICENSE.
#include <cell/ara/archive_stream.hpp>
#include <cstring>
#include <iostream>

namespace {
using namespace ARA;

struct Archive {
    std::array<ARAByte, 32> bytes{};
    ARASize length{};
    unsigned reads{}, writes{};
    bool fail{};
    static Archive& get(ARAArchivingControllerHostRef ref) {
        return *reinterpret_cast<Archive*>(ref);
    }
    static ARASize ARA_CALL size(ARAArchivingControllerHostRef ref, ARAArchiveReaderHostRef) {
        return get(ref).length;
    }
    static ARABool ARA_CALL read(ARAArchivingControllerHostRef ref, ARAArchiveReaderHostRef,
                                 ARASize at, ARASize length, ARAByte* data) {
        auto& archive = get(ref);
        ++archive.reads;
        if (archive.fail || at > archive.length || length > archive.length - at)
            return kARAFalse;
        std::memcpy(data, archive.bytes.data() + at, length);
        return kARATrue;
    }
    static ARABool ARA_CALL write(ARAArchivingControllerHostRef ref, ARAArchiveWriterHostRef,
                                  ARASize at, ARASize length, const ARAByte* data) {
        auto& archive = get(ref);
        ++archive.writes;
        if (archive.fail || at > archive.bytes.size() || length > archive.bytes.size() - at)
            return kARAFalse;
        std::memcpy(archive.bytes.data() + at, data, length);
        archive.length = std::max(archive.length, at + length);
        return kARATrue;
    }
};

void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void run() {
    Archive archive;
    ARAArchivingControllerInterface api{};
    api.structSize = sizeof(api);
    api.getArchiveSize = Archive::size;
    api.readBytesFromArchive = Archive::read;
    api.writeBytesToArchive = Archive::write;
    const auto controller = reinterpret_cast<ARAArchivingControllerHostRef>(&archive);
    const auto reader = reinterpret_cast<ARAArchiveReaderHostRef>(&archive);
    const auto writer = reinterpret_cast<ARAArchiveWriterHostRef>(&archive);

    cell::ara::ArchiveOutput output(api, controller, writer);
    check(output.writeInt(0) && output.writeInt64(-0x1020304050607LL) && output.writeString("Cell"),
          "Archive payload write failed");
    check(output.setPosition(0) && output.writeInt(13), "Archive header rewrite failed");
    cell::ara::ArchiveInput input(api, controller, reader);
    check(input.readInt() == 13 && input.readInt64() == -0x1020304050607LL &&
          input.readString() == "Cell" && input.isExhausted() && !input.failed(),
          "Archive roundtrip failed");
    check(input.readInt() == 0 && input.failed(), "Truncation was ignored");

    cell::ara::ArchiveInput failing(api, controller, reader);
    archive.fail = true;
    check(failing.readInt() == 0 && failing.failed() && failing.getPosition() == 0,
          "Failed host read advanced its cursor");
    const auto count = archive.reads;
    archive.fail = false;
    check(failing.readInt() == 0 && archive.reads == count, "Failed reader resumed host access");

    archive.fail = true;
    check(!output.writeInt(99) && output.failed() && output.getPosition() == 4,
          "Failed host write advanced its cursor");
    archive.fail = false;
    check(!output.writeInt(99), "Failed archive writer resumed host access");

    api.structSize = sizeof(ARASize);
    try {
        cell::ara::ArchiveInput invalid(api, controller, reader);
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Accepted an undersized ARA function table");
}
} // namespace

int main() {
    try {
        run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
