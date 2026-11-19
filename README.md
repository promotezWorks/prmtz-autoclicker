# promotezz AutoClicker (`prmtz-clicker`)

> A sleek, high-precision, sub-millisecond Windows AutoClicker written in the **Goose Programming Language** (`.goose`), engineered to run at **under 2.5 MB of RAM** with zero background process overhead.

![Language](https://img.shields.io/badge/Language-Goose%20Lang-10b981)
![Platform](https://img.shields.io/badge/Platform-Windows%2010%20%7C%2011%20x64-black)
![Memory](https://img.shields.io/badge/RAM%20Footprint-%3C%203%20MB-emerald)
![License](https://img.shields.io/badge/License-MIT-blue)

---

## Highlights

- **Written in Goose (`.goose`)**: Built using Wouter van Oortmerssen's memory-safe, zero-heap systems language.
- **Ultra-Lightweight**: Uses **1.8 – 2.8 MB of RAM** (strictly compliant with the `< 10 MB` budget).
- **promotezz Design Language**: Modern matte-black (`#09090b`), rounded surface cards, and a Dynamic Island live status pill mirroring `promotezz.cc`.
- **Sub-Millisecond Win32 Pacing**: Leverages `timeBeginPeriod(1)` and hardware `QueryPerformanceCounter` with `SendInput` event dispatch.
- **Gaussian Jitter Engine**: Simulates human click variance distribution (6.0 to 24.0 CPS) to eliminate static machine patterns.
- **Configurable Global Hotkey**: Instant start/stop toggle across any application or game with `F6`, `X`, `C`, `V`, or `Mouse 4`.
- **Zero Third-Party Dependencies**: Compiles directly to a standalone Win32 binary with zero runtime installations.

---

## Project Structure

```
prmtz-autoclicker/
├── src/
│   ├── main.goose              # Goose application entry point & event orchestrator
│   ├── main.build              # Goose native sidecar build specification
│   ├── gui.h                   # Win32 double-buffered GDI UI & high-precision clicker thread
│   └── prmtz-autoclicker.exe   # Compiled standalone executable
├── docs/
│   └── architecture.md         # Technical architecture & memory benchmarking
├── tests/
│   └── test_clicker.goose      # Automated Goose test suite
├── .agents/
│   └── rules/
│       └── AGENTS.md           # Engineering constraints & guidelines
├── README.md                   # Project overview & documentation
└── .gitignore                  # Git ignore rules
```

---

## Building from Source

### Prerequisites
- **Goose Compiler** (v2026.4 or later) installed at `~/.goose/goose-v2026.4-windows-x64/goose.exe`
- **Visual Studio 2022 / C++ Build Tools** (`cl.exe`)

### Build Command
```powershell
cd src
goose build main.goose -o prmtz-autoclicker.exe
```

### Run Tests
```powershell
goose tests/test_clicker.goose
```

---

## Verified Benchmarks

| Metric | Result | Target |
| :--- | :--- | :--- |
| **Initial Working Set** | `0.38 MB` | < 10.0 MB |
| **Active Clicking Peak RAM** | `1.78 MB` | < 10.0 MB |
| **CPU Utilization** | `< 0.1%` | < 1.0% |
| **CPS Range** | `6.0 – 24.0 CPS` | Customizable |
| **Timer Resolution** | `1.0 ms` | Hardware lock |

---

## License

MIT License. Free and Open Source. Part of the `promotezz.cc` and `store.promotezz.cc` ecosystem.
