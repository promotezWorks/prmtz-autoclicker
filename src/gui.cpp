#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <unknwn.h>
#include <timeapi.h>
#include <dwmapi.h>
#include <psapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <gdiplus.h>

#include "gui.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

#define DWMWA_USE_IMMERSIVE_DARK_MODE_VAL 20
#define DWMWA_WINDOW_CORNER_PREFERENCE_VAL 33
#define DWMWCP_ROUND_VAL 2

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
    1.8,        /* ram_mb */
    NULL,
    NULL,
    0
};

static ULONG_PTR g_gdiplusToken = 0;

/* Hotkey cycling options */
static const int HOTKEY_VKS[] = { VK_F6, 'X', 'C', 'V', VK_XBUTTON1 };
static const char* HOTKEY_NAMES[] = { "F6", "X", "C", "V", "Mouse 4" };
static const int HOTKEY_COUNT = 5;
static int g_hotkey_idx = 0;

/* Interactive Control Rectangles */
static int g_is_dragging_slider = 0;
static RECT g_slider_rect = { 32, 180, 348, 192 };
static RECT g_btn_toggle_rect = { 32, 248, 348, 294 };
static RECT g_btn_hotkey_rect = { 32, 306, 184, 346 };
static RECT g_btn_sound_rect = { 196, 306, 348, 346 };
static RECT g_btn_jitter_rect = { 32, 358, 348, 398 };
static RECT g_btn_close_rect = { 342, 12, 368, 36 };
static RECT g_btn_min_rect = { 312, 12, 338, 36 };

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

