// ClaudeWidget - native Win32 taskbar usage overlay.
// Shows Claude subscription usage as 3 stacked bars (5h session / weekly all / weekly Fable).
// Topmost tool window locked into the taskbar band; ~single-digit MB, ~0% idle CPU.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <psapi.h>
#include <objidl.h>   // IUnknown / IStream — required by <gdiplus.h> under WIN32_LEAN_AND_MEAN
#include <gdiplus.h>
#include <string>
#include <vector>
#include <utility>
#include <cstring>
#include <cstdlib>
#include <climits>
#include <thread>
#include <atomic>

using namespace Gdiplus;

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif

// ---- ids / messages -------------------------------------------------------
#define TID_POLL       1
#define TID_FS         2
#define WM_APP_TRAY    (WM_APP + 1)
#define WM_APP_POLLED  (WM_APP + 2)
#define ID_REFRESH     1001
#define ID_STARTUP     1002
#define ID_RESET       1003
#define ID_QUIT        1004
#define ID_AUTORESUME  1005
#define ID_TESTRESUME  1006

static const wchar_t* RUNKEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// ---------------------------------------------------------------------------
// Minimal JSON parser (dependency-free). Inputs are trusted (Anthropic API +
// local files), so this favors compactness over exhaustive error reporting.
// ---------------------------------------------------------------------------
struct JVal {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    const JVal* get(const char* k) const {
        if (t == OBJ) for (auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    bool isNum() const { return t == NUM; }
    bool isStr() const { return t == STR; }
    bool isObj() const { return t == OBJ; }
    bool isArr() const { return t == ARR; }
};

struct JParser {
    const char* p; const char* e;
    explicit JParser(const std::string& str) { p = str.c_str(); e = p + str.size(); }
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }
    bool parse(JVal& v) { return val(v); }
    bool val(JVal& v) {
        ws(); if (p >= e) return false;
        char c = *p;
        if (c == '{') return obj(v);
        if (c == '[') return arr(v);
        if (c == '"') { v.t = JVal::STR; return str(v.s); }
        if (c == 't') { if (e - p >= 4 && !strncmp(p, "true", 4)) { p += 4; v.t = JVal::BOOL; v.b = true; return true; } return false; }
        if (c == 'f') { if (e - p >= 5 && !strncmp(p, "false", 5)) { p += 5; v.t = JVal::BOOL; v.b = false; return true; } return false; }
        if (c == 'n') { if (e - p >= 4 && !strncmp(p, "null", 4)) { p += 4; v.t = JVal::NUL; return true; } return false; }
        char* end2; double d = strtod(p, &end2);
        if (end2 == p) return false;
        p = end2; v.t = JVal::NUM; v.n = d; return true;
    }
    static int hexv(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    }
    static int hex4(const char* q) { return (hexv(q[0]) << 12) | (hexv(q[1]) << 8) | (hexv(q[2]) << 4) | hexv(q[3]); }
    static void appendUtf8(std::string& s, int cp) {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }
    bool str(std::string& out) {
        p++; out.clear();
        while (p < e) {
            char c = *p++;
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= e) return false;
                char x = *p++;
                switch (x) {
                    case '"': out += '"'; break;  case '\\': out += '\\'; break; case '/': out += '/'; break;
                    case 'b': out += '\b'; break; case 'f': out += '\f'; break;  case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break; case 't': out += '\t'; break;
                    case 'u': {
                        if (e - p < 4) return false;
                        int cp = hex4(p); p += 4;
                        if (cp >= 0xD800 && cp <= 0xDBFF && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                            int lo = hex4(p + 2); p += 6; cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        appendUtf8(out, cp); break;
                    }
                    default: out += x; break;
                }
            } else out += c;
        }
        return false;
    }
    bool obj(JVal& v) {
        v.t = JVal::OBJ; p++; ws();
        if (p < e && *p == '}') { p++; return true; }
        while (p < e) {
            ws(); if (p >= e || *p != '"') return false;
            std::string key; if (!str(key)) return false;
            ws(); if (p >= e || *p != ':') return false; p++;
            JVal child; if (!val(child)) return false;
            v.o.emplace_back(std::move(key), std::move(child));
            ws(); if (p >= e) return false;
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; return true; }
            return false;
        }
        return false;
    }
    bool arr(JVal& v) {
        v.t = JVal::ARR; p++; ws();
        if (p < e && *p == ']') { p++; return true; }
        while (p < e) {
            JVal child; if (!val(child)) return false;
            v.a.push_back(std::move(child));
            ws(); if (p >= e) return false;
            if (*p == ',') { p++; continue; }
            if (*p == ']') { p++; return true; }
            return false;
        }
        return false;
    }
};

// ---- small helpers --------------------------------------------------------
static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static std::wstring u8towide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string widetou8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static std::string readFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "";
    std::string out;
    char buf[8192]; DWORD rd = 0;
    while (ReadFile(h, buf, sizeof(buf), &rd, nullptr) && rd) out.append(buf, rd);
    CloseHandle(h);
    return out;
}
static void writeFile(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0; WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
    CloseHandle(h);
}
static std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"': o += "\\\""; break; case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break; case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
            default: o += c;
        }
    }
    return o;
}

// Parse ISO-8601 (treated as UTC; offset ignored — tooltip granularity only) to FILETIME uint64.
static unsigned long long isoToFt(const std::string& s) {
    int Y, M, D, h = 0, mi = 0, sec = 0;
    if (sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &mi, &sec) < 3) return 0;
    SYSTEMTIME st{}; st.wYear = (WORD)Y; st.wMonth = (WORD)M; st.wDay = (WORD)D;
    st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)sec;
    FILETIME ft; if (!SystemTimeToFileTime(&st, &ft)) return 0;
    ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}
