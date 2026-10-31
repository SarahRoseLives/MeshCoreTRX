// Persistence test: write channels/messages, destroy the engine, reload, verify.
#include "app/engine.h"
#include "mesh/meshcore_build.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

int main()
{
    const std::string temp = "C:\\Users\\Rose\\AppData\\Local\\Temp\\opencode\\mctrx_persist_test";
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    std::filesystem::create_directories(temp, ec);
    _putenv_s("LOCALAPPDATA", temp.c_str());

    {
        mctrx::Engine a;
        mctrx::Identity id = mctrx::Identity::generate();
        a.setPersistence(true);
        a.init(id, "PersistNode", false, 0, 0, mctrx::Role::Companion);
        a.setTx([](std::vector<uint8_t>, int) {});
        const std::vector<uint8_t> key(16, 0x42);
        if (!a.addChannel("PersistTest", key)) {
            std::cerr << "FAIL: add channel\n";
            return 1;
        }
        a.sendGroupText(0, "persist me");
    }

    {
        mctrx::Engine b;
        mctrx::Identity id = mctrx::Identity::generate();
        b.setPersistence(true);
        b.init(id, "PersistNode", false, 0, 0, mctrx::Role::Companion);
        bool channel = false;
        for (const auto& c : b.channels())
            if (c.name == "PersistTest") channel = true;
        bool message = false;
        for (const auto& m : b.messages())
            if (m.text == "persist me") message = true;
        if (!channel || !message) {
            std::cerr << "FAIL: reload channels=" << channel << " messages=" << message << "\n";
            return 1;
        }
    }

    std::filesystem::remove_all(temp, ec);
    std::cout << "PASS: channels and messages persist across engine instances\n";
    return 0;
}
