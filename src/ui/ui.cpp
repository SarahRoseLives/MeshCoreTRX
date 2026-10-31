#include "ui.h"

#include "mesh/meshcore.h"
#include "mesh/meshcore_build.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace mctrx {
namespace {

std::string fmtTime(double t)
{
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

std::string pubHex(const ContactInfo& c) { return bytesToHex(c.pub.data(), c.pub.size()); }

std::string nodeTypeName(int t) { return mc::meshcoreNodeTypeName(t); }

bool parseD(const char* s, double& out)
{
    if (!s || !*s) return false;
    char* end = nullptr;
    out = std::strtod(s, &end);
    return end && end != s;
}

void clearBuf(char* b, size_t n) { std::memset(b, 0, n); }

} // namespace

bool startRadio(UiState& ui, std::string& err)
{
    if (!ui.engine || !ui.radio) {
        err = "engine not initialized";
        return false;
    }
    ui.uri = ui.uriBuf;
    PlutoRadioConfig cfg;
    cfg.centerHz = ui.freqMhz * 1e6;
    cfg.sampleRate = 3840000.0;
    cfg.rfBandwidth = 200000.0;
    cfg.rxGainDb = ui.rxGain;
    cfg.txAttenDb = ui.txAtten;
    LoraModParams mod;
    mod.sf = ui.sf;
    mod.bw = ui.bwKhz * 1000.0;
    mod.fs = cfg.sampleRate;
    mod.cr = ui.cr;
    mod.preambleLen = (ui.sf <= 8) ? 32 : 16;
    if (!ui.radio->start(ui.uri, cfg, mod, err)) return false;
    double lat = 0, lon = 0;
    const bool hasLoc = parseD(ui.latBuf, lat) && parseD(ui.lonBuf, lon);
    ui.engine->setRole(ui.role);
    ui.engine->setNodeName(ui.nameBuf);
    ui.engine->setLocation(hasLoc, lat, lon);
    ui.started = true;
    return true;
}

void stopRadio(UiState& ui)
{
    if (ui.radio) ui.radio->stop();
    ui.started = false;
}

void drawUi(UiState& ui)
{
    if (!ui.engine || !ui.radio) return;
    ui.radio->setTxEnabled(ui.txEnable);
    if (ui.lastSave == 0.0) ui.lastSave = ImGui::GetTime();
    if (ImGui::GetTime() - ui.lastSave >= 5.0) {
        ui.engine->saveIfDirty();
        ui.lastSave = ImGui::GetTime();
    }
    if (ui.autoStart && !ui.started) {
        ui.autoStart = false;
        std::string err;
        if (startRadio(ui, err)) ui.message = "Radio started";
        else ui.message = "Start failed: " + err;
    }
    if (ui.started && !ui.autoSendDone && (ui.autoAdvert || !ui.autoSend.empty())) {
        if (ui.autoAdvert) ui.engine->sendAdvert(true);
        if (!ui.autoSend.empty()) ui.engine->sendGroupText(0, ui.autoSend);
        ui.autoSendDone = true;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("MeshCoreTRX##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

    // ---- header -----------------------------------------------------------
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 5));
    ImGui::TextUnformatted("MeshCoreTRX");
    ImGui::SameLine();
    if (!ui.started) {
        if (ImGui::Button("Start radio")) {
            std::string err;
            if (startRadio(ui, err)) ui.message = "Radio started";
            else ui.message = "Start failed: " + err;
        }
    } else {
        if (ImGui::Button("Stop radio")) {
            stopRadio(ui);
            ui.message = "Radio stopped";
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    const char* roles[] = {"Companion", "Repeater"};
    int roleIdx = static_cast<int>(ui.role);
    if (ImGui::Combo("##role", &roleIdx, roles, 2)) {
        ui.role = static_cast<Role>(roleIdx);
        ui.engine->setRole(ui.role);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%.3f MHz  BW %.1f kHz  SF%d  CR%d", ui.freqMhz, ui.bwKhz, ui.sf, ui.cr);
    ImGui::SameLine();
    ImGui::Checkbox("TX enable", &ui.txEnable);
    ImGui::SameLine();
    if (ImGui::Button("Send advert")) {
        if (ui.started && ui.txEnable) {
            ui.engine->sendAdvert(true);
            ui.message = "Advert queued";
        }
    }
    ImGui::PopStyleVar();

    ImGui::Separator();

    // ---- sidebar ----------------------------------------------------------
    const float sideW = 190.0f;
    ImGui::BeginChild("##side", ImVec2(sideW, 0), ImGuiChildFlags_None);
    {
        auto nav = [&](const char* label, int id) {
            if (ImGui::Selectable(label, ui.tab == id, 0, ImVec2(0, 34))) ui.tab = id;
        };
        nav("Dashboard", 0);
        nav("Chat", 1);
        nav("Contacts", 2);
        nav("Packets", 3);
        nav("Settings", 4);
        ImGui::Separator();
        ImGui::TextWrapped("Node");
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1.0f), "%s", ui.nameBuf);
        ImGui::TextDisabled("%s", ui.identity ? ui.identity->pubHex().substr(0, 12).c_str() : "");
        ImGui::TextDisabled(ui.role == Role::Repeater ? "repeater mode" : "companion mode");
        const auto st = ui.radio->status();
        ImGui::TextDisabled("RX %llu / TX %llu", (unsigned long long)st.decoded,
                            (unsigned long long)st.tx);
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_None);
    const auto stats = ui.engine->stats();
    const auto contacts = ui.engine->contacts();
    const auto messages = ui.engine->messages();
    const auto channels = ui.engine->channels();
    const auto packets = ui.engine->packets();
    const auto logs = ui.engine->logs();
    const auto st = ui.radio->status();

    if (ui.tab == 0) {
        ImGui::TextUnformatted("Dashboard");
        ImGui::Separator();
        if (ImGui::BeginTable("stats", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableNextColumn(); ImGui::Text("RX packets"); ImGui::Text("%llu", (unsigned long long)stats.rxPackets);
            ImGui::TableNextColumn(); ImGui::Text("TX packets"); ImGui::Text("%llu", (unsigned long long)stats.txPackets);
            ImGui::TableNextColumn(); ImGui::Text("Decoded"); ImGui::Text("%llu", (unsigned long long)st.decoded);
            ImGui::TableNextColumn(); ImGui::Text("Forwarded"); ImGui::Text("%llu", (unsigned long long)stats.forwarded);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("Group"); ImGui::Text("%llu", (unsigned long long)stats.rxGroup);
            ImGui::TableNextColumn(); ImGui::Text("Direct"); ImGui::Text("%llu", (unsigned long long)stats.rxDirect);
            ImGui::TableNextColumn(); ImGui::Text("Adverts"); ImGui::Text("%llu", (unsigned long long)stats.rxAdvert);
            ImGui::TableNextColumn(); ImGui::Text("Noise"); ImGui::Text("%.4f", st.noiseFloor);
            ImGui::EndTable();
        }
        ImGui::Spacing();
        if (!st.lastError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", st.lastError.c_str());
        if (!ui.message.empty()) ImGui::TextColored(ImVec4(0.5f, 0.8f, 1, 1), "%s", ui.message.c_str());
        ImGui::Separator();
        ImGui::TextUnformatted("Recent events");
        ImGui::BeginChild("##logs", ImVec2(0, 180), ImGuiChildFlags_Borders);
        const int first = std::max(0, static_cast<int>(logs.size()) - 40);
        for (int i = first; i < static_cast<int>(logs.size()); ++i)
            ImGui::TextWrapped("%s", logs[i].c_str());
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::TextUnformatted("Recent packets");
        ImGui::BeginChild("##dashpkts", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const int pfirst = std::max(0, static_cast<int>(packets.size()) - 30);
        for (int i = static_cast<int>(packets.size()) - 1; i >= pfirst; --i) {
            const auto& p = packets[i];
            ImGui::Text("%s  %-10s %-18s hops=%d  q=%.2f", fmtTime(p.seenAt).c_str(),
                        mc::meshcorePayloadName(p.type), mc::meshcoreRouteName(p.route),
                        p.hops, p.quality);
        }
        ImGui::EndChild();
    } else if (ui.tab == 1) {
        const bool enteredChat = (ui.lastMainTab != 1);
        const int priorChatTab = ui.lastChatTab;
        ImGui::TextUnformatted("Chat");
        ImGui::Separator();
        if (ImGui::BeginTabBar("##chatTabs")) {
            if (ImGui::BeginTabItem("Channels")) {
                ui.chatTab = 0;
                if (!channels.empty()) {
                    std::vector<const char*> names;
                    for (auto& c : channels) names.push_back(c.name.c_str());
                    ImGui::SetNextItemWidth(220);
                    ImGui::Combo("Channel", &ui.channelIdx, names.data(), static_cast<int>(names.size()));
                }
                const std::string active = channels.empty() ? "" : channels[std::max(0, std::min<int>(ui.channelIdx, static_cast<int>(channels.size()) - 1))].name;
                ImGui::BeginChild("##msgs", ImVec2(0, -44), ImGuiChildFlags_Borders);
                for (const auto& m : messages) {
                    if (m.direct || m.channel != active) continue;
                    ImGui::TextColored(m.outgoing ? ImVec4(0.5f, 0.8f, 1, 1) : ImVec4(0.6f, 1, 0.7f, 1),
                                       "[%s] %s:", fmtTime(m.seenAt).c_str(), m.sender.c_str());
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", m.text.c_str());
                }
                if (ui.followChat) ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                if (enteredChat || priorChatTab != 0 || ui.focusChatNext) {
                    ImGui::SetKeyboardFocusHere();
                    ui.focusChatNext = false;
                }
                ImGui::SetNextItemWidth(-110);
                bool enter = ImGui::InputText("##chat", ui.chatBuf, sizeof(ui.chatBuf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::Button("Send", ImVec2(90, 0)) || enter) && ui.started && ui.txEnable) {
                    if (ui.chatBuf[0]) {
                        ui.engine->sendGroupText(ui.channelIdx, ui.chatBuf);
                        clearBuf(ui.chatBuf, sizeof(ui.chatBuf));
                        ui.focusChatNext = true;
                        ui.message = "Group message queued";
                    }
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Direct")) {
                ui.chatTab = 1;
                if (!contacts.empty()) {
                    std::string preview = ui.contactIdx >= 0 && ui.contactIdx < static_cast<int>(contacts.size())
                                              ? contacts[ui.contactIdx].name
                                              : "select contact";
                    if (ImGui::BeginCombo("Contact", preview.c_str())) {
                        for (int i = 0; i < static_cast<int>(contacts.size()); ++i) {
                            (void)pubHex(contacts[i]);
                            if (ImGui::Selectable((contacts[i].name + "##" + std::to_string(i)).c_str(),
                                                  ui.contactIdx == i))
                                ui.contactIdx = i;
                        }
                        ImGui::EndCombo();
                    }
                } else {
                    ImGui::TextDisabled("No contacts yet. Wait for an advert.");
                }
                std::string peer;
                if (ui.contactIdx >= 0 && ui.contactIdx < static_cast<int>(contacts.size()))
                    peer = pubHex(contacts[ui.contactIdx]);
                ImGui::BeginChild("##dmsgs", ImVec2(0, -44), ImGuiChildFlags_Borders);
                for (const auto& m : messages) {
                    if (!m.direct) continue;
                    if (!peer.empty() && m.peerPubHex != peer) continue;
                    ImGui::TextColored(m.outgoing ? ImVec4(0.5f, 0.8f, 1, 1) : ImVec4(0.6f, 1, 0.7f, 1),
                                       "[%s] %s%s:", fmtTime(m.seenAt).c_str(), m.sender.c_str(),
                                       m.outgoing ? (m.acked ? " (acked)" : " (pending)") : "");
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s", m.text.c_str());
                }
                if (ui.followChat) ImGui::SetScrollHereY(1.0f);
                ImGui::EndChild();
                if (enteredChat || priorChatTab != 1 || ui.focusChatNext) {
                    ImGui::SetKeyboardFocusHere();
                    ui.focusChatNext = false;
                }
                ImGui::SetNextItemWidth(-110);
                bool enter = ImGui::InputText("##dm", ui.directBuf, sizeof(ui.directBuf),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::Button("Send DM", ImVec2(90, 0)) || enter) && ui.started && ui.txEnable &&
                    !peer.empty()) {
                    if (ui.directBuf[0]) {
                        if (ui.engine->sendDirectText(peer, ui.directBuf)) {
                            clearBuf(ui.directBuf, sizeof(ui.directBuf));
                            ui.focusChatNext = true;
                            ui.message = "Direct message queued";
                        } else {
                            ui.message = "Direct send failed";
                        }
                    }
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ui.lastChatTab = ui.chatTab;
    } else if (ui.tab == 2) {
        ImGui::TextUnformatted("Contacts / nodes");
        ImGui::Separator();
        if (ImGui::BeginTable("contacts", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Pubkey");
            ImGui::TableSetupColumn("Last seen");
            ImGui::TableSetupColumn("Path");
            ImGui::TableHeadersRow();
            for (int i = static_cast<int>(contacts.size()) - 1; i >= 0; --i) {
                const auto& c = contacts[i];
                const bool selected = ui.contactIdx == i;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable((c.name + "##c" + std::to_string(i)).c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns))
                    ui.contactIdx = i;
                ImGui::TableNextColumn(); ImGui::Text("%s", nodeTypeName(c.type).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s", pubHex(c).substr(0, 16).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s", fmtTime(c.lastSeen).c_str());
                ImGui::TableNextColumn(); ImGui::Text("%zu hops", c.outPath.size());
            }
            ImGui::EndTable();
        }
        if (ui.contactIdx >= 0 && ui.contactIdx < static_cast<int>(contacts.size())) {
            const auto& c = contacts[ui.contactIdx];
            ImGui::Separator();
            ImGui::Text("%s  %s", c.name.c_str(), nodeTypeName(c.type).c_str());
            ImGui::TextWrapped("pubkey: %s", pubHex(c).c_str());
            if (c.hasLocation) ImGui::Text("location: %.6f, %.6f", c.lat, c.lon);
            ImGui::Text("advert ts: %u  path bytes: %zu", c.lastAdvertTs, c.outPath.size());
        }
    } else if (ui.tab == 3) {
        ImGui::TextUnformatted("Packet log");
        ImGui::Separator();
        if (ImGui::BeginTable("packets", 6,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                              ImVec2(0, -140))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Time");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Route");
            ImGui::TableSetupColumn("Hops");
            ImGui::TableSetupColumn("Quality");
            ImGui::TableSetupColumn("Summary");
            ImGui::TableHeadersRow();
            for (int i = static_cast<int>(packets.size()) - 1; i >= 0; --i) {
                const auto& p = packets[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable((fmtTime(p.seenAt) + "##p" + std::to_string(i)).c_str(),
                                      ui.selectedPacket == i))
                    ui.selectedPacket = i;
                ImGui::TableNextColumn(); ImGui::Text("%s", mc::meshcorePayloadName(p.type));
                ImGui::TableNextColumn(); ImGui::Text("%s", mc::meshcoreRouteName(p.route));
                ImGui::TableNextColumn(); ImGui::Text("%d", p.hops);
                ImGui::TableNextColumn(); ImGui::Text("%.2f", p.quality);
                ImGui::TableNextColumn(); ImGui::Text("%s", p.summary.c_str());
            }
            ImGui::EndTable();
        }
        if (ui.selectedPacket >= 0 && ui.selectedPacket < static_cast<int>(packets.size())) {
            const auto& p = packets[ui.selectedPacket];
            ImGui::Separator();
            ImGui::TextWrapped("raw: %s", bytesToHex(p.raw).c_str());
        }
    } else if (ui.tab == 4) {
        ImGui::TextUnformatted("Settings");
        ImGui::Separator();
        ImGui::TextDisabled("Changes to radio parameters take effect on the next Start.");
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("Pluto URI", ui.uriBuf, sizeof(ui.uriBuf));
        ImGui::SetNextItemWidth(160); ImGui::InputDouble("Frequency MHz", &ui.freqMhz, 0.001, 0.1, "%.6f");
        ImGui::SameLine(); ImGui::SetNextItemWidth(120);
        ImGui::InputDouble("Bandwidth kHz", &ui.bwKhz, 0.1, 1.0, "%.1f");
        ImGui::SetNextItemWidth(100); ImGui::InputInt("SF", &ui.sf);
        ImGui::SameLine(); ImGui::SetNextItemWidth(100); ImGui::InputInt("CR", &ui.cr);
        ImGui::SetNextItemWidth(160); ImGui::InputDouble("TX attenuation dB", &ui.txAtten, 1, 5, "%.1f");
        ImGui::SameLine(); ImGui::SetNextItemWidth(160);
        ImGui::InputDouble("RX gain dB", &ui.rxGain, 1, 5, "%.1f");
        ImGui::SetNextItemWidth(240); ImGui::InputText("Node name", ui.nameBuf, sizeof(ui.nameBuf));
        ImGui::SetNextItemWidth(140); ImGui::InputText("Latitude", ui.latBuf, sizeof(ui.latBuf));
        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
        ImGui::InputText("Longitude", ui.lonBuf, sizeof(ui.lonBuf));
        ImGui::Spacing();
        if (ImGui::Button("Apply node settings")) {
            double lat = 0, lon = 0;
            const bool hasLoc = parseD(ui.latBuf, lat) && parseD(ui.lonBuf, lon);
            ui.uri = ui.uriBuf;
            ui.engine->setNodeName(ui.nameBuf);
            ui.engine->setLocation(hasLoc, lat, lon);
            ui.engine->setRole(ui.role);
            ui.message = "Node settings applied";
        }
        ImGui::SameLine();
        if (ImGui::Button("Send flood advert") && ui.started && ui.txEnable) {
            ui.engine->sendAdvert(true);
            ui.message = "Flood advert queued";
        }
        ImGui::SameLine();
        if (ImGui::Button("Send zero-hop advert") && ui.started && ui.txEnable) {
            ui.engine->sendAdvert(false);
            ui.message = "Zero-hop advert queued";
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Channels");
        for (int i = 0; i < static_cast<int>(channels.size()); ++i) {
            bool en = channels[i].enabled;
            if (ImGui::Checkbox(("##ch" + std::to_string(i)).c_str(), &en))
                ui.engine->setChannelEnabled(i, en);
            ImGui::SameLine();
            ImGui::Text("%-16s hash=0x%02X", channels[i].name.c_str(), channels[i].hash);
        }
        ImGui::SetNextItemWidth(180);
        ImGui::InputText("Channel name", ui.chanName, sizeof(ui.chanName));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(280);
        ImGui::InputText("Key hex", ui.chanKey, sizeof(ui.chanKey),
                         ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (ImGui::Button("Add channel")) {
            const auto key = hexToBytes(ui.chanKey);
            if ((key.size() == 16 || key.size() == 32) &&
                ui.engine->addChannel(ui.chanName, key)) {
                ui.message = "Channel added";
                clearBuf(ui.chanKey, sizeof(ui.chanKey));
            } else {
                ui.message = "Channel key must be 16 or 32 bytes of hex";
            }
        }
        ImGui::TextDisabled("Identity: %s", ui.identity ? ui.identity->pubHex().c_str() : "");
        ImGui::TextDisabled("Radio: %.6f MHz, BW %.1f kHz, SF%d, CR%d, Pluto 3.84 MSPS -> 250 kSPS",
                            ui.freqMhz, ui.bwKhz, ui.sf, ui.cr);
    }

    ImGui::EndChild();

    ImGui::Separator();
    ImGui::TextDisabled("RX %llu  TX %llu  captures %llu  noise %.5f  %s",
                        (unsigned long long)stats.rxPackets,
                        (unsigned long long)stats.txPackets,
                        (unsigned long long)st.captures, st.noiseFloor,
                        st.rxRunning ? "RX running" : (ui.started ? "RX stopped" : "idle"));
    ImGui::SameLine();
    if (!st.lastError.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", st.lastError.c_str());

    ui.lastMainTab = ui.tab;
    ImGui::End();
}

} // namespace mctrx
