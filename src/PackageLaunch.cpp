#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cwctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

struct PayloadAsset {
    unsigned identifier;
    const wchar_t* path;
    const char* sha256;
    bool editable;
};

#include "Payload.generated.h"

class Handle {
public:
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { Close(); }
    void Close() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = INVALID_HANDLE_VALUE;
    }
};

class Sha256 {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> object;
public:
    Sha256() {
        DWORD objectLength = 0, returned = 0;
        NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (status >= 0) status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &returned, 0);
        if (status >= 0) {
            object.resize(objectLength);
            status = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0);
        }
        if (status < 0) {
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
            throw std::runtime_error("Cannot initialize SHA-256.");
        }
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    ~Sha256() {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    void Add(const void* bytes, DWORD length) {
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(bytes)), length, 0) < 0)
            throw std::runtime_error("SHA-256 input failed.");
    }
    std::string Finish() {
        std::array<UCHAR, 32> digest = {};
        if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
            throw std::runtime_error("SHA-256 finalization failed.");
        static constexpr char digits[] = "0123456789abcdef";
        std::string result;
        for (UCHAR value : digest) {
            result.push_back(digits[value >> 4]);
            result.push_back(digits[value & 15]);
        }
        return result;
    }
};

static void FailWindows(const char* message) {
    throw std::runtime_error(std::string(message) + " Windows error " + std::to_string(GetLastError()) + ".");
}

static void PrintRuntimeDirectory(const std::wstring& directory) {
    std::wstring message = L"Prepared runtime: " + directory + L"\r\n";
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0, written = 0;
    if (GetConsoleMode(output, &mode)) {
        if (!WriteConsoleW(output, message.data(), static_cast<DWORD>(message.size()), &written, nullptr))
            FailWindows("Cannot write the runtime location.");
        return;
    }
    int required = WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(required, '\0');
    if (!required || !WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), bytes.data(), required, nullptr, nullptr) ||
        !WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
        FailWindows("Cannot write the runtime location.");
}

static std::wstring FullPath(const std::wstring& path) {
    wchar_t buffer[MAX_PATH] = {};
    DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);
    if (!length || length >= MAX_PATH) throw std::runtime_error("Use a shorter, valid game directory path.");
    std::wstring result(buffer, length);
    while (result.size() > 3 && (result.back() == L'\\' || result.back() == L'/')) result.pop_back();
    return result;
}

static bool FileExists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return false;
        FailWindows("Cannot inspect a runtime file.");
    }
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Expected a regular file, not a directory or reparse point.");
    return true;
}

static void EnsureDirectory(const std::wstring& path) {
    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        FailWindows("Cannot create the private runtime directory. Use a writable game folder.");
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("Runtime directories must not be links or reparse points.");
}

static std::string HashFile(const std::wstring& path) {
    if (!FileExists(path)) throw std::runtime_error("A required file is missing.");
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) FailWindows("Cannot read a required file.");
    Sha256 hash;
    std::array<unsigned char, 65536> buffer = {};
    for (;;) {
        DWORD received = 0;
        if (!ReadFile(file.value, buffer.data(), static_cast<DWORD>(buffer.size()), &received, nullptr))
            FailWindows("Cannot read a required file.");
        if (!received) break;
        hash.Add(buffer.data(), received);
    }
    return hash.Finish();
}

static const void* ResourceBytes(const PayloadAsset& asset, DWORD& length) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(asset.identifier), MAKEINTRESOURCEW(10));
    if (!resource) throw std::runtime_error("An embedded asset is missing.");
    length = SizeofResource(module, resource);
    const void* data = LockResource(LoadResource(module, resource));
    if (!data || !length) throw std::runtime_error("An embedded asset is unreadable.");
    return data;
}

static void VerifyPackage() {
    for (const auto& asset : payloadAssets) {
        DWORD length = 0;
        const void* data = ResourceBytes(asset, length);
        Sha256 hash;
        hash.Add(data, length);
        if (hash.Finish() != asset.sha256) throw std::runtime_error("Embedded payload checksum mismatch.");
    }
}

static void ExtractAsset(const std::wstring& directory, const PayloadAsset& asset) {
    std::wstring relative = asset.path;
    if (relative.empty() || relative.front() == L'\\' || relative.find(L':') != std::wstring::npos ||
        relative.find(L"..") != std::wstring::npos || relative.find(L'/') != std::wstring::npos)
        throw std::runtime_error("Invalid embedded asset path.");
    std::wstring destination = directory + L"\\" + relative;
    if (destination.size() + 40 >= MAX_PATH) throw std::runtime_error("The game directory path is too long.");
    for (size_t separator = relative.find(L'\\'); separator != std::wstring::npos;
        separator = relative.find(L'\\', separator + 1))
        EnsureDirectory(directory + L"\\" + relative.substr(0, separator));
    if (FileExists(destination)) {
        if (!asset.editable && HashFile(destination) != asset.sha256)
            throw std::runtime_error("A runtime file has changed. Move this package's runtime folder aside and retry; no files were overwritten.");
        return;
    }
    DWORD length = 0;
    const void* data = ResourceBytes(asset, length);
    std::wstring temporary = destination + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) FailWindows("Cannot create a runtime file.");
    try {
        DWORD written = 0;
        if (!WriteFile(file.value, data, length, &written, nullptr) || written != length || !FlushFileBuffers(file.value))
            FailWindows("Cannot write a runtime file.");
        file.Close();
        if (!MoveFileW(temporary.c_str(), destination.c_str())) FailWindows("Cannot publish a runtime file.");
    } catch (...) {
        file.Close();
        DeleteFileW(temporary.c_str());
        throw;
    }
    if (HashFile(destination) != asset.sha256) throw std::runtime_error("Extracted payload checksum mismatch.");
}

