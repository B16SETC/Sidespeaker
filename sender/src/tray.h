#pragma once
#include <functional>
#include <string>

enum class TrayState { Idle, Active, Error };

void setStatus(TrayState state, const std::string& text);
int runTray(const std::string& configPath, std::function<void()> worker);
