#include "radio.h"

#include "mesh/meshcore.h"
#include "mesh/meshcore_build.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace mctrx {
namespace {

double nowSec()
{
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

double median(std::deque<double> v)
{
    if (v.empty()) return 1e-6;
    std::sort(v.begin(), v.end());
    return std::max(1e-6, v[v.size() / 2]);
}

} // namespace

RadioEngine::~RadioEngine() { stop(); }

bool RadioEngine::start(const std::string& uri, const PlutoRadioConfig& cfg,
                        const LoraModParams& mod, std::string& err)
{
    stop();
    mod_ = mod;
    mod_.fs = cfg.sampleRate;

    if (!radio_.open(uri, err)) return false;
    if (!radio_.configure(cfg, err)) return false;

    decimator_ = std::make_unique<RationalDecimator>(cfg.sampleRate, 250000.0);
    demod_ = mc::LoraParams{};
    demod_.sf = mod.sf;
    demod_.bw = mod.bw;
    demod_.fs = 250000.0;
    if (!mc::initLora(demod_, err)) {
        radio_.close();
        return false;
    }

    preRoll_.clear();
    capture_.clear();
    noiseHistory_.clear();
    active_ = false;
    quietBlocks_ = 0;
    noiseFloor_ = 1e-6;
    preRollMax_ = 25000;

    {
        std::lock_guard<std::mutex> lk(statusMu_);
        status_ = RadioStatus{};
        status_.running = true;
    }

    engine_.setTx([this](std::vector<uint8_t> raw, int delayMs) {
        enqueueTx(std::move(raw), delayMs);
    });

    running_.store(true);
    txThread_ = std::thread([this] { txLoop(); });

    if (!radio_.startRx([this](const std::vector<std::complex<double>>& b) { onRx(b); }, err)) {
        stop();
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.running = true;
        status_.rxRunning = true;
    }
    return true;
}

void RadioEngine::stop()
{
    if (!running_.exchange(false)) {
        if (txThread_.joinable()) txThread_.join();
        radio_.stopRx();
        radio_.close();
        return;
    }
    txCv_.notify_all();
    if (txThread_.joinable()) txThread_.join();
    radio_.stopRx();
    radio_.close();
    {
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.running = false;
        status_.rxRunning = false;
    }
}

void RadioEngine::enqueueTx(std::vector<uint8_t> raw, int delayMs)
{
    if (!running_.load() || !txEnabled_.load()) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        txQueue_.push_back(PendingTx{std::move(raw), nowSec() + delayMs / 1000.0});
    }
    txCv_.notify_one();
}

void RadioEngine::txLoop()
{
    while (running_.load()) {
        PendingTx item;
        {
            std::unique_lock<std::mutex> lk(mu_);
            txCv_.wait_for(lk, std::chrono::milliseconds(100), [this] {
                return !running_.load() || !txQueue_.empty();
            });
            if (!running_.load()) break;
            if (txQueue_.empty()) continue;
            item = std::move(txQueue_.front());
            txQueue_.pop_front();
            const double due = item.due;
            while (running_.load() && nowSec() < due) {
                txCv_.wait_for(lk, std::chrono::milliseconds(5));
            }
            if (!running_.load()) break;
        }
        std::string err;
        auto iq = modulateLoraWithPad(item.raw, mod_,
                                      static_cast<int>(mod_.fs * 0.01),
                                      static_cast<int>(mod_.fs * 0.02), &err);
        if (iq.empty()) {
            setError("modulate: " + err);
            continue;
        }
        if (!radio_.transmit(iq, err)) {
            setError("TX: " + err);
            continue;
        }
        std::lock_guard<std::mutex> lk(statusMu_);
        ++status_.tx;
    }
}

void RadioEngine::onRx(const std::vector<std::complex<double>>& block)
{
    if (block.empty()) return;

    double energy = 0.0;
    for (const auto& x : block) energy += std::norm(x);
    const double level = std::sqrt(energy / static_cast<double>(block.size()));

    noiseHistory_.push_back(level);
    while (noiseHistory_.size() > 200) noiseHistory_.pop_front();
    noiseFloor_ = median(noiseHistory_);

    {
        std::lock_guard<std::mutex> lk(statusMu_);
        status_.noiseFloor = noiseFloor_;
    }

    if (!active_) {
        preRoll_.insert(preRoll_.end(), block.begin(), block.end());
        if (preRoll_.size() > preRollMax_)
            preRoll_.erase(preRoll_.begin(),
                           preRoll_.begin() + static_cast<std::ptrdiff_t>(preRoll_.size() - preRollMax_));
        if (noiseHistory_.size() > 30 && level > noiseFloor_ * 1.7) {
            active_ = true;
            quietBlocks_ = 0;
            capture_ = preRoll_;
            capture_.insert(capture_.end(), block.begin(), block.end());
            preRoll_.clear();
        }
    } else {
        capture_.insert(capture_.end(), block.begin(), block.end());
        if (level < noiseFloor_ * 1.35) ++quietBlocks_;
        else quietBlocks_ = 0;
        if (quietBlocks_ >= 12 || capture_.size() > 500000) {
            decodeCapture();
            active_ = false;
            capture_.clear();
            quietBlocks_ = 0;
        }
    }
}

void RadioEngine::decodeCapture()
{
    mc::LoraPacket pkt;
    std::string why;
    const bool ok = mc::decodeLora(capture_, demod_, pkt, why, false, 8);
    {
        std::lock_guard<std::mutex> lk(statusMu_);
        ++status_.captures;
        status_.lastError = ok ? std::string() : why;
    }
    if (!ok) return;

    const int n = pkt.header.payloadLen;
    if (n <= 0 || static_cast<size_t>(n) > pkt.data.size()) return;
    std::vector<uint8_t> raw(pkt.data.begin(), pkt.data.begin() + n);

    engine_.handleRaw(raw, 0.0f, pkt.quality);

    {
        std::lock_guard<std::mutex> lk(statusMu_);
        ++status_.decoded;
        status_.lastPacket = bytesToHex(raw.data(), std::min<size_t>(raw.size(), 16)) + "...";
    }
}

void RadioEngine::setError(const std::string& e)
{
    std::lock_guard<std::mutex> lk(statusMu_);
    status_.lastError = e;
}

RadioStatus RadioEngine::status() const
{
    std::lock_guard<std::mutex> lk(statusMu_);
    RadioStatus s = status_;
    s.running = running_.load();
    s.rxRunning = radio_.isRxRunning();
    return s;
}

} // namespace mctrx
