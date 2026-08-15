#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <winternl.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"Tabbedpread.TabStrip";
constexpr wchar_t kAppName[] = L"Tabbedpread";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshMs = 90;

constexpr int kHotkeyAdd = 1;
constexpr int kHotkeyPrev = 2;
constexpr int kHotkeyNext = 3;
constexpr int kHotkeyRemove = 4;
constexpr int kHotkeyBackdrop = 5;
constexpr int kHotkeyNewGroup = 6;

constexpr UINT kMenuAdd = 1001;
constexpr UINT kMenuNewGroup = 1002;
constexpr UINT kMenuRemove = 1003;
constexpr UINT kMenuBackdrop = 1004;
constexpr UINT kMenuExit = 1005;

// Numeric values keep this source buildable with SDKs that predate these fields.
constexpr DWMWINDOWATTRIBUTE kDwmwaSystemBackdropType = static_cast<DWMWINDOWATTRIBUTE>(38);
constexpr DWMWINDOWATTRIBUTE kDwmwaLegacyMicaEffect = static_cast<DWMWINDOWATTRIBUTE>(1029);
constexpr DWMWINDOWATTRIBUTE kDwmwaUseImmersiveDarkMode = static_cast<DWMWINDOWATTRIBUTE>(20);
constexpr int kSystemBackdropAuto = 0;
constexpr int kSystemBackdropTabbedWindow = 4; // Mica Alt on Windows 11 22H2+

struct TabGroup {
    std::vector<HWND> windows;
    size_t active = 0;
    RECT normalRect{};
    bool maximized = false;
};

struct Membership {
    int group = -1;
    int tab = -1;
};

struct SavedBackdrop {
    bool hasSystemBackdrop = false;
    int systemBackdrop = kSystemBackdropAuto;
    bool hasLegacyMica = false;
    BOOL legacyMica = FALSE;
};

struct AccentPolicy {
    int accentState;
    int accentFlags;
    DWORD gradientColor;
    int animationId;
};

struct WindowCompositionAttribData {
    int attrib;
    PVOID data;
    SIZE_T sizeOfData;
};

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttribData*);

enum AccentState {
    AccentDisabled = 0,
    AccentEnableBlurBehind = 3,
    AccentEnableAcrylicBlurBehind = 4,
};

constexpr int kWindowCompositionAttribAccentPolicy = 19;

HINSTANCE g_instance = nullptr;
HWND g_overlay = nullptr;
HWND g_lastForeground = nullptr;
std::vector<TabGroup> g_groups;
int g_selectedGroup = -1;
bool g_backdropEnabled = true;
DWORD g_windowsBuild = 0;
NOTIFYICONDATAW g_tray{};
std::unordered_map<HWND, SavedBackdrop> g_savedBackdrops;

DWORD GetWindowsBuild() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtlGetVersion) {
            RTL_OSVERSIONINFOW version{};
            version.dwOSVersionInfoSize = sizeof(version);
            if (rtlGetVersion(&version) == 0) {
                return version.dwBuildNumber;
            }
        }
    }

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
#pragma warning(push)
#pragma warning(disable : 4996)
    if (GetVersionExW(&version)) {
        return version.dwBuildNumber;
    }
#pragma warning(pop)
    return 0;
}

bool IsLightTheme() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS result = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &size);
    return result == ERROR_SUCCESS ? value != 0 : false;
}

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

bool IsCandidateWindow(HWND hwnd) {
    if (!hwnd || hwnd == g_overlay || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return false;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0 || (exStyle & WS_EX_NOACTIVATE) != 0) {
        return false;
    }
    if (IsCloaked(hwnd)) {
        return false;
    }

    wchar_t className[128]{};
    GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
    if (lstrcmpW(className, L"Progman") == 0 ||
        lstrcmpW(className, L"WorkerW") == 0 ||
        lstrcmpW(className, L"Shell_TrayWnd") == 0 ||
        lstrcmpW(className, L"Shell_SecondaryTrayWnd") == 0) {
        return false;
    }

    return true;
}

