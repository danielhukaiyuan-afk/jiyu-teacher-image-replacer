// ============================================================================
// ScreenReplaceHooks.dll  v2
// 注入到 StudentMain.exe / DispcapHelper.exe：
//   1) 教师端"屏幕监控/截屏/打开看"显示指定图片（颜色/翻转修正已固定启用）
//   2) 可选的阻挡（选项卡控制）：忽略广播 / 挡教师消息 / 挡教师执行命令
//      （关机/重启指令不再拦截，直接放行）
//   3) "取消全屏广播"：去掉极域广播窗口的 CS_NOCLOSE（关闭按钮可点）、
//      补上系统菜单样式，并可一键最小化；广播无法抢回前台/全屏
//
// 参考：imengyu/JiYuTrainer（GitHub，1011 stars）的广播窗口化实现
//       * VFixGuangBoWindow：加 WS_OVERLAPPEDWINDOW|WS_SYSMENU、清 CS_NOCLOSE
//       * 广播窗口识别：标题含 广播/演示/共享/共享屏幕/屏幕演播室窗口
//       * hkSetWindowPos/hkSetForegroundWindow 拦截全屏与前台抢占
// 原理其余部分来自 Dzjs-Trainer 逆向结论（LibJPEG20/DispFilter/LibDeskMonitor）
// ============================================================================

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <cwctype>
#include <string>
#include <vector>
#include <gdiplus.h>
#include <shlwapi.h>

#include "sr_compat.h"
#include "mhook-lib/mhook.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ws2_32.lib")

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
static std::wstring g_dir;                       // 本 DLL 所在目录（含尾斜杠）
static bool g_isStudentMain     = false;
static bool g_isCaptureHelper   = false;
static bool g_unloading         = false;
static HANDLE g_watchdog        = NULL;

static CRITICAL_SECTION g_logCs; static bool g_logCsInit = false;
static int g_logLevel = 0;

static void SRLog(const wchar_t* fmt, ...)
{
    if (g_logLevel < 1) return;
    wchar_t buf[1024]; va_list ap; va_start(ap, fmt);
    SRFormatV(buf, 1024, fmt, ap); va_end(ap);
    if (!g_logCsInit) { InitializeCriticalSection(&g_logCs); g_logCsInit = true; }
    EnterCriticalSection(&g_logCs);
    wchar_t path[MAX_PATH]; GetTempPathW(MAX_PATH, path);
    SRAppend(path, MAX_PATH, L"ScreenReplace.log");
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t line[1280];
        SRFormat(line, 1280, L"[%02d:%02d:%02d.%03d] %s\r\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        char mb[4096];
        int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, mb, sizeof(mb), NULL, NULL);
        if (n > 1) { DWORD wr = 0; WriteFile(f, mb, n - 1, &wr, NULL); }
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_logCs);
}

// ---------------------------------------------------------------------------
// 配置（INI 与注入器共享：<exe目录>\ScreenReplace.ini）
// ---------------------------------------------------------------------------
struct SRConfig {
    std::wstring imagePath;
    bool enableImage     = false;   // 教师端所有画面路径 → 图片
    bool blockMsg        = false;   // 挡教师发消息
    bool blockCmd        = false;   // 挡教师执行命令
    bool blockBroadcast  = false;   // 忽略广播：广播窗口自动最小化、抢不了全屏
    int  logLevel        = 1;
};
static SRConfig g_cfg;

// 颜色/方向修正：按用户实测的极域版本固定启用（JPEG 与 DXGI 缓冲均为 BGR+自底向上）
static const bool kSwapRB = true;
static const bool kFlipV  = true;

static std::wstring IniPath() { return g_dir + L"ScreenReplace.ini"; }

static void EnsureRelevantHooks();   // 前向声明

static void LoadConfig()
{
    SRConfig c;
    wchar_t buf[MAX_PATH * 2];
    GetPrivateProfileStringW(L"ScreenReplace", L"ImagePath", L"", buf, MAX_PATH * 2, IniPath().c_str());
    c.imagePath = buf;
    c.enableImage     = GetPrivateProfileIntW(L"ScreenReplace", L"EnableImage",    0, IniPath().c_str()) != 0;
    c.blockMsg        = GetPrivateProfileIntW(L"ScreenReplace", L"BlockMsg",       0, IniPath().c_str()) != 0;
    c.blockCmd        = GetPrivateProfileIntW(L"ScreenReplace", L"BlockCmd",       0, IniPath().c_str()) != 0;
    c.blockBroadcast  = GetPrivateProfileIntW(L"ScreenReplace", L"BlockBroadcast", 0, IniPath().c_str()) != 0;
    c.logLevel        = GetPrivateProfileIntW(L"ScreenReplace", L"LogLevel",       1, IniPath().c_str());
    g_cfg = c; g_logLevel = c.logLevel;
}

static bool AnyBlockOn() { return g_cfg.blockMsg || g_cfg.blockCmd || g_cfg.blockBroadcast; }

// ---------------------------------------------------------------------------
// 替换帧存储（只有图片一份；引用计数保证捕获线程安全）
// ---------------------------------------------------------------------------
struct FrameStore {
    CRITICAL_SECTION cs; bool csInit = false;
    bool valid = false;
    BYTE* rgb = NULL; BYTE* dib = NULL;
    int w = 0, h = 0, stride = 0;
    volatile LONG refs = 0;
    std::vector<BYTE*> pending;
};
static FrameStore g_img;

