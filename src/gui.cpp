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
    int right_click_mode;
    int hold_mode;
    int inv_pause_enabled;
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
    0,          /* inv_pause_enabled */
    0,          /* total_clicks */
    0.0,        /* current_live_cps */
    1.2,        /* ram_mb */
    NULL,
    NULL,
    0
};

static ULONG_PTR g_gdiplusToken = 0;

static const int HOTKEY_VKS[] = { VK_F6, 'X', 'C', 'V', VK_XBUTTON1 };
static const char* HOTKEY_NAMES[] = { "F6", "X", "C", "V", "M4" };
static const int HOTKEY_COUNT = 5;
static int g_hotkey_idx = 0;

/* Window Metrics: 560 x 370 Widescreen Client */
static int g_is_dragging_slider = 0;
static RECT g_slider_rect = { 32, 116, 256, 132 };
static RECT g_btn_toggle_rect = { 288, 318, 542, 352 };
static RECT g_btn_seg_btn_rect = { 152, 186, 256, 214 };
static RECT g_btn_seg_trig_rect = { 152, 242, 256, 270 };
static RECT g_sw_jitter_rect = { 498, 74, 532, 94 };
static RECT g_sw_audio_rect = { 498, 134, 532, 154 };
static RECT g_sw_inv_rect = { 498, 194, 532, 214 };
static RECT g_btn_bind_rect = { 482, 252, 536, 276 };
static RECT g_btn_close_rect = { 524, 12, 548, 34 };
static RECT g_btn_min_rect = { 494, 12, 518, 34 };

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
            // Hold mode
            if (hotkey_is_down != g_state.is_active) {
                g_state.is_active = hotkey_is_down;
                if (g_state.sound_enabled && g_state.is_active) {
                    MessageBeep(MB_OK);
                }
                if (g_state.hwnd) InvalidateRect(g_state.hwnd, NULL, FALSE);
            }
        } else {
            // Toggle mode
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

                Sleep(16);

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
    Color bgColor = on ? Color(255, 16, 185, 129) : Color(255, 45, 50, 60);
    SolidBrush bg(bgColor);
    fill_rounded_card(g, &bg, x, y, w, h, 9.0f);

    float knobR = 6.5f;
    float knobX = on ? (x + w - knobR - 2.5f) : (x + knobR + 2.5f);
    float knobY = y + (h / 2.0f);
    SolidBrush knobBrush(Color(255, 255, 255, 255));
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
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

        // 1. Base Slate #0c0e12
        SolidBrush bgBrush(Color(255, 12, 14, 18));
        g.FillRectangle(&bgBrush, 0, 0, width, height);

        // Ambient radial emerald hero glow behind center
        GraphicsPath glowPath;
        glowPath.AddEllipse(20.0f, 20.0f, (float)(width - 40), (float)(height - 40));
        PathGradientBrush pgb(&glowPath);
        pgb.SetCenterColor(Color(25, 16, 185, 129));
        Color surroundColors[] = { Color(0, 12, 14, 18) };
        int count = 1;
        pgb.SetSurroundColors(surroundColors, &count);
        g.FillPath(&pgb, &glowPath);

        // Top Header Bar
        float headerH = 46.0f;
        SolidBrush headerBg(Color(255, 16, 19, 24));
        Pen sepPen(Color(35, 255, 255, 255), 1.0f);
        g.FillRectangle(&headerBg, 0, 0, width, (int)headerH);
        g.DrawLine(&sepPen, 0.0f, headerH, (float)width, headerH);

        // Outer window border
        Pen winBorder(Color(45, 255, 255, 255), 1.0f);
        g.DrawRectangle(&winBorder, 0, 0, width - 1, height - 1);

        // Fonts
        FontFamily ffSegoe(L"Segoe UI");
        FontFamily ffConsolas(L"Consolas");
        Gdiplus::Font fontLogo(&ffSegoe, 11.5f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontLogoSub(&ffSegoe, 10.5f, FontStyleRegular, UnitPoint);
        Gdiplus::Font fontBadge(&ffSegoe, 8.5f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontCardTitle(&ffSegoe, 10.5f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontLabel(&ffSegoe, 9.5f, FontStyleRegular, UnitPoint);
        Gdiplus::Font fontSub(&ffSegoe, 8.5f, FontStyleRegular, UnitPoint);
        Gdiplus::Font fontMono(&ffConsolas, 10.5f, FontStyleBold, UnitPoint);
        Gdiplus::Font fontBtn(&ffSegoe, 10.5f, FontStyleBold, UnitPoint);

        // Brushes
        SolidBrush whiteBrush(Color(255, 255, 255, 255));
        SolidBrush darkBrush(Color(255, 12, 14, 18));
        SolidBrush zinc300(Color(255, 212, 212, 216));
        SolidBrush zinc400(Color(255, 161, 161, 170));
        SolidBrush zinc500(Color(255, 113, 113, 122));
        SolidBrush emerald(Color(255, 16, 185, 129));
        SolidBrush cardBg(Color(255, 18, 22, 28));
        Pen cardBorder(Color(32, 255, 255, 255), 1.0f);

        StringFormat sfC;
        sfC.SetAlignment(StringAlignmentCenter);
        sfC.SetLineAlignment(StringAlignmentCenter);

        StringFormat sfL;
        sfL.SetAlignment(StringAlignmentNear);
        sfL.SetLineAlignment(StringAlignmentCenter);

        StringFormat sfR;
        sfR.SetAlignment(StringAlignmentFar);
        sfR.SetLineAlignment(StringAlignmentCenter);

        // Header: Pulsing dot + promotezz clicker + FREE badge
        float logoX = 18.0f;
        float logoY = 12.0f;

        Color dotColor = g_state.is_active ? Color(255, 16, 185, 129) : Color(140, 113, 113, 122);
        SolidBrush dotBrush(dotColor);
        g.FillEllipse(&dotBrush, logoX, logoY + 7.0f, 8.0f, 8.0f);

        if (g_state.is_active) {
            Pen dotGlow(Color(80, 16, 185, 129), 2.0f);
            g.DrawEllipse(&dotGlow, logoX - 1.5f, logoY + 5.5f, 11.0f, 11.0f);
        }

        g.DrawString(L"promotezz", -1, &fontLogo, RectF(logoX + 16.0f, logoY, 82.0f, 22.0f), &sfL, &whiteBrush);
        g.DrawString(L"clicker", -1, &fontLogoSub, RectF(logoX + 96.0f, logoY, 56.0f, 22.0f), &sfL, &zinc400);

        SolidBrush verBg(Color(30, 16, 185, 129));
        Pen verBorder(Color(60, 16, 185, 129), 1.0f);
        draw_rounded_card(&g, &verBg, &verBorder, logoX + 154.0f, logoY + 2.0f, 58.0f, 18.0f, 9.0f);
        g.DrawString(L"FREE", -1, &fontBadge, RectF(logoX + 154.0f, logoY + 2.0f, 58.0f, 18.0f), &sfC, &emerald);

        // Window Controls (Right of Header)
        SolidBrush ctrlBg(Color(25, 255, 255, 255));
        fill_rounded_card(&g, &ctrlBg, (float)g_btn_min_rect.left, (float)g_btn_min_rect.top, 24.0f, 22.0f, 11.0f);
        fill_rounded_card(&g, &ctrlBg, (float)g_btn_close_rect.left, (float)g_btn_close_rect.top, 24.0f, 22.0f, 11.0f);
        g.DrawString(L"—", -1, &fontSub, RectF((float)g_btn_min_rect.left, (float)g_btn_min_rect.top, 24.0f, 22.0f), &sfC, &zinc400);
        g.DrawString(L"✕", -1, &fontSub, RectF((float)g_btn_close_rect.left, (float)g_btn_close_rect.top, 24.0f, 22.0f), &sfC, &zinc400);

        // ==========================================
        // DUAL COLUMN CLIENT INTERFACE
        // ==========================================
        float colY = headerH + 16.0f;
        float colGap = 16.0f;
        float colW = ((float)width - 36.0f - colGap) / 2.0f; // ~254px each
        float col1X = 18.0f;
        float col2X = col1X + colW + colGap;

        // COLUMN 1: CARD 1 (SPEED SLIDER)
        float card1H = 114.0f;
        draw_rounded_card(&g, &cardBg, &cardBorder, col1X, colY, colW, card1H, 12.0f);
        g.DrawString(L"Click Speed", -1, &fontCardTitle, RectF(col1X + 14.0f, colY + 12.0f, 110.0f, 20.0f), &sfL, &whiteBrush);

        wchar_t cpsStr[32];
        swprintf_s(cpsStr, 32, L"%.1f CPS", g_state.target_cps);
        SolidBrush badgeBg(Color(30, 16, 185, 129));
        Pen badgeBorder(Color(70, 16, 185, 129), 1.0f);
        draw_rounded_card(&g, &badgeBg, &badgeBorder, col1X + colW - 86.0f, colY + 10.0f, 72.0f, 24.0f, 12.0f);
        g.DrawString(cpsStr, -1, &fontMono, RectF(col1X + colW - 86.0f, colY + 10.0f, 72.0f, 24.0f), &sfC, &emerald);

        float trackX = col1X + 14.0f;
        float trackY = colY + 54.0f;
        float trackW = colW - 28.0f;
        float trackH = 5.0f;

        g_slider_rect.left = (LONG)trackX;
        g_slider_rect.right = (LONG)(trackX + trackW);
        g_slider_rect.top = (LONG)(trackY - 6.0f);
        g_slider_rect.bottom = (LONG)(trackY + trackH + 6.0f);

        SolidBrush railBg(Color(255, 35, 40, 50));
        fill_rounded_card(&g, &railBg, trackX, trackY, trackW, trackH, 2.5f);

        double pct = (g_state.target_cps - 6.0) / (24.0 - 6.0);
        if (pct < 0.0) pct = 0.0;
        if (pct > 1.0) pct = 1.0;
        float fillW = (float)(trackW * pct);
        if (fillW > 4.0f) {
            fill_rounded_card(&g, &whiteBrush, trackX, trackY, fillW, trackH, 2.5f);
        }

        float knobR = 8.0f;
        float knobX = trackX + fillW;
        float knobY = trackY + (trackH / 2.0f);
        SolidBrush knobShadow(Color(60, 0, 0, 0));
        g.FillEllipse(&knobShadow, knobX - knobR - 1.0f, knobY - knobR + 1.0f, (knobR * 2.0f) + 2.0f, (knobR * 2.0f) + 2.0f);
        g.FillEllipse(&whiteBrush, knobX - knobR, knobY - knobR, knobR * 2.0f, knobR * 2.0f);
        Pen knobBorder(Color(255, 220, 220, 225), 1.5f);
        g.DrawEllipse(&knobBorder, knobX - knobR, knobY - knobR, knobR * 2.0f, knobR * 2.0f);

        g.DrawString(L"6.0 CPS (Standard)", -1, &fontSub, RectF(trackX, colY + 76.0f, 120.0f, 16.0f), &sfL, &zinc500);
        g.DrawString(L"24.0 CPS (Max)", -1, &fontSub, RectF(trackX + trackW - 90.0f, colY + 76.0f, 90.0f, 16.0f), &sfR, &zinc500);

        // COLUMN 1: CARD 2 (BUTTON & TRIGGER TYPE)
        float card2Y = colY + card1H + 12.0f;
        float card2H = 114.0f;
        draw_rounded_card(&g, &cardBg, &cardBorder, col1X, card2Y, colW, card2H, 12.0f);

        g.DrawString(L"Click Button", -1, &fontLabel, RectF(col1X + 14.0f, card2Y + 12.0f, 100.0f, 20.0f), &sfL, &zinc300);
        float segW = 56.0f;
        float segH = 26.0f;
        float segX = col1X + colW - (segW * 2.0f) - 14.0f;
        float segY = card2Y + 10.0f;

        g_btn_seg_btn_rect.left = (LONG)segX;
        g_btn_seg_btn_rect.right = (LONG)(segX + (segW * 2.0f) + 4.0f);
        g_btn_seg_btn_rect.top = (LONG)segY;
        g_btn_seg_btn_rect.bottom = (LONG)(segY + segH);

        SolidBrush segActive(Color(45, 255, 255, 255));
        if (!g_state.right_click_mode) {
            fill_rounded_card(&g, &segActive, segX, segY, segW, segH, 13.0f);
        } else {
            fill_rounded_card(&g, &segActive, segX + segW + 4.0f, segY, segW, segH, 13.0f);
        }
        g.DrawString(L"Left", -1, &fontSub, RectF(segX, segY, segW, segH), &sfC, !g_state.right_click_mode ? &whiteBrush : &zinc500);
        g.DrawString(L"Right", -1, &fontSub, RectF(segX + segW + 4.0f, segY, segW, segH), &sfC, g_state.right_click_mode ? &whiteBrush : &zinc500);

        Pen divPen(Color(25, 255, 255, 255), 1.0f);
        g.DrawLine(&divPen, col1X + 14.0f, card2Y + 56.0f, col1X + colW - 14.0f, card2Y + 56.0f);

        g.DrawString(L"Trigger Type", -1, &fontLabel, RectF(col1X + 14.0f, card2Y + 68.0f, 100.0f, 20.0f), &sfL, &zinc300);
        float seg2Y = card2Y + 66.0f;
        g_btn_seg_trig_rect.left = (LONG)segX;
        g_btn_seg_trig_rect.right = (LONG)(segX + (segW * 2.0f) + 4.0f);
        g_btn_seg_trig_rect.top = (LONG)seg2Y;
        g_btn_seg_trig_rect.bottom = (LONG)(seg2Y + segH);

        if (!g_state.hold_mode) {
            fill_rounded_card(&g, &segActive, segX, seg2Y, segW, segH, 13.0f);
        } else {
            fill_rounded_card(&g, &segActive, segX + segW + 4.0f, seg2Y, segW, segH, 13.0f);
        }
        g.DrawString(L"Toggle", -1, &fontSub, RectF(segX, seg2Y, segW, segH), &sfC, !g_state.hold_mode ? &whiteBrush : &zinc500);
        g.DrawString(L"Hold", -1, &fontSub, RectF(segX + segW + 4.0f, seg2Y, segW, segH), &sfC, g_state.hold_mode ? &whiteBrush : &zinc500);

        // COLUMN 2: CARD 3 (TOGGLES & BIND)
        float card3H = 240.0f;
        draw_rounded_card(&g, &cardBg, &cardBorder, col2X, colY, colW, card3H, 12.0f);

        // Toggle 1: Jitter
        float t1Y = colY + 12.0f;
        g.DrawString(L"Gaussian Jitter", -1, &fontCardTitle, RectF(col2X + 14.0f, t1Y, 140.0f, 20.0f), &sfL, &whiteBrush);
        g.DrawString(L"Humanizes micro-click intervals", -1, &fontSub, RectF(col2X + 14.0f, t1Y + 20.0f, 160.0f, 16.0f), &sfL, &zinc500);
        draw_switch(&g, col2X + colW - 46.0f, t1Y + 6.0f, g_state.jitter_enabled);
        g_sw_jitter_rect.left = (LONG)(col2X + colW - 46.0f);
        g_sw_jitter_rect.right = (LONG)(col2X + colW - 12.0f);
        g_sw_jitter_rect.top = (LONG)(t1Y + 4.0f);
        g_sw_jitter_rect.bottom = (LONG)(t1Y + 26.0f);

        g.DrawLine(&divPen, col2X + 14.0f, t1Y + 48.0f, col2X + colW - 14.0f, t1Y + 48.0f);

        // Toggle 2: Audio
        float t2Y = t1Y + 60.0f;
        g.DrawString(L"Click Audio", -1, &fontCardTitle, RectF(col2X + 14.0f, t2Y, 140.0f, 20.0f), &sfL, &whiteBrush);
        g.DrawString(L"Subtle mechanical toggle cue", -1, &fontSub, RectF(col2X + 14.0f, t2Y + 20.0f, 160.0f, 16.0f), &sfL, &zinc500);
        draw_switch(&g, col2X + colW - 46.0f, t2Y + 6.0f, g_state.sound_enabled);
        g_sw_audio_rect.left = (LONG)(col2X + colW - 46.0f);
        g_sw_audio_rect.right = (LONG)(col2X + colW - 12.0f);
        g_sw_audio_rect.top = (LONG)(t2Y + 4.0f);
        g_sw_audio_rect.bottom = (LONG)(t2Y + 26.0f);

        g.DrawLine(&divPen, col2X + 14.0f, t2Y + 48.0f, col2X + colW - 14.0f, t2Y + 48.0f);

        // Toggle 3: Inventory Pause
        float t3Y = t2Y + 60.0f;
        g.DrawString(L"Inventory Pause", -1, &fontCardTitle, RectF(col2X + 14.0f, t3Y, 140.0f, 20.0f), &sfL, &whiteBrush);
        g.DrawString(L"Auto-pause in open menus", -1, &fontSub, RectF(col2X + 14.0f, t3Y + 20.0f, 160.0f, 16.0f), &sfL, &zinc500);
        draw_switch(&g, col2X + colW - 46.0f, t3Y + 6.0f, g_state.inv_pause_enabled);
        g_sw_inv_rect.left = (LONG)(col2X + colW - 46.0f);
        g_sw_inv_rect.right = (LONG)(col2X + colW - 12.0f);
        g_sw_inv_rect.top = (LONG)(t3Y + 4.0f);
        g_sw_inv_rect.bottom = (LONG)(t3Y + 26.0f);

        g.DrawLine(&divPen, col2X + 14.0f, t3Y + 48.0f, col2X + colW - 14.0f, t3Y + 48.0f);

        // Hotkey Bind Row
        float t4Y = t3Y + 60.0f;
        g.DrawString(L"Hotkey Bind", -1, &fontCardTitle, RectF(col2X + 14.0f, t4Y + 2.0f, 100.0f, 20.0f), &sfL, &whiteBrush);

        SolidBrush bindBg(Color(30, 255, 255, 255));
        Pen bindBorder(Color(60, 255, 255, 255), 1.0f);
        draw_rounded_card(&g, &bindBg, &bindBorder, col2X + colW - 68.0f, t4Y, 54.0f, 24.0f, 12.0f);
        g_btn_bind_rect.left = (LONG)(col2X + colW - 68.0f);
        g_btn_bind_rect.right = (LONG)(col2X + colW - 14.0f);
        g_btn_bind_rect.top = (LONG)t4Y;
        g_btn_bind_rect.bottom = (LONG)(t4Y + 24.0f);

        wchar_t hkStr[32];
        swprintf_s(hkStr, 32, L"%S", HOTKEY_NAMES[g_hotkey_idx]);
        g.DrawString(hkStr, -1, &fontMono, RectF(col2X + colW - 68.0f, t4Y, 54.0f, 24.0f), &sfC, &whiteBrush);

        // ==========================================
        // BOTTOM DOCK: TELEMETRY + START ACTION
        // ==========================================
        float dockY = (float)height - 44.0f;
        float dockH = 34.0f;

        // Telemetry Pill (Left)
        SolidBrush statBg(Color(255, 16, 20, 26));
        Pen statBorder(Color(25, 255, 255, 255), 1.0f);
        draw_rounded_card(&g, &statBg, &statBorder, col1X, dockY, colW, dockH, 10.0f);

        wchar_t clicksBuf[32], liveBuf[32], ramBuf[32];
        swprintf_s(clicksBuf, 32, L"%lld CLK", g_state.total_clicks);
        swprintf_s(liveBuf, 32, L"%.1f CPS", g_state.current_live_cps);
        swprintf_s(ramBuf, 32, L"%.1f MB", g_state.ram_mb);

        float thirdW = colW / 3.0f;
        g.DrawString(clicksBuf, -1, &fontMono, RectF(col1X, dockY, thirdW, dockH), &sfC, &zinc400);
        g.DrawString(liveBuf, -1, &fontMono, RectF(col1X + thirdW, dockY, thirdW, dockH), &sfC, &zinc400);
        g.DrawString(ramBuf, -1, &fontMono, RectF(col1X + (thirdW * 2.0f), dockY, thirdW, dockH), &sfC, &emerald);

        // Start Button Pill (Right)
        g_btn_toggle_rect.left = (LONG)col2X;
        g_btn_toggle_rect.right = (LONG)(col2X + colW);
        g_btn_toggle_rect.top = (LONG)dockY;
        g_btn_toggle_rect.bottom = (LONG)(dockY + dockH);

        if (g_state.is_active) {
            SolidBrush activeBtn(Color(255, 225, 29, 72));
            fill_rounded_card(&g, &activeBtn, col2X, dockY, colW, dockH, 17.0f);
            g.DrawString(L"STOP CLICKER (F6)", -1, &fontBtn, RectF(col2X, dockY, colW, dockH), &sfC, &whiteBrush);
        } else {
            fill_rounded_card(&g, &whiteBrush, col2X, dockY, colW, dockH, 17.0f);
            g.DrawString(L"START CLICKER (F6)", -1, &fontBtn, RectF(col2X, dockY, colW, dockH), &sfC, &darkBrush);
        }
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

        // Draggable Header Bar
        if (pt.y < 46) {
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return 0;
        }

        // Slider
        if (PtInRect(&g_slider_rect, pt)) {
            g_is_dragging_slider = 1;
            SetCapture(hwnd);
            update_slider_from_mouse(pt.x);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // Left / Right Click Segmented Button
        if (PtInRect(&g_btn_seg_btn_rect, pt)) {
            g_state.right_click_mode = !g_state.right_click_mode;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }

        // Toggle / Hold Segmented Button
        if (PtInRect(&g_btn_seg_trig_rect, pt)) {
            g_state.hold_mode = !g_state.hold_mode;
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

        // Inventory Switch
        if (PtInRect(&g_sw_inv_rect, pt)) {
            g_state.inv_pause_enabled = !g_state.inv_pause_enabled;
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

        // Start / Stop Master Button
        if (PtInRect(&g_btn_toggle_rect, pt)) {
            g_state.is_active = !g_state.is_active;
            if (g_state.sound_enabled) {
                MessageBeep(g_state.is_active ? MB_OK : MB_ICONASTERISK);
            }
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

    int w = 560;
    int h = 370;
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
