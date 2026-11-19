#pragma once
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <timeapi.h>
#include <dwmapi.h>
#include <psapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")

#define DWMWA_USE_IMMERSIVE_DARK_MODE_VAL 20

typedef struct {
    double target_cps;
    int is_active;
    int hotkey_vk;
    int sound_enabled;
    int jitter_enabled;
    long long total_clicks;
    double current_live_cps;
    double ram_mb;
    HWND hwnd;
    HANDLE click_thread;
    int should_exit;
} PrmtzClickerState;

static PrmtzClickerState g_state = {
    14.0,       /* target_cps */
    0,          /* is_active */
    VK_F6,      /* hotkey_vk */
    1,          /* sound_enabled */
    1,          /* jitter_enabled */
    0,          /* total_clicks */
    0.0,        /* current_live_cps */
    2.5,        /* ram_mb */
    NULL,
    NULL,
    0
};

static HFONT g_font_title = NULL;
static HFONT g_font_bold = NULL;
static HFONT g_font_regular = NULL;
static HFONT g_font_small = NULL;
static HFONT g_font_mono = NULL;

static int g_is_dragging_slider = 0;
static RECT g_slider_rect = { 30, 160, 330, 172 };
static RECT g_btn_toggle_rect = { 30, 240, 330, 286 };
static RECT g_btn_hotkey_rect = { 30, 305, 170, 342 };
static RECT g_btn_sound_rect = { 190, 305, 330, 342 };
static RECT g_btn_jitter_rect = { 30, 355, 330, 392 };

/* Hotkey cycling options */
static const int HOTKEY_VKS[] = { VK_F6, 'X', 'C', 'V', VK_XBUTTON1 };
static const char* HOTKEY_NAMES[] = { "F6", "X", "C", "V", "Mouse 4" };
static const int HOTKEY_COUNT = 5;
static int g_hotkey_idx = 0;

/* Gaussian random generator (Box-Muller) */
static double generate_gaussian_jitter(double std_dev) {
    static int have_spare = 0;
    static double spare;
    if (have_spare) {
        have_spare = 0;
        return spare * std_dev;
    }
    have_spare = 1;
    double u = ((double)rand() + 1.0) / ((double)RAND_MAX + 1.0);
    double v = ((double)rand() + 1.0) / ((double)RAND_MAX + 1.0);
    double mag = sqrt(-2.0 * log(u));
    spare = mag * sin(2.0 * 3.14159265358979323846 * v);
    return (mag * cos(2.0 * 3.14159265358979323846 * v)) * std_dev;
}

static void update_ram_stat(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        g_state.ram_mb = (double)pmc.WorkingSetSize / (1024.0 * 1024.0);
    }
}