static unsigned long long epochToFt(long long secs) {
    // unix epoch -> FILETIME (100ns since 1601). 11644473600 s between epochs.
    return (unsigned long long)(secs + 11644473600LL) * 10000000ULL;
}
static unsigned long long nowFt() {
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
static std::wstring formatReset(unsigned long long ftUtc) {
    if (!ftUtc) return L"";
    FILETIME ftu; ftu.dwLowDateTime = (DWORD)(ftUtc & 0xFFFFFFFF); ftu.dwHighDateTime = (DWORD)(ftUtc >> 32);
    FILETIME nowft; GetSystemTimeAsFileTime(&nowft);
    ULARGE_INTEGER a, b; a.LowPart = ftu.dwLowDateTime; a.HighPart = ftu.dwHighDateTime;
    b.LowPart = nowft.dwLowDateTime; b.HighPart = nowft.dwHighDateTime;
    long long diff100 = (long long)a.QuadPart - (long long)b.QuadPart;
    bool within24h = diff100 < (long long)24 * 3600 * 10000000LL;
    SYSTEMTIME utc, loc; FileTimeToSystemTime(&ftu, &utc);
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc)) loc = utc;
    wchar_t buf[64];
    if (within24h) wsprintfW(buf, L"%02d:%02d", loc.wHour, loc.wMinute);
    else {
        static const wchar_t* dow[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
        wsprintfW(buf, L"%s %02d:%02d", dow[loc.wDayOfWeek % 7], loc.wHour, loc.wMinute);
    }
    return buf;
}
// Absolute local reset stamp, e.g. "Mon 14 Sep 09:00" — for the weekly rows once their limit is hit.
static std::wstring formatResetDate(unsigned long long ftUtc) {
    FILETIME ftu; ftu.dwLowDateTime = (DWORD)(ftUtc & 0xFFFFFFFF); ftu.dwHighDateTime = (DWORD)(ftUtc >> 32);
    SYSTEMTIME utc, loc; FileTimeToSystemTime(&ftu, &utc);
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc)) loc = utc;
    static const wchar_t* dow[] = { L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat" };
    static const wchar_t* mon[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
    wchar_t buf[48];
    wsprintfW(buf, L"%s %d %s %02d:%02d", dow[loc.wDayOfWeek % 7], loc.wDay, mon[(loc.wMonth + 11) % 12], loc.wHour, loc.wMinute);
    return buf;
}

// ---- model ----------------------------------------------------------------
struct BarData {
    std::wstring label;
    int percent = 0;
    std::string severity = "normal";
    unsigned long long resetsFt = 0; // UTC FILETIME uint64, 0 = none
    bool stale = false;
};
struct Config {
    int x = INT_MIN, y = INT_MIN;
    int pollSeconds = 180;
    std::wstring userAgent = L"claude-code/2.1.209";
    bool manualPosition = false;
    // auto-resume: when the 5h limit is hit, ping every terminal at reset so blocked sessions continue.
    // Ships OFF (it types into terminals) — opt in via config.json "AutoResume": true or the tray menu.
    bool autoResume = false;
    int  resumeBufferSec = 90;                 // fire this long AFTER the reset instant (avoid firing too early)
    bool resumeLeadingEnter = true;            // press Enter first to clear Claude's wait/upgrade prompt (default option)
    std::wstring resumeMessage = L"[Auto resume on reset] : The session usage limit has been reached, it has now reset however. If you were doing something,  you can now continue again.";
};
struct PollResult {
    bool hasBars = false;
    std::vector<BarData> bars;
    bool stale = true;
    std::wstring note;
    unsigned long long retryUntil = 0; // GetTickCount64 ms, 0 = no change
};

// ---- globals (widget state; touched only on the UI thread) ----------------
static HWND  g_hwnd = nullptr;
static HWND  g_tip = nullptr;
static UINT  g_dpi = 96;
static Config g_cfg;
static std::vector<BarData> g_bars;
static std::wstring g_note = L"loading…";
static bool  g_stale = true;
static bool  g_userHidden = false, g_fsHidden = false;
static std::atomic<bool> g_polling{ false };
static unsigned long long g_retryUntil = 0;
static int   g_tick = 0;
static std::wstring g_exeDir;
static ULONG_PTR g_gdip = 0;
static NOTIFYICONDATAW g_nid{};
static HICON g_trayIcon = nullptr;
static unsigned long long g_rlFileTime = 0;
static long long g_cdMin = -2;             // last-painted 5h countdown minute (-1 = none/past); repaint only when it changes
static int   g_rdMask = -1;                // last-painted set of rows showing a reset date (bit i = g_bars[i])
// auto-resume state (touched only on the UI thread)
static bool  g_armed = false;
static unsigned long long g_resetFt = 0;   // the 5h reset instant we're waiting on (UTC FILETIME)
static unsigned long long g_fireFt = 0;    // reset + buffer: when we actually ping the terminals
static unsigned long long g_lastFiredReset = 0; // don't re-fire for the same reset cycle
static int   g_targetCount = 0;            // terminals currently open (shown while armed)
static std::atomic<bool> g_resuming{ false };

static int S(int v) { return MulDiv(v, g_dpi, 96); }

// ---- paths ----------------------------------------------------------------
static std::wstring exePath() {
    wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH); return buf;
}
static void initExeDir() {
    std::wstring p = exePath();
    size_t slash = p.find_last_of(L"\\/");
    g_exeDir = (slash == std::wstring::npos) ? L"." : p.substr(0, slash);
}
static std::wstring dirFile(const wchar_t* name) { return g_exeDir + L"\\" + name; }

