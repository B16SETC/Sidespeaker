#pragma once
#include <memory>
#include <string>
#include <vector>
#include "config.h"
#include "net.h"

struct Found {
    std::string id;
    std::string kind;
    std::string name;
    std::string serial;
    int state = 2;
    int muxId = -1;
};

class Transport {
public:
    virtual ~Transport() = default;
    virtual const char* kind() const = 0;
    virtual void scan(std::vector<Found>& out, std::string& note) = 0;
    virtual sock_t connect(const Found& f, std::string& why) = 0;
};

std::unique_ptr<Transport> makeUsbmuxd(const Config& cfg);
std::unique_ptr<Transport> makeAdb(const Config& cfg);