std::wstring WindowTitle(HWND hwnd) {
    int length = GetWindowTextLengthW(hwnd);
    std::wstring title;
    if (length > 0) {
        title.resize(static_cast<size_t>(length) + 1);
        const int copied = GetWindowTextW(hwnd, title.data(), length + 1);
        title.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    }

    if (!title.empty()) {
        return title;
    }

    wchar_t className[128]{};
    if (GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) > 0) {
        return className;
    }
    return L"Window";
}

Membership FindMembership(HWND hwnd) {
    for (size_t gi = 0; gi < g_groups.size(); ++gi) {
        for (size_t ti = 0; ti < g_groups[gi].windows.size(); ++ti) {
            if (g_groups[gi].windows[ti] == hwnd) {
                return {static_cast<int>(gi), static_cast<int>(ti)};
            }
        }
    }
    return {};
}

void RememberForeground(HWND hwnd) {
    if (IsCandidateWindow(hwnd)) {
        g_lastForeground = hwnd;
    }
}

HWND TargetForeground() {
    HWND hwnd = GetForegroundWindow();
    if (IsCandidateWindow(hwnd)) {
        RememberForeground(hwnd);
        return hwnd;
    }
    return IsCandidateWindow(g_lastForeground) ? g_lastForeground : nullptr;
}

void CapturePlacement(TabGroup& group) {
    if (group.windows.empty() || group.active >= group.windows.size()) {
        return;
    }

    HWND hwnd = group.windows[group.active];
    if (!IsWindow(hwnd)) {
        return;
    }

    group.maximized = IsZoomed(hwnd) != FALSE;
    if (!group.maximized && !IsIconic(hwnd)) {
        RECT rect{};
        if (GetWindowRect(hwnd, &rect)) {
            group.normalRect = rect;
        }
    }
}

bool EnableAcrylicFallback(HWND hwnd, bool enabled) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return false;
    }

    auto setComposition = reinterpret_cast<SetWindowCompositionAttributeFn>(
        GetProcAddress(user32, "SetWindowCompositionAttribute"));
    if (!setComposition) {
        return false;
    }

    AccentPolicy policy{};
    policy.accentState = enabled ? AccentEnableAcrylicBlurBehind : AccentDisabled;
    policy.accentFlags = 2;
    // AABBGGRR; neutral tint so it works with arbitrary accent colors.
    policy.gradientColor = IsLightTheme() ? 0xCCF2F2F2u : 0xCC202020u;

    WindowCompositionAttribData data{};
    data.attrib = kWindowCompositionAttribAccentPolicy;
    data.data = &policy;
    data.sizeOfData = sizeof(policy);

    if (setComposition(hwnd, &data)) {
        return true;
    }

    if (enabled) {
        policy.accentState = AccentEnableBlurBehind;
        return setComposition(hwnd, &data) != FALSE;
    }
    return false;
}

void ApplyOverlayBackdrop() {
    if (!g_overlay) {
        return;
    }

    const BOOL dark = IsLightTheme() ? FALSE : TRUE;
    if (g_windowsBuild >= 22000) {
        DwmSetWindowAttribute(g_overlay, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark));
    }

    MARGINS margins{-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(g_overlay, &margins);

    if (!g_backdropEnabled) {
        const int none = kSystemBackdropAuto;
        const BOOL micaOff = FALSE;
        DwmSetWindowAttribute(g_overlay, kDwmwaSystemBackdropType, &none, sizeof(none));
        DwmSetWindowAttribute(g_overlay, kDwmwaLegacyMicaEffect, &micaOff, sizeof(micaOff));
        EnableAcrylicFallback(g_overlay, false);
        InvalidateRect(g_overlay, nullptr, TRUE);
        return;
    }

    if (g_windowsBuild >= 22621) {
        const int backdrop = kSystemBackdropTabbedWindow;
        if (SUCCEEDED(DwmSetWindowAttribute(
                g_overlay, kDwmwaSystemBackdropType, &backdrop, sizeof(backdrop)))) {
            EnableAcrylicFallback(g_overlay, false);
            InvalidateRect(g_overlay, nullptr, TRUE);
            return;
        }
    }

    if (g_windowsBuild >= 22000) {
        // Windows 11 21H2 has no supported Mica Alt system-backdrop enum.
        // The old Mica switch is tried dynamically, then Acrylic is the fallback.
        const BOOL micaOn = TRUE;
        if (SUCCEEDED(DwmSetWindowAttribute(
                g_overlay, kDwmwaLegacyMicaEffect, &micaOn, sizeof(micaOn)))) {
            InvalidateRect(g_overlay, nullptr, TRUE);
            return;
        }
    }

    EnableAcrylicFallback(g_overlay, true);
    InvalidateRect(g_overlay, nullptr, TRUE);
}

