#include "pluto_tx.h"
#include "pluto_context.h"

#include <iio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace mctrx {
namespace {

bool writeAttr(iio_channel* ch, const char* name, const char* value)
{
    if (!ch) return false;
    return iio_channel_attr_write(ch, name, value) >= 0;
}

bool writeAttr(iio_device* dev, const char* name, const char* value)
{
    if (!dev) return false;
    return iio_device_attr_write(dev, name, value) >= 0;
}

std::string attr(iio_channel* ch, const char* name)
{
    if (!ch) return {};
    char buf[128] = {};
    const ssize_t n = iio_channel_attr_read(ch, name, buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

} // namespace

PlutoTx::~PlutoTx() { close(); }

bool PlutoTx::open(const std::string& uri, std::string& err)
{
    close();
    ctx_ = openPlutoContext(uri, uri_, err);
    if (!ctx_) return false;

    phy_ = iio_context_find_device(ctx_, "ad9361-phy");
    tx_ = iio_context_find_device(ctx_, "cf-ad9361-dds-core-lpc");
    if (!phy_ || !tx_) {
        err = "Pluto IIO devices not found";
        close();
        return false;
    }

    txI_ = iio_device_find_channel(tx_, "voltage0", true);
    txQ_ = iio_device_find_channel(tx_, "voltage1", true);
    txLo_ = iio_device_find_channel(phy_, "altvoltage1", true);
    phyTx_ = iio_device_find_channel(phy_, "voltage0", true);
    phyRx_ = iio_device_find_channel(phy_, "voltage0", false);
    if (!txI_ || !txQ_ || !txLo_ || !phyTx_) {
        err = "Pluto TX channels not found";
        close();
        return false;
    }

    iio_channel_enable(txI_);
    iio_channel_enable(txQ_);
    return true;
}

void PlutoTx::close()
{
    if (ctx_) {
        iio_context_destroy(ctx_);
        ctx_ = nullptr;
    }
    phy_ = nullptr;
    tx_ = nullptr;
    txI_ = txQ_ = txLo_ = phyTx_ = phyRx_ = nullptr;
}

bool PlutoTx::configure(double centerHz, double sampleRate, double rfBandwidth,
                        double attenuationDb, std::string& err)
{
    if (!ctx_ || !phy_ || !tx_) {
        err = "Pluto TX not open";
        return false;
    }

    sampleRate_ = sampleRate;
    char buf[64];

    // AD9361 baseband at 30.72 MSPS; DMA at 3.84 MSPS.
    writeAttr(phyTx_, "sampling_frequency", "30720000");
    if (phyRx_) writeAttr(phyRx_, "sampling_frequency", "30720000");

    std::snprintf(buf, sizeof(buf), "%.0f", sampleRate);
    if (!writeAttr(txI_, "sampling_frequency", buf) ||
        !writeAttr(txQ_, "sampling_frequency", buf)) {
        err = "failed to set TX sample rate";
        return false;
    }

    std::snprintf(buf, sizeof(buf), "%.0f", centerHz);
    if (!writeAttr(txLo_, "frequency", buf)) {
        err = "failed to set TX LO frequency";
        return false;
    }

    const double bw = std::max(200000.0, std::min(2000000.0, rfBandwidth));
    std::snprintf(buf, sizeof(buf), "%.0f", bw);
    writeAttr(phyTx_, "rf_bandwidth", buf);

    // hardwaregain is attenuation in dB on the TX path.
    std::snprintf(buf, sizeof(buf), "%.2f", -std::fabs(attenuationDb));
    if (!writeAttr(phyTx_, "hardwaregain", buf)) {
        err = "failed to set TX attenuation";
        return false;
    }
    writeAttr(phyTx_, "rf_port_select", "A");

    if (!writeAttr(phy_, "ensm_mode", "fdd")) {
        err = "failed to set Pluto FDD mode";
        return false;
    }

    return true;
}

bool PlutoTx::transmit(const std::vector<std::complex<double>>& iq, std::string& err)
{
    if (!tx_ || iq.empty()) {
        err = "empty TX buffer or Pluto not open";
        return false;
    }

    const size_t n = iq.size();
    // Cyclic TX keeps the AD9361 transmit path keyed for the whole buffer,
    // which is more reliable than a single non-cyclic push.
    iio_buffer* buf = iio_device_create_buffer(tx_, n, true);
    if (!buf) {
        err = "failed to create Pluto TX buffer";
        return false;
    }

    auto* p = static_cast<int16_t*>(iio_buffer_start(buf));
    constexpr double scale = 0.55 * 32767.0;
    for (size_t i = 0; i < n; ++i) {
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
    const double seconds = static_cast<double>(n) / sampleRate_;
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds * 1.05));
    iio_buffer_cancel(buf);
    iio_buffer_destroy(buf);
    return true;
}

} // namespace mctrx
