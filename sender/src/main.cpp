#include "net.h"
#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
#include <mmsystem.h>
#include "capture.h"
#include "config.h"
#include "transport.h"
#include "tray.h"

namespace {

using Clock = std::chrono::steady_clock;

enum class End { Disconnected, CaptureError };

std::string defaultConfigPath() {
    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p, n);
    size_t k = s.find_last_of("\\/");
    if (n > 0 && k != std::string::npos) return s.substr(0, k + 1) + "config.ini";
    return "config.ini";
}

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

End stream(sock_t s, Capture& cap, const std::string& label, int pollMs, int fillMs) {
    uint32_t rate = cap.sampleRate();
    int ch = cap.channels();
    uint32_t hdr[2] = { rate, (uint32_t)ch };
    if (!sendAll(s, hdr, 8)) return End::Disconnected;
    if (!cap.start()) return End::CaptureError;
    setStatus(TrayState::Active, "streaming - " + label);

    auto t0 = Clock::now();
    long long sent = 0;
    long long fillFrames = (long long)rate * fillMs / 1000;
    auto nowFrames = [&]() -> long long {
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
        return us * (long long)rate / 1000000;
    };

    std::vector<int16_t> buf;
    End end = End::Disconnected;
    for (;;) {
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
                if (!sendAll(s, buf.data(), (int)(buf.size() * 2))) break;
                sent += fr;
            }
            sleepMs(pollMs);
            continue;
        }
        if (!sendAll(s, buf.data(), (int)(buf.size() * 2))) break;
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

void work(const std::string& configPath) {
    Config cfg(configPath);
    int pollMs = cfg.getInt("poll_ms", 5);
    int retryMs = cfg.getInt("retry_ms", 1000);
    int fillMs = cfg.getInt("fill_ms", 10);
    std::string mode = cfg.getString("transport", "auto");

    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    timeBeginPeriod(1);

    std::vector<std::unique_ptr<Transport>> transports;
    if (mode == "auto" || mode == "ios") transports.push_back(makeUsbmuxd(cfg));
    if (mode == "auto" || mode == "android") transports.push_back(makeAdb(cfg));
    if (transports.empty()) {
        setStatus(TrayState::Error, "unknown transport '" + mode + "' (use auto, ios or android)");
        return;
    }

    std::string waiting = mode == "ios"     ? "waiting for iPhone (cable, unlocked, trusted)..."
                        : mode == "android" ? "waiting for Android (cable, USB debugging allowed)..."
                                            : "waiting for iPhone or Android (cable, unlocked)...";

    std::unique_ptr<Capture> cap;
    for (;;) {
        if (!cap) {
            cap = makeCapture();
            std::string err;
            if (!cap->open(err)) {
                setStatus(TrayState::Error, "audio capture failed: " + err);
                cap.reset();
                sleepMs(retryMs);
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
            setStatus(TrayState::Idle, note.empty() ? waiting : note);
            sleepMs(retryMs);
            continue;
        }

        int nd = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof nd);
        End e = stream(s, *cap, via, pollMs, fillMs);
        closeSock(s);
        if (e == End::CaptureError) {
            cap.reset();
            setStatus(TrayState::Error, "audio device changed, reopening capture...");
        } else {
            setStatus(TrayState::Idle, "disconnected");
        }
        sleepMs(retryMs);
    }
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    std::string path = __argc > 1 ? __argv[1] : defaultConfigPath();
    return runTray(path, [path] { work(path); });
}
