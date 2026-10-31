#include "pluto_radio.h"

#include "pluto_context.h"

#include <iio.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mctrx {
namespace {

bool wch(iio_channel* ch, const char* name, const char* value)
{
    return ch && iio_channel_attr_write(ch, name, value) >= 0;
}

bool wdev(iio_device* dev, const char* name, const char* value)
{
    return dev && iio_device_attr_write(dev, name, value) >= 0;
}

} // namespace

PlutoRadio::~PlutoRadio() { close(); }

bool PlutoRadio::open(const std::string& uri, std::string& err)
{
    close();
    ctx_ = openPlutoContext(uri, uri_, err);
    if (!ctx_) return false;

    phy_ = iio_context_find_device(ctx_, "ad9361-phy");
    rx_ = iio_context_find_device(ctx_, "cf-ad9361-lpc");
    tx_ = iio_context_find_device(ctx_, "cf-ad9361-dds-core-lpc");
    if (!phy_ || !rx_ || !tx_) {
        err = "Pluto IIO devices not found";
        close();
        return false;
    }

    rxI_ = iio_device_find_channel(rx_, "voltage0", false);
    rxQ_ = iio_device_find_channel(rx_, "voltage1", false);
    txI_ = iio_device_find_channel(tx_, "voltage0", true);
    txQ_ = iio_device_find_channel(tx_, "voltage1", true);
    rxLo_ = iio_device_find_channel(phy_, "altvoltage0", true);
    txLo_ = iio_device_find_channel(phy_, "altvoltage1", true);
    phyRx_ = iio_device_find_channel(phy_, "voltage0", false);
    phyTx_ = iio_device_find_channel(phy_, "voltage0", true);
    if (!rxI_ || !rxQ_ || !txI_ || !txQ_ || !rxLo_ || !txLo_ || !phyRx_ || !phyTx_) {
        err = "Pluto RX/TX channels not found";
        close();
        return false;
    }
    iio_channel_enable(rxI_);
    iio_channel_enable(rxQ_);
    iio_channel_enable(txI_);
    iio_channel_enable(txQ_);
    return true;
}

void PlutoRadio::close()
{
    stopRx();
    if (ctx_) {
        iio_context_destroy(ctx_);
        ctx_ = nullptr;
    }
    phy_ = rx_ = tx_ = nullptr;
    rxI_ = rxQ_ = txI_ = txQ_ = rxLo_ = txLo_ = phyRx_ = phyTx_ = nullptr;
}

bool PlutoRadio::configure(const PlutoRadioConfig& cfg, std::string& err)
{
    if (!ctx_) {
        err = "Pluto not open";
        return false;
    }
    cfg_ = cfg;

    char buf[64];

    // The AD9361 baseband runs at 30.72 MSPS; the FPGA/DMA stream is
    // decimated/interpolated to 3.84 MSPS.
    if (!wch(phyRx_, "sampling_frequency", "30720000") ||
        !wch(phyTx_, "sampling_frequency", "30720000")) {
        err = "failed to set AD9361 baseband sample rate";
        return false;
    }

    std::snprintf(buf, sizeof(buf), "%.0f", cfg.sampleRate);
    if (!wch(rxI_, "sampling_frequency", buf) || !wch(rxQ_, "sampling_frequency", buf) ||
        !wch(txI_, "sampling_frequency", buf) || !wch(txQ_, "sampling_frequency", buf)) {
        err = "failed to set Pluto DMA sample rate";
        return false;
    }

    std::snprintf(buf, sizeof(buf), "%.0f", cfg.centerHz);
    if (!wch(rxLo_, "frequency", buf) || !wch(txLo_, "frequency", buf)) {
        err = "failed to set Pluto LO";
        return false;
    }

    std::snprintf(buf, sizeof(buf), "%.0f", std::max(200000.0, cfg.rfBandwidth));
    wch(phyRx_, "rf_bandwidth", buf);
    wch(phyTx_, "rf_bandwidth", buf);

    if (!wch(phyRx_, "gain_control_mode", "manual")) {
        err = "failed to set RX manual gain";
        return false;
    }
    std::snprintf(buf, sizeof(buf), "%.2f", cfg.rxGainDb);
    if (!wch(phyRx_, "hardwaregain", buf)) {
        err = "failed to set RX gain";
        return false;
    }
    if (!wch(phyRx_, "rf_port_select", cfg.rxPort.c_str())) {
        err = "failed to set RX port " + cfg.rxPort;
        return false;
    }

    std::snprintf(buf, sizeof(buf), "%.2f", -std::fabs(cfg.txAttenDb));
    if (!wch(phyTx_, "hardwaregain", buf)) {
        err = "failed to set TX attenuation";
        return false;
    }
    wch(phyTx_, "rf_port_select", "A");

    if (!wdev(phy_, "ensm_mode", "fdd")) {
        err = "failed to set Pluto FDD mode";
        return false;
    }

    return true;
}

