#include "engine.h"

#include "mesh/crypto.h"
#include "mesh/meshcore.h"
#include "mesh/meshcore_build.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <random>
#include <filesystem>
#include <fstream>

namespace mctrx {
namespace {

constexpr uint8_t ROUTE_FLOOD = 1;
constexpr uint8_t ROUTE_DIRECT = 2;
constexpr uint8_t TYPE_REQ = 0;
constexpr uint8_t TYPE_RESPONSE = 1;
constexpr uint8_t TYPE_TXT = 2;
constexpr uint8_t TYPE_ACK = 3;
constexpr uint8_t TYPE_ADVERT = 4;
constexpr uint8_t TYPE_GRP_TXT = 5;
constexpr uint8_t TYPE_GRP_DATA = 6;
constexpr uint8_t TYPE_ANON_REQ = 7;
constexpr uint8_t TYPE_PATH = 8;
constexpr uint8_t TYPE_CONTROL = 0x0B;

double nowSec()
{
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

uint64_t fnv1a(const std::vector<uint8_t>& v)
{
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : v) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    return h;
}

uint8_t headerByte(uint8_t type, uint8_t route)
{
    return static_cast<uint8_t>((type << 2) | (route & 3));
}

void putLe32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

uint32_t getLe32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::filesystem::path dataDir()
{
    const char* la = std::getenv("LOCALAPPDATA");
    std::filesystem::path dir = la ? std::filesystem::path(la) / "MeshCoreTRX"
                                   : std::filesystem::path(".");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void appendEvent(const std::string& line)
{
    std::ofstream f(dataDir() / "events.log", std::ios::app);
    if (f) f << line << "\n";
}

template <typename T>
void putPod(std::vector<uint8_t>& b, const T& v)
{
    const size_t off = b.size();
    b.resize(off + sizeof(T));
    std::memcpy(&b[off], &v, sizeof(T));
}

template <typename T>
bool getPod(const uint8_t*& p, const uint8_t* end, T& v)
{
    if (p + sizeof(T) > end) return false;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return true;
}

void putStr(std::vector<uint8_t>& b, const std::string& s)
{
    putPod<uint32_t>(b, static_cast<uint32_t>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

bool getStr(const uint8_t*& p, const uint8_t* end, std::string& s)
{
    uint32_t n = 0;
    if (!getPod(p, end, n)) return false;
    if (n > (1u << 20) || p + n > end) return false;
    s.assign(reinterpret_cast<const char*>(p), n);
    p += n;
    return true;
}

bool writeAtomic(const std::filesystem::path& path, const std::vector<uint8_t>& data)
{
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

std::vector<uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0 || n > (64 << 20)) return {};
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(data.data()), n);
    if (!f) return {};
    return data;
}

constexpr uint32_t kMagic = 0x4D435458; // MCTX
constexpr uint32_t kVersion = 1;

std::string trimZero(const std::vector<uint8_t>& v, size_t off)
{
    std::string s;
    for (size_t i = off; i < v.size(); ++i) {
        if (v[i] == 0) break;
        s.push_back(static_cast<char>(v[i]));
    }
    return s;
}

bool decryptDirect(const uint8_t secret[32], const uint8_t* src, size_t len,
                   std::vector<uint8_t>& out)
{
    out.clear();
    if (len <= 2) return false;
    const size_t ctLen = len - 2;
    if (ctLen == 0 || ctLen % 16 != 0) return false;
    uint8_t hmac[32];
    mc::hmacSha256(secret, 32, src + 2, ctLen, hmac);
    if (hmac[0] != src[0] || hmac[1] != src[1]) return false;
    mc::Aes128 aes;
    aes.setKey(secret);
    out.resize(ctLen);
    for (size_t off = 0; off < ctLen; off += 16)
        aes.decryptBlock(src + 2 + off, &out[off]);
    while (!out.empty() && out.back() == 0) out.pop_back();
    return true;
}

std::vector<uint8_t> encryptDirect(const uint8_t secret[32], const uint8_t* data, size_t len)
{
    std::vector<uint8_t> plain(data, data + len);
    while (plain.size() % 16 != 0) plain.push_back(0);
    mc::Aes128 aes;
    aes.setKey(secret);
    std::vector<uint8_t> ct(plain.size());
    for (size_t off = 0; off < plain.size(); off += 16)
        aes.encryptBlock(&plain[off], &ct[off]);
    uint8_t hmac[32];
    mc::hmacSha256(secret, 32, ct.data(), ct.size(), hmac);
    std::vector<uint8_t> out;
    out.reserve(2 + ct.size());
    out.push_back(hmac[0]);
    out.push_back(hmac[1]);
    out.insert(out.end(), ct.begin(), ct.end());
    return out;
}

} // namespace

void Engine::init(const Identity& id, const std::string& name, bool hasLocation,
                  double lat, double lon, Role role)
{
    std::lock_guard<std::mutex> lk(mu_);
    id_ = id;
    name_ = name;
    hasLoc_ = hasLocation;
    lat_ = lat;
    lon_ = lon;
    role_ = role;
    channels_.clear();
    contacts_.clear();
    messages_.clear();
    packets_.clear();
    ChannelInfo pub;
    pub.name = "Public";
    pub.key.assign(kPublicChannelKey, kPublicChannelKey + 16);
    pub.hash = mc::sha256(pub.key.data(), pub.key.size())[0];
    pub.enabled = true;
    channels_.push_back(pub);
    logs_.clear();
    if (persistence_) loadPersistent();
    log("engine ready: " + id.pubHex().substr(0, 12) + "...");
}

void Engine::loadPersistent()
{
    // Channels
    {
        const auto data = readFile(dataDir() / "channels.bin");
        if (data.size() >= 12) {
            const uint8_t* p = data.data();
            const uint8_t* end = data.data() + data.size();
            uint32_t magic = 0, ver = 0, count = 0;
            if (getPod(p, end, magic) && getPod(p, end, ver) && getPod(p, end, count) &&
                magic == kMagic && ver == kVersion && count < 10000) {
                for (uint32_t i = 0; i < count; ++i) {
                    std::string name, key;
                    uint8_t hash = 0, enabled = 1;
                    if (!getStr(p, end, name) || !getStr(p, end, key) ||
                        !getPod(p, end, hash) || !getPod(p, end, enabled))
                        break;
                    if (key.size() != 16 && key.size() != 32) continue;
                    bool dup = false;
                    for (const auto& c : channels_)
                        if (c.key == std::vector<uint8_t>(key.begin(), key.end())) dup = true;
                    if (dup) continue;
                    ChannelInfo c;
                    c.name = name.empty() ? bytesToHex(reinterpret_cast<const uint8_t*>(key.data()),
                                                       std::min<size_t>(4, key.size())) + "..."
                                          : name;
                    c.key.assign(key.begin(), key.end());
                    c.hash = mc::sha256(c.key.data(), c.key.size())[0];
                    c.enabled = enabled != 0;
                    channels_.push_back(c);
                }
            }
        }
    }

    // Contacts
    {
        const auto data = readFile(dataDir() / "contacts.bin");
        if (data.size() >= 12) {
            const uint8_t* p = data.data();
            const uint8_t* end = data.data() + data.size();
            uint32_t magic = 0, ver = 0, count = 0;
            if (getPod(p, end, magic) && getPod(p, end, ver) && getPod(p, end, count) &&
                magic == kMagic && ver == kVersion && count < 10000) {
                for (uint32_t i = 0; i < count; ++i) {
                    std::array<uint8_t, 32> pub{};
                    uint8_t type = 0, hasLoc = 0;
                    double lat = 0, lon = 0, lastSeen = 0;
                    uint32_t lastAdvertTs = 0;
                    std::string name, path;
                    if (p + 32 > end) break;
                    std::memcpy(pub.data(), p, 32); p += 32;
                    if (!getPod(p, end, type) || !getPod(p, end, hasLoc) ||
                        !getPod(p, end, lat) || !getPod(p, end, lon) ||
                        !getPod(p, end, lastSeen) || !getPod(p, end, lastAdvertTs) ||
                        !getStr(p, end, name) || !getStr(p, end, path))
                        break;
                    if (std::memcmp(pub.data(), id_.pub.data(), 32) == 0) continue;
                    ContactInfo* c = nullptr;
                    for (auto& x : contacts_)
                        if (std::memcmp(x.pub.data(), pub.data(), 32) == 0) c = &x;
                    if (!c) {
                        contacts_.push_back(ContactInfo{});
                        c = &contacts_.back();
                    }
                    c->pub = pub;
                    c->name = name.empty() ? bytesToHex(pub.data(), 4) + "..." : name;
                    c->type = type;
                    c->hasLocation = hasLoc != 0;
                    c->lat = lat;
                    c->lon = lon;
                    c->lastSeen = lastSeen;
                    c->lastAdvertTs = lastAdvertTs;
                    c->outPath.assign(path.begin(), path.end());
                    c->secretValid = false;
                }
            }
        }
    }

    // Messages
    {
        const auto data = readFile(dataDir() / "messages.bin");
        if (data.size() >= 12) {
            const uint8_t* p = data.data();
            const uint8_t* end = data.data() + data.size();
            uint32_t magic = 0, ver = 0, count = 0;
            if (getPod(p, end, magic) && getPod(p, end, ver) && getPod(p, end, count) &&
                magic == kMagic && ver == kVersion && count < 50000) {
                for (uint32_t i = 0; i < count; ++i) {
                    uint8_t outgoing = 0, direct = 0, acked = 0, chHash = 0;
                    uint32_t ts = 0;
                    double seenAt = 0;
                    int32_t hops = 0;
                    float snr = 0;
                    std::string channel, peer, sender, text;
                    if (!getPod(p, end, outgoing) || !getPod(p, end, direct) ||
                        !getPod(p, end, acked) || !getPod(p, end, chHash) ||
                        !getPod(p, end, ts) || !getPod(p, end, seenAt) ||
                        !getPod(p, end, hops) || !getPod(p, end, snr) ||
                        !getStr(p, end, channel) || !getStr(p, end, peer) ||
                        !getStr(p, end, sender) || !getStr(p, end, text))
                        break;
                    ChatMessage m;
                    m.outgoing = outgoing != 0;
                    m.direct = direct != 0;
                    m.acked = acked != 0;
                    m.channelHash = chHash;
                    m.timestamp = ts;
                    m.seenAt = seenAt;
                    m.hops = hops;
                    m.snr = snr;
                    m.channel = channel;
                    m.peerPubHex = peer;
                    m.sender = sender;
                    m.text = text;
                    if (m.direct && m.peerPubHex.size() == 64) {
                        const auto pub = hexToBytes(m.peerPubHex);
                        if (pub.size() == 32) {
                            bool found = false;
                            for (const auto& c : contacts_)
                                if (std::memcmp(c.pub.data(), pub.data(), 32) == 0) found = true;
                            if (!found) {
                                ContactInfo c;
                                std::memcpy(c.pub.data(), pub.data(), 32);
                                c.name = m.sender.empty() ? bytesToHex(pub.data(), 4) + "..." : m.sender;
                                c.lastSeen = m.seenAt;
                                c.secretValid = false;
                                contacts_.push_back(c);
                            }
                        }
                    }
                    messages_.push_back(std::move(m));
                }
                while (messages_.size() > 2000) messages_.erase(messages_.begin());
            }
        }
    }
    if (!contacts_.empty() || !messages_.empty() || channels_.size() > 1)
        log("persistent state: " + std::to_string(contacts_.size()) + " contacts, " +
            std::to_string(messages_.size()) + " messages, " +
            std::to_string(channels_.size()) + " channels");
    dirty_ = false;
}

void Engine::savePersistent()
{
    {
        std::vector<uint8_t> b;
        putPod<uint32_t>(b, kMagic);
        putPod<uint32_t>(b, kVersion);
        putPod<uint32_t>(b, static_cast<uint32_t>(channels_.size()));
        for (const auto& c : channels_) {
            putStr(b, c.name);
            putStr(b, std::string(reinterpret_cast<const char*>(c.key.data()), c.key.size()));
            putPod<uint8_t>(b, c.hash);
            putPod<uint8_t>(b, c.enabled ? 1 : 0);
        }
        writeAtomic(dataDir() / "channels.bin", b);
    }
    {
        std::vector<uint8_t> b;
        putPod<uint32_t>(b, kMagic);
        putPod<uint32_t>(b, kVersion);
        putPod<uint32_t>(b, static_cast<uint32_t>(contacts_.size()));
        for (const auto& c : contacts_) {
            b.insert(b.end(), c.pub.begin(), c.pub.end());
            putPod<uint8_t>(b, static_cast<uint8_t>(c.type));
            putPod<uint8_t>(b, c.hasLocation ? 1 : 0);
            putPod<double>(b, c.lat);
            putPod<double>(b, c.lon);
            putPod<double>(b, c.lastSeen);
            putPod<uint32_t>(b, c.lastAdvertTs);
            putStr(b, c.name);
            std::string pathStr(c.outPath.begin(), c.outPath.end());
            putStr(b, pathStr);
        }
        writeAtomic(dataDir() / "contacts.bin", b);
    }
    {
        std::vector<uint8_t> b;
        putPod<uint32_t>(b, kMagic);
        putPod<uint32_t>(b, kVersion);
        putPod<uint32_t>(b, static_cast<uint32_t>(messages_.size()));
        for (const auto& m : messages_) {
            putPod<uint8_t>(b, m.outgoing ? 1 : 0);
            putPod<uint8_t>(b, m.direct ? 1 : 0);
            putPod<uint8_t>(b, m.acked ? 1 : 0);
            putPod<uint8_t>(b, m.channelHash);
            putPod<uint32_t>(b, m.timestamp);
            putPod<double>(b, m.seenAt);
            putPod<int32_t>(b, m.hops);
            putPod<float>(b, m.snr);
            putStr(b, m.channel);
            putStr(b, m.peerPubHex);
            putStr(b, m.sender);
            putStr(b, m.text);
        }
        writeAtomic(dataDir() / "messages.bin", b);
    }
    dirty_ = false;
}

Engine::~Engine()
{
    try {
        save();
    } catch (...) {
    }
}

void Engine::saveIfDirty()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (persistence_ && dirty_) savePersistent();
}

void Engine::save()
{
    std::lock_guard<std::mutex> lk(mu_);
    if (persistence_) savePersistent();
}

void Engine::setPublicChannelEnabled(bool en)
{
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& c : channels_)
        if (c.name == "Public") c.enabled = en;
    markDirty();
}

