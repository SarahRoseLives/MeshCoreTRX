#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace mctrx {

bool writeIqWav(const std::string& path,
                const std::vector<std::complex<double>>& iq,
                double sampleRate,
                double amplitude = 0.7,
                std::string* err = nullptr);

} // namespace mctrx
