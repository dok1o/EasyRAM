#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <iomanip>

// ── ImGui (single-header amalgamation assumed in /imgui/) ──
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_win32.h"
#include "imgui/backends/imgui_impl_dx11.h"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// ─────────────────────────────────────────────────────────
// Data
// ─────────────────────────────────────────────────────────
struct ProcessEntry {
    std::string name;
    DWORD       pid;
    double      ramMB;
    bool        highRam;
};

static ID3D11Device*           g_pd3dDevice           = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext     = nullptr;
static IDXGISwapChain*         g_pSwapChain            = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView  = nullptr;
static HWND                    g_hwnd                  = nullptr;

// ─────────────────────────────────────────────────────────
// D3D helpers
// ─────────────────────────────────────────────────────────
void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = 0;
    sd.BufferDesc.Height                  = 0;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hWnd;
    sd.SampleDesc.Count                   = 1;
    sd.SampleDesc.Quality                 = 0;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        featureLevelArray, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice,
        &featureLevel, &g_pd3dDeviceContext);

    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            featureLevelArray, 2, D3D11_SDK_VERSION,
            &sd, &g_pSwapChain, &g_pd3dDevice,
            &featureLevel, &g_pd3dDeviceContext);

    if (res != S_OK) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ─────────────────────────────────────────────────────────
// App logic
// ─────────────────────────────────────────────────────────
std::string GetSteamPath() {
    HKEY hKey;
    char value[512];
    DWORD size = sizeof(value);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hKey, "SteamPath", nullptr, nullptr,
                             (LPBYTE)value, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::string(value);
        }
        RegCloseKey(hKey);
    }
    return "";
}

bool PatchLaunchOptions(const std::string& steamPath,
                        const std::string& appId, int memMB) {
    std::string path = steamPath + "\\userdata\\config\\localconfig.vdf";
    std::ifstream in(path);
    if (!in.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    in.close();

    size_t pos = content.find(appId);
    if (pos == std::string::npos) return false;

    std::string newOpt = "\"LaunchOptions\" \"-mem " + std::to_string(memMB) + "\"";
    size_t launchPos   = content.find("LaunchOptions", pos);
    if (launchPos != std::string::npos) {
        size_t end = content.find("\n", launchPos);
        content.replace(launchPos, end - launchPos, newOpt);
    } else {
        content.insert(pos + appId.size(), "\n\t\t\t\t" + newOpt);
    }

    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << content;
    return true;
}

std::vector<ProcessEntry> GetProcesses(int limitMB) {
    std::vector<ProcessEntry> list;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);
    if (Process32First(snap, &pe)) {
        do {
            HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                    FALSE, pe.th32ProcessID);
            if (hp) {
                PROCESS_MEMORY_COUNTERS pmc;
                if (GetProcessMemoryInfo(hp, &pmc, sizeof(pmc))) {
                    double mb   = pmc.WorkingSetSize / (1024.0 * 1024.0);
                    int nmLen = WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, nullptr, 0, nullptr, nullptr);
                    std::string nm;
                    if (nmLen > 1) {
                        nm.resize(nmLen - 1);
                        WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, &nm[0], nmLen - 1, nullptr, nullptr);
                    }
                    bool highRam = (nm != "dota2.exe" && nm != "cs2.exe" && mb > limitMB);
                    list.push_back({nm, pe.th32ProcessID, mb, highRam});
                }
                CloseHandle(hp);
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    std::sort(list.begin(), list.end(),
              [](const ProcessEntry& a, const ProcessEntry& b){ return a.ramMB > b.ramMB; });
    return list;
}

