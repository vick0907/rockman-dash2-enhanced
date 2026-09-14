#include <windows.h>
#include <winternl.h>
#include <winioctl.h>
#include <ntddcdrm.h>
#include <ntddscsi.h>
#include <MinHook.h>
#include <array>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "logging.h"
#include "secdrv_ioctl.h"

static HMODULE localModule;
static std::wstring logPath;
static decltype(&CreateFileA) originalCreateFile;
static decltype(&CreateFileW) originalCreateFileW;
static decltype(&CloseHandle) originalCloseHandle;
static decltype(&MessageBoxA) originalMessageBox;
static decltype(&GetVolumeInformationA) originalVolumeA;
static decltype(&GetVolumeInformationW) originalVolumeW;
static LONG loggedDriverHandle = 0;
static LONG loggedDiscRequests = 0;
static LONG loggedDiscReads = 0;
static LONG loggedScsiRequests = 0;
static bool verboseLogging = false;
static thread_local bool inDiscDiagnostics = false;
static std::wstring referenceLabel;
static std::wstring referenceImagePath;
static std::vector<unsigned char> referenceCab;
static DWORD referenceSectorCount = 0;
static bool emulateImageRequests = false;
static std::mutex discHandleMutex;
static std::unordered_map<HANDLE, std::wstring> discHandles;
using IoctlFunction = NTSTATUS(NTAPI*)(HANDLE, HANDLE, void*, void*, PIO_STATUS_BLOCK,
    ULONG, void*, ULONG, void*, ULONG);
static IoctlFunction originalIoctl;
using ReadFunction = NTSTATUS(NTAPI*)(HANDLE, HANDLE, void*, void*, PIO_STATUS_BLOCK,
    void*, ULONG, PLARGE_INTEGER, PULONG);
static ReadFunction originalRead;

void WriteLocalLog(const char* level, const std::string& message) {
    if (logPath.empty()) return;
    if (!verboseLogging && !strcmp(level, "trace")) return;
    HANDLE output = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return;
    std::string line = std::to_string(GetCurrentProcessId()) + " [" + level + "] " + message + "\r\n";
    DWORD written = 0;
    WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(output);
}

static void InitializeLog() {
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(localModule, modulePath, MAX_PATH);
    logPath = modulePath;
    logPath.resize(logPath.find_last_of(L'\\') + 1);
    logPath += L"LocalCompat.log";
    char traceSetting[8] = {};
    verboseLogging = GetEnvironmentVariableA("ROCKMAN_COMPAT_TRACE", traceSetting, sizeof(traceSetting)) == 1 &&
        traceSetting[0] == '1';
}

static std::wstring DecodeBig5(const char* text) {
    if (!text) return {};
    int required = MultiByteToWideChar(950, 0, text, -1, nullptr, 0);
    if (!required) return {};
    std::wstring converted(required, L'\0');
    MultiByteToWideChar(950, 0, text, -1, converted.data(), required);
    converted.pop_back();
    return converted;
}

static std::string EncodeUtf8(const std::wstring& text) {
    int required = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    std::string converted(required, '\0');
    if (required) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
        converted.data(), required, nullptr, nullptr);
    return converted;
}

static bool IsWine() {
    return GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
}

