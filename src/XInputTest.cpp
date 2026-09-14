#define DIRECTINPUT_VERSION 0x0700
#include <windows.h>
#include <dinput.h>
#include <xinput.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

using CreateInputFunction = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
using EnumDevicesFunction = HRESULT(WINAPI*)(void*, DWORD, LPDIENUMDEVICESCALLBACKA, void*, DWORD);
using CreateDeviceFunction = HRESULT(WINAPI*)(void*, REFGUID, REFIID, void**, IUnknown*);
using GetDeviceStateFunction = HRESULT(WINAPI*)(void*, DWORD, void*);
using GetXInputStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using GetButtonFunction = int(__cdecl*)(unsigned);
using DrawBindingsFunction = int(__cdecl*)(void*, int);
using DrawMenuTextFunction = void(__cdecl*)(const char*, int, int, int);
static_assert(sizeof(DIJOYSTATE) == 80);

static HMODULE localModule = nullptr;
static HMODULE adapterModule = nullptr;
static std::wstring localDirectory;
static CreateInputFunction adapterCreate = nullptr;
static CreateInputFunction originalCreate = nullptr;
static EnumDevicesFunction originalEnumDevices = nullptr;
static CreateDeviceFunction originalCreateDevice = nullptr;
static GetDeviceStateFunction originalGetDeviceState = nullptr;
static GetDeviceStateFunction originalGetMouseState = nullptr;
static GetXInputStateFunction getXInputState = nullptr;
static void* inputEntry = nullptr;
static std::atomic<unsigned> redirectedCreates = 0;
static std::atomic<unsigned> stateTransitions = 0;
static std::atomic<unsigned> mouseSamples = 0;
static std::atomic<DWORD> previousState = 0xffffffff;
static std::atomic<ULONGLONG> lastConnectionCheck = 0;
static std::atomic<DWORD> previousConnections = 0xffffffff;
static thread_local bool forwardingCreate = false;
static GetButtonFunction originalGetButton = nullptr;
static DrawBindingsFunction originalDrawBindings = nullptr;
static std::array<std::string, 17> xboxButtonNames;
static constexpr std::array<uint16_t, 8> controllerMenuActions = {
    0x400, 0x800, 0x200, 0x4000, 0x8000, 0x1000, 0x2000, 0x100
};

static void InitializeDirectory() {
    wchar_t filename[MAX_PATH] = {};
    GetModuleFileNameW(localModule, filename, MAX_PATH);
    localDirectory = filename;
    localDirectory.resize(localDirectory.find_last_of(L'\\'));
}

static void Log(const std::string& message) {
    HANDLE output = CreateFileW((localDirectory + L"\\XInputTest.log").c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return;
    std::string line = std::to_string(GetCurrentProcessId()) + " " + message + "\r\n";
    DWORD written = 0;
    WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(output);
}

template<typename Value>
static bool ReadGame(uintptr_t address, Value& value) {
    SIZE_T returned = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value,
        sizeof(value), &returned) && returned == sizeof(value);
}

