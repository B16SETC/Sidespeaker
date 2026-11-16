#include "hub.h"
#include "net.h"
#include <windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include "capture.h"
#include "mixer.h"
#include "transport.h"
#include "tray.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr double kPi = 3.14159265358979323846;

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

class Link {
public:
    Link(sock_t s, int outCh, uint32_t rate) : s_(s), outCh_(outCh), rate_(rate), cap_((size_t)rate * outCh) {
        th_ = std::thread([this] { run(); });
    }

    ~Link() { stop(); }

    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    int outCh() const { return outCh_; }
    uint32_t rate() const { return rate_; }
    bool dead() const { return dead_.load(); }

    void push(std::vector<char>&& b) {
        if (dead_.load()) return;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (bytes_ + b.size() > cap_) {
                q_.clear();
                bytes_ = 0;
            }
            bytes_ += b.size();
            q_.push_back(std::move(b));
        }
        cv_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (stopped_) return;
            stopped_ = true;
        }
        cv_.notify_all();
        shutdown(s_, 2);
        if (th_.joinable()) th_.join();
        closeSock(s_);
    }

    std::vector<float> row;
    std::atomic<int> beep{ 0 };
    double phase = 0;

private:
    void run() {
        for (;;) {
            std::vector<char> b;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stopped_ || !q_.empty(); });
                if (stopped_) break;
                b = std::move(q_.front());
                q_.pop_front();
                bytes_ -= b.size();
            }
            if (!sendAll(s_, b.data(), (int)b.size())) break;
        }
        dead_ = true;
    }

    sock_t s_;
    int outCh_;
    uint32_t rate_;
    size_t cap_;
    size_t bytes_ = 0;
    bool stopped_ = false;
    std::atomic<bool> dead_{ false };
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::vector<char>> q_;
    std::thread th_;
};

struct Saved {
    bool selected = true;
    double x = 0;
    double y = 0;
};

struct Seen {
    Found f;
    std::string status;
    std::string lastWhy;
    Phase phase = Phase::Idle;
    Clock::time_point nextTry{};
    std::shared_ptr<Link> link;
};

std::string settingsPath() {
    wchar_t b[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", b, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    std::wstring dir = std::wstring(b, n) + L"\\Sidespeaker";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring full = dir + L"\\layout.ini";
    int len = WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)std::max(0, len - 1), '\0');
    if (len > 1) WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, &out[0], len, nullptr, nullptr);
    return out;
}

class Hub {
public:
    void init(const Config& c) {
        scanMs_ = std::max(200, c.getInt("scan_ms", 1000));
        retryMs_ = std::max(100, c.getInt("retry_ms", 1000));
        pollMs_ = std::max(1, c.getInt("poll_ms", 5));
        fillMs_ = std::max(1, c.getInt("fill_ms", 10));
        mode_ = c.getString("transport", "auto");
        if (mode_ == "auto" || mode_ == "ios") transports_.push_back(makeUsbmuxd(c));
        if (mode_ == "auto" || mode_ == "android") transports_.push_back(makeAdb(c));
        std::lock_guard<std::mutex> lk(mu_);
        path_ = settingsPath();
        load();
    }

    void run() {
        if (transports_.empty()) {
            setStatus(TrayState::Error, "unknown transport '" + mode_ + "' (use auto, ios or android)");
            return;
        }
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        timeBeginPeriod(1);
        std::thread([this] { audio(); }).detach();
        for (;;) {
            scan();
            connectPending();
            publish();
            std::unique_lock<std::mutex> lk(mu_);
            wake_.wait_for(lk, std::chrono::milliseconds(scanMs_));
        }
    }

    HubView view() {
        std::lock_guard<std::mutex> lk(mu_);
        HubView v;
        v.surround = surround_;
        for (auto& s : seen_) {
            DeviceView d;
            d.id = s.f.id;
            d.kind = s.f.kind;
            d.name = s.f.name;
            d.status = s.status;
            d.phase = s.phase;
            auto it = saved_.find(s.f.id);
            if (it != saved_.end()) {
                d.selected = it->second.selected;
                d.x = it->second.x;
                d.y = it->second.y;
            }
            v.devices.push_back(d);
        }
        if (seen_.empty() || mode_ != "auto") v.note = note_;
        return v;
    }

