// SPDX-License-Identifier: MIT
// =============================================================================
//  xways-json-viewer — a *Viewer* X-Tension for X-Ways Forensics 21.7+ (C++/x64)
//
//  A live JMESPath query box over JSON / JSONL files, hosted inside the
//  Preview pane. Returned HTML cannot run script (verified 2026-08-22), so the
//  UI is an Edge WebView2 control parented onto the viewer component's preview
//  window (XWF_GetWindow(0,6), class "SCCVIEWER"); XT_View returns a 1-byte NUL
//  buffer so the stock viewer draws nothing. The page (ui/viewer.html) and the
//  JMESPath engine (ui/jmespath.js, Apache-2.0) are embedded as RCDATA.
//
//  What testing against 21.8 SR-5 established (2026-08-23; see README "How it works"):
//    * index 6 is NULL on the first call of a session and after mode switches
//      (Gallery/Details); X-Ways destroys and recreates that window at will, so
//      every call re-reads it and IsWindow()s the cached one;
//    * when index 6 is NULL the returned buffer is shown as plain TEXT (HTML is
//      not rendered) — so the no-pane fallback is a pretty-printed text dump;
//    * XT_ReleaseMem fires on the *next* selection, not right after the call;
//    * X-Ways hides our child itself when it previews something else, but we
//      hide explicitly on decline anyway;
//    * case reports do not call XT_View (default report settings).
//
//  Official API reference: https://www.x-ways.net/forensics/x-tensions/api.html
//  Viewer entry points:    https://www.x-ways.net/forensics/x-tensions/XT_functions.html#viewer
// =============================================================================

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <wrl.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include "WebView2.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

// --- Identity ---------------------------------------------------------------
static const wchar_t* NAME         = L"xways-json-viewer";
static const wchar_t* VERSION      = L"0.1.0-beta";
static const wchar_t* DESCRIPTION  = L"Viewer X-Tension: live JMESPath query over JSON/JSONL in the preview pane";
static const wchar_t* REPORT_TABLE = L"Json-Viewer Findings";   // unused by a viewer; kept for convention

// --- Logging verbosity ------------------------------------------------------
static constexpr bool VERBOSE = true;
// One-shot diagnostics that dump the pane's child windows per call — off now that the
// z-order behaviour is understood (see README "How it works").
static constexpr bool kDumpChildren = false;

// --- Limits -----------------------------------------------------------------
// XT_View runs synchronously on X-Ways' UI thread: bound the work.
static constexpr INT64 kDefaultCapBytes = 8 * 1024 * 1024;    // render cap default
static constexpr INT64 kMaxCapBytes     = 512LL * 1024 * 1024; // hard ceiling for the cfg value
static INT64 g_capBytes = kDefaultCapBytes;                    // adjustable in the page, persisted
static constexpr UINT  kRetryTimerId   = 1;
static constexpr UINT  kBeatTimerId    = 2;
static constexpr UINT  kBeatPeriodMs   = 250;   // keeps bounds + z-order right: SCCVIEWER forwards no WM_SIZE
static constexpr size_t kHistoryMax    = 50;
static constexpr UINT  kRetryPeriodMs  = 100;
static constexpr int   kRetryMaxTicks  = 20;     // 2 s of polling for a late pane
// When X-Ways has no viewer window at call time: return a text dump (true) or the NUL
// buffer and wait for the late pane (false). 2026-08-23: the text dump leaves X-Ways in its
// text view, which then hides the hosted WebView on the next call — so default to false.
static constexpr bool  kTextFallback   = false;

// --- Function-pointer typedefs (names/types mirror X-Tension.h) --------------
typedef VOID   (__stdcall *pfn_XWF_OutputMessage)(const wchar_t* msg, DWORD nFlags);
typedef const wchar_t* (__stdcall *pfn_XWF_GetItemName)(LONG nItemID);
typedef INT64  (__stdcall *pfn_XWF_GetItemSize)(LONG nItemID);
typedef INT64  (__stdcall *pfn_XWF_GetSize)(HANDLE hVolumeOrItem, LPVOID lpOptional);
typedef DWORD  (__stdcall *pfn_XWF_Read)(HANDLE hVolumeOrItem, INT64 nOffset, BYTE* lpBuffer, DWORD nNumberOfBytesToRead);
typedef LONG   (__stdcall *pfn_XWF_GetItemType)(LONG nItemID, wchar_t* lpTypeDescr, DWORD nBufferLenAndFlags);
typedef HWND   (__stdcall *pfn_XWF_GetWindow)(WORD nWndNo, WORD nWndIndex);

static pfn_XWF_OutputMessage XWF_OutputMessage = nullptr;
static pfn_XWF_GetItemName   XWF_GetItemName   = nullptr;
static pfn_XWF_GetItemSize   XWF_GetItemSize   = nullptr;
static pfn_XWF_GetSize       XWF_GetSize       = nullptr;
static pfn_XWF_Read          XWF_Read          = nullptr;
static pfn_XWF_GetItemType   XWF_GetItemType   = nullptr;
static pfn_XWF_GetWindow     XWF_GetWindow     = nullptr;

// XWF_GetItemType: low WORD = buffer length in wchar_t, bit 29 = type description.
static constexpr DWORD kItemTypeDescr = 0x20000000;