/* Background Clicker Thread */
static DWORD WINAPI clicker_worker_thread(LPVOID param) {
    (void)param;
    timeBeginPeriod(1);
    LARGE_INTEGER freq, t_last_click, t_now, t_last_cps_calc;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t_last_click);
    QueryPerformanceCounter(&t_last_cps_calc);

    long long clicks_in_last_sec = 0;
    int hotkey_was_down = 0;

    while (!g_state.should_exit) {
        /* Check hotkey transition */
        int hotkey_is_down = (GetAsyncKeyState(g_state.hotkey_vk) & 0x8000) != 0;
        if (hotkey_is_down && !hotkey_was_down) {
            /* Toggle clicker */
            g_state.is_active = !g_state.is_active;
            if (g_state.sound_enabled) {
                MessageBeep(g_state.is_active ? MB_OK : MB_ICONASTERISK);
            }
            if (g_state.hwnd) {
                InvalidateRect(g_state.hwnd, NULL, FALSE);
            }
        }
        hotkey_was_down = hotkey_is_down;

        /* Calculate live CPS every 500ms */
        QueryPerformanceCounter(&t_now);
        double elapsed_since_cps = (double)(t_now.QuadPart - t_last_cps_calc.QuadPart) / (double)freq.QuadPart;
        if (elapsed_since_cps >= 0.5) {
            g_state.current_live_cps = g_state.is_active ? ((double)clicks_in_last_sec / elapsed_since_cps) : 0.0;
            clicks_in_last_sec = 0;
            t_last_cps_calc = t_now;
            update_ram_stat();
            if (g_state.hwnd) {
                InvalidateRect(g_state.hwnd, NULL, FALSE);
            }
        }

        if (g_state.is_active) {
            double target = g_state.target_cps;
            if (target < 6.0) target = 6.0;
            if (target > 25.0) target = 25.0;

            double base_interval_sec = 1.0 / target;
            double jitter_sec = 0.0;
            if (g_state.jitter_enabled) {
                jitter_sec = generate_gaussian_jitter(base_interval_sec * 0.12);
            }
            double total_delay_sec = base_interval_sec + jitter_sec;
            if (total_delay_sec < 0.038) total_delay_sec = 0.038; /* max ~26 CPS */

            double elapsed_since_click = (double)(t_now.QuadPart - t_last_click.QuadPart) / (double)freq.QuadPart;
            if (elapsed_since_click >= total_delay_sec) {
                /* Dispatch left mouse click */
                INPUT inputs[2];
                ZeroMemory(inputs, sizeof(inputs));

                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                SendInput(1, &inputs[0], sizeof(INPUT));

                /* Natural click hold duration (14 - 22 ms) */
                Sleep(16);

                inputs[1].type = INPUT_MOUSE;
                inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                SendInput(1, &inputs[1], sizeof(INPUT));

                QueryPerformanceCounter(&t_last_click);
                g_state.total_clicks++;
                clicks_in_last_sec++;
            }
        }

        Sleep(2);
    }

    timeEndPeriod(1);
    return 0;
}

static void draw_rounded_card(HDC hdc, int x, int y, int w, int h, COLORREF bg, COLORREF border) {
    HPEN hPen = CreatePen(PS_SOLID, 1, border);
    HBRUSH hBrush = CreateSolidBrush(bg);
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, hBrush);

    RoundRect(hdc, x, y, x + w, y + h, 14, 14);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(hPen);
    DeleteObject(hBrush);
}

