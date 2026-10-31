#include "decimator.h"

#include <algorithm>
#include <cmath>

namespace mctrx {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

double sinc(double x)
{
    if (std::fabs(x) < 1e-12) return 1.0;
    return std::sin(kPi * x) / (kPi * x);
}

} // namespace

RationalDecimator::RationalDecimator(double inRate, double outRate, int taps)
    : inRate_(inRate), outRate_(outRate), taps_(taps)
{
    if (taps_ < 32) taps_ = 32;
    if (taps_ % 2) ++taps_;
    center_ = taps_ / 2 - 1;
    ratio_ = inRate_ / outRate_;

    // Pass the 62.5 kHz LoRa channel with margin, reject above the 125 kHz
    // output Nyquist.  fc is normalized to the input sample rate.
    const double fc = std::min(0.9 * outRate_ * 0.5, 45000.0) / inRate_;
    proto_.assign(static_cast<size_t>(taps_) * kPhases, 0.0f);

    for (int p = 0; p < kPhases; ++p) {
        const double frac = static_cast<double>(p) / kPhases;
        double sum = 0.0;
        std::vector<double> row(static_cast<size_t>(taps_));
        for (int j = 0; j < taps_; ++j) {
            const double d = static_cast<double>(j - center_) - frac;
            const double x = 2.0 * fc * sinc(2.0 * fc * d);
            const double w = 0.42 - 0.5 * std::cos(2.0 * kPi * j / (taps_ - 1)) +
                             0.08 * std::cos(4.0 * kPi * j / (taps_ - 1));
            row[static_cast<size_t>(j)] = x * w;
            sum += x * w;
        }
        if (std::fabs(sum) < 1e-12) sum = 1.0;
        for (int j = 0; j < taps_; ++j)
            proto_[static_cast<size_t>(p) + static_cast<size_t>(j) * kPhases] =
                static_cast<float>(row[static_cast<size_t>(j)] / sum);
    }
}

void RationalDecimator::reset()
{
    pending_.clear();
    baseAbs_ = 0;
    nextOutAbs_ = 0;
}

void RationalDecimator::process(const std::complex<double>* in, size_t count,
                                std::vector<std::complex<double>>& out)
{
    if (count == 0) return;
    pending_.insert(pending_.end(), in, in + count);

    const int half = center_;
    while (true) {
        const double pAbs = static_cast<double>(nextOutAbs_) * ratio_;
        const long long cAbs = static_cast<long long>(std::floor(pAbs));
        const long long needLast = cAbs + (taps_ - 1 - center_);
        if (needLast >= baseAbs_ + static_cast<long long>(pending_.size()))
            break;

        const long long startAbs = cAbs - center_;
        const long long firstNeeded = startAbs;
        if (firstNeeded < baseAbs_) {
            // Not enough history yet (startup); output zero and advance.
            out.emplace_back(0.0, 0.0);
            ++nextOutAbs_;
            continue;
        }
        const size_t off = static_cast<size_t>(firstNeeded - baseAbs_);
        const double frac = pAbs - static_cast<double>(cAbs);
        int phase = static_cast<int>(std::llround(frac * kPhases));
        if (phase >= kPhases) phase -= kPhases;
        if (phase < 0) phase += kPhases;
        const float* h = &proto_[static_cast<size_t>(phase)];

        double re = 0.0, im = 0.0;
        for (int j = 0; j < taps_; ++j) {
            const auto& x = pending_[off + static_cast<size_t>(j)];
            const double w = h[static_cast<size_t>(j) * kPhases];
            re += x.real() * w;
            im += x.imag() * w;
        }
        out.emplace_back(re, im);
        ++nextOutAbs_;
    }

    // Drop samples that can no longer be needed.
    const long long nextStart = static_cast<long long>(std::floor(static_cast<double>(nextOutAbs_) * ratio_)) - center_ - 4;
    if (nextStart > baseAbs_) {
        const long long drop = std::min<long long>(nextStart - baseAbs_,
                                                   static_cast<long long>(pending_.size()));
        if (drop > 0) {
            pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(drop));
            baseAbs_ += drop;
        }
    }
    (void)half;
}

} // namespace mctrx
