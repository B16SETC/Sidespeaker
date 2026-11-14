#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Capture {
public:
    virtual ~Capture() = default;
    virtual bool open(std::string& err) = 0;
    virtual uint32_t sampleRate() const = 0;
    virtual int channels() const = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual int read(std::vector<int16_t>& out) = 0;
};

std::unique_ptr<Capture> makeCapture();
