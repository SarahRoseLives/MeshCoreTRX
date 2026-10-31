#pragma once

#include "decimator.h"

#include <atomic>
#include <complex>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct iio_context;
struct iio_device;
struct iio_channel;
struct iio_buffer;

namespace mctrx {

struct PlutoRadioConfig {
    double centerHz = 910525000.0;
    double sampleRate = 3840000.0;
    double rfBandwidth = 200000.0;
    double rxGainDb = 35.0;
    double txAttenDb = 10.0;
    bool fullDuplex = true;
    std::string rxPort = "A_BALANCED"; // use TX_MONITOR1 for internal TX loopback
};

class PlutoRadio {
public:
    using RxCallback = std::function<void(const std::vector<std::complex<double>>&)>;

    PlutoRadio() = default;
    ~PlutoRadio();
    PlutoRadio(const PlutoRadio&) = delete;
    PlutoRadio& operator=(const PlutoRadio&) = delete;

    bool open(const std::string& uri, std::string& err);
    void close();

    bool configure(const PlutoRadioConfig& cfg, std::string& err);
    bool startRx(RxCallback cb, std::string& err);
    void stopRx();

    bool transmit(const std::vector<std::complex<double>>& iq, std::string& err);

    bool isOpen() const { return ctx_ != nullptr; }
    bool isRxRunning() const { return rxRunning_.load(); }
    const PlutoRadioConfig& config() const { return cfg_; }

private:
    void rxLoop();
    bool setupRx(std::string& err);

    iio_context* ctx_ = nullptr;
    iio_device* phy_ = nullptr;
    iio_device* rx_ = nullptr;
    iio_device* tx_ = nullptr;

    iio_channel* rxI_ = nullptr;
    iio_channel* rxQ_ = nullptr;
    iio_channel* txI_ = nullptr;
    iio_channel* txQ_ = nullptr;
    iio_channel* rxLo_ = nullptr;
    iio_channel* txLo_ = nullptr;
    iio_channel* phyRx_ = nullptr;
    iio_channel* phyTx_ = nullptr;
    iio_buffer* rxBuf_ = nullptr;

    std::thread rxThread_;
    std::atomic<bool> rxRunning_{false};
    RxCallback rxCb_;

    std::mutex txMu_;
    PlutoRadioConfig cfg_;
    RationalDecimator decimator_{3840000.0, 250000.0};
    std::string uri_;
};

} // namespace mctrx
