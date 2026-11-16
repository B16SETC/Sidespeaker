#include "ui.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "hub.h"
#include "mixer.h"
#include "util.h"

namespace {

#ifndef SS_VERSION
#define SS_VERSION "2.0.0"
#endif

const COLORREF kBg = RGB(255, 255, 255);
const COLORREF kText = RGB(28, 28, 32);
const COLORREF kMuted = RGB(112, 116, 126);
const COLORREF kLine = RGB(228, 230, 236);
const COLORREF kAccent = RGB(0, 120, 212);
const COLORREF kPanel = RGB(243, 245, 249);
const COLORREF kRing = RGB(212, 217, 227);
const COLORREF kGreen = RGB(46, 170, 80);
const COLORREF kAmber = RGB(226, 150, 30);
const COLORREF kGrey = RGB(160, 164, 174);

HWND g_win = nullptr;
HFONT g_font = nullptr;
HFONT g_bold = nullptr;
HFONT g_small = nullptr;
HFONT g_title = nullptr;
double g_scale = 1.0;
HubView g_view;
std::string g_drag;
const DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;

int S(int v) {
    return (int)(v * g_scale + 0.5);
}

bool isActive(const DeviceView& d) {
    return d.phase == Phase::Waiting || d.phase == Phase::Connecting || d.phase == Phase::Streaming;
}

struct Geo {
    int w = 0;
    int h = 0;
    int pad = 0;
    int rowsTop = 0;
    int rowH = 0;
    int surroundY = 0;
    int surroundH = 0;
    int canvasTop = 0;
    int side = 0;
    int cx = 0;
    int cy = 0;
    int radius = 0;
    int footerH = 0;
    bool canvas = false;
    std::vector<const DeviceView*> active;
};

Geo geometry() {
    Geo g;
    g.w = S(440);
    g.pad = S(16);
    g.rowsTop = S(50);
    g.rowH = S(40);
    int rows = std::max<int>(1, (int)g_view.devices.size());
    g.surroundY = g.rowsTop + rows * g.rowH + S(10);
    g.surroundH = S(48);
    g.canvasTop = g.surroundY + g.surroundH + S(6);
    g.canvas = g_view.surround;
    g.footerH = g_view.note.empty() ? S(12) : S(52);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int avail = (wa.bottom - wa.top) - g.canvasTop - g.footerH - S(80);
    g.side = std::max(S(220), std::min(S(340), avail));
    g.cx = g.w / 2;
    g.cy = g.canvasTop + g.side / 2;
    g.radius = g.side / 2 - S(46);
    g.h = (g.canvas ? g.canvasTop + g.side + S(8) : g.surroundY + g.surroundH) + g.footerH;
    for (auto& d : g_view.devices)
        if (isActive(d)) g.active.push_back(&d);
    return g;
}

RECT checkRect(const Geo& g, int y) {
    int b = S(18);
    return { g.pad, y + (g.rowH - b) / 2, g.pad + b, y + (g.rowH - b) / 2 + b };
}

RECT testRect(const Geo& g, int y) {
    return { g.w - g.pad - S(64), y + S(8), g.w - g.pad, y + g.rowH - S(8) };
}

POINT phonePoint(const Geo& g, const DeviceView& d) {
    return { g.cx + (int)std::lround(d.x * g.radius), g.cy - (int)std::lround(d.y * g.radius) };
}

struct Hit {
    enum Kind { None, Check, Test, Surround, Phone } kind = None;
    std::string id;
};

Hit hitTest(const Geo& g, POINT p) {
    Hit h;
    if (g.canvas) {
        int best = S(30) * S(30);
        for (auto* d : g.active) {
            POINT c = phonePoint(g, *d);
            int dx = p.x - c.x, dy = p.y - c.y;
            if (std::abs(dx) <= S(16) && std::abs(dy) <= S(22) && dx * dx + dy * dy <= best) {
                best = dx * dx + dy * dy;
                h.kind = Hit::Phone;
                h.id = d->id;
            }
        }
        if (h.kind != Hit::None) return h;
    }
    for (size_t i = 0; i < g_view.devices.size(); ++i) {
        int y = g.rowsTop + (int)i * g.rowH;
        if (p.y < y || p.y >= y + g.rowH) continue;
        RECT t = testRect(g, y);
        if (PtInRect(&t, p)) {
            if (g_view.devices[i].phase == Phase::Streaming) {
                h.kind = Hit::Test;
                h.id = g_view.devices[i].id;
            }
            return h;
        }
        if (p.x >= g.pad && p.x < t.left && g_view.devices[i].phase != Phase::Unavailable) {
            h.kind = Hit::Check;
            h.id = g_view.devices[i].id;
        }
        return h;
    }
    if (p.y >= g.surroundY && p.y < g.surroundY + g.surroundH && p.x >= g.pad && p.x < g.w - g.pad) h.kind = Hit::Surround;
    return h;
}

void text(HDC dc, const std::string& s, RECT r, UINT flags, HFONT f, COLORREF c) {
    std::wstring w = widen(s);
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, w.c_str(), (int)w.size(), &r, flags | DT_NOPREFIX);
}