// --- State ------------------------------------------------------------------
static HMODULE g_hSelf     = nullptr;
static HWND    g_hMainWnd  = nullptr;
static std::atomic<long> g_viewCalls{0};
static std::atomic<long> g_releaseCalls{0};
static bool    g_comInitedHere = false;

// WebView2 host
static ComPtr<ICoreWebView2Environment> g_env;
static ComPtr<ICoreWebView2Controller>  g_ctrl;
static ComPtr<ICoreWebView2>            g_web;
static HWND         g_hPane        = nullptr;   // pane the controller is parented to
static bool         g_envCreating  = false;
static bool         g_ctrlCreating = false;
static bool         g_pageReady    = false;     // page posted "ready"
static std::wstring g_pendingEnvelope;          // document waiting for the page
static std::wstring g_pageHtml;                 // assembled once from resources
struct HistEntry { std::wstring file, query; };
static std::vector<HistEntry> g_history;        // most recent first, persisted in cfg
static RECT         g_lastFit      = {0};
static HWND         g_hMsgWnd      = nullptr;   // message-only window for the retry timer
static int          g_retryTicks   = 0;
static bool         g_itemLive     = false;     // our last claimed buffer not yet released
static bool         g_hidden       = true;

// --- Logging ----------------------------------------------------------------
static void Log(const std::wstring& msg) {
    std::wstring line = L"["; line += NAME; line += L"] "; line += msg;
    if (XWF_OutputMessage) XWF_OutputMessage(line.c_str(), 0);
}
static void Logf(const wchar_t* fmt, ...) {
    wchar_t b[1024];
    va_list ap; va_start(ap, fmt); _vsnwprintf_s(b, _TRUNCATE, fmt, ap); va_end(ap);
    Log(b);
}
static void LogVerbosef(const wchar_t* fmt, ...) {
    if (!VERBOSE) return;
    wchar_t b[1024];
    va_list ap; va_start(ap, fmt); _vsnwprintf_s(b, _TRUNCATE, fmt, ap); va_end(ap);
    Log(b);
}

template <typename T>
static T Resolve(HMODULE h, const char* name, int& missing) {
    T p = reinterpret_cast<T>(GetProcAddress(h, name));
    if (!p) ++missing;
    return p;
}

static int RetrieveFunctionPointers() {
    HMODULE h = GetModuleHandleW(nullptr);
    int missing = 0;
    XWF_OutputMessage = Resolve<pfn_XWF_OutputMessage>(h, "XWF_OutputMessage", missing);
    XWF_GetItemName   = Resolve<pfn_XWF_GetItemName  >(h, "XWF_GetItemName",   missing);
    XWF_GetItemSize   = Resolve<pfn_XWF_GetItemSize  >(h, "XWF_GetItemSize",   missing);
    XWF_GetSize       = Resolve<pfn_XWF_GetSize      >(h, "XWF_GetSize",       missing);
    XWF_Read          = Resolve<pfn_XWF_Read         >(h, "XWF_Read",          missing);
    XWF_GetItemType   = Resolve<pfn_XWF_GetItemType  >(h, "XWF_GetItemType",   missing);
    XWF_GetWindow     = Resolve<pfn_XWF_GetWindow    >(h, "XWF_GetWindow",     missing);
    return missing;
}

// =============================================================================
//  Small utilities
// =============================================================================
static std::wstring ToLower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

static std::wstring DescribePane(HWND h) {
    if (!h || !IsWindow(h)) return L"NULL";
    RECT rc = {0}; GetWindowRect(h, &rc);
    wchar_t b[160];
    swprintf_s(b, L"0x%p vis=%d rect=%ldx%ld@%ld,%ld", (void*)h, IsWindowVisible(h) ? 1 : 0, rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top);
    return b;
}

static std::wstring DllDir() {
    wchar_t p[MAX_PATH] = {0};
    GetModuleFileNameW(g_hSelf, p, MAX_PATH);
    std::wstring s = p;
    size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}

static std::wstring CfgPath() { return DllDir() + L"\\" + NAME + L".cfg"; }

static std::wstring Utf8ToW(const char* p, int n) {
    if (n <= 0) return {};
    int w = MultiByteToWideChar(CP_UTF8, 0, p, n, nullptr, 0);
    std::wstring s(w, L'\0');
    if (w) MultiByteToWideChar(CP_UTF8, 0, p, n, &s[0], w);
    return s;
}
static std::string WToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &o[0], n, nullptr, nullptr);
    return o;
}

static void LoadCfg() {
    HANDLE h = CreateFileW(CfgPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD sz = GetFileSize(h, nullptr), got = 0;
    std::string raw(sz, '\0');
    if (sz) ReadFile(h, &raw[0], sz, &got, nullptr);
    CloseHandle(h);
    std::wstring text = Utf8ToW(raw.data(), (int)got);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos);
        pos = e == std::wstring::npos ? text.size() : e + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (line.rfind(L"cap_bytes=", 0) == 0) {
            INT64 v = _wtoi64(line.c_str() + 10);
            if (v >= 64 * 1024 && v <= kMaxCapBytes) g_capBytes = v;
        }
        else if (line.rfind(L"history=", 0) == 0 && g_history.size() < kHistoryMax) {
            std::wstring rest = line.substr(8);
            size_t t = rest.find(L'\t');
            if (t != std::wstring::npos) g_history.push_back({rest.substr(0, t), rest.substr(t + 1)});
            else g_history.push_back({L"", rest});   // pre-filename format
        }
    }
}

