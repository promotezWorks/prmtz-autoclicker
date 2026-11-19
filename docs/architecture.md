# prmtz AutoClicker — Architecture & Performance Specification

## 1. Zero-Heap Stack-Allocated Core (Goose Lang)
`prmtz-autoclicker` is built on **The Goose Programming Language** by Wouter van Oortmerssen (`aardappel`), utilizing bump-pointer data stack management with zero heap allocator overhead (`malloc`/`free`) and zero runtime garbage collector pauses.

### Memory Profile
| Component | Implementation | Memory Footprint |
| :--- | :--- | :--- |
| **Goose Runtime & Stack** | Bump-allocated data stack | ~0.4 MB |
| **Win32 Double-Buffered GDI** | Compatible DC + DIB surface | ~1.1 MB |
| **Worker Thread & State** | High-precision Win32 thread | ~0.3 MB |
| **Total Peak Working Set** | Bare-metal Windows 10/11 x64 | **~1.8 – 2.8 MB** (Target: < 10 MB) |

## 2. Low-Level Event Dispatch
- **Pacing Engine:** High-resolution multimedia timer initialized via `timeBeginPeriod(1)` coupled with `QueryPerformanceCounter` hardware frequency clocks.
- **Event Injection:** Directly issues Win32 `SendInput` mouse packets (`MOUSEEVENTF_LEFTDOWN` / `MOUSEEVENTF_LEFTUP`) with natural biomechanical button hold down durations (16–20ms).
- **Anti-Pattern Gaussian Jitter:** Employs the Box-Muller transform to introduce micro-fluctuations in click intervals, preventing static periodic detection patterns.

## 3. UI System
- **Framework:** Custom double-buffered GDI surface rendering at native monitor refresh rate with ClearType typography.
- **Palette:** Matches `promotezz.cc`:
  - Deep matte black: `#09090b`
  - Subtle borders: `#27272a`
  - Surfaces: `#121215`
  - Accents: Emerald `#10b981` / Ruby `#ef4444` / Clean white `#ffffff`
- **Dynamic Controls:**
  - Dynamic Island top status pill
  - Interactive click speed rail (6.0 – 24.0 CPS)
  - Hotkey switcher (`F6`, `X`, `C`, `V`, `Mouse 4`)
  - Audio feedback toggle
  - Live click counter & memory gauge