void SaveAndApplyTargetBackdrop(HWND hwnd) {
    if (!g_backdropEnabled || !IsWindow(hwnd) || g_windowsBuild < 22000) {
        return;
    }

    if (!g_savedBackdrops.contains(hwnd)) {
        SavedBackdrop saved{};
        if (g_windowsBuild >= 22621) {
            int current = kSystemBackdropAuto;
            if (SUCCEEDED(DwmGetWindowAttribute(
                    hwnd, kDwmwaSystemBackdropType, &current, sizeof(current)))) {
                saved.hasSystemBackdrop = true;
                saved.systemBackdrop = current;
            }
        } else {
            BOOL current = FALSE;
            if (SUCCEEDED(DwmGetWindowAttribute(
                    hwnd, kDwmwaLegacyMicaEffect, &current, sizeof(current)))) {
                saved.hasLegacyMica = true;
                saved.legacyMica = current;
            }
        }
        g_savedBackdrops.emplace(hwnd, saved);
    }

    if (g_windowsBuild >= 22621) {
        const int backdrop = kSystemBackdropTabbedWindow;
        DwmSetWindowAttribute(hwnd, kDwmwaSystemBackdropType, &backdrop, sizeof(backdrop));
    } else {
        const BOOL micaOn = TRUE;
        DwmSetWindowAttribute(hwnd, kDwmwaLegacyMicaEffect, &micaOn, sizeof(micaOn));
    }
}

void RestoreTargetBackdrop(HWND hwnd) {
    auto it = g_savedBackdrops.find(hwnd);
    if (it == g_savedBackdrops.end()) {
        return;
    }

    if (IsWindow(hwnd)) {
        if (g_windowsBuild >= 22621) {
            const int value = it->second.hasSystemBackdrop
                ? it->second.systemBackdrop
                : kSystemBackdropAuto;
            DwmSetWindowAttribute(hwnd, kDwmwaSystemBackdropType, &value, sizeof(value));
        } else if (g_windowsBuild >= 22000) {
            const BOOL value = it->second.hasLegacyMica ? it->second.legacyMica : FALSE;
            DwmSetWindowAttribute(hwnd, kDwmwaLegacyMicaEffect, &value, sizeof(value));
        }
    }

    g_savedBackdrops.erase(it);
}

void RestoreAllBackdrops() {
    std::vector<HWND> handles;
    handles.reserve(g_savedBackdrops.size());
    for (const auto& entry : g_savedBackdrops) {
        handles.push_back(entry.first);
    }
    for (HWND hwnd : handles) {
        RestoreTargetBackdrop(hwnd);
    }
}