static void SaveCfg() {
    std::wstring cfgText = L"# xways-json-viewer settings (auto-written; history=<file>\\t<query>)\r\n";
    cfgText += L"cap_bytes=" + std::to_wstring(g_capBytes) + L"\r\n";
    for (auto& h : g_history) cfgText += L"history=" + h.file + L"\t" + h.query + L"\r\n";
    std::string body = WToUtf8(cfgText);
    HANDLE h = CreateFileW(CfgPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Logf(L"could not write %s (gle=%lu)", CfgPath().c_str(), GetLastError()); return; }
    DWORD w = 0; WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr);
    CloseHandle(h);
    LogVerbosef(L"cfg saved to %s", CfgPath().c_str());
}

static std::wstring LoadResourceText(int id) {
    HRSRC r = FindResourceW(g_hSelf, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!r) return {};
    HGLOBAL g = LoadResource(g_hSelf, r);
    if (!g) return {};
    const char* p = (const char*)LockResource(g);
    DWORD n = SizeofResource(g_hSelf, r);
    if (n >= 3 && (BYTE)p[0] == 0xEF && (BYTE)p[1] == 0xBB && (BYTE)p[2] == 0xBF) { p += 3; n -= 3; }
    return Utf8ToW(p, (int)n);
}

// JSON string escaping for the envelope (UTF-16 in → JSON text out).
static void AppendJsonString(std::wstring& o, const std::wstring& s) {
    o += L'"';
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  o += L"\\\""; break;
            case L'\\': o += L"\\\\"; break;
            case L'\n': o += L"\\n";  break;
            case L'\r': o += L"\\r";  break;
            case L'\t': o += L"\\t";  break;
            default:
                if (c < 0x20 || c == 0x2028 || c == 0x2029) {
                    wchar_t b[8]; swprintf_s(b, L"\\u%04x", (unsigned)c); o += b;
                } else o += c;
        }
    }
    o += L'"';
}

// =============================================================================
//  Type gate — is this file ours?
// =============================================================================
static bool HasJsonExtension(const std::wstring& lowerName, bool* jsonl) {
    static const wchar_t* kJsonl[] = { L".jsonl", L".ndjson", L".jsonlines" };
    static const wchar_t* kExts[]  = {
        L".json", L".geojson", L".har", L".ipynb", L".jsonc", L".webmanifest",
        L".json5", L".topojson", L".jsonld", L".avsc",
    };
    auto ends = [&](const wchar_t* e) {
        size_t n = wcslen(e);
        return lowerName.size() >= n && lowerName.compare(lowerName.size() - n, n, e) == 0;
    };
    for (auto e : kJsonl) if (ends(e)) { *jsonl = true; return true; }
    for (auto e : kExts)  if (ends(e)) return true;
    return false;
}

// Content sniff: after an optional UTF-8/UTF-16 BOM and whitespace, JSON starts with { or [.
static bool SniffLooksJson(HANDLE hItem) {
    if (!XWF_Read) return false;
    BYTE head[64] = {0};
    DWORD got = XWF_Read(hItem, 0, head, sizeof(head));
    if (got == 0) return false;
    size_t i = 0; bool utf16le = false;
    if (got >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) i = 3;
    else if (got >= 2 && head[0] == 0xFF && head[1] == 0xFE) { i = 2; utf16le = true; }
    for (; i < got; i += (utf16le ? 2 : 1)) {
        BYTE c = head[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        return c == '{' || c == '[';
    }
    return false;
}

// Type description contains "json", or a JSON-ish extension, or the bytes look like JSON
// on a text-typed item. (Observed: the type string alone is not a reliable gate —
// .jsonl reports "7-ASCII, Unix LF".)
static bool IsJsonItem(HANDLE hItem, LONG nItemID, std::wstring& typeDescrOut, const wchar_t** how, bool* jsonl) {
    wchar_t descr[256] = {0};
    if (XWF_GetItemType) XWF_GetItemType(nItemID, descr, kItemTypeDescr | (DWORD)(sizeof(descr) / sizeof(descr[0])));
    typeDescrOut = descr;
    *jsonl = false;
    const wchar_t* nm = XWF_GetItemName ? XWF_GetItemName(nItemID) : nullptr;
    if (nm && HasJsonExtension(ToLower(nm), jsonl)) { *how = L"ext"; return true; }
    std::wstring ld = ToLower(typeDescrOut);
    if (ld.find(L"json") != std::wstring::npos) { *how = L"type"; return true; }
    if (ld.empty() || ld.find(L"text") != std::wstring::npos || ld.find(L"ascii") != std::wstring::npos ||
        ld.find(L"unicode") != std::wstring::npos || ld.find(L"unknown") != std::wstring::npos) {
        if (SniffLooksJson(hItem)) { *how = L"sniff"; return true; }
    }
    *how = L"";
    return false;
}

// =============================================================================
//  Decode + fallback pretty-printer
// =============================================================================
struct Decoded { std::wstring text; std::wstring encoding; };

static Decoded DecodeBytes(const std::vector<BYTE>& raw) {
    Decoded d;
    const BYTE* p = raw.data(); size_t n = raw.size();
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        d.encoding = L"UTF-16 LE (BOM)";
        d.text.assign((const wchar_t*)(p + 2), (n - 2) / 2);
        return d;
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        d.encoding = L"UTF-16 BE (BOM)";
        d.text.resize((n - 2) / 2);
        for (size_t i = 0; i < d.text.size(); ++i) d.text[i] = (wchar_t)((p[2 + 2 * i] << 8) | p[3 + 2 * i]);
        return d;
    }
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; d.encoding = L"UTF-8 (BOM)"; }
    // Strict UTF-8 first; fall back to Windows-1252 so nothing is ever unreadable.
    int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)p, (int)n, nullptr, 0);
    if (w > 0 || n == 0) {
        if (d.encoding.empty()) d.encoding = L"UTF-8";
        d.text.resize(w);
        if (w) MultiByteToWideChar(CP_UTF8, 0, (const char*)p, (int)n, &d.text[0], w);
    } else {
        d.encoding = L"Windows-1252 (invalid UTF-8)";
        w = MultiByteToWideChar(1252, 0, (const char*)p, (int)n, nullptr, 0);
        d.text.resize(w);
        if (w) MultiByteToWideChar(1252, 0, (const char*)p, (int)n, &d.text[0], w);
    }
    return d;
}

