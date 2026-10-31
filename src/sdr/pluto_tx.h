#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

struct iio_context;
struct iio_device;
struct iio_channel;

namespace mctrx {

class PlutoTx {
public:
    PlutoTx() = default;
    ~PlutoTx();
    PlutoTx(const PlutoTx&) = delete;
    PlutoTx& operator=(const PlutoTx&) = delete;

    bool open(const std::string& uri, std::string& err);
    void close();

    bool configure(double centerHz, double sampleRate, double rfBandwidth,
                   double attenuationDb, std::string& err);

    bool transmit(const std::vector<std::complex<double>>& iq, std::string& err);

    const std::string& uri() const { return uri_; }

private:
    iio_context* ctx_ = nullptr;
    iio_device* phy_ = nullptr;
    iio_device* tx_ = nullptr;
    iio_channel* txI_ = nullptr;
    iio_channel* txQ_ = nullptr;
    iio_channel* txLo_ = nullptr;
    iio_channel* phyTx_ = nullptr;
    iio_channel* phyRx_ = nullptr;
    double sampleRate_ = 3840000.0;
    std::string uri_;
};

} // namespace mctrx
