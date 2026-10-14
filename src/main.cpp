// No arguments opens the window. With arguments it launches the client directly, no window:
// TibiaFpsUnlimiter.exe path\to\client.exe [client arguments...]

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <uxtheme.h>
#include "resource.h"
#include "unlimit.h"

static const wchar_t kRegKey[] = L"Software\\TibiaFpsUnlimiter";
static const UINT_PTR kRefreshTimer = 1;

static HWND g_dlg, g_list, g_status;
static std::vector<TibiaClient> g_clients;

static std::wstring loadClientPath() {
    wchar_t buf[MAX_PATH];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"ClientPath", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return L"";
}

static void saveClientPath(const std::wstring& path) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"ClientPath", REG_SZ, path.c_str(),
                    DWORD((path.size() + 1) * sizeof(wchar_t)));
}

static std::wstring characterName(const std::wstring& title) {
    size_t sep = title.find(L" - ");
    return sep == std::wstring::npos ? L"(login screen)" : title.substr(sep + 3);
}

// Last two folders of the install (without bin), which is what tells installs apart.
static std::wstring folderName(std::wstring path) {
    path.resize(path.find_last_of(L"\\/"));
    size_t bin = path.find_last_of(L"\\/");
    if (bin != std::wstring::npos && _wcsicmp(path.c_str() + bin + 1, L"bin") == 0) path.resize(bin);
    size_t first = path.find_last_of(L"\\/");
    size_t second = first == std::wstring::npos ? first : path.find_last_of(L"\\/", first - 1);
    return second == std::wstring::npos ? path : path.substr(second + 1);
}

static const wchar_t* stateText(LimitState s) {
    switch (s) {
    case LimitState::Active: return L"active";
    case LimitState::Removed: return L"removed";
    default: return L"no access";
    }
}