// Tolerant pretty-printer for the no-pane fallback: walks tokens, never fails on bad input.
static std::wstring PrettyPrint(const std::wstring& s) {
    std::wstring o; o.reserve(s.size() + s.size() / 4);
    int depth = 0; bool inStr = false, esc = false;
    auto nl = [&]() { o += L"\r\n"; for (int i = 0; i < depth; ++i) o += L"  "; };
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (inStr) {
            o += c;
            if (esc) esc = false; else if (c == L'\\') esc = true; else if (c == L'"') inStr = false;
            continue;
        }
        switch (c) {
            case L'"': inStr = true; o += c; break;
            case L'{': case L'[': {
                o += c;
                size_t j = i + 1; while (j < s.size() && iswspace(s[j])) ++j;
                if (j < s.size() && (s[j] == L'}' || s[j] == L']')) { o += s[j]; i = j; }
                else { ++depth; nl(); }
                break;
            }
            case L'}': case L']': if (depth > 0) --depth; nl(); o += c; break;
            case L',': o += c; nl(); break;
            case L':': o += L": "; break;
            case L' ': case L'\t': case L'\r': case L'\n': break;
            default: o += c;
        }
    }
    return o;
}

// =============================================================================
//  WebView2 host
// =============================================================================
static void ShowWeb(bool show) {
    if (g_ctrl && g_hidden == show) { g_ctrl->put_IsVisible(show ? TRUE : FALSE); g_hidden = !show; }
}

static void FitToPane() {
    if (!g_ctrl || !g_hPane || !IsWindow(g_hPane)) return;
    RECT rc = {0}; GetClientRect(g_hPane, &rc);
    g_ctrl->put_Bounds(rc);
    g_lastFit = rc;
}

// Outside In's SCCDISPLAY child is created after ours and lands above it in z-order,
// painting its empty grey over the WebView (seen 2026-08-23). Put our HWND back on top.
static void RaiseWeb() {
    if (!g_hPane || !IsWindow(g_hPane)) return;
    HWND h = FindWindowExW(g_hPane, nullptr, L"Chrome_WidgetWin_0", nullptr);
    if (h) SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static BOOL CALLBACK LogChildProc(HWND h, LPARAM) {
    wchar_t cls[64] = {0}; GetClassNameW(h, cls, 64);
    RECT rc = {0}; GetWindowRect(h, &rc);
    LogVerbosef(L"    child 0x%p \"%s\" vis=%d %ldx%ld@%ld,%ld", (void*)h, cls, IsWindowVisible(h) ? 1 : 0,
                rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top);
    return TRUE;
}

static void LogHostState(const wchar_t* when) {
    if (!VERBOSE) return;
    RECT b = {0}; if (g_ctrl) g_ctrl->get_Bounds(&b);
    BOOL vis = FALSE; if (g_ctrl) g_ctrl->get_IsVisible(&vis);
    LogVerbosef(L"  [%s] pane %s; ctrl bounds=%ldx%ld@%ld,%ld visible=%d", when, DescribePane(g_hPane).c_str(),
                b.right - b.left, b.bottom - b.top, b.left, b.top, vis ? 1 : 0);
    if (kDumpChildren && g_hPane && IsWindow(g_hPane)) EnumChildWindows(g_hPane, LogChildProc, 0);
}

static void PostEnvelopeIfReady() {
    if (g_web && g_pageReady && !g_pendingEnvelope.empty()) {
        HRESULT hr = g_web->PostWebMessageAsString(g_pendingEnvelope.c_str());
        LogVerbosef(L"  envelope posted (%llu chars) hr=0x%08lX", (unsigned long long)g_pendingEnvelope.size(), hr);
        g_pendingEnvelope.clear();
        FitToPane();
        RaiseWeb();
        LogHostState(L"after envelope");
    }
}

static LRESULT CALLBACK PaneSubclassProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_SIZE || m == WM_WINDOWPOSCHANGED || m == WM_SHOWWINDOW) { FitToPane(); RaiseWeb(); }
    else if (m == WM_NCDESTROY) RemoveWindowSubclass(h, PaneSubclassProc, 1);
    return DefSubclassProc(h, m, w, l);
}

