// Original Cell Framework implementation; see LICENSE.
#include <cell/network_audio_session.hpp>
#include <cell/uuid.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace cell::network_audio {
namespace {
using namespace std::chrono_literals;
using Time = SessionClock::time_point;
using Nonce = std::array<std::byte, 16>;
using Bytes = std::span<const std::byte>;
constexpr auto retry = 200ms, sessionTimeout = 1200ms, challengeTimeout = 2400ms;
constexpr std::uint32_t magic = 0x32534143; // "CAS2"
constexpr std::size_t prefixSize = 5, tagSize = 32, aeadTagSize = 16;
constexpr std::size_t audioHeaderSize = prefixSize + 16 + 8;
static_assert(audioHeaderSize + aeadTagSize == AuthenticatedSender::overhead);
enum class Kind : std::uint8_t { hello = 1, challenge, audio, acknowledgment, audioKey, acknowledgmentKey };
using Cipher = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
Cipher cipher() {
    Cipher context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context) throw std::bad_alloc();
    return context;
}

const unsigned char* octets(Bytes bytes) noexcept {
    return reinterpret_cast<const unsigned char*>(bytes.data());
}

std::array<unsigned char, 12> audioNonce(std::uint64_t sequence) noexcept {
    // Each handshake derives a new audio-only key. A sequence is used once per key.
    std::array<unsigned char, 12> result{};
    for (std::size_t i = 0; i < 8; ++i)
        result[4 + i] = static_cast<unsigned char>(sequence >> (i * 8));
    return result;
}

void seal(EVP_CIPHER_CTX* context, Datagram& packet, Bytes pcm, const PairingKey& key, std::uint64_t sequence) {
    packet.resize(audioHeaderSize + pcm.size() + aeadTagSize);
    auto* bytes = reinterpret_cast<unsigned char*>(packet.data());
    const auto nonce = audioNonce(sequence);
    int ignored{}, written{}, tail{};
    if (EVP_EncryptInit_ex(context, EVP_chacha20_poly1305(), nullptr, octets(key), nonce.data()) != 1 ||
        EVP_EncryptUpdate(context, nullptr, &ignored, bytes, audioHeaderSize) != 1 ||
        EVP_EncryptUpdate(context, bytes + audioHeaderSize, &written, octets(pcm), static_cast<int>(pcm.size())) != 1 ||
        EVP_EncryptFinal_ex(context, bytes + audioHeaderSize + written, &tail) != 1 ||
        written + tail != static_cast<int>(pcm.size()) ||
        EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_GET_TAG, aeadTagSize, bytes + audioHeaderSize + pcm.size()) != 1)
        throw std::runtime_error("Network audio encryption failed");
}

bool open(EVP_CIPHER_CTX* context, Bytes packet, std::span<std::byte> pcm, const PairingKey& key,
          std::uint64_t sequence) noexcept {
    auto* output = reinterpret_cast<unsigned char*>(pcm.data());
    const auto nonce = audioNonce(sequence);
    std::array<std::byte, aeadTagSize> digest{};
    std::ranges::copy(packet.last(aeadTagSize), digest.begin());
    int ignored{}, written{}, tail{};
    return EVP_DecryptInit_ex(context, EVP_chacha20_poly1305(), nullptr, octets(key), nonce.data()) == 1 &&
           EVP_DecryptUpdate(context, nullptr, &ignored, octets(packet), audioHeaderSize) == 1 &&
           EVP_DecryptUpdate(context, output, &written, octets(packet.subspan(audioHeaderSize)), static_cast<int>(pcm.size())) == 1 &&
           EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_AEAD_SET_TAG, aeadTagSize, digest.data()) == 1 &&
           EVP_DecryptFinal_ex(context, output + written, &tail) == 1 &&
           written + tail == static_cast<int>(pcm.size());
}

PairingKey tag(const PairingKey& key, Bytes data) {
    PairingKey digest;
    unsigned length{};
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(data.data()), data.size(),
              reinterpret_cast<unsigned char*>(digest.data()), &length) || length != digest.size())
        throw std::runtime_error("Network audio HMAC failed");
    return digest;
}