bool PlutoRadio::setupRx(std::string& err)
{
    if (!rx_) {
        err = "RX device not open";
        return false;
    }
    if (rxBuf_) {
        iio_buffer_destroy(rxBuf_);
        rxBuf_ = nullptr;
    }
    decimator_.reset();
    rxBuf_ = iio_device_create_buffer(rx_, 65536, false);
    if (!rxBuf_) {
        err = "failed to create Pluto RX buffer";
        return false;
    }
    return true;
}

bool PlutoRadio::startRx(RxCallback cb, std::string& err)
{
    if (rxRunning_.load()) return true;
    if (!setupRx(err)) return false;
    rxCb_ = std::move(cb);
    rxRunning_.store(true);
    rxThread_ = std::thread([this] { rxLoop(); });
    return true;
}

void PlutoRadio::stopRx()
{
    if (!rxRunning_.exchange(false)) {
        if (rxThread_.joinable()) rxThread_.join();
        return;
    }
    if (rxBuf_) iio_buffer_cancel(rxBuf_);
    if (rxThread_.joinable()) rxThread_.join();
    if (rxBuf_) {
        iio_buffer_destroy(rxBuf_);
        rxBuf_ = nullptr;
    }
}

void PlutoRadio::rxLoop()
{
    std::vector<std::complex<double>> block;
    std::vector<std::complex<double>> decimated;
    block.reserve(65536);
    decimated.reserve(8192);

    while (rxRunning_.load()) {
        if (!rxBuf_) break;
        const ssize_t n = iio_buffer_refill(rxBuf_);
        if (n < 0) {
            if (!rxRunning_.load()) break;
            continue;
        }
        auto* p = static_cast<const int16_t*>(iio_buffer_start(rxBuf_));
        const size_t samples = static_cast<size_t>(n) / 4; // two int16 per IQ pair
        block.clear();
        for (size_t i = 0; i < samples; ++i) {
            const double re = static_cast<double>(p[2 * i]) / 2048.0;
            const double im = static_cast<double>(p[2 * i + 1]) / 2048.0;
            block.emplace_back(re, im);
        }
        decimated.clear();
        decimator_.process(block.data(), block.size(), decimated);
        if (!decimated.empty() && rxCb_) rxCb_(decimated);
    }
}

bool PlutoRadio::transmit(const std::vector<std::complex<double>>& iq, std::string& err)
{
    std::lock_guard<std::mutex> lk(txMu_);
    if (!tx_ || iq.empty()) {
        err = "empty TX buffer or Pluto not open";
        return false;
    }

    iio_buffer* buf = iio_device_create_buffer(tx_, iq.size(), true);
    if (!buf) {
        err = "failed to create Pluto TX buffer";
        return false;
    }
    auto* p = static_cast<int16_t*>(iio_buffer_start(buf));
    constexpr double scale = 0.55 * 32767.0;
    for (size_t i = 0; i < iq.size(); ++i) {
        const double re = std::max(-1.0, std::min(1.0, iq[i].real())) * scale;
        const double im = std::max(-1.0, std::min(1.0, iq[i].imag())) * scale;
        p[2 * i] = static_cast<int16_t>(std::llround(re));
        p[2 * i + 1] = static_cast<int16_t>(std::llround(im));
    }
    const ssize_t rc = iio_buffer_push(buf);
    if (rc < 0) {
        iio_buffer_destroy(buf);
        err = "Pluto TX push failed";
        return false;
    }
    const double seconds = static_cast<double>(iq.size()) / cfg_.sampleRate;
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds * 1.05));
    iio_buffer_cancel(buf);
    iio_buffer_destroy(buf);
    return true;
}

} // namespace mctrx
