// lora_mod.cpp - LoRa PHY modulator for MeshCoreTRX.
//
// This is the inverse of the reference MeshCoreSniffer demod chain and mirrors
// the gr-lora_sdr transmit chain:
//   whitening -> explicit header -> CRC -> Hamming -> interleave -> Gray ->
//   chirp modulation -> 32-symbol preamble + sync + 2.25 down-chirp SFD.

#include "lora_mod.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace mctrx {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

int mod(int a, int b)
{
    const int r = a % b;
    return r < 0 ? r + b : r;
}

int grayEnc(int v) { return v ^ (v >> 1); }

int grayDec(int g)
{
    int v = 0;
    while (g) {
        v ^= g;
        g >>= 1;
    }
    return v;
}

const std::array<uint8_t, 255>& whiteningSeq()
{
    static const std::array<uint8_t, 255> seq = {
        0xFF, 0xFE, 0xFC, 0xF8, 0xF0, 0xE1, 0xC2, 0x85, 0x0B, 0x17, 0x2F, 0x5E,
        0xBC, 0x78, 0xF1, 0xE3, 0xC6, 0x8D, 0x1A, 0x34, 0x68, 0xD0, 0xA0, 0x40,
        0x80, 0x01, 0x02, 0x04, 0x08, 0x11, 0x23, 0x47, 0x8E, 0x1C, 0x38, 0x71,
        0xE2, 0xC4, 0x89, 0x12, 0x25, 0x4B, 0x97, 0x2E, 0x5C, 0xB8, 0x70, 0xE0,
        0xC0, 0x81, 0x03, 0x06, 0x0C, 0x19, 0x32, 0x64, 0xC9, 0x92, 0x24, 0x49,
        0x93, 0x26, 0x4D, 0x9B, 0x37, 0x6E, 0xDC, 0xB9, 0x72, 0xE4, 0xC8, 0x90,
        0x20, 0x41, 0x82, 0x05, 0x0A, 0x15, 0x2B, 0x56, 0xAD, 0x5B, 0xB6, 0x6D,
        0xDA, 0xB5, 0x6B, 0xD6, 0xAC, 0x59, 0xB2, 0x65, 0xCB, 0x96, 0x2C, 0x58,
        0xB0, 0x61, 0xC3, 0x87, 0x0F, 0x1F, 0x3E, 0x7D, 0xFB, 0xF6, 0xED, 0xDB,
        0xB7, 0x6F, 0xDE, 0xBD, 0x7A, 0xF5, 0xEB, 0xD7, 0xAE, 0x5D, 0xBA, 0x74,
        0xE8, 0xD1, 0xA2, 0x44, 0x88, 0x10, 0x21, 0x43, 0x86, 0x0D, 0x1B, 0x36,
        0x6C, 0xD8, 0xB1, 0x63, 0xC7, 0x8F, 0x1E, 0x3C, 0x79, 0xF3, 0xE7, 0xCE,
        0x9C, 0x39, 0x73, 0xE6, 0xCC, 0x98, 0x31, 0x62, 0xC5, 0x8B, 0x16, 0x2D,
        0x5A, 0xB4, 0x69, 0xD2, 0xA4, 0x48, 0x91, 0x22, 0x45, 0x8A, 0x14, 0x29,
        0x52, 0xA5, 0x4A, 0x95, 0x2A, 0x54, 0xA9, 0x53, 0xA7, 0x4E, 0x9D, 0x3B,
        0x77, 0xEE, 0xDD, 0xBB, 0x76, 0xEC, 0xD9, 0xB3, 0x67, 0xCF, 0x9E, 0x3D,
        0x7B, 0xF7, 0xEF, 0xDF, 0xBF, 0x7E, 0xFD, 0xFA, 0xF4, 0xE9, 0xD3, 0xA6,
        0x4C, 0x99, 0x33, 0x66, 0xCD, 0x9A, 0x35, 0x6A, 0xD4, 0xA8, 0x51, 0xA3,
        0x46, 0x8C, 0x18, 0x30, 0x60, 0xC1, 0x83, 0x07, 0x0E, 0x1D, 0x3A, 0x75,
        0xEA, 0xD5, 0xAA, 0x55, 0xAB, 0x57, 0xAF, 0x5F, 0xBE, 0x7C, 0xF9, 0xF2,
        0xE5, 0xCA, 0x94, 0x28, 0x50, 0xA1, 0x42, 0x84, 0x09, 0x13, 0x27, 0x4F,
        0x9F, 0x3F, 0x7F};
    return seq;
}