static void DropController(const wchar_t* why) {
    if (g_ctrl) {
        LogVerbosef(L"  closing WebView2 controller (%s)", why);
        g_web.Reset();
        g_ctrl->Close();
        g_ctrl.Reset();
    }
    if (g_hPane && IsWindow(g_hPane)) RemoveWindowSubclass(g_hPane, PaneSubclassProc, 1);
    g_hPane = nullptr; g_pageReady = false; g_hidden = true;
}

static void AddHistory(const std::wstring& file, const std::wstring& q) {
    if (q.empty()) return;
    for (auto it = g_history.begin(); it != g_history.end(); ++it)
        if (it->query == q && it->file == file) { g_history.erase(it); break; }
    g_history.insert(g_history.begin(), {file, q});
    if (g_history.size() > kHistoryMax) g_history.resize(kHistoryMax);
    SaveCfg();
}

static void OnWebMessage(const std::wstring& msg) {
    size_t t = msg.find(L'\t');
    std::wstring kind = msg.substr(0, t), body = t == std::wstring::npos ? L"" : msg.substr(t + 1);
    if (kind == L"ready") { g_pageReady = true; LogVerbosef(L"  page ready"); PostEnvelopeIfReady(); }
    else if (kind == L"hist") {
        size_t t2 = body.find(L'\t');
        if (t2 != std::wstring::npos) AddHistory(body.substr(0, t2), body.substr(t2 + 1));
    }
    else if (kind == L"cap") {
        INT64 v = _wtoi64(body.c_str());
        if (v >= 64 * 1024 && v <= kMaxCapBytes) { g_capBytes = v; SaveCfg(); Logf(L"render cap set to %lld bytes (applies on the next selection)", (long long)g_capBytes); }
    }
    else if (kind == L"log") { LogVerbosef(L"  [page] %s", body.c_str()); }
}

static void CreateController(HWND pane);
static void StartBeat();

static void EnsureEnvironment(HWND pane) {
    if (g_env) { CreateController(pane); return; }
    if (g_envCreating) return;
    g_envCreating = true;
    wchar_t local[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
    std::wstring udf = std::wstring(local) + L"\\" + NAME + L"\\WebView2";
    SHCreateDirectoryExW(nullptr, udf.c_str(), nullptr);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [pane](HRESULT res, ICoreWebView2Environment* env) -> HRESULT {
                g_envCreating = false;
                if (FAILED(res) || !env) { Logf(L"WebView2 environment creation failed hr=0x%08lX (is the WebView2 Runtime installed?)", res); return S_OK; }
                g_env = env;
                LPWSTR ver = nullptr;
                if (SUCCEEDED(env->get_BrowserVersionString(&ver)) && ver) { LogVerbosef(L"  WebView2 environment ready, runtime %s", ver); CoTaskMemFree(ver); }
                if (IsWindow(pane)) CreateController(pane);
                return S_OK;
            }).Get());
    if (FAILED(hr)) { g_envCreating = false; Logf(L"CreateCoreWebView2EnvironmentWithOptions failed hr=0x%08lX", hr); }
}

