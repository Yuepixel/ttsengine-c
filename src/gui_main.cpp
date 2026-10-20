// =============================================================================
//  gui_main.cpp - the desktop front end (ttsc-gui.exe)
//
//  Why this exists
//    The server itself is a console program: for a customer that is not a
//    deliverable. The legacy Python build shipped a Tk window with a status
//    line, a copyable endpoint URL (that is what goes into the game MOD), a
//    start/stop pair and a log pane. This is the same thing in Win32, with no
//    extra runtime dependency: the model, the HTTP server and the window live
//    in one process.
//
//  Design notes
//    * The engine is created on start and destroyed on stop, so pressing
//      "stop" really hands the VRAM back (16 GB cards care).
//    * Everything the engine prints (including the vendored qwen/qwentts log
//      lines) is captured by redirecting fd 1 and 2 into a pipe; a reader
//      thread turns that into WM_APP_LOG messages. No logging callback has to
//      thread through the service layer.
//    * All UI strings are wide literals; the file is compiled with /utf-8.
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmsystem.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <io.h>
#include <fcntl.h>
#include <process.h>

#include "ttsc.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

// Built-in voice designs (generated from the legacy Python server's presets).
#include "voice_presets.inc"

// ---------------------------------------------------------------------------
//  Message ids used between the worker threads and the window
// ---------------------------------------------------------------------------
constexpr UINT WM_APP_LOG     = WM_APP + 1;   // wParam unused, lParam = std::wstring*
constexpr UINT WM_APP_READY   = WM_APP + 2;   // port bound, accepting
constexpr UINT WM_APP_STOPPED = WM_APP + 3;   // server thread finished cleanly
constexpr UINT WM_APP_FAILED  = WM_APP + 4;   // lParam = std::wstring* (reason)
constexpr UINT WM_APP_DONE    = WM_APP + 5;   // one-shot task finished

enum {
    IDC_STATUS = 100,
    IDC_TIP,
    IDC_ADDR,
    IDC_COPY,
    IDC_CFGPATH,
    IDC_OPENCFG,
    IDC_RELOADCFG,
    IDC_MODELS,
    IDC_BROWSE,
    IDC_SAVEMODELS,
    IDC_START,
    IDC_STOP,
    IDC_TEST,
    IDC_GENVOICES,
    IDC_VOICES,
    IDC_LOGCLEAR,
    IDC_LOG,
    IDC_VOICESEL      // combo: which voice 「试音」 speaks with
};

HWND  g_hwnd       = nullptr;
HWND  g_status     = nullptr;
HWND  g_tip        = nullptr;
HWND  g_addr       = nullptr;
HWND  g_cfgpath    = nullptr;
HWND  g_models     = nullptr;
HWND  g_voices     = nullptr;
HWND  g_log        = nullptr;
HWND  g_btn_start  = nullptr;
HWND  g_btn_stop   = nullptr;
HWND  g_btn_test   = nullptr;
HWND  g_btn_gen    = nullptr;
HWND  g_voicesel   = nullptr;   // combo box: which voice 「试音」 speaks with

// Combo contents, index-aligned with the CB_ items. Before this existed the
// panel always tested the first voice in the store (q3f_01, a female one), so
// listening to "试音" a few times made all 44 voices sound like one (reported).
std::vector<std::wstring> g_voice_ids;

std::wstring g_ini;          // absolute path of ttsserver.ini
HANDLE       g_log_read = INVALID_HANDLE_VALUE;
HFONT        g_font     = nullptr;
HFONT        g_font_bold = nullptr;

// Startup breadcrumbs: if the window never shows up, %TEMP%\ttsc-gui-start.log
// says how far wWinMain got. Cheap enough to keep in release builds.
void trace(const char* what) {
    char tmp[MAX_PATH] = {0};
    if (!GetTempPathA(MAX_PATH, tmp)) return;
    std::string p(tmp);
    p += "ttsc-gui-start.log";
    FILE* f = std::fopen(p.c_str(), "a");
    if (!f) return;
    std::fprintf(f, "%s\n", what);
    std::fclose(f);
}

// ---------------------------------------------------------------------------
//  small helpers
// ---------------------------------------------------------------------------
std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), &w[0], n);
    return w;
}

std::wstring get_text(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s(size_t(n), L'\0');
    if (n) GetWindowTextW(h, &s[0], n + 1);
    return s;
}

void set_text(HWND h, const std::wstring& s) {
    SetWindowTextW(h, s.c_str());
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    const size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? L"." : p.substr(0, cut);
}