uint16_t crc16(uint16_t crc, uint8_t b)
{
    for (int k = 0; k < 8; ++k) {
        if (((crc & 0x8000) >> 8) ^ (b & 0x80))
            crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
        else
            crc = static_cast<uint16_t>(crc << 1);
        b = static_cast<uint8_t>(b << 1);
    }
    return crc;
}

uint8_t hammingEncode(uint8_t nibble, int crApp)
{
    const int db[4] = {(nibble >> 3) & 1, (nibble >> 2) & 1, (nibble >> 1) & 1, nibble & 1};
    int cw = 0;
    if (crApp != 1) {
        const int p0 = db[3] ^ db[2] ^ db[1];
        const int p1 = db[2] ^ db[1] ^ db[0];
        const int p2 = db[3] ^ db[2] ^ db[0];
        const int p3 = db[3] ^ db[1] ^ db[0];
        cw = (db[3] << 7) | (db[2] << 6) | (db[1] << 5) | (db[0] << 4) |
             (p0 << 3) | (p1 << 2) | (p2 << 1) | p3;
        cw >>= (4 - crApp);
    } else {
        const int p4 = db[0] ^ db[1] ^ db[2] ^ db[3];
        cw = (db[3] << 4) | (db[2] << 3) | (db[1] << 2) | (db[0] << 1) | p4;
    }
    return static_cast<uint8_t>(cw);
}

int headerChecksum(const int nib[5])
{
    const int c4 = (nib[0] >> 3 & 1) ^ (nib[0] >> 2 & 1) ^ (nib[0] >> 1 & 1) ^ (nib[0] & 1);
    const int c3 = (nib[0] >> 3 & 1) ^ (nib[1] >> 3 & 1) ^ (nib[1] >> 2 & 1) ^ (nib[1] >> 1 & 1) ^ (nib[2] & 1);
    const int c2 = (nib[0] >> 2 & 1) ^ (nib[1] >> 3 & 1) ^ (nib[1] & 1) ^ (nib[2] >> 3 & 1) ^ (nib[2] >> 1 & 1);
    const int c1 = (nib[0] >> 1 & 1) ^ (nib[1] >> 2 & 1) ^ (nib[1] & 1) ^ (nib[2] >> 2 & 1) ^ (nib[2] >> 1 & 1) ^ (nib[2] & 1);
    const int c0 = (nib[0] & 1) ^ (nib[1] >> 1 & 1) ^ (nib[2] >> 3 & 1) ^ (nib[2] >> 2 & 1) ^ (nib[2] >> 1 & 1) ^ (nib[2] & 1);
    return (c4 << 4) | (c3 << 3) | (c2 << 2) | (c1 << 1) | c0;
}

std::vector<uint32_t> interleaveBlock(const std::vector<uint8_t>& cws,
                                      int sfApp, int cwLen, int sf, bool header)
{
    std::vector<uint32_t> out(static_cast<size_t>(cwLen), 0);
    for (int i = 0; i < cwLen; ++i) {
        uint32_t v = 0;
        for (int j = 0; j < sfApp; ++j) {
            const int r = mod(i - j - 1, sfApp);
            const int bit = (cws[static_cast<size_t>(r)] >> (cwLen - 1 - i)) & 1;
            v = (v << 1) | static_cast<uint32_t>(bit);
        }
        if (header) {
            const uint32_t parity = static_cast<uint32_t>(__builtin_popcount(v & ((1u << sfApp) - 1)) & 1);
            v = (v << (sf - sfApp)) | (parity << (sf - sfApp - 1));
        }
        out[static_cast<size_t>(i)] = v;
    }
    return out;
}

