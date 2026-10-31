#pragma once

#include "mesh/identity.h"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mctrx {

enum class Role { Companion, Repeater };

struct ChannelInfo {
    std::string name;
    std::vector<uint8_t> key;
    uint8_t hash = 0;
    bool enabled = true;
};

struct ContactInfo {
    std::array<uint8_t, 32> pub{};
    std::string name;
    int type = 0;
    bool hasLocation = false;
    double lat = 0.0;
    double lon = 0.0;
    double lastSeen = 0.0;
    uint32_t lastAdvertTs = 0;
    std::vector<uint8_t> outPath;
    std::array<uint8_t, 32> secret{};
    bool secretValid = false;
    int lastRssi = 0;
    float lastSnr = 0.0f;
};

struct ChatMessage {
    bool outgoing = false;
    bool direct = false;
    bool acked = false;
    uint8_t channelHash = 0;
    std::string channel;
    std::string peerPubHex;
    std::string sender;
    std::string text;
    uint32_t timestamp = 0;
    double seenAt = 0.0;
    int hops = 0;
    float snr = 0.0f;
};

struct PacketInfo {
    double seenAt = 0.0;
    uint8_t header = 0;
    uint8_t type = 0;
    uint8_t route = 0;
    int hops = 0;
    std::vector<uint8_t> path;
    std::vector<uint8_t> raw;
    float snr = 0.0f;
    double quality = 0.0;
    std::string summary;
};

struct EngineStats {
    uint64_t rxPackets = 0;
    uint64_t txPackets = 0;
    uint64_t rxGroup = 0;
    uint64_t rxDirect = 0;
    uint64_t rxAdvert = 0;
    uint64_t dropped = 0;
    uint64_t forwarded = 0;
};

class Engine {
public:
    using TxFn = std::function<void(std::vector<uint8_t> raw, int delayMs)>;

    Engine() = default;
    ~Engine();
    void setPersistence(bool en) { persistence_ = en; }
    void saveIfDirty();
    void save();

    void init(const Identity& id, const std::string& name, bool hasLocation,
              double lat, double lon, Role role);
    void setTx(TxFn fn) { tx_ = std::move(fn); }
    void setRole(Role r) { role_ = r; }
    Role role() const { return role_; }
    void setNodeName(const std::string& n) { name_ = n; }
    void setLocation(bool has, double lat, double lon) { hasLoc_ = has; lat_ = lat; lon_ = lon; }
    void setPublicChannelEnabled(bool en);
    bool addChannel(const std::string& name, const std::vector<uint8_t>& key);
    void setChannelEnabled(int index, bool en);

    void handleRaw(const std::vector<uint8_t>& raw, float snr, double quality);

    void sendGroupText(int channelIndex, const std::string& text);
    void sendAdvert(bool flood);
    bool sendDirectText(const std::string& pubHex, const std::string& text);

    std::vector<ChatMessage> messages() const;
    std::vector<ContactInfo> contacts() const;
    std::vector<PacketInfo> packets() const;
    std::vector<ChannelInfo> channels() const;
    EngineStats stats() const;
    std::string identityHex() const;
    std::string nodeName() const;
    bool hasLocation() const { return hasLoc_; }
    double latitude() const { return lat_; }
    double longitude() const { return lon_; }
    void log(const std::string& line);
    std::vector<std::string> logs() const;

private:
    struct PendingAck {
        std::array<uint8_t, 4> code{};
        size_t messageIndex = 0;
        double created = 0.0;
    };

    ContactInfo* findContactByHash(uint8_t h);
    ContactInfo* findContactByPub(const std::string& hex);
    ContactInfo& ensureContact(const uint8_t pub[32]);
    void computeSecret(ContactInfo& c);
    uint32_t nextTimestamp();
    uint64_t packetKey(uint8_t type, uint16_t pathLenByte,
                       const std::vector<uint8_t>& payload) const;
    bool isSeen(uint64_t key);
    void markSeen(uint64_t key);
    void addPacket(const std::vector<uint8_t>& raw, uint8_t header, uint8_t type, uint8_t route,
                   int hops, const std::vector<uint8_t>& path, float snr, double quality,
                   const std::string& summary);
    void addMessage(const ChatMessage& m);
    void processAdvert(const std::vector<uint8_t>& raw, const std::vector<uint8_t>& payload,
                       int hops, float snr);
    void processGroup(const std::vector<uint8_t>& raw, const std::vector<uint8_t>& payload,
                      uint8_t type, int hops, float snr);
    void processDirect(const std::vector<uint8_t>& raw, const std::vector<uint8_t>& payload,
                       uint8_t type, uint8_t route, const std::vector<uint8_t>& path,
                       int hops, float snr);
    void processAck(const std::vector<uint8_t>& payload);
    void processPath(const std::vector<uint8_t>& raw, const std::vector<uint8_t>& payload,
                     uint8_t route, int hops, float snr);
    void sendAck(const std::array<uint8_t, 4>& code, const std::vector<uint8_t>& path = {});
    void sendPathReturn(const ContactInfo& contact, const std::vector<uint8_t>& path,
                        const std::array<uint8_t, 4>& ack, uint8_t extraAttempt);
    void forwardFlood(const std::vector<uint8_t>& raw, const std::vector<uint8_t>& path,
                      const std::vector<uint8_t>& payload, uint8_t header, int hops);
    void forwardDirect(const std::vector<uint8_t>& raw, const std::vector<uint8_t>& path,
                       const std::vector<uint8_t>& payload, uint8_t header);
    std::vector<uint8_t> buildDirectPayload(const ContactInfo& contact, uint8_t type,
                                            const uint8_t* data, size_t len);
    void txRaw(std::vector<uint8_t> raw, int delayMs);
    void loadPersistent();
    void savePersistent();
    void markDirty() { dirty_ = true; }

    mutable std::mutex mu_;
    Identity id_;
    std::string name_ = "MeshCoreTRX";
    bool hasLoc_ = false;
    double lat_ = 0.0;
    double lon_ = 0.0;
    Role role_ = Role::Companion;
    TxFn tx_;

    std::vector<ChannelInfo> channels_;
    std::vector<ContactInfo> contacts_;
    std::vector<ChatMessage> messages_;
    std::vector<PacketInfo> packets_;
    std::vector<std::string> logs_;
    std::deque<PendingAck> pendingAcks_;
    std::unordered_map<uint64_t, double> seen_;
    uint32_t lastTs_ = 0;
    EngineStats stats_;
    bool persistence_ = true;
    bool dirty_ = false;
};

} // namespace mctrx