std::wstring dir_of(const std::wstring& path) {
    const size_t cut = path.find_last_of(L"\\/");
    return cut == std::wstring::npos ? L"." : path.substr(0, cut);
}

std::wstring join_path(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

bool file_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Append a line to the log edit control, dropping the oldest lines beyond 800.
// ui_log is called from worker threads (voice generation, test synthesis and
// the stdout pump). Each send to the edit control is marshalled to the UI
// thread on its own, so two workers can interleave between EM_SETSEL and
// EM_REPLACESEL and splice half a line into the middle of another (observed:
// "[音色] 生成 q3f_2[试音] ..."). Serialise the whole append.
std::mutex g_log_mu;

void log_append(const std::wstring& line) {
    if (!g_log) return;
    std::lock_guard<std::mutex> lk(g_log_mu);
    const int len = GetWindowTextLengthW(g_log);
    if (len > 60000) {   // hard cap: rebuild the buffer from the last chunk
        std::wstring all(size_t(len), L'\0');
        GetWindowTextW(g_log, &all[0], len + 1);
        const size_t keep = all.rfind(L'\n', all.size() > 20000 ? all.size() - 20000 : 0);
        std::wstring tail = (keep == std::wstring::npos) ? all : all.substr(keep + 1);
        SetWindowTextW(g_log, tail.c_str());
    }
    SendMessageW(g_log, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    std::wstring msg = line + L"\r\n";
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)msg.c_str());
    SendMessageW(g_log, EM_SCROLLCARET, 0, 0);
}

void ui_log(const std::wstring& s) { log_append(s); }

void set_status(const std::wstring& s, COLORREF color) {
    if (g_status) {
        set_text(g_status, s);
        InvalidateRect(g_status, nullptr, TRUE);
        (void)color;
    }
}

// ---------------------------------------------------------------------------
//  stdout/stderr -> pipe -> window
// ---------------------------------------------------------------------------
unsigned __stdcall log_reader(void*) {
    char buf[4096];
    std::string acc;
    DWORD got = 0;
    while (ReadFile(g_log_read, buf, sizeof(buf), &got, nullptr) && got > 0) {
        acc.append(buf, got);
        size_t nl;
        while ((nl = acc.find('\n')) != std::string::npos) {
            std::string line = acc.substr(0, nl);
            acc.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // Strip the upstream log tag so the pane stays readable.
            for (const char* tag : {"[qwen/info] ", "[qwen/debug] ", "[qwen/warn] "}) {
                const size_t n = std::strlen(tag);
                if (line.rfind(tag, 0) == 0) { line = line.substr(n); break; }
            }
            if (line.empty()) continue;
            auto* w = new std::wstring(utf8_to_wide(line));
            if (!PostMessageW(g_hwnd, WM_APP_LOG, 0, (LPARAM)w)) delete w;
        }
    }
    return 0;
}

void redirect_std_streams() {
    // setvbuf must run before the stream is redirected or used: doing it after
    // _dup2 leaves the stream's buffer bookkeeping stale and can hang.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    trace("buf ok");

    int fds[2] = {-1, -1};
    if (_pipe(fds, 8192, _O_BINARY) != 0) { trace("pipe failed"); return; }
    trace("pipe ok");
    g_log_read = (HANDLE)_get_osfhandle(fds[0]);
    _dup2(fds[1], 1);
    _dup2(fds[1], 2);
    trace("dup ok");

    uintptr_t h = _beginthreadex(nullptr, 0, log_reader, nullptr, 0, nullptr);
    if (h) CloseHandle((HANDLE)h);
    trace("reader ok");
}

// ---------------------------------------------------------------------------
//  ini read / write (only the keys the window shows)
// ---------------------------------------------------------------------------
std::string ini_get(const std::string& path, const std::string& key) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        const size_t b = k.find_first_not_of(" \t");
        const size_t e = k.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        k = k.substr(b, e - b + 1);
        if (k != key) continue;
        std::string v = line.substr(eq + 1);
        const size_t vb = v.find_first_not_of(" \t");
        const size_t ve = v.find_last_not_of(" \t");
        return vb == std::string::npos ? std::string() : v.substr(vb, ve - vb + 1);
    }
    return {};
}

bool ini_set(const std::string& path, const std::string& key, const std::string& value) {
    std::vector<std::string> lines;
    {
        std::ifstream f(path);
        if (!f) return false;
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }
    bool done = false;
    for (auto& line : lines) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        const size_t b = k.find_first_not_of(" \t");
        const size_t e = k.find_last_not_of(" \t");
        if (b == std::string::npos) continue;
        k = k.substr(b, e - b + 1);
        if (k == key) { line = key + " = " + value; done = true; break; }
    }
    if (!done) lines.push_back(key + " = " + value);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    for (const auto& l : lines) f << l << "\r\n";
    return true;
}

