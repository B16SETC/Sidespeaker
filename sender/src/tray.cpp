#include "tray.h"
#include "ui.h"
#include "util.h"
#include <windows.h>
#include <shellapi.h>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <thread>
#include <utility>

namespace {

constexpr UINT kTrayMsg = WM_APP + 1;
constexpr UINT kStatusMsg = WM_APP + 2;
constexpr UINT kIconId = 1;
constexpr UINT kCmdOpen = 1000;
constexpr UINT kCmdConfig = 1001;
constexpr UINT kCmdExit = 1002;

std::mutex g_mu;
TrayState g_state = TrayState::Idle;
std::string g_text = "starting...";
HWND g_hwnd = nullptr;
HICON g_icons[3] = {};
UINT g_taskbarCreated = 0;
std::wstring g_configPath;

std::wstring statusLine() {
    std::string tx;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        tx = g_text;
    }
    std::wstring w = widen(tx);
    if (!w.empty()) w[0] = (wchar_t)std::towupper(w[0]);
    return w;
}

HICON makeIcon(BYTE r, BYTE g, BYTE b) {
    int sz = GetSystemMetrics(SM_CXSMICON);
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = sz;
    bi.bV5Height = -sz;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);

    double c = sz / 2.0;
    double rad = c - 1.0;
    DWORD* px = (DWORD*)bits;
    for (int y = 0; y < sz; ++y) {
        for (int x = 0; x < sz; ++x) {
            double dx = x + 0.5 - c, dy = y + 0.5 - c;
            double d = std::sqrt(dx * dx + dy * dy);
            double cov = rad + 0.5 - d;
            cov = cov < 0 ? 0 : (cov > 1 ? 1 : cov);
            DWORD a = (DWORD)(cov * 255.0);
            px[y * sz + x] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
        }
    }

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

void notify(DWORD msg) {
    TrayState st;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        st = g_state;
    }
    std::wstring tip = L"Sidespeaker\n" + statusLine();
    NOTIFYICONDATAW n = {};
    n.cbSize = sizeof n;
    n.hWnd = g_hwnd;
    n.uID = kIconId;
    n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    n.uCallbackMessage = kTrayMsg;
    n.hIcon = g_icons[(int)st];
    lstrcpynW(n.szTip, tip.c_str(), (int)(sizeof n.szTip / sizeof n.szTip[0]));
    Shell_NotifyIconW(msg, &n);
}

void showMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, statusLine().c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kCmdOpen, L"Phones and surround");
    AppendMenuW(m, MF_STRING, kCmdConfig, L"Edit config");
    AppendMenuW(m, MF_STRING, kCmdExit, L"Exit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD, p.x, p.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == kCmdOpen) uiToggle();
    if (cmd == kCmdConfig) ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + g_configPath + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    if (cmd == kCmdExit) DestroyWindow(g_hwnd);
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && g_taskbarCreated != 0) {
        notify(NIM_ADD);
        return 0;
    }
    switch (msg) {
    case kStatusMsg:
        notify(NIM_MODIFY);
        return 0;
    case kTrayMsg:
        if (LOWORD(lp) == WM_LBUTTONUP) uiToggle();
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) showMenu();
        return 0;
    case WM_DESTROY: {
        NOTIFYICONDATAW n = {};
        n.cbSize = sizeof n;
        n.hWnd = h;
        n.uID = kIconId;
        Shell_NotifyIconW(NIM_DELETE, &n);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}

void setStatus(TrayState state, const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_state == state && g_text == text) return;
        g_state = state;
        g_text = text;
    }
    if (g_hwnd) PostMessageW(g_hwnd, kStatusMsg, 0, 0);
}

int runTray(const std::string& configPath, std::function<void()> worker) {
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Local\\SidespeakerSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_configPath = widen(configPath);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SidespeakerTray";
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Sidespeaker", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;
    uiInit(g_hwnd);

    g_icons[(int)TrayState::Idle] = makeIcon(150, 150, 150);
    g_icons[(int)TrayState::Active] = makeIcon(46, 190, 80);
    g_icons[(int)TrayState::Error] = makeIcon(220, 60, 50);
    notify(NIM_ADD);

    std::thread(std::move(worker)).detach();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    ReleaseMutex(once);
    ExitProcess(0);
}
