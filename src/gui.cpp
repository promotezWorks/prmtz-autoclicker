#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <unknwn.h>
#include <timeapi.h>
#include <dwmapi.h>
#include <psapi.h>
#include <shellapi.h>
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
#pragma comment(lib, "shell32.lib")
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
    int speed_rand_pct;     /* 0, 15, 35, 50 % */
    int duty_cycle_pct;     /* 30, 45, 60, 75 % */
    int button_mode;        /* 0 = Left, 1 = Middle, 2 = Right */
    int hold_mode;          /* 0 = Toggle, 1 = Hold */
    int is_dark_mode;       /* 0 = Black-on-White (Light), 1 = White-on-000000-Black (Dark) */
    long long total_clicks;
    double current_live_cps;
    double ram_mb;
    HWND hwnd;
    HANDLE click_thread;
    int should_exit;
} BlurClickerState;

static BlurClickerState g_state = {
    25.0,       /* target_cps (matching BlurAutoClicker default 25 CPS) */
    0,          /* is_active */
    VK_F6,      /* hotkey_vk */
    1,          /* sound_enabled */
    35,         /* speed_rand_pct (matching Blur 35%) */
    45,         /* duty_cycle_pct (matching Blur 45%) */
    0,          /* button_mode (0=Left, 1=Middle, 2=Right) */
    0,          /* hold_mode (0=Toggle, 1=Hold) */
    0,          /* is_dark_mode (0 = Black-on-White default, toggleable to 1 = White-on-Black) */
    0,          /* total_clicks */
    0.0,        /* current_live_cps */
    0.2,        /* ram_mb */
    NULL,
    NULL,
    0
};

static ULONG_PTR g_gdiplusToken = 0;

static const int HOTKEY_VKS[] = { VK_F6, 'X', 'C', 'V', VK_XBUTTON1 };
static const char* HOTKEY_NAMES[] = { "F6", "X", "C", "V", "M4" };
static const int HOTKEY_COUNT = 5;
static int g_hotkey_idx = 0;

/* Window Metrics: 720 x 210 Landscape form factor identical to BlurAutoClicker */
static const int WIN_WIDTH  = 720;
static const int WIN_HEIGHT = 210;

/* Interactive Hit Rectangles */
static RECT g_rect_gear         = { 14, 8, 38, 32 };
static RECT g_rect_mouse_btn    = { 42, 8, 70, 32 };
static RECT g_rect_pin          = { 638, 8, 660, 32 };
static RECT g_rect_min          = { 666, 8, 688, 32 };
static RECT g_rect_close        = { 692, 8, 714, 32 };

/* Row 1 */
static RECT g_rect_cps_box      = { 14, 54, 74, 100 };
static RECT g_rect_rate_drop    = { 284, 54, 404, 100 };
static RECT g_rect_hotkey_box   = { 416, 54, 561, 100 };
static RECT g_rect_trigger_box  = { 561, 54, 706, 100 };

/* Row 2 */
static RECT g_rect_btn_left     = { 104, 122, 158, 152 };
static RECT g_rect_btn_mid      = { 160, 122, 214, 152 };
static RECT g_rect_btn_right    = { 216, 122, 270, 152 };
static RECT g_rect_duty_cycle   = { 296, 114, 476, 160 };
static RECT g_rect_speed_rand   = { 488, 114, 706, 160 };