// ---- config ---------------------------------------------------------------
static Config loadConfig() {
    Config c;
    std::string raw = readFile(dirFile(L"config.json"));
    if (raw.empty()) return c;
    JVal v; JParser jp(raw);
    if (!jp.parse(v) || !v.isObj()) return c;
    if (auto* x = v.get("X"); x && x->isNum()) c.x = (int)x->n;
    if (auto* y = v.get("Y"); y && y->isNum()) c.y = (int)y->n;
    if (auto* p = v.get("PollSeconds"); p && p->isNum()) c.pollSeconds = (int)p->n;
    if (auto* u = v.get("UserAgent"); u && u->isStr()) c.userAgent = u8towide(u->s);
    if (auto* m = v.get("ManualPosition"); m && m->t == JVal::BOOL) c.manualPosition = m->b;
    if (auto* a = v.get("AutoResume"); a && a->t == JVal::BOOL) c.autoResume = a->b;
    if (auto* b = v.get("ResumeBufferSec"); b && b->isNum()) c.resumeBufferSec = (int)b->n;
    if (auto* e = v.get("ResumeLeadingEnter"); e && e->t == JVal::BOOL) c.resumeLeadingEnter = e->b;
    if (auto* m = v.get("ResumeMessage"); m && m->isStr()) c.resumeMessage = u8towide(m->s);
    return c;
}
static void saveConfig() {
    std::string s = "{\n";
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "  \"X\": %d,\n  \"Y\": %d,\n  \"PollSeconds\": %d,\n", g_cfg.x, g_cfg.y, g_cfg.pollSeconds); s += buf;
    s += "  \"UserAgent\": \"" + jsonEscape(widetou8(g_cfg.userAgent)) + "\",\n";
    s += std::string("  \"ManualPosition\": ") + (g_cfg.manualPosition ? "true" : "false") + ",\n";
    s += std::string("  \"AutoResume\": ") + (g_cfg.autoResume ? "true" : "false") + ",\n";
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "  \"ResumeBufferSec\": %d,\n", g_cfg.resumeBufferSec); s += buf;
    s += std::string("  \"ResumeLeadingEnter\": ") + (g_cfg.resumeLeadingEnter ? "true" : "false") + ",\n";
    s += "  \"ResumeMessage\": \"" + jsonEscape(widetou8(g_cfg.resumeMessage)) + "\"\n}\n";
    writeFile(dirFile(L"config.json"), s);
}

// ---- parse usage response into bars ---------------------------------------
static std::wstring scopedLabel(const JVal& l) {
    if (auto* sc = l.get("scope"); sc && sc->isObj())
        if (auto* mo = sc->get("model"); mo && mo->isObj())
            if (auto* dn = mo->get("display_name"); dn && dn->isStr())
                return u8towide(dn->s);
    return L"Model";
}
static void addUtil(const JVal& root, const char* prop, const wchar_t* label, std::vector<BarData>& out) {
    auto* el = root.get(prop);
    if (!el || !el->isObj()) return;
    BarData b; b.label = label;
    if (auto* u = el->get("utilization"); u && u->isNum()) b.percent = (int)(u->n + 0.5);
    if (auto* r = el->get("resets_at"); r && r->isStr()) b.resetsFt = isoToFt(r->s);
    out.push_back(std::move(b));
}
static std::vector<BarData> parseBars(const std::string& json) {
    std::vector<BarData> out;
    JVal root; JParser jp(json);
    if (!jp.parse(root) || !root.isObj()) return out;
    if (auto* limits = root.get("limits"); limits && limits->isArr()) {
        for (auto& l : limits->a) {
            if (out.size() == 4) break;
            if (!l.isObj()) continue;
            std::string kind;
            if (auto* k = l.get("kind"); k && k->isStr()) kind = k->s;
            BarData b;
            if (kind == "session") b.label = L"5h";
            else if (kind == "weekly_all") b.label = L"Week";
            else if (kind == "weekly_scoped") b.label = scopedLabel(l);
            else b.label = u8towide(kind);
            if (auto* p = l.get("percent"); p && p->isNum()) b.percent = (int)(p->n + 0.5);
            if (auto* s = l.get("severity"); s && s->isStr()) b.severity = s->s;
            if (auto* r = l.get("resets_at"); r && r->isStr()) b.resetsFt = isoToFt(r->s);
            out.push_back(std::move(b));
        }
    }
    if (out.empty()) { // legacy shape
        addUtil(root, "five_hour", L"5h", out);
        addUtil(root, "seven_day", L"Week", out);
    }
    return out;
}