static bool WritableCommands(uintptr_t address, size_t length) {
    MEMORY_BASIC_INFORMATION region = {};
    if (!length || !VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) ||
        region.State != MEM_COMMIT || region.Protect & PAGE_GUARD ||
        !(region.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    uintptr_t end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    return address <= end && length <= end - address;
}

static const char* ButtonName(int button) {
    return button > 0 && button < int(xboxButtonNames.size()) && !xboxButtonNames[button].empty() ?
        xboxButtonNames[button].c_str() : nullptr;
}

static bool LoadButtonNames() {
    xboxButtonNames = {};
    std::wstring filename = localDirectory + L"\\xinput\\Xidi.ini";
    wchar_t mapper[64] = {}, playerMapper[64] = {}, templateName[64] = {};
    GetPrivateProfileStringW(L"Mapper", L"Type", L"", mapper, 64, filename.c_str());
    GetPrivateProfileStringW(L"Mapper", L"Type.1", mapper, playerMapper, 64, filename.c_str());
    const wchar_t* section = L"CustomMapper:RockmanDash2";
    GetPrivateProfileStringW(section, L"Template", L"", templateName, 64, filename.c_str());
    if (_wcsicmp(playerMapper, L"RockmanDash2") || _wcsicmp(templateName, L"StandardGamepad")) {
        Log("Unknown controller mapper; original numeric menu labels retained.");
        return false;
    }
    struct PhysicalButton { const wchar_t* key; const char* name; unsigned defaultNumber; };
    const PhysicalButton controls[] = {
        {L"ButtonA", "A", 1}, {L"ButtonB", "B", 2}, {L"ButtonX", "X", 3}, {L"ButtonY", "Y", 4},
        {L"ButtonLB", "LB", 5}, {L"ButtonRB", "RB", 6}, {L"TriggerLT", "LT", 7}, {L"TriggerRT", "RT", 8},
        {L"ButtonBack", "View", 9}, {L"ButtonStart", "Menu", 10}, {L"ButtonLS", "LS", 11}, {L"ButtonRS", "RS", 12}
    };
    std::array<bool, 17> ambiguous = {};
    for (const auto& control : controls) {
        std::wstring fallback = L"Button(" + std::to_wstring(control.defaultNumber) + L")";
        wchar_t mapping[96] = {};
        GetPrivateProfileStringW(section, control.key, fallback.c_str(), mapping, 96, filename.c_str());
        CharLowerBuffW(mapping, DWORD(wcslen(mapping)));
        unsigned number = 0;
        int consumed = 0;
        if (swscanf(mapping, L"button ( %u ) %n", &number, &consumed) != 1 || !consumed || mapping[consumed] ||
            !number || number >= xboxButtonNames.size()) {
            xboxButtonNames = {};
            Log("Controller mapper uses a non-button expression; numeric menu labels retained.");
            return false;
        }
        if (ambiguous[number]) continue;
        if (!xboxButtonNames[number].empty()) {
            ambiguous[number] = true;
            xboxButtonNames[number].clear();
        }
        else xboxButtonNames[number] = control.name;
    }
    return true;
}

struct ButtonSprite {
    uint32_t link;
    uint32_t command;
    uint32_t position;
    uint32_t texture;
    uint32_t dimensions;
};
static_assert(sizeof(ButtonSprite) == 20);

static uint32_t ButtonPosition(unsigned index) {
    unsigned cell = index + 1;
    return (130 + (cell / 5) * 132) | ((62 + (cell % 5) * 16) << 16);
}

static bool MatchesButtonSprite(const ButtonSprite& sprite, unsigned index, uint16_t texture) {
    return index < controllerMenuActions.size() && sprite.command == 0x8c808080 &&
        sprite.position == ButtonPosition(index) && sprite.texture == (0x3fdc0000u | texture) &&
        sprite.dimensions == 0x00100010;
}

static int __cdecl DrawBindings(void* state, int selection) {
    uint32_t start = 0;
    ReadGame(0x6d004c, start);
    int result = originalDrawBindings(state, selection);
    uint8_t mode = 255;
    uint32_t end = 0;
    if (!ReadGame(reinterpret_cast<uintptr_t>(state) + 0x14, mode) || mode != 0 ||
        !ReadGame(0x6d004c, end) || end <= start || end - start > 16384 || start % 4 || end % 4 ||
        !WritableCommands(start, size_t(end - start) + 8 * 4 * 24)) return result;
    std::array<ButtonSprite*, 8> sprites = {};
    std::array<int, 8> buttons = {};
    for (unsigned index = 0; index < sprites.size(); ++index) {
        buttons[index] = originalGetButton(controllerMenuActions[index]);
        if (buttons[index] < 1 || buttons[index] > 16) return result;
        uint16_t texture = 0;
        if (!ReadGame(0x918ed8 + (buttons[index] - 1) * 2, texture)) return result;
        for (uintptr_t address = start; address + sizeof(ButtonSprite) <= end; address += 4) {
            auto* sprite = reinterpret_cast<ButtonSprite*>(address);
            if (MatchesButtonSprite(*sprite, index, texture)) {
                if (sprites[index]) return result;
                sprites[index] = sprite;
            }
        }
        if (!sprites[index]) return result;
    }
    unsigned drawn = 0;
    static std::array<int, 8> previousButtons = {};
    for (unsigned index = 0; index < sprites.size(); ++index) {
        const char* name = ButtonName(buttons[index]);
        if (previousButtons[index] != buttons[index]) {
            previousButtons[index] = buttons[index];
            Log("Controller menu action=" + std::to_string(controllerMenuActions[index]) + " button=" +
                std::to_string(buttons[index]) + " Xbox=" + (name ? name : "numeric"));
        }
        if (!name) continue;
        float width = 0;
        bool metricsValid = true;
        for (const char* character = name; *character; ++character) {
            int8_t advance = 0;
            if (!ReadGame(0x8b085d + static_cast<unsigned char>(*character) * 2, advance) || advance <= 0 || advance > 32) {
                metricsValid = false;
                break;
            }
            width += advance * 0.5f + 1.0f;
        }
        if (!metricsValid || width > 32) continue;
        int horizontal = int(sprites[index]->position & 0xffff) + 8 - int(width * 0.5f);
        int vertical = int(sprites[index]->position >> 16) + 2;
        reinterpret_cast<DrawMenuTextFunction>(0x55bcc0)(name, horizontal, vertical, 1);
        sprites[index]->dimensions = 0;
        ++drawn;
    }
    static unsigned previousDrawn = 0;
    if (drawn != previousDrawn) {
        previousDrawn = drawn;
        Log("Native Xbox menu labels drawn=" + std::to_string(drawn));
    }
    return result;
}

static void InstallButtonLabels() {
    if (originalDrawBindings) return;
    wchar_t executable[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const wchar_t* basename = wcsrchr(executable, L'\\');
    if (!basename || _wcsicmp(basename + 1, L"dash2.exe") ||
        GetModuleHandleW(nullptr) != reinterpret_cast<HMODULE>(0x400000)) return;
    const std::array<unsigned char, 12> expected = {
        0x8b,0x54,0x24,0x04,0x56,0x33,0xc9,0xb8,0x72,0xad,0xa2,0x00
    };
    const std::array<unsigned char, 5> callerExpected = {0xe8,0xd0,0xa9,0xe1,0xff};
    const std::array<unsigned char, 15> drawExpected = {
        0x8b,0x44,0x24,0x04,0x83,0xec,0x08,0x0f,0xbe,0x40,0x14,0x53,0x55,0x56,0x57
    };
    const std::array<unsigned char, 16> textExpected = {
        0xdb,0x44,0x24,0x08,0x55,0x8b,0x6c,0x24,0x08,0x8a,0x45,0x00,0xd9,0x5c,0x24,0x0c
    };
    std::array<unsigned char, 12> actual = {};
    std::array<unsigned char, 5> caller = {};
    std::array<unsigned char, 15> draw = {};
    std::array<unsigned char, 16> text = {};
    std::array<uint16_t, 8> actions = {};
    SIZE_T read = 0;
    bool matches = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(0x40a260),
        actual.data(), actual.size(), &read) && read == actual.size() && actual == expected &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(0x5ef88b),
        caller.data(), caller.size(), &read) && read == caller.size() && caller == callerExpected &&
        ReadGame(0x5ef1f0, draw) && draw == drawExpected && ReadGame(0x55bcc0, text) && text == textExpected &&
        ReadGame(0x918ec6, actions) && actions == controllerMenuActions;
    if (!matches) {
        Log("Controller menu signature mismatch; original button display retained.");
        return;
    }
    originalGetButton = reinterpret_cast<GetButtonFunction>(0x40a260);
    MH_STATUS status = MH_CreateHook(reinterpret_cast<void*>(0x5ef1f0), reinterpret_cast<void*>(&DrawBindings),
        reinterpret_cast<void**>(&originalDrawBindings));
    if (status == MH_OK) status = MH_EnableHook(reinterpret_cast<void*>(0x5ef1f0));
    Log("Native Xbox controller menu hook status=" + std::to_string(status));
}