// Continuous-time LoRa chirp.  This works at any sample rate, including the
// Pluto's 3.84 MSPS, without requiring an integer samples-per-symbol ratio.
void appendChirp(std::vector<std::complex<double>>& out, uint32_t id,
                 int nSamples, double tStart, double Ts, double bw, double fs,
                 int N, bool down)
{
    const double Nf = static_cast<double>(N);
    const double tWrap = (1.0 - static_cast<double>(id) / Nf) * Ts;
    out.reserve(out.size() + static_cast<size_t>(std::max(0, nSamples)));
    for (int i = 0; i < nSamples; ++i) {
        const double t = tStart + static_cast<double>(i) / fs;
        double phase = 2.0 * kPi *
            (0.5 * (bw / Ts) * t * t + (static_cast<double>(id) / Nf - 0.5) * bw * t);
        if (t >= tWrap)
            phase -= 2.0 * kPi * bw * t;
        if (down) phase = -phase;
        out.emplace_back(std::cos(phase), std::sin(phase));
    }
}

} // namespace

std::vector<std::complex<double>> modulateLora(const std::vector<uint8_t>& payload,
                                               const LoraModParams& p,
                                               std::string* err)
{
    auto fail = [&](const char* msg) {
        if (err) *err = msg;
        return std::vector<std::complex<double>>{};
    };

    if (p.sf < 5 || p.sf > 12) return fail("SF must be 5..12");
    if (p.cr < 5 || p.cr > 8) return fail("CR must be 5..8 (4/5..4/8)");
    if (payload.empty() || payload.size() > 250) return fail("payload must be 1..250 bytes");
    if (p.bw <= 0.0 || p.fs <= 0.0) return fail("invalid bandwidth/sample rate");

    const int sf = p.sf;
    const int N = 1 << sf;
    const double Ts = static_cast<double>(N) / p.bw;
    const int crApp = p.cr - 4;
    const int cwLen = crApp + 4;

    // --- payload whitening -> nibbles (low nibble first) --------------------
    const auto& seq = whiteningSeq();
    std::vector<uint8_t> dataNibbles;
    dataNibbles.reserve(payload.size() * 2);
    for (size_t i = 0; i < payload.size(); ++i) {
        const uint8_t w = static_cast<uint8_t>(payload[i] ^ seq[i % seq.size()]);
        dataNibbles.push_back(w & 0x0F);
        dataNibbles.push_back((w >> 4) & 0x0F);
    }

    // --- explicit header ----------------------------------------------------
    int nib[5] = {};
    nib[0] = (static_cast<int>(payload.size()) >> 4) & 0x0F;
    nib[1] = static_cast<int>(payload.size()) & 0x0F;
    nib[2] = (crApp << 1) | (p.hasCrc ? 1 : 0);
    const int chk = headerChecksum(nib);
    nib[3] = (chk >> 4) & 1;
    nib[4] = chk & 0x0F;

    // The first SF-2 nibbles are the explicit header block.  For SF>7 this
    // also contains the first payload nibble(s), matching the gr-lora stream.
    std::vector<uint8_t> allNibbles;
    allNibbles.reserve(5 + dataNibbles.size() + 4);
    for (int i = 0; i < 5; ++i)
        allNibbles.push_back(static_cast<uint8_t>(nib[i]));
    allNibbles.insert(allNibbles.end(), dataNibbles.begin(), dataNibbles.end());

    // --- LoRa CRC (appended as 4 nibbles, low nibble first) -----------------
    if (p.hasCrc) {
        uint16_t crc = 0;
        for (size_t i = 0; i + 2 < payload.size(); ++i)
            crc = crc16(crc, payload[i]);
        crc ^= payload[payload.size() - 1];
        crc ^= static_cast<uint16_t>(payload[payload.size() - 2] << 8);
        allNibbles.push_back(static_cast<uint8_t>(crc & 0x0F));
        allNibbles.push_back(static_cast<uint8_t>((crc >> 4) & 0x0F));
        allNibbles.push_back(static_cast<uint8_t>((crc >> 8) & 0x0F));
        allNibbles.push_back(static_cast<uint8_t>((crc >> 12) & 0x0F));
    }

    const int headerCount = sf - 2;
    std::vector<uint8_t> headerCws;
    headerCws.reserve(static_cast<size_t>(headerCount));
    for (int i = 0; i < headerCount; ++i)
        headerCws.push_back(hammingEncode(allNibbles[static_cast<size_t>(i)], 4));

    // Hamming-encode every remaining nibble, pad to whole SF blocks.
    std::vector<uint8_t> dataCws;
    dataCws.reserve(allNibbles.size() - static_cast<size_t>(headerCount));
    for (size_t i = static_cast<size_t>(headerCount); i < allNibbles.size(); ++i)
        dataCws.push_back(hammingEncode(allNibbles[i], crApp));
    while (dataCws.size() % static_cast<size_t>(sf) != 0)
        dataCws.push_back(0);

    // --- interleave ---------------------------------------------------------
    std::vector<uint32_t> symbols;
    symbols.reserve(8 + dataCws.size() / static_cast<size_t>(sf) * static_cast<size_t>(cwLen));
    const auto headerSyms = interleaveBlock(headerCws, sf - 2, 8, sf, true);
    symbols.insert(symbols.end(), headerSyms.begin(), headerSyms.end());
    for (size_t off = 0; off < dataCws.size(); off += static_cast<size_t>(sf)) {
        std::vector<uint8_t> block(dataCws.begin() + static_cast<std::ptrdiff_t>(off),
                                   dataCws.begin() + static_cast<std::ptrdiff_t>(off + sf));
        const auto syms = interleaveBlock(block, sf, cwLen, sf, false);
        symbols.insert(symbols.end(), syms.begin(), syms.end());
    }

    // --- chirps -------------------------------------------------------------
    // Symbols are scheduled in continuous time.  A fractional samples-per-
    // symbol count is used, so the output is exact at 3.84 MSPS as well as at
    // 250 kSPS for offline loopback tests.
    std::vector<std::complex<double>> out;
    double t = 0.0;

    auto append = [&](uint32_t id, double duration, bool down) {
        const long long s0 = std::llround(t * p.fs);
        const long long s1 = std::llround((t + duration) * p.fs);
        const int n = std::max(1, static_cast<int>(s1 - s0));
        const double tStart = static_cast<double>(s0) / p.fs - t;
        appendChirp(out, id, n, tStart, Ts, p.bw, p.fs, N, down);
        t += duration;
    };

    for (int i = 0; i < p.preambleLen; ++i)
        append(0, Ts, false);

    const uint32_t sync1 = static_cast<uint32_t>(((p.syncWord >> 4) & 0x0F) << 3);
    const uint32_t sync2 = static_cast<uint32_t>((p.syncWord & 0x0F) << 3);
    append(sync1, Ts, false);
    append(sync2, Ts, false);

    // 2.25 down-chirps: two full symbols plus one quarter.
    append(0, Ts, true);
    append(0, Ts, true);
    append(0, Ts / 4.0, true);

    for (uint32_t v : symbols) {
        const uint32_t id = static_cast<uint32_t>(mod(grayDec(static_cast<int>(v)) + 1, N));
        append(id, Ts, false);
    }

    if (err) err->clear();
    return out;
}

std::vector<std::complex<double>> modulateLoraWithPad(const std::vector<uint8_t>& payload,
                                                      const LoraModParams& p,
                                                      int leadingSilence,
                                                      int trailingSilence,
                                                      std::string* err)
{
    auto iq = modulateLora(payload, p, err);
    if (iq.empty()) return iq;
    std::vector<std::complex<double>> out;
    out.reserve(static_cast<size_t>(std::max(0, leadingSilence)) + iq.size() +
                static_cast<size_t>(std::max(0, trailingSilence)));
    out.insert(out.end(), static_cast<size_t>(std::max(0, leadingSilence)), std::complex<double>(0.0, 0.0));
    out.insert(out.end(), iq.begin(), iq.end());
    out.insert(out.end(), static_cast<size_t>(std::max(0, trailingSilence)), std::complex<double>(0.0, 0.0));
    return out;
}

} // namespace mctrx
