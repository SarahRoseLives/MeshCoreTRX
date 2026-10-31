#pragma once

#include "engine.h"
#include "phy/lora_demod.h"
#include "phy/lora_mod.h"
#include "sdr/pluto_radio.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mctrx {

struct RadioStatus {
    bool running = false;
    bool rxRunning = false;
    std::string lastError;
    double noiseFloor = 0.0;
    uint64_t captures = 0;
    uint64_t decoded = 0;
    uint64_t tx = 0;
    double lastSnr = 0.0;
    std::string lastPacket;
};

class RadioEngine {
public:
    explicit RadioEngine(Engine& engine) : engine_(engine) {}
    ~RadioEngine();

    bool start(const std::string& uri, const PlutoRadioConfig& cfg,
               const LoraModParams& mod, std::string& err);
    void stop();
    bool running() const { return running_.load(); }

    // Called by Engine; queues a raw MeshCore packet for transmission.
    void enqueueTx(std::vector<uint8_t> raw, int delayMs);
    void setTxEnabled(bool en) { txEnabled_.store(en); }

    RadioStatus status() const;

private:
    struct PendingTx {
        std::vector<uint8_t> raw;
        double due = 0.0;
    };

    void txLoop();
    void onRx(const std::vector<std::complex<double>>& block);
    void decodeCapture();
    void setError(const std::string& e);

    Engine& engine_;
    PlutoRadio radio_;
    LoraModParams mod_;
    mutable std::mutex mu_;
    mutable std::mutex statusMu_;
    RadioStatus status_;

    std::atomic<bool> running_{false};
    std::atomic<bool> txEnabled_{true};
    std::thread txThread_;
    std::condition_variable txCv_;
    std::deque<PendingTx> txQueue_;

    std::unique_ptr<RationalDecimator> decimator_;
    mc::LoraParams demod_;

    // Burst detector state (only touched by the RX callback thread).
    std::vector<std::complex<double>> preRoll_;
    std::vector<std::complex<double>> capture_;
    std::deque<double> noiseHistory_;
    bool active_ = false;
    int quietBlocks_ = 0;
    double noiseFloor_ = 1e-6;
    size_t preRollMax_ = 25000; // 0.1 s at 250 kSPS
};

} // namespace mctrx