// ─────────────────────────────────────────────────────────
// Custom dark style (Catppuccin-inspired)
// ─────────────────────────────────────────────────────────
void ApplyDarkStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 8.0f;
    s.FrameRounding     = 6.0f;
    s.GrabRounding      = 4.0f;
    s.ScrollbarRounding = 6.0f;
    s.TabRounding       = 6.0f;
    s.FramePadding      = {8, 5};
    s.ItemSpacing       = {8, 6};
    s.WindowPadding     = {16, 16};
    s.ScrollbarSize     = 12.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]             = {0.118f, 0.118f, 0.180f, 1.0f};
    c[ImGuiCol_ChildBg]              = {0.094f, 0.094f, 0.145f, 1.0f};
    c[ImGuiCol_PopupBg]              = {0.118f, 0.118f, 0.180f, 1.0f};
    c[ImGuiCol_Border]               = {0.271f, 0.278f, 0.322f, 1.0f};
    c[ImGuiCol_FrameBg]              = {0.192f, 0.196f, 0.267f, 1.0f};
    c[ImGuiCol_FrameBgHovered]       = {0.271f, 0.278f, 0.322f, 1.0f};
    c[ImGuiCol_FrameBgActive]        = {0.271f, 0.278f, 0.322f, 1.0f};
    c[ImGuiCol_TitleBg]              = {0.094f, 0.094f, 0.145f, 1.0f};
    c[ImGuiCol_TitleBgActive]        = {0.094f, 0.094f, 0.145f, 1.0f};
    c[ImGuiCol_ScrollbarBg]          = {0.094f, 0.094f, 0.145f, 1.0f};
    c[ImGuiCol_ScrollbarGrab]        = {0.271f, 0.278f, 0.322f, 1.0f};
    c[ImGuiCol_ScrollbarGrabHovered] = {0.361f, 0.369f, 0.431f, 1.0f};
    c[ImGuiCol_CheckMark]            = {0.792f, 0.651f, 0.969f, 1.0f};
    c[ImGuiCol_Button]               = {0.486f, 0.227f, 0.929f, 1.0f};
    c[ImGuiCol_ButtonHovered]        = {0.427f, 0.169f, 0.851f, 1.0f};
    c[ImGuiCol_ButtonActive]         = {0.357f, 0.129f, 0.714f, 1.0f};
    c[ImGuiCol_Header]               = {0.192f, 0.196f, 0.267f, 1.0f};
    c[ImGuiCol_HeaderHovered]        = {0.271f, 0.278f, 0.322f, 1.0f};
    c[ImGuiCol_HeaderActive]         = {0.271f, 0.278f, 0.322f, 1.0f};
    c[ImGuiCol_Tab]                  = {0.094f, 0.094f, 0.145f, 1.0f};
    c[ImGuiCol_TabHovered]           = {0.192f, 0.196f, 0.267f, 1.0f};
    c[ImGuiCol_TabActive]            = {0.192f, 0.196f, 0.267f, 1.0f};
    c[ImGuiCol_TabUnfocusedActive]   = {0.192f, 0.196f, 0.267f, 1.0f};
    c[ImGuiCol_Text]                 = {0.804f, 0.839f, 0.957f, 1.0f};
    c[ImGuiCol_TextDisabled]         = {0.345f, 0.357f, 0.439f, 1.0f};
    c[ImGuiCol_Separator]            = {0.192f, 0.196f, 0.267f, 1.0f};
    c[ImGuiCol_TableHeaderBg]        = {0.094f, 0.094f, 0.145f, 1.0f};
    c[ImGuiCol_TableBorderLight]     = {0.192f, 0.196f, 0.267f, 1.0f};
    c[ImGuiCol_TableRowBg]           = {0.000f, 0.000f, 0.000f, 0.0f};
    c[ImGuiCol_TableRowBgAlt]        = {1.000f, 1.000f, 1.000f, 0.03f};
}

// ─────────────────────────────────────────────────────────
// Config panel helper
// ─────────────────────────────────────────────────────────
static ImVec4 purple = {0.792f, 0.651f, 0.969f, 1.0f};
static ImVec4 green  = {0.651f, 0.890f, 0.631f, 1.0f};
static ImVec4 red    = {0.953f, 0.545f, 0.659f, 1.0f};
static ImVec4 muted  = {0.345f, 0.357f, 0.439f, 1.0f};
static ImVec4 text2  = {0.651f, 0.678f, 0.784f, 1.0f};

