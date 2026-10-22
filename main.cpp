#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <map>
#include <vector>
#include <algorithm>
#include <string>

// 自動連結 Win32 系統庫
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

// 選單控制 ID
#define ID_TRAY_EXIT          1001
#define ID_TRAY_RELEASE_ALL   1002
#define ID_TRAY_RELOAD_CONFIG 1003
#define ID_TRAY_PINNED_BASE   2000

// 全域組態設定參數
COLORREF BorderColor     = RGB(0x8C, 0xF2, 0x87);
int      BorderThickness = 3;
int      CornerRadius    = 10;
int      TargetOpacity   = 200; // 不透明度 (0~255)
int      OffsetX = 0;
int      OffsetY = 0;
int      OffsetW = 0;
int      OffsetH = 0;

struct OverlayInfo {
    HWND borderHwnd;
    UINT_PTR timerId;
};

std::map<HWND, OverlayInfo> ActiveToggles;
std::vector<HWND> g_MenuWindowMap;
HINSTANCE g_hInstance = NULL;
HWND g_ControllerHwnd = NULL;
NOTIFYICONDATA nid = { 0 };

// 函數前置宣告
void ToggleOverlay();
void ClearAllOverlays();
void PinWindow(HWND targetHwnd);
void UnpinWindow(HWND targetHwnd);
bool IsValidWindow(HWND hwnd);
bool GetVisibleWinPos(HWND hwnd, RECT* outRect);
void SyncBorderPosition(HWND targetHwnd, HWND borderHwnd);
void LoadConfig();

// 轉換 Hex 顏色字串為 COLORREF (RGB)
COLORREF HexToCOLORREF(const std::wstring& hexStr) {
    std::wstring cleanHex = hexStr;
    if (!cleanHex.empty() && cleanHex[0] == L'#') {
        cleanHex = cleanHex.substr(1);
    }
    if (cleanHex.length() < 6) return RGB(0x8C, 0xF2, 0x87);

    unsigned long colorVal = wcstoul(cleanHex.c_str(), NULL, 16);
    BYTE r = (colorVal >> 16) & 0xFF;
    BYTE g = (colorVal >> 8) & 0xFF;
    BYTE b = colorVal & 0xFF;
    return RGB(r, g, b);
}

// 從 config.ini 讀取設定
void LoadConfig() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);
    PathRemoveFileSpec(exePath);

    std::wstring iniPath = std::wstring(exePath) + L"\\config.ini";

    wchar_t colorBuf[32] = { 0 };
    GetPrivateProfileString(L"Settings", L"BorderColor", L"8cf287", colorBuf, 32, iniPath.c_str());
    BorderColor = HexToCOLORREF(colorBuf);

    BorderThickness = GetPrivateProfileInt(L"Settings", L"BorderThickness", 3, iniPath.c_str());
    CornerRadius    = GetPrivateProfileInt(L"Settings", L"CornerRadius", 10, iniPath.c_str());
    
    // 讀取不透明度 (限制在 10 ~ 255 避免視窗完全隱形無法找回)
    TargetOpacity = GetPrivateProfileInt(L"Settings", L"TargetOpacity", 200, iniPath.c_str());
    if (TargetOpacity < 10) TargetOpacity = 10;
    if (TargetOpacity > 255) TargetOpacity = 255;

    OffsetX = GetPrivateProfileInt(L"Settings", L"OffsetX", 0, iniPath.c_str());
    OffsetY = GetPrivateProfileInt(L"Settings", L"OffsetY", 0, iniPath.c_str());
    OffsetW = GetPrivateProfileInt(L"Settings", L"OffsetW", 0, iniPath.c_str());
    OffsetH = GetPrivateProfileInt(L"Settings", L"OffsetH", 0, iniPath.c_str());
}

// 掃描所有可見視窗的回呼函數
BOOL CALLBACK PopulateMenuEnumProc(HWND hwnd, LPARAM lParam) {
    if (IsValidWindow(hwnd) && IsWindowVisible(hwnd)) {
        int length = GetWindowTextLength(hwnd);
        if (length > 0 || ActiveToggles.find(hwnd) != ActiveToggles.end()) {
            std::vector<HWND>* map = (std::vector<HWND>*)lParam;
            map->push_back(hwnd);
        }
    }
    return TRUE;
}

