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
// Process struct
// =========================
struct ProcessInfo {
    std::wstring name;
    DWORD pid;
    SIZE_T memory;
};

// =========================
// Steam path (registry)
// =========================
std::string GetSteamPath() {
    HKEY hKey;
    char value[512];
    DWORD size = sizeof(value);

    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Valve\\Steam",
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

// =========================
// Get RAM usage
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

    std::sort(list.begin(), list.end(),
        [](const ProcessInfo& a, const ProcessInfo& b) {
            return a.memory > b.memory;
        });

    return list;
}

// =========================
// Set Dota 2 launch options
// =========================
void SetDotaOptions(const std::string& steamPath, int memMB) {
    std::string path = steamPath + "\\userdata\\config\\localconfig.vdf";

    std::ifstream in(path);
    if (!in.is_open()) {
        std::cout << "Cannot open Steam config\n";
        return;
    }

    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    in.close();

    std::string appID = "570"; // Dota 2

    size_t pos = content.find(appID);
    if (pos == std::string::npos) return;

    std::string newOpt = "\"LaunchOptions\" \"-mem " + std::to_string(memMB) + "\"";

    size_t launchPos = content.find("LaunchOptions", pos);

    if (launchPos != std::string::npos) {
        size_t end = content.find("\n", launchPos);
        content.replace(launchPos, end - launchPos, newOpt);
    } else {
        content.insert(pos + appID.size(), "\n\t\t\t\t" + newOpt);
    }

    std::ofstream out(path);
    out << content;
    out.close();

    std::cout << "Dota 2 launch options updated\n";
}

// =========================
// MAIN
// =========================
int main() {
    setlocale(LC_ALL, "");

    int limitMB;
    int dotaMB;

    std::cout << "Enter RAM limit for all apps (MB): ";
    std::cin >> limitMB;

    std::cout << "Enter RAM for Dota 2 (MB): ";
    std::cin >> dotaMB;

    std::string steamPath = GetSteamPath();

    if (steamPath.empty()) {
        std::cout << "Steam not found!\n";
        return 1;
    }

    std::cout << "Steam found: " << steamPath << "\n";

    SetDotaOptions(steamPath, dotaMB);

    while (true) {
        system("cls");

        auto processes = GetProcesses();

        std::wcout << L"===== RAM MONITOR + DOTA OPTIMIZER =====\n\n";

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

            if (p.name != L"dota2.exe" && mb > limitMB) {
                std::wcout << L"  ⚠ WARNING: high RAM usage!\n";
            }
        }

        Sleep(2000);
    }

    return 0;
}
