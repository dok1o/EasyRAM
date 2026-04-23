#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <iomanip>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ─── IDs ───────────────────────────────────────────────
#define ID_TAB          100
#define ID_LISTVIEW     101
#define ID_TIMER        102
#define ID_BTN_APPLY    110
#define ID_BTN_RESET    111
#define ID_EDIT_LIMIT   120
#define ID_EDIT_DOTA    121
#define ID_EDIT_CS      122
#define ID_EDIT_STEAM   123
#define ID_STATIC_STEAM 130
#define ID_STATUSBAR    140

// ─── Globals ───────────────────────────────────────────
HWND hMain, hTab, hListView, hStatusBar;
HWND hEditLimit, hEditDota, hEditCS, hEditSteam;
HWND hBtnApply, hBtnReset;

// panels (one per tab)
HWND hPanelDota, hPanelCS, hPanelMonitor, hPanelSettings;

int  gLimitMB  = 512;
int  gDotaMB   = 4096;
int  gCSMB     = 4096;
std::string gSteamPath;

// ─── Structs ───────────────────────────────────────────
struct ProcessInfo {
    std::wstring name;
    DWORD        pid;
    SIZE_T       memory;
};

// ═══════════════════════════════════════════════════════
// UTILITIES
// ═══════════════════════════════════════════════════════

std::string GetSteamPath() {
    HKEY hKey;
    char value[512];
    DWORD size = sizeof(value);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hKey, "SteamPath", NULL, NULL,
                             (LPBYTE)value, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::string(value);
        }
        RegCloseKey(hKey);
    }
    return "";
}

std::vector<ProcessInfo> GetProcesses() {
    std::vector<ProcessInfo> list;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);
    if (Process32First(snap, &pe)) {
        do {
            HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                    FALSE, pe.th32ProcessID);
            if (hp) {
                PROCESS_MEMORY_COUNTERS pmc;
                if (GetProcessMemoryInfo(hp, &pmc, sizeof(pmc)))
                    list.push_back({pe.szExeFile, pe.th32ProcessID, pmc.WorkingSetSize});
                CloseHandle(hp);
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    std::sort(list.begin(), list.end(),
              [](const ProcessInfo& a, const ProcessInfo& b){ return a.memory > b.memory; });
    return list;
}

bool PatchLaunchOptions(const std::string& steamPath, const std::string& appID, int memMB) {
    std::string path = steamPath + "\\userdata\\config\\localconfig.vdf";
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    in.close();

    size_t pos = content.find(appID);
    if (pos == std::string::npos) return false;

    std::string newOpt = "\"LaunchOptions\" \"-mem " + std::to_string(memMB) + "\"";
    size_t launchPos   = content.find("LaunchOptions", pos);
    if (launchPos != std::string::npos) {
        size_t end = content.find("\n", launchPos);
        content.replace(launchPos, end - launchPos, newOpt);
    } else {
        content.insert(pos + appID.size(), "\n\t\t\t\t" + newOpt);
    }

    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << content;
    return true;
}

void SetStatus(const wchar_t* msg) {
    if (hStatusBar) SendMessageW(hStatusBar, SB_SETTEXTW, 0, (LPARAM)msg);
}

int GetEditInt(HWND hEdit, int fallback) {
    wchar_t buf[32];
    GetWindowTextW(hEdit, buf, 32);
    int v = _wtoi(buf);
    return (v > 0) ? v : fallback;
}

// ═══════════════════════════════════════════════════════
// LIST VIEW  (Monitor tab)
// ═══════════════════════════════════════════════════════

void InitListView(HWND hLV) {
    ListView_SetExtendedListViewStyle(hLV,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;

    col.fmt = LVCFMT_LEFT;
    col.cx  = 220; col.pszText = (LPWSTR)L"Process";
    ListView_InsertColumn(hLV, 0, &col);

    col.cx  = 80;  col.pszText = (LPWSTR)L"PID";
    ListView_InsertColumn(hLV, 1, &col);

    col.fmt = LVCFMT_RIGHT;
    col.cx  = 110; col.pszText = (LPWSTR)L"RAM (MB)";
    ListView_InsertColumn(hLV, 2, &col);

    col.cx  = 100; col.pszText = (LPWSTR)L"Status";
    ListView_InsertColumn(hLV, 3, &col);
}

void RefreshListView() {
    auto procs = GetProcesses();
    ListView_DeleteAllItems(hListView);

    int idx = 0;
    for (auto& p : procs) {
        double mb = p.memory / (1024.0 * 1024.0);
        bool warn = (p.name != L"dota2.exe" && p.name != L"cs2.exe" && mb > gLimitMB);

        LVITEMW item = {};
        item.mask    = LVIF_TEXT;
        item.iItem   = idx;

        item.pszText = (LPWSTR)p.name.c_str();
        ListView_InsertItem(hListView, &item);

        std::wstring pidStr = std::to_wstring(p.pid);
        ListView_SetItemText(hListView, idx, 1, (LPWSTR)pidStr.c_str());

        std::wostringstream oss;
        oss << std::fixed << std::setprecision(1) << mb;
        std::wstring mbStr = oss.str();
        ListView_SetItemText(hListView, idx, 2, (LPWSTR)mbStr.c_str());

        std::wstring status = warn ? L"⚠ High RAM" : L"OK";
        ListView_SetItemText(hListView, idx, 3, (LPWSTR)status.c_str());

        idx++;
        if (idx >= 40) break;
    }

    SetStatus(L"Monitoring active  ·  Auto-refresh every 2 s");
}

// ═══════════════════════════════════════════════════════
// TAB SWITCHING
// ═══════════════════════════════════════════════════════

void ShowTab(int i) {
    ShowWindow(hPanelDota,     i == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(hPanelCS,       i == 1 ? SW_SHOW : SW_HIDE);
    ShowWindow(hPanelMonitor,  i == 2 ? SW_SHOW : SW_HIDE);
    ShowWindow(hPanelSettings, i == 3 ? SW_SHOW : SW_HIDE);
}

// ═══════════════════════════════════════════════════════
// PANEL BUILDER HELPERS
// ═══════════════════════════════════════════════════════

HWND MakeStatic(HWND parent, const wchar_t* text, int x, int y, int w, int h, DWORD extra = 0) {
    return CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFT | extra,
        x, y, w, h, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
}

HWND MakeEdit(HWND parent, const wchar_t* text, int x, int y, int w, int id) {
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        x, y, w, 24, parent, (HMENU)(intptr_t)id,
        GetModuleHandleW(nullptr), nullptr);
    return h;
}

HWND MakeButton(HWND parent, const wchar_t* text, int x, int y, int w, int id) {
    return CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x, y, w, 28, parent, (HMENU)(intptr_t)id,
        GetModuleHandleW(nullptr), nullptr);
}

void SetFont(HWND h, HFONT f) { SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); }