// ---- HTTP poll (worker thread) --------------------------------------------
static void pollWorker(std::wstring exeDir, std::wstring userAgent) {
    PollResult* res = new PollResult();
    res->stale = true;

    // token
    wchar_t profile[MAX_PATH]; DWORD pn = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    std::string token;
    if (pn) {
        std::string creds = readFile(std::wstring(profile) + L"\\.claude\\.credentials.json");
        if (!creds.empty()) {
            JVal v; JParser jp(creds);
            if (jp.parse(v) && v.isObj())
                if (auto* o = v.get("claudeAiOauth"); o && o->isObj())
                    if (auto* t = o->get("accessToken"); t && t->isStr()) token = t->s;
        }
    }
    if (token.empty()) {
        res->note = L"no credentials";
        PostMessageW(g_hwnd, WM_APP_POLLED, 0, (LPARAM)res);
        return;
    }

    HINTERNET hS = WinHttpOpen(userAgent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) { res->note = L"offline"; PostMessageW(g_hwnd, WM_APP_POLLED, 0, (LPARAM)res); return; }
    WinHttpSetTimeouts(hS, 10000, 10000, 10000, 10000);
    HINTERNET hC = WinHttpConnect(hS, L"api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET hR = hC ? WinHttpOpenRequest(hC, L"GET", L"/api/oauth/usage", nullptr,
                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    bool sent = false;
    if (hR) {
        std::wstring headers = L"Authorization: Bearer " + u8towide(token) + L"\r\nanthropic-beta: oauth-2025-04-20";
        WinHttpAddRequestHeaders(hR, headers.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        sent = WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            && WinHttpReceiveResponse(hR, nullptr);
    }
    if (!sent) {
        res->note = L"offline";
    } else {
        DWORD status = 0, slen = sizeof(status);
        WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            std::string body; DWORD avail = 0;
            do {
                avail = 0; WinHttpQueryDataAvailable(hR, &avail);
                if (avail) {
                    std::string chunk(avail, 0); DWORD rd = 0;
                    if (WinHttpReadData(hR, &chunk[0], avail, &rd) && rd) body.append(chunk.data(), rd);
                    else break;
                }
            } while (avail > 0);
            res->bars = parseBars(body);
            res->hasBars = true;
            res->stale = false;
            res->note.clear();
            writeFile(exeDir + L"\\cache.json", body);
        } else if (status == 429) {
            long long delay = 3600;
            wchar_t rbuf[64]; DWORD rlen = sizeof(rbuf);
            if (WinHttpQueryHeaders(hR, WINHTTP_QUERY_RETRY_AFTER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    rbuf, &rlen, WINHTTP_NO_HEADER_INDEX)) {
                long long v = _wtoi64(rbuf);
                if (v > 0) delay = v;
            }
            res->retryUntil = GetTickCount64() + (unsigned long long)(delay * 1000) + 15000;
            SYSTEMTIME st; GetLocalTime(&st);
            FILETIME ft; SystemTimeToFileTime(&st, &ft);
            ULARGE_INTEGER u; u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
            u.QuadPart += (unsigned long long)delay * 10000000ULL;
            ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
            FileTimeToSystemTime(&ft, &st);
            wchar_t nb[64]; wsprintfW(nb, L"rate-limited — retry %02d:%02d", st.wHour, st.wMinute);
            res->note = nb;
        } else if (status == 401) {
            res->note = L"token expired — use Claude Code to refresh";
        } else {
            wchar_t nb[32]; wsprintfW(nb, L"HTTP %lu", status); res->note = nb;
        }
    }
    if (hR) WinHttpCloseHandle(hR);
    if (hC) WinHttpCloseHandle(hC);
    if (hS) WinHttpCloseHandle(hS);
    PostMessageW(g_hwnd, WM_APP_POLLED, 0, (LPARAM)res);
}
static void triggerPoll() {
    if (GetTickCount64() < g_retryUntil) return;
    bool expected = false;
    if (!g_polling.compare_exchange_strong(expected, true)) return;
    std::thread(pollWorker, g_exeDir, g_cfg.userAgent).detach();
}

// ---- positioning ----------------------------------------------------------
static HWND trayWnd() { return FindWindowW(L"Shell_TrayWnd", nullptr); }
static void winSize(int& w, int& h) {
    RECT r; GetWindowRect(g_hwnd, &r); w = r.right - r.left; h = r.bottom - r.top;
}
static void positionOverTaskbar() {
    int w, h; winSize(w, h);
    HWND tray = trayWnd(); RECT t;
    if (tray && GetWindowRect(tray, &t)) {
        int y = t.top + imax(0, (t.bottom - t.top - h) / 2);
        int right = t.right;
        HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
        RECT nr;
        if (notify && GetWindowRect(notify, &nr)) right = nr.left;
        int x = imax(t.left, right - w - S(8));
        SetWindowPos(g_hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    // fallback: bottom-right of the primary work area
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(g_hwnd, nullptr, wa.right - w - S(16), wa.bottom - h - S(4), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static void clampToTaskbar() {
    int w, h; winSize(w, h);
    HWND tray = trayWnd(); RECT t;
    if (!tray || !GetWindowRect(tray, &t)) return;
    RECT wr; GetWindowRect(g_hwnd, &wr);
    int y = t.top + imax(0, (t.bottom - t.top - h) / 2);
    int x = clampi(wr.left, t.left, imax(t.left, t.right - w));
    if (wr.left != x || wr.top != y)
        SetWindowPos(g_hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}
static void ensureTopmost() {
    RECT r; if (GetWindowRect(g_hwnd, &r)) {
        POINT c{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        if (WindowFromPoint(c) == g_hwnd) return; // already on top where it matters
    }
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
}
static bool isShellClass(const wchar_t* c) {
    static const wchar_t* list[] = {
        L"WorkerW", L"Progman", L"Shell_TrayWnd",
        L"XamlExplorerHostIslandWindow", L"Windows.UI.Core.CoreWindow"
    };
    for (auto* s : list) if (!wcscmp(c, s)) return true;
    return false;
}
static void applyVisibility() {
    ShowWindow(g_hwnd, (!g_userHidden && !g_fsHidden) ? SW_SHOWNOACTIVATE : SW_HIDE);
}
static void fullscreenCheck() {
    HWND fg = GetForegroundWindow();
    bool fs = false;
    RECT r;
    if (fg && GetWindowRect(fg, &r)) {
        HMONITOR mFg = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
        HMONITOR mUs = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        if (mFg == mUs) {
            MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mFg, &mi);
            RECT s = mi.rcMonitor;
            if (r.left <= s.left && r.top <= s.top && r.right >= s.right && r.bottom >= s.bottom) {
                wchar_t cls[64]; GetClassNameW(fg, cls, 64);
                if (!isShellClass(cls)) fs = true;
            }
        }
    }
    if (fs != g_fsHidden) { g_fsHidden = fs; applyVisibility(); }
}
static void watchdog() {
    if (g_userHidden || g_fsHidden) return;
    if (!IsWindowVisible(g_hwnd)) ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    RECT r; GetWindowRect(g_hwnd, &r);
    POINT tl{ r.left, r.top };
    if (!MonitorFromPoint(tl, MONITOR_DEFAULTTONULL)) positionOverTaskbar();
    else clampToTaskbar();
    ensureTopmost();
}

// ---- sizing ---------------------------------------------------------------
static void updateTooltipRect();
static void paintLayered();
static void applySize(int n) {
    if (n < 1) n = 1;
    int w = S(176), h = n * S(14) + S(4);
    SetWindowPos(g_hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    updateTooltipRect();
    paintLayered();
}

// ---- tray + tooltip text --------------------------------------------------
static std::wstring buildTrayText() {
    if (g_bars.empty()) return L"Claude usage" + (g_note.empty() ? L"" : L" — " + g_note);
    std::wstring s = L"Claude — ";
    for (size_t i = 0; i < g_bars.size(); i++) {
        wchar_t b[64]; wsprintfW(b, L"%s %d%%", g_bars[i].label.c_str(), g_bars[i].percent);
        if (i) s += L" · ";
        s += b;
    }
    if (g_stale) s += L" (stale)";
    if (s.size() > 127) s = s.substr(0, 127);
    return s;
}
static std::wstring buildDetailText() {
    std::wstring s;
    for (auto& b : g_bars) {
        wchar_t line[128];
        std::wstring reset = formatReset(b.resetsFt);
        if (!reset.empty()) wsprintfW(line, L"%s: %d%% — resets %s", b.label.c_str(), b.percent, reset.c_str());
        else wsprintfW(line, L"%s: %d%%", b.label.c_str(), b.percent);
        if (!s.empty()) s += L"\n";
        s += line;
    }
    if (!g_note.empty()) { if (!s.empty()) s += L"\n"; s += g_note; }
    return s;
}
static void updateTray() {
    std::wstring t = buildTrayText();
    wcsncpy_s(g_nid.szTip, t.c_str(), _TRUNCATE);
    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static TOOLINFOW g_ti{};
static std::wstring g_tipText;
static void createTooltip() {
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, g_hwnd, nullptr,
        (HINSTANCE)GetWindowLongPtrW(g_hwnd, GWLP_HINSTANCE), nullptr);
    g_ti.cbSize = sizeof(g_ti);
    g_ti.uFlags = TTF_SUBCLASS;
    g_ti.hwnd = g_hwnd;
    g_ti.uId = 1;
    g_tipText = L"loading…";
    g_ti.lpszText = &g_tipText[0];
    GetClientRect(g_hwnd, &g_ti.rect);
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&g_ti);
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 320);
}
static void updateTooltipRect() {
    if (!g_tip) return;
    GetClientRect(g_hwnd, &g_ti.rect);
    SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&g_ti);
}
static void updateTooltip() {
    if (!g_tip) return;
    g_tipText = buildDetailText();
    if (g_tipText.empty()) g_tipText = L"Claude usage";
    g_ti.lpszText = &g_tipText[0];
    SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&g_ti);
}

static HICON makeTrayIcon() {
    // 3 coral bars on a 16x16 transparent icon, drawn via GDI+.
    Bitmap bmp(16, 16, PixelFormat32bppARGB);
    { Graphics g(&bmp); g.Clear(Color(0, 0, 0, 0));
      SolidBrush br(Color(255, 217, 119, 87));
      g.FillRectangle(&br, 1, 3, 13, 2);
      g.FillRectangle(&br, 1, 7, 9, 2);
      g.FillRectangle(&br, 1, 11, 11, 2);
    }
    HICON ico = nullptr; bmp.GetHICON(&ico);
    return ico;
}
static void addTray() {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_trayIcon = makeTrayIcon();
    g_nid.hIcon = g_trayIcon;
    wcscpy_s(g_nid.szTip, L"Claude usage");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void removeTray() { Shell_NotifyIconW(NIM_DELETE, &g_nid); if (g_trayIcon) DestroyIcon(g_trayIcon); }

// ---- registry autostart ---------------------------------------------------
static bool startupEnabled() {
    HKEY k; if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[1024]; DWORD sz = sizeof(buf), type = 0; bool r = false;
    if (RegQueryValueExW(k, L"ClaudeWidget", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS && type == REG_SZ) {
        std::wstring v = buf;
        if (!v.empty() && v.front() == L'"') { size_t last = v.find_last_of(L'"'); if (last > 0) v = v.substr(1, last - 1); }
        r = _wcsicmp(v.c_str(), exePath().c_str()) == 0;
    }
    RegCloseKey(k); return r;
}
static void setStartup(bool on) {
    HKEY k; if (RegCreateKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) { std::wstring v = L"\"" + exePath() + L"\""; RegSetValueExW(k, L"ClaudeWidget", 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t))); }
    else RegDeleteValueW(k, L"ClaudeWidget");
    RegCloseKey(k);
}

// ---- statusline file (near-real-time 5h/Week when Claude Code is wired) ----
static bool mergeRl(const JVal& root, const char* key, const wchar_t* label) {
    auto* el = root.get(key);
    if (!el || !el->isObj()) return false;
    BarData* bar = nullptr;
    for (auto& b : g_bars) if (b.label == label) { bar = &b; break; }
    if (!bar) return false;
    bool changed = bar->stale; bar->stale = false;
    if (auto* p = el->get("used_percentage"); p && p->isNum()) {
        int v = (int)(p->n + 0.5);
        if (v != bar->percent) { bar->percent = v; changed = true; }
        // statusline carries no severity: a polled "exceeded" would outlive the reset and pin the reset-date row
        if (bar->severity != "normal") { bar->severity = "normal"; changed = true; }
    }
    if (auto* r = el->get("resets_at")) {
        unsigned long long ft = 0;
        if (r->isNum()) ft = epochToFt((long long)r->n);
        else if (r->isStr()) ft = isoToFt(r->s);
        if (ft && ft != bar->resetsFt) { bar->resetsFt = ft; changed = true; }
    }
    return changed;
}
static void checkStatuslineFile() {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    std::wstring path = dirFile(L"rate_limits.json");
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return;
    unsigned long long mt = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
    if (mt == g_rlFileTime) return;
    g_rlFileTime = mt;
    // ignore leftovers older than 10 minutes
    FILETIME nowft; GetSystemTimeAsFileTime(&nowft);
    unsigned long long now = ((unsigned long long)nowft.dwHighDateTime << 32) | nowft.dwLowDateTime;
    if (now > mt && now - mt > (unsigned long long)10 * 60 * 10000000ULL) return;
    std::string raw = readFile(path);
    if (raw.empty()) return;
    JVal root; JParser jp(raw);
    if (!jp.parse(root) || !root.isObj()) return;
    if (g_bars.empty()) { // seed the two statusline bars
        g_bars.push_back(BarData{ L"5h" });
        g_bars.push_back(BarData{ L"Week" });
        applySize(2);
        if (!g_cfg.manualPosition) positionOverTaskbar();
        g_note.clear();
    }
    bool changed = mergeRl(root, "five_hour", L"5h");
    changed = mergeRl(root, "seven_day", L"Week") || changed;
    if (changed) { updateTray(); updateTooltip(); paintLayered(); }
}

// ---- painting -------------------------------------------------------------
// Whole minutes until a bar's reset (-1 = none/past). Drives the 5h countdown label.
static long long resetMinLeft(const BarData& b) {
    if (!b.resetsFt) return -1;
    long long remain = ((long long)b.resetsFt - (long long)nowFt()) / 10000000LL;
    return remain > 0 ? remain / 60 : -1;
}
static long long fiveHourMinLeft() {
    for (auto& b : g_bars) if (b.label == L"5h") return resetMinLeft(b);
    return -1;
}
static bool atLimit(const BarData& b) {
    return b.percent >= 100 || b.severity == "exceeded" || b.severity == "rejected";
}
// Non-5h rows (Week, Fable) swap bar + % for the reset date while their limit is hit.
static bool showsResetDate(const BarData& b) {
    return b.label != L"5h" && atLimit(b) && b.resetsFt > nowFt();
}
static int resetDateMask() {
    int m = 0;
    for (size_t i = 0; i < g_bars.size(); i++) if (showsResetDate(g_bars[i])) m |= 1 << i;
    return m;
}
static Color barColor(const BarData& b) {
    if (b.stale) return Color(255, 122, 122, 122);
    if (b.severity == "exceeded" || b.severity == "rejected" || b.percent >= 85) return Color(255, 235, 87, 87);
    if (b.severity == "warning" || b.percent >= 60) return Color(255, 242, 201, 76);
    return Color(255, 76, 195, 138);
}
static void fillPill(Graphics& g, Color c, int x, int y, int w, int h) {
    int d = h; if (w < d) w = d;
    GraphicsPath path;
    path.AddArc(x, y, d, d, 90, 180);
    path.AddArc(x + w - d, y, d, d, 270, 180);
    path.CloseFigure();
    SolidBrush br(c);
    g.FillPath(&br, &path);
}
static void drawContent(Graphics& g, int w, int h) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias); // grayscale AA blends over alpha (ClearType would fringe)
    // near-zero alpha fill: invisible against the taskbar, but keeps the whole widget hit-testable/draggable
    g.Clear(Color(1, 0, 0, 0));

    FontFamily ff(L"Segoe UI");
    REAL px = 7.5f * g_dpi / 72.0f;
    Font font(&ff, px, FontStyleRegular, UnitPixel);

    if (g_bars.empty()) {
        StringFormat sf; sf.SetAlignment(StringAlignmentCenter); sf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush br(Color(255, 220, 220, 220));
        RectF r(0, 0, (REAL)w, (REAL)h);
        std::wstring t = g_note.empty() ? L"…" : g_note;
        g.DrawString(t.c_str(), -1, &font, r, &sf, &br);
        return;
    }
    int n = (int)g_bars.size();
    int rowH = (h - S(4)) / n;
    int top = S(2), labelW = S(36), pctW = S(34), pad = S(8);
    StringFormat sfL; sfL.SetLineAlignment(StringAlignmentCenter); sfL.SetAlignment(StringAlignmentNear);
    sfL.SetTrimming(StringTrimmingEllipsisCharacter); sfL.SetFormatFlags(StringFormatFlagsNoWrap);
    StringFormat sfR; sfR.SetLineAlignment(StringAlignmentCenter); sfR.SetAlignment(StringAlignmentFar);
    sfR.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush labelBr(Color(255, 176, 176, 176));
    for (int i = 0; i < n; i++) {
        auto& b = g_bars[i];
        int y = top + i * rowH;
        // armed: the 5h row becomes "Reset <countdown> · <N> term" (amber), replacing its bar
        if (g_armed && b.label == L"5h") {
            long long remain64 = ((long long)g_resetFt - (long long)nowFt()) / 10000000LL;
            int remain = remain64 > 2147483647LL ? 2147483647 : (remain64 < 0 ? 0 : (int)remain64);
            wchar_t txt[80];
            if (remain > 0) {
                wchar_t cd[24];
                if (remain >= 3600) wsprintfW(cd, L"%d:%02d:%02d", remain / 3600, (remain % 3600) / 60, remain % 60);
                else wsprintfW(cd, L"%d:%02d", remain / 60, remain % 60);
                wsprintfW(txt, L"Reset %s · %d term", cd, g_targetCount);
            } else {
                wsprintfW(txt, L"resetting… · %d term", g_targetCount);
            }
            RectF rr((REAL)pad, (REAL)y, (REAL)(w - pad * 2), (REAL)rowH);
            SolidBrush armBr(Color(255, 242, 201, 76));
            g.DrawString(txt, -1, &font, rr, &sfL, &armBr);
            continue;
        }
        // 5h row: label shows time-to-reset as H:MM; falls back to "5h" when unknown/past
        RectF lr((REAL)pad, (REAL)y, (REAL)labelW, (REAL)rowH);
        const wchar_t* lbl = b.label.c_str();
        wchar_t cd[16];
        if (b.label == L"5h") {
            long long m = resetMinLeft(b);
            if (m >= 0) { wsprintfW(cd, L"%d:%02d", (int)(m / 60), (int)(m % 60)); lbl = cd; }
        }
        g.DrawString(lbl, -1, &font, lr, &sfL, &labelBr);
        int barX = pad + labelW + S(4);
        if (showsResetDate(b)) {
            std::wstring txt = L"resets " + formatResetDate(b.resetsFt);
            RectF rr((REAL)barX, (REAL)y, (REAL)(w - pad - barX), (REAL)rowH);
            SolidBrush rdBr(barColor(b));
            g.DrawString(txt.c_str(), -1, &font, rr, &sfL, &rdBr);
            continue;
        }
        wchar_t pcts[16]; wsprintfW(pcts, L"%d%%", b.percent);
        RectF pr((REAL)(w - pctW - pad), (REAL)y, (REAL)pctW, (REAL)rowH);
        SolidBrush pctBr(b.stale ? Color(255, 128, 128, 128) : Color(255, 220, 220, 220));
        g.DrawString(pcts, -1, &font, pr, &sfR, &pctBr);
        int barW = (w - pctW - pad) - S(6) - barX;
        int barH = S(5);
        int barY = y + (rowH - barH) / 2;
        if (barW > barH) {
            fillPill(g, Color(255, 58, 58, 58), barX, barY, barW, barH);
            int fillW = (int)(barW * clampi(b.percent, 0, 100) / 100.0);
            if (fillW > 0) fillPill(g, barColor(b), barX, barY, imax(fillW, barH), barH);
        }
    }
}

// Render into a premultiplied-alpha DIB and push it with UpdateLayeredWindow — gives a
// truly borderless widget: only the bars/text show, the taskbar shows through everywhere else.
static void paintLayered() {
    RECT rc; GetWindowRect(g_hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC screen = GetDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, dib);
    {
        Bitmap bmp(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits); // GDI+ writes premultiplied straight into the DIB
        Graphics g(&bmp);
        drawContent(g, w, h);
    }
    POINT src{ 0, 0 }; SIZE sz{ w, h };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_hwnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteObject(dib); DeleteDC(mem); ReleaseDC(nullptr, screen);
}

// ---- auto-resume: ping every terminal when the 5h limit resets ------------
static BOOL CALLBACK enumTermProc(HWND h, LPARAM lp) {
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t cls[128]; GetClassNameW(h, cls, 128);
    // Windows Terminal + classic conhost. Each Claude session is its own window (user setup).
    if (!wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS") || !wcscmp(cls, L"ConsoleWindowClass"))
        ((std::vector<HWND>*)lp)->push_back(h);
    return TRUE;
}
static std::vector<HWND> findTerminals() {
    std::vector<HWND> v; EnumWindows(enumTermProc, (LPARAM)&v); return v;
}
static void forceForeground(HWND h) {
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    HWND fg = GetForegroundWindow();
    DWORD fgT = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myT = GetCurrentThreadId();
    if (fgT && fgT != myT) AttachThreadInput(myT, fgT, TRUE);
    BringWindowToTop(h); SetForegroundWindow(h); SetFocus(h);
    if (fgT && fgT != myT) AttachThreadInput(myT, fgT, FALSE);
}
static void sendEnter() {
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_RETURN;
    in[1] = in[0]; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}
static void sendText(const std::wstring& s) {
    for (wchar_t c : s) {
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD; in[0].ki.wScan = c; in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0]; in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        Sleep(2);
    }
}
// Worker thread: focus each terminal and type. NEVER presses arrow keys, so a blind Enter can only
// ever pick the default-highlighted option (the user's "wait") — it cannot reach "upgrade credits".
static void doResumeWork() {
    std::wstring msg = g_cfg.resumeMessage;
    bool leadEnter = g_cfg.resumeLeadingEnter;
    std::vector<HWND> targets = findTerminals();
    HWND prevFg = GetForegroundWindow();
    for (HWND t : targets) {
        forceForeground(t); Sleep(160);
        if (leadEnter) { sendEnter(); Sleep(140); }
        sendText(msg); Sleep(60);
        sendEnter(); Sleep(160);
    }
    if (prevFg) forceForeground(prevFg);
    g_resuming = false;
}
static void triggerResume() {
    bool expected = false;
    if (!g_resuming.compare_exchange_strong(expected, true)) return;
    std::thread(doResumeWork).detach();
}
// Called on each fresh poll: arm when the 5h limit is hit, holding until the fire moment.
static void updateArm() {
    const BarData* five = nullptr;
    for (auto& b : g_bars) if (b.label == L"5h") { five = &b; break; }
    if (!five) return;
    if (g_cfg.autoResume && atLimit(*five) && five->resetsFt > nowFt() && five->resetsFt != g_lastFiredReset) {
        if (!g_armed || g_resetFt != five->resetsFt) {
            g_armed = true;
            g_resetFt = five->resetsFt;
            g_fireFt = five->resetsFt + (unsigned long long)g_cfg.resumeBufferSec * 10000000ULL;
        }
    }
    // deliberately no disarm-on-!atLimit: the post-reset poll shows !atLimit before the fire moment.
}

// ---- apply poll result ----------------------------------------------------
static void applyResult(PollResult* r) {
    g_polling = false;
    if (r->hasBars) {
        bool countChanged = r->bars.size() != g_bars.size();
        g_bars = std::move(r->bars);
        if (countChanged) {
            applySize((int)g_bars.size());
            if (g_cfg.manualPosition) clampToTaskbar(); else positionOverTaskbar();
        }
    }
    g_stale = r->stale;
    g_note = r->note;
    if (r->retryUntil) g_retryUntil = r->retryUntil;
    if (g_stale) for (auto& b : g_bars) b.stale = true;
    if (r->hasBars) updateArm();
    updateTray(); updateTooltip();
    paintLayered();
    EmptyWorkingSet(GetCurrentProcess());
    delete r;
}

static void toggleVisibility() {
    if (IsWindowVisible(g_hwnd)) { g_userHidden = true; }
    else { g_userHidden = false; g_fsHidden = false; }
    applyVisibility();
}
static void showMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_REFRESH, L"Refresh now");
    AppendMenuW(m, MF_STRING | (startupEnabled() ? MF_CHECKED : 0), ID_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_STRING, ID_RESET, L"Reset position");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_cfg.autoResume ? MF_CHECKED : 0), ID_AUTORESUME, L"Auto-resume on 5h reset");
    AppendMenuW(m, MF_STRING, ID_TESTRESUME, L"Test resume now");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
    POINT pt; GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    DestroyMenu(m);
}

