// Full-duplex loopback check: receive our own transmitted packet on the Pluto.
#include "app/engine.h"
#include "mesh/identity.h"
#include "mesh/meshcore.h"
#include "mesh/meshcore_build.h"
#include "phy/lora_demod.h"
#include "phy/lora_mod.h"
#include "sdr/pluto_radio.h"

#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv)
{
    const std::string uri = argc > 1 ? argv[1] : "usb:1.8.5";
    const double freq = argc > 2 ? std::stod(argv[2]) : 910.525;
    const double rxGain = argc > 3 ? std::stod(argv[3]) : 10.0;
    const double txAtten = argc > 4 ? std::stod(argv[4]) : 30.0;

    mctrx::PlutoRadio radio;
    std::string err;
    if (!radio.open(uri, err)) {
        std::cerr << err << "\n";
        return 1;
    }
    mctrx::PlutoRadioConfig cfg;
    cfg.centerHz = freq * 1e6;
    cfg.sampleRate = 3840000.0;
    cfg.rfBandwidth = 200000.0;
    cfg.rxGainDb = rxGain;
    cfg.txAttenDb = txAtten;
    cfg.rxPort = "A_BALANCED";
    if (!radio.configure(cfg, err)) {
        std::cerr << err << "\n";
        return 1;
    }

    std::mutex mu;
    std::vector<std::complex<double>> captured;
    captured.reserve(250000 * 4);
    if (!radio.startRx([&](const std::vector<std::complex<double>>& block) {
            std::lock_guard<std::mutex> lk(mu);
            if (captured.size() < 250000 * 4)
                captured.insert(captured.end(), block.begin(), block.end());
        }, err)) {
        std::cerr << err << "\n";
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    mctrx::Identity id = mctrx::Identity::generate();
    const auto raw = mctrx::buildGroupText(
        std::vector<uint8_t>(mctrx::kPublicChannelKey, mctrx::kPublicChannelKey + 16),
        "Loopback", "full duplex self test", static_cast<uint32_t>(std::time(nullptr)), 1);

    mctrx::LoraModParams lp;
    lp.sf = 7;
    lp.bw = 62500.0;
    lp.fs = 3840000.0;
    lp.cr = 8;
    lp.preambleLen = 32;
    auto iq = mctrx::modulateLoraWithPad(raw, lp, 38400, 76800, &err);
    if (iq.empty()) {
        std::cerr << "mod failed: " << err << "\n";
        return 1;
    }
    std::cerr << "transmitting " << raw.size() << " bytes / " << iq.size() << " samples...\n";
    if (!radio.transmit(iq, err)) {
        std::cerr << err << "\n";
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    radio.stopRx();

    std::vector<std::complex<double>> buf;
    {
        std::lock_guard<std::mutex> lk(mu);
        buf = captured;
    }
    std::cerr << "captured " << buf.size() << " samples @ 250k\n";

    mc::LoraParams p;
    p.sf = 7;
    p.bw = 62500.0;
    p.fs = 250000.0;
    if (!mc::initLora(p, err)) {
        std::cerr << err << "\n";
        return 1;
    }
    mc::LoraPacket pkt;
    std::string log;
    if (!mc::decodeLora(buf, p, pkt, log, true, 8)) {
        std::cerr << "decode failed: " << log << "\n";
        return 2;
    }
    const int n = pkt.header.payloadLen;
    std::vector<uint8_t> mesh(pkt.data.begin(), pkt.data.begin() + n);
    std::cerr << "decoded " << n << " bytes: " << mctrx::bytesToHex(mesh) << "\n";

    mctrx::Engine engine;
    engine.setPersistence(false);
    engine.init(id, "Loopback", false, 0.0, 0.0, mctrx::Role::Companion);
    engine.handleRaw(mesh, 0.0f, pkt.quality);
    for (const auto& m : engine.messages()) {
        std::cerr << "engine message: channel=" << m.channel << " sender=" << m.sender
                  << " text=\"" << m.text << "\"\n";
    }

    mc::MeshCorePacket mp;
    if (mc::parseMeshCore(mesh, mp, err) && mp.type == 0x05) {
        std::vector<uint8_t> plain;
        bool macOk = false;
        uint8_t expected = 0;
        if (mc::decryptGroupPayload(mp, std::vector<uint8_t>(mctrx::kPublicChannelKey,
                                                            mctrx::kPublicChannelKey + 16),
                                    plain, macOk, expected)) {
            uint32_t ts = 0;
            uint8_t flags = 0;
            std::string text;
            mc::parseGroupText(plain, ts, flags, text);
            std::cerr << "MAC=" << (macOk ? "valid" : "bad") << " text=\"" << text << "\"\n";
            return 0;
        }
    }
    return 3;
}
