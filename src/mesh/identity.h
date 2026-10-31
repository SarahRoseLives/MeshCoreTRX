#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mctrx {

struct Identity {
    std::array<uint8_t, 32> pub{};
    std::array<uint8_t, 64> priv{};
    bool valid = false;

    static Identity generate();
    static Identity fromSeed(const std::array<uint8_t, 32>& seed);
    static bool load(const std::string& path, Identity& out);
    bool save(const std::string& path) const;

    std::array<uint8_t, 64> sign(const uint8_t* msg, size_t len) const;
    std::array<uint8_t, 64> sign(const std::vector<uint8_t>& msg) const;
    bool verify(const uint8_t sig[64], const uint8_t* msg, size_t len) const;
    std::array<uint8_t, 32> sharedSecret(const uint8_t otherPub[32]) const;

    std::string pubHex() const;
    std::string privHex() const;
};

} // namespace mctrx