bool Engine::addChannel(const std::string& name, const std::vector<uint8_t>& key)
{
    if (key.size() != 16 && key.size() != 32) return false;
    std::lock_guard<std::mutex> lk(mu_);
    const uint8_t h = mc::sha256(key.data(), key.size())[0];
    for (const auto& c : channels_) {
        if (c.hash == h && c.key == key) return false;
    }
    ChannelInfo c;
    c.name = name.empty() ? bytesToHex(key.data(), 4) + "..." : name;
    c.key = key;
    c.hash = h;
    c.enabled = true;
    channels_.push_back(c);
    log("channel added: " + c.name);
    markDirty();
    return true;
}

void Engine::setChannelEnabled(int index, bool en)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (index >= 0 && index < static_cast<int>(channels_.size())) {
        channels_[index].enabled = en;
        markDirty();
    }
}

void Engine::log(const std::string& line)
{
    logs_.push_back(line);
    if (logs_.size() > 1000) logs_.erase(logs_.begin());
    appendEvent(line);
}

uint32_t Engine::nextTimestamp()
{
    uint32_t t = static_cast<uint32_t>(std::time(nullptr));
    if (t <= lastTs_) t = ++lastTs_;
    else lastTs_ = t;
    return t;
}

uint64_t Engine::packetKey(uint8_t type, uint16_t pathLenByte,
                           const std::vector<uint8_t>& payload) const
{
    mc::Sha256 sha;
    sha.update(&type, 1);
    if (type == 9) { // TRACE includes the path length in MeshCore's packet hash
        const uint8_t pl[2] = {static_cast<uint8_t>(pathLenByte & 0xFF),
                               static_cast<uint8_t>((pathLenByte >> 8) & 0xFF)};
        sha.update(pl, 2);
    }
    if (!payload.empty()) sha.update(payload.data(), payload.size());
    uint8_t full[32];
    sha.finalize(full);
    uint64_t key = 0;
    std::memcpy(&key, full, sizeof(key));
    return key;
}

