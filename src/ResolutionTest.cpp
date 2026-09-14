#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <initguid.h>
#include <ddraw.h>
#include <MinHook.h>
#include <array>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

using CreateDrawFunction = HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*);
using EnumerateDrawFunction = HRESULT(WINAPI*)(LPDDENUMCALLBACKA, void*);
using MemoryCheckFunction = int(__cdecl*)(DWORD, DWORD, DWORD, DWORD, DWORD);
using SaveConfigFunction = void(__cdecl*)();
using SetModeFunction = HRESULT(WINAPI*)(IDirectDraw7*, DWORD, DWORD, DWORD, DWORD, DWORD);
using CreateSurfaceFunction = HRESULT(WINAPI*)(IDirectDraw7*, DDSURFACEDESC2*, IDirectDrawSurface7**, IUnknown*);
using FlipFunction = HRESULT(WINAPI*)(IDirectDrawSurface7*, IDirectDrawSurface7*, DWORD);

static HMODULE localModule = nullptr;
static std::wstring localDirectory;
static CreateDrawFunction createDraw = nullptr;
static CreateDrawFunction originalCreateDraw = nullptr;
static MemoryCheckFunction originalMemoryCheck = nullptr;
static SaveConfigFunction originalSaveConfig = nullptr;
static SetModeFunction originalSetMode = nullptr;
static CreateSurfaceFunction originalCreateSurface = nullptr;
static FlipFunction originalFlip = nullptr;
static bool modeFixInstalled = false;

static bool HookMethod(void* object, unsigned index, void* replacement, void** original);

template<typename Value>
static bool ReadGame(uintptr_t address, Value& value) {
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value, sizeof(value), &read) &&
        read == sizeof(value);
}

static bool MemoryBudgetFits(DWORD available, DWORD fullscreen, DWORD width, DWORD height, DWORD bits,
    bool primary, DWORD desktopWidth, DWORD desktopHeight, DWORD desktopBits) {
    uint64_t budget = available;
    if (fullscreen && primary) budget += uint64_t(desktopWidth) * desktopHeight * (desktopBits / 8);
    uint64_t required = uint64_t(width) * height * ((fullscreen + 1) * (bits / 8) + 2);
    return budget >= required;
}

static void InitializeDirectory() {
    wchar_t filename[MAX_PATH] = {};
    GetModuleFileNameW(localModule, filename, MAX_PATH);
    localDirectory = filename;
    localDirectory.resize(localDirectory.find_last_of(L'\\'));
}

