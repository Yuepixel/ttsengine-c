// =============================================================================
//  launch_main.cpp - windowless launcher (ttsc-launch.exe)
//
//  Why this exists
//    ttsc-gui.exe links ggml.dll, which statically imports ggml-cuda.dll, which
//    imports cublas64_13.dll. Windows resolves that import chain *before* main
//    runs, so the GUI cannot fix PATH itself: without CUDA on PATH the process
//    dies with 0xC0000135 before a single line of our code executes (verified).
//    Shipping the CUDA redistributables would mean ~530 MB of cublas/cublasLt
//    next to a 47 MB engine, so instead this stub sets PATH and starts the GUI.
//    It has no console and no window: double-clicking the shortcut is silent.
// =============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

namespace {

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    const size_t cut = p.find_last_of(L"\\/");
    return cut == std::wstring::npos ? L"." : p.substr(0, cut);
}

bool dir_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    const std::wstring dir = exe_dir();

    wchar_t old[32767] = {0};
    GetEnvironmentVariableW(L"PATH", old, DWORD(sizeof(old) / sizeof(wchar_t)));

    // CUDA_PATH is set by the toolkit installer; the literal fallbacks cover
    // custom installs that omit it. Keep this list in sync with gui_main.cpp.
    std::wstring path;
    wchar_t cuda_root[32767] = {0};
    if (GetEnvironmentVariableW(L"CUDA_PATH", cuda_root, DWORD(sizeof(cuda_root) / sizeof(wchar_t))) > 0) {
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
    path += dir + L";";
    path += old;
    SetEnvironmentVariableW(L"PATH", path.c_str());

    const std::wstring gui = dir + L"\\ttsc-gui.exe";
    if (GetFileAttributesW(gui.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr, (L"ttsc-gui.exe 不在这个目录里：\n" + dir).c_str(),
                    L"TtsEngine-C", MB_ICONERROR);
        return 1;
    }

    std::wstring cmd = L"\"" + gui + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(gui.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(),
                        &si, &pi)) {
        MessageBoxW(nullptr, L"启动 ttsc-gui.exe 失败。", L"TtsEngine-C", MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
