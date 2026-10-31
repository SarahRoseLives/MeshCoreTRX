// MeshCoreTRX CLI - transmit a MeshCore packet to a PlutoSDR or IQ WAV.

#include "io/wav_writer.h"
#include "mesh/identity.h"
#include "mesh/meshcore_build.h"
#include "phy/lora_mod.h"
#include "sdr/pluto_tx.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string mode = "group";
    std::string text = "Hello from MeshCoreTRX";
    std::string name = "MeshCoreTRX";
    std::string uri;
    std::string wav;
    std::string identityPath;
    std::string channelKeyHex = "8b3387e9c5cdea6ac9e5edbaa115cd72";
    double freqMhz = 910.525;
    double bwKhz = 62.5;
    int sf = 7;
    int cr = 8;
    double attenDb = 10.0;
    double wavRate = 250000.0;
    double lat = 0.0;
    double lon = 0.0;
    bool hasLocation = false;
    bool advert = false;
    int repeat = 1;
    uint32_t timestamp = 0;
};

void usage()
{
    std::cout <<
        "MeshCoreTRX-cli - MeshCore transmitter\n\n"
        "  --mode group|advert       packet type (default: group)\n"
        "  --text TEXT               group message text\n"
        "  --name NAME               sender/node name (default: MeshCoreTRX)\n"
        "  --uri URI                 Pluto IIO URI, e.g. usb:1.8.5 or ip:192.168.2.1\n"
        "  --wav FILE                write IQ WAV instead of transmitting\n"
        "  --wav-rate HZ             WAV sample rate (default: 250000)\n"
        "  --freq MHZ                center frequency (default: 910.525)\n"
        "  --bw KHZ                  bandwidth (default: 62.5)\n"
        "  --sf N                    spreading factor (default: 7)\n"
        "  --cr N                    coding rate 5..8 (default: 8 = 4/8)\n"
        "  --atten DB                Pluto TX attenuation in dB (default: 10)\n"
        "  --identity FILE           identity file (default: LOCALAPPDATA\\MeshCoreTRX\\identity.txt)\n"
        "  --channel HEX             16/32-byte channel key (default: MeshCore public)\n"
        "  --lat DEG --lon DEG       include location in advert\n"
        "  --advert                  send an advert before the group message\n"
        "  --repeat N                transmit N times (default: 1)\n"
        "  --timestamp UNIX          override timestamp\n";
}

bool parse(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) return {};
            return argv[++i];
        };
        if (a == "--help" || a == "-h") { usage(); return false; }
        else if (a == "--mode") o.mode = next();
        else if (a == "--text") o.text = next();
        else if (a == "--name") o.name = next();
        else if (a == "--uri") o.uri = next();
        else if (a == "--wav") o.wav = next();
        else if (a == "--identity") o.identityPath = next();
        else if (a == "--channel") o.channelKeyHex = next();
        else if (a == "--freq") o.freqMhz = std::stod(next());
        else if (a == "--bw") o.bwKhz = std::stod(next());
        else if (a == "--sf") o.sf = std::stoi(next());
        else if (a == "--cr") o.cr = std::stoi(next());
        else if (a == "--atten") o.attenDb = std::stod(next());
        else if (a == "--wav-rate") o.wavRate = std::stod(next());
        else if (a == "--lat") { o.lat = std::stod(next()); o.hasLocation = true; }
        else if (a == "--lon") { o.lon = std::stod(next()); o.hasLocation = true; }
        else if (a == "--advert") o.advert = true;
        else if (a == "--repeat") o.repeat = std::max(1, std::stoi(next()));
        else if (a == "--timestamp") o.timestamp = static_cast<uint32_t>(std::stoul(next()));
        else {
            std::cerr << "unknown option: " << a << "\n";
            return false;
        }
    }
    return true;
}

std::string defaultIdentityPath()
{
    const char* la = std::getenv("LOCALAPPDATA");
    std::filesystem::path p = la ? std::filesystem::path(la) / "MeshCoreTRX" : ".";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return (p / "identity.txt").string();
}