bool Engine::isSeen(uint64_t key)
{
    const double now = nowSec();
    if (seen_.size() > 20000) {
        for (auto it = seen_.begin(); it != seen_.end();) {
            if (now - it->second > 120.0) it = seen_.erase(it);
            else ++it;
        }
    }
    auto it = seen_.find(key);
    if (it != seen_.end() && now - it->second < 120.0) return true;
    seen_[key] = now;
    return false;
}

void Engine::markSeen(uint64_t key) { seen_[key] = nowSec(); }

ContactInfo* Engine::findContactByHash(uint8_t h)
{
    for (auto& c : contacts_)
        if (c.pub[0] == h) return &c;
    return nullptr;
}

ContactInfo* Engine::findContactByPub(const std::string& hex)
{
    for (auto& c : contacts_)
        if (bytesToHex(c.pub.data(), c.pub.size()) == hex) return &c;
    return nullptr;
}

ContactInfo& Engine::ensureContact(const uint8_t pub[32])
{
    for (auto& c : contacts_)
        if (std::memcmp(c.pub.data(), pub, 32) == 0) return c;
    ContactInfo c;
    std::memcpy(c.pub.data(), pub, 32);
    c.name = bytesToHex(pub, 4) + "...";
    contacts_.push_back(c);
    return contacts_.back();
}

