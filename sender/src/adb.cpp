#include "transport.h"
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace {

struct Dev {
    std::string serial;
    std::string state;
};

bool run(const std::string& cmd, std::string& out) {
    out.clear();
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::string full = "cmd.exe /d /c \"" + cmd + " 2>&1\"";
    std::vector<char> line(full.begin(), full.end());
    line.push_back('\0');

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }
    char b[512];
    DWORD n;
    while (ReadFile(rd, b, sizeof b, &n, nullptr) && n > 0) out.append(b, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

std::vector<Dev> parseDevices(const std::string& out) {
    std::vector<Dev> v;
    std::istringstream is(out);
    std::string line;
    while (std::getline(is, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        size_t t = line.find('\t');
        if (t == std::string::npos) continue;
        v.push_back({ line.substr(0, t), line.substr(t + 1) });
    }
    return v;
}

class Adb : public Transport {
public:
    explicit Adb(const Config& c)
        : adb_(c.getString("adb_path", "adb")),
          serial_(c.getString("adb_serial", "")),
          port_(c.getInt("port", 5000)),
          explicit_(c.getString("transport", "auto") == "android") {}

    const char* name() const override { return "Android"; }

    sock_t connect(std::string& why) override {
        why.clear();
        std::string out;
        run(quoted() + " devices", out);
        if (explicit_ && (out.find("not recognized") != std::string::npos || out.find("not found") != std::string::npos)) {
            why = "adb not found - set adb_path in config.ini";
            return kBadSock;
        }

        std::vector<Dev> match;
        for (auto& d : parseDevices(out))
            if (serial_.empty() || d.serial == serial_) match.push_back(d);
        if (match.empty()) return kBadSock;

        std::vector<Dev> ready;
        for (auto& d : match)
            if (d.state == "device") ready.push_back(d);
        if (ready.empty()) {
            why = "Android found: unlock it and allow USB debugging";
            return kBadSock;
        }
        if (ready.size() > 1) {
            why = "Several Android devices: set adb_serial in config.ini";
            return kBadSock;
        }

        std::string spec = "tcp:" + std::to_string(port_);
        if (!run(quoted() + " -s " + ready[0].serial + " forward " + spec + " " + spec, out)) {
            why = "adb forward failed (is the port already in use?)";
            return kBadSock;
        }

        sock_t s = connectLocal(port_);
        if (s == kBadSock) {
            why = "adb forward failed (is the port already in use?)";
            return kBadSock;
        }
        char b = 0;
        if (!waitReadable(s, 1500) || recv(s, &b, 1, 0) != 1 || b != 'S') {
            closeSock(s);
            why = "Android found, waiting for app to be open...";
            return kBadSock;
        }
        return s;
    }

private:
    std::string quoted() const { return "\"" + adb_ + "\""; }

    std::string adb_;
    std::string serial_;
    int port_;
    bool explicit_;
};

}

std::unique_ptr<Transport> makeAdb(const Config& cfg) {
    return std::make_unique<Adb>(cfg);
}