bool match(Bytes a, Bytes b) noexcept {
    return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

ByteWriter message(Kind kind) {
    ByteWriter writer;
    writer.u32(magic);
    writer.bytes.push_back(std::byte(kind));
    return writer;
}

void append(ByteWriter& writer, Bytes bytes) {
    writer.bytes.insert(writer.bytes.end(), bytes.begin(), bytes.end());
}

Datagram sign(ByteWriter writer, const PairingKey& key) {
    const auto digest = tag(key, writer.bytes);
    append(writer, digest);
    return std::move(writer.bytes);
}

std::optional<Kind> kindOf(Bytes packet) noexcept {
    ByteReader reader(packet);
    std::uint32_t type{};
    if (packet.size() < prefixSize + tagSize || packet.size() > maxPacketBytes ||
        !reader.u32(type) || type != magic) return {};
    return static_cast<Kind>(packet[4]);
}

bool authentic(Bytes packet, const PairingKey& key) {
    return match(packet.last(tagSize), tag(key, packet.first(packet.size() - tagSize)));
}

Bytes body(Bytes packet) noexcept { return packet.subspan(prefixSize, packet.size() - prefixSize - tagSize); }

Nonce nonce() {
    Nonce result;
    randomBytes(result);
    return result;
}

PairingKey trafficKey(Kind kind, const PairingKey& key, const Nonce& client, const Nonce& server) {
    auto derivation = message(kind);
    append(derivation, client);
    append(derivation, server);
    return tag(key, derivation.bytes);
}

void validate(SessionFormat format, const PairingKey& key) {
    if (format.sampleRate < 8000 || format.sampleRate > 384000 || !format.channels || format.channels > 32)
        throw std::invalid_argument("Invalid network audio format");
    if (std::ranges::all_of(key, [](std::byte byte) { return byte == std::byte{}; }))
        throw std::invalid_argument("The network audio pairing key must be randomly generated");
}

bool admit(std::uint64_t seq, std::uint64_t& highest, std::uint64_t& seen) noexcept {
    if (seq > highest) {
        const auto shift = seq - highest;
        seen = shift < 64 ? (seen << shift) | 1u : 1u;
        highest = seq;
        return true;
    }
    const auto offset = highest - seq;
    if (offset >= 64 || (seen & (std::uint64_t{1} << offset))) return false;
    seen |= std::uint64_t{1} << offset;
    return true;
}
}

PairingKey generatePairingKey() {
    PairingKey key;
    randomBytes(key);
    return key;
}

struct AuthenticatedSender::State {
    PairingKey key, audio{}, ack{};
    Cipher context = cipher();
    SessionFormat format;
    Nonce client = nonce(), server{};
    std::optional<Time> lastHello, lastContact;
    std::uint64_t sequence{}, acknowledgment{};
    bool ready{}, acknowledged{};
    ~State() {
        OPENSSL_cleanse(key.data(), key.size());
        OPENSSL_cleanse(audio.data(), audio.size());
        OPENSSL_cleanse(ack.data(), ack.size());
    }
};

AuthenticatedSender::AuthenticatedSender(const PairingKey& key, SessionFormat format) {
    validate(format, key);
    state_.reset(new State{.key = key, .format = format});
}
AuthenticatedSender::~AuthenticatedSender() = default;

Datagram AuthenticatedSender::poll(Time now) {
    auto& state = *state_;
    if (state.lastContact && now - *state.lastContact >= sessionTimeout) {
        state.client = nonce();
        state.lastContact.reset();
        state.lastHello.reset();
        state.sequence = state.acknowledgment = 0;
        state.ready = state.acknowledged = false;
    }
    if (state.ready || (state.lastHello && now - *state.lastHello < retry)) return {};
    state.lastHello = now;
    auto hello = message(Kind::hello);
    append(hello, state.client);
    hello.u32(state.format.sampleRate);
    hello.u32(state.format.channels);
    return sign(std::move(hello), state.key);
}

void AuthenticatedSender::receive(Bytes packet, Time now) {
    auto& state = *state_;
    const auto kind = kindOf(packet);
    if (kind == Kind::challenge && !state.ready && state.lastHello && body(packet).size() == 32 &&
        match(body(packet).first(16), state.client) && authentic(packet, state.key)) {
        std::ranges::copy(body(packet).last(16), state.server.begin());
        state.audio = trafficKey(Kind::audioKey, state.key, state.client, state.server);
        state.ack = trafficKey(Kind::acknowledgmentKey, state.key, state.client, state.server);
        state.ready = true;
        state.lastContact = now;
    } else if (kind == Kind::acknowledgment && state.ready && body(packet).size() == 24 &&
               match(body(packet).first(16), state.server) && authentic(packet, state.ack)) {
        ByteReader reader(body(packet).last(8));
        std::uint64_t sequence{};
        if (reader.u64(sequence) && sequence >= state.acknowledgment && sequence < state.sequence &&
            (!state.acknowledged || sequence > state.acknowledgment)) {
            state.acknowledged = true;
            state.acknowledgment = sequence;
            state.lastContact = now;
        }
    }
}