void Engine::computeSecret(ContactInfo& c)
{
    if (c.secretValid) return;
    c.secret = id_.sharedSecret(c.pub.data());
    c.secretValid = true;
}

void Engine::addPacket(const std::vector<uint8_t>& raw, uint8_t header, uint8_t type,
                       uint8_t route, int hops, const std::vector<uint8_t>& path,
                       float snr, double quality, const std::string& summary)
{
    PacketInfo p;
    p.seenAt = nowSec();
    p.header = header;
    p.type = type;
    p.route = route;
    p.hops = hops;
    p.path = path;
    p.raw = raw;
    p.snr = snr;
    p.quality = quality;
    p.summary = summary;
    packets_.push_back(std::move(p));
    if (packets_.size() > 5000) packets_.erase(packets_.begin());
}

void Engine::addMessage(const ChatMessage& m)
{
    messages_.push_back(m);
    if (messages_.size() > 2000) messages_.erase(messages_.begin());
    markDirty();
    appendEvent(std::string(m.outgoing ? "TX " : "RX ") +
                (m.direct ? "DM " + m.sender : "GRP " + m.channel + " " + m.sender) +
                ": " + m.text);
}

void Engine::handleRaw(const std::vector<uint8_t>& raw, float snr, double quality)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (raw.empty()) return;

    mc::MeshCorePacket mp;
    std::string err;
    if (!mc::parseMeshCore(raw, mp, err)) {
        ++stats_.dropped;
        return;
    }
    const uint64_t key = packetKey(mp.type, mp.pathLenByte, mp.payload);
    if (isSeen(key)) return;
    ++stats_.rxPackets;

    const std::string summary = std::string(mc::meshcorePayloadName(mp.type)) + " " +
                                mc::meshcoreRouteName(mp.route) + " hops=" +
                                std::to_string(mp.hashCount);
    addPacket(raw, mp.header, mp.type, mp.route, mp.hashCount, mp.path, snr, quality, summary);

    bool forMe = false;
    switch (mp.type) {
    case TYPE_ADVERT:
        processAdvert(raw, mp.payload, mp.hashCount, snr);
        break;
    case TYPE_GRP_TXT:
    case TYPE_GRP_DATA:
        processGroup(raw, mp.payload, mp.type, mp.hashCount, snr);
        break;
    case TYPE_TXT:
    case TYPE_REQ:
    case TYPE_RESPONSE:
        if (mp.payload.size() >= 2 && mp.payload[0] == id_.pub[0]) forMe = true;
        processDirect(raw, mp.payload, mp.type, mp.route, mp.path, mp.hashCount, snr);
        break;
    case TYPE_ACK:
        processAck(mp.payload);
        break;
    case TYPE_PATH:
        if (mp.payload.size() >= 2 && mp.payload[0] == id_.pub[0]) forMe = true;
        processPath(raw, mp.payload, mp.route, mp.hashCount, snr);
        break;
    case TYPE_ANON_REQ:
        if (mp.payload.size() >= 1 && mp.payload[0] == id_.pub[0]) forMe = true;
        break;
    default:
        break;
    }

    if (role_ == Role::Repeater) {
        if (mp.route == ROUTE_FLOOD || mp.route == 0) {
            const bool addressedToMe = forMe && (mp.type == TYPE_TXT || mp.type == TYPE_REQ ||
                                                 mp.type == TYPE_RESPONSE || mp.type == TYPE_PATH ||
                                                 mp.type == TYPE_ANON_REQ || mp.type == TYPE_ACK);
            if (!addressedToMe)
                forwardFlood(raw, mp.path, mp.payload, mp.header, mp.hashCount);
        } else if (mp.route == ROUTE_DIRECT || mp.route == 3) {
            if (!mp.path.empty() && mp.path[0] == id_.pub[0])
                forwardDirect(raw, mp.path, mp.payload, mp.header);
        }
    }
}

