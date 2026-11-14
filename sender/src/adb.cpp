#include "transport.h"
#include <algorithm>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>

namespace {

struct Dev {
    std::string serial;
    std::string state;
};

bool isFile(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string envVar(const char* name) {
    char b[2048];
    DWORD n = GetEnvironmentVariableA(name, b, sizeof b);
    return n > 0 && n < sizeof b ? std::string(b, n) : std::string();
}

std::string exeDir() {
    char p[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p, n);
    size_t k = s.find_last_of("\\/");
    return k == std::string::npos ? std::string() : s.substr(0, k + 1);
}

std::string searchPath(const std::string& name) {
    char b[MAX_PATH];
    DWORD n = SearchPathA(nullptr, name.c_str(), ".exe", MAX_PATH, b, nullptr);
    return n > 0 && n < MAX_PATH ? std::string(b, n) : std::string();
}

std::string locateAdb(const std::string& configured) {
    std::string found = searchPath(configured);
    if (!found.empty() || configured != "adb") return found;

    std::vector<std::string> dirs = { exeDir(), exeDir() + "platform-tools\\" };
    for (const char* v : { "ANDROID_HOME", "ANDROID_SDK_ROOT" }) {
        std::string r = envVar(v);
        if (!r.empty()) dirs.push_back(r + "\\platform-tools\\");
    }
    std::string local = envVar("LOCALAPPDATA");
    if (!local.empty()) dirs.push_back(local + "\\Android\\Sdk\\platform-tools\\");
    std::string profile = envVar("USERPROFILE");
    if (!profile.empty()) {
        dirs.push_back(profile + "\\scoop\\apps\\adb\\current\\platform-tools\\");
        dirs.push_back(profile + "\\platform-tools\\");
    }
    std::string choco = envVar("ChocolateyInstall");
    if (!choco.empty()) dirs.push_back(choco + "\\bin\\");
    dirs.push_back("C:\\platform-tools\\");
    dirs.push_back("C:\\adb\\");

    for (auto& d : dirs)
        if (isFile(d + "adb.exe")) return d + "adb.exe";
    return std::string();
}

bool run(const std::string& adb, const std::string& args, std::string& out, DWORD timeoutMs) {
    out.clear();
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOA si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = wr;
    si.hStdError = wr;

    std::string cl = "\"" + adb + "\" " + args;
    std::vector<char> line(cl.begin(), cl.end());
    line.push_back('\0');

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        CloseHandle(rd);
        return false;
    }

    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    bool exited = false;
    bool timedOut = false;
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
            char b[512];
            DWORD n = 0;
            if (ReadFile(rd, b, std::min<DWORD>(avail, sizeof b), &n, nullptr) && n > 0) out.append(b, n);
            continue;
        }
        if (exited) break;
        if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
            exited = true;
            continue;
        }
        if (GetTickCount64() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            timedOut = true;
            break;
        }
    }

    DWORD code = 1;
    if (!timedOut) GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return !timedOut && code == 0;
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
        std::string adb = locateAdb(adb_);
        if (adb.empty()) {
            why = explicit_ ? "adb not found - install Android platform-tools or set adb_path in config.ini"
                            : "waiting for iPhone or Android (adb not found, needed for Android)...";
            return kBadSock;
        }

        std::string out;
        if (!run(adb, "devices", out, 15000) && out.empty()) {
            why = "adb did not respond - close other adb or phone-manager programs";
            return kBadSock;
        }

        std::vector<Dev> match;
        for (auto& d : parseDevices(out))
            if (serial_.empty() || d.serial == serial_) match.push_back(d);
        if (match.empty()) {
            if (explicit_) why = "no Android device listed by adb - check cable, file-transfer mode and USB debugging";
            return kBadSock;
        }

        std::vector<Dev> ready;
        for (auto& d : match)
            if (d.state == "device") ready.push_back(d);
        if (ready.empty()) {
            why = "Android found (" + match[0].state + "): unlock it and allow USB debugging";
            return kBadSock;
        }
        if (ready.size() > 1) {
            why = "Several Android devices: set adb_serial in config.ini";
            return kBadSock;
        }

        std::string spec = "tcp:" + std::to_string(port_);
        if (!run(adb, "-s " + ready[0].serial + " forward " + spec + " " + spec, out, 10000)) {
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
    std::string adb_;
    std::string serial_;
    int port_;
    bool explicit_;
};

}

std::unique_ptr<Transport> makeAdb(const Config& cfg) {
    return std::make_unique<Adb>(cfg);
}