void PositionOverlayFor(HWND target, const TabGroup& group) {
    if (!g_overlay || !IsWindow(target)) {
        return;
    }

    RECT rect{};
    if (!GetWindowRect(target, &rect)) {
        ShowWindow(g_overlay, SW_HIDE);
        return;
    }

    const UINT dpi = GetDpiForWindow(target);
    const auto scale = [dpi](int px) {
        return MulDiv(px, static_cast<int>(dpi), 96);
    };

    const int targetWidth = std::max(0L, rect.right - rect.left);
    const int reservedCaptionButtons = scale(150);
    const int maxWidth = scale(980);
    const int minWidth = scale(150);
    int width = targetWidth - reservedCaptionButtons - scale(10);
    width = std::clamp(width, minWidth, maxWidth);
    width = std::min(width, std::max(minWidth, targetWidth - scale(16)));
    const int height = scale(36);

    SetWindowPos(
        g_overlay,
        HWND_TOPMOST,
        rect.left + scale(7),
        rect.top + scale(2),
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);

    const int radius = scale(8);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    if (region) {
        SetWindowRgn(g_overlay, region, TRUE); // The system owns region after success.
    }

    SaveAndApplyTargetBackdrop(target);
    InvalidateRect(g_overlay, nullptr, FALSE);
}

void HideOverlay() {
    if (g_overlay && IsWindowVisible(g_overlay)) {
        ShowWindow(g_overlay, SW_HIDE);
    }
}

void UpdateOverlay() {
    if (g_groups.empty()) {
        HideOverlay();
        return;
    }

    HWND foreground = GetForegroundWindow();
    if (foreground == g_overlay) {
        foreground = g_lastForeground;
    } else {
        RememberForeground(foreground);
    }

    Membership member = FindMembership(foreground);
    if (member.group < 0) {
        HideOverlay();
        return;
    }

    g_selectedGroup = member.group;
    TabGroup& group = g_groups[static_cast<size_t>(member.group)];
    group.active = static_cast<size_t>(member.tab);
    CapturePlacement(group);
    PositionOverlayFor(foreground, group);
}

void PruneGroups() {
    for (int gi = static_cast<int>(g_groups.size()) - 1; gi >= 0; --gi) {
        TabGroup& group = g_groups[static_cast<size_t>(gi)];
        for (size_t i = group.windows.size(); i-- > 0;) {
            if (!IsWindow(group.windows[i])) {
                g_savedBackdrops.erase(group.windows[i]);
                group.windows.erase(group.windows.begin() + static_cast<std::ptrdiff_t>(i));
                if (group.active > i && group.active > 0) {
                    --group.active;
                }
            }
        }

        if (group.windows.empty()) {
            g_groups.erase(g_groups.begin() + gi);
            if (g_selectedGroup == gi) {
                g_selectedGroup = -1;
            } else if (g_selectedGroup > gi) {
                --g_selectedGroup;
            }
            continue;
        }

        if (group.active >= group.windows.size()) {
            group.active = group.windows.size() - 1;
        }
    }

    if (g_selectedGroup >= static_cast<int>(g_groups.size())) {
        g_selectedGroup = g_groups.empty() ? -1 : static_cast<int>(g_groups.size()) - 1;
    }
}

