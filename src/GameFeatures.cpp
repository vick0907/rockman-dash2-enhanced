#include <windows.h>
#include <array>
#include <iostream>
#include <mutex>
#include <string>

using FeatureFunction = int(__cdecl*)();

struct Feature {
    const wchar_t* filename;
    const char* label;
    const char* setupExport;
    HMODULE module = nullptr;
    FeatureFunction setup = nullptr;
    FeatureFunction diagnose = nullptr;
};

static HMODULE localModule = nullptr;
static std::wstring localDirectory;
static std::array<Feature, 3> features = {{
    {L"SubtitleTest.dll", "Subtitles and base compatibility", "Setup"},
    {L"XInputTest.dll", "XInput and right-stick camera", "EnableFeatures"},
    {L"ResolutionTest.dll", "Native high-resolution modes", "EnableFeatures"}
}};

static void InitializeDirectory() {
    wchar_t filename[MAX_PATH] = {};
    GetModuleFileNameW(localModule, filename, MAX_PATH);
    localDirectory = filename;
    localDirectory.resize(localDirectory.find_last_of(L'\\'));
}

static void Log(const std::string& message) {
    HANDLE output = CreateFileW((localDirectory + L"\\GameFeatures.log").c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return;
    std::string line = std::to_string(GetCurrentProcessId()) + " " + message + "\r\n";
    DWORD written = 0;
    WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(output);
}

static bool LoadFeatures() {
    for (auto& feature : features) {
        if (!feature.module) feature.module = LoadLibraryW((localDirectory + L"\\" + feature.filename).c_str());
        feature.setup = feature.module ? reinterpret_cast<FeatureFunction>(GetProcAddress(feature.module, feature.setupExport)) : nullptr;
        feature.diagnose = feature.module ? reinterpret_cast<FeatureFunction>(GetProcAddress(feature.module, "Diagnose")) : nullptr;
        if (!feature.setup || !feature.diagnose) {
            Log(std::string("Missing feature DLL or required exports: ") + feature.label +
                "; Windows error=" + std::to_string(GetLastError()));
            return false;
        }
    }
    return true;
}

extern "C" __declspec(dllexport) int __cdecl Diagnose() {
    InitializeDirectory();
    if (!LoadFeatures()) return 2;
    for (const auto& feature : features) {
        int result = feature.diagnose();
        Log(std::string("Preflight ") + feature.label + " result=" + std::to_string(result));
        if (result != 0) return 2;
    }
    std::cout << "Combined subtitle, controller, and resolution preflight: PASS" << std::endl;
    return 0;
}

extern "C" __declspec(dllexport) int __cdecl Setup() {
    wchar_t filename[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, filename, MAX_PATH);
    const wchar_t* basename = wcsrchr(filename, L'\\');
    if (!basename || _wcsicmp(basename + 1, L"dash2.exe")) return 0;
    static std::once_flag initialized;
    static int setupResult = 0;
    std::call_once(initialized, []() {
        InitializeDirectory();
        if (!LoadFeatures() || features.front().diagnose() != 0) return;
        for (const auto& feature : features) {
            int result = feature.setup();
            Log(std::string("Enable ") + feature.label + " result=" + std::to_string(result));
            if (result != 100) return;
        }
        setupResult = 100;
        Log("All three features enabled in one game process.");
    });
    return setupResult;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) localModule = instance;
    return TRUE;
}