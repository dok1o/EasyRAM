#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <iomanip>
#include <fstream>

#pragma comment(lib, "psapi.lib")

// =========================
// Steam path (auto detect)
// =========================
std::string GetSteamPath() {
    HKEY hKey;
    char value[512];
    DWORD value_length = sizeof(value);

    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Valve\\Steam",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {

        if (RegQueryValueExA(hKey, "SteamPath", NULL, NULL,
            (LPBYTE)value, &value_length) == ERROR_SUCCESS) {

            RegCloseKey(hKey);
            return std::string(value);
        }

        RegCloseKey(hKey);
    }

    return "";
}

// =========================
// Process struct
// =========================
struct ProcessInfo {
    std::wstring name;
    DWORD pid;
    SIZE_T memory;
};

// =========================
// Get processes RAM
// =========================
std::vector<ProcessInfo> GetProcesses() {
    std::vector<ProcessInfo> list;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(snapshot, &pe)) {
        do {
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);

            if (hProcess) {
                PROCESS_MEMORY_COUNTERS pmc;

                if (GetProcessMemoryInfo(hProcess, &pmc, sizeof(pmc))) {
                    list.push_back({pe.szExeFile, pe.th32ProcessID, pmc.WorkingSetSize});
                }

                CloseHandle(hProcess);
            }

        } while (Process32Next(snapshot, &pe));
    }

    CloseHandle(snapshot);

    std::sort(list.begin(), list.end(), [](const ProcessInfo& a, const ProcessInfo& b) {
        return a.memory > b.memory;
    });

    return list;
}

// =========================
// Modify CS launch options
// =========================
void SetCSMemory(const std::string& steamPath, int memMB) {
    std::string path = steamPath + "\\userdata";

    std::string configPath = path + "\\config\\localconfig.vdf";

    std::ifstream in(configPath);
    if (!in.is_open()) {
        std::cout << "Cannot open Steam config\n";
        return;
    }

    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    in.close();

    std::string appID = "730"; // CS

    size_t pos = content.find(appID);
    if (pos == std::string::npos) return;

    std::string newLine = "\"LaunchOptions\" \"-mem " + std::to_string(memMB) + "\"";

    size_t launchPos = content.find("LaunchOptions", pos);

    if (launchPos != std::string::npos) {
        size_t end = content.find("\n", launchPos);
        content.replace(launchPos, end - launchPos, newLine);
    } else {
        content.insert(pos + appID.size(), "\n\t\t\t\t" + newLine);
    }

    std::ofstream out(configPath);
    out << content;
    out.close();

    std::cout << "CS memory setting applied\n";
}

// =========================
// MAIN
// =========================
int main() {
    setlocale(LC_ALL, "");

    int limitMB, csMB;

    std::cout << "Enter RAM limit for all apps (MB): ";
    std::cin >> limitMB;

    std::cout << "Enter RAM for CS (MB): ";
    std::cin >> csMB;

    std::string steamPath = GetSteamPath();

    if (steamPath.empty()) {
        std::cout << "Steam not found!\n";
        return 1;
    }

    std::cout << "Steam found: " << steamPath << "\n";

    SetCSMemory(steamPath, csMB);

    while (true) {
        system("cls");

        auto processes = GetProcesses();

        std::wcout << L"===== RAM MONITOR + STEAM OPTIMIZER =====\n\n";

        std::wcout << std::left
                   << std::setw(25) << L"Process"
                   << std::setw(10) << L"PID"
                   << std::setw(12) << L"RAM(MB)"
                   << std::endl;

        std::wcout << L"---------------------------------------------\n";

        for (const auto& p : processes) {
            double mb = p.memory / (1024.0 * 1024.0);

            std::wcout << std::left
                       << std::setw(25) << p.name
                       << std::setw(10) << p.pid
                       << std::setw(12) << std::fixed << std::setprecision(2) << mb
                       << std::endl;

            if (p.name != L"cs2.exe" && mb > limitMB) {
                std::wcout << L"  ⚠ WARNING: high RAM usage!\n";
            }
        }

        Sleep(2000);
    }

    return 0;
}