static DWORD ReadLittleEndian32(const unsigned char* bytes) {
    DWORD value = 0;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static bool ReadImageRange(HANDLE image, unsigned long long offset, unsigned char* output, DWORD size) {
    LARGE_INTEGER position = {};
    position.QuadPart = static_cast<LONGLONG>(offset);
    DWORD returned = 0;
    return SetFilePointerEx(image, position, nullptr, FILE_BEGIN) &&
        ReadFile(image, output, size, &returned, nullptr) && returned == size;
}

static bool LoadDiscReference() {
    referenceLabel.clear();
    referenceImagePath.clear();
    referenceCab.clear();
    referenceSectorCount = 0;
    std::wstring compatibilityDirectory = logPath.substr(0, logPath.find_last_of(L'\\'));
    referenceImagePath = compatibilityDirectory.substr(0, compatibilityDirectory.find_last_of(L'\\')) +
        L"\\gamez88_d2.windows.iso";
    HANDLE image = CreateFileW(referenceImagePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (image == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER imageSize = {};
    if (!GetFileSizeEx(image, &imageSize) || imageSize.QuadPart < 17 * 2048 ||
        imageSize.QuadPart % 2048 || imageSize.QuadPart / 2048 > 255 * 60 * 75 - 150) {
        CloseHandle(image);
        return false;
    }
    referenceSectorCount = static_cast<DWORD>(imageSize.QuadPart / 2048);
    std::array<unsigned char, 2048> descriptor = {};
    if (!ReadImageRange(image, 16 * 2048, descriptor.data(), descriptor.size()) ||
        descriptor[0] != 1 || memcmp(descriptor.data() + 1, "CD001", 5) || descriptor[6] != 1 ||
        descriptor[128] != 0 || descriptor[129] != 8 || descriptor[156] < 34) {
        CloseHandle(image);
        return false;
    }
    DWORD directorySize = ReadLittleEndian32(descriptor.data() + 166);
    unsigned long long directorySector = ReadLittleEndian32(descriptor.data() + 158);
    directorySector += descriptor[157];
    if (!directorySize || directorySize > 1024 * 1024) { CloseHandle(image); return false; }
    std::vector<unsigned char> directory(directorySize);
    if (!ReadImageRange(image, directorySector * 2048, directory.data(), directorySize)) {
        CloseHandle(image);
        return false;
    }
    for (size_t offset = 0; offset < directory.size();) {
        size_t length = directory[offset];
        if (!length) { offset = (offset / 2048 + 1) * 2048; continue; }
        if (length < 34 || offset + length > directory.size()) break;
        const unsigned char* record = directory.data() + offset;
        if (33u + record[32] > length) break;
        std::string name(reinterpret_cast<const char*>(record + 33), record[32]);
        if (name == "DATA1.CAB;1" && !(record[25] & 0x82)) {
            DWORD fileSize = ReadLittleEndian32(record + 10);
            unsigned long long fileSector = ReadLittleEndian32(record + 2);
            fileSector += record[1];
            if (!fileSize || fileSize > 1024 * 1024) break;
            referenceCab.resize(fileSize);
            if (!ReadImageRange(image, fileSector * 2048, referenceCab.data(), fileSize)) {
                referenceCab.clear();
                break;
            }
            std::string label(reinterpret_cast<const char*>(descriptor.data() + 40), 32);
            while (!label.empty() && (label.back() == ' ' || label.back() == '\0')) label.pop_back();
            referenceLabel.assign(label.begin(), label.end());
            break;
        }
        offset += length;
    }
    CloseHandle(image);
    if (referenceLabel.empty() || referenceCab.empty()) return false;
    WriteLocalLog("disc", "ISO metadata: label=" + EncodeUtf8(referenceLabel) +
        " referenceCabBytes=" + std::to_string(referenceCab.size()) +
        " sectors=" + std::to_string(referenceSectorCount));
    return true;
}

static std::wstring VerifiedDiscLabel(LPCWSTR root) {
    if (!root || referenceLabel.empty() || referenceCab.empty() || GetDriveTypeW(root) != DRIVE_CDROM) return {};
    std::wstring path = root;
    if (path.size() != 3 || path[1] != L':' || path[2] != L'\\') return {};
    path += L"DATA1.CAB";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size = {};
    bool matches = GetFileSizeEx(file, &size) && size.QuadPart == static_cast<LONGLONG>(referenceCab.size());
    if (matches) {
        std::vector<unsigned char> contents(referenceCab.size());
        DWORD returned = 0;
        matches = ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &returned, nullptr) &&
            returned == contents.size() && contents == referenceCab;
    }
    CloseHandle(file);
    return matches ? referenceLabel : std::wstring();
}

static constexpr DWORD tocReplySize = 4 + 2 * sizeof(TRACK_DATA);
static constexpr ULONG cdromMediaRemoval = CTL_CODE(FILE_DEVICE_CD_ROM, 0x0201, METHOD_BUFFERED, FILE_READ_ACCESS);

static CDROM_TOC ReferenceToc() {
    CDROM_TOC table = {};
    table.Length[1] = static_cast<UCHAR>(tocReplySize - 2);
    table.FirstTrack = 1;
    table.LastTrack = 1;
    for (unsigned index = 0; index < 2; ++index) {
        TRACK_DATA& track = table.TrackData[index];
        track.Adr = 1;
        track.Control = 4;
        track.TrackNumber = index == 0 ? 1 : 0xaa;
        DWORD frames = index == 0 ? 150 : referenceSectorCount + 149;
        track.Address[1] = static_cast<UCHAR>(frames / (60 * 75));
        track.Address[2] = static_cast<UCHAR>((frames / 75) % 60);
        track.Address[3] = static_cast<UCHAR>(frames % 75);
    }
    return table;
}

static DISK_GEOMETRY ReferenceGeometry() {
    DISK_GEOMETRY geometry = {};
    geometry.Cylinders.QuadPart = referenceSectorCount / (64 * 32);
    geometry.MediaType = RemovableMedia;
    geometry.TracksPerCylinder = 64;
    geometry.SectorsPerTrack = 32;
    geometry.BytesPerSector = 2048;
    return geometry;
}

using SeekFunction = decltype(&SetFilePointerEx);

static HANDLE EnsureSeekableDisc(HANDLE handle, DWORD flags, LPSECURITY_ATTRIBUTES security,
    SeekFunction seekFile = &SetFilePointerEx) {
    LARGE_INTEGER zero = {};
    LARGE_INTEGER descriptorOffset = {};
    descriptorOffset.QuadPart = 16 * 2048;
    if (seekFile(handle, descriptorOffset, nullptr, FILE_BEGIN) &&
        seekFile(handle, zero, nullptr, FILE_BEGIN)) return handle;
    DWORD seekError = GetLastError();
    if (referenceImagePath.empty()) return handle;
    auto openImage = originalCreateFileW ? originalCreateFileW : &CreateFileW;
    auto closeHandle = originalCloseHandle ? originalCloseHandle : &CloseHandle;
    HANDLE image = openImage(referenceImagePath.c_str(), GENERIC_READ, FILE_SHARE_READ, security,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | (flags & FILE_FLAG_OVERLAPPED), nullptr);
    if (image == INVALID_HANDLE_VALUE) {
        WriteLocalLog("image", "Unable to open read-only sector backing file; error=" + std::to_string(GetLastError()));
        return handle;
    }
    if (!closeHandle(handle)) {
        closeHandle(image);
        return handle;
    }
    WriteLocalLog("image", "Bound unseekable CD handle to read-only ISO; original seekError=" + std::to_string(seekError));
    return image;
}

static BOOL WINAPI RejectTestSeek(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD) {
    SetLastError(ERROR_BAD_DEV_TYPE);
    return FALSE;
}

static bool TestSectorBackend() {
    HANDLE reader = CreateFileW(referenceImagePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reader == INVALID_HANDLE_VALUE) return false;
    HANDLE unchanged = EnsureSeekableDisc(reader, 0, nullptr);
    bool preserved = unchanged == reader;
    CloseHandle(unchanged);
    reader = CreateFileW(referenceImagePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (reader == INVALID_HANDLE_VALUE) return false;
    HANDLE backing = EnsureSeekableDisc(reader, 0, nullptr, &RejectTestSeek);
    bool redirected = backing != reader;
    std::array<unsigned char, 2048> descriptor = {};
    bool valid = preserved && redirected && ReadImageRange(backing, 16 * 2048, descriptor.data(), descriptor.size()) &&
        descriptor[0] == 1 && !memcmp(descriptor.data() + 1, "CD001", 5);
    CloseHandle(backing);
    WriteLocalLog("test", std::string("Unseekable handle sector backend: ") + (valid ? "PASS" : "FAIL"));
    return valid;
}

static HANDLE TrackDiscHandle(HANDLE handle, LPCWSTR path, DWORD flags, LPSECURITY_ATTRIBUTES security) {
    if (!emulateImageRequests || handle == INVALID_HANDLE_VALUE || !path) return handle;
    std::wstring device = path;
    if (device.size() == 7 && device.back() == L'\\') device.pop_back();
    if (device.size() != 6 || device[0] != L'\\' || device[1] != L'\\' ||
        (device[2] != L'.' && device[2] != L'?') || device[3] != L'\\' || device[5] != L':') return handle;
    std::wstring root = device.substr(4, 2) + L"\\";
    if (VerifiedDiscLabel(root.c_str()).empty()) return handle;
    handle = EnsureSeekableDisc(handle, flags, security);
    {
        std::lock_guard<std::mutex> guard(discHandleMutex);
        discHandles[handle] = root;
    }
    WriteLocalLog("disc", "Tracking verified CD handle for " + EncodeUtf8(root));
    return handle;
}

static HANDLE WINAPI OpenGameFileW(LPCWSTR path, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile) {
    HANDLE handle = originalCreateFileW(path, access, sharing, security, disposition, flags, templateFile);
    DWORD error = GetLastError();
    handle = TrackDiscHandle(handle, path, flags, security);
    SetLastError(error);
    return handle;
}

static BOOL WINAPI CloseGameHandle(HANDLE handle) {
    BOOL closed = originalCloseHandle(handle);
    DWORD error = GetLastError();
    if (closed) {
        std::lock_guard<std::mutex> guard(discHandleMutex);
        discHandles.erase(handle);
    }
    SetLastError(error);
    return closed;
}

static const UCHAR capabilityCommand[10] = {0x5a, 0x08, 0x2a, 0, 0, 0, 0, 0, 50, 0};

static bool CapabilityRejection(void* input, ULONG inputSize, void* output, ULONG outputSize,
    PIO_STATUS_BLOCK status) {
    if (!input || !output || !status || inputSize < sizeof(SCSI_PASS_THROUGH) ||
        outputSize < sizeof(SCSI_PASS_THROUGH)) return false;
    SCSI_PASS_THROUGH response = {};
    memcpy(&response, input, sizeof(response));
    constexpr UCHAR sense[18] = {0x70, 0, 5, 0, 0, 0, 0, 10, 0, 0, 0, 0, 0x24, 0, 0, 0, 0, 0};
    if (response.Length != sizeof(response) || response.CdbLength != sizeof(capabilityCommand) ||
        memcmp(response.Cdb, capabilityCommand, sizeof(capabilityCommand)) || response.DataIn != SCSI_IOCTL_DATA_IN ||
        response.DataTransferLength != 50 || response.SenseInfoLength < sizeof(sense) ||
        response.DataBufferOffset < sizeof(response) || response.DataBufferOffset > outputSize ||
        response.DataTransferLength > outputSize - response.DataBufferOffset ||
        response.SenseInfoOffset < sizeof(response) || response.SenseInfoOffset > response.DataBufferOffset ||
        sizeof(sense) > response.DataBufferOffset - response.SenseInfoOffset) return false;
    response.ScsiStatus = 2;
    response.SenseInfoLength = sizeof(sense);
    response.DataTransferLength = 8;
    memcpy(output, &response, sizeof(response));
    auto* bytes = static_cast<unsigned char*>(output);
    memcpy(bytes + response.SenseInfoOffset, sense, sizeof(sense));
    memset(bytes + response.DataBufferOffset, 0, response.DataTransferLength);
    status->Status = 0;
    status->Information = response.DataBufferOffset + response.DataTransferLength;
    return true;
}

static std::array<unsigned char, 130> CapabilityRequest() {
    std::array<unsigned char, 130> buffer = {};
    SCSI_PASS_THROUGH request = {};
    request.Length = sizeof(request);
    request.CdbLength = sizeof(capabilityCommand);
    request.DataIn = SCSI_IOCTL_DATA_IN;
    request.DataTransferLength = 50;
    request.TimeOutValue = 2;
    request.DataBufferOffset = 80;
    request.SenseInfoOffset = 48;
    request.SenseInfoLength = 32;
    memcpy(request.Cdb, capabilityCommand, sizeof(capabilityCommand));
    memcpy(buffer.data(), &request, sizeof(request));
    return buffer;
}

static bool TestCapabilityRejection() {
    auto request = CapabilityRequest();
    auto output = request;
    IO_STATUS_BLOCK status = {};
    bool handled = CapabilityRejection(request.data(), request.size(), output.data(), output.size(), &status);
    SCSI_PASS_THROUGH response = {};
    memcpy(&response, output.data(), sizeof(response));
    bool valid = handled && status.Status == 0 && status.Information == 88 && response.ScsiStatus == 2 &&
        response.DataTransferLength == 8 && response.SenseInfoLength == 18 && output[48] == 0x70 &&
        output[50] == 5 && output[60] == 0x24;
    auto unchanged = request;
    valid = valid && !CapabilityRejection(request.data(), request.size(), unchanged.data(), 87, &status) && unchanged == request;
    request[offsetof(SCSI_PASS_THROUGH, Cdb)] = 0x12;
    valid = valid && !CapabilityRejection(request.data(), request.size(), unchanged.data(), unchanged.size(), &status);
    WriteLocalLog("test", std::string("Bounded SCSI capability rejection: ") + (valid ? "PASS" : "FAIL"));
    return valid;
}

static NTSTATUS ImageDiscReply(HANDLE file, HANDLE event, void* routine, PIO_STATUS_BLOCK status,
    ULONG code, void* input, ULONG inputSize, void* output, ULONG outputSize, NTSTATUS originalStatus) {
    if (!emulateImageRequests || !status || routine ||
        (originalStatus != static_cast<NTSTATUS>(0xc0000010) && originalStatus != static_cast<NTSTATUS>(0xc00000bb) &&
            !(code == IOCTL_SCSI_PASS_THROUGH && originalStatus == static_cast<NTSTATUS>(0xc0000023))) ||
        (code != cdromMediaRemoval && code != IOCTL_CDROM_READ_TOC && code != IOCTL_CDROM_GET_DRIVE_GEOMETRY &&
            code != IOCTL_SCSI_PASS_THROUGH))
        return originalStatus;
    std::wstring root;
    {
        std::lock_guard<std::mutex> guard(discHandleMutex);
        auto found = discHandles.find(file);
        if (found != discHandles.end()) root = found->second;
    }
    if (root.empty() || VerifiedDiscLabel(root.c_str()).empty()) return originalStatus;
    if (code == IOCTL_SCSI_PASS_THROUGH) {
        if (!CapabilityRejection(input, inputSize, output, outputSize, status)) return originalStatus;
        if (event) SetEvent(event);
        WriteLocalLog("image", "MODE SENSE capabilities: transport completed, device CHECK CONDITION / invalid CDB field.");
        return 0;
    }
    NTSTATUS result = 0;
    ULONG returned = 0;
    if (code == cdromMediaRemoval) {
        if (!input || inputSize < sizeof(PREVENT_MEDIA_REMOVAL)) result = static_cast<NTSTATUS>(0xc000000d);
    } else if (code == IOCTL_CDROM_READ_TOC) {
        if (!output || outputSize < tocReplySize) result = static_cast<NTSTATUS>(0xc0000023);
        else {
            CDROM_TOC table = ReferenceToc();
            memcpy(output, &table, tocReplySize);
            returned = tocReplySize;
        }
    } else {
        if (!output || outputSize < sizeof(DISK_GEOMETRY)) result = static_cast<NTSTATUS>(0xc0000023);
        else {
            DISK_GEOMETRY geometry = ReferenceGeometry();
            memcpy(output, &geometry, sizeof(geometry));
            returned = sizeof(geometry);
        }
    }
    status->Status = result;
    status->Information = returned;
    if (event) SetEvent(event);
    std::ostringstream details;
    details << "Image-backed CD response code=0x" << std::hex << code << " status=0x" << static_cast<ULONG>(result);
    WriteLocalLog("image", details.str());
    return result;
}

static BOOL WINAPI ReadVolumeW(LPCWSTR root, LPWSTR label, DWORD capacity, LPDWORD serial,
    LPDWORD maximumName, LPDWORD flags, LPWSTR filesystem, DWORD filesystemCapacity) {
    BOOL result = originalVolumeW(root, label, capacity, serial, maximumName, flags, filesystem, filesystemCapacity);
    if (!result || !label || !capacity || label[0]) return result;
    DWORD previousError = GetLastError();
    std::wstring verified = VerifiedDiscLabel(root);
    if (!verified.empty()) {
        if (capacity <= verified.size()) { SetLastError(ERROR_MORE_DATA); return FALSE; }
        memcpy(label, verified.c_str(), (verified.size() + 1) * sizeof(wchar_t));
        WriteLocalLog("volume", std::string(inDiscDiagnostics ? "diagnostic" : "game") +
            " restored verified ISO label=" + EncodeUtf8(verified));
    }
    SetLastError(previousError);
    return result;
}

static BOOL WINAPI ReadVolumeA(LPCSTR root, LPSTR label, DWORD capacity, LPDWORD serial,
    LPDWORD maximumName, LPDWORD flags, LPSTR filesystem, DWORD filesystemCapacity) {
    BOOL result = originalVolumeA(root, label, capacity, serial, maximumName, flags, filesystem, filesystemCapacity);
    if (!result || !label || !capacity || label[0] || !root) return result;
    DWORD previousError = GetLastError();
    std::wstring verified = VerifiedDiscLabel(DecodeBig5(root).c_str());
    if (!verified.empty()) {
        std::string encoded(verified.begin(), verified.end());
        if (capacity <= encoded.size()) { SetLastError(ERROR_MORE_DATA); return FALSE; }
        memcpy(label, encoded.c_str(), encoded.size() + 1);
        WriteLocalLog("volume", std::string(inDiscDiagnostics ? "diagnostic" : "game") +
            " restored verified ISO label=" + encoded);
    }
    SetLastError(previousError);
    return result;
}

static void LogDiscState() {
    bool previousDiagnosticState = inDiscDiagnostics;
    inDiscDiagnostics = true;
    WriteLocalLog("disc", "Seekable image backend build 2026-09-13; ANSI code page=" + std::to_string(GetACP()));
    DWORD drives = GetLogicalDrives();
    for (unsigned index = 0; index < 26; ++index) {
        if (!(drives & (1u << index)) && index != ('X' - 'A')) continue;
        std::wstring root = L"A:\\";
        root[0] += static_cast<wchar_t>(index);
        UINT type = GetDriveTypeW(root.c_str());
        WriteLocalLog("disc", EncodeUtf8(root) + " type=" + std::to_string(type));
        if (type != DRIVE_CDROM && root[0] != L'X') continue;
        wchar_t label[MAX_PATH] = {};
        wchar_t filesystem[MAX_PATH] = {};
        DWORD serial = 0, maximumName = 0, flags = 0;
        BOOL volumeRead = GetVolumeInformationW(root.c_str(), label, MAX_PATH, &serial,
            &maximumName, &flags, filesystem, MAX_PATH);
        DWORD volumeError = volumeRead ? 0 : GetLastError();
        WriteLocalLog("disc", EncodeUtf8(root) + " volumeError=" + std::to_string(volumeError) +
            " label=" + EncodeUtf8(label) + " filesystem=" + EncodeUtf8(filesystem));
        std::wstring verified = VerifiedDiscLabel(root.c_str());
        WriteLocalLog("disc", EncodeUtf8(root) + " content-verified ISO label=" + EncodeUtf8(verified));
        for (const wchar_t* name : {L"AUTORUN.INF", L"DATA1.CAB", L"DAT"}) {
            std::wstring path = root + name;
            DWORD attributes = GetFileAttributesW(path.c_str());
            DWORD fileError = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
            WriteLocalLog("disc", EncodeUtf8(path) + " error=" + std::to_string(fileError));
        }
        std::wstring devicePath = L"\\\\.\\" + root.substr(0, 2);
        HANDLE volume = CreateFileW(devicePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        DWORD deviceError = volume == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        WriteLocalLog("disc", EncodeUtf8(devicePath) + " openError=" + std::to_string(deviceError));
        if (volume == INVALID_HANDLE_VALUE) continue;
        CDROM_TOC table = {};
        DWORD returned = 0;
        BOOL tocRead = DeviceIoControl(volume, IOCTL_CDROM_READ_TOC, nullptr, 0,
            &table, sizeof(table), &returned, nullptr);
        DWORD tocError = tocRead ? 0 : GetLastError();
        WriteLocalLog("disc", "TOC error=" + std::to_string(tocError) +
            " firstTrack=" + std::to_string(table.FirstTrack) + " lastTrack=" + std::to_string(table.LastTrack));
        if (tocRead && !verified.empty()) {
            CDROM_TOC expected = ReferenceToc();
            bool sameToc = returned == tocReplySize && !memcmp(&table, &expected, tocReplySize);
            WriteLocalLog("test", std::string("TOC matches Windows reference: ") + (sameToc ? "PASS" : "FAIL"));
        }
        DISK_GEOMETRY geometry = {};
        returned = 0;
        BOOL geometryRead = DeviceIoControl(volume, IOCTL_CDROM_GET_DRIVE_GEOMETRY, nullptr, 0,
            &geometry, sizeof(geometry), &returned, nullptr);
        DWORD geometryError = geometryRead ? 0 : GetLastError();
        WriteLocalLog("disc", "Geometry error=" + std::to_string(geometryError));
        if (geometryRead && !verified.empty()) {
            DISK_GEOMETRY expected = ReferenceGeometry();
            bool sameGeometry = returned == sizeof(geometry) && !memcmp(&geometry, &expected, sizeof(geometry));
            WriteLocalLog("test", std::string("Geometry matches Windows reference: ") + (sameGeometry ? "PASS" : "FAIL"));
        }
        if (!verified.empty()) {
            auto capability = CapabilityRequest();
            returned = 0;
            HANDLE capabilityDevice = CreateFileW(devicePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            BOOL completed = capabilityDevice != INVALID_HANDLE_VALUE &&
                DeviceIoControl(capabilityDevice, IOCTL_SCSI_PASS_THROUGH, capability.data(), capability.size(),
                    capability.data(), capability.size(), &returned, nullptr);
            DWORD capabilityError = completed ? 0 : GetLastError();
            if (capabilityDevice != INVALID_HANDLE_VALUE) CloseHandle(capabilityDevice);
            if (completed) {
                auto expected = CapabilityRequest();
                IO_STATUS_BLOCK expectedStatus = {};
                CapabilityRejection(expected.data(), expected.size(), expected.data(), expected.size(), &expectedStatus);
                expected[offsetof(SCSI_PASS_THROUGH, Lun)] = capability[offsetof(SCSI_PASS_THROUGH, Lun)];
                bool sameResponse = returned == expectedStatus.Information && capability == expected;
                WriteLocalLog("test", std::string("SCSI capability response matches Windows reference excluding assigned LUN: ") +
                    (sameResponse ? "PASS" : "FAIL"));
                if (!sameResponse) {
                    std::ostringstream differences;
                    differences << "SCSI native bytes=" << returned << " expected=" << expectedStatus.Information << " differences=";
                    for (size_t index = 0; index < capability.size(); ++index) {
                        if (capability[index] != expected[index])
                            differences << index << ":" << static_cast<unsigned>(capability[index]) << "/" <<
                                static_cast<unsigned>(expected[index]) << " ";
                    }
                    WriteLocalLog("test", differences.str());
                }
            } else WriteLocalLog("disc", "SCSI capability error=" + std::to_string(capabilityError));
        }
        LARGE_INTEGER descriptorOffset = {};
        descriptorOffset.QuadPart = 16 * 2048;
        BOOL seeked = SetFilePointerEx(volume, descriptorOffset, nullptr, FILE_BEGIN);
        DWORD seekError = seeked ? 0 : GetLastError();
        WriteLocalLog("disc", "Raw PVD seekError=" + std::to_string(seekError));
        auto* rawData = static_cast<unsigned char*>(VirtualAlloc(nullptr, 2048, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (seeked && rawData) {
            returned = 0;
            BOOL read = ReadFile(volume, rawData, 2048, &returned, nullptr);
            DWORD readError = read ? 0 : GetLastError();
            bool correctDescriptor = read && returned == 2048 && rawData[0] == 1 && !memcmp(rawData + 1, "CD001", 5);
            WriteLocalLog("disc", "Raw PVD readError=" + std::to_string(readError) +
                " returned=" + std::to_string(returned) + " ISO descriptor=" + (correctDescriptor ? "YES" : "NO"));
        }
        if (rawData) VirtualFree(rawData, 0, MEM_RELEASE);
        CloseHandle(volume);
    }
    inDiscDiagnostics = previousDiagnosticState;
}

static int WINAPI ShowGameMessage(HWND owner, LPCSTR text, LPCSTR caption, UINT style) {
    std::wstring decodedText = DecodeBig5(text);
    std::wstring decodedCaption = DecodeBig5(caption);
    WriteLocalLog("dialog", EncodeUtf8(decodedCaption) + ": " + EncodeUtf8(decodedText));
    LogDiscState();
    if (text && *text && decodedText.empty()) return originalMessageBox(owner, text, caption, style);
    if (IsWine() && decodedCaption == L"Cannot locate the CD-ROM" &&
        decodedText.find(L"\u904a\u6232\u5149\u789f") != std::wstring::npos) {
        decodedText = L"Please insert the correct game disc, then click OK to restart the program.";
    }
    return MessageBoxW(owner, decodedText.c_str(), decodedCaption.c_str(), style);
}

extern "C" __declspec(dllexport) int __cdecl Diagnose() {
    InitializeLog();
    bool correctText = DecodeBig5("\xBD\xD0") == L"\x8ACB";
    WriteLocalLog("test", std::string("Big5 conversion: ") + (correctText ? "PASS" : "FAIL"));
    bool validImage = LoadDiscReference();
    WriteLocalLog("test", std::string("ISO metadata parser: ") + (validImage ? "PASS" : "FAIL"));
    bool validBackend = validImage && TestSectorBackend();
    bool validCapability = TestCapabilityRejection();
    LogDiscState();
    return correctText && validImage && validBackend && validCapability ? 0 : 2;
}

static HANDLE WINAPI OpenGameFile(LPCSTR filename, DWORD access, DWORD sharing,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile) {
    if (filename && (!_stricmp(filename, "\\\\.\\Secdrv") || !_stricmp(filename, "\\\\.\\Global\\SecDrv"))) {
        if (InterlockedCompareExchange(&loggedDriverHandle, 1, 0) == 0)
            WriteLocalLog("info", "Providing user-mode SecDrv handle.");
        return originalCreateFile("NUL", GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    HANDLE handle = originalCreateFile(filename, access, sharing, security, disposition, flags, templateFile);
    DWORD error = GetLastError();
    if (filename && filename[0] == '\\' && filename[1] == '\\')
        handle = TrackDiscHandle(handle, DecodeBig5(filename).c_str(), flags, security);
    SetLastError(error);
    return handle;
}

static NTSTATUS NTAPI ReadGameFile(HANDLE file, HANDLE event, void* routine, void* context,
    PIO_STATUS_BLOCK status, void* buffer, ULONG length, PLARGE_INTEGER offset, PULONG key) {
    NTSTATUS result = originalRead(file, event, routine, context, status, buffer, length, offset, key);
    bool tracked = false;
    if (!inDiscDiagnostics) {
        std::lock_guard<std::mutex> guard(discHandleMutex);
        tracked = discHandles.find(file) != discHandles.end();
    }
    if (tracked && InterlockedIncrement(&loggedDiscReads) <= 48) {
        std::ostringstream details;
        details << "CD read offset=" << (offset ? offset->QuadPart : -2) << " length=" << length <<
            " status=0x" << std::hex << static_cast<ULONG>(result);
        if (status) details << " returned=" << std::dec << status->Information;
        if (result == 0 && buffer && status && status->Information >= 8 && length >= 8) {
            auto* bytes = static_cast<unsigned char*>(buffer);
            details << " prefix=" << std::hex;
            for (unsigned index = 0; index < 8; ++index) details << static_cast<unsigned>(bytes[index]) << ":";
        }
        WriteLocalLog("read", details.str());
    }
    return result;
}

static NTSTATUS NTAPI GameIoctl(HANDLE file, HANDLE event, void* routine, void* context,
    PIO_STATUS_BLOCK status, ULONG code, void* input, ULONG inputSize, void* output, ULONG outputSize) {
    if (code != secdrvIoctl::ioctlCodeMain) {
        bool traceScsi = !inDiscDiagnostics && (code == IOCTL_SCSI_PASS_THROUGH ||
            code == IOCTL_SCSI_PASS_THROUGH_DIRECT) && InterlockedIncrement(&loggedScsiRequests) <= 16;
        if (traceScsi && input && inputSize >= sizeof(SCSI_PASS_THROUGH)) {
            SCSI_PASS_THROUGH request = {};
            memcpy(&request, input, sizeof(request));
            std::ostringstream details;
            details << "request inputSize=" << inputSize << " outputSize=" << outputSize <<
                " length=" << request.Length << " direction=" << static_cast<unsigned>(request.DataIn) <<
                " transfer=" << request.DataTransferLength << " dataOffset=" << request.DataBufferOffset <<
                " senseOffset=" << request.SenseInfoOffset << " cdb=" << std::hex;
            for (unsigned index = 0; index < request.CdbLength && index < sizeof(request.Cdb); ++index)
                details << static_cast<unsigned>(request.Cdb[index]) << ":";
            WriteLocalLog("scsi", details.str());
        }
        NTSTATUS result = originalIoctl(file, event, routine, context, status, code,
            input, inputSize, output, outputSize);
        if (traceScsi) {
            std::ostringstream details;
            details << "response status=0x" << std::hex << static_cast<ULONG>(result);
            if (status) details << " information=" << std::dec << status->Information;
            if (result == 0 && output && outputSize >= sizeof(SCSI_PASS_THROUGH)) {
                SCSI_PASS_THROUGH response = {};
                memcpy(&response, output, sizeof(response));
                details << " scsiStatus=" << static_cast<unsigned>(response.ScsiStatus) <<
                    " transfer=" << response.DataTransferLength << " senseLength=" <<
                    static_cast<unsigned>(response.SenseInfoLength);
                if (response.SenseInfoOffset < outputSize && response.SenseInfoLength <= outputSize - response.SenseInfoOffset) {
                    auto* sense = static_cast<unsigned char*>(output) + response.SenseInfoOffset;
                    details << " sense=" << std::hex;
                    for (unsigned index = 0; index < response.SenseInfoLength && index < 32; ++index)
                        details << static_cast<unsigned>(sense[index]) << ":";
                }
                if (code == IOCTL_SCSI_PASS_THROUGH && response.DataBufferOffset < outputSize &&
                    response.DataTransferLength <= outputSize - response.DataBufferOffset) {
                    auto* bytes = static_cast<unsigned char*>(output) + response.DataBufferOffset;
                    details << " data=" << std::hex;
                    for (unsigned index = 0; index < response.DataTransferLength && index < 64; ++index)
                        details << static_cast<unsigned>(bytes[index]) << ":";
                }
            }
            WriteLocalLog("scsi", details.str());
        }
        result = ImageDiscReply(file, event, routine, status, code, input, inputSize, output, outputSize, result);
        if (!inDiscDiagnostics && ((code >> 16) == FILE_DEVICE_CD_ROM ||
            code == IOCTL_SCSI_PASS_THROUGH || code == IOCTL_SCSI_PASS_THROUGH_DIRECT ||
            code == IOCTL_STORAGE_QUERY_PROPERTY) && InterlockedIncrement(&loggedDiscRequests) <= 32) {
            std::ostringstream details;
            details << "CD request code=0x" << std::hex << code << " status=0x" << static_cast<ULONG>(result);
            WriteLocalLog("ioctl", details.str());
        }
        return result;
    }
    if (!status) return static_cast<NTSTATUS>(0xc000000d);
    bool succeeded = secdrvIoctl::ProcessMainIoctl(input, inputSize, output, outputSize);
    status->Information = succeeded ? outputSize : 0;
    status->Status = succeeded ? 0 : static_cast<NTSTATUS>(0xc0000001);
    return status->Status;
}

extern "C" __declspec(dllexport) int __cdecl Setup() {
    wchar_t executable[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const wchar_t* filename = wcsrchr(executable, L'\\');
    if (!filename || _wcsicmp(filename + 1, L"dash2.exe")) return 0;

    InitializeLog();
    WriteLocalLog("info", "Initializing game-local compatibility; no driver or system installation.");
    for (const char* name : {"BOX64_DYNAREC", "BOX64_IGNOREINT3", "BOX64_DYNAREC_INTERP_SIGNAL"}) {
        char value[32] = {};
        DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
        WriteLocalLog("environment", std::string(name) + "=" + (length && length < sizeof(value) ? value : "(not set)"));
    }
    bool metadataAvailable = IsWine() && LoadDiscReference();

    MH_STATUS result = MH_Initialize();
    if (result != MH_OK) {
        spdlog::error("MinHook initialization failed", static_cast<int>(result));
        return 0;
    }
    result = MH_CreateHookApi(L"kernel32", "CreateFileA", reinterpret_cast<void*>(&OpenGameFile),
        reinterpret_cast<void**>(&originalCreateFile));
    if (result == MH_OK) {
        result = MH_CreateHookApi(L"ntdll", "NtDeviceIoControlFile", reinterpret_cast<void*>(&GameIoctl),
            reinterpret_cast<void**>(&originalIoctl));
    }
    if (result == MH_OK) {
        MH_STATUS dialogResult = MH_CreateHookApi(L"user32", "MessageBoxA", reinterpret_cast<void*>(&ShowGameMessage),
            reinterpret_cast<void**>(&originalMessageBox));
        WriteLocalLog("info", "Big5 dialog hook status=" + std::to_string(static_cast<int>(dialogResult)));
    }
    if (result == MH_OK && metadataAvailable) {
        MH_STATUS openStatus = MH_CreateHookApi(L"kernel32", "CreateFileW", reinterpret_cast<void*>(&OpenGameFileW),
            reinterpret_cast<void**>(&originalCreateFileW));
        MH_STATUS closeStatus = MH_CreateHookApi(L"kernel32", "CloseHandle", reinterpret_cast<void*>(&CloseGameHandle),
            reinterpret_cast<void**>(&originalCloseHandle));
        emulateImageRequests = openStatus == MH_OK && closeStatus == MH_OK;
        WriteLocalLog("info", "Image-backed CD hooks open=" + std::to_string(static_cast<int>(openStatus)) +
            " close=" + std::to_string(static_cast<int>(closeStatus)));
        MH_STATUS readStatus = MH_CreateHookApi(L"ntdll", "NtReadFile", reinterpret_cast<void*>(&ReadGameFile),
            reinterpret_cast<void**>(&originalRead));
        WriteLocalLog("info", "CD read tracing hook status=" + std::to_string(static_cast<int>(readStatus)));
        MH_STATUS volumeStatusW = MH_CreateHookApi(L"kernel32", "GetVolumeInformationW", reinterpret_cast<void*>(&ReadVolumeW),
            reinterpret_cast<void**>(&originalVolumeW));
        MH_STATUS volumeStatusA = MH_CreateHookApi(L"kernel32", "GetVolumeInformationA", reinterpret_cast<void*>(&ReadVolumeA),
            reinterpret_cast<void**>(&originalVolumeA));
        WriteLocalLog("info", "ISO volume hook status W=" + std::to_string(static_cast<int>(volumeStatusW)) +
            " A=" + std::to_string(static_cast<int>(volumeStatusA)));
    }
    if (result == MH_OK) result = MH_EnableHook(MH_ALL_HOOKS);
    if (result != MH_OK) {
        spdlog::error("Hook setup failed", static_cast<int>(result));
        MH_Uninitialize();
        return 0;
    }
    WriteLocalLog("info", "Game-local compatibility is active.");
    LogDiscState();
    return 100;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        localModule = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}