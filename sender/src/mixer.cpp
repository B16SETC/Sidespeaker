#include "mixer.h"
#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Spk {
    double az;
    bool omni;
    std::vector<float> feed;
};

double roleAz(Role r) {
    switch (r) {
    case Role::FL: return -30;
    case Role::FR: return 30;
    case Role::FC: return 0;
    case Role::BL: return -135;
    case Role::BR: return 135;
    case Role::FLC: return -15;
    case Role::FRC: return 15;
    case Role::BC: return 180;
    case Role::SL: return -90;
    case Role::SR: return 90;
    default: return 0;
    }
}

double smooth(double a, double b, double v) {
    double t = std::min(1.0, std::max(0.0, (v - a) / (b - a)));
    return t * t * (3 - 2 * t);
}

double wrapDeg(double d) {
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return d;
}

std::vector<Spk> speakers(const Layout& l, float& trim) {
    std::vector<Spk> v;
    trim = 1.0f;
    int n = l.srcCh;
    if (n == 1) {
        v.push_back({ 0, true, { 1.0f } });
        return v;
    }
    if (n == 2 && l.roles[0] == Role::FL && l.roles[1] == Role::FR) {
        v.push_back({ -30, false, { 0.875f, -0.125f } });
        v.push_back({ 30, false, { -0.125f, 0.875f } });
        v.push_back({ 0, false, { 0.25f, 0.25f } });
        v.push_back({ -110, false, { 0.25f, -0.25f } });
        v.push_back({ 110, false, { -0.25f, 0.25f } });
        return v;
    }
    for (int c = 0; c < n; ++c) {
        Role r = l.roles[c];
        if (r == Role::LFE || r == Role::None) continue;
        std::vector<float> f(n, 0.0f);
        f[c] = 1.0f;
        v.push_back({ roleAz(r), false, f });
    }
    if (v.empty()) {
        std::vector<float> f(n, 0.0f);
        f[0] = 1.0f;
        v.push_back({ 0, true, f });
    }
    if (v.size() > 2) trim = (float)(1.0 / std::sqrt(v.size() / 2.0));
    return v;
}

double rawWeight(Pos p, const Spk& s) {
    if (s.omni) return 1.0;
    double r = std::hypot(p.x, p.y);
    double dir = smooth(0.06, 0.3, r);
    double az = std::atan2(p.x, p.y) * 180.0 / kPi;
    double d = std::fabs(wrapDeg(az - s.az)) * kPi / 180.0;
    double c = std::pow((1.0 + std::cos(d)) / 2.0, 4.0);
    return (1.0 - dir) + dir * c;
}

float softLimit(float x) {
    const float k = 0.8f;
    float a = std::fabs(x);
    if (a <= k) return x;
    float s = k + (1.0f - k) * std::tanh((a - k) / (1.0f - k));
    return x < 0 ? -s : s;
}

}

Layout makeLayout(int srcCh, uint32_t mask) {
    static const Role bits[] = { Role::FL, Role::FR, Role::FC, Role::LFE, Role::BL, Role::BR, Role::FLC, Role::FRC, Role::BC, Role::SL, Role::SR };
    Layout l;
    l.srcCh = std::max(1, srcCh);
    l.roles.assign(l.srcCh, Role::None);
    if (mask != 0) {
        int idx = 0;
        for (int b = 0; b < 32 && idx < l.srcCh; ++b) {
            if (!(mask & (1u << b))) continue;
            l.roles[idx++] = b < 11 ? bits[b] : Role::None;
        }
        return l;
    }
    switch (l.srcCh) {
    case 1: l.roles = { Role::FC }; break;
    case 4: l.roles = { Role::FL, Role::FR, Role::BL, Role::BR }; break;
    case 6: l.roles = { Role::FL, Role::FR, Role::FC, Role::LFE, Role::BL, Role::BR }; break;
    case 8: l.roles = { Role::FL, Role::FR, Role::FC, Role::LFE, Role::BL, Role::BR, Role::SL, Role::SR }; break;
    default:
        l.roles[0] = Role::FL;
        if (l.srcCh > 1) l.roles[1] = Role::FR;
        break;
    }
    return l;
}

std::vector<float> stereoRows(const Layout& l) {
    int n = l.srcCh;
    std::vector<float> m((size_t)2 * n, 0.0f);
    if (n == 1) {
        m[0] = 1.0f;
        m[1] = 1.0f;
        return m;
    }
    const float h = 0.707f;
    bool any = false;
    for (int c = 0; c < n; ++c) {
        float lc = 0, rc = 0;
        switch (l.roles[c]) {
        case Role::FL: lc = 1; break;
        case Role::FR: rc = 1; break;
        case Role::FC: lc = h; rc = h; break;
        case Role::BL: case Role::SL: case Role::FLC: lc = h; break;
        case Role::BR: case Role::SR: case Role::FRC: rc = h; break;
        case Role::BC: lc = 0.5f; rc = 0.5f; break;
        default: break;
        }
        if (l.roles[c] != Role::None) any = true;
        m[c] = lc;
        m[n + c] = rc;
    }
    if (!any) {
        std::fill(m.begin(), m.end(), 0.0f);
        m[0] = 1.0f;
        m[n + 1] = 1.0f;
    }
    return m;
}

std::vector<float> surroundRows(const Layout& l, const std::vector<Pos>& phones) {
    int n = l.srcCh;
    size_t pc = phones.size();
    std::vector<float> m(pc * n, 0.0f);
    if (pc == 0) return m;
    float trim = 1.0f;
    std::vector<Spk> spk = speakers(l, trim);
    std::vector<double> raw(pc);
    for (const Spk& s : spk) {
        double sum = 0;
        size_t best = 0;
        for (size_t d = 0; d < pc; ++d) {
            raw[d] = rawWeight(phones[d], s);
            sum += raw[d] * raw[d];
            if (raw[d] > raw[best]) best = d;
        }
        for (size_t d = 0; d < pc; ++d) {
            double g = sum < 1e-9 ? (d == best ? 1.0 : 0.0) : raw[d] / std::sqrt(sum);
            for (int c = 0; c < n; ++c) m[d * n + c] += (float)(g * s.feed[c] * trim);
        }
    }
    return m;
}

void mixBlock(const int16_t* in, int frames, int srcCh, const float* rows, int outCh, int16_t* out) {
    const float inv = 1.0f / 32768.0f;
    for (int f = 0; f < frames; ++f) {
        const int16_t* s = in + (size_t)f * srcCh;
        for (int o = 0; o < outCh; ++o) {
            const float* r = rows + (size_t)o * srcCh;
            float acc = 0;
            for (int c = 0; c < srcCh; ++c) acc += r[c] * (float)s[c];
            float y = softLimit(acc * inv);
            long v = std::lround(y * 32768.0f);
            out[(size_t)f * outCh + o] = (int16_t)std::max(-32768L, std::min(32767L, v));
        }
    }
}

double azimuthDeg(Pos p) {
    return std::atan2(p.x, p.y) * 180.0 / kPi;
}

std::string roleName(Pos p) {
    if (std::hypot(p.x, p.y) < 0.2) return "Full mix";
    double a = azimuthDeg(p);
    double m = std::fabs(a);
    bool right = a > 0;
    if (m <= 22.5) return "Center";
    if (m <= 67.5) return right ? "Front right" : "Front left";
    if (m <= 112.5) return right ? "Side right" : "Side left";
    if (m <= 157.5) return right ? "Rear right" : "Rear left";
    return "Rear center";
}
