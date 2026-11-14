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

class Usbmuxd : public Transport {
public:
    explicit Usbmuxd(const Config& c) : port_(c.getInt("port", 5000)), usbOnly_(c.getInt("usb_only", 1) != 0) {}

    const char* name() const override { return "iPhone"; }

    sock_t connect(std::string& why) override {
        why.clear();
        int id = findDevice();
        if (id < 0) return kBadSock;
        sock_t s = muxConnect(id);
        if (s == kBadSock) why = "iPhone found, waiting for app to be open...";
        return s;
    }

private:
    int findDevice() {
        sock_t s = connectLocal(27015);
        if (s == kBadSock) return -1;
        std::string r;
        int id = -1;
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
                std::string type;
                size_t c = seg.find("<key>ConnectionType</key>");
                if (c != std::string::npos) {
                    size_t x = seg.find("<string>", c), y = seg.find("</string>", c);
                    if (x != std::string::npos && y != std::string::npos && y > x) type = seg.substr(x + 8, y - x - 8);
                }
                if (!usbOnly_ || type == "USB") {
                    id = pos[j].second;
                    break;
                }
                j = e;
            }
        }
        closeSock(s);
        return id;
    }

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