/* Footer */
static RECT g_rect_audio        = { 14, 184, 250, 208 };
static RECT g_rect_theme_toggle = { 545, 186, 625, 206 };

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

        if (g_state.hold_mode) {
            if (hotkey_is_down != g_state.is_active) {
                g_state.is_active = hotkey_is_down;
                if (g_state.sound_enabled && g_state.is_active) {
                    MessageBeep(MB_OK);
                }
                if (g_state.hwnd) InvalidateRect(g_state.hwnd, NULL, FALSE);
            }
        } else {
            if (hotkey_is_down && !hotkey_was_down) {
                g_state.is_active = !g_state.is_active;
                if (g_state.sound_enabled) {
                    MessageBeep(g_state.is_active ? MB_OK : MB_ICONASTERISK);
                }
                if (g_state.hwnd) InvalidateRect(g_state.hwnd, NULL, FALSE);
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
            if (g_state.hwnd) InvalidateRect(g_state.hwnd, NULL, FALSE);
        }

        if (g_state.is_active) {
            double target = g_state.target_cps;
            if (target < 1.0) target = 1.0;
            if (target > 100.0) target = 100.0;

            double base_interval_sec = 1.0 / target;
            double jitter_sec = 0.0;
            if (g_state.speed_rand_pct > 0) {
                double pct = (double)g_state.speed_rand_pct / 100.0;
                jitter_sec = generate_gaussian_jitter(base_interval_sec * pct * 0.4);
            }
            double total_delay_sec = base_interval_sec + jitter_sec;
            if (total_delay_sec < 0.010) total_delay_sec = 0.010;

            double elapsed_since_click = (double)(t_now.QuadPart - t_last_click.QuadPart) / (double)freq.QuadPart;

            if (elapsed_since_click >= total_delay_sec) {
                INPUT inputs[2];
                ZeroMemory(inputs, sizeof(inputs));

                DWORD down_flag = MOUSEEVENTF_LEFTDOWN;
                DWORD up_flag   = MOUSEEVENTF_LEFTUP;

                if (g_state.button_mode == 1) {
                    down_flag = MOUSEEVENTF_MIDDLEDOWN;
                    up_flag   = MOUSEEVENTF_MIDDLEUP;
                } else if (g_state.button_mode == 2) {
                    down_flag = MOUSEEVENTF_RIGHTDOWN;
                    up_flag   = MOUSEEVENTF_RIGHTUP;
                }

                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dwFlags = down_flag;
                SendInput(1, &inputs[0], sizeof(INPUT));

                double duty_ratio = (double)g_state.duty_cycle_pct / 100.0;
                DWORD hold_ms = (DWORD)(total_delay_sec * 1000.0 * duty_ratio);
                if (hold_ms < 6) hold_ms = 6;
                if (hold_ms > 40) hold_ms = 40;
                Sleep(hold_ms);

                inputs[1].type = INPUT_MOUSE;
                inputs[1].mi.dwFlags = up_flag;
                SendInput(1, &inputs[1], sizeof(INPUT));

                QueryPerformanceCounter(&t_last_click);
                g_state.total_clicks++;
                clicks_in_last_sec++;
            }
        }

        Sleep(1);
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
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHighQuality);

        int isDark = g_state.is_dark_mode;

        // STRICT PURE 000000 BLACK AND PURE FFFFFF WHITE COLOR SYSTEM
        Color colorBg           = isDark ? Color(255, 0, 0, 0)          : Color(255, 255, 255, 255);
        Color colorCardBg       = isDark ? Color(255, 14, 14, 14)      : Color(255, 246, 246, 248);
        Color colorText         = isDark ? Color(255, 255, 255, 255)  : Color(255, 0, 0, 0);
        Color colorTextSub      = isDark ? Color(255, 175, 175, 175)  : Color(255, 80, 80, 80);
        Color colorBorder       = isDark ? Color(255, 255, 255, 255)  : Color(255, 0, 0, 0);
        Color colorBorderSubtle = isDark ? Color(55, 255, 255, 255)   : Color(45, 0, 0, 0);
        Color colorActivePill   = isDark ? Color(255, 255, 255, 255)  : Color(255, 0, 0, 0);
        Color colorActiveText   = isDark ? Color(255, 0, 0, 0)          : Color(255, 255, 255, 255);
        Color colorInactPill    = isDark ? Color(255, 28, 28, 28)      : Color(255, 228, 228, 232);
        Color colorEmerald      = Color(255, 34, 197, 94);

        SolidBrush brushBg(colorBg);
        SolidBrush brushCardBg(colorCardBg);
        SolidBrush brushText(colorText);
        SolidBrush brushTextSub(colorTextSub);
        SolidBrush brushActivePill(colorActivePill);
        SolidBrush brushActiveText(colorActiveText);
        SolidBrush brushInactPill(colorInactPill);
        SolidBrush brushEmerald(colorEmerald);

        Pen penBorder(colorBorder, 1.2f);
        Pen penBorderSubtle(colorBorderSubtle, 1.0f);
        Pen penAccent(colorEmerald, 2.0f);
        Pen penCtrl(colorText, 1.5f);

        // Fill window canvas with rounded corners
        g.FillRectangle(&brushBg, 0, 0, width, height);

        // Typography (Segoe UI) - ClearType GridFit ensures 100% crispness
        FontFamily ffSegoe(L"Segoe UI");
        FontFamily ffSymbol(L"Segoe UI Symbol");

        Gdiplus::Font fontTitle(&ffSegoe, 12.0f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontHero(&ffSegoe, 12.0f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontBody(&ffSegoe, 10.0f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontSub(&ffSegoe, 9.0f, FontStyleRegular, UnitPoint);
        Gdiplus::Font fontSmall(&ffSegoe, 8.5f, FontStyleRegular, UnitPoint);
        Gdiplus::Font fontMicro(&ffSegoe, 7.8f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontSymbol(&ffSymbol, 10.0f, FontStyleRegular, UnitPoint);

        StringFormat sfC;
        sfC.SetAlignment(StringAlignmentCenter);
        sfC.SetLineAlignment(StringAlignmentCenter);

        StringFormat sfL;
        sfL.SetAlignment(StringAlignmentNear);
        sfL.SetLineAlignment(StringAlignmentCenter);

        StringFormat sfR;
        sfR.SetAlignment(StringAlignmentFar);
        sfR.SetLineAlignment(StringAlignmentCenter);

        // --- 1. TITLE BAR ---
        float titleH = 38.0f;

        // Gear icon (Settings / Theme switch)
        g.DrawString(L"\u2699", -1, &fontSymbol, RectF((float)g_rect_gear.left, (float)g_rect_gear.top, 24.0f, 24.0f), &sfC, &brushText);

        // Mouse indicator button (turns glowing emerald green when active!)
        float mouseX = (float)g_rect_mouse_btn.left;
        float mouseY = (float)g_rect_mouse_btn.top;
        float mouseW = 28.0f;
        float mouseH = 24.0f;

        if (g_state.is_active) {
            // Glowing Active Emerald State
            SolidBrush mActiveBg(Color(255, 34, 197, 94));
            fill_rounded_card(&g, &mActiveBg, mouseX, mouseY, mouseW, mouseH, 6.0f);
            Pen mActivePen(Color(255, 255, 255, 255), 1.5f);
            g.DrawArc(&mActivePen, mouseX + 8.0f, mouseY + 4.0f, 12.0f, 16.0f, 0.0f, 360.0f);
            g.DrawLine(&mActivePen, mouseX + 14.0f, mouseY + 4.0f, mouseX + 14.0f, mouseY + 10.0f);
        } else {
            // Idle State with subtle emerald outline
            SolidBrush mIdleBg(isDark ? Color(35, 34, 197, 94) : Color(20, 34, 197, 94));
            Pen mIdlePen(colorEmerald, 1.5f);
            draw_rounded_card(&g, &mIdleBg, &mIdlePen, mouseX, mouseY, mouseW, mouseH, 6.0f);
            g.DrawArc(&mIdlePen, mouseX + 8.0f, mouseY + 4.0f, 12.0f, 16.0f, 0.0f, 360.0f);
            g.DrawLine(&mIdlePen, mouseX + 14.0f, mouseY + 4.0f, mouseX + 14.0f, mouseY + 10.0f);
        }

        // View Mode Icons (Layers, Grid, Target)
        float iconX = mouseX + mouseW + 12.0f;
        g.DrawString(L"\u25A4", -1, &fontSymbol, RectF(iconX, mouseY, 20.0f, 24.0f), &sfC, &brushTextSub);
        iconX += 26.0f;
        g.DrawString(L"\u229E", -1, &fontSymbol, RectF(iconX, mouseY, 20.0f, 24.0f), &sfC, &brushTextSub);
        iconX += 26.0f;
        g.DrawString(L"\u25CE", -1, &fontSymbol, RectF(iconX, mouseY, 20.0f, 24.0f), &sfC, &brushTextSub);

        // Center Title: promotezz AutoClicker
        g.DrawString(L"promotezz AutoClicker", -1, &fontTitle, RectF(0.0f, 0.0f, (float)width, titleH), &sfC, &brushText);

        // Right Window Controls: Pin, Minimize, Close (Vector drawn - Zero Mojibake!)
        float pinX = (float)g_rect_pin.left;
        g.DrawString(L"\u2360", -1, &fontSymbol, RectF(pinX, mouseY, 20.0f, 24.0f), &sfC, &brushTextSub);

        float minX = (float)g_rect_min.left;
        g.DrawLine(&penCtrl, minX + 4.0f, mouseY + 12.0f, minX + 16.0f, mouseY + 12.0f);

        float clsX = (float)g_rect_close.left;
        g.DrawLine(&penCtrl, clsX + 4.0f, mouseY + 6.0f, clsX + 16.0f, mouseY + 18.0f);
        g.DrawLine(&penCtrl, clsX + 16.0f, mouseY + 6.0f, clsX + 4.0f, mouseY + 18.0f);

        // Vibrant Emerald Accent Line (Signature BlurAutoClicker rule)
        g.DrawLine(&penAccent, 0.0f, titleH, (float)width, titleH);

        // --- 2. ROW 1 (TOP CARD ROW) ---
        float row1Y = titleH + 16.0f;
        float cardH = 46.0f;

        // Card 1: Click Speed Group [ 25 | Clicks Per | Second ▾ | Rate ▾ ]
        float c1X = 14.0f;
        float c1W = 390.0f;
        draw_rounded_card(&g, &brushCardBg, &penBorderSubtle, c1X, row1Y, c1W, cardH, 8.0f);

        // CPS Number Box
        float cpsBoxW = 60.0f;
        wchar_t cpsStr[16];
        swprintf_s(cpsStr, 16, L"%.0f", g_state.target_cps);
        g.DrawString(cpsStr, -1, &fontHero, RectF(c1X, row1Y, cpsBoxW, cardH), &sfC, &brushText);

        // Divider
        float divX = c1X + cpsBoxW;
        g.DrawLine(&penBorderSubtle, divX, row1Y + 8.0f, divX, row1Y + cardH - 8.0f);

        // "Clicks Per"
        float cpW = 95.0f;
        g.DrawString(L"Clicks Per", -1, &fontBody, RectF(divX, row1Y, cpW, cardH), &sfC, &brushText);

        divX += cpW;
        g.DrawLine(&penBorderSubtle, divX, row1Y + 8.0f, divX, row1Y + cardH - 8.0f);

        // "Second ▾"
        float secW = 115.0f;
        g.DrawString(L"Second \u25BE", -1, &fontBody, RectF(divX, row1Y, secW, cardH), &sfC, &brushText);

        divX += secW;
        g.DrawLine(&penBorderSubtle, divX, row1Y + 8.0f, divX, row1Y + cardH - 8.0f);

        // "Rate ▾"
        float rateW = c1W - (divX - c1X);
        g.DrawString(L"Rate \u25BE", -1, &fontBody, RectF(divX, row1Y, rateW, cardH), &sfC, &brushText);

        // Card 2: Hotkey & Trigger Group [ F6  ⌨ | Toggle ▾ ]
        float c2X = c1X + c1W + 12.0f;
        float c2W = (float)width - c2X - 14.0f;
        draw_rounded_card(&g, &brushCardBg, &penBorderSubtle, c2X, row1Y, c2W, cardH, 8.0f);

        // Hotkey Box: [ F6 ]
        float hkW = 145.0f;
        wchar_t hkStr[16];
        swprintf_s(hkStr, 16, L"%S", HOTKEY_NAMES[g_hotkey_idx]);
        g.DrawString(hkStr, -1, &fontHero, RectF(c2X + 16.0f, row1Y, 60.0f, cardH), &sfL, &brushText);
        g.DrawString(L"\u2328", -1, &fontSymbol, RectF(c2X + hkW - 34.0f, row1Y, 24.0f, cardH), &sfC, &brushTextSub);

        float div2X = c2X + hkW;
        g.DrawLine(&penBorderSubtle, div2X, row1Y + 8.0f, div2X, row1Y + cardH - 8.0f);

        // Trigger Dropdown: [ Toggle ▾ / Hold ▾ ]
        float trigW = c2W - hkW;
        const wchar_t* trigStr = g_state.hold_mode ? L"Hold \u25BE" : L"Toggle \u25BE";
        g.DrawString(trigStr, -1, &fontBody, RectF(div2X, row1Y, trigW, cardH), &sfC, &brushText);

        // --- 3. ROW 2 (BOTTOM CARD ROW) ---
        float row2Y = row1Y + cardH + 12.0f;

        // Card 3: Button Selector [ 🖱 | ⌨ | LEFT | MIDDLE | RIGHT ]
        float c3X = 14.0f;
        float c3W = 270.0f;
        draw_rounded_card(&g, &brushCardBg, &penBorderSubtle, c3X, row2Y, c3W, cardH, 8.0f);

        // Mouse Mode Pill (Active)
        float mPillX = c3X + 8.0f;
        float mPillY = row2Y + 8.0f;
        float mPillW = 32.0f;
        float mPillH = cardH - 16.0f;
        fill_rounded_card(&g, &brushInactPill, mPillX, mPillY, mPillW, mPillH, 6.0f);
        // Small mouse icon
        Pen mSmallPen(colorText, 1.2f);
        g.DrawArc(&mSmallPen, mPillX + 10.0f, mPillY + 7.0f, 12.0f, 16.0f, 0.0f, 360.0f);
        g.DrawLine(&mSmallPen, mPillX + 16.0f, mPillY + 7.0f, mPillX + 16.0f, mPillY + 12.0f);

        // Keyboard Mode Pill
        float kPillX = mPillX + mPillW + 4.0f;
        g.DrawString(L"\u2328", -1, &fontSymbol, RectF(kPillX, mPillY, mPillW, mPillH), &sfC, &brushTextSub);

        // Divider
        float div3X = kPillX + mPillW + 6.0f;
        g.DrawLine(&penBorderSubtle, div3X, row2Y + 8.0f, div3X, row2Y + cardH - 8.0f);

        // Segmented Buttons: LEFT | MIDDLE | RIGHT
        float segBtnW = 54.0f;
        float btnLeftX = div3X + 8.0f;
        float btnMidX  = btnLeftX + segBtnW + 2.0f;
        float btnRightX = btnMidX + segBtnW + 2.0f;

        if (g_state.button_mode == 0) {
            fill_rounded_card(&g, &brushActivePill, btnLeftX, mPillY, segBtnW, mPillH, 6.0f);
            g.DrawString(L"LEFT", -1, &fontMicro, RectF(btnLeftX, mPillY, segBtnW, mPillH), &sfC, &brushActiveText);
            g.DrawString(L"MIDDLE", -1, &fontMicro, RectF(btnMidX, mPillY, segBtnW, mPillH), &sfC, &brushTextSub);
            g.DrawString(L"RIGHT", -1, &fontMicro, RectF(btnRightX, mPillY, segBtnW, mPillH), &sfC, &brushTextSub);
        } else if (g_state.button_mode == 1) {
            fill_rounded_card(&g, &brushActivePill, btnMidX, mPillY, segBtnW, mPillH, 6.0f);
            g.DrawString(L"LEFT", -1, &fontMicro, RectF(btnLeftX, mPillY, segBtnW, mPillH), &sfC, &brushTextSub);
            g.DrawString(L"MIDDLE", -1, &fontMicro, RectF(btnMidX, mPillY, segBtnW, mPillH), &sfC, &brushActiveText);
            g.DrawString(L"RIGHT", -1, &fontMicro, RectF(btnRightX, mPillY, segBtnW, mPillH), &sfC, &brushTextSub);
        } else {
            fill_rounded_card(&g, &brushActivePill, btnRightX, mPillY, segBtnW, mPillH, 6.0f);
            g.DrawString(L"LEFT", -1, &fontMicro, RectF(btnLeftX, mPillY, segBtnW, mPillH), &sfC, &brushTextSub);
            g.DrawString(L"MIDDLE", -1, &fontMicro, RectF(btnMidX, mPillY, segBtnW, mPillH), &sfC, &brushTextSub);
            g.DrawString(L"RIGHT", -1, &fontMicro, RectF(btnRightX, mPillY, segBtnW, mPillH), &sfC, &brushActiveText);
        }

        // Card 4: Duty Cycle [ Duty Cycle | 45 % ]
        float c4X = c3X + c3W + 12.0f;
        float c4W = 180.0f;
        draw_rounded_card(&g, &brushCardBg, &penBorderSubtle, c4X, row2Y, c4W, cardH, 8.0f);

        float dcLabelW = 105.0f;
        g.DrawString(L"Duty Cycle", -1, &fontBody, RectF(c4X, row2Y, dcLabelW, cardH), &sfC, &brushText);

        float div4X = c4X + dcLabelW;
        g.DrawLine(&penBorderSubtle, div4X, row2Y + 8.0f, div4X, row2Y + cardH - 8.0f);

        float dcValW = c4W - dcLabelW;
        wchar_t dcBuf[16];
        swprintf_s(dcBuf, 16, L"%d %%", g_state.duty_cycle_pct);
        g.DrawString(dcBuf, -1, &fontHero, RectF(div4X, row2Y, dcValW, cardH), &sfC, &brushText);

        // Card 5: Speed Randomization [ Speed Randomization | 35 % ]
        float c5X = c4X + c4W + 12.0f;
        float c5W = (float)width - c5X - 14.0f;
        draw_rounded_card(&g, &brushCardBg, &penBorderSubtle, c5X, row2Y, c5W, cardH, 8.0f);

        float randLabelW = 150.0f;
        g.DrawString(L"Speed Randomization", -1, &fontBody, RectF(c5X, row2Y, randLabelW, cardH), &sfC, &brushText);

        float div5X = c5X + randLabelW;
        g.DrawLine(&penBorderSubtle, div5X, row2Y + 8.0f, div5X, row2Y + cardH - 8.0f);

        float randValW = c5W - randLabelW;
        wchar_t randBuf[16];
        swprintf_s(randBuf, 16, L"%d %%", g_state.speed_rand_pct);
        g.DrawString(randBuf, -1, &fontHero, RectF(div5X, row2Y, randValW, cardH), &sfC, &brushText);

        // --- 4. FOOTER STATUS BAR ---
        float footY = (float)height - 26.0f;
        g.DrawLine(&penBorderSubtle, 0.0f, footY, (float)width, footY);

        // Left Status & Audio (Vector Speaker)
        float footTextX = 14.0f;
        Point speakerPts[6] = {
            Point((INT)footTextX, (INT)footY + 10),
            Point((INT)footTextX + 4, (INT)footY + 10),
            Point((INT)footTextX + 8, (INT)footY + 6),
            Point((INT)footTextX + 8, (INT)footY + 18),
            Point((INT)footTextX + 4, (INT)footY + 14),
            Point((INT)footTextX, (INT)footY + 14)
        };
        g.FillPolygon(&brushEmerald, speakerPts, 6);
        g.DrawArc(&penAccent, footTextX + 9.0f, footY + 7.0f, 6.0f, 10.0f, -60.0f, 120.0f);

        wchar_t footBuf[128];
        if (g_state.is_active) {
            swprintf_s(footBuf, 128, L"ACTIVE · %lld clicks · %.1f Live CPS (prmtz engine < 3.2 MB)",
                g_state.total_clicks, g_state.current_live_cps);
        } else {
            swprintf_s(footBuf, 128, L"Audio: %s · prmtz engine (< 3.2 MB RAM) · Target: %.0f CPS",
                g_state.sound_enabled ? L"ON" : L"MUTE", g_state.target_cps);
        }
        g.DrawString(footBuf, -1, &fontSmall, RectF(footTextX + 22.0f, footY, 380.0f, 24.0f), &sfL, &brushTextSub);

        // Right Controls: THEME TOGGLE PILL [ ☀️ Light / 🌙 Dark ]
        float themePillW = 80.0f;
        float themePillH = 18.0f;
        float themePillX = (float)width - 180.0f;
        float themePillY = footY + 3.0f;

        g_rect_theme_toggle.left   = (LONG)themePillX;
        g_rect_theme_toggle.top    = (LONG)themePillY;
        g_rect_theme_toggle.right  = (LONG)(themePillX + themePillW);
        g_rect_theme_toggle.bottom = (LONG)(themePillY + themePillH);

        fill_rounded_card(&g, &brushInactPill, themePillX, themePillY, themePillW, themePillH, 9.0f);
        const wchar_t* themeStr = isDark ? L"\u2600 Light" : L"\u263D Dark";
        g.DrawString(themeStr, -1, &fontMicro, RectF(themePillX, themePillY, themePillW, themePillH), &sfC, &brushText);

        // Zoom scale
        float zoomX = themePillX + themePillW + 12.0f;
        g.DrawString(L"200%", -1, &fontSmall, RectF(zoomX, footY, 40.0f, 24.0f), &sfC, &brushTextSub);

        // Version
        float verX = (float)width - 45.0f;
        g.DrawString(L"v1.0.0", -1, &fontSmall, RectF(verX, footY, 36.0f, 24.0f), &sfR, &brushTextSub);

        // Window border (outer)
        g.DrawRectangle(&penBorder, 0, 0, width - 1, height - 1);
    }

    BitBlt(hdcWindow, 0, 0, width, height, hdc, 0, 0, SRCCOPY);

    SelectObject(hdc, oldBm);
    DeleteObject(hbm);
    DeleteDC(hdc);
    EndPaint(hwnd, &ps);
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

        // 1. Close Button
        if (PtInRect(&g_rect_close, pt)) {
            g_state.should_exit = 1;
            PostQuitMessage(0);
            return 0;
        }

        // 2. Minimize Button
        if (PtInRect(&g_rect_min, pt)) {
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }

        // 3. Theme Toggle (Pill in footer or Gear in header)
        if (PtInRect(&g_rect_theme_toggle, pt) || PtInRect(&g_rect_gear, pt)) {
            g_state.is_dark_mode = !g_state.is_dark_mode;
            BOOL use_dark = g_state.is_dark_mode ? TRUE : FALSE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE_VAL, &use_dark, sizeof(use_dark));
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 4. Mouse Status Button in Header -> Start / Stop clicker!
        if (PtInRect(&g_rect_mouse_btn, pt)) {
            g_state.is_active = !g_state.is_active;
            if (g_state.sound_enabled) {
                MessageBeep(g_state.is_active ? MB_OK : MB_ICONASTERISK);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 5. CPS Number Box -> Left click increases (+1, or cycles presets 10, 15, 20, 25, 30, 40, 50)
        if (PtInRect(&g_rect_cps_box, pt)) {
            static const double PRESETS[] = { 10.0, 15.0, 20.0, 25.0, 30.0, 40.0, 50.0 };
            static int p_idx = 3; // 25.0 default
            p_idx = (p_idx + 1) % 7;
            g_state.target_cps = PRESETS[p_idx];
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 6. Hotkey Box -> Click cycles hotkeys (F6, X, C, V, M4)
        if (PtInRect(&g_rect_hotkey_box, pt)) {
            g_hotkey_idx = (g_hotkey_idx + 1) % HOTKEY_COUNT;
            g_state.hotkey_vk = HOTKEY_VKS[g_hotkey_idx];
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 7. Trigger Mode Box -> Toggle / Hold
        if (PtInRect(&g_rect_trigger_box, pt)) {
            g_state.hold_mode = !g_state.hold_mode;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 8. Button Mode: LEFT
        if (PtInRect(&g_rect_btn_left, pt)) {
            g_state.button_mode = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 9. Button Mode: MIDDLE
        if (PtInRect(&g_rect_btn_mid, pt)) {
            g_state.button_mode = 1;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 10. Button Mode: RIGHT
        if (PtInRect(&g_rect_btn_right, pt)) {
            g_state.button_mode = 2;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 11. Duty Cycle Box -> Cycles 30%, 45%, 60%, 75%
        if (PtInRect(&g_rect_duty_cycle, pt)) {
            if (g_state.duty_cycle_pct == 30) g_state.duty_cycle_pct = 45;
            else if (g_state.duty_cycle_pct == 45) g_state.duty_cycle_pct = 60;
            else if (g_state.duty_cycle_pct == 60) g_state.duty_cycle_pct = 75;
            else g_state.duty_cycle_pct = 30;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 12. Speed Randomization Box -> Cycles 0%, 15%, 35%, 50%
        if (PtInRect(&g_rect_speed_rand, pt)) {
            if (g_state.speed_rand_pct == 0) g_state.speed_rand_pct = 15;
            else if (g_state.speed_rand_pct == 15) g_state.speed_rand_pct = 35;
            else if (g_state.speed_rand_pct == 35) g_state.speed_rand_pct = 50;
            else g_state.speed_rand_pct = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 13. Audio status in Footer -> Toggle Audio ON/OFF
        if (PtInRect(&g_rect_audio, pt)) {
            g_state.sound_enabled = !g_state.sound_enabled;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // 14. Title bar dragging
        if (pt.y < 38) {
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }

        break;
    }

    case WM_RBUTTONDOWN: {
        POINT pt;
        pt.x = LOWORD(lParam);
        pt.y = HIWORD(lParam);

        // Right click CPS Box decreases CPS (-5)
        if (PtInRect(&g_rect_cps_box, pt)) {
            g_state.target_cps -= 5.0;
            if (g_state.target_cps < 5.0) g_state.target_cps = 50.0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;
    }

    case WM_DESTROY:
        g_state.should_exit = 1;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

extern "C" {

void prmtz_gui_init(double initial_cps) {
    if (initial_cps > 0.0) {
        g_state.target_cps = initial_cps;
    }
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

    int w = WIN_WIDTH;
    int h = WIN_HEIGHT;
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

    BOOL use_dark_mode = g_state.is_dark_mode ? TRUE : FALSE;
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