void box(HDC dc, RECT r, COLORREF fill, COLORREF border, int radius) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

void disc(HDC dc, int cx, int cy, int r, COLORREF fill, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

void ring(HDC dc, int cx, int cy, int r, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}

void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c, int w) {
    HPEN p = CreatePen(PS_SOLID, w, c);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, op);
    DeleteObject(p);
}

void checkbox(HDC dc, RECT r, bool on, bool enabled) {
    if (on && enabled) {
        box(dc, r, kAccent, kAccent, S(5));
        int w = r.right - r.left;
        HPEN p = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(dc, p);
        MoveToEx(dc, r.left + w * 22 / 100, r.top + w * 52 / 100, nullptr);
        LineTo(dc, r.left + w * 42 / 100, r.top + w * 72 / 100);
        LineTo(dc, r.left + w * 78 / 100, r.top + w * 28 / 100);
        SelectObject(dc, op);
        DeleteObject(p);
    } else {
        box(dc, r, kBg, enabled ? kGrey : kLine, S(5));
    }
}

COLORREF phaseColor(Phase p) {
    switch (p) {
    case Phase::Streaming: return kGreen;
    case Phase::Waiting:
    case Phase::Connecting: return kAmber;
    default: return kGrey;
    }
}

void paintRows(HDC dc, const Geo& g) {
    text(dc, "Phones", { g.pad, S(14), g.w - g.pad, S(40) }, DT_LEFT | DT_SINGLELINE, g_title, kText);
    text(dc, "v" SS_VERSION, { g.pad, S(20), g.w - g.pad, S(38) }, DT_RIGHT | DT_SINGLELINE, g_small, kMuted);
    if (g_view.devices.empty()) {
        text(dc, "No phones found. Plug one in with a data cable.", { g.pad, g.rowsTop, g.w - g.pad, g.rowsTop + g.rowH }, DT_LEFT | DT_VCENTER | DT_SINGLELINE, g_font, kMuted);
        return;
    }
    for (size_t i = 0; i < g_view.devices.size(); ++i) {
        const DeviceView& d = g_view.devices[i];
        int y = g.rowsTop + (int)i * g.rowH;
        bool usable = d.phase != Phase::Unavailable;
        checkbox(dc, checkRect(g, y), d.selected, usable);
        int tx = g.pad + S(32);
        RECT tr = testRect(g, y);
        text(dc, d.name == d.kind ? d.name : d.name + "   " + d.kind, { tx, y + S(3), tr.left - S(8), y + S(21) }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, g_font, usable ? kText : kMuted);
        disc(dc, tx + S(4), y + S(29), S(3), phaseColor(d.phase), phaseColor(d.phase));
        text(dc, d.status, { tx + S(14), y + S(22), tr.left - S(8), y + S(38) }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, g_small, kMuted);
        bool canTest = d.phase == Phase::Streaming;
        box(dc, tr, kBg, canTest ? kGrey : kLine, S(8));
        text(dc, "Test", tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE, g_font, canTest ? kText : kGrey);
        line(dc, g.pad, y + g.rowH - 1, g.w - g.pad, y + g.rowH - 1, kLine, 1);
    }
}