void Engine::processAdvert(const std::vector<uint8_t>&, const std::vector<uint8_t>& payload,
                           int hops, float snr)
{
    mc::MeshCoreAdvert a;
    if (!mc::parseAdvert(payload, a)) return;
    if (std::memcmp(a.publicKey, id_.pub.data(), 32) == 0) return;
    if (!a.signatureValid) {
        log("advert rejected: bad signature");
        return;
    }
    ContactInfo& c = ensureContact(a.publicKey);
    c.name = a.name.empty() ? bytesToHex(a.publicKey, 4) + "..." : a.name;
    c.type = a.nodeType;
    c.hasLocation = a.hasLocation;
    if (a.hasLocation) {
        c.lat = a.latitude / 1e6;
        c.lon = a.longitude / 1e6;
    }
    c.lastAdvertTs = a.timestamp;
    c.lastSeen = nowSec();
    c.lastSnr = snr;
    c.secretValid = false;
    ++stats_.rxAdvert;
    markDirty();
    log("advert: " + c.name + " type=" + mc::meshcoreNodeTypeName(c.type) + " hops=" +
        std::to_string(hops));
}

void Engine::processGroup(const std::vector<uint8_t>&, const std::vector<uint8_t>& payload,
                          uint8_t type, int hops, float snr)
{
    if (payload.size() < 3) return;
    ChannelInfo* ch = nullptr;
    for (auto& c : channels_)
        if (c.enabled && c.hash == payload[0]) { ch = &c; break; }
    if (!ch) {
        log("group packet for unknown channel hash " + bytesToHex(payload.data(), 1));
        return;
    }

    mc::MeshCorePacket p;
    p.type = type;
    p.payload = payload;
    std::vector<uint8_t> plain;
    bool macOk = false;
    uint8_t expected = 0;
    if (!mc::decryptGroupPayload(p, ch->key, plain, macOk, expected)) return;

    ChatMessage m;
    m.outgoing = false;
    m.direct = false;
    m.channel = ch->name;
    m.channelHash = ch->hash;
    m.seenAt = nowSec();
    m.hops = hops;
    m.snr = snr;
    m.acked = true;
    if (type == TYPE_GRP_TXT) {
        uint32_t ts = 0;
        uint8_t flags = 0;
        std::string text;
        if (!mc::parseGroupText(plain, ts, flags, text)) return;
        const size_t sep = text.find(": ");
        if (sep != std::string::npos) {
            m.sender = text.substr(0, sep);
            m.text = text.substr(sep + 2);
        } else {
            m.text = text;
        }
        m.timestamp = ts;
    } else {
        uint16_t dt = 0;
        std::vector<uint8_t> data;
        if (!mc::parseGroupData(plain, dt, data)) return;
        m.timestamp = static_cast<uint32_t>(std::time(nullptr));
        m.sender = "data";
        m.text = "GRP_DATA type=" + bytesToHex(reinterpret_cast<const uint8_t*>(&dt), 2) +
                 " len=" + std::to_string(data.size()) + " " + bytesToHex(data);
    }
    ++stats_.rxGroup;
    addMessage(m);
    log("group " + ch->name + " from " + m.sender + ": " + m.text);
}