static void StoreInit(FrameStore& s) { if (!s.csInit) { InitializeCriticalSection(&s.cs); s.csInit = true; } }

static void StorePublish(FrameStore& s, BYTE* rgb, BYTE* dib, int w, int h, int stride)
{
    StoreInit(s);
    EnterCriticalSection(&s.cs);
    if (s.valid) {
        if (s.rgb) s.pending.push_back(s.rgb);
        if (s.dib) s.pending.push_back(s.dib);
    }
    s.rgb = rgb; s.dib = dib; s.w = w; s.h = h; s.stride = stride; s.valid = true;
    LeaveCriticalSection(&s.cs);
}

static bool StoreAcquire(FrameStore& s, BYTE*& rgb, BYTE*& dib, int& w, int& h, int& stride)
{
    rgb = NULL; dib = NULL;
    if (!s.csInit || !s.valid) return false;
    EnterCriticalSection(&s.cs);
    if (!s.valid) { LeaveCriticalSection(&s.cs); return false; }
    InterlockedIncrement(&s.refs);
    rgb = s.rgb; dib = s.dib; w = s.w; h = s.h; stride = s.stride;
    LeaveCriticalSection(&s.cs);
    return true;
}

static void StoreRelease(FrameStore& s)
{
    EnterCriticalSection(&s.cs);
    if (InterlockedDecrement(&s.refs) == 0 && !s.pending.empty()) {
        for (size_t i = 0; i < s.pending.size(); ++i) delete[] s.pending[i];
        s.pending.clear();
    }
    LeaveCriticalSection(&s.cs);
}

static void StoreClear(FrameStore& s)
{
    if (!s.csInit) return;
    EnterCriticalSection(&s.cs);
    if (s.valid) {
        s.valid = false;
        if (s.refs == 0) { delete[] s.rgb; delete[] s.dib; }
        else { if (s.rgb) s.pending.push_back(s.rgb); if (s.dib) s.pending.push_back(s.dib); }
        s.rgb = NULL; s.dib = NULL; s.w = s.h = s.stride = 0;
    }
    LeaveCriticalSection(&s.cs);
}

// ---------------------------------------------------------------------------
// 图片解码：BMP/PNG/JPG → RGB24 + 顶向下 24bpp DIB
// ---------------------------------------------------------------------------
static bool BitmapToRGB24(Bitmap* bmp, BYTE*& rgb, BYTE*& dib, int& w, int& h, int& stride)
{
    rgb = NULL; dib = NULL; w = h = stride = 0;
    if (!bmp || bmp->GetLastStatus() != Ok) return false;
    bool ok = false;
    UINT bw = bmp->GetWidth(), bh = bmp->GetHeight();
    if (bw > 0 && bh > 0 && bw <= 16384 && bh <= 16384) {
        BitmapData bd; Rect r(0, 0, bw, bh);
        if (bmp->LockBits(&r, ImageLockModeRead, PixelFormat24bppRGB, &bd) == Ok) {
            BYTE* prgb = new (std::nothrow) BYTE[(size_t)bw * 3 * bh];
            if (prgb) {
                for (UINT y = 0; y < bh; y++) {
                    const BYTE* s = (const BYTE*)bd.Scan0 + (size_t)y * bd.Stride;
                    BYTE* d = prgb + (size_t)y * bw * 3;
                    for (UINT x = 0; x < bw; x++) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; s += 3; d += 3; }
                }
                int dstride = (bw * 3 + 3) & ~3;
                BYTE* pdib = new (std::nothrow) BYTE[(size_t)dstride * bh];
                if (pdib) {
                    ZeroMemory(pdib, (size_t)dstride * bh);
                    for (UINT y = 0; y < bh; y++) {
                        const BYTE* s2 = prgb + (size_t)y * bw * 3;
                        BYTE* d2 = pdib + (size_t)y * dstride;
                        for (UINT x = 0; x < bw; x++) { d2[0] = s2[2]; d2[1] = s2[1]; d2[2] = s2[0]; s2 += 3; d2 += 3; }
                    }
                    rgb = prgb; dib = pdib; w = (int)bw; h = (int)bh; stride = dstride; ok = true;
                } else delete[] prgb;
            }
            bmp->UnlockBits(&bd);
        }
    }
    return ok;
}

static bool DecodeToRGB24(LPCWSTR path, BYTE*& rgb, BYTE*& dib, int& w, int& h, int& stride)
{
    rgb = NULL; dib = NULL; w = h = stride = 0;
    GdiplusStartupInput gsi; ULONG_PTR tok = 0;
    if (GdiplusStartup(&tok, &gsi, NULL) != Ok) return false;
    bool ok = false;
    Bitmap* bmp = Bitmap::FromFile(path);
    if (bmp) {
        ok = BitmapToRGB24(bmp, rgb, dib, w, h, stride);
        delete bmp;
    }
    GdiplusShutdown(tok);
    return ok;
}

