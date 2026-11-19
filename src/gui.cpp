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
    int jitter_enabled;
    int right_click_mode;
    int hold_mode;
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
    0,          /* right_click_mode (0=Left, 1=Right) */
    0,          /* hold_mode (0=Toggle, 1=Hold) */
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

/* Window Metrics: 540 x 445 */
static const int WIN_WIDTH = 540;
static const int WIN_HEIGHT = 445;

static int g_is_dragging_slider = 0;

/* Interactive Hit Rectangles */
static RECT g_slider_rect       = { 34, 156, 502, 168 };
static RECT g_btn_toggle_rect   = { 18, 383, 522, 425 };
static RECT g_btn_seg_btn_rect  = { 164, 218, 256, 242 };
static RECT g_btn_seg_trig_rect = { 164, 278, 256, 302 };
static RECT g_sw_jitter_rect    = { 474, 216, 508, 234 };
static RECT g_sw_audio_rect     = { 474, 256, 508, 274 };
static RECT g_btn_bind_rect     = { 462, 296, 508, 318 };
static RECT g_btn_close_rect    = { 512, 14, 534, 38 };
static RECT g_btn_min_rect      = { 486, 14, 508, 38 };
static RECT g_pill_dash_rect    = { 368, 14, 472, 42 };
static RECT g_island_rect       = { 190, 12, 350, 44 };

/* Preset Buttons: 10, 12, 14, 18, 22 */
static RECT g_preset_rects[5];
static const double PRESET_VALUES[5] = { 10.0, 12.0, 14.0, 18.0, 22.0 };

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

                DWORD down_flag = g_state.right_click_mode ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
                DWORD up_flag = g_state.right_click_mode ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;

                inputs[0].type = INPUT_MOUSE;
                inputs[0].mi.dwFlags = down_flag;
                SendInput(1, &inputs[0], sizeof(INPUT));

                Sleep(12);

                inputs[1].type = INPUT_MOUSE;
                inputs[1].mi.dwFlags = up_flag;
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

