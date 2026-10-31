// End-to-end RadioEngine test: TX a group packet, receive it through the
// full RX pipeline (Pluto -> decimator -> burst detector -> demod -> Engine).
#include "app/engine.h"
#include "app/radio.h"
#include "mesh/meshcore_build.h"

#include <chrono>
#include <ctime>
#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
    const std::string uri = argc > 1 ? argv[1] : "usb:1.8.5";
    const double freq = argc > 2 ? std::stod(argv[2]) : 910.525;
    const double rxGain = argc > 3 ? std::stod(argv[3]) : 40.0;
    const double txAtten = argc > 4 ? std::stod(argv[4]) : 0.0;

    mctrx::Identity id = mctrx::Identity::generate();
    mctrx::Engine engine;
    engine.setPersistence(false);
    engine.init(id, "RadioTest", false, 0.0, 0.0, mctrx::Role::Companion);

    mctrx::RadioEngine radio(engine);
    mctrx::PlutoRadioConfig cfg;
    cfg.centerHz = freq * 1e6;
    cfg.sampleRate = 3840000.0;
    cfg.rfBandwidth = 200000.0;
    cfg.rxGainDb = rxGain;
    cfg.txAttenDb = txAtten;

    mctrx::LoraModParams mod;
    mod.sf = 7;
    mod.bw = 62500.0;
    mod.fs = cfg.sampleRate;
    mod.cr = 8;
    mod.preambleLen = 32;

    std::string err;
    if (!radio.start(uri, cfg, mod, err)) {
        std::cerr << "start: " << err << "\n";
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    auto raw = mctrx::buildGroupText(
        std::vector<uint8_t>(mctrx::kPublicChannelKey, mctrx::kPublicChannelKey + 16),
        "RadioTest", "end to end radio engine test", static_cast<uint32_t>(std::time(nullptr)), 1);
    radio.enqueueTx(raw, 0);

    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto msgs = engine.messages();
        if (!msgs.empty()) {
            for (const auto& m : msgs)
                std::cout << "message: [" << m.channel << "] " << m.sender << ": " << m.text << "\n";
            const auto st = radio.status();
            std::cout << "decoded=" << st.decoded << " captures=" << st.captures
                      << " tx=" << st.tx << " noise=" << st.noiseFloor << "\n";
            radio.stop();
            return 0;
        }
    }
    const auto st = radio.status();
    std::cerr << "no message decoded; captures=" << st.captures << " decoded=" << st.decoded
              << " tx=" << st.tx << " err=" << st.lastError << "\n";
    radio.stop();
    return 2;
}
