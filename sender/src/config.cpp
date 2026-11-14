#include "config.h"
#include <cstdlib>
#include <fstream>

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

Config::Config(const std::string& path) {
    std::ifstream f(path);
    std::string l;
    while (std::getline(f, l)) {
        l = trim(l);
        if (l.empty() || l[0] == '#') continue;
        size_t p = l.find('=');
        if (p == std::string::npos) continue;
        m_[trim(l.substr(0, p))] = trim(l.substr(p + 1));
    }
}

int Config::getInt(const std::string& key, int def) const {
    auto it = m_.find(key);
    return it == m_.end() || it->second.empty() ? def : atoi(it->second.c_str());
}

std::string Config::getString(const std::string& key, const std::string& def) const {
    auto it = m_.find(key);
    return it == m_.end() || it->second.empty() ? def : it->second;
}