static bool LoadAdapter() {
    if (adapterCreate) return true;
    std::wstring corePath = localDirectory + L"\\xinput\\Xidi.32.dll";
    HMODULE core = LoadLibraryExW(corePath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!core) {
        Log("Xidi core load failed; Windows error=" + std::to_string(GetLastError()));
        return false;
    }
    std::wstring adapterPath = localDirectory + L"\\xinput\\dinput.dll";
    adapterModule = LoadLibraryExW(adapterPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    adapterCreate = adapterModule ? reinterpret_cast<CreateInputFunction>(
        GetProcAddress(adapterModule, "DirectInputCreateEx")) : nullptr;
    if (!adapterCreate) Log("Private DirectInput adapter load failed; Windows error=" + std::to_string(GetLastError()));
    return adapterCreate != nullptr;
}

static bool HookMethod(void* object, unsigned index, void* replacement, void** original) {
    if (*original) return true;
    void* entry = (*static_cast<void***>(object))[index];
    MH_STATUS status = MH_CreateHook(entry, replacement, original);
    if (status == MH_OK) status = MH_EnableHook(entry);
    if (status != MH_OK) {
        *original = nullptr;
        Log("Controller method hook failed=" + std::to_string(status));
        return false;
    }
    return true;
}

struct ControllerEnumeration {
    LPDIENUMDEVICESCALLBACKA callback;
    void* context;
    unsigned count = 0;
};

static BOOL CALLBACK FirstVirtualController(const DIDEVICEINSTANCEA* instance, void* context) {
    if (std::strncmp(instance->tszInstanceName, "Xidi", 4)) return DIENUM_CONTINUE;
    auto* enumeration = static_cast<ControllerEnumeration*>(context);
    ++enumeration->count;
    enumeration->callback(instance, enumeration->context);
    return DIENUM_STOP;
}

static HRESULT WINAPI EnumDevices(void* object, DWORD type, LPDIENUMDEVICESCALLBACKA callback,
    void* context, DWORD flags) {
    if (type != DIDEVTYPE_JOYSTICK || !callback) return originalEnumDevices(object, type, callback, context, flags);
    ControllerEnumeration enumeration = {callback, context};
    HRESULT result = originalEnumDevices(object, type, &FirstVirtualController, &enumeration, flags);
    Log("Game-visible virtual controller count=" + std::to_string(enumeration.count));
    return result;
}

static HRESULT WINAPI GetDeviceState(void* object, DWORD size, void* output) {
    HRESULT result = originalGetDeviceState(object, size, output);
    ULONGLONG now = GetTickCount64();
    ULONGLONG previous = lastConnectionCheck.load();
    if (getXInputState && now - previous >= 500 && lastConnectionCheck.compare_exchange_strong(previous, now)) {
        DWORD connections = 0;
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            XINPUT_STATE state = {};
            if (getXInputState(slot, &state) == ERROR_SUCCESS) connections |= DWORD(1) << slot;
        }
        if (previousConnections.exchange(connections) != connections) {
            stateTransitions = 0;
            previousState = 0xffffffff;
            Log("Physical XInput connected mask=" + std::to_string(connections));
        }
    }
    if (SUCCEEDED(result) && size == sizeof(DIJOYSTATE) && output) {
        const auto& state = *static_cast<DIJOYSTATE*>(output);
        DWORD buttons = 0;
        for (unsigned index = 0; index < 16; ++index) {
            if (state.rgbButtons[index] & 0x80) buttons |= DWORD(1) << index;
        }
        DWORD directions = (state.lX > 500 ? 1u : 0u) | (state.lX < -500 ? 2u : 0u) |
            (state.lY > 500 ? 4u : 0u) | (state.lY < -500 ? 8u : 0u);
        DWORD packed = buttons | (directions << 16);
        if (previousState.exchange(packed) != packed && stateTransitions.fetch_add(1) < 64) {
            Log("Joystick state buttons=" + std::to_string(buttons) + " directions=" + std::to_string(directions));
        }
    }
    return result;
}