static void CreateController(HWND pane) {
    if (!g_env || g_ctrlCreating || !IsWindow(pane)) return;
    g_ctrlCreating = true;
    g_hPane = pane;
    SetWindowSubclass(pane, PaneSubclassProc, 1, 0);
    HRESULT hr = g_env->CreateCoreWebView2Controller(pane,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [pane](HRESULT res, ICoreWebView2Controller* ctrl) -> HRESULT {
                g_ctrlCreating = false;
                if (FAILED(res) || !ctrl) { Logf(L"WebView2 controller creation failed hr=0x%08lX", res); return S_OK; }
                if (pane != g_hPane || !IsWindow(pane)) { ctrl->Close(); LogVerbosef(L"  controller arrived for a stale pane; discarded"); return S_OK; }
                g_ctrl = ctrl;
                g_ctrl->get_CoreWebView2(&g_web);
                ComPtr<ICoreWebView2Settings> st;
                if (g_web && SUCCEEDED(g_web->get_Settings(&st)) && st) {
                    st->put_AreDefaultContextMenusEnabled(TRUE);   // filtered below via ContextMenuRequested
                    st->put_IsStatusBarEnabled(FALSE);
                    st->put_AreDevToolsEnabled(VERBOSE ? TRUE : FALSE);
                    st->put_IsZoomControlEnabled(TRUE);
                    // Microsoft WebView2 security guidance (see README "Security posture"): switch off
                    // everything the page doesn't need.
                    st->put_AreHostObjectsAllowed(FALSE);
                    st->put_AreDefaultScriptDialogsEnabled(FALSE);
                    ComPtr<ICoreWebView2Settings4> st4;
                    if (SUCCEEDED(st.As(&st4)) && st4) {
                        st4->put_IsPasswordAutosaveEnabled(FALSE);
                        st4->put_IsGeneralAutofillEnabled(FALSE);
                    }
                }
                {
                    ComPtr<ICoreWebView2Controller4> c4;
                    if (SUCCEEDED(g_ctrl.As(&c4)) && c4) c4->put_AllowExternalDrop(FALSE);
                }
                EventRegistrationToken tok;
                // The page is NavigateToString-only (origin about:blank). Cancel any other
                // navigation and every popup — evidence content must never take the pane
                // somewhere else.
                g_web->add_NavigationStarting(
                    Callback<ICoreWebView2NavigationStartingEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                            LPWSTR uri = nullptr; a->get_Uri(&uri);
                            const bool ok = uri && (wcscmp(uri, L"about:blank") == 0 || wcsncmp(uri, L"data:", 5) == 0);
                            if (!ok) {
                                a->put_Cancel(TRUE);
                                Logf(L"blocked navigation to %s", uri ? uri : L"<null>");
                            }
                            if (uri) CoTaskMemFree(uri);
                            return S_OK;
                        }).Get(), &tok);
                g_web->add_NewWindowRequested(
                    Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                            a->put_Handled(TRUE);   // no popups / external windows
                            return S_OK;
                        }).Get(), &tok);
                g_web->add_ProcessFailed(
                    Callback<ICoreWebView2ProcessFailedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* a) -> HRESULT {
                            COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                            a->get_ProcessFailedKind(&kind);
                            Logf(L"WebView2 process failed (kind=%d) — dropping controller; next preview recreates it", (int)kind);
                            DropController(L"process failed");
                            return S_OK;
                        }).Get(), &tok);
                // Context menu: allowlist the items that make sense in an evidence viewer.
                {
                    ComPtr<ICoreWebView2_11> web11;
                    if (SUCCEEDED(g_web.As(&web11)) && web11) {
                        web11->add_ContextMenuRequested(
                            Callback<ICoreWebView2ContextMenuRequestedEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2ContextMenuRequestedEventArgs* args) -> HRESULT {
                                    ComPtr<ICoreWebView2ContextMenuItemCollection> items;
                                    if (FAILED(args->get_MenuItems(&items)) || !items) return S_OK;
                                    UINT n = 0; items->get_Count(&n);
                                    for (INT i = (INT)n - 1; i >= 0; --i) {
                                        ComPtr<ICoreWebView2ContextMenuItem> it;
                                        if (FAILED(items->GetValueAtIndex((UINT)i, &it)) || !it) continue;
                                        LPWSTR nm = nullptr;
                                        if (FAILED(it->get_Name(&nm)) || !nm) continue;
                                        const bool keep =
                                            wcscmp(nm, L"copy") == 0 || wcscmp(nm, L"cut") == 0 ||
                                            wcscmp(nm, L"paste") == 0 || wcscmp(nm, L"selectAll") == 0 ||
                                            wcscmp(nm, L"print") == 0 ||
                                            (VERBOSE && wcscmp(nm, L"inspectElement") == 0);
                                        CoTaskMemFree(nm);
                                        if (!keep) items->RemoveValueAtIndex((UINT)i);
                                    }
                                    return S_OK;
                                }).Get(), &tok);
                    }
                }
                g_web->add_WebMessageReceived(
                    Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                            LPWSTR s = nullptr;
                            if (SUCCEEDED(a->TryGetWebMessageAsString(&s)) && s) { OnWebMessage(s); CoTaskMemFree(s); }
                            return S_OK;
                        }).Get(), &tok);
                g_hidden = true;
                FitToPane();
                ShowWeb(true);
                StartBeat();
                g_pageReady = false;
                HRESULT nh = g_web->NavigateToString(g_pageHtml.c_str());
                LogVerbosef(L"  WebView2 controller ready on pane %s; NavigateToString hr=0x%08lX", DescribePane(pane).c_str(), nh);
                return S_OK;
            }).Get());
    if (FAILED(hr)) { g_ctrlCreating = false; Logf(L"CreateCoreWebView2Controller failed hr=0x%08lX", hr); }
}

// Attach (or re-attach) to the pane and hand over the current document.
static void AttachAndShow(HWND pane) {
    if (g_hPane && (g_hPane != pane || !IsWindow(g_hPane))) DropController(g_hPane != pane ? L"pane changed" : L"pane destroyed");
    if (g_ctrl) { FitToPane(); ShowWeb(true); RaiseWeb(); PostEnvelopeIfReady(); return; }
    EnsureEnvironment(pane);
}

// Retry timer: the pane often appears a moment after XT_View returned (first call of a
// session, after a mode switch). Poll briefly while our item is still the current one.
static void CALLBACK RetryTimerProc(HWND, UINT, UINT_PTR, DWORD) {
    if (!g_itemLive || g_pendingEnvelope.empty() || ++g_retryTicks > kRetryMaxTicks) { KillTimer(g_hMsgWnd, kRetryTimerId); return; }
    HWND pane = XWF_GetWindow ? XWF_GetWindow(0, 6) : nullptr;
    if (pane && IsWindow(pane)) {
        KillTimer(g_hMsgWnd, kRetryTimerId);
        LogVerbosef(L"  late pane %s appeared after %d tick(s) — attaching", DescribePane(pane).c_str(), g_retryTicks);
        AttachAndShow(pane);
    }
}

static void CALLBACK BeatTimerProc(HWND, UINT, UINT_PTR, DWORD) {
    if (!g_ctrl) { KillTimer(g_hMsgWnd, kBeatTimerId); return; }
    if (!g_hPane || !IsWindow(g_hPane)) { DropController(L"pane destroyed (heartbeat)"); KillTimer(g_hMsgWnd, kBeatTimerId); return; }
    if (g_hidden) return;
    RECT rc = {0}; GetClientRect(g_hPane, &rc);
    if (rc.right != g_lastFit.right || rc.bottom != g_lastFit.bottom) FitToPane();
    HWND top = GetWindow(g_hPane, GW_CHILD);
    wchar_t cls[32] = {0}; if (top) GetClassNameW(top, cls, 32);
    if (wcscmp(cls, L"Chrome_WidgetWin_0") != 0) RaiseWeb();
}