// ---- window proc ----------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps); paintLayered(); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (wp == TID_POLL) triggerPoll();
        else if (wp == TID_FS) {
            fullscreenCheck(); checkStatuslineFile(); watchdog();
            if (g_armed) {
                if (nowFt() >= g_fireFt) {                 // reset + buffer elapsed: fire once, then disarm
                    g_lastFiredReset = g_resetFt; g_armed = false;
                    triggerResume(); paintLayered();
                } else {                                    // tick the countdown + refresh terminal count
                    g_targetCount = (int)findTerminals().size();
                    paintLayered();
                }
            } else {
                long long m = fiveHourMinLeft();            // 5h label countdown: repaint only on minute change
                int rd = resetDateMask();                   // ...or when a weekly row's reset instant passes
                if (m != g_cdMin || rd != g_rdMask) { g_cdMin = m; g_rdMask = rd; paintLayered(); }
            }
            if (++g_tick % 15 == 0) EmptyWorkingSet(GetCurrentProcess());
        }
        return 0;
    case WM_APP_POLLED: applyResult((PollResult*)lp); return 0;
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONDBLCLK) toggleVisibility();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) showMenu();
        return 0;
    case WM_LBUTTONDOWN:
        ReleaseCapture(); SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0); return 0;
    case WM_EXITSIZEMOVE: {
        clampToTaskbar();
        RECT wr; GetWindowRect(hwnd, &wr);
        g_cfg.x = wr.left; g_cfg.y = wr.top; g_cfg.manualPosition = true; saveConfig();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_REFRESH: triggerPoll(); break;
        case ID_STARTUP: setStartup(!startupEnabled()); break;
        case ID_RESET: g_cfg.manualPosition = false; saveConfig(); positionOverTaskbar(); break;
        case ID_AUTORESUME:
            g_cfg.autoResume = !g_cfg.autoResume; saveConfig();
            if (!g_cfg.autoResume) { g_armed = false; paintLayered(); }
            break;
        case ID_TESTRESUME: triggerResume(); break;
        case ID_QUIT: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (g_cfg.manualPosition) clampToTaskbar(); else positionOverTaskbar();
        return 0;
    case WM_DPICHANGED:
        g_dpi = HIWORD(wp);
        applySize(g_bars.empty() ? 3 : (int)g_bars.size());
        if (g_cfg.manualPosition) clampToTaskbar(); else positionOverTaskbar();
        paintLayered();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TID_POLL); KillTimer(hwnd, TID_FS);
        removeTray(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- cache seed -----------------------------------------------------------
static void loadCache() {
    std::string raw = readFile(dirFile(L"cache.json"));
    if (raw.empty()) return;
    g_bars = parseBars(raw);
    for (auto& b : g_bars) b.stale = true;
    if (!g_bars.empty()) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (GetFileAttributesExW(dirFile(L"cache.json").c_str(), GetFileExInfoStandard, &fad)) {
            SYSTEMTIME utc, loc; FileTimeToSystemTime(&fad.ftLastWriteTime, &utc);
            if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc)) loc = utc;
            wchar_t nb[32]; wsprintfW(nb, L"cached %02d:%02d", loc.wHour, loc.wMinute); g_note = nb;
        } else g_note.clear();
    }
}