static void draw_switch(Graphics* g, float x, float y, bool on) {
    float w = 34.0f;
    float h = 18.0f;
    Color bgColor = on ? Color(255, 255, 255, 255) : Color(255, 39, 39, 44);
    SolidBrush bg(bgColor);
    fill_rounded_card(g, &bg, x, y, w, h, 9.0f);

    float knobR = 6.5f;
    float knobX = on ? (x + w - knobR - 2.5f) : (x + knobR + 2.5f);
    float knobY = y + (h / 2.0f);
    Color knobColor = on ? Color(255, 12, 12, 15) : Color(255, 161, 161, 170);
    SolidBrush knobBrush(knobColor);
    g->FillEllipse(&knobBrush, knobX - knobR, knobY - knobR, knobR * 2.0f, knobR * 2.0f);
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

        // 1. Deep Zinc Canvas #0c0c0f
        SolidBrush winBg(Color(255, 12, 12, 15));
        g.FillRectangle(&winBg, 0, 0, width, height);

        // Window border: 1px subtle white/15
        Pen winBorder(Color(38, 255, 255, 255), 1.0f);
        g.DrawRectangle(&winBorder, 0, 0, width - 1, height - 1);

        // Typography (Segoe UI) - razor sharp with ClearTypeGridFit
        FontFamily ffSegoe(L"Segoe UI");
        FontFamily ffConsolas(L"Consolas");
        Gdiplus::Font fontLogo(&ffSegoe, 11.5f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontLogoSub(&ffSegoe, 8.5f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontHeroNum(&ffSegoe, 32.0f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontHeroBadge(&ffSegoe, 9.5f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontCardTitle(&ffSegoe, 9.5f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontSub(&ffSegoe, 8.0f, FontStyleRegular, UnitPoint);
        Gdiplus::Font fontMicro(&ffSegoe, 7.5f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontBtn(&ffSegoe, 10.5f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontMono(&ffConsolas, 8.5f, FontStyleBold, UnitPoint);

        // Palette
        SolidBrush whiteBrush(Color(255, 255, 255, 255));
        SolidBrush darkBrush(Color(255, 12, 12, 15));
        SolidBrush zinc100(Color(255, 244, 244, 245));
        SolidBrush zinc200(Color(255, 228, 228, 231));
        SolidBrush zinc300(Color(255, 212, 212, 216));
        SolidBrush zinc400(Color(255, 161, 161, 170));
        SolidBrush zinc500(Color(255, 113, 113, 122));
        SolidBrush emeraldBrush(Color(255, 16, 185, 129));
        SolidBrush taupeBrush(Color(255, 89, 75, 69));

        // Translucent card backgrounds
        SolidBrush cardBg(Color(255, 18, 18, 22));
        Pen cardBorder(Color(28, 255, 255, 255), 1.0f);

        StringFormat sfC;
        sfC.SetAlignment(StringAlignmentCenter);
        sfC.SetLineAlignment(StringAlignmentCenter);

        StringFormat sfL;
        sfL.SetAlignment(StringAlignmentNear);
        sfL.SetLineAlignment(StringAlignmentCenter);

        StringFormat sfR;
        sfR.SetAlignment(StringAlignmentFar);
        sfR.SetLineAlignment(StringAlignmentCenter);

        // --- 1. TOP HEADER ZONE ---
        // Top Left: promotezz logo + FREE badge
        float logoX = 20.0f;
        float logoY = 14.0f;
        g.DrawString(L"promotezz", -1, &fontLogo, RectF(logoX, logoY, 90.0f, 24.0f), &sfL, &whiteBrush);

        float badgeX = logoX + 92.0f;
        float badgeY = logoY + 3.0f;
        float badgeW = 44.0f;
        float badgeH = 18.0f;
        SolidBrush badgeBg(Color(25, 255, 255, 255));
        Pen badgePen(Color(40, 255, 255, 255), 1.0f);
        draw_rounded_card(&g, &badgeBg, &badgePen, badgeX, badgeY, badgeW, badgeH, 9.0f);
        g.DrawString(L"FREE", -1, &fontLogoSub, RectF(badgeX, badgeY, badgeW, badgeH), &sfC, &zinc200);

        // Top Center: FLOATING DYNAMIC ISLAND CAPSULE (Matching promotezz.app)
        float islandW = 160.0f;
        float islandH = 32.0f;
        float islandX = ((float)width - islandW) / 2.0f;
        float islandY = 12.0f;

        g_island_rect.left = (LONG)islandX;
        g_island_rect.top = (LONG)islandY;
        g_island_rect.right = (LONG)(islandX + islandW);
        g_island_rect.bottom = (LONG)(islandY + islandH);

        // Subtle island drop shadow
        SolidBrush islandShadow(Color(50, 0, 0, 0));
        fill_rounded_card(&g, &islandShadow, islandX, islandY + 2.0f, islandW, islandH, 16.0f);

        SolidBrush islandBg(Color(255, 0, 0, 0));
        Pen islandPen(Color(35, 255, 255, 255), 1.0f);
        draw_rounded_card(&g, &islandBg, &islandPen, islandX, islandY, islandW, islandH, 16.0f);

        if (g_state.is_active) {
            // Expanded Dynamic Island active state
            g.FillEllipse(&emeraldBrush, islandX + 14.0f, islandY + 12.0f, 8.0f, 8.0f);
            wchar_t actIslandBuf[64];
            swprintf_s(actIslandBuf, 64, L"ACTIVE · %.1f CPS (F6)", g_state.target_cps);
            g.DrawString(actIslandBuf, -1, &fontSub, RectF(islandX + 28.0f, islandY, islandW - 32.0f, islandH), &sfL, &whiteBrush);
        } else {
            // Dynamic Island Inner Tabs: [Clicker | Tweaks]
            float tabW = (islandW / 2.0f) - 4.0f;
            float tabH = islandH - 6.0f;
            float tabY = islandY + 3.0f;
            float tabActiveX = islandX + 3.0f;

            SolidBrush tabActiveBg(Color(255, 39, 39, 42));
            fill_rounded_card(&g, &tabActiveBg, tabActiveX, tabY, tabW, tabH, 13.0f);
            g.DrawString(L"Clicker", -1, &fontSub, RectF(tabActiveX, tabY, tabW, tabH), &sfC, &whiteBrush);

            float tabInactX = tabActiveX + tabW + 2.0f;
            g.DrawString(L"Tweaks", -1, &fontSub, RectF(tabInactX, tabY, tabW, tabH), &sfC, &zinc400);
        }

        // Top Right: PROFILE PILL (Directly matching "(T) Dashboard •")
        float pillW = 104.0f;
        float pillH = 28.0f;
        float pillX = (float)width - 68.0f - pillW;
        float pillY = 14.0f;

        g_pill_dash_rect.left = (LONG)pillX;
        g_pill_dash_rect.top = (LONG)pillY;
        g_pill_dash_rect.right = (LONG)(pillX + pillW);
        g_pill_dash_rect.bottom = (LONG)(pillY + pillH);

        SolidBrush pillBg(Color(255, 24, 24, 27));
        Pen pillPen(Color(35, 255, 255, 255), 1.0f);
        draw_rounded_card(&g, &pillBg, &pillPen, pillX, pillY, pillW, pillH, 14.0f);

        // Avatar Circle (T)
        float avaD = 20.0f;
        float avaX = pillX + 4.0f;
        float avaY = pillY + 4.0f;
        g.FillEllipse(&taupeBrush, avaX, avaY, avaD, avaD);
        g.DrawString(L"T", -1, &fontMicro, RectF(avaX, avaY, avaD, avaD), &sfC, &whiteBrush);

        // Dashboard Text
        g.DrawString(L"Dashboard", -1, &fontSub, RectF(avaX + avaD + 4.0f, pillY, 58.0f, pillH), &sfL, &zinc200);

        // Emerald status dot
        float dotD = 5.0f;
        float dotX = pillX + pillW - 12.0f;
        float dotY = pillY + ((pillH - dotD) / 2.0f);
        g.FillEllipse(&emeraldBrush, dotX, dotY, dotD, dotD);

        // Vector Window Controls (No text mojibake)
        float minX = (float)width - 54.0f;
        float minY = 16.0f;
        Pen ctrlPen(Color(255, 161, 161, 170), 1.5f);
        g.DrawLine(&ctrlPen, minX + 5.0f, minY + 11.0f, minX + 17.0f, minY + 11.0f);

        g_btn_min_rect.left = (LONG)minX;
        g_btn_min_rect.top = (LONG)minY;
        g_btn_min_rect.right = (LONG)(minX + 22.0f);
        g_btn_min_rect.bottom = (LONG)(minY + 22.0f);

        float clsX = (float)width - 28.0f;
        float clsY = 16.0f;
        g.DrawLine(&ctrlPen, clsX + 5.0f, clsY + 5.0f, clsX + 17.0f, clsY + 17.0f);
        g.DrawLine(&ctrlPen, clsX + 17.0f, clsY + 5.0f, clsX + 5.0f, clsY + 17.0f);

        g_btn_close_rect.left = (LONG)clsX;
        g_btn_close_rect.top = (LONG)clsY;
        g_btn_close_rect.right = (LONG)(clsX + 22.0f);
        g_btn_close_rect.bottom = (LONG)(clsY + 22.0f);

        // --- 2. HERO SPEED CARD ---
        float card1X = 18.0f;
        float card1Y = 58.0f;
        float card1W = (float)width - 36.0f;
        float card1H = 138.0f;

        draw_rounded_card(&g, &cardBg, &cardBorder, card1X, card1Y, card1W, card1H, 14.0f);

        // Header: Label + Preset Pills
        g.DrawString(L"TARGET CLICK SPEED", -1, &fontMicro, RectF(card1X + 16.0f, card1Y + 12.0f, 200.0f, 18.0f), &sfL, &zinc400);

        // Preset Pills: [10] [12] [14] [18] [22]
        float preX = card1X + card1W - 16.0f - (5.0f * 34.0f);
        for (int i = 0; i < 5; i++) {
            float pW = 30.0f;
            float pH = 20.0f;
            float pY = card1Y + 10.0f;

            g_preset_rects[i].left = (LONG)preX;
            g_preset_rects[i].top = (LONG)pY;
            g_preset_rects[i].right = (LONG)(preX + pW);
            g_preset_rects[i].bottom = (LONG)(pY + pH);

            wchar_t pValStr[8];
            swprintf_s(pValStr, 8, L"%.0f", PRESET_VALUES[i]);

            if (fabs(g_state.target_cps - PRESET_VALUES[i]) < 0.1) {
                fill_rounded_card(&g, &whiteBrush, preX, pY, pW, pH, 10.0f);
                g.DrawString(pValStr, -1, &fontMicro, RectF(preX, pY, pW, pH), &sfC, &darkBrush);
            } else {
                SolidBrush pBg(Color(255, 28, 28, 33));
                Pen pPen(Color(25, 255, 255, 255), 1.0f);
                draw_rounded_card(&g, &pBg, &pPen, preX, pY, pW, pH, 10.0f);
                g.DrawString(pValStr, -1, &fontMicro, RectF(preX, pY, pW, pH), &sfC, &zinc400);
            }
            preX += 34.0f;
        }

        // Hero Speed Readout
        float heroNumY = card1Y + 36.0f;
        wchar_t cpsStr[32];
        swprintf_s(cpsStr, 32, L"%.1f", g_state.target_cps);
        g.DrawString(cpsStr, -1, &fontHeroNum, RectF(card1X + 14.0f, heroNumY, 110.0f, 46.0f), &sfL, &whiteBrush);

        float cpsBadgeX = card1X + 126.0f;
        float cpsBadgeY = heroNumY + 12.0f;
        float cpsBadgeW = 46.0f;
        float cpsBadgeH = 22.0f;
        SolidBrush cpsBadgeBg(Color(255, 39, 39, 44));
        fill_rounded_card(&g, &cpsBadgeBg, cpsBadgeX, cpsBadgeY, cpsBadgeW, cpsBadgeH, 11.0f);
        g.DrawString(L"CPS", -1, &fontHeroBadge, RectF(cpsBadgeX, cpsBadgeY, cpsBadgeW, cpsBadgeH), &sfC, &zinc100);

        g.DrawString(L"Sub-millisecond hardware mouse dispatch", -1, &fontSub, RectF(cpsBadgeX + cpsBadgeW + 12.0f, cpsBadgeY, 260.0f, cpsBadgeH), &sfL, &zinc500);

        // Slider Track
        float trackX = card1X + 16.0f;
        float trackY = card1Y + 98.0f;
        float trackW = card1W - 32.0f;
        float trackH = 6.0f;

        g_slider_rect.left = (LONG)trackX;
        g_slider_rect.right = (LONG)(trackX + trackW);
        g_slider_rect.top = (LONG)(trackY - 8.0f);
        g_slider_rect.bottom = (LONG)(trackY + trackH + 8.0f);

        SolidBrush trackBg(Color(255, 39, 39, 44));
        fill_rounded_card(&g, &trackBg, trackX, trackY, trackW, trackH, 3.0f);

        double pct = (g_state.target_cps - 6.0) / (24.0 - 6.0);
        if (pct < 0.0) pct = 0.0;
        if (pct > 1.0) pct = 1.0;
        float fillW = (float)(trackW * pct);
        if (fillW > 4.0f) {
            fill_rounded_card(&g, &whiteBrush, trackX, trackY, fillW, trackH, 3.0f);
        }

        // Slider Knob
        float knobR = 9.0f;
        float knobX = trackX + fillW;
        float knobY = trackY + (trackH / 2.0f);
        SolidBrush knobShadow(Color(80, 0, 0, 0));
        g.FillEllipse(&knobShadow, knobX - knobR - 1.0f, knobY - knobR + 2.0f, (knobR * 2.0f) + 2.0f, (knobR * 2.0f) + 2.0f);
        g.FillEllipse(&whiteBrush, knobX - knobR, knobY - knobR, knobR * 2.0f, knobR * 2.0f);

        g.DrawString(L"6.0 CPS (Standard)", -1, &fontSub, RectF(trackX, trackY + 12.0f, 150.0f, 18.0f), &sfL, &zinc500);
        g.DrawString(L"24.0 CPS (Fast)", -1, &fontSub, RectF(trackX + trackW - 150.0f, trackY + 12.0f, 150.0f, 18.0f), &sfR, &zinc500);

        // --- 3. DUAL CONFIGURATION CARDS ---
        float cardGap = 12.0f;
        float card2W = (card1W - cardGap) / 2.0f;
        float card2H = 126.0f;
        float card2Y = card1Y + card1H + 12.0f;

        // LEFT CARD: INPUT TARGETING
        float cardLeftX = card1X;
        draw_rounded_card(&g, &cardBg, &cardBorder, cardLeftX, card2Y, card2W, card2H, 14.0f);

        // Row 1: Click Button
        float r1Y = card2Y + 12.0f;
        g.DrawString(L"Click Button", -1, &fontCardTitle, RectF(cardLeftX + 14.0f, r1Y, 120.0f, 18.0f), &sfL, &whiteBrush);
        g.DrawString(L"Target mouse key", -1, &fontSub, RectF(cardLeftX + 14.0f, r1Y + 18.0f, 120.0f, 16.0f), &sfL, &zinc500);

        float segW = 46.0f;
        float segH = 24.0f;
        float segY = r1Y + 6.0f;
        float segX = cardLeftX + card2W - 14.0f - (segW * 2.0f);

        g_btn_seg_btn_rect.left = (LONG)segX;
        g_btn_seg_btn_rect.top = (LONG)segY;
        g_btn_seg_btn_rect.right = (LONG)(segX + (segW * 2.0f));
        g_btn_seg_btn_rect.bottom = (LONG)(segY + segH);

        SolidBrush segBg(Color(255, 28, 28, 33));
        fill_rounded_card(&g, &segBg, segX, segY, segW * 2.0f, segH, 12.0f);

        if (!g_state.right_click_mode) {
            fill_rounded_card(&g, &whiteBrush, segX + 1.0f, segY + 1.0f, segW - 2.0f, segH - 2.0f, 11.0f);
            g.DrawString(L"Left", -1, &fontSub, RectF(segX, segY, segW, segH), &sfC, &darkBrush);
            g.DrawString(L"Right", -1, &fontSub, RectF(segX + segW, segY, segW, segH), &sfC, &zinc400);
        } else {
            fill_rounded_card(&g, &whiteBrush, segX + segW + 1.0f, segY + 1.0f, segW - 2.0f, segH - 2.0f, 11.0f);
            g.DrawString(L"Left", -1, &fontSub, RectF(segX, segY, segW, segH), &sfC, &zinc400);
            g.DrawString(L"Right", -1, &fontSub, RectF(segX + segW, segY, segW, segH), &sfC, &darkBrush);
        }

        Pen divPen(Color(20, 255, 255, 255), 1.0f);
        g.DrawLine(&divPen, cardLeftX + 14.0f, card2Y + 62.0f, cardLeftX + card2W - 14.0f, card2Y + 62.0f);

        // Row 2: Trigger Mode
        float r2Y = card2Y + 72.0f;
        g.DrawString(L"Trigger Mode", -1, &fontCardTitle, RectF(cardLeftX + 14.0f, r2Y, 120.0f, 18.0f), &sfL, &whiteBrush);
        g.DrawString(L"Activation type", -1, &fontSub, RectF(cardLeftX + 14.0f, r2Y + 18.0f, 120.0f, 16.0f), &sfL, &zinc500);

        float seg2Y = r2Y + 6.0f;
        g_btn_seg_trig_rect.left = (LONG)segX;
        g_btn_seg_trig_rect.top = (LONG)seg2Y;
        g_btn_seg_trig_rect.right = (LONG)(segX + (segW * 2.0f));
        g_btn_seg_trig_rect.bottom = (LONG)(seg2Y + segH);

        fill_rounded_card(&g, &segBg, segX, seg2Y, segW * 2.0f, segH, 12.0f);

        if (!g_state.hold_mode) {
            fill_rounded_card(&g, &whiteBrush, segX + 1.0f, seg2Y + 1.0f, segW - 2.0f, segH - 2.0f, 11.0f);
            g.DrawString(L"Toggle", -1, &fontSub, RectF(segX, seg2Y, segW, segH), &sfC, &darkBrush);
            g.DrawString(L"Hold", -1, &fontSub, RectF(segX + segW, seg2Y, segW, segH), &sfC, &zinc400);
        } else {
            fill_rounded_card(&g, &whiteBrush, segX + segW + 1.0f, seg2Y + 1.0f, segW - 2.0f, segH - 2.0f, 11.0f);
            g.DrawString(L"Toggle", -1, &fontSub, RectF(segX, seg2Y, segW, segH), &sfC, &zinc400);
            g.DrawString(L"Hold", -1, &fontSub, RectF(segX + segW, seg2Y, segW, segH), &sfC, &darkBrush);
        }

        // RIGHT CARD: ENGINE CONFIGURATION
        float cardRightX = cardLeftX + card2W + cardGap;
        draw_rounded_card(&g, &cardBg, &cardBorder, cardRightX, card2Y, card2W, card2H, 14.0f);

        // Row 1: Gaussian Jitter
        float rj1Y = card2Y + 10.0f;
        g.DrawString(L"Gaussian Jitter", -1, &fontCardTitle, RectF(cardRightX + 14.0f, rj1Y, 140.0f, 18.0f), &sfL, &whiteBrush);
        g.DrawString(L"Humanize intervals", -1, &fontSub, RectF(cardRightX + 14.0f, rj1Y + 16.0f, 140.0f, 16.0f), &sfL, &zinc500);

        float swX = cardRightX + card2W - 14.0f - 34.0f;
        draw_switch(&g, swX, rj1Y + 6.0f, g_state.jitter_enabled);
        g_sw_jitter_rect.left = (LONG)swX;
        g_sw_jitter_rect.top = (LONG)(rj1Y + 4.0f);
        g_sw_jitter_rect.right = (LONG)(swX + 34.0f);
        g_sw_jitter_rect.bottom = (LONG)(rj1Y + 26.0f);

        g.DrawLine(&divPen, cardRightX + 14.0f, card2Y + 46.0f, cardRightX + card2W - 14.0f, card2Y + 46.0f);

        // Row 2: Click Audio
        float rj2Y = card2Y + 50.0f;
        g.DrawString(L"Click Audio", -1, &fontCardTitle, RectF(cardRightX + 14.0f, rj2Y, 140.0f, 18.0f), &sfL, &whiteBrush);
        g.DrawString(L"Mechanical toggle cue", -1, &fontSub, RectF(cardRightX + 14.0f, rj2Y + 16.0f, 140.0f, 16.0f), &sfL, &zinc500);

        draw_switch(&g, swX, rj2Y + 6.0f, g_state.sound_enabled);
        g_sw_audio_rect.left = (LONG)swX;
        g_sw_audio_rect.top = (LONG)(rj2Y + 4.0f);
        g_sw_audio_rect.right = (LONG)(swX + 34.0f);
        g_sw_audio_rect.bottom = (LONG)(rj2Y + 26.0f);

        g.DrawLine(&divPen, cardRightX + 14.0f, card2Y + 86.0f, cardRightX + card2W - 14.0f, card2Y + 86.0f);

        // Row 3: Hotkey Bind
        float rj3Y = card2Y + 90.0f;
        g.DrawString(L"Hotkey Bind", -1, &fontCardTitle, RectF(cardRightX + 14.0f, rj3Y, 140.0f, 18.0f), &sfL, &whiteBrush);
        g.DrawString(L"Global activation key", -1, &fontSub, RectF(cardRightX + 14.0f, rj3Y + 16.0f, 140.0f, 16.0f), &sfL, &zinc500);

        float hkW = 46.0f;
        float hkH = 22.0f;
        float hkX = cardRightX + card2W - 14.0f - hkW;
        float hkY = rj3Y + 4.0f;

        g_btn_bind_rect.left = (LONG)hkX;
        g_btn_bind_rect.top = (LONG)hkY;
        g_btn_bind_rect.right = (LONG)(hkX + hkW);
        g_btn_bind_rect.bottom = (LONG)(hkY + hkH);

        SolidBrush hkBg(Color(255, 39, 39, 44));
        fill_rounded_card(&g, &hkBg, hkX, hkY, hkW, hkH, 11.0f);
        wchar_t hkStr[16];
        swprintf_s(hkStr, 16, L"%S", HOTKEY_NAMES[g_hotkey_idx]);
        g.DrawString(hkStr, -1, &fontMono, RectF(hkX, hkY, hkW, hkH), &sfC, &whiteBrush);

        // --- 4. BOTTOM MASTER ACTION & TELEMETRY ---
        float btnW = card1W;
        float btnH = 42.0f;
        float btnX = card1X;
        float btnY = (float)height - 62.0f;

        g_btn_toggle_rect.left = (LONG)btnX;
        g_btn_toggle_rect.top = (LONG)btnY;
        g_btn_toggle_rect.right = (LONG)(btnX + btnW);
        g_btn_toggle_rect.bottom = (LONG)(btnY + btnH);

        if (g_state.is_active) {
            SolidBrush activeBtn(Color(255, 24, 24, 27));
            Pen activeRing(Color(255, 255, 255, 255), 2.0f);
            draw_rounded_card(&g, &activeBtn, &activeRing, btnX, btnY, btnW, btnH, 21.0f);
            g.FillEllipse(&emeraldBrush, btnX + (btnW / 2.0f) - 95.0f, btnY + 17.0f, 8.0f, 8.0f);
            g.DrawString(L"STOP CLICKER (F6)", -1, &fontBtn, RectF(btnX, btnY, btnW, btnH), &sfC, &whiteBrush);
        } else {
            fill_rounded_card(&g, &whiteBrush, btnX, btnY, btnW, btnH, 21.0f);
            g.DrawString(L"START CLICKER (F6)", -1, &fontBtn, RectF(btnX, btnY, btnW, btnH), &sfC, &darkBrush);
        }

        // Micro Telemetry Row
        float telY = (float)height - 18.0f;
        wchar_t telBuf[128];
        swprintf_s(telBuf, 128, L"%.1f MB RAM   ·   %lld CLICKS   ·   %.1f LIVE CPS   ·   PROMOTEZZ ENGINE",
            g_state.ram_mb, g_state.total_clicks, g_state.current_live_cps);
        g.DrawString(telBuf, -1, &fontMicro, RectF(0.0f, telY, (float)width, 14.0f), &sfC, &zinc500);
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

        // Window Controls
        if (PtInRect(&g_btn_close_rect, pt)) {
            g_state.should_exit = 1;
            PostQuitMessage(0);
            return 0;
        }
        if (PtInRect(&g_btn_min_rect, pt)) {
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        }

        // Dashboard Pill click -> open website
        if (PtInRect(&g_pill_dash_rect, pt)) {
            ShellExecuteA(NULL, "open", "https://promotezz.cc/dashboard", NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }

        // Preset Pills
        for (int i = 0; i < 5; i++) {
            if (PtInRect(&g_preset_rects[i], pt)) {
                g_state.target_cps = PRESET_VALUES[i];
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }

        // Slider Track
        if (PtInRect(&g_slider_rect, pt)) {
            g_is_dragging_slider = 1;
            SetCapture(hwnd);
            update_slider_from_mouse(pt.x);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // Segmented Click Button: Left / Right
        if (PtInRect(&g_btn_seg_btn_rect, pt)) {
            int mid = (g_btn_seg_btn_rect.left + g_btn_seg_btn_rect.right) / 2;
            g_state.right_click_mode = (pt.x >= mid) ? 1 : 0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // Segmented Trigger Type: Toggle / Hold
        if (PtInRect(&g_btn_seg_trig_rect, pt)) {
            int mid = (g_btn_seg_trig_rect.left + g_btn_seg_trig_rect.right) / 2;
            g_state.hold_mode = (pt.x >= mid) ? 1 : 0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // Jitter Switch
        if (PtInRect(&g_sw_jitter_rect, pt)) {
            g_state.jitter_enabled = !g_state.jitter_enabled;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // Audio Switch
        if (PtInRect(&g_sw_audio_rect, pt)) {
            g_state.sound_enabled = !g_state.sound_enabled;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // Hotkey Bind Button
        if (PtInRect(&g_btn_bind_rect, pt)) {
            g_hotkey_idx = (g_hotkey_idx + 1) % HOTKEY_COUNT;
            g_state.hotkey_vk = HOTKEY_VKS[g_hotkey_idx];
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // Start / Stop Master Pill Button
        if (PtInRect(&g_btn_toggle_rect, pt)) {
            g_state.is_active = !g_state.is_active;
            if (g_state.sound_enabled) {
                MessageBeep(g_state.is_active ? MB_OK : MB_ICONASTERISK);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // Draggable Title / Header Area
        if (pt.y < 56) {
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
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
