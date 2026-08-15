# Tabbedpread

Tabbedpread is a native Win32 utility that gives ordinary desktop windows a lightweight **tabbed-window effect** and a **Mica Alt-style tab strip** without injecting DLLs into other processes.

## OS support

- **Windows 10 1709 (build 16299)+** — tab grouping with an Acrylic/blur-style composition fallback.
- **Windows 11 21H2 (build 22000)** — tab grouping with a best-effort Mica/Acrylic fallback.
- **Windows 11 22H2 (build 22621)+** — uses the supported `DWMWA_SYSTEMBACKDROP_TYPE` / `DWMSBT_TABBEDWINDOW` path for native Mica Alt where available.

Tabbedpread checks the Windows build at runtime, so the same executable stays compatible with older supported systems instead of importing newer-only APIs.

## Current controls

| Action | Shortcut |
| --- | --- |
| Start a group / add the foreground window to the selected group | `Win + Alt + T` |
| Previous tab | `Win + Alt + Left` |
| Next tab | `Win + Alt + Right` |
| Remove the foreground tab from its group | `Win + Alt + U` |
| Toggle the backdrop effect | `Win + Alt + B` |

The first press of **Win+Alt+T** on an ungrouped window creates a group seed. Focus another window and press it again to add that window to the same group. Tabbedpread keeps grouped windows aligned and switches them by showing the selected window and hiding the other members of the group.

A tray icon provides the same core actions and Exit.

## Design

- Native C++/Win32; no framework/runtime dependency.
- No code injection and no shell replacement.
- Per-monitor DPI aware.
- Runtime DWM feature detection.
- Mica Alt on Windows versions that officially expose it; graceful visual fallbacks elsewhere.
- Ordinary app windows remain owned by their original processes.

## Build

Requirements: Visual Studio 2022 with the Desktop development with C++ workload and CMake.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable will be at `build/Release/tabbedpread.exe`.

GitHub Actions builds x64 and x86 packages on every push and pull request.

## Notes / limitations

This first implementation creates the tabbed effect as a companion overlay rather than rewriting another application's non-client area. Some elevated, protected, custom-rendered, or unusual top-level windows may reject positioning/show-state changes. Native Mica Alt itself is an operating-system feature introduced after Windows 11 21H2, so older systems use a visual fallback rather than pretending the unavailable API exists.

## License

MIT
