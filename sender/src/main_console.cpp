#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include "config.h"
#include "engine.h"

int main(int argc, char** argv) {
    Config cfg(argc > 1 ? std::filesystem::path(argv[1]) : defaultConfigPath());
    std::atomic<bool> stop{false};
    return runEngine(cfg, [](State, const std::string& m) {
        printf("\r%-78s", m.c_str());
        fflush(stdout);
    }, stop);
}