// ggml-cuda.dll pulls in the CUDA runtime at load time; a double-clicked exe
// does not inherit the developer PATH. Prepend the usual locations so the GUI
// can be launched from Explorer without the run.cmd wrapper.
// CUDA_PATH is set by the toolkit installer; the literal fallbacks cover
// custom installs that omit it. Neither the redistributables nor the toolkit
// are shipped with the app (cublasLt alone is ~480 MB).
void ensure_runtime_path() {
    wchar_t old[MAX_PATH * 4] = {0};
    GetEnvironmentVariableW(L"PATH", old, DWORD(std::size(old)));
    std::wstring path;
    wchar_t cuda_root[32767] = {0};
    if (GetEnvironmentVariableW(L"CUDA_PATH", cuda_root, DWORD(std::size(cuda_root))) > 0) {
        const std::wstring base(cuda_root);
        for (const wchar_t* sub : {L"\\bin\\x64", L"\\bin"}) {
            const std::wstring d = base + sub;
            if (dir_exists(d)) path += d + L";";
        }
    }
    for (const wchar_t* d : {L"C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\v13.0\\bin",
                             L"C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\v12.8\\bin"}) {
        if (dir_exists(d)) path += std::wstring(d) + L";";
    }
    path += exe_dir() + L";";
    path += old;
    SetEnvironmentVariableW(L"PATH", path.c_str());
}

// ---------------------------------------------------------------------------
//  server lifecycle
// ---------------------------------------------------------------------------
struct Runtime {
    std::thread  thread;
    std::mutex   mu;
    ttsc::ServerContext ctx;
    std::unique_ptr<ttsc::VoiceStore> voices;
    std::unique_ptr<ttsc::Engine>     engine;
    std::unique_ptr<ttsc::Perf>       perf;
    std::atomic<bool> busy{false};      // a server thread exists
    std::atomic<bool> ready{false};     // port bound
    std::atomic<bool> stop{false};
};

Runtime g_rt;

void post_failure(const std::wstring& why) {
    auto* w = new std::wstring(why);
    if (!PostMessageW(g_hwnd, WM_APP_FAILED, 0, (LPARAM)w)) delete w;
}

void server_thread() {
    try {
        const ttsc::Config cfg = ttsc::Config::load(wide_to_utf8(g_ini));

        auto voices = std::make_unique<ttsc::VoiceStore>(cfg.data_dir);
        voices->load();
        auto engine = std::make_unique<ttsc::Engine>(cfg, voices.get());
        auto perf   = std::make_unique<ttsc::Perf>();

        {
            std::lock_guard<std::mutex> lk(g_rt.mu);
            g_rt.voices = std::move(voices);
            g_rt.engine = std::move(engine);
            g_rt.perf   = std::move(perf);

            g_rt.ctx.cfg        = cfg;
            g_rt.ctx.voices     = g_rt.voices.get();
            g_rt.ctx.engine     = g_rt.engine.get();
            g_rt.ctx.perf       = g_rt.perf.get();
            g_rt.ctx.started_at = ttsc::now_string();
        }

        g_rt.ctx.on_listening = []() { PostMessageW(g_hwnd, WM_APP_READY, 0, 0); };

        ui_log(L"[服务] 正在加载模型（首次约 3 秒）...");
        {
            std::lock_guard<std::mutex> lk(g_rt.mu);
            g_rt.engine->preload();
        }
        ui_log(utf8_to_wide("[服务] " + g_rt.engine->describe()));

        ttsc::run_http_server(g_rt.ctx);

        if (!g_rt.stop.load()) PostMessageW(g_hwnd, WM_APP_STOPPED, 0, 0);
        else                   PostMessageW(g_hwnd, WM_APP_STOPPED, 1, 0);
    } catch (const std::exception& e) {
        post_failure(utf8_to_wide(e.what()));
    } catch (...) {
        post_failure(L"未知错误");
    }
}

void start_service() {
    if (g_rt.busy.load()) return;
    g_rt.stop = false;
    g_rt.ready = false;
    g_rt.busy = true;

    EnableWindow(g_btn_start, FALSE);
    EnableWindow(g_btn_stop, TRUE);
    set_status(L"启动中...", RGB(180, 120, 0));
    ui_log(L"[服务] 启动中 ...");
    g_rt.thread = std::thread(server_thread);
}