// ---- statusline mode ------------------------------------------------------
static int runStatusline() {
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    std::string input; char buf[4096]; DWORD rd = 0;
    if (hin && hin != INVALID_HANDLE_VALUE)
        while (ReadFile(hin, buf, sizeof(buf), &rd, nullptr) && rd) input.append(buf, rd);
    if (input.empty()) return 0;
    JVal root; JParser jp(input);
    if (!jp.parse(root) || !root.isObj()) return 0;
    auto* rl = root.get("rate_limits");
    if (!rl || !rl->isObj()) return 0;
    // tee the rate_limits object to a file the widget watches
    initExeDir();
    // re-serialize minimal: just write the raw substring is hard here, so rebuild the needed fields
    std::string out = "{";
    bool first = true;
    for (const char* key : { "five_hour", "seven_day" }) {
        auto* o = rl->get(key);
        if (!o || !o->isObj()) continue;
        auto* up = o->get("used_percentage");
        auto* rs = o->get("resets_at");
        if (!first) out += ",";
        first = false;
        out += "\""; out += key; out += "\":{";
        bool f2 = true;
        if (up && up->isNum()) { char nb[32]; _snprintf_s(nb, sizeof(nb), _TRUNCATE, "\"used_percentage\":%g", up->n); out += nb; f2 = false; }
        if (rs) {
            if (!f2) out += ",";
            if (rs->isNum()) { char nb[48]; _snprintf_s(nb, sizeof(nb), _TRUNCATE, "\"resets_at\":%lld", (long long)rs->n); out += nb; }
            else if (rs->isStr()) { out += "\"resets_at\":\""; out += jsonEscape(rs->s); out += "\""; }
        }
        out += "}";
    }
    out += "}";
    writeFile(dirFile(L"rate_limits.json"), out);
    // print compact usage line to stdout (UTF-8)
    std::string line;
    for (const char* key : { "five_hour", "seven_day" }) {
        auto* o = rl->get(key);
        if (!o || !o->isObj()) continue;
        auto* up = o->get("used_percentage");
        if (!up || !up->isNum()) continue;
        char nb[32]; _snprintf_s(nb, sizeof(nb), _TRUNCATE, "%s %d%%", (strcmp(key, "five_hour") == 0 ? "5h" : "wk"), (int)(up->n + 0.5));
        if (!line.empty()) line += " \xc2\xb7 "; // UTF-8 middot
        line += nb;
    }
    if (!line.empty()) {
        HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD wr = 0; WriteFile(hout, line.data(), (DWORD)line.size(), &wr, nullptr);
    }
    return 0;
}