void Engine::processDirect(const std::vector<uint8_t>&, const std::vector<uint8_t>& payload,
                           uint8_t type, uint8_t route, const std::vector<uint8_t>& path,
                           int hops, float snr)
{
    if (payload.size() < 3) return;
    const uint8_t dest = payload[0];
    const uint8_t src = payload[1];
    if (dest != id_.pub[0]) return;

    ContactInfo* c = findContactByHash(src);
    if (!c) {
        log("direct packet for us from unknown contact hash " + bytesToHex(&src, 1));
        return;
    }
    computeSecret(*c);
    std::vector<uint8_t> plain;
    if (!decryptDirect(c->secret.data(), payload.data() + 2, payload.size() - 2, plain))
        return;

    if (type == TYPE_TXT && plain.size() >= 5) {
        const uint32_t ts = getLe32(plain.data());
        const uint8_t flags = plain[4];
        const uint8_t txtType = flags >> 2;
        std::string text = trimZero(plain, 5);
        ChatMessage m;
        m.outgoing = false;
        m.direct = true;
        m.peerPubHex = bytesToHex(c->pub.data(), c->pub.size());
        m.sender = c->name;
        m.text = text;
        m.timestamp = ts;
        m.seenAt = nowSec();
        m.hops = hops;
        m.snr = snr;
        m.acked = true;
        ++stats_.rxDirect;
        addMessage(m);
        log("direct from " + c->name + ": " + text);

        if (txtType == 0) {
            std::vector<uint8_t> ackMsg;
            putLe32(ackMsg, ts);
            ackMsg.push_back(flags); // attempt + txt_type, as MeshCore hashes it
            ackMsg.insert(ackMsg.end(), text.begin(), text.end());
            std::array<uint8_t, 4> code{};
            // MeshCore appends the sender pubkey to the hash.
            mc::Sha256 sha;
            sha.update(ackMsg.data(), ackMsg.size());
            sha.update(c->pub.data(), c->pub.size());
            uint8_t full[32];
            sha.finalize(full);
            std::memcpy(code.data(), full, 4);

            const uint8_t extraAttempt =
                (plain.size() > 5 + text.size() + 1) ? plain[5 + text.size() + 1] : 0;
            if (route == ROUTE_FLOOD || route == 0)
                sendPathReturn(*c, path, code, extraAttempt);
            else
                sendAck(code, c->outPath);
        }
    } else {
        log("direct " + std::string(mc::meshcorePayloadName(type)) + " from " + c->name);
    }
}

void Engine::sendAck(const std::array<uint8_t, 4>& code, const std::vector<uint8_t>& path)
{
    std::vector<uint8_t> payload(code.begin(), code.end());
    const bool havePath = !path.empty();
    auto raw = havePath
        ? buildPlainRaw(headerByte(TYPE_ACK, ROUTE_DIRECT), payload, path)
        : buildPlainRaw(headerByte(TYPE_ACK, ROUTE_FLOOD), payload);
    txRaw(std::move(raw), 200);
}