static void setStatus(const std::wstring& text) {
    SendMessageW(g_status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

static int selectedIndex() {
    return ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
}

// The default button follows the focused section, so Enter in the path field launches the client.
static void updateButtons() {
    int sel = selectedIndex();
    bool canUnlimit = sel >= 0 && g_clients[sel].state != LimitState::Removed;
    EnableWindow(GetDlgItem(g_dlg, IDC_UNLIMIT), canUnlimit);

    HWND focus = GetFocus();
    bool inLaunch = focus == GetDlgItem(g_dlg, IDC_PATH) || focus == GetDlgItem(g_dlg, IDC_BROWSE);
    SendMessageW(g_dlg, DM_SETDEFID, canUnlimit && !inLaunch ? IDC_UNLIMIT : IDC_LAUNCH, 0);
}

static void refresh(bool manual) {
    auto clients = listTibiaClients();
    if (!manual && clients == g_clients) return;

    int sel = selectedIndex();
    DWORD selPid = sel >= 0 ? g_clients[sel].pid : 0;
    g_clients = std::move(clients);

    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    int newSel = g_clients.empty() ? -1 : 0;
    for (int i = 0; i < (int)g_clients.size(); ++i) {
        auto& c = g_clients[i];
        std::wstring name = characterName(c.title), install = folderName(c.exePath), pid = std::to_wstring(c.pid);
        LVITEMW item{LVIF_TEXT, i};
        item.pszText = name.data();
        ListView_InsertItem(g_list, &item);
        ListView_SetItemText(g_list, i, 1, install.data());
        ListView_SetItemText(g_list, i, 2, const_cast<wchar_t*>(stateText(c.state)));
        ListView_SetItemText(g_list, i, 3, pid.data());
        if (c.pid == selPid) newSel = i;
    }
    if (newSel >= 0) ListView_SetItemState(g_list, newSel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    updateButtons();

    if (manual) {
        size_t n = g_clients.size();
        setStatus(n == 0 ? L"No Tibia client open."
                 : n == 1 ? L"1 client found."
                          : std::to_wstring(n) + L" clients found.");
    }
}

static void unlimitSelected() {
    int sel = selectedIndex();
    if (sel < 0) return;
    auto c = g_clients[sel];
    std::wstring msg;
    bool ok = unlimitProcess(c.pid, msg);
    std::wstring who = c.title.find(L" - ") == std::wstring::npos ? folderName(c.exePath) : characterName(c.title);
    setStatus(ok ? who + L": " + msg : msg);
    refresh(false);
}

static void launch() {
    wchar_t buf[MAX_PATH];
    if (GetDlgItemTextW(g_dlg, IDC_PATH, buf, MAX_PATH) == 0) {
        setStatus(L"Choose the client executable with Browse.");
        return;
    }
    std::wstring msg;
    if (launchClient(buf, L"", msg)) saveClientPath(buf);
    setStatus(msg);
}

static void browse() {
    wchar_t file[MAX_PATH];
    GetDlgItemTextW(g_dlg, IDC_PATH, file, MAX_PATH);
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g_dlg;
    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose the client executable";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) SetDlgItemTextW(g_dlg, IDC_PATH, file);
}

static void initList() {
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(g_list, L"Explorer", nullptr);
    struct { const wchar_t* name; int width; int fmt; } cols[] = {
        {L"Character", 82, LVCFMT_LEFT},
        {L"Folder", 102, LVCFMT_LEFT},
        {L"Qt limit", 56, LVCFMT_LEFT},
        {L"PID", 38, LVCFMT_RIGHT},
    };
    for (int i = 0; i < 4; ++i) {
        RECT r{0, 0, cols[i].width, 0};
        MapDialogRect(g_dlg, &r);
        LVCOLUMNW col{LVCF_TEXT | LVCF_WIDTH | LVCF_FMT, cols[i].fmt, r.right};
        col.pszText = const_cast<wchar_t*>(cols[i].name);
        ListView_InsertColumn(g_list, i, &col);
    }
}

static INT_PTR CALLBACK dlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG:
        g_dlg = dlg;
        g_list = GetDlgItem(dlg, IDC_LIST);
        g_status = GetDlgItem(dlg, IDC_STATUS);
        SendMessageW(g_status, WM_SIZE, 0, 0);
        initList();
        SetDlgItemTextW(dlg, IDC_PATH, loadClientPath().c_str());
        SendDlgItemMessageW(dlg, IDC_PATH, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Choose the client executable"));
        refresh(true);
        SetTimer(dlg, kRefreshTimer, 3000, nullptr);
        return TRUE;

    case WM_TIMER:
        if (wp == kRefreshTimer) refresh(false);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_REFRESH: refresh(true); return TRUE;
        case IDC_UNLIMIT: unlimitSelected(); return TRUE;
        case IDC_BROWSE: browse(); return TRUE;
        case IDC_LAUNCH: launch(); return TRUE;
        case IDC_PATH:
            if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) updateButtons();
            return TRUE;
        case IDCANCEL: DestroyWindow(dlg); return TRUE;
        }
        break;

    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->idFrom != IDC_LIST) break;
        if (hdr->code == LVN_GETEMPTYMARKUP) {
            auto* m = reinterpret_cast<NMLVEMPTYMARKUP*>(lp);
            m->dwFlags = EMF_CENTERED;
            wcscpy_s(m->szMarkup, L"No Tibia client open.\nStart one under \"Launch a new client\" or click Refresh.");
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }
        if (hdr->code == LVN_ITEMCHANGED || hdr->code == NM_SETFOCUS) updateButtons();
        break;
    }

    case WM_DESTROY:
        KillTimer(dlg, kRefreshTimer);
        PostQuitMessage(0);
        return TRUE;
    }
    return FALSE;
}

static int launchFromArgs(int argc, wchar_t** argv) {
    std::wstring args;
    for (int i = 2; i < argc; ++i) args += (i > 2 ? L" \"" : L"\"") + std::wstring(argv[i]) + L"\"";
    std::wstring msg;
    if (launchClient(argv[1], args, msg)) return 0;
    MessageBoxW(nullptr, msg.c_str(), L"Tibia FPS Unlimiter", MB_ICONERROR);
    return 1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) {
        int rc = launchFromArgs(argc, argv);
        LocalFree(argv);
        return rc;
    }
    LocalFree(argv);

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    HWND dlg = CreateDialogParamW(inst, MAKEINTRESOURCEW(IDD_MAIN), nullptr, dlgProc, 0);
    if (!dlg) return 1;
    ShowWindow(dlg, show);
    HACCEL accel = LoadAcceleratorsW(inst, MAKEINTRESOURCEW(IDR_ACCEL));

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(dlg, accel, &m) || IsDialogMessageW(dlg, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