    void select(const std::string& id, bool on) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            ensureSaved(id).selected = on;
            dirty_ = true;
            saveLocked();
        }
        wake_.notify_all();
    }

    void setSurround(bool on) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            surround_ = on;
            dirty_ = true;
            saveLocked();
        }
        wake_.notify_all();
    }

    void move(const std::string& id, double x, double y) {
        std::lock_guard<std::mutex> lk(mu_);
        double r = std::hypot(x, y);
        if (r > 1.0) {
            x /= r;
            y /= r;
        }
        Saved& s = ensureSaved(id);
        s.x = x;
        s.y = y;
        dirty_ = true;
    }

    void beep(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& s : seen_)
            if (s.f.id == id && s.link && !s.link->dead()) s.link->beep = (int)(s.link->rate() * 0.8);
    }

    void save() {
        std::lock_guard<std::mutex> lk(mu_);
        saveLocked();
    }

private:
    Saved& ensureSaved(const std::string& id) {
        auto it = saved_.find(id);
        if (it != saved_.end()) return it->second;
        static const double angles[] = { -45, 45, -135, 135, 0, 180, -90, 90 };
        double a = angles[saved_.size() % 8] * kPi / 180.0;
        Saved s;
        s.x = 0.75 * std::sin(a);
        s.y = 0.75 * std::cos(a);
        return saved_[id] = s;
    }

    void load() {
        if (path_.empty()) return;
        std::ifstream f(path_);
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("surround=", 0) == 0) {
                surround_ = line.size() > 9 && line[9] == '1';
            } else if (line.rfind("device|", 0) == 0) {
                std::vector<std::string> p;
                std::stringstream ss(line);
                std::string t;
                while (std::getline(ss, t, '|')) p.push_back(t);
                if (p.size() == 5) {
                    Saved s;
                    s.selected = p[2] == "1";
                    s.x = atof(p[3].c_str());
                    s.y = atof(p[4].c_str());
                    saved_[p[1]] = s;
                }
            }
        }
    }

    void saveLocked() {
        if (path_.empty()) return;
        std::ofstream f(path_, std::ios::trunc);
        f << "surround=" << (surround_ ? 1 : 0) << "\n";
        for (auto& kv : saved_) f << "device|" << kv.first << "|" << (kv.second.selected ? 1 : 0) << "|" << kv.second.x << "|" << kv.second.y << "\n";
    }

    int wantCh() const { return surround_ ? 1 : 2; }

    Seen* find(const std::string& id) {
        for (auto& s : seen_)
            if (s.f.id == id) return &s;
        return nullptr;
    }

    void refresh(Seen& s) {
        bool sel = ensureSaved(s.f.id).selected;
        if (s.f.state == 0) {
            s.phase = Phase::Unavailable;
            s.status = "Offline";
        } else if (s.f.state == 1) {
            s.phase = Phase::Unavailable;
            s.status = "Tap Allow on the phone";
        } else if (!sel) {
            s.phase = Phase::Idle;
            s.status = "Not in use";
        } else if (s.link && !s.link->dead()) {
            s.phase = Phase::Streaming;
            s.status = "Streaming";
        } else if (!s.lastWhy.empty()) {
            s.phase = Phase::Waiting;
            s.status = s.lastWhy;
        } else {
            s.phase = Phase::Connecting;
            s.status = "Connecting...";
        }
    }

    void recount() {
        int n = 0;
        for (auto& s : seen_)
            if (s.link && !s.link->dead()) n++;
        streaming_ = n;
    }

    void scan() {
        std::vector<Found> found;
        std::vector<std::string> notes;
        for (auto& t : transports_) {
            std::string n;
            t->scan(found, n);
            if (!n.empty()) notes.push_back(n);
        }
        std::vector<std::shared_ptr<Link>> drop;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto it = seen_.begin(); it != seen_.end();) {
                bool still = std::any_of(found.begin(), found.end(), [&](const Found& f) { return f.id == it->f.id; });
                if (still) {
                    ++it;
                    continue;
                }
                if (it->link) drop.push_back(it->link);
                it = seen_.erase(it);
                dirty_ = true;
            }
            for (auto& f : found) {
                Seen* s = find(f.id);
                if (!s) {
                    seen_.emplace_back();
                    s = &seen_.back();
                    ensureSaved(f.id);
                    saveLocked();
                }
                s->f = f;
                bool sel = ensureSaved(f.id).selected;
                if (s->link && (s->link->dead() || s->link->outCh() != wantCh() || !sel || f.state != 2)) {
                    drop.push_back(s->link);
                    s->link.reset();
                    s->nextTry = Clock::now();
                    dirty_ = true;
                }
                refresh(*s);
            }
            std::string joined;
            for (auto& n : notes) joined += (joined.empty() ? "" : " ") + n;
            note_ = joined;
            recount();
        }
        for (auto& l : drop) l->stop();
    }

    struct Job {
        std::string id;
        Found f;
        Transport* t;
        int outCh;
        uint32_t rate;
    };

    Transport* transportFor(const std::string& kind) {
        for (auto& t : transports_)
            if (kind == t->kind()) return t.get();
        return nullptr;
    }

    void connectPending() {
        std::vector<Job> jobs;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!fmtOk_) return;
            auto now = Clock::now();
            for (auto& s : seen_) {
                if (s.f.state != 2 || s.link || now < s.nextTry || !ensureSaved(s.f.id).selected) continue;
                Transport* t = transportFor(s.f.kind);
                if (t) jobs.push_back({ s.f.id, s.f, t, wantCh(), rate_ });
            }
        }
        for (auto& j : jobs) {
            std::string why;
            std::shared_ptr<Link> link;
            sock_t sk = j.t->connect(j.f, why);
            if (sk != kBadSock) {
                int nd = 1;
                setsockopt(sk, IPPROTO_TCP, TCP_NODELAY, (const char*)&nd, sizeof nd);
                uint32_t hdr[2] = { j.rate, (uint32_t)j.outCh };
                if (sendAll(sk, hdr, 8)) {
                    link = std::make_shared<Link>(sk, j.outCh, j.rate);
                } else {
                    closeSock(sk);
                    why = "Connection lost";
                }
            }
            std::shared_ptr<Link> reject;
            {
                std::lock_guard<std::mutex> lk(mu_);
                Seen* s = find(j.id);
                if (!s) {
                    reject = link;
                } else if (link && fmtOk_ && rate_ == j.rate && wantCh() == j.outCh && ensureSaved(j.id).selected) {
                    s->link = link;
                    s->lastWhy.clear();
                    dirty_ = true;
                    refresh(*s);
                } else {
                    reject = link;
                    s->lastWhy = why;
                    s->nextTry = Clock::now() + std::chrono::milliseconds(retryMs_);
                    refresh(*s);
                }
                recount();
            }
            if (reject) reject->stop();
        }
    }

    void publish() {
        TrayState st = TrayState::Idle;
        std::string text;
        {
            std::lock_guard<std::mutex> lk(mu_);
            std::vector<std::string> live;
            for (auto& s : seen_)
                if (s.link && !s.link->dead()) live.push_back(s.f.name);
            if (!capErr_.empty()) {
                st = TrayState::Error;
                text = "audio capture failed: " + capErr_;
            } else if (!live.empty()) {
                st = TrayState::Active;
                text = live.size() == 1 ? "streaming - " + live[0] : "streaming to " + std::to_string(live.size()) + " phones";
                if (surround_) text += " (surround)";
            } else if (seen_.empty()) {
                text = (mode_ != "auto" && !note_.empty()) ? note_
                     : mode_ == "ios" ? "waiting for iPhone (cable, unlocked, trusted)..."
                     : mode_ == "android" ? "waiting for Android (cable, USB debugging allowed)..."
                     : "waiting for iPhone or Android (cable, unlocked)...";
            } else {
                const Seen* pick = nullptr;
                for (auto& s : seen_)
                    if (ensureSaved(s.f.id).selected) {
                        pick = &s;
                        break;
                    }
                text = pick ? pick->f.name + ": " + pick->status : "no phone selected - click the tray icon";
            }
        }
        setStatus(st, text);
    }

    void failCapture(const std::string& err) {
        std::vector<std::shared_ptr<Link>> drop;
        {
            std::lock_guard<std::mutex> lk(mu_);
            capErr_ = err;
            fmtOk_ = false;
            for (auto& s : seen_)
                if (s.link) {
                    drop.push_back(s.link);
                    s.link.reset();
                    refresh(s);
                }
            recount();
        }
        for (auto& l : drop) l->stop();
        publish();
    }

    void rebuild() {
        dirty_ = false;
        Layout lay = makeLayout(srcCh_, mask_);
        std::vector<Seen*> sur;
        std::vector<Pos> pos;
        for (auto& s : seen_) {
            if (!s.link || s.link->dead()) continue;
            if (s.link->outCh() == 1) {
                const Saved& sv = ensureSaved(s.f.id);
                sur.push_back(&s);
                pos.push_back({ sv.x, sv.y });
            } else {
                s.link->row = stereoRows(lay);
            }
        }
        if (sur.empty()) return;
        std::vector<float> rows = surroundRows(lay, pos);
        for (size_t i = 0; i < sur.size(); ++i)
            sur[i]->link->row.assign(rows.begin() + (long)(i * srcCh_), rows.begin() + (long)((i + 1) * srcCh_));
    }

    void feed(const std::vector<int16_t>& in, int frames, int srcCh) {
        struct Out {
            std::shared_ptr<Link> link;
            std::vector<float> row;
            int outCh;
        };
        std::vector<Out> outs;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (srcCh != srcCh_) return;
            if (dirty_) rebuild();
            for (auto& s : seen_)
                if (s.link && !s.link->dead()) outs.push_back({ s.link, s.link->row, s.link->outCh() });
        }
        for (auto& o : outs) {
            if (o.row.size() != (size_t)o.outCh * srcCh) continue;
            std::vector<char> bytes((size_t)frames * o.outCh * 2);
            int16_t* p = reinterpret_cast<int16_t*>(bytes.data());
            int left = o.link->beep.load();
            if (left > 0) {
                int rate = (int)o.link->rate();
                int total = (int)(rate * 0.8);
                double fade = rate * 0.01;
                double step = 2.0 * kPi * 880.0 / rate;
                for (int i = 0; i < frames; ++i) {
                    int l = left - i;
                    double env = l > 0 ? std::min(1.0, std::min((double)(total - l), (double)l) / fade) : 0.0;
                    env = std::max(0.0, env);
                    int16_t v = (int16_t)(std::sin(o.link->phase) * 0.3 * env * 32767.0);
                    o.link->phase += step;
                    if (o.link->phase > 2.0 * kPi) o.link->phase -= 2.0 * kPi;
                    for (int c = 0; c < o.outCh; ++c) p[(size_t)i * o.outCh + c] = v;
                }
                o.link->beep.store(std::max(0, left - frames));
            } else {
                mixBlock(in.data(), frames, srcCh, o.row.data(), o.outCh, p);
            }
            o.link->push(std::move(bytes));
        }
    }

    void audio() {
        std::unique_ptr<Capture> cap;
        bool started = false;
        std::vector<int16_t> buf;
        Clock::time_point t0 = Clock::now();
        long long sent = 0;
        uint32_t rate = 0;
        int ch = 0;
        for (;;) {
            if (!cap) {
                cap = makeCapture();
                std::string err;
                if (!cap->open(err)) {
                    cap.reset();
                    failCapture(err);
                    sleepMs(retryMs_);
                    continue;
                }
                rate = cap->sampleRate();
                ch = cap->channels();
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    capErr_.clear();
                    fmtOk_ = true;
                    rate_ = rate;
                    srcCh_ = ch;
                    mask_ = cap->channelMask();
                    dirty_ = true;
                }
                wake_.notify_all();
                started = false;
            }
            if (streaming_.load() == 0) {
                if (started) {
                    cap->stop();
                    started = false;
                }
                sleepMs(20);
                continue;
            }
            if (!started) {
                if (!cap->start()) {
                    cap.reset();
                    failCapture("could not start capture");
                    sleepMs(retryMs_);
                    continue;
                }
                started = true;
                t0 = Clock::now();
                sent = 0;
            }
            auto nowFrames = [&]() -> long long {
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
                return us * (long long)rate / 1000000;
            };
            int n = cap->read(buf);
            if (n < 0) {
                cap.reset();
                failCapture("audio device changed, reopening capture...");
                sleepMs(retryMs_);
                continue;
            }
            if (n == 0) {
                long long deficit = nowFrames() - sent;
                if (deficit >= (long long)rate * fillMs_ / 1000) {
                    int fr = (int)std::min<long long>(deficit, rate / 5);
                    buf.assign((size_t)fr * ch, 0);
                    feed(buf, fr, ch);
                    sent += fr;
                }
                sleepMs(pollMs_);
                continue;
            }
            feed(buf, n, ch);
            sent += n;
            if (sent - nowFrames() > (long long)rate / 5) {
                t0 = Clock::now();
                sent = 0;
            }
        }
    }

    std::mutex mu_;
    std::condition_variable wake_;
    std::vector<std::unique_ptr<Transport>> transports_;
    std::vector<Seen> seen_;
    std::map<std::string, Saved> saved_;
    std::string path_;
    std::string mode_ = "auto";
    std::string note_;
    std::string capErr_;
    bool surround_ = false;
    bool dirty_ = true;
    bool fmtOk_ = false;
    uint32_t rate_ = 0;
    int srcCh_ = 0;
    uint32_t mask_ = 0;
    int scanMs_ = 1000;
    int retryMs_ = 1000;
    int pollMs_ = 5;
    int fillMs_ = 10;
    std::atomic<int> streaming_{ 0 };
};

Hub& hub() {
    static Hub* h = new Hub();
    return *h;
}

}

HubView hubView() { return hub().view(); }
void hubSelect(const std::string& id, bool on) { hub().select(id, on); }
void hubSurround(bool on) { hub().setSurround(on); }
void hubMove(const std::string& id, double x, double y) { hub().move(id, x, y); }
void hubBeep(const std::string& id) { hub().beep(id); }
void hubSave() { hub().save(); }

void hubRun(const Config& cfg) {
    hub().init(cfg);
    hub().run();
}