static bool LoadImageInto(FrameStore& s, LPCWSTR path)
{
    if (!path || !*path || !PathFileExistsW(path)) { StoreClear(s); return false; }
    BYTE* rgb = NULL; BYTE* dib = NULL; int w = 0, h = 0, stride = 0;
    if (!DecodeToRGB24(path, rgb, dib, w, h, stride)) {
        StoreClear(s);
        SRLog(L"图片解码失败: %s", path);
        return false;
    }
    StorePublish(s, rgb, dib, w, h, stride);
    SRLog(L"图片已加载 %dx%d: %s", w, h, path);
    return true;
}

// ---------------------------------------------------------------------------
// 把图片画进目标 DC（拉伸铺满）
// ---------------------------------------------------------------------------
static bool DrawFrame(HDC hdc, FrameStore& s, int x, int y, int w, int h)
{
    BYTE* rgb = NULL; BYTE* dib = NULL; int iw = 0, ih = 0, stride = 0;
    if (!StoreAcquire(s, rgb, dib, iw, ih, stride)) return false;
    bool ok = false;
    if (dib && iw > 0 && ih > 0 && stride >= iw * 3 && w > 0 && h > 0) {
        BITMAPINFO info; ZeroMemory(&info, sizeof(info));
        info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth       = iw;
        info.bmiHeader.biHeight      = -ih;      // 顶向下
        info.bmiHeader.biPlanes      = 1;
        info.bmiHeader.biBitCount    = 24;
        info.bmiHeader.biCompression = BI_RGB;
        int old = SetStretchBltMode(hdc, COLORONCOLOR);
        int n = StretchDIBits(hdc, x, y, w, h, 0, 0, iw, ih, dib, &info, DIB_RGB_COLORS, SRCCOPY);
        if (old != 0) SetStretchBltMode(hdc, old);
        ok = (n != 0 && n != GDI_ERROR);
    }
    StoreRelease(s);
    return ok;
}

// ---------------------------------------------------------------------------
// 钩子 1：gdi32!BitBlt —— 视频流/整屏捕获 → 图片
// ---------------------------------------------------------------------------
typedef BOOL (WINAPI *fnBitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
static fnBitBlt fpBitBlt = NULL; static bool hkBitBltOn = false;
static SR_TLS bool tlsInBitBlt = false;

static bool IsScreenCopyRop(DWORD rop) { return (rop & 0x00FFFFFFu) == SRCCOPY; }

static BOOL WINAPI hkBitBlt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop)
{
    if (!fpBitBlt) return FALSE;
    if (tlsInBitBlt) return fpBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
    BOOL r = fpBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
    if (!g_unloading && g_cfg.enableImage &&
        IsScreenCopyRop(rop) && GetObjectType(hdc) == OBJ_MEMDC && GetObjectType(hdcSrc) == OBJ_DC) {
        bool doIt;
        if (g_isCaptureHelper) doIt = true;
        else doIt = (cx == GetSystemMetrics(SM_CXSCREEN) && cy == GetSystemMetrics(SM_CYSCREEN));
        if (doIt) {
            tlsInBitBlt = true;
            if (!DrawFrame(hdc, g_img, x, y, cx, cy))
                PatBlt(hdc, x, y, cx, cy, BLACKNESS);
            tlsInBitBlt = false;
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// 钩子 2：LibDeskMonitor.dll IAT 补丁 —— "打开看"实时画面 → 图片
// ---------------------------------------------------------------------------
static BOOL (WINAPI *g_origLdmBitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD) = NULL;
static BOOL (WINAPI *g_origLdmStretchBlt)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD) = NULL;
static ULONG_PTR g_ldmSlots[2]; static ULONG_PTR g_ldmOrigs[2]; static int g_ldmCount = 0;
static SR_TLS bool tlsInLdm = false;

static BOOL PatchLdmImport(HMODULE hMod, const char* dllName, const char* funcName, ULONG_PTR hook, ULONG_PTR* origOut)
{
    if (!hMod || g_ldmCount >= 2) return FALSE;
    BYTE* base = (BYTE*)hMod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return FALSE;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for (; imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dllName) != 0) continue;
        PIMAGE_THUNK_DATA oft = (PIMAGE_THUNK_DATA)(base + imp->OriginalFirstThunk);
        PIMAGE_THUNK_DATA ft  = (PIMAGE_THUNK_DATA)(base + imp->FirstThunk);
        if (!oft) oft = ft;
        for (; ft->u1.Function; ++oft, ++ft) {
            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            PIMAGE_IMPORT_BY_NAME ibn = (PIMAGE_IMPORT_BY_NAME)(base + oft->u1.AddressOfData);
            if (!ibn || _stricmp((LPCSTR)ibn->Name, funcName) != 0) continue;
            DWORD oldProt = 0;
            if (!VirtualProtect(&ft->u1.Function, sizeof(ULONG_PTR), PAGE_EXECUTE_READWRITE, &oldProt)) return FALSE;
            ULONG_PTR orig = ft->u1.Function;
            if (origOut) *origOut = orig;
            ft->u1.Function = hook;
            VirtualProtect(&ft->u1.Function, sizeof(ULONG_PTR), oldProt, &oldProt);
            g_ldmSlots[g_ldmCount] = (ULONG_PTR)&ft->u1.Function;
            g_ldmOrigs[g_ldmCount] = orig;
            g_ldmCount++;
            return TRUE;
        }
    }
    return FALSE;
}