static void Log(const std::string& message) {
    HANDLE output = CreateFileW((localDirectory + L"\\ResolutionTest.log").c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return;
    std::string line = std::to_string(GetCurrentProcessId()) + " " + message + "\r\n";
    DWORD written = 0;
    WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(output);
}

struct ModeSummary {
    DWORD budget = 0;
    unsigned count = 0;
    bool fullHD = false;
};

static HRESULT WINAPI CountMode(DDSURFACEDESC2* mode, void* context) {
    auto& summary = *static_cast<ModeSummary*>(context);
    ++summary.count;
    if (mode->dwWidth == 1920 && mode->dwHeight == 1080 && mode->ddpfPixelFormat.dwRGBBitCount == 32) {
        summary.fullHD = true;
    }
    if (mode->dwWidth >= 640 && mode->dwHeight >= 480 && mode->dwWidth <= 1920 && mode->dwHeight <= 1080) {
        uint64_t required = (2 * (mode->ddpfPixelFormat.dwRGBBitCount / 8) + 2) *
            uint64_t(mode->dwWidth) * mode->dwHeight;
        Log("Enumerated " + std::to_string(mode->dwWidth) + "x" + std::to_string(mode->dwHeight) +
            "x" + std::to_string(mode->ddpfPixelFormat.dwRGBBitCount) +
            " legacy-buffer-estimate=" + std::to_string(required));
    }
    return DDENUMRET_OK;
}

static BOOL CALLBACK DiagnoseAdapter(GUID* identifier, char*, char*, void* context) {
    auto& successful = *static_cast<unsigned*>(context);
    IDirectDraw7* draw = nullptr;
    HRESULT result = createDraw(identifier, reinterpret_cast<void**>(&draw), IID_IDirectDraw7, nullptr);
    Log("Adapter primary=" + std::to_string(identifier == nullptr) + " create=" + std::to_string(result));
    if (FAILED(result) || !draw) return TRUE;
    DDSCAPS2 caps = {};
    caps.dwCaps = DDSCAPS_VIDEOMEMORY;
    DWORD videoTotal = 0, videoFree = 0, textureTotal = 0, textureFree = 0;
    HRESULT videoResult = draw->GetAvailableVidMem(&caps, &videoTotal, &videoFree);
    caps.dwCaps = DDSCAPS_TEXTURE;
    HRESULT textureResult = draw->GetAvailableVidMem(&caps, &textureTotal, &textureFree);
    ModeSummary summary;
    summary.budget = videoTotal;
    if (textureTotal < videoTotal) summary.budget -= textureTotal;
    Log("Video total=" + std::to_string(videoTotal) + " free=" + std::to_string(videoFree) +
        " result=" + std::to_string(videoResult) + " texture total=" + std::to_string(textureTotal) +
        " free=" + std::to_string(textureFree) + " result=" + std::to_string(textureResult) +
        " legacy-budget=" + std::to_string(summary.budget));
    result = draw->EnumDisplayModes(0, nullptr, &summary, &CountMode);
    Log("Mode enumeration result=" + std::to_string(result) + " count=" + std::to_string(summary.count) +
        " fullHD32=" + std::to_string(summary.fullHD));
    HWND window = CreateWindowExW(0, L"STATIC", L"Rockman resolution diagnostics", WS_OVERLAPPED,
        0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    bool allocated = false;
    if (window && SUCCEEDED(draw->SetCooperativeLevel(window, DDSCL_NORMAL))) {
        DDSURFACEDESC2 surfaceDescription = {};
        surfaceDescription.dwSize = sizeof(surfaceDescription);
        surfaceDescription.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
        surfaceDescription.dwWidth = 1920;
        surfaceDescription.dwHeight = 1080;
        surfaceDescription.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_3DDEVICE | DDSCAPS_VIDEOMEMORY;
        surfaceDescription.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
        surfaceDescription.ddpfPixelFormat.dwFlags = DDPF_RGB;
        surfaceDescription.ddpfPixelFormat.dwRGBBitCount = 32;
        surfaceDescription.ddpfPixelFormat.dwRBitMask = 0x00ff0000;
        surfaceDescription.ddpfPixelFormat.dwGBitMask = 0x0000ff00;
        surfaceDescription.ddpfPixelFormat.dwBBitMask = 0x000000ff;
        IDirectDrawSurface7* surface = nullptr;
        result = draw->CreateSurface(&surfaceDescription, &surface, nullptr);
        allocated = SUCCEEDED(result) && surface;
        if (surface) surface->Release();
        Log("1920x1080x32 offscreen 3D surface allocation=" + std::to_string(result));
    }
    draw->Release();
    if (window) DestroyWindow(window);
    if (summary.fullHD && allocated) ++successful;
    return TRUE;
}

static int __cdecl CheckModeMemory(DWORD available, DWORD fullscreen, DWORD width, DWORD height, DWORD bits) {
    int legacy = originalMemoryCheck(available, fullscreen, width, height, bits);
    if (fullscreen > 1 || width < 640 || height < 480 || width > 1920 || height > 1080 ||
        (bits != 16 && bits != 32)) return legacy;
    DWORD adapter = 0, primary = 0, desktopWidth = 0, desktopHeight = 0, desktopBits = 0;
    if (!ReadGame(0x8460a8, adapter) || adapter >= 4 ||
        !ReadGame(0x9246b0 + adapter * 0x154, primary) ||
        !ReadGame(0xa29c04, desktopWidth) || !ReadGame(0x929c00, desktopHeight) ||
        !ReadGame(0x922658, desktopBits) || desktopWidth > 65535 || desktopHeight > 65535 ||
        (desktopBits != 16 && desktopBits != 24 && desktopBits != 32)) return legacy;
    int corrected = MemoryBudgetFits(available, fullscreen, width, height, bits,
        primary != 0, desktopWidth, desktopHeight, desktopBits) ? 1 : 0;
    if (corrected != legacy) {
        Log("Memory overflow corrected for " + std::to_string(width) + "x" + std::to_string(height) +
            "x" + std::to_string(bits) + " fullscreen=" + std::to_string(fullscreen) +
            " reported-budget=" + std::to_string(available) + " old=" + std::to_string(legacy) +
            " new=" + std::to_string(corrected));
    }
    return corrected;
}

static void __cdecl SaveConfig() {
    originalSaveConfig();
    Log("Native configuration-save shutdown completed.");
    wchar_t eventName[160] = {};
    DWORD length = GetEnvironmentVariableW(L"ROCKMAN_RESOLUTION_QUIT_EVENT", eventName, 160);
    if (!length || length >= 160) return;
    HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
    if (event) {
        SetEvent(event);
        CloseHandle(event);
    }
}

static bool InstallModeFix() {
    if (modeFixInstalled) return true;
    if (GetModuleHandleW(nullptr) != reinterpret_cast<HMODULE>(0x400000)) return false;
    const std::array<unsigned char, 16> helperSignature = {
        0x8b,0x54,0x24,0x08,0x85,0xd2,0x74,0x3c,0x8b,0x0d,0xa8,0x60,0x84,0x00,0x8b,0xc1
    };
    const std::array<unsigned char, 16> callerSignature = {
        0x68,0x90,0x80,0x40,0x00,0x50,0x6a,0x00,0x8b,0x11,0x6a,0x00,0x51,0xff,0x52,0x20
    };
    const std::array<unsigned char, 11> saveSignature = {
        0x55,0x68,0xbc,0x6a,0x84,0x00,0x68,0x18,0xeb,0x8a,0x00
    };
    std::array<unsigned char, 16> helper = {}, caller = {};
    std::array<unsigned char, 11> save = {};
    if (!ReadGame(0x408af0, helper) || helper != helperSignature ||
        !ReadGame(0x407f70, caller) || caller != callerSignature ||
        !ReadGame(0x54aba0, save) || save != saveSignature) {
        Log("Resolution bindings do not match this game image; no native code patched.");
        return false;
    }
    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(0x408af0), reinterpret_cast<void*>(&CheckModeMemory),
        reinterpret_cast<void**>(&originalMemoryCheck));
    if (status == MH_OK) status = MH_CreateHook(reinterpret_cast<void*>(0x54aba0), reinterpret_cast<void*>(&SaveConfig),
        reinterpret_cast<void**>(&originalSaveConfig));
    if (status == MH_OK) status = MH_EnableHook(reinterpret_cast<void*>(0x408af0));
    if (status == MH_OK) status = MH_EnableHook(reinterpret_cast<void*>(0x54aba0));
    modeFixInstalled = status == MH_OK;
    Log("Signature-checked 64-bit mode-budget fix installed=" + std::to_string(modeFixInstalled));
    return modeFixInstalled;
}

static HRESULT WINAPI SetMode(IDirectDraw7* draw, DWORD width, DWORD height, DWORD bits, DWORD refresh, DWORD flags) {
    HRESULT result = originalSetMode(draw, width, height, bits, refresh, flags);
    Log("Game SetDisplayMode " + std::to_string(width) + "x" + std::to_string(height) + "x" +
        std::to_string(bits) + " refresh=" + std::to_string(refresh) + " result=" + std::to_string(result));
    return result;
}

static bool SaveDisplayedFrame(DWORD width, DWORD height, unsigned sample) {
    HWND window = GetForegroundWindow();
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    wchar_t className[32] = {};
    GetClassNameW(window, className, 32);
    RECT bounds = {};
    POINT origin = {};
    if (process != GetCurrentProcessId() || wcscmp(className, L"Dash2") ||
        !GetClientRect(window, &bounds) || !ClientToScreen(window, &origin) ||
        bounds.right != LONG(width) || bounds.bottom != LONG(height)) return false;
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = LONG(width);
    info.bmiHeader.biHeight = -LONG(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = width * height * 4;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC memory = CreateCompatibleDC(screen);
    bool saved = false;
    if (bitmap && memory && pixels) {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        if (BitBlt(memory, 0, 0, width, height, screen, origin.x, origin.y, SRCCOPY)) {
            GdiFlush();
            auto* colors = static_cast<unsigned char*>(pixels);
            unsigned nonblack = 0;
            for (DWORD index = 0; index < width * height; ++index) {
                if (colors[index * 4] > 16 || colors[index * 4 + 1] > 16 || colors[index * 4 + 2] > 16) ++nonblack;
            }
            if (nonblack > width * height / 50) {
                std::wstring directory = localDirectory + L"\\build\\resolution-test";
                std::wstring filename = directory + L"\\frame-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                    std::to_wstring(width) + L"x" + std::to_wstring(height) + L"-" + std::to_wstring(sample) + L".bmp";
                BITMAPFILEHEADER header = {};
                header.bfType = 0x4d42;
                header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
                header.bfSize = header.bfOffBits + info.bmiHeader.biSizeImage;
                HANDLE output = CreateFileW(filename.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (output != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    saved = WriteFile(output, &header, sizeof(header), &written, nullptr) && written == sizeof(header) &&
                        WriteFile(output, &info.bmiHeader, sizeof(BITMAPINFOHEADER), &written, nullptr) &&
                        written == sizeof(BITMAPINFOHEADER) &&
                        WriteFile(output, pixels, info.bmiHeader.biSizeImage, &written, nullptr) &&
                        written == info.bmiHeader.biSizeImage;
                    CloseHandle(output);
                }
                if (saved) Log("Displayed frame " + std::to_string(width) + "x" + std::to_string(height) +
                    " sample=" + std::to_string(sample) + " nonblack=" + std::to_string(nonblack));
            }
        }
        SelectObject(memory, previous);
    }
    if (memory) DeleteDC(memory);
    if (bitmap) DeleteObject(bitmap);
    ReleaseDC(nullptr, screen);
    return saved;
}

static void ObservePresentation(IDirectDrawSurface7* primary) {
    static DWORD previousWidth = 0, previousHeight = 0;
    static ULONGLONG changedAt = 0, lastAttempt = 0;
    static unsigned sample = 0;
    DDSURFACEDESC2 description = {};
    description.dwSize = sizeof(description);
    if (FAILED(primary->GetSurfaceDesc(&description)) || !(description.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) ||
        description.dwWidth > 1920 || description.dwHeight > 1080) return;
    ULONGLONG now = GetTickCount64();
    if (description.dwWidth != previousWidth || description.dwHeight != previousHeight) {
        previousWidth = description.dwWidth;
        previousHeight = description.dwHeight;
        changedAt = now;
        sample = 0;
        DWORD viewportWidth = 0, viewportHeight = 0;
        float horizontalScale = 0, verticalScale = 0;
        if (ReadGame(0xa2a8e0, viewportWidth) && ReadGame(0xa2a8e4, viewportHeight) &&
            ReadGame(0xa29c28, horizontalScale) && ReadGame(0xa29c24, verticalScale)) {
            Log("Presented primary=" + std::to_string(previousWidth) + "x" + std::to_string(previousHeight) +
                " viewport=" + std::to_string(viewportWidth) + "x" + std::to_string(viewportHeight) +
                " scale=" + std::to_string(horizontalScale) + "," + std::to_string(verticalScale));
        }
    }
    const ULONGLONG delays[] = {3000, 12000, 25000};
    if (sample < 3 && now - changedAt >= delays[sample] && now - lastAttempt >= 1000) {
        lastAttempt = now;
        if (SaveDisplayedFrame(previousWidth, previousHeight, sample + 1)) ++sample;
    }
}

static HRESULT WINAPI Flip(IDirectDrawSurface7* primary, IDirectDrawSurface7* overrideSurface, DWORD flags) {
    HRESULT result = originalFlip(primary, overrideSurface, flags);
    if (SUCCEEDED(result)) ObservePresentation(primary);
    return result;
}

static HRESULT WINAPI CreateSurface(IDirectDraw7* draw, DDSURFACEDESC2* description,
    IDirectDrawSurface7** output, IUnknown* outer) {
    HRESULT result = originalCreateSurface(draw, description, output, outer);
    if (SUCCEEDED(result) && output && *output && description &&
        (description->ddsCaps.dwCaps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_ZBUFFER | DDSCAPS_3DDEVICE))) {
        DDSURFACEDESC2 actual = {};
        actual.dwSize = sizeof(actual);
        if (SUCCEEDED((*output)->GetSurfaceDesc(&actual))) {
            Log("Game surface " + std::to_string(actual.dwWidth) + "x" + std::to_string(actual.dwHeight) +
                " caps=" + std::to_string(actual.ddsCaps.dwCaps));
            if (actual.ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE) {
                HookMethod(*output, 11, reinterpret_cast<void*>(&Flip), reinterpret_cast<void**>(&originalFlip));
            }
        }
    }
    return result;
}

static bool HookMethod(void* object, unsigned index, void* replacement, void** original) {
    if (*original) return true;
    void* entry = (*static_cast<void***>(object))[index];
    MH_STATUS status = MH_CreateHook(entry, replacement, original);
    if (status == MH_OK) status = MH_EnableHook(entry);
    return status == MH_OK;
}

static HRESULT WINAPI CreateDraw(GUID* identifier, void** output, REFIID interfaceId, IUnknown* outer) {
    HRESULT result = originalCreateDraw(identifier, output, interfaceId, outer);
    if (SUCCEEDED(result) && output && *output && interfaceId == IID_IDirectDraw7) {
        if (!InstallModeFix()) return result;
        bool observed = HookMethod(*output, 21, reinterpret_cast<void*>(&SetMode),
            reinterpret_cast<void**>(&originalSetMode)) &&
            HookMethod(*output, 6, reinterpret_cast<void*>(&CreateSurface), reinterpret_cast<void**>(&originalCreateSurface));
        if (!observed) Log("Display/surface observation could not be installed.");
    }
    return result;
}

extern "C" __declspec(dllexport) int __cdecl Diagnose() {
    InitializeDirectory();
    DWORD wrapped = DWORD(0xffff0000u + DWORD(1920 * 1080 * 4));
    bool arithmetic = wrapped == 8228864 && wrapped < 1920 * 1080 * 10 &&
        MemoryBudgetFits(0xffff0000, 1, 1920, 1080, 32, true, 1920, 1080, 32) &&
        MemoryBudgetFits(64 * 1024 * 1024, 1, 1920, 1080, 32, false, 1920, 1080, 32) &&
        !MemoryBudgetFits(8 * 1024 * 1024, 1, 1920, 1080, 32, false, 1920, 1080, 32) &&
        MemoryBudgetFits(1920 * 1080 * 10, 1, 1920, 1080, 32, false, 1920, 1080, 32) &&
        !MemoryBudgetFits(1920 * 1080 * 10 - 1, 1, 1920, 1080, 32, false, 1920, 1080, 32);
    Log(std::string("Memory overflow, insufficient budget, and exact boundary checks: ") + (arithmetic ? "PASS" : "FAIL"));
    if (!arithmetic) return 2;
    HMODULE directDraw = LoadLibraryExW(L"ddraw.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    createDraw = directDraw ? reinterpret_cast<CreateDrawFunction>(GetProcAddress(directDraw, "DirectDrawCreateEx")) : nullptr;
    auto enumerate = directDraw ? reinterpret_cast<EnumerateDrawFunction>(GetProcAddress(directDraw, "DirectDrawEnumerateA")) : nullptr;
    if (!createDraw || !enumerate) return 2;
    unsigned successful = 0;
    HRESULT result = enumerate(&DiagnoseAdapter, &successful);
    bool passed = SUCCEEDED(result) && successful;
    std::cout << "Native 1080p mode and offscreen 3D surface: " << (passed ? "PASS" : "FAIL") << std::endl;
    return passed ? 0 : 2;
}

static int InitializeFeatures(bool loadCompatibility) {
    wchar_t filename[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, filename, MAX_PATH);
    const wchar_t* basename = wcsrchr(filename, L'\\');
    if (!basename || _wcsicmp(basename + 1, L"dash2.exe")) return 0;
    InitializeDirectory();
    if (loadCompatibility) {
        HMODULE compatibility = LoadLibraryW((localDirectory + L"\\LocalCompat.dll").c_str());
        using SetupFunction = int(__cdecl*)();
        auto setup = compatibility ? reinterpret_cast<SetupFunction>(GetProcAddress(compatibility, "Setup")) : nullptr;
        if (!setup || setup() != 100) return 0;
    }
    else if (!GetModuleHandleW(L"LocalCompat.dll")) return 0;
    if (MH_Initialize() != MH_OK) return 0;
    HMODULE directDraw = LoadLibraryExW(L"ddraw.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    void* entry = directDraw ? reinterpret_cast<void*>(GetProcAddress(directDraw, "DirectDrawCreateEx")) : nullptr;
    if (!entry) return 0;
    MH_STATUS status = MH_CreateHook(entry, reinterpret_cast<void*>(&CreateDraw),
        reinterpret_cast<void**>(&originalCreateDraw));
    if (status == MH_OK) status = MH_EnableHook(entry);
    Log("Opt-in native resolution extension armed; existing configuration preserved.");
    return status == MH_OK ? 100 : 0;
}

extern "C" __declspec(dllexport) int __cdecl Setup() {
    return InitializeFeatures(true);
}

extern "C" __declspec(dllexport) int __cdecl EnableFeatures() {
    return InitializeFeatures(false);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) localModule = instance;
    return TRUE;
}