bool AuthenticatedSender::canSend() const noexcept { return state_->ready; }
bool AuthenticatedSender::confirmed() const noexcept { return state_->acknowledged; }

Datagram AuthenticatedSender::pack(Bytes pcm) {
    auto& state = *state_;
    if (!state.ready || pcm.empty() || pcm.size() > payloadLimit) return {};
    if (state.sequence == std::numeric_limits<std::uint64_t>::max()) return {};
    auto packet = message(Kind::audio);
    append(packet, state.server);
    const auto sequence = state.sequence++;
    packet.u64(sequence);
    seal(state.context.get(), packet.bytes, pcm, state.audio, sequence);
    return std::move(packet.bytes);
}

struct AuthenticatedReceiver::State {
    struct Route {
        Nonce client, server;
        PairingKey audio, ack;
        std::uint64_t peer;
        Time created;
        std::optional<Time> acknowledged;
        std::uint64_t highest{}, seen{};
        ~Route() {
            OPENSSL_cleanse(audio.data(), audio.size());
            OPENSSL_cleanse(ack.data(), ack.size());
        }
    };
    PairingKey key;
    SessionFormat format;
    Cipher context = cipher();
    std::array<std::byte, AuthenticatedSender::payloadLimit + EVP_MAX_BLOCK_LENGTH> plaintext{};
    std::optional<Route> challenge, established;
    ~State() { OPENSSL_cleanse(key.data(), key.size()); }
};

AuthenticatedReceiver::AuthenticatedReceiver(const PairingKey& key, SessionFormat format) {
    validate(format, key);
    state_.reset(new State{key, format});
}
AuthenticatedReceiver::~AuthenticatedReceiver() = default;
void AuthenticatedReceiver::retire() noexcept { state_->established.reset(); }

AuthenticatedPacket AuthenticatedReceiver::receive(Bytes packet, std::uint64_t peer, Time now, bool allowNewSession) {
    auto& state = *state_;
    if (state.challenge && now - state.challenge->created >= challengeTimeout) state.challenge.reset();
    const auto kind = kindOf(packet);
    if (kind == Kind::hello && body(packet).size() == 24 && authentic(packet, state.key)) {
        const auto client = body(packet).first(16);
        ByteReader reader(body(packet).last(8));
        std::uint32_t rate{}, channels{};
        if (!reader.u32(rate) || !reader.u32(channels) || rate != state.format.sampleRate ||
            channels != state.format.channels) return {};
        if (!state.challenge || state.challenge->peer != peer || !match(client, state.challenge->client)) {
            Nonce clientNonce;
            std::ranges::copy(client, clientNonce.begin());
            const auto serverNonce = nonce();
            state.challenge = State::Route{clientNonce, serverNonce,
                trafficKey(Kind::audioKey, state.key, clientNonce, serverNonce),
                trafficKey(Kind::acknowledgmentKey, state.key, clientNonce, serverNonce), peer, now};
        }
        auto response = message(Kind::challenge);
        append(response, state.challenge->client);
        append(response, state.challenge->server);
        return {{}, sign(std::move(response), state.key)};
    }
    if (kind != Kind::audio || packet.size() <= AuthenticatedSender::overhead) return {};
    const auto session = packet.subspan(prefixSize, 16);
    ByteReader reader(packet.subspan(prefixSize + 16, 8));
    std::uint64_t sequence{};
    if (!reader.u64(sequence)) return {};
    const auto matches = [peer, session](const std::optional<State::Route>& route) {
        return route && route->peer == peer && match(session, route->server);
    };
    const bool continuing = matches(state.established);
    if (!continuing && (!allowNewSession || !matches(state.challenge))) return {};
    auto& candidate = continuing ? *state.established : *state.challenge;
    auto highest = candidate.highest, seen = candidate.seen;
    if (!admit(sequence, highest, seen)) return {};
    auto plaintext = std::span(state.plaintext).first(packet.size() - AuthenticatedSender::overhead);
    if (!open(state.context.get(), packet, plaintext, candidate.audio, sequence)) return {};
    candidate.highest = highest;
    candidate.seen = seen;
    if (!continuing) {
        state.established = state.challenge;
        state.challenge.reset();
    }
    auto& route = *state.established;
    AuthenticatedPacket result{plaintext, {}};
    if (!route.acknowledged || now - *route.acknowledged >= retry) {
        auto response = message(Kind::acknowledgment);
        append(response, route.server);
        response.u64(route.highest);
        result.reply = sign(std::move(response), route.ack);
        route.acknowledged = now;
    }
    return result;
}
} // namespace cell::network_audio
