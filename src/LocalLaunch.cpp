#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

struct BootstrapContext {
    void* loadLibrary;
    void* getProcAddress;
    void* dllPath;
    void* exportName;
    DWORD result;
};
static_assert(sizeof(BootstrapContext) == 20);

struct JobProcesses {
    DWORD assigned;
    DWORD count;
    ULONG_PTR identifiers[64];
};

struct CloseRequest {
    DWORD process;
    bool posted = false;
};

static BOOL CALLBACK RequestClose(HWND window, LPARAM parameter) {
    auto& request = *reinterpret_cast<CloseRequest*>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    wchar_t className[32] = {};
    GetClassNameW(window, className, 32);
    if (process != request.process || wcscmp(className, L"Dash2")) return TRUE;
    request.posted = PostMessageW(window, WM_CLOSE, 0, 0) != FALSE;
    return FALSE;
}

static std::wstring launcherLog;

static std::string Utf8(const wchar_t* text) {
    int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (!required) return {};
    std::string result(required, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), required, nullptr, nullptr);
    result.pop_back();
    return result;
}

static void LogStatus(const std::string& message) {
    HANDLE file = CreateFileW(launcherLog.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::string line = std::to_string(GetCurrentProcessId()) + " " + message + "\r\n";
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(file);
}

static BOOL CALLBACK PrintChild(HWND window, LPARAM) {
    wchar_t title[1024] = {};
    DWORD_PTR result = 0;
    SendMessageTimeoutW(window, WM_GETTEXT, 1024, reinterpret_cast<LPARAM>(title),
        SMTO_ABORTIFHUNG | SMTO_BLOCK, 200, &result);
    if (title[0]) LogStatus("Control: " + Utf8(title));
    return TRUE;
}

static BOOL CALLBACK PrintWindow(HWND window, LPARAM parameter) {
    auto* processes = reinterpret_cast<JobProcesses*>(parameter);
    DWORD identifier = 0;
    GetWindowThreadProcessId(window, &identifier);
    bool matches = false;
    for (DWORD index = 0; index < processes->count && index < 64; ++index) {
        if (processes->identifiers[index] == identifier) matches = true;
    }
    if (!matches) return TRUE;
    wchar_t title[1024] = {};
    wchar_t className[128] = {};
    GetWindowTextW(window, title, 1024);
    GetClassNameW(window, className, 128);
    LogStatus("Window PID=" + std::to_string(identifier) + " visible=" + std::to_string(IsWindowVisible(window)) +
        " class=" + Utf8(className) + " title=" + Utf8(title));
    std::cout << "Game-family window recorded for PID " << identifier << "." << std::endl;
    EnumChildWindows(window, PrintChild, 0);
    return TRUE;
}

static void RecordProcessState(HANDLE process, DWORD identifier) {
    FILETIME created = {}, exited = {}, kernel = {}, user = {};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        unsigned long long kernelTicks = (static_cast<unsigned long long>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
        unsigned long long userTicks = (static_cast<unsigned long long>(user.dwHighDateTime) << 32) | user.dwLowDateTime;
        LogStatus("CPU milliseconds=" + std::to_string((kernelTicks + userTicks) / 10000));
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, identifier);
    if (snapshot == INVALID_HANDLE_VALUE) {
        LogStatus("Module snapshot error=" + std::to_string(GetLastError()));
        return;
    }
    MODULEENTRY32W module = {};
    module.dwSize = sizeof(module);
    unsigned count = 0;
    if (Module32FirstW(snapshot, &module)) {
        do {
            std::ostringstream location;
            location << "Module: " << Utf8(module.szExePath) << " base=0x" << std::hex <<
                reinterpret_cast<uintptr_t>(module.modBaseAddr) << " size=0x" << module.modBaseSize;
            LogStatus(location.str());
        }
        while (++count < 96 && Module32NextW(snapshot, &module));
    }
    CloseHandle(snapshot);
}

static void RecordThreads(HANDLE process, DWORD identifier) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != identifier) continue;
            HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
                FALSE, entry.th32ThreadID);
            if (!thread) continue;
            DWORD suspended = SuspendThread(thread);
            if (suspended != static_cast<DWORD>(-1)) {
                CONTEXT context = {};
                context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                BOOL captured = GetThreadContext(thread, &context);
                DWORD error = captured ? 0 : GetLastError();
                DWORD stack[48] = {};
                SIZE_T returned = 0;
                if (captured) ReadProcessMemory(process, reinterpret_cast<void*>(context.Esp), stack, sizeof(stack), &returned);
                unsigned char code[96] = {};
                SIZE_T codeReturned = 0;
                DWORD codeAddress = captured && context.Eip >= 32 ? context.Eip - 32 : 0;
                if (codeAddress) ReadProcessMemory(process, reinterpret_cast<void*>(codeAddress), code, sizeof(code), &codeReturned);
                ResumeThread(thread);
                std::ostringstream details;
                details << "Thread " << entry.th32ThreadID << " captureError=" << error << " EIP=0x" <<
                    std::hex << context.Eip << " ESP=0x" << context.Esp << " EBP=0x" << context.Ebp << " stack=";
                for (unsigned index = 0; index < returned / sizeof(DWORD); ++index) details << stack[index] << ":";
                LogStatus(details.str());
                if (codeReturned) {
                    std::ostringstream instructions;
                    instructions << "Thread " << entry.th32ThreadID << " code address=0x" << std::hex << codeAddress << " bytes=";
                    for (size_t index = 0; index < codeReturned; ++index) {
                        instructions.width(2);
                        instructions.fill('0');
                        instructions << static_cast<unsigned>(code[index]);
                    }
                    LogStatus(instructions.str());
                }
                if (captured) {
                    MEMORY_BASIC_INFORMATION region = {};
                    if (VirtualQueryEx(process, reinterpret_cast<void*>(context.Eip), &region, sizeof(region))) {
                        std::ostringstream location;
                        location << "Thread " << entry.th32ThreadID << " code allocation=0x" << std::hex <<
                            reinterpret_cast<uintptr_t>(region.AllocationBase) << " offset=0x" <<
                            (context.Eip - reinterpret_cast<uintptr_t>(region.AllocationBase));
                        LogStatus(location.str());
                    }
                }
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

static bool RecordSubtitleImage(HANDLE process, DWORD identifier, const std::wstring& directory) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, identifier);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W module = {};
    module.dwSize = sizeof(module);
    bool valid = Module32FirstW(snapshot, &module) && !_wcsicmp(module.szModule, L"dash2.exe") &&
        module.modBaseSize >= 4096 && module.modBaseSize <= 32 * 1024 * 1024;
    CloseHandle(snapshot);
    if (!valid) return false;

    std::vector<unsigned char> image(module.modBaseSize);
    SIZE_T readableBytes = 0;
    for (SIZE_T offset = 0; offset < image.size(); offset += 4096) {
        MEMORY_BASIC_INFORMATION region = {};
        if (!VirtualQueryEx(process, module.modBaseAddr + offset, &region, sizeof(region)) ||
            region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD))) continue;
        SIZE_T length = image.size() - offset;
        if (length > 4096) length = 4096;
        SIZE_T returned = 0;
        ReadProcessMemory(process, module.modBaseAddr + offset, image.data() + offset, length, &returned);
        readableBytes += returned;
    }
    if (image[0] != 'M' || image[1] != 'Z') return false;
    std::wstring outputDirectory = directory + L"\\build";
    if (!CreateDirectoryW(outputDirectory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    std::wstring filename = L"subtitle-image-" + std::to_wstring(identifier) + L".bin";
    HANDLE output = CreateFileW((outputDirectory + L"\\" + filename).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool saved = WriteFile(output, image.data(), static_cast<DWORD>(image.size()), &written, nullptr) &&
        written == image.size();
    CloseHandle(output);
    if (!saved) return false;
    std::ostringstream details;
    details << "Subtitle inspection image=" << Utf8(filename.c_str()) << " base=0x" << std::hex <<
        reinterpret_cast<uintptr_t>(module.modBaseAddr) << " size=0x" << image.size() <<
        " readable=0x" << readableBytes;
    LogStatus(details.str());
    std::cout << details.str() << std::endl;
    return true;
}

static bool WriteRemote(HANDLE process, void* address, const void* data, SIZE_T size) {
    SIZE_T written = 0;
    return WriteProcessMemory(process, address, data, size, &written) && written == size;
}

int wmain(int argumentCount, wchar_t** arguments) {
    bool probe = false, selfTest = false, diagnose = false, interpreter = false;
    bool subtitleProbe = false;
    bool subtitleTest = false;
    bool xinputTest = false;
    bool resolutionTest = false;
    bool enhanced = false;
    DWORD probeSeconds = 0;
    for (int index = 1; index < argumentCount; ++index) {
        if (!wcscmp(arguments[index], L"--probe")) probe = true;
        else if (!wcscmp(arguments[index], L"--subtitle-probe")) { subtitleProbe = true; probe = true; }
        else if (!wcscmp(arguments[index], L"--subtitle-test")) subtitleTest = true;
        else if (!wcscmp(arguments[index], L"--xinput-test")) xinputTest = true;
        else if (!wcscmp(arguments[index], L"--resolution-test")) resolutionTest = true;
        else if (!wcscmp(arguments[index], L"--enhanced")) enhanced = true;
        else if (!wcscmp(arguments[index], L"--probe-seconds")) {
            if (++index >= argumentCount) return 2;
            wchar_t* end = nullptr;
            unsigned long seconds = wcstoul(arguments[index], &end, 10);
            if (!end || *end || seconds < 1 || seconds > 600) return 2;
            probeSeconds = static_cast<DWORD>(seconds);
        }
        else if (!wcscmp(arguments[index], L"--self-test")) selfTest = true;
        else if (!wcscmp(arguments[index], L"--diagnose")) diagnose = true;
        else if (!wcscmp(arguments[index], L"--interpreter")) interpreter = true;
        else return 2;
    }
    if ((selfTest || diagnose) && (probe || interpreter || (selfTest && diagnose))) return 2;
    if (probeSeconds && !probe) return 2;
    if (xinputTest && (subtitleTest || subtitleProbe || interpreter)) return 2;
    if (resolutionTest && (subtitleTest || subtitleProbe || xinputTest || interpreter)) return 2;
    if (enhanced && (subtitleTest || subtitleProbe || xinputTest || resolutionTest || interpreter)) return 2;
    bool hasResolution = resolutionTest || enhanced;

    wchar_t launcherPath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, launcherPath, MAX_PATH);
    std::wstring compatibilityDirectory = launcherPath;
    compatibilityDirectory.resize(compatibilityDirectory.find_last_of(L'\\'));
    std::wstring gameDirectory = compatibilityDirectory.substr(0, compatibilityDirectory.find_last_of(L'\\'));
    std::wstring gamePath = gameDirectory + L"\\dash2.exe";
    std::wstring dllPath = compatibilityDirectory + (subtitleTest ? L"\\SubtitleTest.dll" : L"\\LocalCompat.dll");
    if (xinputTest) dllPath = compatibilityDirectory + L"\\XInputTest.dll";
    if (resolutionTest) dllPath = compatibilityDirectory + L"\\ResolutionTest.dll";
    if (enhanced) dllPath = compatibilityDirectory + L"\\GameFeatures.dll";
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::cerr << "The local compatibility DLL is missing." << std::endl;
        return 2;
    }
    if (selfTest) {
        HMODULE module = LoadLibraryExW(dllPath.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        bool valid = module && GetProcAddress(module, "Setup");
        if (module) FreeLibrary(module);
        std::cout << "32-bit launcher and local Setup export: " << (valid ? "PASS" : "FAIL") << std::endl;
        return valid ? 0 : 2;
    }
    if (diagnose) {
        HMODULE module = LoadLibraryW(dllPath.c_str());
        using DiagnoseFunction = int(__cdecl*)();
        auto diagnostic = module ? reinterpret_cast<DiagnoseFunction>(GetProcAddress(module, "Diagnose")) : nullptr;
        if (!diagnostic) {
            if (module) FreeLibrary(module);
            std::cerr << "The local diagnostic export could not be loaded." << std::endl;
            return 2;
        }
        int result = diagnostic();
        FreeLibrary(module);
        std::cout << (enhanced ? "Combined feature diagnostics completed; game not started." :
            resolutionTest ? "Resolution diagnostics completed; game not started." :
            xinputTest ? "XInput adapter diagnostics completed; game not started." :
            subtitleTest ? "Subtitle cue and playback-clock diagnostics completed; game not started." :
            "Read-only disc diagnostics recorded in pc-compat/LocalCompat.log; game not started.") << std::endl;
        return result;
    }

    if (GetFileAttributesW(gamePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::cerr << "The adjacent original dash2.exe is missing." << std::endl;
        return 2;
    }
    std::wstring temporaryDirectory = compatibilityDirectory + L"\\temp";
    launcherLog = compatibilityDirectory + (xinputTest ? L"\\XInputLaunch.log" : L"\\LocalLaunch.log");
    if (resolutionTest) launcherLog = compatibilityDirectory + L"\\ResolutionLaunch.log";
    if (enhanced) launcherLog = compatibilityDirectory + L"\\GameLaunch.log";
    DWORD diagnosticSeconds = probeSeconds ? probeSeconds : (subtitleTest ? 30 : 15);
    LogStatus("Starting game; " + std::to_string(diagnosticSeconds) + "-second diagnostic snapshot enabled.");
    if (interpreter) {
        if (!SetEnvironmentVariableW(L"BOX64_DYNAREC", L"0") ||
            !SetEnvironmentVariableW(L"BOX64_IGNOREINT3", L"0")) {
            LogStatus("Cannot set the child-process Box64 diagnostic options.");
            return 2;
        }
        LogStatus("Requested child environment: BOX64_DYNAREC=0 BOX64_IGNOREINT3=0");
        std::cout << "Requesting Box64 interpreter and breakpoint delivery for this game launch only." << std::endl;
    }
    if (!CreateDirectoryW(temporaryDirectory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return 2;
    SetEnvironmentVariableW(L"TEMP", temporaryDirectory.c_str());
    SetEnvironmentVariableW(L"TMP", temporaryDirectory.c_str());

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        if (job) CloseHandle(job);
        return 2;
    }
    HANDLE resolutionQuit = nullptr;
    if (hasResolution) {
        std::wstring eventName = L"Local\\RockmanResolutionQuit-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64());
        resolutionQuit = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        if (!resolutionQuit || GetLastError() == ERROR_ALREADY_EXISTS ||
            !SetEnvironmentVariableW(L"ROCKMAN_RESOLUTION_QUIT_EVENT", eventName.c_str())) {
            if (resolutionQuit) CloseHandle(resolutionQuit);
            CloseHandle(job);
            return 2;
        }
    }
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    std::wstring commandLine = L"\"" + gamePath + L"\"";
    if (!CreateProcessW(gamePath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED, nullptr, gameDirectory.c_str(), &startup, &process)) {
        std::cerr << "Game creation failed: " << GetLastError() << std::endl;
        if (resolutionQuit) CloseHandle(resolutionQuit);
        CloseHandle(job);
        return 2;
    }
    auto finish = [&]() {
        CloseHandle(job);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        if (resolutionQuit) CloseHandle(resolutionQuit);
    };
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 2);
        finish();
        return 2;
    }

    const unsigned char bootstrap[] = {
        0x55, 0x89, 0xe5, 0x53, 0x8b, 0x5d, 0x08, 0xff, 0x73, 0x08,
        0xff, 0x13, 0x85, 0xc0, 0x74, 0x10, 0xff, 0x73, 0x0c, 0x50,
        0xff, 0x53, 0x04, 0x85, 0xc0, 0x74, 0x05, 0xff, 0xd0, 0x89,
        0x43, 0x10, 0x5b, 0x89, 0xec, 0x5d, 0xc2, 0x04, 0x00
    };
    SIZE_T pathSize = (dllPath.size() + 1) * sizeof(wchar_t);
    SIZE_T dataSize = sizeof(BootstrapContext) + pathSize + sizeof("Setup");
    auto* remoteData = static_cast<unsigned char*>(VirtualAllocEx(process.hProcess, nullptr,
        dataSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    void* remoteCode = VirtualAllocEx(process.hProcess, nullptr, sizeof(bootstrap),
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteData || !remoteCode) { finish(); return 2; }
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    BootstrapContext context = {
        reinterpret_cast<void*>(GetProcAddress(kernel, "LoadLibraryW")),
        reinterpret_cast<void*>(GetProcAddress(kernel, "GetProcAddress")),
        remoteData + sizeof(BootstrapContext),
        remoteData + sizeof(BootstrapContext) + pathSize,
        0
    };
    DWORD oldProtection = 0;
    bool prepared = WriteRemote(process.hProcess, remoteData, &context, sizeof(context)) &&
        WriteRemote(process.hProcess, context.dllPath, dllPath.c_str(), pathSize) &&
        WriteRemote(process.hProcess, context.exportName, "Setup", sizeof("Setup")) &&
        WriteRemote(process.hProcess, remoteCode, bootstrap, sizeof(bootstrap)) &&
        VirtualProtectEx(process.hProcess, remoteCode, sizeof(bootstrap), PAGE_EXECUTE_READ, &oldProtection) &&
        FlushInstructionCache(process.hProcess, remoteCode, sizeof(bootstrap)) &&
        QueueUserAPC(reinterpret_cast<PAPCFUNC>(remoteCode), process.hThread, reinterpret_cast<ULONG_PTR>(remoteData));
    if (!prepared || ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        std::cerr << "Local initialization failed: " << GetLastError() << std::endl;
        finish();
        return 2;
    }
    std::cout << "Original game started as PID " << process.dwProcessId << "." << std::endl;
    LogStatus("Game PID=" + std::to_string(process.dwProcessId));
    DWORD waitResult = WaitForSingleObject(process.hProcess, diagnosticSeconds * 1000);
    if (waitResult == WAIT_TIMEOUT) {
        BootstrapContext remoteContext = {};
        SIZE_T bytesRead = 0;
        if (ReadProcessMemory(process.hProcess, remoteData, &remoteContext, sizeof(remoteContext), &bytesRead)) {
            std::cout << "Compatibility initialization result: " << remoteContext.result << std::endl;
            LogStatus("Compatibility initialization result=" + std::to_string(remoteContext.result));
        }
        if ((xinputTest || hasResolution) && (bytesRead != sizeof(remoteContext) || remoteContext.result != 100)) {
            std::cerr << (enhanced ? "Combined initialization failed; see pc-compat/GameFeatures.log." :
                resolutionTest ? "Resolution initialization failed; see pc-compat/ResolutionTest.log." :
                "XInput initialization failed; see pc-compat/XInputTest.log.") << std::endl;
            finish();
            return 2;
        }
        JobProcesses processes = {};
        QueryInformationJobObject(job, JobObjectBasicProcessIdList, &processes, sizeof(processes), nullptr);
        LogStatus(std::to_string(diagnosticSeconds) + " seconds; active game-family processes=" + std::to_string(processes.count));
        RecordProcessState(process.hProcess, process.dwProcessId);
        RecordThreads(process.hProcess, process.dwProcessId);
        EnumWindows(PrintWindow, reinterpret_cast<LPARAM>(&processes));
        if (subtitleProbe && !RecordSubtitleImage(process.hProcess, process.dwProcessId, compatibilityDirectory)) {
            std::cerr << "Subtitle inspection image could not be captured." << std::endl;
            finish();
            return 2;
        }
        if (probe) {
            if (hasResolution) {
                CloseRequest request = {process.dwProcessId};
                EnumWindows(&RequestClose, reinterpret_cast<LPARAM>(&request));
                bool quit = request.posted &&
                    WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0 &&
                    WaitForSingleObject(resolutionQuit, 0) == WAIT_OBJECT_0;
                LogStatus(std::string("Resolution probe native shutdown/save=") + (quit ? "PASS" : "FAIL"));
                if (!quit) {
                    finish();
                    std::cerr << "Resolution probe could not complete native shutdown." << std::endl;
                    return 2;
                }
            }
            finish();
            std::cout << "Probe processes closed." << std::endl;
            return 3;
        }
        std::cout << "Startup snapshot saved to pc-compat/" <<
            (enhanced ? "GameLaunch.log" : resolutionTest ? "ResolutionLaunch.log" : xinputTest ? "XInputLaunch.log" : "LocalLaunch.log") <<
            ". Waiting for game exit." << std::endl;
        waitResult = WaitForSingleObject(process.hProcess, INFINITE);
    }
    DWORD exitCode = 2;
    if (waitResult == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exitCode);
    std::cout << "Game exit code: " << exitCode << std::endl;
    LogStatus("Game exit code=" + std::to_string(exitCode));
    if (hasResolution && exitCode == 1 && WaitForSingleObject(resolutionQuit, 0) == WAIT_OBJECT_0) {
        LogStatus("Recognized native normal exit after configuration save.");
        exitCode = 0;
    }
    finish();
    return static_cast<int>(exitCode);
}