// ═══════════════════════════════════════════════════════
// BUILD PANELS
// ═══════════════════════════════════════════════════════

HWND MakePanel(HWND parent) {
    return CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | SS_LEFT,
        0, 0, 0, 0, parent, nullptr,
        GetModuleHandleW(nullptr), nullptr);
}

void BuildPanels(HWND parent, HFONT hFont, HFONT hBold, RECT clientArea) {
    int px = clientArea.left + 14;
    int py = clientArea.top  + 14;
    int pw = clientArea.right  - clientArea.left - 28;
    int ph = clientArea.bottom - clientArea.top  - 14;

    // ── DOTA 2 PANEL ──────────────────────────────────
    hPanelDota = MakePanel(parent);
    SetWindowPos(hPanelDota, nullptr, px, py, pw, ph, SWP_NOZORDER);

    {
        HWND h;
        h = MakeStatic(hPanelDota, L"Game configuration — Dota 2", 0, 0, 400, 20, SS_SUNKEN);
        SetFont(h, hBold);

        MakeStatic(hPanelDota, L"RAM limit for all other apps (MB):", 0, 36, 260, 20);
        hEditLimit = MakeEdit(hPanelDota, L"512", 270, 34, 100, ID_EDIT_LIMIT);
        SetFont(hEditLimit, hFont);

        MakeStatic(hPanelDota, L"Dota 2 RAM target (MB):", 0, 72, 260, 20);
        hEditDota = MakeEdit(hPanelDota, L"4096", 270, 70, 100, ID_EDIT_DOTA);
        SetFont(hEditDota, hFont);

        MakeStatic(hPanelDota, L"Steam path (auto-detected):", 0, 108, 260, 20);
        std::wstring sp(gSteamPath.begin(), gSteamPath.end());
        hEditSteam = MakeEdit(hPanelDota, sp.c_str(), 270, 106, pw - 270, ID_EDIT_STEAM);
        SetFont(hEditSteam, hFont);
        EnableWindow(hEditSteam, FALSE);

        hBtnApply = MakeButton(hPanelDota, L"Apply & Start", 0, 150, 130, ID_BTN_APPLY);
        SetFont(hBtnApply, hFont);
        hBtnReset = MakeButton(hPanelDota, L"Reset Defaults", 140, 150, 130, ID_BTN_RESET);
        SetFont(hBtnReset, hFont);

        h = MakeStatic(hPanelDota,
            L"Applies the -mem launch option to Dota 2 in your Steam config,\r\n"
            L"then opens the process monitor.",
            0, 194, pw, 40);
        SetFont(h, hFont);
    }

    // ── CS2 PANEL ─────────────────────────────────────
    hPanelCS = MakePanel(parent);
    SetWindowPos(hPanelCS, nullptr, px, py, pw, ph, SWP_NOZORDER);

    {
        HWND h;
        h = MakeStatic(hPanelCS, L"Game configuration — CS2", 0, 0, 400, 20, SS_SUNKEN);
        SetFont(h, hBold);

        MakeStatic(hPanelCS, L"RAM limit for all other apps (MB):", 0, 36, 260, 20);
        HWND hEL2 = MakeEdit(hPanelCS, L"512", 270, 34, 100, ID_EDIT_LIMIT);
        SetFont(hEL2, hFont);

        MakeStatic(hPanelCS, L"CS2 RAM target (MB):", 0, 72, 260, 20);
        hEditCS = MakeEdit(hPanelCS, L"4096", 270, 70, 100, ID_EDIT_CS);
        SetFont(hEditCS, hFont);

        MakeStatic(hPanelCS, L"Steam path (auto-detected):", 0, 108, 260, 20);
        std::wstring sp(gSteamPath.begin(), gSteamPath.end());
        HWND hES2 = MakeEdit(hPanelCS, sp.c_str(), 270, 106, pw - 270, ID_EDIT_STEAM);
        SetFont(hES2, hFont);
        EnableWindow(hES2, FALSE);

        HWND hBA2 = MakeButton(hPanelCS, L"Apply & Start", 0, 150, 130, ID_BTN_APPLY);
        SetFont(hBA2, hFont);
        HWND hBR2 = MakeButton(hPanelCS, L"Reset Defaults", 140, 150, 130, ID_BTN_RESET);
        SetFont(hBR2, hFont);

        h = MakeStatic(hPanelCS,
            L"Applies the -mem launch option to CS2 in your Steam config,\r\n"
            L"then opens the process monitor.",
            0, 194, pw, 40);
        SetFont(h, hFont);
    }

    // ── MONITOR PANEL ─────────────────────────────────
    hPanelMonitor = MakePanel(parent);
    SetWindowPos(hPanelMonitor, nullptr, px, py, pw, ph, SWP_NOZORDER);

    hListView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, pw, ph - 10,
        hPanelMonitor, (HMENU)ID_LISTVIEW,
        GetModuleHandleW(nullptr), nullptr);
    InitListView(hListView);

    // ── SETTINGS PANEL ────────────────────────────────
    hPanelSettings = MakePanel(parent);
    SetWindowPos(hPanelSettings, nullptr, px, py, pw, ph, SWP_NOZORDER);

    {
        HWND h;
        h = MakeStatic(hPanelSettings, L"Settings", 0, 0, 300, 20, SS_SUNKEN);
        SetFont(h, hBold);

        h = MakeStatic(hPanelSettings,
            L"EasyRam  v1.0.0\r\n\r\n"
            L"Steam path is detected automatically from the Windows registry.\r\n"
            L"Launch options are written to:\r\n"
            L"  Steam\\userdata\\config\\localconfig.vdf\r\n\r\n"
            L"Process monitor refreshes every 2 seconds.\r\n"
            L"Warnings appear for processes exceeding your RAM limit\r\n"
            L"(Dota 2 and CS2 are always excluded from warnings).",
            0, 36, pw, 200);
        SetFont(h, hFont);
    }
}