static HRESULT WINAPI GetMouseState(void* object, DWORD size, void* output) {
    HRESULT result = originalGetMouseState(object, size, output);
    if (SUCCEEDED(result) && size == sizeof(DIMOUSESTATE) && output && mouseSamples < 24) {
        const auto& state = *static_cast<DIMOUSESTATE*>(output);
        DWORD foregroundProcess = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
        if ((state.lX || state.lY) && foregroundProcess == GetCurrentProcessId() && mouseSamples.fetch_add(1) < 24) {
            Log("Game mouse-look delta x=" + std::to_string(state.lX) + " y=" + std::to_string(state.lY));
        }
    }
    return result;
}

static HRESULT WINAPI CreateDevice(void* object, REFGUID deviceId, REFIID interfaceId,
    void** output, IUnknown* outer) {
    HRESULT result = originalCreateDevice(object, deviceId, interfaceId, output, outer);
    if (SUCCEEDED(result) && output && *output && interfaceId == IID_IDirectInputDevice7A &&
        deviceId != GUID_SysKeyboard && deviceId != GUID_SysMouse) {
        DIDEVICEINSTANCEA instance = {};
        instance.dwSize = sizeof(instance);
        auto* device = static_cast<IDirectInputDevice7A*>(*output);
        if (SUCCEEDED(device->GetDeviceInfo(&instance)) && !std::strncmp(instance.tszInstanceName, "Xidi", 4)) {
            bool hooked = HookMethod(*output, 9, reinterpret_cast<void*>(&GetDeviceState),
                reinterpret_cast<void**>(&originalGetDeviceState));
            Log(std::string("Virtual controller created; state observer=") + (hooked ? "ready" : "unavailable"));
        }
    }
    if (SUCCEEDED(result) && output && *output && interfaceId == IID_IDirectInputDevice7A &&
        deviceId == GUID_SysMouse) {
        bool hooked = HookMethod(*output, 9, reinterpret_cast<void*>(&GetMouseState),
            reinterpret_cast<void**>(&originalGetMouseState));
        Log(std::string("Native mouse passthrough; state observer=") + (hooked ? "ready" : "unavailable"));
    }
    return result;
}