/* Double-buffered GDI Paint Routine */
static void on_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdcWindow = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    HDC hdc = CreateCompatibleDC(hdcWindow);
    HBITMAP hbm = CreateCompatibleBitmap(hdcWindow, width, height);
    HGDIOBJ oldBm = SelectObject(hdc, hbm);

    SetBkMode(hdc, TRANSPARENT);

    /* Background: Matte dark #09090b */
    HBRUSH bgBrush = CreateSolidBrush(RGB(9, 9, 11));
    FillRect(hdc, &rc, bgBrush);
    DeleteObject(bgBrush);

    /* 1. Header: Dynamic Island Pill */
    int pill_w = 230;
    int pill_h = 34;
    int pill_x = (width - pill_w) / 2;
    int pill_y = 16;
    draw_rounded_card(hdc, pill_x, pill_y, pill_w, pill_h, RGB(24, 24, 27), RGB(39, 39, 42));

    /* Pulsing status LED in pill */
    COLORREF led_color = g_state.is_active ? RGB(16, 185, 129) : RGB(113, 113, 122);
    HBRUSH ledBrush = CreateSolidBrush(led_color);
    HPEN ledPen = CreatePen(PS_SOLID, 1, led_color);
    SelectObject(hdc, ledBrush);
    SelectObject(hdc, ledPen);
    Ellipse(hdc, pill_x + 16, pill_y + 12, pill_x + 26, pill_y + 22);
    DeleteObject(ledBrush);
    DeleteObject(ledPen);

    SelectObject(hdc, g_font_bold);
    SetTextColor(hdc, RGB(255, 255, 255));
    RECT pillTextRc = { pill_x + 32, pill_y + 8, pill_x + pill_w, pill_y + pill_h };
    DrawTextA(hdc, "promotezz", -1, &pillTextRc, DT_LEFT | DT_SINGLELINE);

    SelectObject(hdc, g_font_regular);
    SetTextColor(hdc, RGB(161, 161, 170));
    RECT pillSubRc = { pill_x + 115, pill_y + 8, pill_x + pill_w, pill_y + pill_h };
    DrawTextA(hdc, "· autoclicker", -1, &pillSubRc, DT_LEFT | DT_SINGLELINE);

    /* 2. Status Card (Active / Idle) */
    int card_y = 66;
    if (g_state.is_active) {
        draw_rounded_card(hdc, 30, card_y, 300, 52, RGB(6, 78, 59), RGB(16, 185, 129));
        SelectObject(hdc, g_font_bold);
        SetTextColor(hdc, RGB(209, 250, 229));
        RECT stRc = { 30, card_y + 10, 330, card_y + 30 };
        DrawTextA(hdc, "ACTIVE · CLICKING ENGAGED", -1, &stRc, DT_CENTER | DT_SINGLELINE);

        SelectObject(hdc, g_font_small);
        SetTextColor(hdc, RGB(167, 243, 208));
        char subText[64];
        sprintf_s(subText, sizeof(subText), "Pacing ~%.1f CPS · Press [%s] to pause", g_state.target_cps, HOTKEY_NAMES[g_hotkey_idx]);
        RECT stSub = { 30, card_y + 30, 330, card_y + 48 };
        DrawTextA(hdc, subText, -1, &stSub, DT_CENTER | DT_SINGLELINE);
    } else {
        draw_rounded_card(hdc, 30, card_y, 300, 52, RGB(18, 18, 21), RGB(39, 39, 42));
        SelectObject(hdc, g_font_bold);
        SetTextColor(hdc, RGB(244, 244, 245));
        RECT stRc = { 30, card_y + 10, 330, card_y + 30 };
        DrawTextA(hdc, "IDLE · READY TO ENGAGE", -1, &stRc, DT_CENTER | DT_SINGLELINE);

        SelectObject(hdc, g_font_small);
        SetTextColor(hdc, RGB(113, 113, 122));
        char subText[64];
        sprintf_s(subText, sizeof(subText), "Toggle Key: [%s] · Win32 Low-Level Dispatch", HOTKEY_NAMES[g_hotkey_idx]);
        RECT stSub = { 30, card_y + 30, 330, card_y + 48 };
        DrawTextA(hdc, subText, -1, &stSub, DT_CENTER | DT_SINGLELINE);
    }

    /* 3. CPS Slider Section */
    SelectObject(hdc, g_font_bold);
    SetTextColor(hdc, RGB(255, 255, 255));
    RECT lblCps = { 30, 134, 180, 154 };
    DrawTextA(hdc, "Click Speed", -1, &lblCps, DT_LEFT | DT_SINGLELINE);

    SelectObject(hdc, g_font_mono);
    SetTextColor(hdc, RGB(16, 185, 129));
    char cpsValStr[32];
    sprintf_s(cpsValStr, sizeof(cpsValStr), "%.1f CPS", g_state.target_cps);
    RECT valCps = { 200, 134, 330, 154 };
    DrawTextA(hdc, cpsValStr, -1, &valCps, DT_RIGHT | DT_SINGLELINE);

    /* Slider Track */
    int track_x = g_slider_rect.left;
    int track_y = g_slider_rect.top;
    int track_w = g_slider_rect.right - g_slider_rect.left;
    int track_h = 8;
    draw_rounded_card(hdc, track_x, track_y, track_w, track_h, RGB(39, 39, 42), RGB(39, 39, 42));

    /* Active Fill */
    double pct = (g_state.target_cps - 6.0) / (24.0 - 6.0);
    if (pct < 0.0) pct = 0.0;
    if (pct > 1.0) pct = 1.0;
    int fill_w = (int)(track_w * pct);
    if (fill_w > 4) {
        draw_rounded_card(hdc, track_x, track_y, fill_w, track_h, RGB(255, 255, 255), RGB(255, 255, 255));
    }

    /* Slider Knob */
    int knob_cx = track_x + fill_w;
    int knob_cy = track_y + (track_h / 2);
    int knob_r = 9;
    HBRUSH knobBrush = CreateSolidBrush(RGB(255, 255, 255));
    HPEN knobPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
    SelectObject(hdc, knobBrush);
    SelectObject(hdc, knobPen);
    Ellipse(hdc, knob_cx - knob_r, knob_cy - knob_r, knob_cx + knob_r, knob_cy + knob_r);
    DeleteObject(knobBrush);
    DeleteObject(knobPen);

    /* 4. Main Toggle Button */
    if (g_state.is_active) {
        draw_rounded_card(hdc, g_btn_toggle_rect.left, g_btn_toggle_rect.top,
                          g_btn_toggle_rect.right - g_btn_toggle_rect.left,
                          g_btn_toggle_rect.bottom - g_btn_toggle_rect.top,
                          RGB(220, 38, 38), RGB(239, 68, 68));
        SelectObject(hdc, g_font_bold);
        SetTextColor(hdc, RGB(255, 255, 255));
        RECT btnRc = g_btn_toggle_rect;
        btnRc.top += 12;
        DrawTextA(hdc, "STOP CLICKER (F6)", -1, &btnRc, DT_CENTER | DT_SINGLELINE);
    } else {
        draw_rounded_card(hdc, g_btn_toggle_rect.left, g_btn_toggle_rect.top,
                          g_btn_toggle_rect.right - g_btn_toggle_rect.left,
                          g_btn_toggle_rect.bottom - g_btn_toggle_rect.top,
                          RGB(255, 255, 255), RGB(255, 255, 255));
        SelectObject(hdc, g_font_bold);
        SetTextColor(hdc, RGB(9, 9, 11));
        RECT btnRc = g_btn_toggle_rect;
        btnRc.top += 12;
        DrawTextA(hdc, "START CLICKER (F6)", -1, &btnRc, DT_CENTER | DT_SINGLELINE);
    }

    /* 5. Hotkey & Audio Buttons Row */
    /* Hotkey Pill */
    draw_rounded_card(hdc, g_btn_hotkey_rect.left, g_btn_hotkey_rect.top,
                      g_btn_hotkey_rect.right - g_btn_hotkey_rect.left,
                      g_btn_hotkey_rect.bottom - g_btn_hotkey_rect.top,
                      RGB(18, 18, 21), RGB(39, 39, 42));
    SelectObject(hdc, g_font_small);
    SetTextColor(hdc, RGB(161, 161, 170));
    RECT hkRc = g_btn_hotkey_rect;
    hkRc.top += 9;
    char hkLabel[32];
    sprintf_s(hkLabel, sizeof(hkLabel), "Key: [%s]", HOTKEY_NAMES[g_hotkey_idx]);
    DrawTextA(hdc, hkLabel, -1, &hkRc, DT_CENTER | DT_SINGLELINE);

    /* Audio Pill */
    draw_rounded_card(hdc, g_btn_sound_rect.left, g_btn_sound_rect.top,
                      g_btn_sound_rect.right - g_btn_sound_rect.left,
                      g_btn_sound_rect.bottom - g_btn_sound_rect.top,
                      RGB(18, 18, 21), RGB(39, 39, 42));
    SelectObject(hdc, g_font_small);
    SetTextColor(hdc, g_state.sound_enabled ? RGB(16, 185, 129) : RGB(113, 113, 122));
    RECT sndRc = g_btn_sound_rect;
    sndRc.top += 9;
    DrawTextA(hdc, g_state.sound_enabled ? "Audio: ON" : "Audio: MUTE", -1, &sndRc, DT_CENTER | DT_SINGLELINE);

    /* 6. Humanized Jitter Button */
    draw_rounded_card(hdc, g_btn_jitter_rect.left, g_btn_jitter_rect.top,
                      g_btn_jitter_rect.right - g_btn_jitter_rect.left,
                      g_btn_jitter_rect.bottom - g_btn_jitter_rect.top,
                      RGB(18, 18, 21), RGB(39, 39, 42));
    SelectObject(hdc, g_font_small);
    SetTextColor(hdc, g_state.jitter_enabled ? RGB(16, 185, 129) : RGB(113, 113, 122));
    RECT jitRc = g_btn_jitter_rect;
    jitRc.top += 9;
    DrawTextA(hdc, g_state.jitter_enabled ? "Gaussian Jitter: ENABLED (Anti-Pattern)" : "Gaussian Jitter: DISABLED (Flat)",
              -1, &jitRc, DT_CENTER | DT_SINGLELINE);

    /* 7. Realtime Telemetry Stats Card */
    int stats_y = 405;
    draw_rounded_card(hdc, 30, stats_y, 300, 58, RGB(18, 18, 21), RGB(39, 39, 42));

    /* 3 Columns: Clicks | Live CPS | RAM */
    SelectObject(hdc, g_font_small);
    SetTextColor(hdc, RGB(113, 113, 122));
    RECT col1_lbl = { 35, stats_y + 8, 125, stats_y + 24 };
    RECT col2_lbl = { 135, stats_y + 8, 225, stats_y + 24 };
    RECT col3_lbl = { 235, stats_y + 8, 325, stats_y + 24 };
    DrawTextA(hdc, "CLICKS", -1, &col1_lbl, DT_CENTER | DT_SINGLELINE);
    DrawTextA(hdc, "LIVE SPEED", -1, &col2_lbl, DT_CENTER | DT_SINGLELINE);
    DrawTextA(hdc, "RAM USAGE", -1, &col3_lbl, DT_CENTER | DT_SINGLELINE);

    SelectObject(hdc, g_font_mono);
    SetTextColor(hdc, RGB(255, 255, 255));
    char clicksBuf[32], liveCpsBuf[32], ramBuf[32];
    sprintf_s(clicksBuf, sizeof(clicksBuf), "%lld", g_state.total_clicks);
    sprintf_s(liveCpsBuf, sizeof(liveCpsBuf), "%.1f", g_state.current_live_cps);
    sprintf_s(ramBuf, sizeof(ramBuf), "%.1f MB", g_state.ram_mb);

    RECT col1_val = { 35, stats_y + 28, 125, stats_y + 50 };
    RECT col2_val = { 135, stats_y + 28, 225, stats_y + 50 };
    RECT col3_val = { 235, stats_y + 28, 325, stats_y + 50 };
    DrawTextA(hdc, clicksBuf, -1, &col1_val, DT_CENTER | DT_SINGLELINE);
    DrawTextA(hdc, liveCpsBuf, -1, &col2_val, DT_CENTER | DT_SINGLELINE);
    SetTextColor(hdc, RGB(16, 185, 129)); /* Green for < 10 MB badge! */
    DrawTextA(hdc, ramBuf, -1, &col3_val, DT_CENTER | DT_SINGLELINE);

    /* 8. Footer Link */
    SelectObject(hdc, g_font_small);
    SetTextColor(hdc, RGB(82, 82, 91));
    RECT ftrRc = { 30, 475, 330, 495 };
    DrawTextA(hdc, "promotezz.cc · Free & Open Source · Win32", -1, &ftrRc, DT_CENTER | DT_SINGLELINE);

    /* Blit to screen */
    BitBlt(hdcWindow, 0, 0, width, height, hdc, 0, 0, SRCCOPY);

    SelectObject(hdc, oldBm);
    DeleteObject(hbm);
    DeleteDC(hdc);
    EndPaint(hwnd, &ps);
}