void paintSurround(HDC dc, const Geo& g) {
    int b = S(18);
    RECT cb = { g.pad, g.surroundY + S(6), g.pad + b, g.surroundY + S(6) + b };
    checkbox(dc, cb, g_view.surround, true);
    text(dc, "Surround sound", { g.pad + S(32), g.surroundY + S(4), g.w - g.pad, g.surroundY + S(24) }, DT_LEFT | DT_SINGLELINE, g_bold, kText);
    text(dc, "Each phone plays the part of the sound that matches where it sits.", { g.pad + S(32), g.surroundY + S(25), g.w - g.pad, g.surroundY + S(44) }, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS, g_small, kMuted);
}

void paintCanvas(HDC dc, const Geo& g) {
    RECT panel = { g.pad, g.canvasTop, g.w - g.pad, g.canvasTop + g.side };
    box(dc, panel, kPanel, kLine, S(12));
    for (int i = 1; i <= 3; ++i) ring(dc, g.cx, g.cy, g.radius * i / 3, kRing);
    line(dc, g.cx - g.radius, g.cy, g.cx + g.radius, g.cy, kRing, 1);
    line(dc, g.cx, g.cy - g.radius, g.cx, g.cy + g.radius, kRing, 1);
    text(dc, "FRONT", { g.cx - S(40), g.canvasTop + S(6), g.cx + S(40), g.canvasTop + S(22) }, DT_CENTER | DT_SINGLELINE, g_small, kMuted);
    disc(dc, g.cx, g.cy, S(11), RGB(60, 64, 76), RGB(60, 64, 76));
    text(dc, "You", { g.cx - S(30), g.cy + S(12), g.cx + S(30), g.cy + S(28) }, DT_CENTER | DT_SINGLELINE, g_small, kMuted);
    if (g.active.empty()) {
        text(dc, "Tick a phone above to place it here.", { g.pad, g.canvasTop + g.side - S(26), g.w - g.pad, g.canvasTop + g.side - S(8) }, DT_CENTER | DT_SINGLELINE, g_small, kMuted);
        return;
    }
    for (size_t i = 0; i < g.active.size(); ++i) {
        const DeviceView& d = *g.active[i];
        POINT c = phonePoint(g, d);
        bool live = d.phase == Phase::Streaming;
        COLORREF body = !live ? kGrey : d.kind == "iPhone" ? RGB(0, 122, 255) : RGB(52, 168, 83);
        if (d.id == g_drag) ring(dc, c.x, c.y, S(24), kAccent);
        RECT r = { c.x - S(10), c.y - S(17), c.x + S(10), c.y + S(17) };
        box(dc, r, body, body, S(7));
        text(dc, std::to_string(i + 1), r, DT_CENTER | DT_VCENTER | DT_SINGLELINE, g_bold, RGB(255, 255, 255));
        int half = S(64);
        int lx = std::max(g.pad + half, std::min(g.w - g.pad - half, (int)c.x));
        text(dc, d.name, { lx - half, c.y + S(19), lx + half, c.y + S(34) }, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS, g_small, kText);
        text(dc, roleName({ d.x, d.y }), { lx - half, c.y + S(33), lx + half, c.y + S(48) }, DT_CENTER | DT_SINGLELINE, g_small, kMuted);
    }
}

void paint(HDC dc, const Geo& g) {
    RECT all = { 0, 0, g.w, g.h };
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    paintRows(dc, g);
    paintSurround(dc, g);
    if (g.canvas) paintCanvas(dc, g);
    if (!g_view.note.empty())
        text(dc, g_view.note, { g.pad, g.h - g.footerH + S(8), g.w - g.pad, g.h - S(6) }, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS, g_small, kMuted);
}