static HRESULT WINAPI CreateInput(HINSTANCE instance, DWORD version, REFIID identifier,
    void** output, IUnknown* outer) {
    if (forwardingCreate || version != 0x0700 || identifier != IID_IDirectInput7A ||
        instance != GetModuleHandleW(nullptr)) {
        return originalCreate(instance, version, identifier, output, outer);
    }
    forwardingCreate = true;
    HRESULT result = adapterCreate(instance, version, identifier, output, outer);
    forwardingCreate = false;
    if (SUCCEEDED(result) && output && *output) {
        bool hooked = HookMethod(*output, 4, reinterpret_cast<void*>(&EnumDevices),
            reinterpret_cast<void**>(&originalEnumDevices)) &&
            HookMethod(*output, 9, reinterpret_cast<void*>(&CreateDevice),
                reinterpret_cast<void**>(&originalCreateDevice));
        if (!hooked) {
            static_cast<IDirectInput7A*>(*output)->Release();
            *output = nullptr;
            return E_FAIL;
        }
    }
    unsigned count = ++redirectedCreates;
    if (count <= 4) Log("DirectInput7 -> Xidi result=" + std::to_string(result));
    if (SUCCEEDED(result)) InstallButtonLabels();
    return result;
}

static bool InstallBridge() {
    if (!LoadAdapter()) return false;
    wchar_t systemDirectory[MAX_PATH] = {};
    UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (!length || length >= MAX_PATH) return false;
    std::wstring systemPath = std::wstring(systemDirectory) + L"\\dinput.dll";
    HMODULE systemInput = LoadLibraryExW(systemPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!systemInput || systemInput == adapterModule) return false;
    HMODULE xinput = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    getXInputState = xinput ? reinterpret_cast<GetXInputStateFunction>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
    if (!getXInputState) return false;
    inputEntry = reinterpret_cast<void*>(GetProcAddress(systemInput, "DirectInputCreateEx"));
    if (!inputEntry || MH_Initialize() != MH_OK) return false;
    MH_STATUS status = MH_CreateHook(inputEntry, reinterpret_cast<void*>(&CreateInput),
        reinterpret_cast<void**>(&originalCreate));
    if (status == MH_OK) status = MH_EnableHook(inputEntry);
    if (status != MH_OK) {
        Log("DirectInput bridge installation failed=" + std::to_string(status));
        MH_Uninitialize();
        return false;
    }
    Log("Opt-in XInput bridge installed; system files and game bindings unchanged.");
    return true;
}

