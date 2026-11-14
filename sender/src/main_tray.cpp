#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>
#include "config.h"
#include "engine.h"

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_STATUS = WM_APP + 2;
constexpr UINT ID_STATUS = 1;
constexpr UINT ID_EDIT = 2;
constexpr UINT ID_RELOAD = 3;
constexpr UINT ID_QUIT = 4;

std::mutex g_lock;
State g_state = State::Waiting;
std::string g_text = "Starting";

HWND g_wnd = nullptr;
HICON g_icons[3] = {};
NOTIFYICONDATAW g_nid = {};
UINT g_created = 0;
std::filesystem::path g_cfg;
std::atomic<bool> g_stop{false};
std::thread g_worker;

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

HICON makeIcon(int r, int g, int b) {
    const int N = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = N;
    bi.bV5Height = -N;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC dc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    HBITMAP mask = CreateBitmap(N, N, 1, 1, nullptr);

    uint32_t* px = (uint32_t*)bits;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            float dx = x + 0.5f - N / 2.0f;
            float dy = y + 0.5f - N / 2.0f;
            float d = std::sqrt(dx * dx + dy * dy);
            float a = std::fmin(1.0f, std::fmax(0.0f, 14.5f - d));
            float k = d > 11.5f ? 0.6f : 1.0f;
            uint32_t A = (uint32_t)(a * 255.0f);
            px[y * N + x] = (A << 24) | ((uint32_t)(r * k) << 16) | ((uint32_t)(g * k) << 8) | (uint32_t)(b * k);
        }
    }

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

void applyStatus(bool add) {
    State st;
    std::string tx;
    {
        std::lock_guard<std::mutex> l(g_lock);
        st = g_state;
        tx = g_text;
    }
    std::wstring tip = L"Sidespeaker\n" + widen(tx);
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    g_nid.hIcon = g_icons[(int)st];
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid);
}

void startEngine() {
    g_stop = false;
    g_worker = std::thread([] {
        Config cfg(g_cfg);
        runEngine(cfg, [](State s, const std::string& m) {
            {
                std::lock_guard<std::mutex> l(g_lock);
                g_state = s;
                g_text = m;
            }
            PostMessageW(g_wnd, WM_STATUS, 0, 0);
        }, g_stop);
    });
}

void stopEngine() {
    g_stop = true;
    if (g_worker.joinable()) g_worker.join();
}

void showMenu() {
    std::string tx;
    {
        std::lock_guard<std::mutex> l(g_lock);
        tx = g_text;
    }
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DISABLED, ID_STATUS, widen(tx).c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EDIT, L"Edit config");
    AppendMenuW(m, MF_STRING, ID_RELOAD, L"Reload config");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (g_created && msg == g_created) {
        applyStatus(true);
        return 0;
    }
    switch (msg) {
    case WM_STATUS:
        applyStatus(false);
        return 0;
    case WM_TRAY:
        if (l == WM_RBUTTONUP) showMenu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_EDIT:
            ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + g_cfg.wstring() + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            break;
        case ID_RELOAD:
            stopEngine();
            {
                std::lock_guard<std::mutex> lk(g_lock);
                g_state = State::Waiting;
                g_text = "Reloading config";
            }
            applyStatus(false);
            startEngine();
            break;
        case ID_QUIT:
            DestroyWindow(h);
            break;
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}

}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    HANDLE once = CreateMutexW(nullptr, TRUE, L"Sidespeaker");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Sidespeaker is already running. Look for its icon in the system tray.", L"Sidespeaker", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    g_cfg = defaultConfigPath();
    g_icons[0] = makeIcon(140, 140, 140);
    g_icons[1] = makeIcon(46, 184, 92);
    g_icons[2] = makeIcon(220, 60, 60);
    g_created = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SidespeakerTray";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Sidespeaker", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);

    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uCallbackMessage = WM_TRAY;
    applyStatus(true);

    if (Config(g_cfg).getInt("startup_notice", 1) != 0) {
        g_nid.uFlags = NIF_INFO;
        wcscpy_s(g_nid.szInfoTitle, L"Sidespeaker");
        wcscpy_s(g_nid.szInfo, L"Running in the system tray. Hover over the icon to see its status.");
        g_nid.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        g_nid.szInfo[0] = L'\0';
    }

    startEngine();
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    stopEngine();
    return 0;
}