mctrx::Identity loadIdentity(const std::string& path)
{
    mctrx::Identity id;
    if (!mctrx::Identity::load(path, id)) {
        id = mctrx::Identity::generate();
        id.save(path);
        std::cerr << "generated new identity " << id.pubHex() << " -> " << path << "\n";
    } else {
        std::cerr << "using identity " << id.pubHex() << "\n";
    }
    return id;
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    if (!parse(argc, argv, o)) return 1;
    if (o.uri.empty() && o.wav.empty()) {
        std::cerr << "specify --uri (Pluto) or --wav (offline IQ)\n";
        usage();
        return 1;
    }

    if (o.timestamp == 0)
        o.timestamp = static_cast<uint32_t>(std::time(nullptr));
    if (o.identityPath.empty())
        o.identityPath = defaultIdentityPath();
    const auto id = loadIdentity(o.identityPath);

    const double bw = o.bwKhz * 1000.0;
    const double fs = o.wav.empty() ? 3840000.0 : o.wavRate;

    mctrx::LoraModParams lp;
    lp.sf = o.sf;
    lp.bw = bw;
    lp.fs = fs;
    lp.cr = o.cr;
    lp.hasCrc = true;
    lp.preambleLen = (o.sf <= 8) ? 32 : 16;
    lp.syncWord = 0x12;

    auto makeGroup = [&]() {
        return mctrx::buildGroupText(mctrx::hexToBytes(o.channelKeyHex), o.name, o.text,
                                     o.timestamp, 1);
    };
    auto makeAdvert = [&]() {
        return mctrx::buildAdvert(id, o.name, o.hasLocation, o.lat, o.lon, o.timestamp, 1);
    };

    if (o.mode == "advert")
        o.advert = false;

    std::vector<std::vector<uint8_t>> packets;
    if (o.advert)
        packets.push_back(makeAdvert());
    if (o.mode == "group")
        packets.push_back(makeGroup());
    else if (o.mode == "advert" && packets.empty())
        packets.push_back(makeAdvert());

    if (packets.empty()) {
        std::cerr << "nothing to transmit\n";
        return 1;
    }

    if (!o.wav.empty()) {
        std::vector<std::complex<double>> all;
        const int lead = static_cast<int>(fs * 0.05);
        const int tail = static_cast<int>(fs * 0.05);
        for (const auto& pkt : packets) {
            std::string err;
            auto iq = mctrx::modulateLoraWithPad(pkt, lp, lead, tail, &err);
            if (iq.empty()) {
                std::cerr << "modulation failed: " << err << "\n";
                return 1;
            }
            all.insert(all.end(), iq.begin(), iq.end());
            std::cerr << "packet " << pkt.size() << " bytes -> " << iq.size()
                      << " samples  raw=" << mctrx::bytesToHex(pkt) << "\n";
        }
        std::string err;
        if (!mctrx::writeIqWav(o.wav, all, fs, 0.7, &err)) {
            std::cerr << "WAV write failed: " << err << "\n";
            return 1;
        }
        std::cerr << "wrote " << o.wav << " (" << all.size() << " samples @ " << fs << ")\n";
        return 0;
    }

    mctrx::PlutoTx tx;
    std::string err;
    if (!tx.open(o.uri, err)) {
        std::cerr << err << "\n";
        return 1;
    }
    if (!tx.configure(o.freqMhz * 1e6, fs, 200000.0, o.attenDb, err)) {
        std::cerr << err << "\n";
        return 1;
    }

    for (int rep = 0; rep < o.repeat; ++rep) {
        for (const auto& pkt : packets) {
            std::string e;
            auto iq = mctrx::modulateLoraWithPad(pkt, lp, static_cast<int>(fs * 0.01),
                                                 static_cast<int>(fs * 0.02), &e);
            if (iq.empty()) {
                std::cerr << "modulation failed: " << e << "\n";
                return 1;
            }
            std::cerr << "TX " << pkt.size() << " bytes, " << iq.size() << " samples, raw="
                      << mctrx::bytesToHex(pkt) << "\n";
            if (!tx.transmit(iq, e)) {
                std::cerr << "TX failed: " << e << "\n";
                return 1;
            }
        }
    }
    std::cerr << "transmitted on " << o.freqMhz << " MHz SF" << o.sf << " BW" << o.bwKhz
              << " CR" << o.cr << "\n";
    return 0;
}
