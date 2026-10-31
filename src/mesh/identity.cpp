#include "identity.h"

#include "ed_25519.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace mctrx {
namespace {

std::string toHex(const uint8_t* p, size_t n)
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

bool fromHex(const std::string& s, uint8_t* out, size_t n)
{
    if (s.size() != n * 2) return false;
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        const int hi = val(s[2 * i]);
        const int lo = val(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

} // namespace

Identity Identity::fromSeed(const std::array<uint8_t, 32>& seed)
{
    Identity id;
    ed25519_create_keypair(id.pub.data(), id.priv.data(), seed.data());
    id.valid = true;
    return id;
}

Identity Identity::generate()
{
    std::array<uint8_t, 32> seed{};
    ed25519_create_seed(seed.data());
    return fromSeed(seed);
}

bool Identity::load(const std::string& path, Identity& out)
{
    std::ifstream f(path);
    if (!f) return false;
    std::string k1, v1, k2, v2;
    if (!(f >> k1 >> v1 >> k2 >> v2)) return false;
    Identity id;
    if (!fromHex(v1, id.pub.data(), 32)) return false;
    if (!fromHex(v2, id.priv.data(), 64)) return false;
    // Re-derive the public key from the private key to catch corruption.
    ed25519_derive_pub(id.pub.data(), id.priv.data());
    id.valid = true;
    out = id;
    return true;
}

bool Identity::save(const std::string& path) const
{
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "pub " << pubHex() << "\npriv " << privHex() << "\n";
    return bool(f);
}

std::array<uint8_t, 64> Identity::sign(const uint8_t* msg, size_t len) const
{
    std::array<uint8_t, 64> sig{};
    ed25519_sign(sig.data(), msg, len, pub.data(), priv.data());
    return sig;
}

std::array<uint8_t, 64> Identity::sign(const std::vector<uint8_t>& msg) const
{
    return sign(msg.data(), msg.size());
}

bool Identity::verify(const uint8_t sig[64], const uint8_t* msg, size_t len) const
{
    return ed25519_verify(sig, msg, len, pub.data()) == 1;
}

std::array<uint8_t, 32> Identity::sharedSecret(const uint8_t otherPub[32]) const
{
    std::array<uint8_t, 32> s{};
    ed25519_key_exchange(s.data(), otherPub, priv.data());
    return s;
}

std::string Identity::pubHex() const { return toHex(pub.data(), pub.size()); }
std::string Identity::privHex() const { return toHex(priv.data(), priv.size()); }

} // namespace mctrx