static void update_slider_from_mouse(int mouse_x) {
    int track_x = g_slider_rect.left;
    int track_w = g_slider_rect.right - g_slider_rect.left;
    double ratio = (double)(mouse_x - track_x) / (double)track_w;
    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    double new_cps = 6.0 + ratio * (24.0 - 6.0);
    g_state.target_cps = floor(new_cps * 10.0 + 0.5) / 10.0;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT:
        on_paint(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1; /* Handled in on_paint to prevent flicker */

    case WM_LBUTTONDOWN: {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        POINT pt = { x, y };

        /* Slider hit */
        RECT slider_hit_area = g_slider_rect;
        slider_hit_area.top -= 10;
        slider_hit_area.bottom += 10;
        if (PtInRect(&slider_hit_area, pt)) {
            g_is_dragging_slider = 1;
            SetCapture(hwnd);
            update_slider_from_mouse(x);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* Toggle Button */
        if (PtInRect(&g_btn_toggle_rect, pt)) {
            g_state.is_active = !g_state.is_active;
            if (g_state.sound_enabled) {
                MessageBeep(g_state.is_active ? MB_OK : MB_ICONASTERISK);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* Hotkey Selector */
        if (PtInRect(&g_btn_hotkey_rect, pt)) {
            g_hotkey_idx = (g_hotkey_idx + 1) % HOTKEY_COUNT;
            g_state.hotkey_vk = HOTKEY_VKS[g_hotkey_idx];
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* Audio Toggle */
        if (PtInRect(&g_btn_sound_rect, pt)) {
            g_state.sound_enabled = !g_state.sound_enabled;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        /* Jitter Toggle */
        if (PtInRect(&g_btn_jitter_rect, pt)) {
            g_state.jitter_enabled = !g_state.jitter_enabled;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE:
        if (g_is_dragging_slider) {
            int x = LOWORD(lParam);
            update_slider_from_mouse(x);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (g_is_dragging_slider) {
            g_is_dragging_slider = 0;
            ReleaseCapture();
            return 0;
        }
        break;

    case WM_DESTROY:
        g_state.should_exit = 1;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* Public API called from Goose */
static void prmtz_gui_init(double initial_cps) {
    g_state.target_cps = initial_cps;
    HINSTANCE hInstance = GetModuleHandleA(NULL);

    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "PrmtzAutoClickerClass";
    RegisterClassExA(&wc);

    int w = 376;
    int h = 540;
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int pos_x = (screen_w - w) / 2;
    int pos_y = (screen_h - h) / 2;

    HWND hwnd = CreateWindowExA(
        WS_EX_APPWINDOW,
        "PrmtzAutoClickerClass",
        "promotezz · autoclicker",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        pos_x, pos_y, w, h,
        NULL, NULL, hInstance, NULL
    );

    g_state.hwnd = hwnd;

    /* Enable DWM Dark Mode */
    BOOL use_dark_mode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_VAL, &use_dark_mode, sizeof(use_dark_mode));

    /* Create Typography */
    g_font_title = CreateFontA(-18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_font_bold = CreateFontA(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_font_regular = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_font_small = CreateFontA(-11, 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_font_mono = CreateFontA(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    /* Launch background clicking worker thread */
    g_state.click_thread = CreateThread(NULL, 0, clicker_worker_thread, NULL, 0, NULL);

    /* Optimize initial working set size to stay under 4 MB */
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    update_ram_stat();
}

static int prmtz_gui_poll_messages(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return 0;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 1;
}

static void prmtz_gui_cleanup(void) {
    g_state.should_exit = 1;
    if (g_state.click_thread) {
        WaitForSingleObject(g_state.click_thread, 1000);
        CloseHandle(g_state.click_thread);
        g_state.click_thread = NULL;
    }
    if (g_font_title) DeleteObject(g_font_title);
    if (g_font_bold) DeleteObject(g_font_bold);
    if (g_font_regular) DeleteObject(g_font_regular);
    if (g_font_small) DeleteObject(g_font_small);
    if (g_font_mono) DeleteObject(g_font_mono);
}

static double prmtz_get_ram_usage_mb(void) {
    update_ram_stat();
    return g_state.ram_mb;
}

static long long prmtz_get_click_count(void) {
    return g_state.total_clicks;
}