static void LdmRenderInto(HDC hdc, int x, int y, int cx, int cy)
{
    if (!DrawFrame(hdc, g_img, x, y, cx, cy))
        PatBlt(hdc, x, y, cx, cy, BLACKNESS);
}

static BOOL WINAPI hkLdmBitBlt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop)
{
    if (!g_origLdmBitBlt) return FALSE;
    if (tlsInLdm) return g_origLdmBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
    BOOL r = g_origLdmBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
    if (!g_unloading && g_cfg.enableImage && IsScreenCopyRop(rop) &&
        GetObjectType(hdc) == OBJ_MEMDC && GetObjectType(hdcSrc) == OBJ_DC) {
        tlsInLdm = true;
        LdmRenderInto(hdc, x, y, cx, cy);
        tlsInLdm = false;
    }
    return r;
}

static BOOL WINAPI hkLdmStretchBlt(HDC hdc, int x, int y, int cx, int cy,
                                   HDC hdcSrc, int sx, int sy, int cw, int ch, DWORD rop)
{
    if (!g_origLdmStretchBlt) return FALSE;
    if (tlsInLdm) return g_origLdmStretchBlt(hdc, x, y, cx, cy, hdcSrc, sx, sy, cw, ch, rop);
    BOOL r = g_origLdmStretchBlt(hdc, x, y, cx, cy, hdcSrc, sx, sy, cw, ch, rop);
    if (!g_unloading && g_cfg.enableImage && IsScreenCopyRop(rop) &&
        GetObjectType(hdc) == OBJ_MEMDC && GetObjectType(hdcSrc) == OBJ_DC) {
        tlsInLdm = true;
        LdmRenderInto(hdc, x, y, cx, cy);
        tlsInLdm = false;
    }
    return r;
}

// ---------------------------------------------------------------------------
// 钩子 3：LibJPEG20!EncodeToJPEGBuffer —— 缩略图/监控流 → 图片
// 颜色交换与垂直翻转已按实测版本固定启用（kSwapRB / kFlipV）
// ---------------------------------------------------------------------------
typedef int (__cdecl *fnJpegEnc)(const BYTE* pixels, int width, int height, int stride,
                                 BYTE* jpegOutput, DWORD* jpegLength, int quality, int r1, int r2);
static fnJpegEnc fpJpeg = NULL; static bool hkJpegOn = false;

static void OverlayRGB(BYTE* dst, int dw, int dh, int dstride,
                       const BYTE* src, int sw, int sh, bool swapRB, bool flipV)
{
    if (!src || sw <= 0 || sh <= 0) return;
    for (int y = 0; y < dh; y++) {
        int sy = (int)((long long)y * sh / dh); if (sy >= sh) sy = sh - 1;
        if (flipV) sy = sh - 1 - sy;
        BYTE* d = dst + (size_t)y * dstride;
        const BYTE* s = src + (size_t)sy * sw * 3;
        for (int x = 0; x < dw; x++) {
            int sx = (int)((long long)x * sw / dw); if (sx >= sw) sx = sw - 1;
            if (swapRB) { d[0] = s[sx * 3 + 2]; d[1] = s[sx * 3 + 1]; d[2] = s[sx * 3 + 0]; }
            else        { d[0] = s[sx * 3 + 0]; d[1] = s[sx * 3 + 1]; d[2] = s[sx * 3 + 2]; }
            d += 3;
        }
    }
}

static int __cdecl hkJpeg(const BYTE* pixels, int w, int h, int stride,
                          BYTE* out, DWORD* outLen, int quality, int r1, int r2)
{
    if (!fpJpeg) return 0;
    if (g_unloading || !g_cfg.enableImage)
        return fpJpeg(pixels, w, h, stride, out, outLen, quality, r1, r2);

    BYTE* rgb = NULL; BYTE* dib = NULL; int iw = 0, ih = 0, istride = 0;
    if (StoreAcquire(g_img, rgb, dib, iw, ih, istride)) {
        if (rgb && iw > 0 && ih > 0 && w > 0 && h > 0 && stride >= w * 3) {
            OverlayRGB(const_cast<BYTE*>(pixels), w, h, stride, rgb, iw, ih, kSwapRB, kFlipV);
            int r = fpJpeg(pixels, w, h, stride, out, outLen, quality, r1, r2);
            StoreRelease(g_img);
            return r;
        }
        StoreRelease(g_img);
    }
    memset(const_cast<BYTE*>(pixels), 0, (size_t)stride * h);   // 无图 → 纯黑帧
    return fpJpeg(pixels, w, h, stride, out, outLen, quality, r1, r2);
}

// ---------------------------------------------------------------------------
// 钩子 4：DispFilter.dll 序号 1 DispDXGIBitBlt —— DXGI 帧 → 图片
// ---------------------------------------------------------------------------
typedef BOOL (__cdecl *fnDispDXGIBitBlt)(void* pRect, BYTE* lpbyBits);
static fnDispDXGIBitBlt fpDxgi = NULL; static bool hkDxgiOn = false;