/* Background Clicker Worker Thread */
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
        int hotkey_is_down = (GetAsyncKeyState(g_state.hotkey_vk) & 0x8000) != 0;
        if (hotkey_is_down && !hotkey_was_down) {
            g_state.is_active = !g_state.is_active;
            if (g_state.sound_enabled) {
                MessageBeep(g_state.is_active ? MB_OK : MB_ICONASTERISK);
            }
            if (g_state.hwnd) {
                InvalidateRect(g_state.hwnd, NULL, FALSE);
            }
        }
        hotkey_was_down = hotkey_is_down;

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
            if (target > 24.0) target = 24.0;

            double base_interval_sec = 1.0 / target;
            double jitter_sec = 0.0;
            if (g_state.jitter_enabled) {
                jitter_sec = generate_gaussian_jitter(base_interval_sec * 0.12);
            }
            double total_delay_sec = base_interval_sec + jitter_sec;
            if (total_delay_sec < 0.038) total_delay_sec = 0.038;

            double elapsed_since_click = (double)(t_now.QuadPart - t_last_click.QuadPart) / (double)freq.QuadPart;

            if (elapsed_since_click >= total_delay_sec) {
                INPUT inputs[2];
                ZeroMemory(inputs, sizeof(inputs));

                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                SendInput(1, &inputs[0], sizeof(INPUT));

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

static void add_rounded_rect_path(GraphicsPath* path, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    path->AddArc(x, y, d, d, 180.0f, 90.0f);
    path->AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path->AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path->AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path->CloseFigure();
}

static void fill_rounded_card(Graphics* g, Brush* brush, float x, float y, float w, float h, float r) {
    GraphicsPath path;
    add_rounded_rect_path(&path, x, y, w, h, r);
    g->FillPath(brush, &path);
}

static void draw_rounded_card(Graphics* g, Brush* brush, Pen* pen, float x, float y, float w, float h, float r) {
    GraphicsPath path;
    add_rounded_rect_path(&path, x, y, w, h, r);
    if (brush) g->FillPath(brush, &path);
    if (pen) g->DrawPath(pen, &path);
}

static void draw_text(Graphics* g, const WCHAR* text, const Gdiplus::Font* font, const Brush* brush, const RectF& rect, const StringFormat* format) {
    g->DrawString(text, -1, font, rect, format, brush);
}

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

    {
        Graphics g(hdc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        // 1. Base Canvas: #09090b
        SolidBrush bgBrush(Color(255, 9, 9, 11));
        g.FillRectangle(&bgBrush, 0, 0, width, height);

        // Ambient radial emerald hero glow behind center
        GraphicsPath glowPath;
        glowPath.AddEllipse(20.0f, 40.0f, (float)(width - 40), (float)(height - 100));
        PathGradientBrush pgb(&glowPath);
        pgb.SetCenterColor(Color(28, 16, 185, 129));
        Color surroundColors[] = { Color(0, 9, 9, 11) };
        int count = 1;
        pgb.SetSurroundColors(surroundColors, &count);
        g.FillPath(&pgb, &glowPath);

        // Typography
        FontFamily ffSegoe(L"Segoe UI");
        FontFamily ffConsolas(L"Consolas");
        Gdiplus::Font fontTitle(&ffSegoe, 11.5f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontHeading(&ffSegoe, 13.0f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontBody(&ffSegoe, 9.5f, FontStyleRegular, UnitPoint);
        Gdiplus::Font fontSmall(&ffSegoe, 8.5f, FontStyleRegular, UnitPoint);
        Gdiplus::Font fontMono(&ffConsolas, 10.5f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontMonoLarge(&ffConsolas, 13.5f, FontStyleBold, UnitPoint);

        // Colors
        SolidBrush whiteBrush(Color(255, 255, 255, 255));
        SolidBrush darkBrush(Color(255, 9, 9, 11));
        SolidBrush zinc300(Color(255, 212, 212, 216));
        SolidBrush zinc400(Color(255, 161, 161, 170));
        SolidBrush zinc500(Color(255, 113, 113, 122));
        SolidBrush zinc600(Color(255, 82, 82, 91));
        SolidBrush emerald(Color(255, 16, 185, 129));
        SolidBrush ruby(Color(255, 239, 68, 68));

        SolidBrush glassCardBg(Color(18, 255, 255, 255));
        Pen glassBorder(Color(32, 255, 255, 255), 1.0f);

        StringFormat sfCenter;
        sfCenter.SetAlignment(StringAlignmentCenter);
        sfCenter.SetLineAlignment(StringAlignmentCenter);

        StringFormat sfLeft;
        sfLeft.SetAlignment(StringAlignmentNear);
        sfLeft.SetLineAlignment(StringAlignmentCenter);

        StringFormat sfRight;
        sfRight.SetAlignment(StringAlignmentFar);
        sfRight.SetLineAlignment(StringAlignmentCenter);

        // 2. Window Controls (Minimize & Close)
        SolidBrush ctrlBg(Color(20, 255, 255, 255));
        fill_rounded_card(&g, &ctrlBg, (float)g_btn_min_rect.left, (float)g_btn_min_rect.top,
                          (float)(g_btn_min_rect.right - g_btn_min_rect.left),
                          (float)(g_btn_min_rect.bottom - g_btn_min_rect.top), 12.0f);
        draw_text(&g, L"—", &fontTitle, &zinc400,
                  RectF((float)g_btn_min_rect.left, (float)g_btn_min_rect.top,
                        (float)(g_btn_min_rect.right - g_btn_min_rect.left),
                        (float)(g_btn_min_rect.bottom - g_btn_min_rect.top)), &sfCenter);

        fill_rounded_card(&g, &ctrlBg, (float)g_btn_close_rect.left, (float)g_btn_close_rect.top,
                          (float)(g_btn_close_rect.right - g_btn_close_rect.left),
                          (float)(g_btn_close_rect.bottom - g_btn_close_rect.top), 12.0f);
        draw_text(&g, L"✕", &fontTitle, &zinc400,
                  RectF((float)g_btn_close_rect.left, (float)g_btn_close_rect.top,
                        (float)(g_btn_close_rect.right - g_btn_close_rect.left),
                        (float)(g_btn_close_rect.bottom - g_btn_close_rect.top)), &sfCenter);

        // 3. Dynamic Island Nav Capsule (promotezz.cc)
        float pillW = 224.0f;
        float pillH = 34.0f;
        float pillX = ((float)width - pillW) / 2.0f;
        float pillY = 12.0f;

        SolidBrush islandBg(Color(245, 9, 9, 11));
        Pen islandBorder(Color(45, 255, 255, 255), 1.0f);
        draw_rounded_card(&g, &islandBg, &islandBorder, pillX, pillY, pillW, pillH, 17.0f);

        Color dotColor = g_state.is_active ? Color(255, 16, 185, 129) : Color(140, 113, 113, 122);
        SolidBrush dotBrush(dotColor);
        g.FillEllipse(&dotBrush, pillX + 14.0f, pillY + 12.0f, 9.0f, 9.0f);

        if (g_state.is_active) {
            Pen dotGlow(Color(80, 16, 185, 129), 2.5f);
            g.DrawEllipse(&dotGlow, pillX + 12.5f, pillY + 10.5f, 12.0f, 12.0f);
        }

        draw_text(&g, L"promotezz", &fontTitle, &whiteBrush,
                  RectF(pillX + 30.0f, pillY, 78.0f, pillH), &sfLeft);
        draw_text(&g, L"· autoclicker", &fontBody, &zinc400,
                  RectF(pillX + 108.0f, pillY, 100.0f, pillH), &sfLeft);

        // 4. Hero Status Banner Card
        float heroX = 32.0f;
        float heroY = 62.0f;
        float heroW = (float)width - 64.0f;
        float heroH = 58.0f;

        if (g_state.is_active) {
            SolidBrush activeBg(Color(35, 16, 185, 129));
            Pen activeBorder(Color(120, 16, 185, 129), 1.0f);
            draw_rounded_card(&g, &activeBg, &activeBorder, heroX, heroY, heroW, heroH, 16.0f);

            draw_text(&g, L"ACTIVE · CLICKING ENGAGED", &fontTitle, &whiteBrush,
                      RectF(heroX, heroY + 8.0f, heroW, 22.0f), &sfCenter);

            wchar_t subText[64];
            swprintf_s(subText, 64, L"Pacing ~%.1f CPS · Press [%S] to pause", g_state.target_cps, HOTKEY_NAMES[g_hotkey_idx]);
            draw_text(&g, subText, &fontSmall, &emerald,
                      RectF(heroX, heroY + 30.0f, heroW, 20.0f), &sfCenter);
        } else {
            draw_rounded_card(&g, &glassCardBg, &glassBorder, heroX, heroY, heroW, heroH, 16.0f);

            draw_text(&g, L"IDLE · READY TO ENGAGE", &fontTitle, &whiteBrush,
                      RectF(heroX, heroY + 8.0f, heroW, 22.0f), &sfCenter);

            wchar_t subText[64];
            swprintf_s(subText, 64, L"Toggle Key: [%S] · Low-Latency Win32", HOTKEY_NAMES[g_hotkey_idx]);
            draw_text(&g, subText, &fontSmall, &zinc500,
                      RectF(heroX, heroY + 30.0f, heroW, 20.0f), &sfCenter);
        }

        // 5. Click Speed Slider Card
        float speedCardY = 132.0f;
        float speedCardH = 96.0f;
        draw_rounded_card(&g, &glassCardBg, &glassBorder, heroX, speedCardY, heroW, speedCardH, 16.0f);

        draw_text(&g, L"CLICK SPEED", &fontTitle, &zinc400,
                  RectF(heroX + 16.0f, speedCardY + 12.0f, 120.0f, 22.0f), &sfLeft);

        wchar_t cpsStr[32];
        swprintf_s(cpsStr, 32, L"%.1f CPS", g_state.target_cps);

        float badgeW = 76.0f;
        float badgeH = 24.0f;
        float badgeX = heroX + heroW - badgeW - 14.0f;
        float badgeY = speedCardY + 12.0f;
        SolidBrush badgeBg(Color(30, 16, 185, 129));
        Pen badgeBorder(Color(70, 16, 185, 129), 1.0f);
        draw_rounded_card(&g, &badgeBg, &badgeBorder, badgeX, badgeY, badgeW, badgeH, 12.0f);
        draw_text(&g, cpsStr, &fontMonoLarge, &emerald,
                  RectF(badgeX, badgeY, badgeW, badgeH), &sfCenter);

        float trackX = heroX + 16.0f;
        float trackY = speedCardY + 48.0f;
        float trackW = heroW - 32.0f;
        float trackH = 6.0f;

        g_slider_rect.left = (LONG)trackX;
        g_slider_rect.right = (LONG)(trackX + trackW);
        g_slider_rect.top = (LONG)(trackY - 6.0f);
        g_slider_rect.bottom = (LONG)(trackY + trackH + 6.0f);

        SolidBrush railBg(Color(255, 39, 39, 44));
        fill_rounded_card(&g, &railBg, trackX, trackY, trackW, trackH, 3.0f);

        double pct = (g_state.target_cps - 6.0) / (24.0 - 6.0);
        if (pct < 0.0) pct = 0.0;
        if (pct > 1.0) pct = 1.0;
        float fillW = (float)(trackW * pct);
        if (fillW > 4.0f) {
            fill_rounded_card(&g, &whiteBrush, trackX, trackY, fillW, trackH, 3.0f);
        }

        float knobR = 9.0f;
        float knobX = trackX + fillW;
        float knobY = trackY + (trackH / 2.0f);

        SolidBrush knobShadow(Color(40, 0, 0, 0));
        g.FillEllipse(&knobShadow, knobX - knobR - 1.0f, knobY - knobR + 1.0f, (knobR * 2.0f) + 2.0f, (knobR * 2.0f) + 2.0f);
        g.FillEllipse(&whiteBrush, knobX - knobR, knobY - knobR, knobR * 2.0f, knobR * 2.0f);
        Pen knobBorder(Color(255, 220, 220, 225), 1.0f);
        g.DrawEllipse(&knobBorder, knobX - knobR, knobY - knobR, knobR * 2.0f, knobR * 2.0f);

        draw_text(&g, L"6.0 CPS (Standard)", &fontSmall, &zinc500,
                  RectF(trackX, speedCardY + 68.0f, 120.0f, 18.0f), &sfLeft);
        draw_text(&g, L"24.0 CPS (High-Speed)", &fontSmall, &zinc500,
                  RectF(trackX + trackW - 130.0f, speedCardY + 68.0f, 130.0f, 18.0f), &sfRight);

        // 6. Primary Action Button
        float btnY = 244.0f;
        float btnH = 46.0f;
        g_btn_toggle_rect.left = (LONG)heroX;
        g_btn_toggle_rect.right = (LONG)(heroX + heroW);
        g_btn_toggle_rect.top = (LONG)btnY;
        g_btn_toggle_rect.bottom = (LONG)(btnY + btnH);

        if (g_state.is_active) {
            SolidBrush stopBrush(Color(255, 225, 29, 72));
            fill_rounded_card(&g, &stopBrush, heroX, btnY, heroW, btnH, 23.0f);
            draw_text(&g, L"STOP CLICKER (F6)", &fontHeading, &whiteBrush,
                      RectF(heroX, btnY, heroW, btnH), &sfCenter);
        } else {
            fill_rounded_card(&g, &whiteBrush, heroX, btnY, heroW, btnH, 23.0f);
            draw_text(&g, L"START CLICKER (F6)", &fontHeading, &darkBrush,
                      RectF(heroX, btnY, heroW, btnH), &sfCenter);
        }

        // 7. Secondary Controls: Hotkey & Audio Pills
        float row2Y = 302.0f;
        float btn2W = (heroW - 12.0f) / 2.0f;
        float btn2H = 38.0f;

        g_btn_hotkey_rect.left = (LONG)heroX;
        g_btn_hotkey_rect.right = (LONG)(heroX + btn2W);
        g_btn_hotkey_rect.top = (LONG)row2Y;
        g_btn_hotkey_rect.bottom = (LONG)(row2Y + btn2H);

        draw_rounded_card(&g, &glassCardBg, &glassBorder, heroX, row2Y, btn2W, btn2H, 19.0f);
        wchar_t hkStr[32];
        swprintf_s(hkStr, 32, L"Hotkey: [%S]", HOTKEY_NAMES[g_hotkey_idx]);
        draw_text(&g, hkStr, &fontBody, &zinc300,
                  RectF(heroX, row2Y, btn2W, btn2H), &sfCenter);

        float sndX = heroX + btn2W + 12.0f;
        g_btn_sound_rect.left = (LONG)sndX;
        g_btn_sound_rect.right = (LONG)(sndX + btn2W);
        g_btn_sound_rect.top = (LONG)row2Y;
        g_btn_sound_rect.bottom = (LONG)(row2Y + btn2H);

        draw_rounded_card(&g, &glassCardBg, &glassBorder, sndX, row2Y, btn2W, btn2H, 19.0f);
        draw_text(&g, g_state.sound_enabled ? L"Audio: ON" : L"Audio: MUTE", &fontBody,
                  g_state.sound_enabled ? &emerald : &zinc500,
                  RectF(sndX, row2Y, btn2W, btn2H), &sfCenter);

        // 8. Gaussian Jitter Anti-Pattern Card
        float jitY = 352.0f;
        float jitH = 40.0f;
        g_btn_jitter_rect.left = (LONG)heroX;
        g_btn_jitter_rect.right = (LONG)(heroX + heroW);
        g_btn_jitter_rect.top = (LONG)jitY;
        g_btn_jitter_rect.bottom = (LONG)(jitY + jitH);

        if (g_state.jitter_enabled) {
            SolidBrush jitBg(Color(25, 16, 185, 129));
            Pen jitBorder(Color(65, 16, 185, 129), 1.0f);
            draw_rounded_card(&g, &jitBg, &jitBorder, heroX, jitY, heroW, jitH, 20.0f);
            draw_text(&g, L"Gaussian Jitter: ENABLED (Humanized)", &fontBody, &emerald,
                      RectF(heroX, jitY, heroW, jitH), &sfCenter);
        } else {
            draw_rounded_card(&g, &glassCardBg, &glassBorder, heroX, jitY, heroW, jitH, 20.0f);
            draw_text(&g, L"Gaussian Jitter: DISABLED (Fixed)", &fontBody, &zinc500,
                      RectF(heroX, jitY, heroW, jitH), &sfCenter);
        }

        // 9. Telemetry Dashboard (3 Glass Capsules)
        float statsY = 404.0f;
        float statsH = 62.0f;
        draw_rounded_card(&g, &glassCardBg, &glassBorder, heroX, statsY, heroW, statsH, 16.0f);

        float colW = heroW / 3.0f;

        draw_text(&g, L"TOTAL CLICKS", &fontSmall, &zinc500,
                  RectF(heroX, statsY + 10.0f, colW, 16.0f), &sfCenter);
        draw_text(&g, L"LIVE SPEED", &fontSmall, &zinc500,
                  RectF(heroX + colW, statsY + 10.0f, colW, 16.0f), &sfCenter);
        draw_text(&g, L"RAM FOOTPRINT", &fontSmall, &zinc500,
                  RectF(heroX + (colW * 2.0f), statsY + 10.0f, colW, 16.0f), &sfCenter);

        wchar_t clicksBuf[32], liveCpsBuf[32], ramBuf[32];
        swprintf_s(clicksBuf, 32, L"%lld", g_state.total_clicks);
        swprintf_s(liveCpsBuf, 32, L"%.1f CPS", g_state.current_live_cps);
        swprintf_s(ramBuf, 32, L"%.1f MB", g_state.ram_mb);

        draw_text(&g, clicksBuf, &fontMono, &whiteBrush,
                  RectF(heroX, statsY + 30.0f, colW, 22.0f), &sfCenter);
        draw_text(&g, liveCpsBuf, &fontMono, &whiteBrush,
                  RectF(heroX + colW, statsY + 30.0f, colW, 22.0f), &sfCenter);
        draw_text(&g, ramBuf, &fontMono, &emerald,
                  RectF(heroX + (colW * 2.0f), statsY + 30.0f, colW, 22.0f), &sfCenter);

        // 10. Footer Link
        float ftrY = 484.0f;
        draw_text(&g, L"promotezz.cc · Free & Open Source · Win32", &fontSmall, &zinc600,
                  RectF(heroX, ftrY, heroW, 20.0f), &sfCenter);
    }

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
        return 1;

    case WM_LBUTTONDOWN: {
        POINT pt;
        pt.x = LOWORD(lParam);
        pt.y = HIWORD(lParam);

        if (PtInRect(&g_btn_close_rect, pt)) {
            g_state.should_exit = 1;
            PostQuitMessage(0);
            return 0;
        }
        if (PtInRect(&g_btn_min_rect, pt)) {
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }

        if (pt.y < 48) {
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }

        if (PtInRect(&g_slider_rect, pt)) {
            g_is_dragging_slider = 1;
            SetCapture(hwnd);
            update_slider_from_mouse(pt.x);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        if (PtInRect(&g_btn_toggle_rect, pt)) {
            g_state.is_active = !g_state.is_active;
            if (g_state.sound_enabled) {
                MessageBeep(g_state.is_active ? MB_OK : MB_ICONASTERISK);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        if (PtInRect(&g_btn_hotkey_rect, pt)) {
            g_hotkey_idx = (g_hotkey_idx + 1) % HOTKEY_COUNT;
            g_state.hotkey_vk = HOTKEY_VKS[g_hotkey_idx];
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        if (PtInRect(&g_btn_sound_rect, pt)) {
            g_state.sound_enabled = !g_state.sound_enabled;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

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

extern "C" {

void prmtz_gui_init(double initial_cps) {
    g_state.target_cps = initial_cps;
    HINSTANCE hInstance = GetModuleHandleA(NULL);

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "PrmtzAutoClickerClass";
    RegisterClassExA(&wc);

    int w = 380;
    int h = 520;
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int pos_x = (screen_w - w) / 2;
    int pos_y = (screen_h - h) / 2;

    HWND hwnd = CreateWindowExA(
        WS_EX_APPWINDOW,
        "PrmtzAutoClickerClass",
        "promotezz · autoclicker",
        WS_POPUP,
        pos_x, pos_y, w, h,
        NULL, NULL, hInstance, NULL
    );

    g_state.hwnd = hwnd;

    DWM_WINDOW_CORNER_PREFERENCE corner_pref = (DWM_WINDOW_CORNER_PREFERENCE)DWMWCP_ROUND_VAL;
    DwmSetWindowAttribute(hwnd, (DWORD)DWMWA_WINDOW_CORNER_PREFERENCE_VAL, &corner_pref, sizeof(corner_pref));

    BOOL use_dark_mode = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_VAL, &use_dark_mode, sizeof(use_dark_mode));

    MARGINS margins = { 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    g_state.click_thread = CreateThread(NULL, 0, clicker_worker_thread, NULL, 0, NULL);

    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    update_ram_stat();
}

int prmtz_gui_poll_messages(void) {
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

void prmtz_gui_cleanup(void) {
    g_state.should_exit = 1;
    if (g_state.click_thread) {
        WaitForSingleObject(g_state.click_thread, 1000);
        CloseHandle(g_state.click_thread);
        g_state.click_thread = NULL;
    }
    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

double prmtz_get_ram_usage_mb(void) {
    update_ram_stat();
    return g_state.ram_mb;
}

long long prmtz_get_click_count(void) {
    return g_state.total_clicks;
}

} // extern "C"