// 計時器：20ms 同步一次邊框位置
void CALLBACK SyncTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
    HWND targetHwnd = (HWND)idEvent;
    if (ActiveToggles.find(targetHwnd) != ActiveToggles.end()) {
        SyncBorderPosition(targetHwnd, ActiveToggles[targetHwnd].borderHwnd);
    }
}

// 邊框視窗處理函數：強制回傳 HTTRANSPARENT 確保滑鼠 100% 穿透邊框
LRESULT CALLBACK BorderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// 控制視窗處理函數 (全域熱鍵與系統匣選單)
LRESULT CALLBACK ControllerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_HOTKEY:
        if (wParam == 101) ToggleOverlay();     // Alt + U
        if (wParam == 102) ClearAllOverlays();  // Alt + Shift + U
        break;

    case WM_USER + 1: // 點擊系統匣圖示
        if (lParam == WM_RBUTTONUP) {
            POINT pt; GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();

            // 1. 掃描目前所有可見視窗
            g_MenuWindowMap.clear();
            EnumWindows(PopulateMenuEnumProc, (LPARAM)&g_MenuWindowMap);

            AppendMenu(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"--- 視窗清單 (點擊切換釘選) ---");

            if (g_MenuWindowMap.empty()) {
                AppendMenu(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"  (無可用視窗)");
            } else {
                int itemIdx = 0;
                for (HWND targetHwnd : g_MenuWindowMap) {
                    wchar_t titleBuf[256] = { 0 };
                    GetWindowText(targetHwnd, titleBuf, 256);

                    std::wstring displayName = titleBuf;
                    if (displayName.empty()) displayName = L"Untitled Window";
                    if (displayName.length() > 35) displayName = displayName.substr(0, 32) + L"...";

                    UINT flags = MF_STRING;
                    if (ActiveToggles.find(targetHwnd) != ActiveToggles.end()) {
                        flags |= MF_CHECKED; // 打勾標示已釘選
                    }

                    AppendMenu(hMenu, flags, ID_TRAY_PINNED_BASE + itemIdx, displayName.c_str());
                    itemIdx++;
                }
            }

            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            
            // 2. 功能按鈕
            AppendMenu(hMenu, MF_STRING, ID_TRAY_RELOAD_CONFIG, L"重新載入設定 (Reload Config)");
            
            UINT releaseFlags = MF_STRING | (ActiveToggles.empty() ? (MF_DISABLED | MF_GRAYED) : 0);
            AppendMenu(hMenu, releaseFlags, ID_TRAY_RELEASE_ALL, L"解除所有釘選 (Release All)");

            AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出 (Exit)");

            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        break;

    case WM_COMMAND: {
        WORD cmdId = LOWORD(wParam);

        if (cmdId == ID_TRAY_EXIT) {
            ClearAllOverlays();
            PostQuitMessage(0);
        } 
        else if (cmdId == ID_TRAY_RELEASE_ALL) {
            ClearAllOverlays();
        } 
        else if (cmdId == ID_TRAY_RELOAD_CONFIG) {
            LoadConfig();
            // 熱載入：更新所有已釘選視窗的不透明度
            for (auto const& [targetHwnd, info] : ActiveToggles) {
                SetLayeredWindowAttributes(targetHwnd, 0, (BYTE)TargetOpacity, LWA_ALPHA);
            }
        }
        else if (cmdId >= ID_TRAY_PINNED_BASE && cmdId < ID_TRAY_PINNED_BASE + (WORD)g_MenuWindowMap.size()) {
            int targetIdx = cmdId - ID_TRAY_PINNED_BASE;
            HWND targetHwnd = g_MenuWindowMap[targetIdx];

            if (IsWindow(targetHwnd)) {
                if (ActiveToggles.find(targetHwnd) != ActiveToggles.end()) {
                    UnpinWindow(targetHwnd);
                } else {
                    PinWindow(targetHwnd);
                }
            }
        }
        break;
    }

    case WM_DESTROY:
        ClearAllOverlays();
        PostQuitMessage(0);
        break;

    default: return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// RAII 自動清理資源結構
struct ScopedCleanup {
    ~ScopedCleanup() {
        ClearAllOverlays();
        if (g_ControllerHwnd) {
            UnregisterHotKey(g_ControllerHwnd, 101);
            UnregisterHotKey(g_ControllerHwnd, 102);
        }
        Shell_NotifyIcon(NIM_DELETE, &nid);
    }
};

// 啟動時自動還原之前已釘選的視窗
BOOL CALLBACK EnumWindowsRestoreProc(HWND hwnd, LPARAM lParam) {
    if (!IsValidWindow(hwnd) || !IsWindowVisible(hwnd)) return TRUE;

    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOPMOST) && (exStyle & WS_EX_TRANSPARENT)) {
        PinWindow(hwnd);
    }
    return TRUE;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_hInstance = hInstance;

    // 讀取 config.ini
    LoadConfig();

    WNDCLASSEX wc = { sizeof(WNDCLASSEX), 0, ControllerWndProc, 0, 0, hInstance, NULL, NULL, NULL, NULL, L"AHKControllerClass", NULL };
    RegisterClassEx(&wc);

    WNDCLASSEX bwc = { sizeof(WNDCLASSEX), 0, BorderWndProc, 0, 0, hInstance, NULL, NULL, CreateSolidBrush(BorderColor), NULL, L"AHKBorderClass", NULL };
    RegisterClassEx(&bwc);

    g_ControllerHwnd = CreateWindowEx(0, L"AHKControllerClass", L"Controller", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    nid.cbSize = sizeof(NOTIFYICONDATA);
    nid.hWnd = g_ControllerHwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_USER + 1;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"Trans Overlay Tool (Alt+U / Alt+Shift+U)");
    Shell_NotifyIcon(NIM_ADD, &nid);

    ScopedCleanup cleanupGuard;

    if (!RegisterHotKey(g_ControllerHwnd, 101, MOD_ALT, 'U')) {
        MessageBox(NULL, L"Alt + U 快捷鍵已被其他程式占用。\n程式即將退出...", L"快捷鍵衝突", MB_ICONWARNING);
        return 1;
    }

    if (!RegisterHotKey(g_ControllerHwnd, 102, MOD_ALT | MOD_SHIFT, 'U')) {
        MessageBox(NULL, L"Alt + Shift + U 快捷鍵已被其他程式占用。\n程式即將退出...", L"快捷鍵衝突", MB_ICONWARNING);
        return 1;
    }

    EnumWindows(EnumWindowsRestoreProc, 0);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}