void Engine::sendPathReturn(const ContactInfo& contact, const std::vector<uint8_t>& path,
                            const std::array<uint8_t, 4>& ack, uint8_t extraAttempt)
{
    std::vector<uint8_t> plain;
    const size_t hopCount = std::min<size_t>(path.size(), 63);
    plain.push_back(static_cast<uint8_t>(hopCount)); // 1-byte path hashes
    plain.insert(plain.end(), path.begin(), path.begin() + static_cast<std::ptrdiff_t>(hopCount));
    plain.push_back(TYPE_ACK);
    plain.insert(plain.end(), ack.begin(), ack.end());
    plain.push_back(extraAttempt);
    plain.push_back(static_cast<uint8_t>(std::rand() & 0xFF));

    const auto enc = encryptDirect(contact.secret.data(), plain.data(), plain.size());
    std::vector<uint8_t> payload;
    payload.reserve(2 + enc.size());
    payload.push_back(contact.pub[0]);
    payload.push_back(id_.pub[0]);
    payload.insert(payload.end(), enc.begin(), enc.end());

    auto raw = buildPlainRaw(headerByte(TYPE_PATH, ROUTE_FLOOD), payload);
    txRaw(std::move(raw), 200);
}

void Engine::processAck(const std::vector<uint8_t>& payload)
{
    if (payload.size() < 4) return;
    for (auto it = pendingAcks_.begin(); it != pendingAcks_.end(); ++it) {
        if (std::memcmp(it->code.data(), payload.data(), 4) == 0) {
            if (it->messageIndex < messages_.size())
                messages_[it->messageIndex].acked = true;
            pendingAcks_.erase(it);
            markDirty();
            log("ACK matched");
            return;
        }
    }
}

void Engine::processPath(const std::vector<uint8_t>&, const std::vector<uint8_t>& payload,
                         uint8_t, int hops, float snr)
{
    if (payload.size() < 3) return;
    if (payload[0] != id_.pub[0]) return;
    ContactInfo* c = findContactByHash(payload[1]);
    if (!c) return;
    computeSecret(*c);
    std::vector<uint8_t> plain;
    if (!decryptDirect(c->secret.data(), payload.data() + 2, payload.size() - 2, plain))
        return;
    if (plain.empty()) return;

    const uint8_t pathLenByte = plain[0];
    const int hashSize = (pathLenByte >> 6) + 1;
    const int hashCount = pathLenByte & 0x3F;
    const size_t pathBytes = static_cast<size_t>(hashSize) * hashCount;
    if (1 + pathBytes > plain.size()) return;

    c->outPath.assign(plain.begin() + 1, plain.begin() + 1 + static_cast<std::ptrdiff_t>(pathBytes));
    c->lastSeen = nowSec();
    c->lastSnr = snr;
    markDirty();
    log("PATH from " + c->name + " hops=" + std::to_string(hashCount));

    const size_t extraOff = 1 + pathBytes;
    if (extraOff < plain.size()) {
        const uint8_t extraType = plain[extraOff] & 0x0F;
        const uint8_t* extra = plain.data() + extraOff + 1;
        const size_t extraLen = plain.size() - extraOff - 1;
        if (extraType == TYPE_ACK && extraLen >= 4) {
            std::vector<uint8_t> ack(extra, extra + extraLen);
            processAck(ack);
        }
    }
    (void)hops;
}

std::vector<uint8_t> Engine::buildDirectPayload(const ContactInfo& contact, uint8_t type,
                                                const uint8_t* data, size_t len)
{
    (void)type;
    std::vector<uint8_t> payload;
    payload.reserve(2 + len + 16);
    payload.push_back(contact.pub[0]);
    payload.push_back(id_.pub[0]);
    const auto enc = encryptDirect(contact.secret.data(), data, len);
    payload.insert(payload.end(), enc.begin(), enc.end());
    return payload;
}