static BOOL __cdecl hkDxgi(void* pRect, BYTE* buf)
{
    BOOL r = fpDxgi ? fpDxgi(pRect, buf) : FALSE;
    if (g_unloading || !r || !buf || !pRect) return r;
    RECT* rc = (RECT*)pRect;
    LONG w = rc->right - rc->left, h = rc->bottom - rc->top;
    if (w <= 0 || h <= 0 || w >= 8192 || h >= 8192) return r;

    BYTE* rgb = NULL; BYTE* dib = NULL; int iw = 0, ih = 0, istride = 0;
    if (g_cfg.enableImage && StoreAcquire(g_img, rgb, dib, iw, ih, istride)) {
        DWORD* px = (DWORD*)buf;
        for (LONG y = 0; y < h; y++) {
            LONG yy = kFlipV ? (h - 1 - y) : y;
            int sy = (int)((long long)y * ih / h); if (sy >= ih) sy = ih - 1;
            const BYTE* s = rgb + (size_t)sy * iw * 3;
            DWORD* row = px + (size_t)yy * w;
            for (LONG x = 0; x < w; x++) {
                int sx = (int)((long long)x * iw / w); if (sx >= iw) sx = iw - 1;
                BYTE B = s[sx * 3 + 0], G = s[sx * 3 + 1], R = s[sx * 3 + 2];
                row[x] = kSwapRB ? (0xFF000000u | ((DWORD)R << 16) | ((DWORD)G << 8) | B)
                                 : (0xFF000000u | ((DWORD)B << 16) | ((DWORD)G << 8) | R);
            }
        }
        StoreRelease(g_img);
    } else {
        DWORD* px = (DWORD*)buf;
        SIZE_T n = (SIZE_T)w * h;
        for (SIZE_T i = 0; i < n; i++) px[i] = 0xFF000000u;   // 不透明黑
    }
    return r;
}

// ---------------------------------------------------------------------------
// 钩子 5：冻结 —— 按类型拦教师指令（关机/重启已去掉，永不拦截）
// 指令包：0-3 = "DMOC"，偏移 52 = 类型（0x05 消息 / 0x0f 执行命令）
// ---------------------------------------------------------------------------
typedef int (WSAAPI *fnRecvFrom)(SOCKET, char*, int, int, sockaddr*, int*);
typedef int (WSAAPI *fnWSARecvFrom)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD,
                                    sockaddr*, LPINT, LPWSAOVERLAPPED,
                                    LPWSAOVERLAPPED_COMPLETION_ROUTINE);
static fnRecvFrom fpRecvFrom = NULL; static fnWSARecvFrom fpWSARecvFrom = NULL;
static bool hkRecvOn = false, hkWSARecvOn = false;

static bool ShouldDropPacket(const char* buf, int len)
{
    if (!buf || len < 56) return false;
    const unsigned char* p = (const unsigned char*)buf;
    if (p[0] != 0x44 || p[1] != 0x4d || p[2] != 0x4f || p[3] != 0x43) return false;   // DMOC
    unsigned char t = p[52];
    if (t == 0x05) return g_cfg.blockMsg;      // 消息
    if (t == 0x0f) return g_cfg.blockCmd;      // 执行命令
    return false;                              // 关机/重启(0x13/0x14)等一律放行
}

static int WSAAPI hkRecvFrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen)
{
    int r = fpRecvFrom(s, buf, len, flags, from, fromlen);
    if (r > 0 && ShouldDropPacket(buf, r)) {
        SRLog(L"已挡指令 type=0x%02x (%d 字节)", (unsigned char)buf[52], r);
        return 0;
    }
    return r;
}