void ActivateTab(int groupIndex, size_t tabIndex) {
    if (groupIndex < 0 || groupIndex >= static_cast<int>(g_groups.size())) {
        return;
    }

    TabGroup& group = g_groups[static_cast<size_t>(groupIndex)];
    if (tabIndex >= group.windows.size()) {
        return;
    }

    CapturePlacement(group);
    HWND current = group.active < group.windows.size() ? group.windows[group.active] : nullptr;
    HWND target = group.windows[tabIndex];
    if (!IsWindow(target)) {
        PruneGroups();
        return;
    }

    if (current && current != target && IsWindow(current)) {
        ShowWindow(current, SW_HIDE);
    }

    if (!group.maximized) {
        const int width = group.normalRect.right - group.normalRect.left;
        const int height = group.normalRect.bottom - group.normalRect.top;
        if (width > 0 && height > 0) {
            SetWindowPos(
                target,
                nullptr,
                group.normalRect.left,
                group.normalRect.top,
                width,
                height,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ShowWindow(target, SW_RESTORE);
    } else {
        ShowWindow(target, SW_MAXIMIZE);
    }

    group.active = tabIndex;
    g_selectedGroup = groupIndex;
    SaveAndApplyTargetBackdrop(target);
    SetForegroundWindow(target);
    g_lastForeground = target;
    PositionOverlayFor(target, group);
}

void StartNewGroup(HWND hwnd) {
    if (!IsCandidateWindow(hwnd)) {
        return;
    }

    Membership existing = FindMembership(hwnd);
    if (existing.group >= 0) {
        g_selectedGroup = existing.group;
        ActivateTab(existing.group, static_cast<size_t>(existing.tab));
        return;
    }

    TabGroup group{};
    group.windows.push_back(hwnd);
    group.active = 0;
    group.maximized = IsZoomed(hwnd) != FALSE;
    if (!group.maximized) {
        GetWindowRect(hwnd, &group.normalRect);
    }

    g_groups.push_back(group);
    g_selectedGroup = static_cast<int>(g_groups.size()) - 1;
    SaveAndApplyTargetBackdrop(hwnd);
    PositionOverlayFor(hwnd, g_groups.back());
}

void AddWindowToSelectedGroup(HWND hwnd) {
    if (!IsCandidateWindow(hwnd)) {
        return;
    }

    Membership existing = FindMembership(hwnd);
    if (existing.group >= 0) {
        g_selectedGroup = existing.group;
        ActivateTab(existing.group, static_cast<size_t>(existing.tab));
        return;
    }

    if (g_selectedGroup < 0 || g_selectedGroup >= static_cast<int>(g_groups.size())) {
        StartNewGroup(hwnd);
        return;
    }

    TabGroup& group = g_groups[static_cast<size_t>(g_selectedGroup)];
    if (group.windows.empty()) {
        StartNewGroup(hwnd);
        return;
    }

    CapturePlacement(group);
    HWND previous = group.windows[group.active];
    if (previous && previous != hwnd && IsWindow(previous)) {
        ShowWindow(previous, SW_HIDE);
    }

    group.windows.push_back(hwnd);
    group.active = group.windows.size() - 1;

    if (!group.maximized) {
        const int width = group.normalRect.right - group.normalRect.left;
        const int height = group.normalRect.bottom - group.normalRect.top;
        if (width > 0 && height > 0) {
            SetWindowPos(
                hwnd,
                nullptr,
                group.normalRect.left,
                group.normalRect.top,
                width,
                height,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_MAXIMIZE);
    }

    SaveAndApplyTargetBackdrop(hwnd);
    SetForegroundWindow(hwnd);
    g_lastForeground = hwnd;
    PositionOverlayFor(hwnd, group);
}

void RemoveWindowFromGroup(HWND hwnd) {
    Membership membership = FindMembership(hwnd);
    if (membership.group < 0) {
        return;
    }

    TabGroup& group = g_groups[static_cast<size_t>(membership.group)];
    const size_t index = static_cast<size_t>(membership.tab);
    const bool wasActive = group.active == index;
    const RECT lastRect = group.normalRect;
    const bool wasMaximized = group.maximized;

    RestoreTargetBackdrop(hwnd);
    group.windows.erase(group.windows.begin() + static_cast<std::ptrdiff_t>(index));

    if (group.windows.empty()) {
        g_groups.erase(g_groups.begin() + membership.group);
        if (g_selectedGroup == membership.group) {
            g_selectedGroup = -1;
        } else if (g_selectedGroup > membership.group) {
            --g_selectedGroup;
        }
        ShowWindow(hwnd, wasMaximized ? SW_MAXIMIZE : SW_RESTORE);
        SetForegroundWindow(hwnd);
        HideOverlay();
        return;
    }

    if (group.active > index) {
        --group.active;
    }
    if (group.active >= group.windows.size()) {
        group.active = group.windows.size() - 1;
    }

    if (wasActive) {
        HWND replacement = group.windows[group.active];
        if (!wasMaximized) {
            const int width = lastRect.right - lastRect.left;
            const int height = lastRect.bottom - lastRect.top;
            if (width > 0 && height > 0) {
                SetWindowPos(
                    replacement,
                    nullptr,
                    lastRect.left,
                    lastRect.top,
                    width,
                    height,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            ShowWindow(replacement, SW_RESTORE);
        } else {
            ShowWindow(replacement, SW_MAXIMIZE);
        }
        SaveAndApplyTargetBackdrop(replacement);
    }

    // The removed app becomes independent and remains available to the user.
    ShowWindow(hwnd, wasMaximized ? SW_MAXIMIZE : SW_RESTORE);
    SetForegroundWindow(hwnd);
    g_lastForeground = hwnd;
    HideOverlay();
}

void CycleTab(int delta) {
    HWND foreground = TargetForeground();
    Membership membership = FindMembership(foreground);
    int groupIndex = membership.group >= 0 ? membership.group : g_selectedGroup;
    if (groupIndex < 0 || groupIndex >= static_cast<int>(g_groups.size())) {
        return;
    }

    TabGroup& group = g_groups[static_cast<size_t>(groupIndex)];
    if (group.windows.size() < 2) {
        return;
    }

    const int count = static_cast<int>(group.windows.size());
    const int current = static_cast<int>(group.active);
    const int next = (current + delta + count) % count;
    ActivateTab(groupIndex, static_cast<size_t>(next));
}

void ToggleBackdrop() {
    g_backdropEnabled = !g_backdropEnabled;
    if (!g_backdropEnabled) {
        RestoreAllBackdrops();
    } else {
        for (auto& group : g_groups) {
            for (HWND hwnd : group.windows) {
                SaveAndApplyTargetBackdrop(hwnd);
            }
        }
    }
    ApplyOverlayBackdrop();
    UpdateOverlay();
}

RECT TabRect(size_t index, size_t count, const RECT& client, UINT dpi) {
    const int pad = MulDiv(4, static_cast<int>(dpi), 96);
    const int gap = MulDiv(3, static_cast<int>(dpi), 96);
    const int maxTab = MulDiv(220, static_cast<int>(dpi), 96);
    const int width = client.right - client.left;
    const int available = std::max(1, width - pad * 2 - gap * static_cast<int>(count - 1));
    int tabWidth = std::max(1, available / static_cast<int>(std::max<size_t>(count, 1)));
    tabWidth = std::min(tabWidth, maxTab);
    const int left = pad + static_cast<int>(index) * (tabWidth + gap);
    return {left, pad, left + tabWidth, client.bottom - pad};
}

void PaintOverlay(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    if (!dc) {
        return;
    }

    RECT client{};
    GetClientRect(hwnd, &client);
    const UINT dpi = GetDpiForWindow(hwnd);
    const bool light = IsLightTheme();

    // Paint a neutral title-bar base. DWM/Acrylic remains visible around the rounded tabs.
    HBRUSH baseBrush = CreateSolidBrush(light ? RGB(232, 232, 232) : RGB(28, 28, 28));
    FillRect(dc, &client, baseBrush);
    DeleteObject(baseBrush);

    if (g_selectedGroup < 0 || g_selectedGroup >= static_cast<int>(g_groups.size())) {
        EndPaint(hwnd, &ps);
        return;
    }

    const TabGroup& group = g_groups[static_cast<size_t>(g_selectedGroup)];
    if (group.windows.empty()) {
        EndPaint(hwnd, &ps);
        return;
    }

    DWORD colorization = 0;
    BOOL opaque = FALSE;
    COLORREF accent = RGB(0, 120, 215);
    if (SUCCEEDED(DwmGetColorizationColor(&colorization, &opaque))) {
        accent = RGB(
            (colorization >> 16) & 0xFF,
            (colorization >> 8) & 0xFF,
            colorization & 0xFF);
    }

    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font));
    SetBkMode(dc, TRANSPARENT);

    const int iconSize = MulDiv(16, static_cast<int>(dpi), 96);
    const int inset = MulDiv(8, static_cast<int>(dpi), 96);
    const int indicatorHeight = std::max(2, MulDiv(2, static_cast<int>(dpi), 96));

    for (size_t i = 0; i < group.windows.size(); ++i) {
        RECT tab = TabRect(i, group.windows.size(), client, dpi);
        const bool active = i == group.active;
        HBRUSH tabBrush = CreateSolidBrush(
            active
                ? (light ? RGB(250, 250, 250) : RGB(52, 52, 52))
                : (light ? RGB(239, 239, 239) : RGB(38, 38, 38)));
        HPEN pen = CreatePen(PS_SOLID, 1, light ? RGB(218, 218, 218) : RGB(62, 62, 62));
        HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(dc, tabBrush));
        HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
        const int radius = MulDiv(7, static_cast<int>(dpi), 96);
        RoundRect(dc, tab.left, tab.top, tab.right, tab.bottom, radius, radius);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(tabBrush);

        HWND window = group.windows[i];
        HICON icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICONSM));
        if (!icon) {
            icon = reinterpret_cast<HICON>(GetClassLongPtrW(window, GCLP_HICON));
        }

        int textLeft = tab.left + inset;
        if (icon) {
            DrawIconEx(
                dc,
                tab.left + inset,
                tab.top + (tab.bottom - tab.top - iconSize) / 2,
                icon,
                iconSize,
                iconSize,
                0,
                nullptr,
                DI_NORMAL);
            textLeft += iconSize + MulDiv(6, static_cast<int>(dpi), 96);
        }

        RECT textRect{
            textLeft,
            tab.top,
            tab.right - inset,
            tab.bottom - indicatorHeight};
        std::wstring title = WindowTitle(window);
        SetTextColor(dc, light ? RGB(28, 28, 28) : RGB(245, 245, 245));
        DrawTextW(
            dc,
            title.c_str(),
            static_cast<int>(title.size()),
            &textRect,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        if (active) {
            RECT indicator{
                tab.left + inset,
                tab.bottom - indicatorHeight,
                tab.right - inset,
                tab.bottom};
            HBRUSH accentBrush = CreateSolidBrush(accent);
            FillRect(dc, &indicator, accentBrush);
            DeleteObject(accentBrush);
        }
    }

    SelectObject(dc, oldFont);
    EndPaint(hwnd, &ps);
}

void ShowTrayMenu(HWND hwnd) {
    HWND before = GetForegroundWindow();
    RememberForeground(before);

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kMenuAdd, L"Add foreground window to selected group\tWin+Alt+T");
    AppendMenuW(menu, MF_STRING, kMenuNewGroup, L"Start new group from foreground\tWin+Alt+N");
    AppendMenuW(menu, MF_STRING, kMenuRemove, L"Remove foreground tab\tWin+Alt+U");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        menu,
        MF_STRING | (g_backdropEnabled ? MF_CHECKED : MF_UNCHECKED),
        kMenuBackdrop,
        L"Mica Alt / backdrop effect\tWin+Alt+B");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit Tabbedpread");

    POINT point{};
    GetCursorPos(&point);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, point.x, point.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