void ToggleOverlay() {
    POINT pt; GetCursorPos(&pt);
    HWND targetHwnd = NULL;

    // 1. 優先判斷當前焦點視窗
    HWND activeHwnd = GetForegroundWindow();
    if (activeHwnd && IsValidWindow(activeHwnd)) {
        targetHwnd = activeHwnd;
    }

    // 2. 判斷游標是否懸停在已釘選視窗上
    if (!targetHwnd) { 
        for (HWND hwnd = GetTopWindow(NULL); hwnd != NULL; hwnd = GetWindow(hwnd, GW_HWNDNEXT)) {
            if (ActiveToggles.find(hwnd) != ActiveToggles.end()) {
                if (!IsIconic(hwnd)) {
                    RECT r;
                    if (GetVisibleWinPos(hwnd, &r) && pt.x >= r.left && pt.x <= r.right && pt.y >= r.top && pt.y <= r.bottom) {
                        targetHwnd = hwnd;
                        break;
                    }
                }
            }
        }
    }

    // 3. 判斷游標正下方的視窗
    if (!targetHwnd) {  
        HWND mouseHwnd = WindowFromPoint(pt);
        if (mouseHwnd && IsValidWindow(mouseHwnd)) {
            targetHwnd = GetAncestor(mouseHwnd, GA_ROOT);
        } 
    }

    if (!targetHwnd || !IsValidWindow(targetHwnd)) return;

    if (ActiveToggles.find(targetHwnd) != ActiveToggles.end()) {
        UnpinWindow(targetHwnd);
    } else {
        PinWindow(targetHwnd);
    }
}

bool IsValidWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t className[256];
    GetClassName(hwnd, className, 256);
    std::wstring cls(className);

    // 過濾桌面、工作列以及系統盤溢位選單視窗
    if (cls == L"WorkerW" || cls == L"Progman" || cls == L"Shell_TrayWnd" || 
        cls == L"NotifyIconOverflowWindow" || cls == L"TopLevelWindowForOverflowTray" ||
        cls == L"AHKBorderClass")
        return false;
    return true;
}

