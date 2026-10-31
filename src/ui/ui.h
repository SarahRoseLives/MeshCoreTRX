#pragma once

#include "app/engine.h"
#include "app/radio.h"
#include "mesh/identity.h"

#include <string>

namespace mctrx {

struct UiState {
    Engine* engine = nullptr;
    RadioEngine* radio = nullptr;
    Identity* identity = nullptr;

    std::string uri = "usb:1.8.5";
    double freqMhz = 910.525;
    double bwKhz = 62.5;
    int sf = 7;
    int cr = 8;
    double txAtten = 0.0;
    double rxGain = 35.0;
    bool txEnable = true;
    Role role = Role::Companion;
    bool started = false;
    bool autoStart = false;
    double exitAfter = 0.0;
    bool autoAdvert = false;
    bool autoSendDone = false;
    std::string autoSend;

    char uriBuf[128] = "usb:1.8.5";
    char nameBuf[64] = "MeshCoreTRX";
    char latBuf[32] = "";
    char lonBuf[32] = "";
    char chatBuf[512] = "";
    char directBuf[512] = "";
    char chanName[64] = "Private";
    char chanKey[128] = "";

    int tab = 0;          // 0 dashboard, 1 chat, 2 contacts, 3 packets, 4 settings
    int chatTab = 0;      // 0 channels, 1 direct
    int channelIdx = 0;
    int contactIdx = -1;
    int selectedPacket = -1;
    bool followChat = true;
    bool focusChatNext = true;
    int lastMainTab = -1;
    int lastChatTab = -1;
    double lastSave = 0.0;
    std::string message;
};

bool startRadio(UiState& ui, std::string& err);
void stopRadio(UiState& ui);
void drawUi(UiState& ui);

} // namespace mctrx
