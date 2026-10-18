// ============================================================================
// ScreenReplace.exe —— 注入器 + 选项卡 GUI（v4：原生经典 UI）
// 选项卡 1「基本」：替换图片（支持自带截屏导入）、窗口标题实时修改
// 选项卡 2「阻挡选项」：忽略广播 / 挡消息 / 挡命令 + 取消全屏广播
// 通过同目录 ScreenReplace.ini 与 DLL 通信（DLL 每 400ms 热重载配置）。
// 本程序与 DLL 均为 x86（极域 2016 学生端是 32 位进程）。
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <set>

#include "sr_compat.h"

#ifndef ICC_STANDARD_CLASSES
#  define ICC_STANDARD_CLASSES ICC_WIN95_CLASSES
#endif

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(linker, "/MANIFESTUAC:\"level='requireAdministrator' uiAccess='false'\"")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace Gdiplus;

#define IDC_EDIT_IMG      1001
#define IDC_BTN_IMG       1002
#define IDC_CHK_IMG       1005
#define IDC_BTN_APPLY     1011
#define IDC_BTN_EJECT     1012
#define IDC_BTN_REFRESH   1013
#define IDC_BTN_LOG       1014
#define IDC_STATUS        1015
#define IDC_TAB           1020
#define IDC_CHK_BLOCKBC   1021
#define IDC_CHK_BLOCKMSG  1022
#define IDC_CHK_BLOCKCMD  1023
#define IDC_BTN_CANCELFULL 1024
#define IDC_LBL_IMG       1025
#define IDC_LBL_HINT      1026
#define IDC_BTN_SHOT      1027
#define IDC_EDIT_TITLE    1028
#define IDC_BTN_APPLYTITLE 1029
#define IDC_LBL_TITLE     1030
#define IDC_REDPANEL      1031
#define IDC_LBL_DISC      1032
#define IDT_REINJECT      1

static HWND g_main = NULL;
static HWND g_tab = NULL;
static HWND g_editImg = NULL, g_editTitle = NULL, g_status = NULL;
static HWND g_redPanel = NULL, g_lblDisc = NULL;
static HWND g_tab1Ctrls[12];
static HWND g_tab2Ctrls[5];
static std::set<DWORD> g_injected;
static std::wstring g_logText;
static ULONG_PTR g_gdiplusToken = 0;
static HFONT g_bigFont = NULL;

// ---------------------------------------------------------------------------
static std::wstring ExeDir()
{
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(NULL, p, MAX_PATH);
    std::wstring s = p;
    size_t i = s.find_last_of(L'\\');
    return (i == std::wstring::npos) ? L"" : s.substr(0, i + 1);
}
static std::wstring IniFile() { return ExeDir() + L"ScreenReplace.ini"; }
static std::wstring DllFile() { return ExeDir() + L"ScreenReplaceHooks.dll"; }

static std::wstring NumStr(DWORD v)
{
    wchar_t b[32];
    SRFormat(b, 32, L"%u", (unsigned)v);
    return std::wstring(b);
}