static std::wstring QuoteArgument(const std::wstring& argument) {
    std::wstring quoted = L"\"";
    unsigned slashes = 0;
    for (wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        quoted.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        quoted.push_back(character);
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

static int Run(const std::wstring& executable, const std::wstring& arguments, const std::wstring& directory) {
    std::wstring command = QuoteArgument(executable) + L" " + arguments;
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
        directory.c_str(), &startup, &process)) FailWindows("Cannot start the packaged launcher.");
    Handle processHandle(process.hProcess), threadHandle(process.hThread);
    if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0) FailWindows("Cannot wait for the launcher.");
    DWORD code = 2;
    if (!GetExitCodeProcess(process.hProcess, &code)) FailWindows("Cannot read the launch result.");
    return static_cast<int>(code);
}

int wmain(int argumentCount, wchar_t** arguments) {
    try {
        wchar_t modulePath[MAX_PATH] = {};
        DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (!moduleLength || moduleLength >= MAX_PATH) throw std::runtime_error("The launcher path is too long.");
        std::wstring gameDirectory(modulePath, moduleLength);
        gameDirectory.resize(gameDirectory.find_last_of(L'\\'));
        std::wstring imagePath;
        std::wstring mode;
        for (int index = 1; index < argumentCount; ++index) {
            std::wstring option = arguments[index];
            if (option == L"--help") {
                std::cout << "Rockman Dash 2 Enhanced " << packageVersion << "\n"
                    "Place this EXE beside your original dash2.exe and double-click it.\n"
                    "Options: --game-directory PATH, --iso PATH, --self-test, --extract-only, --diagnose, --check-only\n"
                    "ISO conversion is separate: Convert-DiscImage.ps1 -SourcePath IMAGE\n";
                return 0;
            }
            if (option == L"--game-directory" || option == L"--iso") {
                if (++index >= argumentCount) throw std::runtime_error("An option is missing its path.");
                if (option == L"--game-directory") gameDirectory = FullPath(arguments[index]);
                else imagePath = FullPath(arguments[index]);
            } else if (option == L"--self-test" || option == L"--extract-only" ||
                option == L"--diagnose" || option == L"--check-only") {
                if (!mode.empty()) throw std::runtime_error("Choose only one diagnostic mode.");
                mode = option;
            } else throw std::runtime_error("Unknown option. Use --help.");
        }
        VerifyPackage();
        if (mode == L"--self-test") {
            std::cout << "Embedded payload SHA-256: PASS (" << sizeof(payloadAssets) / sizeof(payloadAssets[0]) << " assets).\n";
            return 0;
        }
        gameDirectory = FullPath(gameDirectory);
        DWORD attributes = GetFileAttributesW(gameDirectory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            throw std::runtime_error("The game directory does not exist.");
        if (mode.empty() || mode == L"--check-only") {
            std::wstring original = gameDirectory + L"\\dash2.exe";
            if (!FileExists(original)) throw std::runtime_error("Place this EXE beside the original dash2.exe. The game is not included.");
            if (HashFile(original) != supportedGameSha256)
                throw std::runtime_error("Unsupported dash2.exe version. The game was not started or modified.");
        }
        std::wstring runtimeDirectory = gameDirectory + L"\\" + runtimeFolder;
        EnsureDirectory(runtimeDirectory);
        std::wstring lockPath = runtimeDirectory + L"\\session.lock";
        FileExists(lockPath);
        Handle lock(CreateFileW(lockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (lock.value == INVALID_HANDLE_VALUE) throw std::runtime_error("This runtime is already in use, or its directory is not writable.");
        for (const auto& asset : payloadAssets) ExtractAsset(runtimeDirectory, asset);
        PrintRuntimeDirectory(runtimeDirectory);
        if (mode == L"--extract-only") return 0;
        wchar_t windowsDirectory[MAX_PATH] = {};
        if (!GetWindowsDirectoryW(windowsDirectory, MAX_PATH)) FailWindows("Cannot locate Windows PowerShell.");
        std::wstring powershell = std::wstring(windowsDirectory) + L"\\Sysnative\\WindowsPowerShell\\v1.0\\powershell.exe";
        if (!FileExists(powershell)) powershell = std::wstring(windowsDirectory) + L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
        std::wstring options = L"-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
            QuoteArgument(runtimeDirectory + L"\\Start-Game.ps1");
        if (mode == L"--diagnose") options += L" -Diagnose";
        if (mode == L"--check-only") options += L" -CheckOnly";
        if (!imagePath.empty()) options += L" -ImagePath " + QuoteArgument(imagePath);
        int result = Run(powershell, options, gameDirectory);
        if (result != 0) throw std::runtime_error("Launch or preflight failed. See the console and runtime logs. Windows 10/11 and the x86 Visual C++ runtime are required.");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        DWORD processes[2] = {};
        if (argumentCount == 1 && GetConsoleProcessList(processes, 2) == 1)
            MessageBoxA(nullptr, error.what(), "Rockman Dash 2 Enhanced", MB_OK | MB_ICONERROR);
        return 2;
    }
}