void DrawConfigPanel(const char* game, const char* appId,
                     char* limitBuf, char* ramBuf,
                     const std::string& steamPath,
                     std::string& statusMsg,
                     int& activeTab,
                     std::vector<ProcessEntry>& processes,
                     DWORD& lastRefresh, int& limitMB) {

    ImGui::TextColored(muted, "%s CONFIGURATION", game);
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextColored(text2, "RAM limit (all apps)");
    ImGui::SameLine(230);
    ImGui::SetNextItemWidth(120);
    char limitId[32]; sprintf_s(limitId, "##limit_%s", game);
    ImGui::InputText(limitId, limitBuf, 16, ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    ImGui::TextColored(purple, "MB");

    ImGui::Spacing();
    ImGui::TextColored(text2, "%s RAM target", game);
    ImGui::SameLine(230);
    ImGui::SetNextItemWidth(120);
    char ramId[32]; sprintf_s(ramId, "##ram_%s", game);
    ImGui::InputText(ramId, ramBuf, 16, ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    ImGui::TextColored(purple, "MB");

    ImGui::Spacing();
    ImGui::TextColored(text2, "Steam path");
    ImGui::SameLine(230);
    if (steamPath.empty())
        ImGui::TextColored(red, "Steam not found");
    else {
        ImGui::TextColored(green, "%s", steamPath.c_str());
        ImGui::SameLine();
        ImGui::TextColored(green, "[Auto]");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(muted,
        "Patches the -mem launch option in Steam config, then opens the monitor.");
    ImGui::Spacing();

    if (steamPath.empty()) ImGui::BeginDisabled();

    if (ImGui::Button("Apply & Start", {130, 32})) {
        limitMB    = atoi(limitBuf);
        int ramMB  = atoi(ramBuf);
        if (limitMB <= 0) limitMB = 512;
        if (ramMB   <= 0) ramMB   = 4096;

        bool ok = PatchLaunchOptions(steamPath, appId, ramMB);
        statusMsg   = ok
            ? std::string(game) + " launch options applied."
            : "Could not write Steam config — check permissions.";
        processes   = GetProcesses(limitMB);
        lastRefresh = GetTickCount();
        activeTab   = 2;
    }

    if (steamPath.empty()) ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Reset", {80, 32})) {
        strcpy_s(limitBuf, 16, "512");
        strcpy_s(ramBuf,   16, "4096");
    }
}

// ─────────────────────────────────────────────────────────
// WinMain
// ─────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    WNDCLASSEXW wc  = {};
    wc.cbSize       = sizeof(wc);
    wc.style        = CS_CLASSDC;
    wc.lpfnWndProc  = WndProc;
    wc.hInstance    = hInst;
    wc.hCursor      = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName= L"EasyRamImGui";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, L"EasyRamImGui",
        L"EasyRam \u2014 RAM Monitor & Game Optimizer",
        WS_OVERLAPPEDWINDOW, 100, 100, 720, 560,
        nullptr, nullptr, hInst, nullptr);

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInst);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyDarkStyle();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    std::string steamPath = GetSteamPath();
    std::string statusMsg = steamPath.empty()
        ? "Steam not found."
        : "Steam detected. Configure a game and click Apply & Start.";

    char dotaLimitBuf[16] = "512";
    char dotaRamBuf[16]   = "4096";
    char csLimitBuf[16]   = "512";
    char csRamBuf[16]     = "4096";

    int  activeTab  = 0;
    int  limitMB    = 512;
    bool running    = true;

    std::vector<ProcessEntry> processes;
    DWORD lastRefresh = 0;

    ImVec4 clearColor = {0.118f, 0.118f, 0.180f, 1.0f};

    MSG msg;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (activeTab == 2 && GetTickCount() - lastRefresh > 2000) {
            processes   = GetProcesses(limitMB);
            lastRefresh = GetTickCount();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RECT rc; GetClientRect(g_hwnd, &rc);
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({(float)rc.right, (float)rc.bottom});
        ImGui::Begin("##main", nullptr,
            ImGuiWindowFlags_NoTitleBar    | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove        | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Header
        ImGui::TextColored(purple, "  EasyRam");
        ImGui::SameLine();
        ImGui::TextColored(muted, "v1.0.0  \xe2\x80\x94  RAM Monitor & Game Optimizer");
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::BeginTabBar("MainTabs")) {

            // DOTA 2
            if (ImGui::BeginTabItem("  Dota 2  ")) {
                activeTab = 0;
                ImGui::Spacing();
                DrawConfigPanel("DOTA 2", "570",
                    dotaLimitBuf, dotaRamBuf,
                    steamPath, statusMsg, activeTab,
                    processes, lastRefresh, limitMB);
                ImGui::EndTabItem();
            }

            // CS2
            if (ImGui::BeginTabItem("  CS2  ")) {
                activeTab = 1;
                ImGui::Spacing();
                DrawConfigPanel("CS2", "730",
                    csLimitBuf, csRamBuf,
                    steamPath, statusMsg, activeTab,
                    processes, lastRefresh, limitMB);
                ImGui::EndTabItem();
            }

            // MONITOR
            if (ImGui::BeginTabItem("  Monitor  ")) {
                activeTab = 2;
                ImGui::Spacing();
                ImGui::TextColored(muted, "LIVE PROCESS MONITOR");
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90);
                if (ImGui::SmallButton("Refresh now")) {
                    processes   = GetProcesses(limitMB);
                    lastRefresh = GetTickCount();
                }
                ImGui::Separator();
                ImGui::Spacing();

                if (ImGui::BeginTable("procs", 4,
                    ImGuiTableFlags_Borders |
                    ImGuiTableFlags_RowBg   |
                    ImGuiTableFlags_ScrollY |
                    ImGuiTableFlags_SizingStretchProp,
                    {0, ImGui::GetContentRegionAvail().y - 30}))
                {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("Process",  ImGuiTableColumnFlags_WidthStretch, 3.0f);
                    ImGui::TableSetupColumn("PID",      ImGuiTableColumnFlags_WidthStretch, 1.0f);
                    ImGui::TableSetupColumn("RAM (MB)", ImGuiTableColumnFlags_WidthStretch, 1.5f);
                    ImGui::TableSetupColumn("Status",   ImGuiTableColumnFlags_WidthStretch, 1.5f);
                    ImGui::TableHeadersRow();

                    for (auto& p : processes) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextUnformatted(p.name.c_str());

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%lu", p.pid);

                        ImGui::TableSetColumnIndex(2);
                        std::ostringstream ss;
                        ss << std::fixed << std::setprecision(1) << p.ramMB;
                        if (p.highRam)
                            ImGui::TextColored(red, "%s", ss.str().c_str());
                        else if (p.name == "dota2.exe" || p.name == "cs2.exe")
                            ImGui::TextColored(purple, "%s", ss.str().c_str());
                        else
                            ImGui::TextUnformatted(ss.str().c_str());

                        ImGui::TableSetColumnIndex(3);
                        if (p.highRam)
                            ImGui::TextColored(red, "High RAM");
                        else if (p.name == "dota2.exe" || p.name == "cs2.exe")
                            ImGui::TextColored(purple, "Active");
                        else
                            ImGui::TextColored(green, "OK");
                    }
                    ImGui::EndTable();
                }
                ImGui::EndTabItem();
            }

            // SETTINGS
            if (ImGui::BeginTabItem("  Settings  ")) {
                activeTab = 3;
                ImGui::Spacing();
                ImGui::TextColored(muted, "ABOUT EASYRAM");
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextColored(purple, "EasyRam v1.0.0");
                ImGui::Spacing();
                ImGui::TextWrapped(
                    "Steam path is detected automatically from the Windows registry.\n\n"
                    "Launch options are written to:\n"
                    "  Steam\\userdata\\config\\localconfig.vdf\n\n"
                    "The process monitor refreshes every 2 seconds automatically.\n"
                    "Warnings appear for processes exceeding your RAM limit.\n"
                    "Dota 2 and CS2 processes are always excluded from warnings.");
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        // Status bar
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 28);
        ImGui::Separator();
        ImGui::TextColored(steamPath.empty() ? red : green,
            "  %s", steamPath.empty() ? "[No Steam]" : "[Steam OK]");
        ImGui::SameLine();
        ImGui::TextColored(muted, "%s", statusMsg.c_str());

        ImGui::End();

        ImGui::Render();
        const float cc[4] = {clearColor.x, clearColor.y, clearColor.z, clearColor.w};
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
