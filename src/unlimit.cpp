#include "unlimit.h"
#include <tlhelp32.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace {

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : h(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (ok()) CloseHandle(h); }
    operator HANDLE() const { return h; }
    bool ok() const { return h && h != INVALID_HANDLE_VALUE; }
};

std::wstring lastError(const wchar_t* what) {
    return std::wstring(what) + L" failed (error " + std::to_wstring(GetLastError()) + L").";
}

bool findQtGui(DWORD pid, MODULEENTRY32W& out) {
    Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid));
    if (!snap.ok()) return false;
    MODULEENTRY32W me{sizeof(me)};
    for (BOOL more = Module32FirstW(snap, &me); more; more = Module32NextW(snap, &me)) {
        if (_wcsicmp(me.szModule, L"Qt6Gui.dll") == 0 || _wcsicmp(me.szModule, L"Qt5Gui.dll") == 0) { out = me; return true; }
    }
    return false;
}

// Statics of QPlatformWindow::requestUpdate(), read from the code that initializes them. Known shapes:
// Qt 6.8+ x64:        lea rdx,[valid] / lea rcx,[str] / call / lea rcx,[guard] / mov [interval],eax
// Qt 5, Qt 6.5 x64:   lea rdx,[rsp+n] / lea rcx,[str] / call / lea rcx,[guard] / mov edx,5 / cmovne / mov [interval],edx
// Qt 5 x86:           push eax / push str / call / push guard / mov ecx,5 / cmovne / mov [interval],ecx
struct Statics { DWORD valid, interval, guard; bool hasValid; };

bool findStatics(const std::wstring& path, Statics& st, std::wstring& msg) {
    std::ifstream f(path, std::ios::binary);
    std::vector<unsigned char> file((std::istreambuf_iterator<char>(f)), {});
    if (file.size() < 0x400) { msg = L"Could not read " + path + L"."; return false; }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(file.data() + dos->e_lfanew);
    bool x64 = nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    DWORD imageBase = x64 ? 0 : reinterpret_cast<IMAGE_NT_HEADERS32*>(nt)->OptionalHeader.ImageBase;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    WORD nsec = nt->FileHeader.NumberOfSections;

    auto toRva = [&](size_t off) -> long long {
        for (WORD i = 0; i < nsec; ++i)
            if (off >= sec[i].PointerToRawData && off < sec[i].PointerToRawData + sec[i].SizeOfRawData)
                return (long long)off - sec[i].PointerToRawData + sec[i].VirtualAddress;
        return -1;
    };
    auto u32 = [&](size_t off) { uint32_t r; memcpy(&r, &file[off], 4); return r; };
    // Address referenced by the instruction at off: rip-relative on x64, absolute on x86.
    auto target = [&](size_t off, size_t dispAt, size_t len) -> long long {
        return x64 ? toRva(off) + (long long)len + (int32_t)u32(dispAt) : (long long)u32(dispAt) - imageBase;
    };

    const wchar_t* unsupported = L"This Qt version is not supported. Nothing was changed.";
    static const char key[] = "QT_QPA_UPDATE_IDLE_TIME";
    auto it = std::search(file.begin(), file.end(), key, key + sizeof(key));
    if (it == file.end()) { msg = unsupported; return false; }
    long long strRva = toRva(it - file.begin());

    for (WORD s = 0; s < nsec; ++s) {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        size_t beg = sec[s].PointerToRawData, end = beg + sec[s].SizeOfRawData;
        for (size_t o = beg + 8; o + 64 < end; ++o) {
            size_t len;
            if (x64 && file[o] == 0x48 && file[o + 1] == 0x8D && file[o + 2] == 0x0D && target(o, o + 3, 7) == strRva) len = 7;
            else if (!x64 && file[o] == 0x68 && target(o, o + 1, 5) == strRva) len = 5;
            else continue;

            size_t call = o + len;
            if (file[call] != 0xFF || file[call + 1] != 0x15) { msg = unsupported; return false; }

            st = {};
            if (x64 && file[o - 7] == 0x48 && file[o - 6] == 0x8D && file[o - 5] == 0x15) {
                st.valid = DWORD(target(o - 7, o - 4, 7));
                st.hasValid = true;
            }
            bool hasGuard = false, hasInterval = false, hasDefault5 = false;
            for (size_t p = call + 6; p < call + 48; ++p) {
                if (!hasGuard && x64 && file[p] == 0x48 && file[p + 1] == 0x8D && file[p + 2] == 0x0D) {
                    st.guard = DWORD(target(p, p + 3, 7)); hasGuard = true; p += 6;
                } else if (!hasGuard && !x64 && file[p] == 0x68) {
                    st.guard = DWORD(target(p, p + 1, 5)); hasGuard = true; p += 4;
                } else if (file[p] >= 0xB8 && file[p] <= 0xBF && u32(p + 1) == 5) {
                    hasDefault5 = true; p += 4;
                } else if (!hasInterval && file[p] == 0x89 && (file[p + 1] & 0xC7) == 0x05) {
                    st.interval = DWORD(target(p, p + 2, 6)); hasInterval = true; p += 5;
                } else if (!hasInterval && !x64 && file[p] == 0xA3) {
                    st.interval = DWORD(target(p, p + 1, 5)); hasInterval = true; p += 4;
                }
            }
            if (!hasGuard || !hasInterval || !(st.hasValid || hasDefault5)) { msg = unsupported; return false; }
            return true;
        }
    }
    msg = unsupported;
    return false;
}

