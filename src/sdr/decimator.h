#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace mctrx {

// Fractional-rate low-pass decimator for the Pluto RX stream.
// Default: 3.84 MSPS -> 250 kSPS (ratio 384/25).
class RationalDecimator {
public:
    RationalDecimator(double inRate, double outRate, int taps = 256);

    void reset();
    void process(const std::complex<double>* in, size_t count,
                 std::vector<std::complex<double>>& out);

    double inRate() const { return inRate_; }
    double outRate() const { return outRate_; }

private:
    static constexpr int kPhases = 1024;
    double inRate_ = 0.0;
    double outRate_ = 0.0;
    double ratio_ = 0.0; // input samples per output sample
    int taps_ = 256;
    int center_ = 127;
    std::vector<float> proto_; // taps_ * kPhases
    std::vector<std::complex<double>> pending_;
    long long baseAbs_ = 0;
    long long nextOutAbs_ = 0;
};

} // namespace mctrx