static void StartBeat() { if (g_hMsgWnd) SetTimer(g_hMsgWnd, kBeatTimerId, kBeatPeriodMs, BeatTimerProc); }

static void StartRetry() {
    if (!g_hMsgWnd) return;
    g_retryTicks = 0;
    SetTimer(g_hMsgWnd, kRetryTimerId, kRetryPeriodMs, RetryTimerProc);
}

static std::wstring BuildEnvelope(LONG nItemID, const std::wstring& name, const std::wstring& typeDescr,
                                  INT64 size, const Decoded& d, bool truncated, bool jsonl) {
    std::wstring e; e.reserve(d.text.size() + 512);
    e += L"{\"name\":"; AppendJsonString(e, name);
    e += L",\"itemId\":" + std::to_wstring(nItemID);
    e += L",\"size\":" + std::to_wstring(size);
    e += L",\"maxBytes\":" + std::to_wstring(g_capBytes);
    e += L",\"typeDescr\":"; AppendJsonString(e, typeDescr);
    e += L",\"encoding\":"; AppendJsonString(e, d.encoding);
    e += truncated ? L",\"truncated\":true" : L",\"truncated\":false";
    e += jsonl ? L",\"isJsonl\":true" : L",\"isJsonl\":false";
    e += L",\"history\":[";
    for (size_t i = 0; i < g_history.size(); ++i) {
        if (i) e += L",";
        e += L"{\"f\":"; AppendJsonString(e, g_history[i].file);
        e += L",\"q\":"; AppendJsonString(e, g_history[i].query);
        e += L"}";
    }
    e += L"]";
    e += L",\"text\":"; AppendJsonString(e, d.text);
    e += L"}";
    return e;
}

static PVOID ReturnText(const std::wstring& text, PINT64 nResSize) {
    const size_t bytes = 2 + text.size() * sizeof(wchar_t);
    BYTE* buf = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, bytes));
    if (!buf) { *nResSize = -2; return nullptr; }
    buf[0] = 0xFF; buf[1] = 0xFE;
    memcpy(buf + 2, text.data(), text.size() * sizeof(wchar_t));
    *nResSize = (INT64)bytes;
    return buf;
}

// =============================================================================
//  Entry points (exported via xways-json-viewer.def)
// =============================================================================
extern "C" {

LONG __stdcall XT_Init(DWORD nVersion, DWORD nFlags, HWND hMainWnd, void* lpReserved) {
    int missing = RetrieveFunctionPointers();
    if (nFlags & 0x20) return (missing > 0) ? -1 : 1;   // XT_INIT_QUICKCHECK
    g_hMainWnd = hMainWnd;
    const DWORD ver = (nVersion >> 16) & 0xFFFF;
    const DWORD sr  = (nVersion >>  8) & 0xFF;
    Logf(L"%s — X-Ways v%lu.%lu SR-%lu (%d missing exports) — loaded; XT_Init flags=0x%02lX",
         VERSION, (unsigned long)(ver / 100), (unsigned long)((ver % 100) / 10), (unsigned long)sr, missing, (unsigned long)nFlags);
    if (nFlags & 0x40) return (missing > 0) ? -1 : 1;   // XT_INIT_ABOUTONLY: no UI setup

    HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_comInitedHere = (ci == S_OK);
    if (ci == RPC_E_CHANGED_MODE) Logf(L"COM already initialised MTA on this thread — WebView2 needs STA; hosting may fail");

    // Assemble the page once: inline the JMESPath engine where the placeholder sits.
    g_pageHtml = LoadResourceText(IDR_VIEWER_HTML);
    std::wstring js = LoadResourceText(IDR_JMESPATH_JS);
    size_t k = g_pageHtml.find(L"/*JMESPATH*/");
    if (k != std::wstring::npos) g_pageHtml.replace(k, 12, js);
    if (g_pageHtml.empty() || js.empty()) Logf(L"embedded resources missing (html=%llu js=%llu chars)", (unsigned long long)g_pageHtml.size(), (unsigned long long)js.size());

    {
        LPWSTR rv = nullptr;
        if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &rv)) && rv) {
            LogVerbosef(L"WebView2 Runtime available: %s", rv);
            CoTaskMemFree(rv);
        } else {
            Log(L"WebView2 Runtime NOT found — previews will stay empty. Install the Evergreen WebView2 Runtime from Microsoft.");
        }
    }
    g_hMsgWnd = CreateWindowExW(0, L"STATIC", L"xways-json-viewer-msg", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hSelf, nullptr);
    LoadCfg();
    if (!g_history.empty()) LogVerbosef(L"loaded %llu history entries", (unsigned long long)g_history.size());
    return (missing > 0) ? -1 : 1;
}

