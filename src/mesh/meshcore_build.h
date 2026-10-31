#pragma once

#include "identity.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mctrx {

// MeshCore public channel key (16 bytes).
extern const uint8_t kPublicChannelKey[16];

std::vector<uint8_t> buildGroupText(const std::vector<uint8_t>& channelKey,
                                    const std::string& sender,
                                    const std::string& text,
                                    uint32_t timestamp,
                                    uint8_t routeType = 1);

std::vector<uint8_t> buildAdvert(const Identity& id,
                                 const std::string& name,
                                 bool hasLocation,
                                 double lat,
                                 double lon,
                                 uint32_t timestamp,
                                 uint8_t routeType = 1);

std::vector<uint8_t> buildPlainRaw(uint8_t header, const std::vector<uint8_t>& payload,
                                   const std::vector<uint8_t>& path = {});

std::vector<uint8_t> hexToBytes(const std::string& hex);
std::string bytesToHex(const uint8_t* p, size_t n);
std::string bytesToHex(const std::vector<uint8_t>& v);

} // namespace mctrx
