// MeshCoreTRX - Dear ImGui transceiver dashboard.
#include "app/engine.h"
#include "app/radio.h"
#include "mesh/identity.h"
#include "mesh/meshcore_build.h"
#include "ui/ui.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <cstdlib>

namespace {

std::string identityPath()
{
    const char* la = std::getenv("LOCALAPPDATA");
    std::filesystem::path p = la ? std::filesystem::path(la) / "MeshCoreTRX" : ".";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return (p / "identity.txt").string();
}

mctrx::Identity loadIdentity()
{
    const std::string path = identityPath();
    mctrx::Identity id;
    if (mctrx::Identity::load(path, id)) return id;
    id = mctrx::Identity::generate();
    id.save(path);
    return id;
}

std::string configPath()
{
    const char* la = std::getenv("LOCALAPPDATA");
    std::filesystem::path p = la ? std::filesystem::path(la) / "MeshCoreTRX" : ".";
    return (p / "config.json").string();
}

void loadConfig(mctrx::UiState& ui)
{
    std::ifstream f(configPath());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if (k == "uri") ui.uri = v;
        else if (k == "freq_mhz") ui.freqMhz = std::atof(v.c_str());
        else if (k == "bw_khz") ui.bwKhz = std::atof(v.c_str());
        else if (k == "sf") ui.sf = std::atoi(v.c_str());
        else if (k == "cr") ui.cr = std::atoi(v.c_str());
        else if (k == "tx_atten") ui.txAtten = std::atof(v.c_str());
        else if (k == "rx_gain") ui.rxGain = std::atof(v.c_str());
        else if (k == "tx_enable") ui.txEnable = v != "0";
        else if (k == "role") ui.role = v == "repeater" ? mctrx::Role::Repeater : mctrx::Role::Companion;
        else if (k == "name") std::snprintf(ui.nameBuf, sizeof(ui.nameBuf), "%s", v.c_str());
        else if (k == "lat") std::snprintf(ui.latBuf, sizeof(ui.latBuf), "%s", v.c_str());
        else if (k == "lon") std::snprintf(ui.lonBuf, sizeof(ui.lonBuf), "%s", v.c_str());
        else if (k == "channel" && ui.engine) {
            const size_t comma = v.find(',');
            if (comma != std::string::npos) {
                const std::string name = v.substr(0, comma);
                const std::string hex = v.substr(comma + 1);
                const auto key = mctrx::hexToBytes(hex);
                if (key.size() == 16 || key.size() == 32)
                    ui.engine->addChannel(name, key);
            }
        }
    }
    std::snprintf(ui.uriBuf, sizeof(ui.uriBuf), "%s", ui.uri.c_str());
}

void saveConfig(const mctrx::UiState& ui)
{
    std::ofstream f(configPath());
    if (!f) return;
    f << "uri=" << ui.uri << "\n";
    f << "freq_mhz=" << ui.freqMhz << "\n";
    f << "bw_khz=" << ui.bwKhz << "\n";
    f << "sf=" << ui.sf << "\n";
    f << "cr=" << ui.cr << "\n";
    f << "tx_atten=" << ui.txAtten << "\n";
    f << "rx_gain=" << ui.rxGain << "\n";
    f << "tx_enable=" << (ui.txEnable ? 1 : 0) << "\n";
    f << "role=" << (ui.role == mctrx::Role::Repeater ? "repeater" : "companion") << "\n";
    f << "name=" << ui.nameBuf << "\n";
    f << "lat=" << ui.latBuf << "\n";
    f << "lon=" << ui.lonBuf << "\n";
    if (ui.engine) {
        for (const auto& c : ui.engine->channels()) {
            if (c.name == "Public") continue;
            f << "channel=" << c.name << ","
              << mctrx::bytesToHex(c.key.data(), c.key.size()) << "\n";
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    glfwSetErrorCallback([](int code, const char* desc) {
        std::fprintf(stderr, "glfw error %d: %s\n", code, desc);
    });
    if (!glfwInit()) {
        std::fprintf(stderr, "failed to init GLFW\n");
        return 1;
    }
    const char* glsl = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1400, 880, "MeshCoreTRX", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 4.0f;
    style.FrameRounding = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.085f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.18f, 0.32f, 0.45f, 1.0f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.30f, 0.40f, 1.0f);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl);

    mctrx::Identity id = loadIdentity();
    mctrx::Engine engine;
    engine.init(id, "MeshCoreTRX", false, 0.0, 0.0, mctrx::Role::Companion);
    mctrx::RadioEngine radio(engine);

    mctrx::UiState ui;
    ui.engine = &engine;
    ui.radio = &radio;
    ui.identity = &id;
    std::snprintf(ui.nameBuf, sizeof(ui.nameBuf), "%s", "MeshCoreTRX");
    loadConfig(ui);

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--start") ui.autoStart = true;
        else if (a == "--uri") {
            ui.uri = next();
            std::snprintf(ui.uriBuf, sizeof(ui.uriBuf), "%s", ui.uri.c_str());
        } else if (a == "--freq") ui.freqMhz = std::stod(next());
        else if (a == "--bw") ui.bwKhz = std::stod(next());
        else if (a == "--sf") ui.sf = std::stoi(next());
        else if (a == "--cr") ui.cr = std::stoi(next());
        else if (a == "--atten") ui.txAtten = std::stod(next());
        else if (a == "--rxgain") ui.rxGain = std::stod(next());
        else if (a == "--exit-after") ui.exitAfter = std::stod(next());
        else if (a == "--role") ui.role = next() == "repeater" ? mctrx::Role::Repeater : mctrx::Role::Companion;
        else if (a == "--name") std::snprintf(ui.nameBuf, sizeof(ui.nameBuf), "%s", next().c_str());
        else if (a == "--lat") std::snprintf(ui.latBuf, sizeof(ui.latBuf), "%s", next().c_str());
        else if (a == "--lon") std::snprintf(ui.lonBuf, sizeof(ui.lonBuf), "%s", next().c_str());
        else if (a == "--send") { ui.autoSend = next(); ui.autoStart = true; }
        else if (a == "--advert") { ui.autoAdvert = true; }
    }

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (ui.exitAfter > 0.0 && glfwGetTime() >= ui.exitAfter)
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        mctrx::drawUi(ui);

        ImGui::Render();
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.04f, 0.05f, 0.06f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    saveConfig(ui);
    radio.stop();
    engine.save();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif
