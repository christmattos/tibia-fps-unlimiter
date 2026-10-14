#pragma once
#include <windows.h>
#include <string>
#include <vector>

enum class LimitState { Active, Removed, Unknown };

struct TibiaClient {
    DWORD pid;
    std::wstring title;
    std::wstring exePath;
    LimitState state;
    bool operator==(const TibiaClient&) const = default;
};

// Processes with Qt6Gui.dll or Qt5Gui.dll and a visible window whose title starts with "Tibia" or that have ..\conf\clientoptions.json.
std::vector<TibiaClient> listTibiaClients();

// Writes into the process Qt*Gui.dll the same state that QT_QPA_UPDATE_IDLE_TIME=0 would produce.
bool unlimitProcess(DWORD pid, std::wstring& msg);

bool launchClient(const std::wstring& exe, const std::wstring& extraArgs, std::wstring& msg);
