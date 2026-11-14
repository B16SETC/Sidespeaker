#pragma once
#include <memory>
#include <string>
#include "config.h"
#include "net.h"

class Transport {
public:
    virtual ~Transport() = default;
    virtual const char* name() const = 0;
    virtual sock_t connect(std::string& why) = 0;
};

std::unique_ptr<Transport> makeUsbmuxd(const Config& cfg);
std::unique_ptr<Transport> makeAdb(const Config& cfg);