static int WSAAPI hkWSARecvFrom(SOCKET s, LPWSABUF bufs, DWORD nbufs, LPDWORD recvd,
                                LPDWORD flags, sockaddr* from, LPINT fromlen,
                                LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
{
    int r = fpWSARecvFrom(s, bufs, nbufs, recvd, flags, from, fromlen, ov, cr);
    if (r == 0 && recvd && *recvd > 0 && bufs && nbufs > 0 && ShouldDropPacket(bufs[0].buf, (int)*recvd)) {
        SRLog(L"已挡指令(WSARecvFrom) type=0x%02x (%u 字节)", (unsigned char)bufs[0].buf[52], *recvd);
        *recvd = 0;
        if (bufs[0].buf) ZeroMemory(bufs[0].buf, bufs[0].len);
    }
    return r;
}

// ---------------------------------------------------------------------------
// 钩子 6：广播窗口处理（参考 imengyu/JiYuTrainer 的窗口化实现）
//  * 广播窗口识别：标题含 广播/演示/共享/共享屏幕/屏幕演播室窗口
//  * 修复：加 WS_OVERLAPPEDWINDOW|WS_SYSMENU、去掉 CS_NOCLOSE（关闭按钮可点）
//  * 拦截：广播窗口的全屏 SetWindowPos 与前台抢占（忽略广播/手动取消后）
// ---------------------------------------------------------------------------
typedef BOOL (WINAPI *fnSetForegroundWindow)(HWND);
typedef BOOL (WINAPI *fnSetWindowPos)(HWND, HWND, int, int, int, int, UINT);
static fnSetForegroundWindow fpSetForegroundWindow = NULL;
static fnSetWindowPos fpSetWindowPos = NULL;
static bool hkUiOn = false;

static HWND g_cancelledWnd  = NULL;   // 手动取消后需要挡全屏的广播窗口
static HWND g_lastSeenGbWnd = NULL;   // 最近一次扫描发现的广播窗口

static bool IsBroadcastTitle(LPCWSTR t)
{
    if (!t || !*t) return false;
    return wcsstr(t, L"广播") || wcsstr(t, L"演示") || wcsstr(t, L"共享")
        || wcsstr(t, L"屏幕演播室窗口") || wcsstr(t, L"BlackScreen Window");
}

static bool IsBroadcastWindow(HWND h)
{
    wchar_t buf[80];
    return h && IsWindow(h) && GetWindowTextW(h, buf, 80) && IsBroadcastTitle(buf);
}

// 修复广播窗口：去掉 CS_NOCLOSE（极域用它禁用关闭按钮），补系统菜单样式
static void BroadcastFix(HWND h)
{
    if (!h || !IsWindow(h)) return;
    LONG style = GetWindowLongW(h, GWL_STYLE);
    style |= WS_OVERLAPPEDWINDOW | WS_SYSMENU;
    SetWindowLongW(h, GWL_STYLE, style);
    LONG cls = GetClassLongW(h, GCL_STYLE);
    if (cls & CS_NOCLOSE) {
        cls &= ~CS_NOCLOSE;
        SetClassLongW(h, GCL_STYLE, cls);
    }
}

static BOOL CALLBACK EnumBroadcastProc(HWND h, LPARAM lp)
{
    if (!IsBroadcastWindow(h)) return TRUE;
    g_lastSeenGbWnd = h;
    BroadcastFix(h);
    if (g_cfg.blockBroadcast || h == g_cancelledWnd)
        ShowWindow(h, SW_MINIMIZE);
    return TRUE;
}

static BOOL WINAPI hkSetForegroundWindow(HWND hWnd)
{
    if (!g_unloading && (g_cfg.blockBroadcast || hWnd == g_cancelledWnd) && IsBroadcastWindow(hWnd))
        return TRUE;   // 广播窗口不许抢前台
    return fpSetForegroundWindow(hWnd);
}

static BOOL WINAPI hkSetWindowPos(HWND hWnd, HWND hAfter, int x, int y, int cx, int cy, UINT f)
{
    if (!g_unloading && !(f & SWP_NOSIZE)) {
        bool full = (x == 0 && y == 0 &&
                     cx == GetSystemMetrics(SM_CXSCREEN) && cy == GetSystemMetrics(SM_CYSCREEN));
        if (full && (g_cfg.blockBroadcast || hWnd == g_cancelledWnd) && IsBroadcastWindow(hWnd))
            return TRUE;   // 忽略广播/已取消 → 不许重新全屏
    }
    return fpSetWindowPos(hWnd, hAfter, x, y, cx, cy, f);
}

// ---------------------------------------------------------------------------
// 钩子 7：LoadLibraryW/ExW —— 检测 DispFilter/LibJPEG20/LibDeskMonitor 加载
// ---------------------------------------------------------------------------
typedef HMODULE (WINAPI *fnLLW)(LPCWSTR);
typedef HMODULE (WINAPI *fnLLX)(LPCWSTR, HANDLE, DWORD);
static fnLLW fpLLW = NULL; static fnLLX fpLLX = NULL; static bool hkLLOn = false;

static bool IsRelevantModule(LPCWSTR file)
{
    LPCWSTR name = PathFindFileNameW(file);
    return name &&
        (!_wcsicmp(name, L"dispfilter.dll") ||
         !_wcsicmp(name, L"libjpeg20.dll") ||
         !_wcsicmp(name, L"libdeskmonitor.dll"));
}

static HMODULE WINAPI hkLLW(LPCWSTR f)
{
    HMODULE m = fpLLW(f);
    if (m && f && IsRelevantModule(f))
        SRLog(L"检测到模块加载: %s", PathFindFileNameW(f));
    return m;
}

static HMODULE WINAPI hkLLX(LPCWSTR f, HANDLE hFile, DWORD flags)
{
    HMODULE m = fpLLX(f, hFile, flags);
    if (m && f && IsRelevantModule(f))
        SRLog(L"检测到模块加载: %s", PathFindFileNameW(f));
    return m;
}

// ---------------------------------------------------------------------------
// 钩子安装/卸载（幂等 + 并发闸门）
// ---------------------------------------------------------------------------
static volatile LONG g_hookInstalling = 0;

static void EnsureRelevantHooks()
{
    if (g_unloading) return;
    if (InterlockedCompareExchange(&g_hookInstalling, 1, 0) != 0) return;

    // 1) BitBlt（截屏/监控流）
    if (g_cfg.enableImage && !hkBitBltOn) {
        HMODULE hG = GetModuleHandleW(L"gdi32.dll");
        if (hG) {
            fpBitBlt = (fnBitBlt)GetProcAddress(hG, "BitBlt");
            if (fpBitBlt) {
                hkBitBltOn = Mhook_SetHook((PVOID*)&fpBitBlt, (PVOID)hkBitBlt) != 0;
                SRLog(L"BitBlt 钩子: %s", hkBitBltOn ? L"已装" : L"失败");
            }
        }
    }

    // 2) LibDeskMonitor IAT（打开看）
    if (g_cfg.enableImage && g_ldmCount < 2) {
        HMODULE hL = GetModuleHandleW(L"LibDeskMonitor.dll");
        if (hL) {
            PatchLdmImport(hL, "GDI32.dll", "BitBlt",     (ULONG_PTR)hkLdmBitBlt,     (ULONG_PTR*)&g_origLdmBitBlt);
            PatchLdmImport(hL, "GDI32.dll", "StretchBlt", (ULONG_PTR)hkLdmStretchBlt, (ULONG_PTR*)&g_origLdmStretchBlt);
        }
    }

    // 3) LibJPEG20（缩略图/监控流）
    if (g_cfg.enableImage && !hkJpegOn) {
        HMODULE hJ = GetModuleHandleW(L"LibJPEG20.dll");
        if (hJ) {
            fpJpeg = (fnJpegEnc)GetProcAddress(hJ, "EncodeToJPEGBuffer");
            if (!fpJpeg) fpJpeg = (fnJpegEnc)GetProcAddress(hJ, (LPCSTR)4);
            if (fpJpeg) {
                hkJpegOn = Mhook_SetHook((PVOID*)&fpJpeg, (PVOID)hkJpeg) != 0;
                SRLog(L"LibJPEG20 钩子: %s", hkJpegOn ? L"已装" : L"失败");
            }
        }
    }

    // 4) DispFilter DXGI（打开看实时流）
    if (g_cfg.enableImage && !hkDxgiOn) {
        HMODULE hD = GetModuleHandleW(L"DispFilter.dll");
        if (hD) {
            fpDxgi = (fnDispDXGIBitBlt)GetProcAddress(hD, (LPCSTR)1);
            if (fpDxgi) {
                hkDxgiOn = Mhook_SetHook((PVOID*)&fpDxgi, (PVOID)hkDxgi) != 0;
                SRLog(L"DispFilter DXGI 钩子: %s", hkDxgiOn ? L"已装" : L"失败");
            }
        }
    }

    // 5) 冻结（仅 StudentMain，它是 UDP 监听者）
    if (g_isStudentMain && (g_cfg.blockMsg || g_cfg.blockCmd) && (!hkRecvOn || !hkWSARecvOn)) {
        HMODULE hW = GetModuleHandleW(L"ws2_32.dll");
        if (hW) {
            if (!fpRecvFrom)    fpRecvFrom    = (fnRecvFrom)GetProcAddress(hW, "recvfrom");
            if (!fpWSARecvFrom) fpWSARecvFrom = (fnWSARecvFrom)GetProcAddress(hW, "WSARecvFrom");
            if (!hkRecvOn && fpRecvFrom)
                hkRecvOn = Mhook_SetHook((PVOID*)&fpRecvFrom, (PVOID)hkRecvFrom) != 0;
            if (!hkWSARecvOn && fpWSARecvFrom)
                hkWSARecvOn = Mhook_SetHook((PVOID*)&fpWSARecvFrom, (PVOID)hkWSARecvFrom) != 0;
            SRLog(L"指令拦截钩子: recvfrom=%d wsarecvfrom=%d", hkRecvOn, hkWSARecvOn);
        }
    }

    // 6) 广播窗口钩子（仅 StudentMain）
    if (g_isStudentMain && AnyBlockOn() && !hkUiOn) {
        HMODULE hU = GetModuleHandleW(L"user32.dll");
        if (hU) {
            fpSetForegroundWindow = (fnSetForegroundWindow)GetProcAddress(hU, "SetForegroundWindow");
            fpSetWindowPos        = (fnSetWindowPos)GetProcAddress(hU, "SetWindowPos");
            bool a = fpSetForegroundWindow ? (Mhook_SetHook((PVOID*)&fpSetForegroundWindow, (PVOID)hkSetForegroundWindow) != 0) : false;
            bool b = fpSetWindowPos        ? (Mhook_SetHook((PVOID*)&fpSetWindowPos, (PVOID)hkSetWindowPos) != 0) : false;
            hkUiOn = a && b;
            SRLog(L"广播窗口钩子: %s (fg=%d pos=%d)", hkUiOn ? L"已装" : L"未装", a, b);
        }
    }

    // 7) LoadLibrary 检测
    if (g_cfg.enableImage && !hkLLOn) {
        HMODULE hK = GetModuleHandleW(L"kernel32.dll");
        if (hK) {
            fpLLW = (fnLLW)GetProcAddress(hK, "LoadLibraryW");
            fpLLX = (fnLLX)GetProcAddress(hK, "LoadLibraryExW");
            if (fpLLW) Mhook_SetHook((PVOID*)&fpLLW, (PVOID)hkLLW);
            if (fpLLX) Mhook_SetHook((PVOID*)&fpLLX, (PVOID)hkLLX);
            hkLLOn = true;
        }
    }

    InterlockedExchange(&g_hookInstalling, 0);
}

static void UninstallAll()
{
    g_unloading = true;
    if (g_watchdog) { WaitForSingleObject(g_watchdog, 1500); CloseHandle(g_watchdog); g_watchdog = NULL; }

    if (hkBitBltOn && fpBitBlt)  { Mhook_Unhook((PVOID*)&fpBitBlt);  hkBitBltOn = false; }
    if (hkJpegOn && fpJpeg)      { Mhook_Unhook((PVOID*)&fpJpeg);     hkJpegOn = false; }
    if (hkDxgiOn && fpDxgi)      { Mhook_Unhook((PVOID*)&fpDxgi);     hkDxgiOn = false; }
    if (hkRecvOn && fpRecvFrom)       { Mhook_Unhook((PVOID*)&fpRecvFrom);    hkRecvOn = false; }
    if (hkWSARecvOn && fpWSARecvFrom) { Mhook_Unhook((PVOID*)&fpWSARecvFrom); hkWSARecvOn = false; }
    if (hkUiOn) {
        if (fpSetForegroundWindow) Mhook_Unhook((PVOID*)&fpSetForegroundWindow);
        if (fpSetWindowPos)        Mhook_Unhook((PVOID*)&fpSetWindowPos);
        hkUiOn = false;
    }
    if (hkLLOn) {
        if (fpLLW) Mhook_Unhook((PVOID*)&fpLLW);
        if (fpLLX) Mhook_Unhook((PVOID*)&fpLLX);
        hkLLOn = false;
    }
    for (int i = 0; i < g_ldmCount; i++) {
        DWORD oldProt = 0;
        if (VirtualProtect((PVOID)g_ldmSlots[i], sizeof(ULONG_PTR), PAGE_EXECUTE_READWRITE, &oldProt)) {
            *(ULONG_PTR*)g_ldmSlots[i] = g_ldmOrigs[i];
            VirtualProtect((PVOID)g_ldmSlots[i], sizeof(ULONG_PTR), oldProt, &oldProt);
        }
    }
    g_ldmCount = 0;
    StoreClear(g_img);
}

// ---------------------------------------------------------------------------
// 看门狗：配置热重载 + 钩子补装 + 图片加载 + 广播处理（自动最小化/手动取消）
// ---------------------------------------------------------------------------
static FILETIME g_iniMtime = { 0 };
static std::wstring g_loadedImg;

static DWORD WINAPI Watchdog(LPVOID)
{
    for (;;) {
        Sleep(400);
        if (g_unloading) break;

        // 配置热重载
        WIN32_FILE_ATTRIBUTE_DATA ad;
        std::wstring ini = IniPath();
        if (GetFileAttributesExW(ini.c_str(), GetFileExInfoStandard, &ad)) {
            if (ad.ftLastWriteTime.dwLowDateTime  != g_iniMtime.dwLowDateTime ||
                ad.ftLastWriteTime.dwHighDateTime != g_iniMtime.dwHighDateTime) {
                g_iniMtime = ad.ftLastWriteTime;
                LoadConfig();
                SRLog(L"配置已重载 img=%d 挡广播=%d 挡消息=%d 挡命令=%d",
                      g_cfg.enableImage, g_cfg.blockBroadcast, g_cfg.blockMsg, g_cfg.blockCmd);
            }
        } else {
            LoadConfig();
        }

        // 图片
        if (g_cfg.enableImage && !g_cfg.imagePath.empty()) {
            if (g_loadedImg != g_cfg.imagePath) {
                if (LoadImageInto(g_img, g_cfg.imagePath.c_str())) g_loadedImg = g_cfg.imagePath;
            }
        } else if (!g_loadedImg.empty()) {
            StoreClear(g_img);
            g_loadedImg.clear();
        }

        // 广播处理：手动取消（一次性）优先
        bool cancelNow = GetPrivateProfileIntW(L"ScreenReplace", L"CancelBroadcast", 0, IniPath().c_str()) != 0;
        if (cancelNow || g_cfg.blockBroadcast || g_cancelledWnd) {
            g_lastSeenGbWnd = NULL;
            EnumWindows(EnumBroadcastProc, 0);
            if (cancelNow) {
                if (g_lastSeenGbWnd) {
                    g_cancelledWnd = g_lastSeenGbWnd;
                    ShowWindow(g_cancelledWnd, SW_MINIMIZE);
                    SRLog(L"已取消全屏广播窗口 0x%08X", (unsigned)(ULONG_PTR)g_cancelledWnd);
                }
                WritePrivateProfileStringW(L"ScreenReplace", L"CancelBroadcast", L"0", IniPath().c_str());
            }
            if (g_cancelledWnd && !IsWindow(g_cancelledWnd)) g_cancelledWnd = NULL;   // 广播结束，解除
        }

        EnsureRelevantHooks();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// DllMain
// ---------------------------------------------------------------------------
static void Lower(std::wstring& s) { for (size_t i = 0; i < s.size(); i++) s[i] = towlower(s[i]); }

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);

        wchar_t path[MAX_PATH];
        GetModuleFileNameW(h, path, MAX_PATH);
        g_dir = path;
        size_t pos = g_dir.find_last_of(L'\\');
        if (pos != std::wstring::npos) g_dir = g_dir.substr(0, pos + 1);

        wchar_t self[MAX_PATH];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        std::wstring name = PathFindFileNameW(self);
        Lower(name);
        g_isStudentMain   = (name.find(L"studentmain.exe") != std::wstring::npos);
        g_isCaptureHelper = (name.find(L"dispcaphelper.exe") != std::wstring::npos);

        LoadConfig();
        SRLog(L"=== DLL v2 加载于 %s (StudentMain=%d CaptureHelper=%d)",
              name.c_str(), g_isStudentMain, g_isCaptureHelper);

        g_watchdog = CreateThread(NULL, 0, Watchdog, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        UninstallAll();
    }
    return TRUE;
}
