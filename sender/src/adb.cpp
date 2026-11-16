#include "transport.h"
#include <algorithm>
#include <map>
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
          port_(c.getInt("port", 5000)),
          explicit_(c.getString("transport", "auto") == "android") {}

    const char* kind() const override { return "Android"; }

    void scan(std::vector<Found>& out, std::string& note) override {
        std::string adb = locateAdb(adb_);
        if (adb.empty()) {
            note = explicit_ ? "adb not found - install Android platform-tools or set adb_path in config.ini"
                             : "Android needs adb (Android platform-tools), which was not found";
            return;
        }
        std::string text;
        if (!run(adb, "devices", text, 15000) && text.empty()) {
            note = "adb did not respond - close other adb or phone-manager programs";
            return;
        }
        for (auto& d : parseDevices(text)) {
            if (d.serial.rfind("emulator-", 0) == 0) continue;
            Found f;
            f.kind = "Android";
            f.serial = d.serial;
            f.id = "android:" + d.serial;
            f.state = d.state == "device" ? 2 : d.state == "unauthorized" ? 1 : 0;
            f.name = f.state == 2 ? modelOf(adb, d.serial) : nameOf(d.serial);
            out.push_back(f);
        }
    }

    sock_t connect(const Found& f, std::string& why) override {
        why.clear();
        std::string adb = locateAdb(adb_);
        if (adb.empty()) {
            why = "adb not found";
            return kBadSock;
        }
        int local = localPort(f.serial);
        std::string out;
        if (!run(adb, "-s " + f.serial + " forward tcp:" + std::to_string(local) + " tcp:" + std::to_string(port_), out, 10000)) {
            why = "adb forward failed";
            return kBadSock;
        }
        sock_t s = connectLocal(local);
        if (s == kBadSock) {
            why = "adb forward failed";
            return kBadSock;
        }
        char b = 0;
        if (!waitReadable(s, 1500) || recv(s, &b, 1, 0) != 1 || b != 'S') {
            closeSock(s);
            why = "Open the Sidespeaker app";
            return kBadSock;
        }
        return s;
    }

private:
    std::string nameOf(const std::string& serial) {
        auto it = names_.find(serial);
        return it == names_.end() ? std::string("Android") : it->second;
    }

    std::string modelOf(const std::string& adb, const std::string& serial) {
        auto it = names_.find(serial);
        if (it != names_.end() && it->second != "Android") return it->second;
        std::string out;
        std::string model = "Android";
        if (run(adb, "-s " + serial + " shell getprop ro.product.model", out, 5000)) {
            while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) out.pop_back();
            if (!out.empty() && out.size() < 48) model = out;
        }
        names_[serial] = model;
        return model;
    }

    int localPort(const std::string& serial) {
        auto it = ports_.find(serial);
        if (it != ports_.end()) return it->second;
        int p = port_ + 100 + (int)ports_.size();
        ports_[serial] = p;
        return p;
    }

    std::string adb_;
    int port_;
    bool explicit_;
    std::map<std::string, std::string> names_;
    std::map<std::string, int> ports_;
};

}

std::unique_ptr<Transport> makeAdb(const Config& cfg) {
    return std::make_unique<Adb>(cfg);
}