void stop_service() {
    if (!g_rt.busy.load()) return;
    g_rt.stop = true;
    EnableWindow(g_btn_stop, FALSE);
    set_status(L"停止中...", RGB(180, 120, 0));

    {
        std::lock_guard<std::mutex> lk(g_rt.mu);
        if (g_rt.ctx.request_stop) g_rt.ctx.request_stop();
    }
    if (g_rt.thread.joinable()) g_rt.thread.join();

    {
        std::lock_guard<std::mutex> lk(g_rt.mu);
        g_rt.engine.reset();     // releases the VRAM
        g_rt.perf.reset();
        g_rt.voices.reset();
        g_rt.ctx = ttsc::ServerContext{};
    }
    g_rt.busy = false;
    g_rt.ready = false;
    EnableWindow(g_btn_start, TRUE);
    EnableWindow(g_btn_stop, FALSE);
    set_status(L"已停止", RGB(120, 120, 120));
    ui_log(L"[服务] 已停止，模型显存已释放");
}

std::wstring endpoint() {
    const std::string host = ini_get(wide_to_utf8(g_ini), "host");
    const std::string port = ini_get(wide_to_utf8(g_ini), "port");
    const std::wstring h = utf8_to_wide(host.empty() ? "127.0.0.1" : host);
    const std::wstring p = utf8_to_wide(port.empty() ? "8081" : port);
    return L"http://" + h + L":" + p;
}

int voice_count() {
    std::lock_guard<std::mutex> lk(g_rt.mu);
    if (!g_rt.voices) return -1;
    return int(g_rt.voices->list().size());
}

void refresh_voice_info() {
    const int n = voice_count();
    if (g_voicesel) EnableWindow(g_voicesel, n > 0);
    if (n < 0) {
        set_text(g_voices, L"音色: 服务未启动");
        if (g_voicesel) {
            SendMessageW(g_voicesel, CB_RESETCONTENT, 0, 0);
            g_voice_ids.clear();
        }
        return;
    }
    std::wstring s = L"音色: " + std::to_wstring(n) + L" 个";
    {
        std::lock_guard<std::mutex> lk(g_rt.mu);
        if (g_rt.voices) {
            const auto all = g_rt.voices->list();
            if (!all.empty()) {
                s += L"  (";
                for (size_t i = 0; i < all.size() && i < 6; ++i) {
                    if (i) s += L", ";
                    s += utf8_to_wide(all[i].id);
                }
                if (all.size() > 6) s += L", ...";
                s += L")";
            }
            if (g_voicesel) {
                // Keep the current choice if it still exists; otherwise fall
                // back to the first item.
                std::wstring keep;
                const int sel = int(SendMessageW(g_voicesel, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && size_t(sel) < g_voice_ids.size()) keep = g_voice_ids[size_t(sel)];
                g_voice_ids.clear();
                SendMessageW(g_voicesel, CB_RESETCONTENT, 0, 0);
                int restore = 0;
                for (const auto& v : all) {
                    const std::wstring wid = utf8_to_wide(v.id);
                    const std::wstring item = wid + L"  " + utf8_to_wide(v.description);
                    g_voice_ids.push_back(wid);
                    const int idx = int(SendMessageW(g_voicesel, CB_ADDSTRING, 0,
                                                     (LPARAM)item.c_str()));
                    if (!keep.empty() && wid == keep) restore = idx;
                }
                if (!g_voice_ids.empty())
                    SendMessageW(g_voicesel, CB_SETCURSEL, (WPARAM)restore, 0);
            }
        }
    }
    set_text(g_voices, s);
}

void update_tip() {
    const std::string mr = ini_get(wide_to_utf8(g_ini), "models_root");
    if (g_rt.ready.load()) {
        set_text(g_tip, L"运行中：点「复制」把地址粘到\n游戏 MOD → 设置 → 自定义 HTTP TTS → URL。");
    } else if (mr.empty() || !dir_exists(utf8_to_wide(mr))) {
        set_text(g_tip, L"① 模型目录不对：点「打开」改配置里的 models_root，\n再点「重新加载」。");
    } else if (g_rt.busy.load()) {
        set_text(g_tip, L"正在加载模型，首次约 3 秒，请稍候……");
    } else {
        set_text(g_tip, L"③ 点「启动服务」，等状态变成「运行中」，\n再点「复制」把地址填进游戏 MOD。");
    }
}

// ---------------------------------------------------------------------------
//  one-shot test synthesis (runs on its own thread)
// ---------------------------------------------------------------------------
void test_thread() {
    try {
        if (!g_rt.ready.load()) {
            post_failure(L"请先启动服务再试音");
            return;
        }
        ttsc::Voice voice;
        {
            std::lock_guard<std::mutex> lk(g_rt.mu);
            // Prefer whatever the operator picked in the combo; fall back to
            // the first voice when nothing is selected.
            int sel = -1;
            if (g_voicesel) {
                const LRESULT r = SendMessageW(g_voicesel, CB_GETCURSEL, 0, 0);
                if (r != CB_ERR) sel = int(r);
            }
            if (sel >= 0 && size_t(sel) < g_voice_ids.size()) {
                if (!g_rt.voices->get(wide_to_utf8(g_voice_ids[size_t(sel)]), &voice))
                    voice = ttsc::Voice{};
            }
            if (voice.id.empty()) {
                const auto all = g_rt.voices->list();
                if (all.empty()) throw std::runtime_error("no voices registered");
                voice = all.front();
            }
        }
        ttsc::Engine::Request req;
        req.text     = "你好，这是一次试听。现在播放的是本机生成的语音。";
        req.language = voice.language;
        req.rate     = 1.0f;

        ttsc::AudioBuffer audio;
        const auto t0 = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(g_rt.mu);
            g_rt.engine->synthesize(voice, req, &audio);
        }
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();

        const auto wav = ttsc::wav_encode(audio);
        wchar_t tmp[MAX_PATH] = {0};
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring out = join_path(tmp, L"ttsc-gui-test.wav");
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(wav.data()), std::streamsize(wav.size()));
        f.close();

        PlaySoundW(out.c_str(), nullptr, SND_FILENAME | SND_ASYNC);
        ui_log(L"[试音] " + utf8_to_wide(voice.id) + L"  "
               + std::to_wstring(int(audio.samples.size() * 1000 / (audio.sample_rate ? audio.sample_rate : 24000)))
               + L" ms 音频，用时 " + std::to_wstring(int(ms)) + L" ms");
    } catch (const std::exception& e) {
        post_failure(utf8_to_wide(std::string("试音失败: ") + e.what()));
    }
    PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ---------------------------------------------------------------------------
