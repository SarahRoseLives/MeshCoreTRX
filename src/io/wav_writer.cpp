#include "wav_writer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace mctrx {
namespace {

void put16(std::ofstream& f, uint16_t v)
{
    char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
    f.write(b, 2);
}

void put32(std::ofstream& f, uint32_t v)
{
    char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                 static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
    f.write(b, 4);
}

} // namespace

bool writeIqWav(const std::string& path,
                const std::vector<std::complex<double>>& iq,
                double sampleRate,
                double amplitude,
                std::string* err)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        if (err) *err = "cannot open " + path;
        return false;
    }

    const uint16_t channels = 2;
    const uint16_t bits = 16;
    const uint32_t rate = static_cast<uint32_t>(std::llround(sampleRate));
    const uint32_t dataBytes = static_cast<uint32_t>(iq.size() * 4);
    const uint32_t riffSize = 36 + dataBytes;

    f.write("RIFF", 4);
    put32(f, riffSize);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put32(f, 16);
    put16(f, 1); // PCM
    put16(f, channels);
    put32(f, rate);
    put32(f, rate * channels * bits / 8);
    put16(f, channels * bits / 8);
    put16(f, bits);
    f.write("data", 4);
    put32(f, dataBytes);

    const double scale = std::max(0.0, std::min(1.0, amplitude)) * 32767.0;
    std::vector<int16_t> pcm(iq.size() * 2);
    for (size_t i = 0; i < iq.size(); ++i) {
        const double re = std::max(-1.0, std::min(1.0, iq[i].real())) * scale;
        const double im = std::max(-1.0, std::min(1.0, iq[i].imag())) * scale;
        pcm[2 * i] = static_cast<int16_t>(std::llround(re));
        pcm[2 * i + 1] = static_cast<int16_t>(std::llround(im));
    }
    f.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size() * 2));
    if (!f) {
        if (err) *err = "write failed";
        return false;
    }
    return true;
}

} // namespace mctrx
