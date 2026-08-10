// See pin_overlay.h. A layered, topmost, click-through window centered on the
// primary display, drawn with GDI (no D3D/Vulkan dependency - the receivers
// are separate processes from the renderer and must not touch its device).

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "pin_overlay.h"

namespace vkr {
namespace {

constexpr wchar_t kClass[] = L"vkrenderPinOverlay";
constexpr int kWidth = 720;
constexpr int kHeight = 300;

std::mutex g_mutex;
HWND g_hwnd = nullptr;
std::thread g_thread;
std::atomic<bool> g_running{false};
std::wstring g_pin, g_label;

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out((size_t)(n > 0 ? n - 1 : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

// Renders the overlay. MUST NOT be called with g_mutex held (it reads the
// text under the lock itself; nesting deadlocks - found in testing when the
// second connection attempt hung forever).
void paint(HWND hwnd) {
    std::wstring pin, label;
    {
        std::lock_guard<std::mutex> hold(g_mutex);
        pin = g_pin;
        label = g_label;
    }

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = kWidth;
    bi.bmiHeader.biHeight = -kHeight; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    // Rounded charcoal panel; premultiplied alpha for UpdateLayeredWindow.
    RECT full{0, 0, kWidth, kHeight};
    HBRUSH bg = CreateSolidBrush(RGB(18, 18, 24));
    FillRect(dc, &full, bg);
    DeleteObject(bg);

    SetBkMode(dc, TRANSPARENT);
    HFONT small = CreateFontW(38, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT big = CreateFontW(150, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                            CLEARTYPE_QUALITY, 0, L"Segoe UI");

    std::wstring header = label.empty() ? L"Enter this PIN on your device"
                                        : label + L" - enter this PIN on your device";
    SelectObject(dc, small);
    SetTextColor(dc, RGB(210, 210, 225));
    RECT hr{0, 34, kWidth, 100};
    DrawTextW(dc, header.c_str(), -1, &hr, DT_CENTER | DT_SINGLELINE);

    SelectObject(dc, big);
    SetTextColor(dc, RGB(255, 95, 162)); // vaporwave pink, matches the scene
    RECT pr{0, 96, kWidth, 260};
    DrawTextW(dc, pin.c_str(), -1, &pr, DT_CENTER | DT_SINGLELINE);

    // Opaque alpha everywhere we painted (GDI text leaves alpha at 0).
    auto* px = (unsigned char*)bits;
    for (int i = 0; i < kWidth * kHeight; ++i) px[i * 4 + 3] = 235;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + ((work.right - work.left) - kWidth) / 2;
    int y = work.top + (int)(((work.bottom - work.top) - kHeight) * 0.22);
    POINT dst{x, y};
    SIZE size{kWidth, kHeight};
    POINT src{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screen, &dst, &size, dc, &src, 0, &blend, ULW_ALPHA);

    DeleteObject(big);
    DeleteObject(small);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

// All window work happens on the UI thread; other threads only post these.
constexpr UINT kMsgShow = WM_APP + 1;
constexpr UINT kMsgHide = WM_APP + 2;

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case kMsgShow:
            paint(hwnd);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            return 0;
        case kMsgHide:
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void uiThread() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    // Diagnostics, not decoration: the overlay silently failed to appear on a
    // real cast and the failure was invisible - display_pin fired, this thread
    // ran, and no window ever existed. A bare `return` on failure gave nothing
    // to go on, so report WHY.
    if (!RegisterClassExW(&wc)) {
        DWORD e = GetLastError();
        // ERROR_CLASS_ALREADY_EXISTS is fine; anything else is fatal.
        if (e != ERROR_CLASS_ALREADY_EXISTS)
            fprintf(stderr, "pin overlay: RegisterClassExW failed (%lu)\n", e);
    }

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClass, L"vkrender PIN", WS_POPUP, 0, 0, kWidth, kHeight, nullptr, nullptr,
        wc.hInstance, nullptr);
    if (!hwnd) {
        fprintf(stderr, "pin overlay: CreateWindowExW failed (%lu)\n", GetLastError());
        g_running = false; // let a later attempt retry instead of wedging
        return;
    }
    fprintf(stderr, "pin overlay: window %p created\n", (void*)hwnd);
    {
        std::lock_guard<std::mutex> hold(g_mutex);
        g_hwnd = hwnd;
    }
    paint(hwnd);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    MSG msg;
    while (g_running.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    {
        std::lock_guard<std::mutex> hold(g_mutex);
        g_hwnd = nullptr;
    }
    DestroyWindow(hwnd);
    UnregisterClassW(kClass, wc.hInstance);
}

} // namespace

bool pinEnabled() {
    const char* v = getenv("VKR_PIN");
    if (!v || !*v) return false;
    return !(strcmp(v, "off") == 0 || strcmp(v, "0") == 0);
}

std::string pinForConnection() {
    const char* v = getenv("VKR_PIN");
    if (!pinEnabled()) return "";
    if (strcmp(v, "auto") == 0 || getenv("VKR_PIN_AUTO")) {
        static std::mt19937 rng((unsigned)GetTickCount64());
        std::uniform_int_distribution<int> d(0, 9999);
        char buf[8];
        snprintf(buf, sizeof(buf), "%04d", d(rng));
        return buf;
    }
    return v; // fixed PIN
}

// Cross-thread rule: callers (protocol threads) only update the text and
// POST to the UI thread. Touching another thread's window directly wedged
// the receiver in testing - the connection thread blocked inside USER32 and
// the HTTP request never completed.
void pinOverlayShow(const std::string& pin, const std::string& label) {
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> hold(g_mutex);
        g_pin = widen(pin);
        g_label = widen(label);
        hwnd = g_hwnd;
    }
    if (hwnd) {
        PostMessageW(hwnd, kMsgShow, 0, 0);
        return;
    }
    if (!g_running.exchange(true)) g_thread = std::thread(uiThread);
}

void pinOverlayHide() {
    HWND hwnd;
    {
        std::lock_guard<std::mutex> hold(g_mutex);
        hwnd = g_hwnd;
    }
    if (hwnd) PostMessageW(hwnd, kMsgHide, 0, 0);
}

} // namespace vkr

#endif // _WIN32
