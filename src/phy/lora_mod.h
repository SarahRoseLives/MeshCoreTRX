#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace mctrx {

struct LoraModParams {
    int sf = 7;
    double bw = 62500.0;
    double fs = 250000.0;
    int cr = 8;          // MeshCore/LoRa coding rate denominator: 5..8
    bool hasCrc = true;
    int preambleLen = 32;
    int syncWord = 0x12;
};

// Modulate one raw LoRa payload (the MeshCore packet bytes) into complex
// baseband at fs.  fs must be an integer multiple of bw.
std::vector<std::complex<double>> modulateLora(const std::vector<uint8_t>& payload,
                                               const LoraModParams& p,
                                               std::string* err = nullptr);

// Same, but append `trailingSilence` samples of zeros.
std::vector<std::complex<double>> modulateLoraWithPad(const std::vector<uint8_t>& payload,
                                                      const LoraModParams& p,
                                                      int leadingSilence,
                                                      int trailingSilence,
                                                      std::string* err = nullptr);

} // namespace mctrx