static BOOL CALLBACK CollectController(const DIDEVICEINSTANCEA* instance, void* context) {
    auto* controllers = static_cast<std::vector<DIDEVICEINSTANCEA>*>(context);
    if (!std::strncmp(instance->tszInstanceName, "Xidi", 4)) controllers->push_back(*instance);
    return DIENUM_CONTINUE;
}

extern "C" __declspec(dllexport) int __cdecl Diagnose() {
    InitializeDirectory();
    bool profile = LoadButtonNames();
    ButtonSprite sprite = {0, 0x8c808080, ButtonPosition(0), 0x3fdc2060, 0x00100010};
    bool labelsValid = MatchesButtonSprite(sprite, 0, 0x2060) && !MatchesButtonSprite(sprite, 1, 0x2060) &&
        !MatchesButtonSprite(sprite, 0, 0x2061) && !ButtonName(0) && !ButtonName(17);
    sprite.dimensions = 0;
    labelsValid = labelsValid && !MatchesButtonSprite(sprite, 0, 0x2060);
    Log(std::string("Menu sprite scope and button bounds: ") + (labelsValid ? "PASS" : "FAIL") +
        "; profile=" + (profile ? "Xbox labels" : "numeric fallback"));
    if (!labelsValid) return 2;
    Log("Starting isolated DirectInput7 / XInput diagnostics.");
    if (!InstallBridge()) return 2;
    IDirectInput7A* input = nullptr;
    auto create = reinterpret_cast<CreateInputFunction>(inputEntry);
    HRESULT created = create(GetModuleHandleW(nullptr), 0x0700, IID_IDirectInput7A,
        reinterpret_cast<void**>(&input), nullptr);
    bool passed = SUCCEEDED(created) && input && redirectedCreates > 0;
    std::vector<DIDEVICEINSTANCEA> controllers;
    HWND window = CreateWindowExW(0, L"STATIC", L"Rockman XInput diagnostics", WS_OVERLAPPED,
        0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (passed) {
        HRESULT enumerated = input->EnumDevices(DIDEVTYPE_JOYSTICK, &CollectController,
            &controllers, DIEDFL_ATTACHEDONLY);
        passed = SUCCEEDED(enumerated) && controllers.size() == 1 && window;
        Log("Attached virtual controllers=" + std::to_string(controllers.size()));
        for (const auto& controller : controllers) {
            IDirectInputDevice7A* device = nullptr;
            HRESULT result = input->CreateDeviceEx(controller.guidInstance, IID_IDirectInputDevice7A,
                reinterpret_cast<void**>(&device), nullptr);
            bool valid = SUCCEEDED(result) && device;
            if (valid) {
                valid = SUCCEEDED(device->SetDataFormat(&c_dfDIJoystick));
                DIPROPRANGE range = {};
                range.diph.dwSize = sizeof(range);
                range.diph.dwHeaderSize = sizeof(range.diph);
                range.diph.dwHow = DIPH_BYOFFSET;
                range.lMin = -1000;
                range.lMax = 1000;
                for (DWORD offset : {DWORD(DIJOFS_X), DWORD(DIJOFS_Y)}) {
                    range.diph.dwObj = offset;
                    valid = SUCCEEDED(device->SetProperty(DIPROP_RANGE, &range.diph)) && valid;
                }
                valid = SUCCEEDED(device->SetCooperativeLevel(window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE)) && valid;
                valid = SUCCEEDED(device->Acquire()) && valid;
                DIJOYSTATE state = {};
                valid = SUCCEEDED(device->Poll()) && valid;
                result = device->GetDeviceState(sizeof(state), &state);
                valid = SUCCEEDED(result) && state.lX >= -1000 && state.lX <= 1000 &&
                    state.lY >= -1000 && state.lY <= 1000 && valid;
                device->Unacquire();
                device->Release();
            }
            Log(std::string(controller.tszInstanceName) + " 80-byte state/range: " + (valid ? "PASS" : "FAIL"));
            passed = valid && passed;
        }
        IDirectInputDevice7A* keyboard = nullptr;
        HRESULT result = input->CreateDeviceEx(GUID_SysKeyboard, IID_IDirectInputDevice7A,
            reinterpret_cast<void**>(&keyboard), nullptr);
        bool keyboardValid = SUCCEEDED(result) && keyboard;
        if (keyboard) {
            keyboardValid = SUCCEEDED(keyboard->SetDataFormat(&c_dfDIKeyboard)) && keyboardValid;
            keyboard->Release();
        }
        Log(std::string("Native keyboard passthrough: ") + (keyboardValid ? "PASS" : "FAIL"));
        passed = keyboardValid && passed;
        IDirectInputDevice7A* mouse = nullptr;
        result = input->CreateDeviceEx(GUID_SysMouse, IID_IDirectInputDevice7A,
            reinterpret_cast<void**>(&mouse), nullptr);
        bool mouseValid = SUCCEEDED(result) && mouse && originalGetMouseState;
        if (mouse) {
            mouseValid = SUCCEEDED(mouse->SetDataFormat(&c_dfDIMouse)) && mouseValid;
            mouse->Release();
        }
        Log(std::string("Native mouse passthrough: ") + (mouseValid ? "PASS" : "FAIL"));
        passed = mouseValid && passed;
    }
    if (input) input->Release();
    if (window) DestroyWindow(window);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    HMODULE xinput = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    using GetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    auto getState = xinput ? reinterpret_cast<GetStateFunction>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
    unsigned connected = 0;
    if (getState) {
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            XINPUT_STATE state = {};
            if (getState(slot, &state) == ERROR_SUCCESS) {
                ++connected;
                Log("Physical XInput controller slot=" + std::to_string(slot));
            }
        }
    }
    if (xinput) FreeLibrary(xinput);
    Log("Physical XInput controllers connected=" + std::to_string(connected));
    Log(std::string("XInput adapter diagnostics: ") + (passed ? "PASS" : "FAIL"));
    std::cout << "DirectInput7 adapter/joystick/keyboard/mouse: " << (passed ? "PASS" : "FAIL") <<
        "; physical XInput controllers: " << connected << std::endl;
    return passed ? 0 : 2;
}

static int InitializeFeatures(bool loadCompatibility) {
    wchar_t filename[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, filename, MAX_PATH);
    const wchar_t* basename = wcsrchr(filename, L'\\');
    if (!basename || _wcsicmp(basename + 1, L"dash2.exe")) return 0;
    InitializeDirectory();
    LoadButtonNames();
    if (loadCompatibility) {
        HMODULE compatibility = LoadLibraryW((localDirectory + L"\\LocalCompat.dll").c_str());
        using SetupFunction = int(__cdecl*)();
        auto setup = compatibility ? reinterpret_cast<SetupFunction>(GetProcAddress(compatibility, "Setup")) : nullptr;
        if (!setup || setup() != 100) return 0;
    }
    else if (!GetModuleHandleW(L"LocalCompat.dll")) return 0;
    return InstallBridge() ? 100 : 0;
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