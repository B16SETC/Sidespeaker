#pragma once
#include <string>
#include <vector>
#include "config.h"

enum class Phase { Unavailable, Idle, Waiting, Connecting, Streaming };

struct DeviceView {
    std::string id;
    std::string kind;
    std::string name;
    std::string status;
    Phase phase = Phase::Idle;
    bool selected = false;
    double x = 0;
    double y = 0;
};

struct HubView {
    std::vector<DeviceView> devices;
    bool surround = false;
    std::string note;
};

HubView hubView();
void hubSelect(const std::string& id, bool on);
void hubSurround(bool on);
void hubMove(const std::string& id, double x, double y);
void hubBeep(const std::string& id);
void hubSave();
void hubRun(const Config& cfg);
