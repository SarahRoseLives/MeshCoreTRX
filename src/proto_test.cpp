// Protocol engine test: adverts, group RX, direct message + ACK, repeater forward.
#include "app/engine.h"
#include "mesh/crypto.h"
#include "mesh/meshcore.h"
#include "mesh/meshcore_build.h"

#include <cstring>
#include <iostream>
#include <vector>

namespace {

uint32_t rd32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

bool decryptTest(const uint8_t secret[32], const uint8_t* src, size_t len,
                 std::vector<uint8_t>& out)
{
    if (len <= 2) return false;
    const size_t ctLen = len - 2;
    if (ctLen == 0 || ctLen % 16) return false;
    uint8_t mac[32];
    mc::hmacSha256(secret, 32, src + 2, ctLen, mac);
    if (mac[0] != src[0] || mac[1] != src[1]) return false;
    mc::Aes128 aes;
    aes.setKey(secret);
    out.resize(ctLen);
    for (size_t off = 0; off < ctLen; off += 16)
        aes.decryptBlock(src + 2 + off, &out[off]);
    while (!out.empty() && out.back() == 0) out.pop_back();
    return true;
}

} // namespace

int main()
{
    mctrx::Identity ida = mctrx::Identity::generate();
    mctrx::Identity idb = mctrx::Identity::generate();

    mctrx::Engine a;
    mctrx::Engine b;
    a.setPersistence(false);
    b.setPersistence(false);
    a.init(ida, "NodeA", false, 0, 0, mctrx::Role::Companion);
    b.init(idb, "NodeB", false, 0, 0, mctrx::Role::Repeater);

    std::vector<std::vector<uint8_t>> aTx, bTx;
    a.setTx([&](std::vector<uint8_t> raw, int) { aTx.push_back(std::move(raw)); });
    b.setTx([&](std::vector<uint8_t> raw, int) { bTx.push_back(std::move(raw)); });

    a.sendAdvert(true);
    if (aTx.empty()) { std::cerr << "FAIL: A advert not built\n"; return 1; }
    b.handleRaw(aTx.back(), 0, 1.0);
    aTx.clear();

    b.sendAdvert(true);
    if (bTx.empty()) { std::cerr << "FAIL: B advert not built\n"; return 1; }
    a.handleRaw(bTx.back(), 0, 1.0);
    bTx.clear();

    if (a.contacts().empty() || b.contacts().empty()) {
        std::cerr << "FAIL: contact discovery\n";
        return 1;
    }

    a.sendGroupText(0, "protocol test group");
    if (aTx.empty()) { std::cerr << "FAIL: group not built\n"; return 1; }
    b.handleRaw(aTx.back(), 0, 1.0);
    aTx.clear();
    const auto bMsgs = b.messages();
    if (bMsgs.empty() || bMsgs.back().text != "protocol test group") {
        std::cerr << "FAIL: group receive\n";
        return 1;
    }
    if (bTx.empty()) { std::cerr << "FAIL: repeater did not forward\n"; return 1; }
    const auto forwarded = bTx.back();
    bTx.clear();
    b.handleRaw(forwarded, 0, 1.0);
    if (!bTx.empty()) { std::cerr << "FAIL: repeater loop not suppressed\n"; return 1; }

    if (!a.sendDirectText(idb.pubHex(), "direct hello")) {
        std::cerr << "FAIL: direct send\n";
        return 1;
    }
    if (aTx.empty()) { std::cerr << "FAIL: direct raw not built\n"; return 1; }
    const auto directRaw = aTx.back();
    b.handleRaw(directRaw, 0, 1.0);
    aTx.clear();

    const auto bDm = b.messages();
    if (bDm.empty() || bDm.back().text != "direct hello") {
        std::cerr << "FAIL: direct receive\n";
        return 1;
    }

    // Independently reproduce MeshCore's expected ACK:
    // sha256(timestamp || flags/attempt || text || sender_pubkey)[0..3].
    mc::MeshCorePacket dp;
    std::string err;
    if (!mc::parseMeshCore(directRaw, dp, err)) {
        std::cerr << "FAIL: parse direct\n";
        return 1;
    }
    const auto secret = idb.sharedSecret(ida.pub.data());
    std::vector<uint8_t> plain;
    if (!decryptTest(secret.data(), dp.payload.data() + 2, dp.payload.size() - 2, plain)) {
        std::cerr << "FAIL: decrypt direct\n";
        return 1;
    }
    const uint32_t ts = rd32(plain.data());
    const uint8_t flags = plain[4];
    std::string text;
    for (size_t i = 5; i < plain.size() && plain[i] != 0; ++i)
        text.push_back(static_cast<char>(plain[i]));

    std::vector<uint8_t> ackMsg;
    ackMsg.push_back(static_cast<uint8_t>(ts));
    ackMsg.push_back(static_cast<uint8_t>(ts >> 8));
    ackMsg.push_back(static_cast<uint8_t>(ts >> 16));
    ackMsg.push_back(static_cast<uint8_t>(ts >> 24));
    ackMsg.push_back(flags);
    ackMsg.insert(ackMsg.end(), text.begin(), text.end());
    mc::Sha256 sha;
    sha.update(ackMsg.data(), ackMsg.size());
    sha.update(ida.pub.data(), ida.pub.size());
    uint8_t expect[32];
    sha.finalize(expect);

    if (bTx.empty()) { std::cerr << "FAIL: no ACK/PATH generated\n"; return 1; }
    mc::MeshCorePacket rp;
    if (!mc::parseMeshCore(bTx.back(), rp, err)) {
        std::cerr << "FAIL: parse ACK/PATH\n";
        return 1;
    }
    std::vector<uint8_t> ackBytes;
    if (rp.type == 8) {
        std::vector<uint8_t> rplain;
        if (!decryptTest(secret.data(), rp.payload.data() + 2, rp.payload.size() - 2, rplain)) {
            std::cerr << "FAIL: decrypt PATH\n";
            return 1;
        }
        const uint8_t pathLen = rplain[0];
        const int hashSize = (pathLen >> 6) + 1;
        const int hashCount = pathLen & 0x3F;
        const size_t pathBytes = static_cast<size_t>(hashSize) * hashCount;
        const size_t extraOff = 1 + pathBytes;
        if (extraOff >= rplain.size() || (rplain[extraOff] & 0x0F) != 3) {
            std::cerr << "FAIL: PATH has no ACK extra\n";
            return 1;
        }
        ackBytes.assign(rplain.begin() + extraOff + 1, rplain.end());
    } else if (rp.type == 3) {
        ackBytes = rp.payload;
    } else {
        std::cerr << "FAIL: unexpected ACK response type " << int(rp.type) << "\n";
        return 1;
    }
    if (ackBytes.size() < 4 || std::memcmp(ackBytes.data(), expect, 4) != 0) {
        std::cerr << "FAIL: ACK does not match MeshCore formula\n";
        return 1;
    }

    a.handleRaw(bTx.back(), 0, 1.0);
    const auto aMsgs = a.messages();
    bool acked = false;
    for (const auto& m : aMsgs)
        if (m.outgoing && m.direct && m.text == "direct hello" && m.acked) acked = true;
    if (!acked) { std::cerr << "FAIL: ACK not matched by sender\n"; return 1; }

    std::cout << "PASS: adverts, group, repeater forward, direct message, MeshCore ACK, PATH return\n";
    return 0;
}
