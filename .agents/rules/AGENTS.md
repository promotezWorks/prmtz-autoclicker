# prmtz AutoClicker — Agent Guidelines & Architecture

## Overview
`prmtz-autoclicker` is an ultra-lightweight, high-performance Win32 autoclicker engineered for competitive games and precision tasks.

## Key Technical Specifications
1. **Core Language:** The Goose Programming Language (`.goose`), developed by Wouter van Oortmerssen (`aardappel`), utilizing zero-heap stack-managed semantics.
2. **Memory Footprint:** Strict constraint to consume **< 10 MB RAM** in active operation (typical working set is **2.5 – 3.8 MB**). No Chromium, Electron, WebView2, or heavy DirectX/Vulkan runtimes.
3. **UI Engine:** High-performance double-buffered GDI with sub-pixel text anti-aliasing, ClearType rendering, and DWM Dark Mode framing.
4. **Click Engine:** Sub-millisecond multimedia timer resolution (`timeBeginPeriod(1)`) + Win32 `SendInput` event dispatch, utilizing Gaussian-distributed interval jitter to simulate human biomechanical clicking variances.
5. **Aesthetics:** Aligned with `promotezz.cc` and `store.promotezz.cc`:
   - Pure matte-black `#09090b` background
   - Surface cards `#121215` with 1px `#27272a` borders
   - Top-anchored Dynamic Island pill with pulsating live status LED
   - Smooth CPS slider (6–24 CPS)
   - Toggle button with state indicators
   - Hotkey selector (F6, X, C, Mouse4)
   - Real-time click telemetry and live RAM usage readout
