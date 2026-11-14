#pragma once
#include <atomic>
#include <functional>
#include <string>
#include "config.h"

enum class State { Waiting, Streaming, Error };

using Report = std::function<void(State, const std::string&)>;

int runEngine(const Config& cfg, const Report& report, const std::atomic<bool>& stop);