LONG __stdcall XT_About(HWND hParentWnd, void* lpReserved) {
    std::wstring msg = NAME; msg += L" "; msg += VERSION; msg += L"\n"; msg += DESCRIPTION;
    msg += L"\nRegister under Options | Viewer Programs | Load viewer X-Tensions. Needs the Edge WebView2 Runtime.";
    if (XWF_OutputMessage) XWF_OutputMessage(msg.c_str(), 0);
    return 0;
}

// *nResSize: -1 = not our file type, -2 = error, 0 = no data, >0 = byte length.
PVOID __stdcall XT_View(HANDLE hItem, LONG nItemID, HANDLE hVolume, HANDLE hEvidence, PVOID lpReserved, PINT64 nResSize) {
    const long callNo = ++g_viewCalls;
    std::wstring typeDescr;
    const wchar_t* how = L"";
    bool jsonl = false;
    const wchar_t* nmRaw = XWF_GetItemName ? XWF_GetItemName(nItemID) : nullptr;
    std::wstring name = nmRaw ? nmRaw : L"<unnamed>";

    if (!IsJsonItem(hItem, nItemID, typeDescr, &how, &jsonl)) {
        ShowWeb(false);
        g_itemLive = false;
        LogVerbosef(L"XT_View #%ld item=%ld \"%s\" type=\"%s\" -> not JSON, declining (-1)", callNo, nItemID, name.c_str(), typeDescr.c_str());
        *nResSize = -1;
        return nullptr;
    }

    INT64 size = 0;
    if (XWF_GetSize) size = XWF_GetSize(hItem, nullptr);
    if (size <= 0 && XWF_GetItemSize) size = XWF_GetItemSize(nItemID);
    if (size <= 0) { ShowWeb(false); *nResSize = 0; return nullptr; }

    const INT64 cap = g_capBytes;
    const bool truncated = size > cap;
    const DWORD toRead = (DWORD)(truncated ? cap : size);
    std::vector<BYTE> raw(toRead);
    DWORD got = XWF_Read ? XWF_Read(hItem, 0, raw.data(), toRead) : 0;
    if (got == 0) {
        Logf(L"XT_View #%ld item=%ld: XWF_Read returned 0 of %lu bytes", callNo, nItemID, (unsigned long)toRead);
        ShowWeb(false); *nResSize = -2; return nullptr;
    }
    raw.resize(got);
    Decoded d = DecodeBytes(raw);

    g_pendingEnvelope = BuildEnvelope(nItemID, name, typeDescr, size, d, truncated, jsonl);
    g_itemLive = true;

    HWND pane = XWF_GetWindow ? XWF_GetWindow(0, 6) : nullptr;
    LogVerbosef(L"XT_View #%ld item=%ld \"%s\" type=\"%s\" via=%s size=%lld enc=%s pane=0x%p%s",
                callNo, nItemID, name.c_str(), typeDescr.c_str(), how, (long long)size, d.encoding.c_str(), (void*)pane,
                truncated ? L" (truncated)" : L"");

    if (!pane || !IsWindow(pane)) {
        StartRetry();
        if (!kTextFallback) {
            // Return the NUL buffer anyway: X-Ways creates the viewer window ~100 ms later and
            // the retry timer parents the WebView onto it.
            BYTE* nb = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, 1));
            if (!nb) { *nResSize = -2; return nullptr; }
            nb[0] = 0; *nResSize = 1;
            return nb;
        }
        // Viewer component window not up: X-Ways shows our buffer as plain text.
        std::wstring text = L"[xways-json-viewer] static view — X-Ways had no viewer window for this call (first preview of a session, or the same file re-selected). Select another file, then come back for the live JMESPath view.\r\n\r\n";
        text += PrettyPrint(d.text);
        if (truncated) text += L"\r\n\r\n[truncated — only the first " + std::to_wstring(g_capBytes) + L" bytes shown]";
        StartRetry();
        return ReturnText(text, nResSize);
    }

    LogVerbosef(L"  hosted: pane %s, ctrl=%s", DescribePane(pane).c_str(), g_ctrl ? L"reused" : L"new");
    AttachAndShow(pane);

    // 1-byte NUL buffer: the stock viewer renders nothing; our WebView owns the pane.
    BYTE* buf = static_cast<BYTE*>(HeapAlloc(GetProcessHeap(), 0, 1));
    if (!buf) { *nResSize = -2; return nullptr; }
    buf[0] = 0;
    *nResSize = 1;
    return buf;
}

// Mandatory companion of XT_View. Fires when the preview moves on (next selection).
BOOL __stdcall XT_ReleaseMem(PVOID lpBuffer) {
    ++g_releaseCalls;
    g_itemLive = false;
    if (lpBuffer) HeapFree(GetProcessHeap(), 0, lpBuffer);
    return TRUE;
}

LONG __stdcall XT_Done(void* lpReserved) {
    if (g_hMsgWnd) { KillTimer(g_hMsgWnd, kRetryTimerId); KillTimer(g_hMsgWnd, kBeatTimerId); }
    DropController(L"XT_Done");
    g_env.Reset();
    if (g_hMsgWnd) { DestroyWindow(g_hMsgWnd); g_hMsgWnd = nullptr; }
    Logf(L"XT_Done — XT_View %ld call(s), XT_ReleaseMem %ld call(s) this session", g_viewCalls.load(), g_releaseCalls.load());
    if (g_comInitedHere) { CoUninitialize(); g_comInitedHere = false; }
    return 0;
}

} // extern "C"

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_hSelf = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}