// ---- entry ----------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++)
        if (!wcscmp(argv[i], L"--statusline")) { LocalFree(argv); return runStatusline(); }
    if (argv) LocalFree(argv);

    CreateMutexW(nullptr, TRUE, L"Local\\ClaudeWidget_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    initExeDir();
    g_cfg = loadConfig();

    GdiplusStartupInput gsi; GdiplusStartup(&g_gdip, &gsi, nullptr);
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES }; InitCommonControlsEx(&icc);

    loadCache();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"ClaudeWidgetWnd";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED,
        L"ClaudeWidgetWnd", L"Claude Usage", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hInst, nullptr);
    g_dpi = GetDpiForWindow(g_hwnd);
    applySize(g_bars.empty() ? 3 : (int)g_bars.size());
    createTooltip();
    updateTooltip();

    if (g_cfg.manualPosition) {
        if (g_cfg.x != INT_MIN)
            SetWindowPos(g_hwnd, nullptr, g_cfg.x, g_cfg.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        clampToTaskbar();
    } else positionOverTaskbar();

    paintLayered(); // set the layered content before the first show
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    addTray();
    updateTray();

    SetTimer(g_hwnd, TID_POLL, imax(180, g_cfg.pollSeconds) * 1000, nullptr);
    SetTimer(g_hwnd, TID_FS, 2000, nullptr);
    triggerPoll();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    GdiplusShutdown(g_gdip);
    return 0;
}