struct Target {
    Handle proc;
    BYTE* base = nullptr;
    Statics st{};
};

bool openTarget(DWORD pid, DWORD access, Target& t, std::wstring& msg) {
    static std::map<std::wstring, Statics> cache;

    MODULEENTRY32W me;
    if (!findQtGui(pid, me)) { msg = L"The client closed or does not use Qt."; return false; }
    auto it = cache.find(me.szExePath);
    if (it == cache.end()) {
        Statics st;
        if (!findStatics(me.szExePath, st, msg)) return false;
        it = cache.emplace(me.szExePath, st).first;
    }
    t.st = it->second;
    t.base = me.modBaseAddr;
    t.proc.h = OpenProcess(access, FALSE, pid);
    if (!t.proc.ok()) {
        msg = GetLastError() == ERROR_ACCESS_DENIED
            ? L"No permission to access this client. If it runs as administrator, run this program as administrator too."
            : lastError(L"OpenProcess");
        return false;
    }
    return true;
}

struct Limit { BYTE valid = 1; int interval = 0, guard = 0; };

bool readLimit(Target& t, Limit& l) {
    return ReadProcessMemory(t.proc, t.base + t.st.guard, &l.guard, 4, nullptr) &&
           ReadProcessMemory(t.proc, t.base + t.st.interval, &l.interval, 4, nullptr) &&
           (!t.st.hasValid || ReadProcessMemory(t.proc, t.base + t.st.valid, &l.valid, 1, nullptr));
}

LimitState limitState(DWORD pid) {
    Target t;
    std::wstring msg;
    Limit l;
    if (!openTarget(pid, PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION, t, msg) || !readLimit(t, l))
        return LimitState::Unknown;
    return l.valid && l.interval == 0 ? LimitState::Removed : LimitState::Active;
}

struct WindowList { std::vector<std::pair<DWORD, std::wstring>> items; };

BOOL CALLBACK collectWindow(HWND hwnd, LPARAM lp) {
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) return TRUE;
    wchar_t buf[256];
    int n = GetWindowTextW(hwnd, buf, 256);
    if (n == 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    reinterpret_cast<WindowList*>(lp)->items.emplace_back(pid, std::wstring(buf, n));
    return TRUE;
}

// Every 15.x client keeps its options in conf\clientoptions.json, one folder above the exe.
bool looksLikeTibiaClient(const std::wstring& exe, const std::wstring& title) {
    if (title.rfind(L"Tibia", 0) == 0) return true;
    std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
    std::wstring conf = dir.substr(0, dir.find_last_of(L'\\')) + L"\\conf\\clientoptions.json";
    return GetFileAttributesW(conf.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

std::vector<TibiaClient> listTibiaClients() {
    WindowList wl;
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&wl));

    std::vector<TibiaClient> out;
    for (auto& [pid, title] : wl.items) {
        if (pid == GetCurrentProcessId()) continue;
        if (std::any_of(out.begin(), out.end(), [&](auto& c) { return c.pid == pid; })) continue;

        Handle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (!proc.ok() || !QueryFullProcessImageNameW(proc, 0, path, &len)) continue;
        std::wstring exe(path, len);
        if (!looksLikeTibiaClient(exe, title)) continue;

        MODULEENTRY32W me;
        if (!findQtGui(pid, me)) continue;
        out.push_back({pid, title, exe, limitState(pid)});
    }
    return out;
}

bool unlimitProcess(DWORD pid, std::wstring& msg) {
    Target t;
    if (!openTarget(pid, PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, t, msg))
        return false;

    Limit l;
    if (!readLimit(t, l)) { msg = lastError(L"ReadProcessMemory"); return false; }
    // Before the first requestUpdate() the guard is 0, and the initialization would overwrite our write.
    if (l.guard == 0) { msg = L"The client is still starting. Try again in a few seconds."; return false; }
    if (l.valid && l.interval == 0) { msg = L"limit was already removed."; return true; }

    BYTE one = 1;
    int zero = 0;
    if (!WriteProcessMemory(t.proc, t.base + t.st.interval, &zero, 4, nullptr) ||
        (t.st.hasValid && !WriteProcessMemory(t.proc, t.base + t.st.valid, &one, 1, nullptr))) {
        msg = lastError(L"WriteProcessMemory");
        return false;
    }
    msg = L"limit removed.";
    return true;
}

bool launchClient(const std::wstring& exe, const std::wstring& extraArgs, std::wstring& msg) {
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) { msg = L"Client not found at " + exe + L"."; return false; }
    std::wstring dir = exe.substr(0, exe.find_last_of(L"\\/"));
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!extraArgs.empty()) cmd += L" " + extraArgs;

    SetEnvironmentVariableW(L"QT_QPA_UPDATE_IDLE_TIME", L"0");
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi);
    SetEnvironmentVariableW(L"QT_QPA_UPDATE_IDLE_TIME", nullptr);
    if (!ok) { msg = lastError(L"Launching the client"); return false; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    msg = L"Client started. It shows up in the list once its window loads.";
    return true;
}
