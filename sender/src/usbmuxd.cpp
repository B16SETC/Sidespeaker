#include "transport.h"
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

const char* kClient = "<key>ClientVersionString</key><string>sidespeaker</string><key>ProgName</key><string>sidespeaker</string>";

std::string plist(const std::string& inner) {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
           "<plist version=\"1.0\"><dict>" + inner + "</dict></plist>";
}

bool muxSend(sock_t s, const std::string& body) {
    uint32_t h[4] = { (uint32_t)(16 + body.size()), 1, 8, 1 };
    return sendAll(s, h, 16) && sendAll(s, body.data(), (int)body.size());
}

bool muxRecv(sock_t s, std::string& out) {
    uint32_t h[4];
    if (!recvAll(s, h, 16) || h[0] < 16 || h[0] > (1u << 20)) return false;
    out.resize(h[0] - 16);
    return out.empty() || recvAll(s, &out[0], (int)out.size());
}

int findInt(const std::string& r, const char* key) {
    size_t k = r.find(std::string("<key>") + key + "</key>");
    if (k == std::string::npos) return -1;
    size_t i = r.find("<integer>", k);
    return i == std::string::npos ? -1 : atoi(r.c_str() + i + 9);
}

std::string findStr(const std::string& seg, const char* key) {
    size_t c = seg.find(std::string("<key>") + key + "</key>");
    if (c == std::string::npos) return std::string();
    size_t x = seg.find("<string>", c), y = seg.find("</string>", c);
    if (x == std::string::npos || y == std::string::npos || y < x) return std::string();
    return seg.substr(x + 8, y - x - 8);
}

class Usbmuxd : public Transport {
public:
    explicit Usbmuxd(const Config& c) : port_(c.getInt("port", 5000)), usbOnly_(c.getInt("usb_only", 1) != 0) {}

    const char* kind() const override { return "iPhone"; }

    void scan(std::vector<Found>& out, std::string&) override {
        sock_t s = connectLocal(27015);
        if (s == kBadSock) return;
        std::string r;
        if (muxSend(s, plist(std::string("<key>MessageType</key><string>ListDevices</string>") + kClient)) && muxRecv(s, r)) {
            const std::string key = "<key>DeviceID</key>";
            std::vector<std::pair<size_t, int>> pos;
            size_t k = 0;
            while ((k = r.find(key, k)) != std::string::npos) {
                size_t i = r.find("<integer>", k);
                if (i == std::string::npos) break;
                pos.push_back({ k, atoi(r.c_str() + i + 9) });
                k += key.size();
            }
            for (size_t j = 0; j < pos.size();) {
                size_t e = j;
                while (e < pos.size() && pos[e].second == pos[j].second) e++;
                size_t to = e < pos.size() ? pos[e].first : r.size();
                std::string seg = r.substr(pos[j].first, to - pos[j].first);
                std::string type = findStr(seg, "ConnectionType");
                std::string serial = findStr(seg, "SerialNumber");
                if (serial.empty()) serial = "device" + std::to_string(pos[j].second);
                if (!usbOnly_ || type == "USB") {
                    Found f;
                    f.kind = "iPhone";
                    f.serial = serial;
                    f.id = "ios:" + serial;
                    f.muxId = pos[j].second;
                    std::string tail = serial.size() > 4 ? serial.substr(serial.size() - 4) : serial;
                    f.name = "iPhone " + tail;
                    f.state = 2;
                    out.push_back(f);
                }
                j = e;
            }
        }
        closeSock(s);
    }

    sock_t connect(const Found& f, std::string& why) override {
        why.clear();
        sock_t s = muxConnect(f.muxId);
        if (s == kBadSock) why = "Open the Sidespeaker app";
        return s;
    }

private:
    sock_t muxConnect(int dev) {
        sock_t s = connectLocal(27015);
        if (s == kBadSock) return kBadSock;
        int p = ((port_ & 0xff) << 8) | (port_ >> 8);
        std::string body = plist(std::string("<key>MessageType</key><string>Connect</string>") + kClient +
            "<key>DeviceID</key><integer>" + std::to_string(dev) + "</integer>"
            "<key>PortNumber</key><integer>" + std::to_string(p) + "</integer>");
        std::string r;
        if (muxSend(s, body) && muxRecv(s, r) && findInt(r, "Number") == 0) return s;
        closeSock(s);
        return kBadSock;
    }

    int port_;
    bool usbOnly_;
};

}

std::unique_ptr<Transport> makeUsbmuxd(const Config& cfg) {
    return std::make_unique<Usbmuxd>(cfg);
}