static void Log(const std::wstring& s)
{
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t line[2048];
    SRFormat(line, 2048, L"[%02d:%02d:%02d] %s", st.wHour, st.wMinute, st.wSecond, s.c_str());
    g_logText += line;
    g_logText += L"\r\n";
    if (g_logText.size() > 200000) g_logText = g_logText.substr(g_logText.size() / 2);
    if (g_status) {
        SetWindowTextW(g_status, g_logText.c_str());
        int n = (int)g_logText.size();
        SendMessageW(g_status, EM_SETSEL, n, n);
        SendMessageW(g_status, EM_SCROLLCARET, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// 配置读写（与 DLL 共用 ScreenReplace.ini；[UI] 段是注入器专用）
// ---------------------------------------------------------------------------
static void ReadCfgToUI()
{
    wchar_t buf[MAX_PATH * 2];
    std::wstring ini = IniFile();
    GetPrivateProfileStringW(L"ScreenReplace", L"ImagePath", L"", buf, MAX_PATH * 2, ini.c_str());
    SetWindowTextW(g_editImg, buf);
    CheckDlgButton(g_main, IDC_CHK_IMG,       GetPrivateProfileIntW(L"ScreenReplace", L"EnableImage",    0, ini.c_str()) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_CHK_BLOCKBC,   GetPrivateProfileIntW(L"ScreenReplace", L"BlockBroadcast", 0, ini.c_str()) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_CHK_BLOCKMSG,  GetPrivateProfileIntW(L"ScreenReplace", L"BlockMsg",       0, ini.c_str()) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_CHK_BLOCKCMD,  GetPrivateProfileIntW(L"ScreenReplace", L"BlockCmd",       0, ini.c_str()) ? BST_CHECKED : BST_UNCHECKED);

    // 窗口标题
    GetPrivateProfileStringW(L"UI", L"Title", L"", buf, MAX_PATH * 2, ini.c_str());
    if (!buf[0]) wcscpy(buf, L"ScreenReplace v4");
    SetWindowTextW(g_main, buf);
    SetWindowTextW(g_editTitle, buf);
}

static void WriteCfgFromUI()
{
    wchar_t buf[MAX_PATH * 2];
    std::wstring ini = IniFile();
    GetWindowTextW(g_editImg, buf, MAX_PATH * 2);
    WritePrivateProfileStringW(L"ScreenReplace", L"ImagePath", buf, ini.c_str());
    WritePrivateProfileStringW(L"ScreenReplace", L"EnableImage",
        IsDlgButtonChecked(g_main, IDC_CHK_IMG) ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"ScreenReplace", L"BlockBroadcast",
        IsDlgButtonChecked(g_main, IDC_CHK_BLOCKBC) ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"ScreenReplace", L"BlockMsg",
        IsDlgButtonChecked(g_main, IDC_CHK_BLOCKMSG) ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"ScreenReplace", L"BlockCmd",
        IsDlgButtonChecked(g_main, IDC_CHK_BLOCKCMD) ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"ScreenReplace", L"LogLevel", L"1", ini.c_str());
}

// ---------------------------------------------------------------------------
// 进程 / 注入
// ---------------------------------------------------------------------------
static std::vector<DWORD> FindProcesses(const wchar_t* name)
{
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe = { sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (!_wcsicmp(pe.szExeFile, name)) out.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

static bool IsX86Process(DWORD pid)
{
    typedef BOOL (WINAPI *fnIWP)(HANDLE, BOOL*);
    fnIWP iwp = (fnIWP)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process");
    if (!iwp) return true;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return true;
    BOOL wow = FALSE;
    BOOL ok = iwp(h, &wow);
    CloseHandle(h);
    return !ok || wow;
}

static bool EnableDebugPriv()
{
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return false;
    LUID luid;
    if (!LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid)) { CloseHandle(t); return false; }
    TOKEN_PRIVILEGES tp = { 1 };
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(t, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(t);
    return GetLastError() == ERROR_SUCCESS;
}

static bool Inject(DWORD pid)
{
    std::wstring dll = DllFile();
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(L"[错误] 找不到 DLL: " + dll);
        return false;
    }
    HANDLE h = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                           PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!h) {
        Log(L"[错误] OpenProcess 失败 pid=" + NumStr(pid) + L"（需要管理员权限）");
        return false;
    }
    size_t n = (dll.size() + 1) * sizeof(wchar_t);
    void* rem = VirtualAllocEx(h, NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!rem) { CloseHandle(h); return false; }
    if (!WriteProcessMemory(h, rem, dll.c_str(), n, NULL)) {
        VirtualFreeEx(h, rem, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }
    FARPROC ll = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE th = CreateRemoteThread(h, NULL, 0, (LPTHREAD_START_ROUTINE)ll, rem, 0, NULL);
    if (!th) {
        VirtualFreeEx(h, rem, 0, MEM_RELEASE);
        CloseHandle(h);
        return false;
    }
    WaitForSingleObject(th, 10000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    VirtualFreeEx(h, rem, 0, MEM_RELEASE);
    CloseHandle(h);
    return code != 0;
}

static void InjectTargets(bool quiet)
{
    bool img = IsDlgButtonChecked(g_main, IDC_CHK_IMG) == BST_CHECKED;
    bool anyBlock = (IsDlgButtonChecked(g_main, IDC_CHK_BLOCKBC) == BST_CHECKED ||
                     IsDlgButtonChecked(g_main, IDC_CHK_BLOCKMSG) == BST_CHECKED ||
                     IsDlgButtonChecked(g_main, IDC_CHK_BLOCKCMD) == BST_CHECKED);

    if (img || anyBlock) {
        std::vector<DWORD> ps = FindProcesses(L"StudentMain.exe");
        if (ps.empty() && !quiet) Log(L"未找到 StudentMain.exe（极域学生端未运行）");
        for (DWORD pid : ps) {
            if (!IsX86Process(pid)) { Log(L"StudentMain 是 64 位进程，请改用 x64 编译版本"); continue; }
            if (g_injected.count(pid)) continue;
            if (Inject(pid)) { g_injected.insert(pid); Log(L"已注入 StudentMain.exe pid=" + NumStr(pid)); }
            else Log(L"注入失败 StudentMain.exe pid=" + NumStr(pid));
        }
    }
    if (img) {
        std::vector<DWORD> ph = FindProcesses(L"DispcapHelper.exe");
        if (ph.empty() && !quiet)
            Log(L"未找到 DispcapHelper.exe（教师打开监控时才启动；每 2 秒自动补注入）");
        for (DWORD pid : ph) {
            if (!IsX86Process(pid)) continue;
            if (g_injected.count(pid)) continue;
            if (Inject(pid)) { g_injected.insert(pid); Log(L"已注入 DispcapHelper.exe pid=" + NumStr(pid)); }
            else Log(L"注入失败 DispcapHelper.exe pid=" + NumStr(pid));
        }
    }
}

static void DoEjectAll()
{
    Log(L"已写入关闭配置。DLL 保留在目标进程内（纯透传、零风险），");
    Log(L"学生端下次重启后 DLL 自动消失。");
    g_injected.clear();
}

// ---------------------------------------------------------------------------
// 右上角"生效项目"列表（红色大字实时刷新）
// ---------------------------------------------------------------------------
static void UpdateActivePanel()
{
    if (!g_redPanel) return;
    std::wstring s;
    bool any = false;
    if (IsDlgButtonChecked(g_main, IDC_CHK_IMG) == BST_CHECKED)      { s += L"● 图片替换\r\n";   any = true; }
    if (IsDlgButtonChecked(g_main, IDC_CHK_BLOCKBC) == BST_CHECKED)  { s += L"● 忽略广播\r\n";   any = true; }
    if (IsDlgButtonChecked(g_main, IDC_CHK_BLOCKMSG) == BST_CHECKED) { s += L"● 挡教师消息\r\n"; any = true; }
    if (IsDlgButtonChecked(g_main, IDC_CHK_BLOCKCMD) == BST_CHECKED) { s += L"● 挡教师命令\r\n"; any = true; }
    if (!any) s = L"暂无生效项目";
    SetWindowTextW(g_redPanel, s.c_str());
}

// ---------------------------------------------------------------------------
// 自带截屏：隐藏本窗口 → 抓屏 → 保存 PNG → 导入为替换图片
// ---------------------------------------------------------------------------
static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    ImageCodecInfo* p = (ImageCodecInfo*)malloc(size);
    if (!p) return -1;
    GetImageEncoders(num, size, p);
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(p[i].MimeType, format) == 0) { *pClsid = p[i].Clsid; free(p); return (int)i; }
    }
    free(p);
    return -1;
}

static void DoScreenshot()
{
    ShowWindow(g_main, SW_HIDE);
    Sleep(350);

    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    HDC scr = GetDC(NULL);
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp && mem && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, w, h, scr, 0, 0, SRCCOPY | CAPTUREBLT);
        SelectObject(mem, old);

        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t path[MAX_PATH];
        GetTempPathW(MAX_PATH, path);
        wchar_t name[64];
        SRFormat(name, 64, L"ScreenReplace_capture_%04d%02d%02d_%02d%02d%02d.png",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        SRAppend(path, MAX_PATH, name);

        Bitmap gb(bmp, NULL);
        CLSID pngClsid;
        if (GetEncoderClsid(L"image/png", &pngClsid) >= 0 && gb.Save(path, &pngClsid, NULL) == Ok) {
            SetWindowTextW(g_editImg, path);
            CheckDlgButton(g_main, IDC_CHK_IMG, BST_CHECKED);
            Log(std::wstring(L"截屏已保存并导入: ") + path);
        } else {
            Log(L"截屏保存失败");
        }
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, scr);

    ShowWindow(g_main, SW_SHOW);
    SetForegroundWindow(g_main);
}

// ---------------------------------------------------------------------------
// 界面（原生控件，经典外观）
// ---------------------------------------------------------------------------
static void BrowseFile(HWND edit, const wchar_t* filter, const wchar_t* title)
{
    wchar_t buf[MAX_PATH * 2] = { 0 };
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH * 2;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) SetWindowTextW(edit, buf);
}

