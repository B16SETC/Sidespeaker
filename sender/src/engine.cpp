#include "net.h"
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif
#include "capture.h"
#include "engine.h"
#include "transport.h"

namespace {

using Clock = std::chrono::steady_clock;

enum class End { Disconnected, CaptureError, Stopped };

struct Platform {
    Platform() {
#ifdef _WIN32
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        timeBeginPeriod(1);
#endif
    }
    ~Platform() {
#ifdef _WIN32
        timeEndPeriod(1);
        WSACleanup();
#endif
    }
};

void sleepMs(int ms, const std::atomic<bool>& stop) {
    auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (!stop && Clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(ms, 25)));
}

End stream(sock_t s, Capture& cap, const std::string& label, int pollMs, int fillMs, const Report& report, const std::atomic<bool>& stop) {
    uint32_t rate = cap.sampleRate();
    int ch = cap.channels();
    uint32_t hdr[2] = { rate, (uint32_t)ch };
    if (!sendAll(s, hdr, 8)) return End::Disconnected;
    if (!cap.start()) return End::CaptureError;
    report(State::Streaming, "Streaming to " + label + " (" + std::to_string(rate) + " Hz)");

    auto t0 = Clock::now();
    long long sent = 0;
    long long fillFrames = (long long)rate * fillMs / 1000;
    auto nowFrames = [&]() -> long long {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
        return us * (long long)rate / 1000000;
    };

    std::vector<int16_t> buf;
    End end = End::Stopped;
    while (!stop) {
        int n = cap.read(buf);
        if (n < 0) {
            end = End::CaptureError;
            break;
        }
        if (n == 0) {
            long long deficit = nowFrames() - sent;
            if (deficit >= fillFrames) {
                long long fr = std::min<long long>(deficit, rate / 5);
                buf.assign((size_t)fr * ch, 0);
                if (!sendAll(s, buf.data(), (int)(buf.size() * 2))) {
                    end = End::Disconnected;
                    break;
                }
                sent += fr;
            }
            sleepMs(pollMs, stop);
            continue;
        }
        if (!sendAll(s, buf.data(), (int)(buf.size() * 2))) {
            end = End::Disconnected;
            break;
        }
        sent += n;
        if (sent - nowFrames() > (long long)rate / 5) {
            t0 = Clock::now();
            sent = 0;
        }
    }
    cap.stop();
    return end;
}

}

int runEngine(const Config& cfg, const Report& report, const std::atomic<bool>& stop) {
    Platform platform;
    int pollMs = cfg.getInt("poll_ms", 5);
    int retryMs = cfg.getInt("retry_ms", 1000);
    int fillMs = cfg.getInt("fill_ms", 10);
    std::string mode = cfg.getString("transport", "auto");

    std::vector<std::unique_ptr<Transport>> transports;
    if (mode == "auto" || mode == "ios") transports.push_back(makeUsbmuxd(cfg));
    if (mode == "auto" || mode == "android") transports.push_back(makeAdb(cfg));
    if (transports.empty()) {
        report(State::Error, "Unknown transport '" + mode + "' (use auto, ios or android)");
        return 1;
    }

    std::string waiting = mode == "ios"     ? "Waiting for iPhone (cable, unlocked, trusted)"
                        : mode == "android" ? "Waiting for Android (cable, USB debugging allowed)"
                                            : "Waiting for iPhone or Android (cable, unlocked)";

    std::unique_ptr<Capture> cap;
    while (!stop) {
        if (!cap) {
            cap = makeCapture();
            std::string err;
            if (!cap->open(err)) {
                report(State::Error, "Audio capture failed: " + err);
                cap.reset();
                sleepMs(retryMs, stop);
                continue;
            }
        }

        sock_t s = kBadSock;
        std::string via;
        std::string note;
        for (auto& t : transports) {
            std::string why;
            s = t->connect(why);
            if (s != kBadSock) {
                via = t->name();
                break;
            }
            if (note.empty()) note = why;
        }
        if (s == kBadSock) {
            report(State::Waiting, note.empty() ? waiting : note);
            sleepMs(retryMs, stop);
            continue;
        }

        int nd = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof nd);
        End e = stream(s, *cap, via, pollMs, fillMs, report, stop);
        closeSock(s);
        if (e == End::CaptureError) {
            cap.reset();
            report(State::Waiting, "Audio device changed, reopening capture");
        } else if (e == End::Disconnected) {
            report(State::Waiting, "Disconnected");
        }
        if (e != End::Stopped) sleepMs(retryMs, stop);
    }
    return 0;
}