bool Engine::sendDirectText(const std::string& pubHex, const std::string& text)
{
    std::lock_guard<std::mutex> lk(mu_);
    ContactInfo* c = findContactByPub(pubHex);
    if (!c) return false;
    computeSecret(*c);

    const uint32_t ts = nextTimestamp();
    std::vector<uint8_t> plain;
    putLe32(plain, ts);
    plain.push_back(0);
    plain.insert(plain.end(), text.begin(), text.end());

    auto payload = buildDirectPayload(*c, TYPE_TXT, plain.data(), plain.size());
    const bool havePath = !c->outPath.empty();
    auto raw = havePath
        ? buildPlainRaw(headerByte(TYPE_TXT, ROUTE_DIRECT), payload, c->outPath)
        : buildPlainRaw(headerByte(TYPE_TXT, ROUTE_FLOOD), payload);

    ChatMessage m;
    m.outgoing = true;
    m.direct = true;
    m.peerPubHex = pubHex;
    m.sender = name_;
    m.text = text;
    m.timestamp = ts;
    m.seenAt = nowSec();
    m.acked = false;
    addMessage(m);

    std::vector<uint8_t> ackMsg;
    putLe32(ackMsg, ts);
    ackMsg.push_back(0); // attempt 0, TXT_TYPE_PLAIN
    ackMsg.insert(ackMsg.end(), text.begin(), text.end());
    mc::Sha256 sha;
    sha.update(ackMsg.data(), ackMsg.size());
    sha.update(id_.pub.data(), id_.pub.size());
    uint8_t full[32];
    sha.finalize(full);
    PendingAck p;
    std::memcpy(p.code.data(), full, 4);
    p.messageIndex = messages_.size() - 1;
    p.created = nowSec();
    pendingAcks_.push_back(p);
    if (pendingAcks_.size() > 64) pendingAcks_.pop_front();

    txRaw(std::move(raw), 0);
    return true;
}

void Engine::sendGroupText(int channelIndex, const std::string& text)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (channelIndex < 0 || channelIndex >= static_cast<int>(channels_.size())) return;
    auto& ch = channels_[channelIndex];
    const uint32_t ts = nextTimestamp();
    auto raw = buildGroupText(ch.key, name_, text, ts, ROUTE_FLOOD);
    if (raw.empty()) return;

    ChatMessage m;
    m.outgoing = true;
    m.direct = false;
    m.channel = ch.name;
    m.channelHash = ch.hash;
    m.sender = name_;
    m.text = text;
    m.timestamp = ts;
    m.seenAt = nowSec();
    m.acked = true;
    addMessage(m);
    txRaw(std::move(raw), 0);
}

void Engine::sendAdvert(bool flood)
{
    std::lock_guard<std::mutex> lk(mu_);
    const uint32_t ts = nextTimestamp();
    auto raw = buildAdvert(id_, name_, hasLoc_, lat_, lon_, ts, flood ? ROUTE_FLOOD : ROUTE_DIRECT);
    if (!raw.empty()) txRaw(std::move(raw), 0);
}

void Engine::forwardFlood(const std::vector<uint8_t>& raw, const std::vector<uint8_t>& path,
                          const std::vector<uint8_t>& payload, uint8_t header, int hops)
{
    (void)raw;
    if (path.size() >= 64) return;
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i] == id_.pub[0]) return; // loop detected

    std::vector<uint8_t> newRaw;
    newRaw.reserve(2 + path.size() + 1 + payload.size());
    newRaw.push_back(header);
    newRaw.push_back(static_cast<uint8_t>(path.size() + 1));
    newRaw.insert(newRaw.end(), path.begin(), path.end());
    newRaw.push_back(id_.pub[0]);
    newRaw.insert(newRaw.end(), payload.begin(), payload.end());

    const int delay = 120 + hops * 60 + (std::rand() % 120);
    ++stats_.forwarded;
    txRaw(std::move(newRaw), delay);
}

void Engine::forwardDirect(const std::vector<uint8_t>& raw, const std::vector<uint8_t>& path,
                           const std::vector<uint8_t>& payload, uint8_t header)
{
    (void)raw;
    if (path.empty()) return;
    std::vector<uint8_t> newRaw;
    newRaw.reserve(2 + path.size() + payload.size());
    newRaw.push_back(header & ~0x03);
    newRaw.back() |= ROUTE_DIRECT;
    newRaw.push_back(static_cast<uint8_t>(path.size() - 1));
    newRaw.insert(newRaw.end(), path.begin() + 1, path.end());
    newRaw.insert(newRaw.end(), payload.begin(), payload.end());
    ++stats_.forwarded;
    txRaw(std::move(newRaw), 60 + (std::rand() % 80));
}

void Engine::txRaw(std::vector<uint8_t> raw, int delayMs)
{
    ++stats_.txPackets;
    mc::MeshCorePacket p;
    std::string e;
    if (mc::parseMeshCore(raw, p, e))
        markSeen(packetKey(p.type, p.pathLenByte, p.payload));
    else
        seen_[fnv1a(raw)] = nowSec();
    if (tx_) tx_(std::move(raw), delayMs);
}

std::vector<ChatMessage> Engine::messages() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return messages_;
}

std::vector<ContactInfo> Engine::contacts() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return contacts_;
}

std::vector<PacketInfo> Engine::packets() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return packets_;
}

std::vector<ChannelInfo> Engine::channels() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return channels_;
}

EngineStats Engine::stats() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

std::string Engine::identityHex() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return id_.pubHex();
}

std::string Engine::nodeName() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return name_;
}

std::vector<std::string> Engine::logs() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return logs_;
}

} // namespace mctrx
