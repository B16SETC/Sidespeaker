#include "net.h"
#include <string>
#include <windows.h>
#include "config.h"
#include "hub.h"
#include "tray.h"

namespace {

std::string defaultConfigPath() {
    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p, n);
    size_t k = s.find_last_of("\\/");
    if (n > 0 && k != std::string::npos) return s.substr(0, k + 1) + "config.ini";
    return "config.ini";
}

}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();
    std::string path = __argc > 1 ? __argv[1] : defaultConfigPath();
    return runTray(path, [path] {
        Config cfg(path);
        hubRun(cfg);
    });
}