//  window
// ---------------------------------------------------------------------------
HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h,
        int id, HFONT font, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                             g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    if (c && font) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}

void build_ui() {
    const UINT dpi = GetDpiForWindow(g_hwnd) ? GetDpiForWindow(g_hwnd) : 96;
    auto px = [dpi](int v) { return MulDiv(v, int(dpi), 96); };

    NONCLIENTMETRICSW ncm = {sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_BOLD;
    g_font_bold = CreateFontIndirectW(&lf);

    const int pad = px(14);
    const int W = px(700);          // client width
    const int label_w = px(78);
    const int field_x = pad + label_w;
    const int field_w = px(340);
    const int btn_w = px(76);
    const int row_h = px(25);
    const int btn_h = px(27);
    int y = px(12);

    g_status = mk(L"STATIC", L"就绪", SS_LEFT, pad, y, W - 2 * pad, px(26), IDC_STATUS, g_font_bold);
    y += px(34);

    mk(L"STATIC", L"服务地址", SS_LEFT, pad, y + px(4), label_w, px(20), 0, g_font);
    g_addr = mk(L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_BORDER, field_x, y, field_w, row_h,
                IDC_ADDR, g_font);
    mk(L"BUTTON", L"复制", BS_PUSHBUTTON, field_x + field_w + px(8), y - px(1), btn_w, btn_h, IDC_COPY, g_font);
    y += px(35);

    mk(L"STATIC", L"配置文件", SS_LEFT, pad, y + px(4), label_w, px(20), 0, g_font);
    g_cfgpath = mk(L"STATIC", L"", SS_LEFT | SS_PATHELLIPSIS, field_x, y + px(4), field_w, px(20),
                   IDC_CFGPATH, g_font);
    mk(L"BUTTON", L"打开", BS_PUSHBUTTON, field_x + field_w + px(8), y - px(1), btn_w, btn_h, IDC_OPENCFG, g_font);
    mk(L"BUTTON", L"重新加载", BS_PUSHBUTTON, field_x + field_w + px(8) + btn_w + px(6), y - px(1), px(88), btn_h,
       IDC_RELOADCFG, g_font);
    y += px(35);

    mk(L"STATIC", L"模型目录", SS_LEFT, pad, y + px(4), label_w, px(20), 0, g_font);
    g_models = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, field_x, y, field_w, row_h, IDC_MODELS, g_font);
    mk(L"BUTTON", L"浏览", BS_PUSHBUTTON, field_x + field_w + px(8), y - px(1), btn_w, btn_h, IDC_BROWSE, g_font);
    mk(L"BUTTON", L"保存", BS_PUSHBUTTON, field_x + field_w + px(8) + btn_w + px(6), y - px(1), px(88), btn_h,
       IDC_SAVEMODELS, g_font);
    y += px(38);

    // Two-line tip: the text is long enough that one line gets clipped.
    g_tip = mk(L"STATIC", L"", SS_LEFT, pad, y, W - 2 * pad, px(40), IDC_TIP, g_font);
    y += px(46);

    g_btn_start = mk(L"BUTTON", L"启动服务", BS_DEFPUSHBUTTON, pad, y, px(118), px(34), IDC_START, g_font_bold);
    g_btn_stop  = mk(L"BUTTON", L"停止服务", BS_PUSHBUTTON, pad + px(126), y, px(118), px(34), IDC_STOP, g_font);
    EnableWindow(g_btn_stop, FALSE);
    g_btn_test  = mk(L"BUTTON", L"试音", BS_PUSHBUTTON, pad + px(252), y, px(90), px(34), IDC_TEST, g_font);
    g_btn_gen   = mk(L"BUTTON", L"生成内置音色", BS_PUSHBUTTON, pad + px(348), y, px(150), px(34),
                     IDC_GENVOICES, g_font);
    mk(L"BUTTON", L"清空日志", BS_PUSHBUTTON, W - pad - px(90), y + px(4), px(90), px(26), IDC_LOGCLEAR, g_font);
    y += px(46);

    g_voices = mk(L"STATIC", L"音色：服务未启动", SS_LEFT, pad, y, px(320), px(22), IDC_VOICES, g_font);
    g_voicesel = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                    pad + px(326), y - px(2), W - 2 * pad - px(326), px(220),
                    IDC_VOICESEL, g_font);
    EnableWindow(g_voicesel, FALSE);
    y += px(30);

    g_log = mk(L"EDIT", L"",
               ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_BORDER | WS_VSCROLL,
               pad, y, W - 2 * pad, px(320), IDC_LOG, g_font);
    SendMessageW(g_log, EM_LIMITTEXT, 0, 0);
    y += px(320) + pad;

    // Size the window so the client area is exactly what we laid out.
    RECT rc = {0, 0, W, y};
    AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, FALSE, 0, dpi);
    SetWindowPos(g_hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

void copy_to_clipboard(const std::wstring& s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* p = GlobalLock(h);
        std::memcpy(p, s.c_str(), bytes);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

std::wstring browse_folder() {
    BROWSEINFOW bi = {};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = L"选择模型目录（包含 Base 与 VoiceDesign 两个子目录）";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
    if (!idl) return {};
    wchar_t path[MAX_PATH] = {0};
    SHGetPathFromIDListW(idl, path);
    CoTaskMemFree(idl);
    return path;
}

// ---------------------------------------------------------------------------
//  batch: mint the 44 built-in voices with the design talker
// ---------------------------------------------------------------------------
std::atomic<bool> g_gen_busy{false};

void gen_thread() {
    const std::string ini = wide_to_utf8(g_ini);
    std::string lang = ini_get(ini, "language");
    if (lang.empty()) lang = kVoicePresetLanguage;
    std::string script = ini_get(ini, "default_script");
    if (script.empty()) {
        script = "其实我真的有发现，我是一个特别善于观察别人情绪的人。我们聊聊最近发生的事吧，今天过得怎么样？";
    }

    int made = 0, skipped = 0, failed = 0;
    {
        std::lock_guard<std::mutex> lk(g_rt.mu);
        if (g_rt.engine) g_rt.engine->set_keep_design_loaded(true);
    }
    ui_log(L"[音色] 开始生成内置音色（已存在的自动跳过）...");

    for (const auto& p : kVoicePresets) {
        const std::wstring wid = utf8_to_wide(p.id);
        try {
            {
                std::lock_guard<std::mutex> lk(g_rt.mu);
                if (g_rt.voices && g_rt.voices->exists(p.id)) {
                    ++skipped;
                    continue;
                }
            }
            ui_log(L"[音色] 生成 " + wid + L" ...");
            {
                std::lock_guard<std::mutex> lk(g_rt.mu);
                const ttsc::AudioBuffer ref = g_rt.engine->design(p.description, script, lang);
                g_rt.voices->add_designed(p.id, p.description, script, lang, ref);
                g_rt.engine->drop_voice(p.id);
            }
            ++made;
            ui_log(L"[音色] 完成 " + wid);
        } catch (const std::exception& e) {
            ++failed;
            ui_log(L"[音色] 失败 " + wid + L": " + utf8_to_wide(e.what()));
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_rt.mu);
        if (g_rt.engine) g_rt.engine->set_keep_design_loaded(false);
    }
    ui_log(L"[音色] 结束：新增 " + std::to_wstring(made) + L"，跳过 " + std::to_wstring(skipped) +
           L"，失败 " + std::to_wstring(failed));
    PostMessageW(g_hwnd, WM_APP_DONE, 1, 0);
}

void on_command(int id) {
    switch (id) {
        case IDC_COPY:
            copy_to_clipboard(endpoint());
            ui_log(L"[界面] 已复制地址 " + endpoint());
            break;
        case IDC_OPENCFG:
            ShellExecuteW(nullptr, L"open", L"notepad.exe", g_ini.c_str(), nullptr, SW_SHOWNORMAL);
            break;
        case IDC_RELOADCFG: {
            const std::string ini = wide_to_utf8(g_ini);
            set_text(g_models, utf8_to_wide(ini_get(ini, "models_root")));
            set_text(g_addr, endpoint());
            ui_log(L"[界面] 已重新读取配置");
            update_tip();
            break;
        }
        case IDC_BROWSE: {
            const std::wstring d = browse_folder();
            if (!d.empty()) set_text(g_models, d);
            break;
        }
        case IDC_SAVEMODELS: {
            const std::wstring mr = get_text(g_models);
            if (mr.empty()) {
                MessageBoxW(g_hwnd, L"模型目录不能为空。", L"提示", MB_ICONWARNING);
                break;
            }
            if (!dir_exists(mr)) {
                MessageBoxW(g_hwnd, L"这个目录不存在。", L"提示", MB_ICONWARNING);
                break;
            }
            if (!ini_set(wide_to_utf8(g_ini), "models_root", wide_to_utf8(mr))) {
                MessageBoxW(g_hwnd, L"写配置文件失败。", L"错误", MB_ICONERROR);
                break;
            }
            ui_log(L"[界面] models_root 已写入配置");
            update_tip();
            break;
        }
        case IDC_START:
            start_service();
            break;
        case IDC_STOP:
            stop_service();
            break;
        case IDC_TEST:
            if (!g_rt.ready.load()) {
                MessageBoxW(g_hwnd, L"请先启动服务。", L"提示", MB_ICONINFORMATION);
                break;
            }
            EnableWindow(g_btn_test, FALSE);
            std::thread(test_thread).detach();
            break;
        case IDC_LOGCLEAR:
            SetWindowTextW(g_log, L"");
            break;
        case IDC_GENVOICES: {
            if (!g_rt.ready.load()) {
                MessageBoxW(g_hwnd,
                            L"请先点「启动服务」——生成音色复用已加载的模型，\n"
                            L"不会另起一份占显存。",
                            L"提示", MB_ICONINFORMATION);
                break;
            }
            if (g_gen_busy.exchange(true)) break;
            EnableWindow(g_btn_gen, FALSE);
            EnableWindow(g_btn_stop, FALSE);   // engine must stay alive
            std::thread(gen_thread).detach();
            break;
        }
        default:
            break;
    }
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_COMMAND:
            on_command(LOWORD(wp));
            return 0;

        case WM_APP_LOG: {
            auto* s = reinterpret_cast<std::wstring*>(lp);
            if (s) { log_append(*s); delete s; }
            return 0;
        }
        case WM_APP_READY:
            g_rt.ready = true;
            set_status(L"运行中", RGB(0, 140, 0));
            set_text(g_addr, endpoint());
            refresh_voice_info();
            ui_log(L"[服务] 已就绪，MOD 地址: " + endpoint());
            update_tip();
            return 0;
        case WM_APP_STOPPED:
            if (wp == 1) {   // user pressed stop: release everything here
                if (g_rt.thread.joinable()) g_rt.thread.join();
                {
                    std::lock_guard<std::mutex> lk(g_rt.mu);
                    g_rt.engine.reset();
                    g_rt.perf.reset();
                    g_rt.voices.reset();
                    g_rt.ctx = ttsc::ServerContext{};
                }
                g_rt.busy = false;
                g_rt.ready = false;
                EnableWindow(g_btn_start, TRUE);
                EnableWindow(g_btn_stop, FALSE);
                set_status(L"已停止", RGB(120, 120, 120));
                refresh_voice_info();
                update_tip();
                ui_log(L"[服务] 已停止，模型显存已释放");
            }
            return 0;
        case WM_APP_FAILED: {
            auto* s = reinterpret_cast<std::wstring*>(lp);
            const std::wstring why = s ? *s : L"未知错误";
            delete s;
            ui_log(L"[错误] " + why);
            if (g_rt.thread.joinable()) g_rt.thread.join();
            g_rt.busy = false;
            g_rt.ready = false;
            EnableWindow(g_btn_start, TRUE);
            EnableWindow(g_btn_stop, FALSE);
            set_status(L"启动失败", RGB(180, 0, 0));
            MessageBoxW(h, why.c_str(), L"启动失败", MB_ICONERROR);
            return 0;
        }
        case WM_APP_DONE:
            EnableWindow(g_btn_test, TRUE);
            if (wp == 1) {                      // batch voice generation finished
                g_gen_busy = false;
                EnableWindow(g_btn_gen, TRUE);
                if (g_rt.busy.load()) EnableWindow(g_btn_stop, TRUE);
                refresh_voice_info();
                update_tip();
            }
            return 0;

        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC dc = (HDC)wp;
            if ((HWND)lp == g_log) {
                SetTextColor(dc, RGB(220, 220, 220));
                SetBkColor(dc, RGB(30, 30, 30));
                static HBRUSH dark = CreateSolidBrush(RGB(30, 30, 30));
                return (LRESULT)dark;
            }
            if ((HWND)lp == g_tip) SetTextColor(dc, RGB(0, 90, 200));
            if ((HWND)lp == g_status) {
                const std::wstring s = get_text(g_status);
                if (s == L"运行中") SetTextColor(dc, RGB(0, 140, 0));
                else if (s.find(L"失败") != std::wstring::npos) SetTextColor(dc, RGB(180, 0, 0));
                else if (s.find(L"中") != std::wstring::npos) SetTextColor(dc, RGB(190, 110, 0));
            }
            SetBkMode(dc, TRANSPARENT);
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }

        case WM_CLOSE:
            if (g_rt.busy.load()) stop_service();
            DestroyWindow(h);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdline, int) {
    trace("enter");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    trace("dpi ok");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    trace("com ok");
    InitCommonControls();
    trace("common controls ok");
    ensure_runtime_path();
    trace("path ok");

    // Config lookup: explicit argument, then next to the exe, then the current
    // directory (handy when running straight out of build-*/).
    std::wstring arg = cmdline ? cmdline : L"";
    if (!arg.empty() && arg.front() == L'"' && arg.back() == L'"') arg = arg.substr(1, arg.size() - 2);
    if (!arg.empty() && file_exists(arg)) {
        g_ini = arg;
    } else if (file_exists(join_path(exe_dir(), L"ttsserver.ini"))) {
        g_ini = join_path(exe_dir(), L"ttsserver.ini");
    } else if (file_exists(L"ttsserver.ini")) {
        wchar_t cwd[MAX_PATH] = {0};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        g_ini = join_path(cwd, L"ttsserver.ini");
    } else {
        MessageBoxW(nullptr,
                    (L"找不到 ttsserver.ini：\n" + join_path(exe_dir(), L"ttsserver.ini")).c_str(),
                    L"TtsEngine-C", MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"TtsEngineCGui";
    /* Resource id 1 = the multi-size icon embedded by src/resources/app.rc, so
     * the caption, taskbar and Alt+Tab all show the product icon rather than
     * the generic Windows one. */
    wc.hIcon         = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXICON),
                                         GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm       = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIcon) wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);
    trace("class ok");

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"TtsEngine-C  本地语音服务 (C++/GGML)",
                             WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 680, 620,
                             nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;
    trace("window ok");

    build_ui();
    trace("ui ok");
    redirect_std_streams();
    trace("streams ok");

    set_text(g_cfgpath, g_ini);
    set_text(g_models, utf8_to_wide(ini_get(wide_to_utf8(g_ini), "models_root")));
    set_text(g_addr, endpoint());
    update_tip();
    ui_log(L"[界面] 配置文件 " + g_ini);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    trace("shown");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_rt.busy.load()) {
        g_rt.stop = true;
        {
            std::lock_guard<std::mutex> lk(g_rt.mu);
            if (g_rt.ctx.request_stop) g_rt.ctx.request_stop();
        }
        if (g_rt.thread.joinable()) g_rt.thread.join();
    }
    CoUninitialize();
    return 0;
}