void Cleanup() {
    KillTimer(g_overlay, kRefreshTimer);

    for (auto& group : g_groups) {
        for (HWND hwnd : group.windows) {
            if (IsWindow(hwnd)) {
                ShowWindow(hwnd, IsZoomed(hwnd) ? SW_MAXIMIZE : SW_RESTORE);
            }
        }
    }
    RestoreAllBackdrops();

    UnregisterHotKey(g_overlay, kHotkeyAdd);
    UnregisterHotKey(g_overlay, kHotkeyPrev);
    UnregisterHotKey(g_overlay, kHotkeyNext);
    UnregisterHotKey(g_overlay, kHotkeyRemove);
    UnregisterHotKey(g_overlay, kHotkeyBackdrop);
    UnregisterHotKey(g_overlay, kHotkeyNewGroup);

    if (g_tray.cbSize != 0) {
        Shell_NotifyIconW(NIM_DELETE, &g_tray);
        g_tray = {};
    }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        PaintOverlay(hwnd);
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_LBUTTONUP: {
        if (g_selectedGroup < 0 || g_selectedGroup >= static_cast<int>(g_groups.size())) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT client{};
        GetClientRect(hwnd, &client);
        const UINT dpi = GetDpiForWindow(hwnd);
        const auto& group = g_groups[static_cast<size_t>(g_selectedGroup)];
        for (size_t i = 0; i < group.windows.size(); ++i) {
            RECT tab = TabRect(i, group.windows.size(), client, dpi);
            if (PtInRect(&tab, point)) {
                ActivateTab(g_selectedGroup, i);
                break;
            }
        }
        return 0;
    }

    case WM_MOUSEWHEEL:
        CycleTab(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
        return 0;

    case WM_HOTKEY:
        switch (static_cast<int>(wParam)) {
        case kHotkeyAdd:
            AddWindowToSelectedGroup(TargetForeground());
            break;
        case kHotkeyPrev:
            CycleTab(-1);
            break;
        case kHotkeyNext:
            CycleTab(1);
            break;
        case kHotkeyRemove:
            RemoveWindowFromGroup(TargetForeground());
            break;
        case kHotkeyBackdrop:
            ToggleBackdrop();
            break;
        case kHotkeyNewGroup:
            StartNewGroup(TargetForeground());
            break;
        default:
            break;
        }
        return 0;

    case WM_TIMER:
        if (wParam == kRefreshTimer) {
            PruneGroups();
            UpdateOverlay();
        }
        return 0;

    case kTrayMessage:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            AddWindowToSelectedGroup(TargetForeground());
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kMenuAdd:
            AddWindowToSelectedGroup(TargetForeground());
            break;
        case kMenuNewGroup:
            StartNewGroup(TargetForeground());
            break;
        case kMenuRemove:
            RemoveWindowFromGroup(TargetForeground());
            break;
        case kMenuBackdrop:
            ToggleBackdrop();
            break;
        case kMenuExit:
            DestroyWindow(hwnd);
            break;
        default:
            break;
        }
        return 0;

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOMPOSITIONCHANGED:
        ApplyOverlayBackdrop();
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_DESTROY:
        Cleanup();
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

bool AddTrayIcon(HWND hwnd) {
    g_tray = {};
    g_tray.cbSize = sizeof(g_tray);
    g_tray.hWnd = hwnd;
    g_tray.uID = 1;
    g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_tray.uCallbackMessage = kTrayMessage;
    g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(g_tray.szTip, L"Tabbedpread", static_cast<int>(std::size(g_tray.szTip)));
    return Shell_NotifyIconW(NIM_ADD, &g_tray) != FALSE;
}

void RegisterHotkeys(HWND hwnd) {
    constexpr UINT mods = MOD_WIN | MOD_ALT | MOD_NOREPEAT;
    RegisterHotKey(hwnd, kHotkeyAdd, mods, 'T');
    RegisterHotKey(hwnd, kHotkeyPrev, mods, VK_LEFT);
    RegisterHotKey(hwnd, kHotkeyNext, mods, VK_RIGHT);
    RegisterHotKey(hwnd, kHotkeyRemove, mods, 'U');
    RegisterHotKey(hwnd, kHotkeyBackdrop, mods, 'B');
    RegisterHotKey(hwnd, kHotkeyNewGroup, mods, 'N');
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Tabbedpread.Singleton");
    if (!mutex) {
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Tabbedpread is already running.", kAppName, MB_OK | MB_ICONINFORMATION);
        CloseHandle(mutex);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_windowsBuild = GetWindowsBuild();
    if (g_windowsBuild != 0 && g_windowsBuild < 16299) {
        MessageBoxW(
            nullptr,
            L"Tabbedpread requires Windows 10 1709 (build 16299) or newer.",
            kAppName,
            MB_OK | MB_ICONERROR);
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 2;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hbrBackground = nullptr;

    if (!RegisterClassExW(&windowClass)) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 3;
    }

    g_overlay = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kWindowClass,
        kAppName,
        WS_POPUP,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        600,
        36,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!g_overlay) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 4;
    }

    ApplyOverlayBackdrop();
    AddTrayIcon(g_overlay);
    RegisterHotkeys(g_overlay);
    SetTimer(g_overlay, kRefreshTimer, kRefreshMs, nullptr);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return static_cast<int>(message.wParam);
}