void PinWindow(HWND targetHwnd) {
    // 1. 設定最上層
    SetWindowPos(targetHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    // 2. 賦予目標視窗滑鼠穿透 (WS_EX_TRANSPARENT) 與分層 (WS_EX_LAYERED) 樣式
    LONG_PTR exStyle = GetWindowLongPtr(targetHwnd, GWL_EXSTYLE);
    SetWindowLongPtr(targetHwnd, GWL_EXSTYLE, exStyle | WS_EX_TRANSPARENT | WS_EX_LAYERED);

    // 3. 設定目標視窗的不透明度 (TargetOpacity: 0~255)
    SetLayeredWindowAttributes(targetHwnd, 0, (BYTE)TargetOpacity, LWA_ALPHA);

    // 4. 強制刷新 DWM 命中測試快取
    SetWindowPos(targetHwnd, NULL, 0, 0, 0, 0, 
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    // 5. 建立透明邊框視窗
    HWND borderHwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        L"AHKBorderClass", L"", WS_POPUP | WS_DISABLED, 0, 0, 0, 0, NULL, NULL, g_hInstance, NULL
    );

    // 邊框本身保持 100% 不透明 (255)
    SetLayeredWindowAttributes(borderHwnd, 0, (BYTE)TargetOpacity, LWA_ALPHA);

    UINT_PTR timerId = SetTimer(NULL, (UINT_PTR)targetHwnd, 20, SyncTimerProc);

    ActiveToggles[targetHwnd] = { borderHwnd, timerId };
    
    SyncBorderPosition(targetHwnd, borderHwnd);
    ShowWindow(borderHwnd, SW_SHOWNOACTIVATE);
}

void UnpinWindow(HWND targetHwnd) {
    if (ActiveToggles.find(targetHwnd) == ActiveToggles.end()) return;

    OverlayInfo info = ActiveToggles[targetHwnd];
    
    KillTimer(NULL, info.timerId);
    if (IsWindow(info.borderHwnd)) DestroyWindow(info.borderHwnd);

    // 1. 取消最上層
    SetWindowPos(targetHwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    // 2. 恢復 100% 不透明度 (255)
    SetLayeredWindowAttributes(targetHwnd, 0, 255, LWA_ALPHA);

    // 3. 移除穿透與分層樣式
    LONG_PTR exStyle = GetWindowLongPtr(targetHwnd, GWL_EXSTYLE);
    SetWindowLongPtr(targetHwnd, GWL_EXSTYLE, exStyle & ~(WS_EX_TRANSPARENT | WS_EX_LAYERED));

    // 4. 強制刷新 DWM 命中測試快取
    SetWindowPos(targetHwnd, NULL, 0, 0, 0, 0, 
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    ActiveToggles.erase(targetHwnd);
}

void ClearAllOverlays() {
    std::vector<HWND> hwnds;
    for (auto const& [hwnd, info] : ActiveToggles) hwnds.push_back(hwnd);
    for (HWND h : hwnds) UnpinWindow(h);
}

bool GetVisibleWinPos(HWND hwnd, RECT* outRect) {
    RECT rect;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, 9, &rect, sizeof(RECT)))) {
        *outRect = rect;
        return true;
    }
    return GetWindowRect(hwnd, outRect);
}

void SyncBorderPosition(HWND targetHwnd, HWND borderHwnd) {
    if (!IsWindow(targetHwnd)) {
        UnpinWindow(targetHwnd);
        return;
    }

    if (IsIconic(targetHwnd)) {
        ShowWindow(borderHwnd, SW_HIDE);
        return;
    }

    RECT r;
    if (!GetVisibleWinPos(targetHwnd, &r)) return;

    int X = r.left + OffsetX;
    int Y = r.top + OffsetY;
    int W = (r.right - r.left) + OffsetW;
    int H = (r.bottom - r.top) + OffsetH;

    if (W <= 0 || H <= 0) return;

    int b = BorderThickness;
    int outX = X - b;
    int outY = Y - b;
    int outW = W + (b * 2);
    int outH = H + (b * 2);

    SetWindowPos(borderHwnd, HWND_TOPMOST, outX, outY, outW, outH, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    int corner = CornerRadius;
    HRGN outerRgn = CreateRoundRectRgn(0, 0, outW + 1, outH + 1, corner * 2, corner * 2);
    int innerCorner = (std::max)(1, corner - b);
    HRGN innerRgn = CreateRoundRectRgn(b, b, outW - b + 1, outH - b + 1, innerCorner * 2, innerCorner * 2);

    CombineRgn(outerRgn, outerRgn, innerRgn, RGN_DIFF);
    SetWindowRgn(borderHwnd, outerRgn, TRUE);
    DeleteObject(innerRgn);
}