// ═══════════════════════════════════════════════════════
// APPLY HANDLER
// ═══════════════════════════════════════════════════════

void OnApply(int tabIndex) {
    gLimitMB = GetEditInt(hEditLimit, 512);

    if (tabIndex == 0) {
        gDotaMB = GetEditInt(hEditDota, 4096);
        if (gSteamPath.empty()) {
            MessageBoxW(hMain, L"Steam not found.", L"EasyRam", MB_ICONWARNING);
            return;
        }
        bool ok = PatchLaunchOptions(gSteamPath, "570", gDotaMB);
        SetStatus(ok ? L"Dota 2 launch options applied." : L"Could not write Steam config.");
    } else {
        gCSMB = GetEditInt(hEditCS, 4096);
        if (gSteamPath.empty()) {
            MessageBoxW(hMain, L"Steam not found.", L"EasyRam", MB_ICONWARNING);
            return;
        }
        bool ok = PatchLaunchOptions(gSteamPath, "730", gCSMB);
        SetStatus(ok ? L"CS2 launch options applied." : L"Could not write Steam config.");
    }

    // switch to Monitor tab
    TabCtrl_SetCurSel(hTab, 2);
    ShowTab(2);
    RefreshListView();
}

// ═══════════════════════════════════════════════════════
// WINDOW PROCEDURE
// ═══════════════════════════════════════════════════════

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static int currentTab = 0;

    switch (msg) {
    case WM_CREATE: {
        // fonts
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        LOGFONTW lf = {};
        GetObjectW(hFont, sizeof(lf), &lf);
        lf.lfWeight = FW_BOLD;
        HFONT hBold = CreateFontIndirectW(&lf);

        // status bar
        hStatusBar = CreateWindowExW(0, STATUSCLASSNAMEW, L"Ready",
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hWnd, (HMENU)ID_STATUSBAR,
            GetModuleHandleW(nullptr), nullptr);

        // tab control
        hTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | TCS_FIXEDWIDTH,
            0, 0, 0, 0, hWnd, (HMENU)ID_TAB,
            GetModuleHandleW(nullptr), nullptr);
        SetFont(hTab, hFont);

        TCITEMW ti = {};
        ti.mask = TCIF_TEXT;
        ti.pszText = (LPWSTR)L"Dota 2";   TabCtrl_InsertItem(hTab, 0, &ti);
        ti.pszText = (LPWSTR)L"CS2";       TabCtrl_InsertItem(hTab, 1, &ti);
        ti.pszText = (LPWSTR)L"Monitor";   TabCtrl_InsertItem(hTab, 2, &ti);
        ti.pszText = (LPWSTR)L"Settings";  TabCtrl_InsertItem(hTab, 3, &ti);

        // client area for panels (below tab header)
        RECT rc; GetClientRect(hWnd, &rc);
        RECT tabArea = {0, 0, rc.right, rc.bottom - 22};
        TabCtrl_AdjustRect(hTab, FALSE, &tabArea);

        gSteamPath = GetSteamPath();
        BuildPanels(hWnd, hFont, hBold, tabArea);
        ShowTab(0);

        SetTimer(hWnd, ID_TIMER, 2000, nullptr);

        if (gSteamPath.empty())
            SetStatus(L"Steam not found — check your installation.");
        else
            SetStatus(L"Steam detected. Configure a game and click Apply & Start.");

        return 0;
    }

    case WM_SIZE: {
        int W = LOWORD(lParam), H = HIWORD(lParam);
        SendMessageW(hStatusBar, WM_SIZE, 0, 0);

        RECT sbrc; GetWindowRect(hStatusBar, &sbrc);
        int sbH = sbrc.bottom - sbrc.top;

        SetWindowPos(hTab, nullptr, 0, 0, W, H - sbH, SWP_NOZORDER);

        RECT tabArea = {0, 0, W, H - sbH};
        TabCtrl_AdjustRect(hTab, FALSE, &tabArea);
        int px = tabArea.left  + 14;
        int py = tabArea.top   + 14;
        int pw = tabArea.right  - tabArea.left - 28;
        int ph = tabArea.bottom - tabArea.top  - 14;

        for (HWND panel : {hPanelDota, hPanelCS, hPanelMonitor, hPanelSettings})
            SetWindowPos(panel, nullptr, px, py, pw, ph, SWP_NOZORDER);

        if (hListView)
            SetWindowPos(hListView, nullptr, 0, 0, pw, ph - 10, SWP_NOZORDER);

        return 0;
    }

    case WM_TIMER:
        if (wParam == ID_TIMER && IsWindowVisible(hPanelMonitor))
            RefreshListView();
        return 0;

    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lParam;
        if (nm->idFrom == ID_TAB && nm->code == TCN_SELCHANGE) {
            currentTab = TabCtrl_GetCurSel(hTab);
            ShowTab(currentTab);
            if (currentTab == 2) RefreshListView();
        }
        return 0;
    }

    case WM_COMMAND: {
        int id  = LOWORD(wParam);
        int evt = HIWORD(wParam);
        if (id == ID_BTN_APPLY && evt == BN_CLICKED)
            OnApply(currentTab);
        if (id == ID_BTN_RESET && evt == BN_CLICKED) {
            SetWindowTextW(hEditLimit, L"512");
            SetWindowTextW(hEditDota,  L"4096");
            if (hEditCS) SetWindowTextW(hEditCS, L"4096");
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hWnd, ID_TIMER);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ═══════════════════════════════════════════════════════
// ENTRY POINT
// ═══════════════════════════════════════════════════════

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    InitCommonControls();

    WNDCLASSEXW wc  = {};
    wc.cbSize       = sizeof(wc);
    wc.lpfnWndProc  = WndProc;
    wc.hInstance    = hInst;
    wc.hCursor      = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground= (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName= L"EasyRamWnd";
    wc.hIcon        = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    hMain = CreateWindowExW(0, L"EasyRamWnd", L"EasyRam  —  RAM Monitor & Game Optimizer",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 660, 460,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(hMain, nCmdShow);
    UpdateWindow(hMain);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
