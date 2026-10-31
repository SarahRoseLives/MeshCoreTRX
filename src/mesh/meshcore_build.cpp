#include "meshcore_build.h"

#include "crypto.h"

#include <algorithm>
#include <cstring>

namespace mctrx {

const uint8_t kPublicChannelKey[16] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72};

namespace {

constexpr uint8_t PH_ROUTE_FLOOD = 1;
constexpr uint8_t PH_TYPE_GRP_TXT = 5;
constexpr uint8_t PH_TYPE_ADVERT = 4;

void putLe32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

uint8_t headerByte(uint8_t type, uint8_t route)
{
    return static_cast<uint8_t>((type << 2) | (route & 0x03));
}

} // namespace

std::vector<uint8_t> buildPlainRaw(uint8_t header, const std::vector<uint8_t>& payload,
                                   const std::vector<uint8_t>& path)
{
    std::vector<uint8_t> raw;
    raw.reserve(2 + path.size() + payload.size());
    raw.push_back(header);
    raw.push_back(static_cast<uint8_t>(path.size() & 0x3F)); // 1-byte hashes
    raw.insert(raw.end(), path.begin(), path.end());
    raw.insert(raw.end(), payload.begin(), payload.end());
    return raw;
}

std::vector<uint8_t> buildGroupText(const std::vector<uint8_t>& channelKey,
                                    const std::string& sender,
                                    const std::string& text,
                                    uint32_t timestamp,
                                    uint8_t routeType)
{
    if (channelKey.size() != 16 && channelKey.size() != 32)
        return {};

    uint8_t secret[32] = {};
    std::memcpy(secret, channelKey.data(), channelKey.size());
    const auto hash = mc::sha256(channelKey.data(), channelKey.size());

    std::vector<uint8_t> plain;
    plain.reserve(5 + sender.size() + 2 + text.size());
    putLe32(plain, timestamp);
    plain.push_back(0); // TXT_TYPE_PLAIN
    plain.insert(plain.end(), sender.begin(), sender.end());
    plain.push_back(':');
    plain.push_back(' ');
    plain.insert(plain.end(), text.begin(), text.end());

    while (plain.size() % 16 != 0)
        plain.push_back(0);

    mc::Aes128 aes;
    aes.setKey(secret);
    std::vector<uint8_t> ct(plain.size());
    for (size_t off = 0; off < plain.size(); off += 16)
        aes.encryptBlock(&plain[off], &ct[off]);

    uint8_t hmac[32];
    mc::hmacSha256(secret, sizeof(secret), ct.data(), ct.size(), hmac);

    std::vector<uint8_t> payload;
    payload.reserve(3 + ct.size());
    payload.push_back(hash[0]);
    payload.push_back(hmac[0]);
    payload.push_back(hmac[1]);
    payload.insert(payload.end(), ct.begin(), ct.end());

    return buildPlainRaw(headerByte(PH_TYPE_GRP_TXT, routeType), payload);
}

std::vector<uint8_t> buildAdvert(const Identity& id,
                                 const std::string& name,
                                 bool hasLocation,
                                 double lat,
                                 double lon,
                                 uint32_t timestamp,
                                 uint8_t routeType)
{
    if (!id.valid) return {};

    std::vector<uint8_t> appdata;
    uint8_t flags = 0x01; // ADV_TYPE_CHAT
    if (hasLocation) flags |= 0x10;
    if (!name.empty()) flags |= 0x80;
    appdata.push_back(flags);
    if (hasLocation) {
        const int32_t ilat = static_cast<int32_t>(lat * 1e6);
        const int32_t ilon = static_cast<int32_t>(lon * 1e6);
        putLe32(appdata, static_cast<uint32_t>(ilat));
        putLe32(appdata, static_cast<uint32_t>(ilon));
    }
    appdata.insert(appdata.end(), name.begin(), name.end());

    std::vector<uint8_t> payload;
    payload.reserve(32 + 4 + 64 + appdata.size());
    payload.insert(payload.end(), id.pub.begin(), id.pub.end());
    putLe32(payload, timestamp);
    const size_t sigOff = payload.size();
    payload.resize(payload.size() + 64, 0);
    payload.insert(payload.end(), appdata.begin(), appdata.end());

    std::vector<uint8_t> signedMsg;
    signedMsg.reserve(32 + 4 + appdata.size());
    signedMsg.insert(signedMsg.end(), id.pub.begin(), id.pub.end());
    putLe32(signedMsg, timestamp);
    signedMsg.insert(signedMsg.end(), appdata.begin(), appdata.end());

    const auto sig = id.sign(signedMsg);
    std::memcpy(&payload[sigOff], sig.data(), sig.size());

    return buildPlainRaw(headerByte(PH_TYPE_ADVERT, routeType), payload);
}

std::vector<uint8_t> hexToBytes(const std::string& hex)
{
    std::vector<uint8_t> out;
    if (hex.size() % 2) return out;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = val(hex[i]);
        const int lo = val(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string bytesToHex(const uint8_t* p, size_t n)
{
    static const char* h = "0123456789abcdef";
    std::string s;
    s.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = h[p[i] >> 4];
        s[2 * i + 1] = h[p[i] & 0x0F];
    }
    return s;
}

std::string bytesToHex(const std::vector<uint8_t>& v)
{
    return bytesToHex(v.data(), v.size());
}

} // namespace mctrx