void fitWindow() {
    if (!g_win) return;
    Geo g = geometry();
    RECT cr;
    GetClientRect(g_win, &cr);
    if (cr.right == g.w && cr.bottom == g.h) return;
    RECT want = { 0, 0, g.w, g.h };
    AdjustWindowRectEx(&want, kStyle, FALSE, 0);
    int ow = want.right - want.left, oh = want.bottom - want.top;
    RECT old;
    GetWindowRect(g_win, &old);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int left = old.right - ow;
    int top = old.bottom - oh;
    left = std::max((int)wa.left, std::min(left, (int)wa.right - ow));
    top = std::max((int)wa.top, std::min(top, (int)wa.bottom - oh));
    SetWindowPos(g_win, nullptr, left, top, ow, oh, SWP_NOZORDER | SWP_NOACTIVATE);
}

void refresh() {
    g_view = hubView();
    fitWindow();
    InvalidateRect(g_win, nullptr, FALSE);
}

void place() {
    Geo g = geometry();
    RECT want = { 0, 0, g.w, g.h };
    AdjustWindowRectEx(&want, kStyle, FALSE, 0);
    int ow = want.right - want.left, oh = want.bottom - want.top;
    POINT cur;
    GetCursorPos(&cur);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int left = cur.x - ow / 2;
    int top = cur.y - oh - S(14);
    if (top < wa.top) top = cur.y + S(14);
    left = std::max((int)wa.left, std::min(left, (int)wa.right - ow));
    top = std::max((int)wa.top, std::min(top, (int)wa.bottom - oh));
    SetWindowPos(g_win, nullptr, left, top, ow, oh, SWP_NOZORDER | SWP_NOACTIVATE);
}

POINT clientPoint(LPARAM lp) {
    return { (short)LOWORD(lp), (short)HIWORD(lp) };
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(h, 1, 500, nullptr);
        return 0;
    case WM_TIMER:
        if (g_drag.empty() && IsWindowVisible(h)) refresh();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Geo g = geometry();
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, g.w, g.h);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem, g);
        BitBlt(dc, 0, 0, g.w, g.h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        Geo g = geometry();
        Hit hit = hitTest(g, clientPoint(lp));
        if (hit.kind == Hit::Check) {
            for (auto& d : g_view.devices)
                if (d.id == hit.id) hubSelect(hit.id, !d.selected);
            refresh();
        } else if (hit.kind == Hit::Test) {
            hubBeep(hit.id);
        } else if (hit.kind == Hit::Surround) {
            hubSurround(!g_view.surround);
            refresh();
        } else if (hit.kind == Hit::Phone) {
            g_drag = hit.id;
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!g_drag.empty()) {
            Geo g = geometry();
            POINT p = clientPoint(lp);
            double x = (double)(p.x - g.cx) / g.radius;
            double y = (double)(g.cy - p.y) / g.radius;
            hubMove(g_drag, x, y);
            g_view = hubView();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (!g_drag.empty()) {
            g_drag.clear();
            ReleaseCapture();
            hubSave();
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        g_drag.clear();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            Hit hit = hitTest(geometry(), p);
            if (hit.kind != Hit::None) {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(hit.kind == Hit::Phone ? 32646 : 32649)));
                return TRUE;
            }
        }
        break;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HFONT makeFont(int px, int weight) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

}

void uiInit(HWND owner) {
    HDC dc = GetDC(nullptr);
    g_scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.0;
    ReleaseDC(nullptr, dc);
    g_font = makeFont(S(13), FW_NORMAL);
    g_bold = makeFont(S(13), FW_SEMIBOLD);
    g_small = makeFont(S(11), FW_NORMAL);
    g_title = makeFont(S(18), FW_SEMIBOLD);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"SidespeakerPhones";
    RegisterClassExW(&wc);
    g_win = CreateWindowExW(0, wc.lpszClassName, L"Sidespeaker", kStyle, 0, 0, 100, 100, owner, nullptr, inst, nullptr);
}

void uiToggle() {
    if (!g_win) return;
    if (IsWindowVisible(g_win)) {
        ShowWindow(g_win, SW_HIDE);
        return;
    }
    g_view = hubView();
    place();
    InvalidateRect(g_win, nullptr, FALSE);
    ShowWindow(g_win, SW_SHOW);
    SetForegroundWindow(g_win);
}