static void ShowTab(int idx)
{
    for (int i = 0; i < 12; i++) if (g_tab1Ctrls[i]) ShowWindow(g_tab1Ctrls[i], idx == 0 ? SW_SHOW : SW_HIDE);
    for (int i = 0; i < 5;  i++) if (g_tab2Ctrls[i]) ShowWindow(g_tab2Ctrls[i], idx == 1 ? SW_SHOW : SW_HIDE);
}

static void BuildUI(HWND hwnd)
{
    HINSTANCE hi = GetModuleHandleW(NULL);
    const int E = 10;

    // 顶部：选项卡（左）+ 生效项目列表（右上角，红色大字）
    g_tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                            8, 6, 310, 26, hwnd, (HMENU)IDC_TAB, hi, NULL);
    TCITEMW ti = { TCIF_TEXT };
    ti.pszText = L"基本";
    TabCtrl_InsertItem(g_tab, 0, &ti);
    ti.pszText = L"阻挡选项";
    TabCtrl_InsertItem(g_tab, 1, &ti);

    g_redPanel = CreateWindowExW(0, L"STATIC", L"",
                    WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                    324, 6, 224, 78, hwnd, (HMENU)IDC_REDPANEL, hi, NULL);
    if (g_bigFont) SendMessageW(g_redPanel, WM_SETFONT, (WPARAM)g_bigFont, TRUE);

    // ---- 选项卡 1：基本 ----
    g_tab1Ctrls[0] = CreateWindowExW(0, L"STATIC", L"替换图片（教师端监控/截屏/打开看看到的画面）:",
                    WS_CHILD | WS_VISIBLE, E, 94, 400, 18, hwnd, (HMENU)IDC_LBL_IMG, hi, NULL);
    g_editImg = g_tab1Ctrls[1] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    E, 116, 390, 24, hwnd, (HMENU)IDC_EDIT_IMG, hi, NULL);
    g_tab1Ctrls[2] = CreateWindowExW(0, L"BUTTON", L"浏览...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    410, 115, 120, 26, hwnd, (HMENU)IDC_BTN_IMG, hi, NULL);
    g_tab1Ctrls[3] = CreateWindowExW(0, L"BUTTON", L"截屏并导入", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    410, 147, 120, 26, hwnd, (HMENU)IDC_BTN_SHOT, hi, NULL);
    g_tab1Ctrls[4] = CreateWindowExW(0, L"BUTTON", L"启用图片替换（颜色/方向修正已自动处理）",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                    E, 150, 380, 22, hwnd, (HMENU)IDC_CHK_IMG, hi, NULL);

    g_tab1Ctrls[5] = CreateWindowExW(0, L"STATIC", L"窗口标题:", WS_CHILD | WS_VISIBLE,
                    E, 186, 70, 18, hwnd, (HMENU)IDC_LBL_TITLE, hi, NULL);
    g_editTitle = g_tab1Ctrls[6] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                    90, 183, 300, 24, hwnd, (HMENU)IDC_EDIT_TITLE, hi, NULL);
    g_tab1Ctrls[7] = CreateWindowExW(0, L"BUTTON", L"应用标题", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    400, 182, 130, 26, hwnd, (HMENU)IDC_BTN_APPLYTITLE, hi, NULL);

    g_tab1Ctrls[8] = CreateWindowExW(0, L"BUTTON", L"保存配置并注入", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    E, 220, 150, 34, hwnd, (HMENU)IDC_BTN_APPLY, hi, NULL);
    g_tab1Ctrls[9] = CreateWindowExW(0, L"BUTTON", L"卸载并还原", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    170, 222, 110, 30, hwnd, (HMENU)IDC_BTN_EJECT, hi, NULL);
    g_tab1Ctrls[10] = CreateWindowExW(0, L"BUTTON", L"刷新", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    290, 222, 80, 30, hwnd, (HMENU)IDC_BTN_REFRESH, hi, NULL);

    // ---- 选项卡 2：阻挡选项 ----
    g_tab2Ctrls[0] = CreateWindowExW(0, L"BUTTON", L"忽略广播（教师广播不显示/自动最小化）",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                    E, 104, 340, 22, hwnd, (HMENU)IDC_CHK_BLOCKBC, hi, NULL);
    g_tab2Ctrls[1] = CreateWindowExW(0, L"BUTTON", L"挡教师发消息",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                    E, 132, 200, 22, hwnd, (HMENU)IDC_CHK_BLOCKMSG, hi, NULL);
    g_tab2Ctrls[2] = CreateWindowExW(0, L"BUTTON", L"挡教师执行命令",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                    E, 160, 200, 22, hwnd, (HMENU)IDC_CHK_BLOCKCMD, hi, NULL);
    g_tab2Ctrls[3] = CreateWindowExW(0, L"BUTTON", L"取消全屏广播", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    E, 196, 210, 36, hwnd, (HMENU)IDC_BTN_CANCELFULL, hi, NULL);
    g_tab2Ctrls[4] = CreateWindowExW(0, L"STATIC",
                    L"提示：关机/重启指令不再拦截；勾选后点「保存配置并注入」生效。",
                    WS_CHILD | WS_VISIBLE, E, 248, 400, 18, hwnd, (HMENU)IDC_LBL_HINT, hi, NULL);

    // 打开日志（两个选项卡都可见）
    CreateWindowExW(0, L"BUTTON", L"打开日志", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                    400, 222, 130, 30, hwnd, (HMENU)IDC_BTN_LOG, hi, NULL);

    g_status = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY,
                    E, 264, 530, 268, hwnd, (HMENU)IDC_STATUS, hi, NULL);
    SendMessageW(g_status, EM_SETLIMITTEXT, 0, 0);

    // 底部免责声明
    g_lblDisc = CreateWindowExW(0, L"STATIC", L"仅供学习用途",
                    WS_CHILD | WS_VISIBLE | SS_CENTER,
                    E, 540, 540, 18, hwnd, (HMENU)IDC_LBL_DISC, hi, NULL);

    ShowTab(0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_bigFont = CreateFontW(-MulDiv(11, 96, 72), 0, 0, 0, FW_BOLD,
                                FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        BuildUI(hwnd);
        ReadCfgToUI();
        UpdateActivePanel();
        SetTimer(hwnd, IDT_REINJECT, 2000, NULL);
        Log(L"就绪。选项卡「基本」选图/截屏导入；「阻挡选项」勾选要挡的功能。");
        if (GetFileAttributesW(DllFile().c_str()) == INVALID_FILE_ATTRIBUTES)
            Log(L"[警告] 未找到 ScreenReplaceHooks.dll");
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        if ((HWND)lp == g_redPanel) {
            SetTextColor(dc, RGB(222, 40, 40));          // 右上角：红色醒目
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        if ((HWND)lp == g_lblDisc) {
            SetTextColor(dc, RGB(120, 124, 132));        // 底部：灰色
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        break;
    }
    case WM_NOTIFY: {
        NMHDR* p = (NMHDR*)lp;
        if (p->idFrom == IDC_TAB && p->code == TCN_SELCHANGE)
            ShowTab(TabCtrl_GetCurSel(g_tab));
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_REINJECT) {
            UpdateActivePanel();
            if (IsDlgButtonChecked(hwnd, IDC_CHK_IMG) == BST_CHECKED ||
                IsDlgButtonChecked(hwnd, IDC_CHK_BLOCKBC) == BST_CHECKED ||
                IsDlgButtonChecked(hwnd, IDC_CHK_BLOCKMSG) == BST_CHECKED ||
                IsDlgButtonChecked(hwnd, IDC_CHK_BLOCKCMD) == BST_CHECKED)
                InjectTargets(true);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_IMG:
            BrowseFile(g_editImg,
                L"图片文件\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0所有文件\0*.*\0\0", L"选择替换图片");
            return 0;
        case IDC_BTN_SHOT:
            DoScreenshot();
            UpdateActivePanel();
            return 0;
        case IDC_BTN_APPLYTITLE: {
            wchar_t buf[256];
            GetWindowTextW(g_editTitle, buf, 256);
            SetWindowTextW(g_main, buf);                     // 不关软件直接改标题
            WritePrivateProfileStringW(L"UI", L"Title", buf, IniFile().c_str());
            Log(L"窗口标题已更新（即时生效，已保存）");
            return 0;
        }
        case IDC_BTN_APPLY:
            WriteCfgFromUI();
            EnableDebugPriv();
            InjectTargets(false);
            UpdateActivePanel();
            Log(L"配置已写入: " + IniFile());
            return 0;
        case IDC_BTN_CANCELFULL: {
            WritePrivateProfileStringW(L"ScreenReplace", L"CancelBroadcast", L"1", IniFile().c_str());
            EnableDebugPriv();
            std::vector<DWORD> ps = FindProcesses(L"StudentMain.exe");
            bool injected = false;
            for (DWORD pid : ps) {
                if (!IsX86Process(pid)) continue;
                if (g_injected.count(pid)) { injected = true; continue; }
                if (Inject(pid)) { g_injected.insert(pid); injected = true; }
            }
            if (!ps.empty() && !injected)
                Log(L"[错误] 无法注入 StudentMain，取消广播失败（需要管理员权限）");
            else if (ps.empty())
                Log(L"未找到 StudentMain.exe（极域学生端未运行）");
            else
                Log(L"已发送「取消全屏广播」指令，广播窗口将被最小化且可关闭");
            UpdateActivePanel();
            return 0;
        }
        case IDC_BTN_EJECT:
            CheckDlgButton(hwnd, IDC_CHK_IMG, BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHK_BLOCKBC, BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHK_BLOCKMSG, BST_UNCHECKED);
            CheckDlgButton(hwnd, IDC_CHK_BLOCKCMD, BST_UNCHECKED);
            WriteCfgFromUI();
            DoEjectAll();
            UpdateActivePanel();
            Log(L"已写入关闭配置（停止全部替换/阻挡，学生端进程不受影响）");
            return 0;
        case IDC_BTN_REFRESH:
            InjectTargets(true);
            Log(L"刷新完成");
            return 0;
        case IDC_BTN_LOG: {
            wchar_t path[MAX_PATH];
            GetTempPathW(MAX_PATH, path);
            SRAppend(path, MAX_PATH, L"ScreenReplace.log");
            ShellExecuteW(hwnd, L"open", path, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        }
        break;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_REINJECT);
        if (g_bigFont) DeleteObject(g_bigFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    GdiplusStartupInput gsi;
    GdiplusStartup(&g_gdiplusToken, &gsi, NULL);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ScreenReplaceMain";
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, 560, 600 };
    AdjustWindowRect(&rc, style, FALSE);
    g_main = CreateWindowExW(0, wc.lpszClassName, L"ScreenReplace v4",
                             style, CW_USEDEFAULT, CW_USEDEFAULT,
                             rc.right - rc.left, rc.bottom - rc.top,
                             NULL, NULL, hi, NULL);
    if (!g_main) return 1;
    ShowWindow(g_main, SW_SHOWNORMAL);
    UpdateWindow(g_main);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    GdiplusShutdown(g_gdiplusToken);
    return (int)m.wParam;
}
