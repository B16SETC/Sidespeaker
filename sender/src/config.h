#pragma once
#include <filesystem>
#include <map>
#include <string>

class Config {
public:
    explicit Config(const std::filesystem::path& path);
    int getInt(const std::string& key, int def) const;
    std::string getString(const std::string& key, const std::string& def) const;

private:
    std::map<std::string, std::string> m_;
};

std::filesystem::path defaultConfigPath();
