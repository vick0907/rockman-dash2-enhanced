#include <windows.h>
#include <initguid.h>
#include <ddraw.h>
#include <dsound.h>
#include <MinHook.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using QueryFunction = HRESULT(WINAPI*)(void*, REFIID, void**);
using CreateSurfaceFunction = HRESULT(WINAPI*)(void*, DDSURFACEDESC2*, void**, IUnknown*);
using FlipFunction = HRESULT(WINAPI*)(void*, void*, DWORD);
using BltFunction = HRESULT(WINAPI*)(void*, RECT*, void*, RECT*, DWORD, DDBLTFX*);
using CreateDrawFunction = HRESULT(WINAPI*)(GUID*, void**, IUnknown*);
using CreateDrawExFunction = HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*);

struct MethodHook {
    void* target;
    void* original;
};

struct SubtitleCue {
    uint32_t start;
    uint32_t end;
    std::wstring text;
};

struct SubtitleTrack {
    unsigned number = 0;
    uint32_t duration = 0;
    std::vector<SubtitleCue> cues;
    std::string archive = "XA02_37.DAT";
};

static constexpr std::array<const char*, 5> subtitleArchives = {
    "XA02_37.DAT", "XA12_37.DAT", "XA15_37.DAT", "XA1F_37.DAT", "XA40_37.DAT"
};
static constexpr std::array<unsigned, 5> archiveTrackLimits = {30, 23, 19, 26, 8};

static int SubtitleArchiveIndex(const char* archive) {
    if (!archive) return -1;
    for (size_t index = 0; index < subtitleArchives.size(); ++index) {
        if (!_stricmp(archive, subtitleArchives[index])) return static_cast<int>(index);
    }
    return -1;
}

static unsigned DiagnosticTrackNumber(const SubtitleTrack& track) {
    int index = SubtitleArchiveIndex(track.archive.c_str());
    return index >= 0 ? static_cast<unsigned>(index) * 32 + track.number : 0;
}

struct VoiceBinding {
    uint32_t caller = 0;
    uint32_t callback = 0;
    std::array<unsigned char, 16> callBytes = {};
    std::array<unsigned char, 16> callbackBytes = {};
    uint32_t dispatch = 0;
    std::array<uint32_t, 3> dispatchWords = {};
};

struct CaptionScript {
    uint32_t start = 0;
    uint32_t end = 0;
    std::vector<uint32_t> words;
    std::vector<VoiceBinding> voices;
};

struct CaptionScene {
    uint8_t type = 0;
    uint32_t wrapper = 0;
    std::array<unsigned char, 16> wrapperBytes = {};
    std::vector<VoiceBinding> voices;
};

struct SceneContext {
    bool readable = false;
    uint8_t flags = 0;
    uint8_t type = 0;
    uint32_t script = 0;
    uint32_t descriptor = 0;
    std::array<uint32_t, 5> messageFlags = {};
};

enum class CaptionGate { Allowed, Unknown, OutsideScript, NativeMessage };

static constexpr uintptr_t openingVoiceCaller = 0x512eb3;
static constexpr uint32_t openingScriptStart = 0x89b790;
static constexpr uint32_t openingScriptEnd = 0x89b890;

static CaptionGate EvaluateContext(const SceneContext& context, const CaptionScript& script) {
    if (!context.readable) return CaptionGate::Unknown;
    if (!(context.flags & 1) || context.script < script.start || context.script >= script.end ||
        (context.script - script.start) % 8) return CaptionGate::OutsideScript;
    for (uint32_t flags : context.messageFlags) {
        if (flags & 0x8080) return CaptionGate::NativeMessage;
    }
    return CaptionGate::Allowed;
}

static CaptionGate EvaluateContext(const SceneContext& context, const CaptionScript* script, const CaptionScene* scene) {
    if (script) return EvaluateContext(context, *script);
    if (!scene || !context.readable) return CaptionGate::Unknown;
    if (!(context.flags & 1) || context.type != scene->type || context.script) return CaptionGate::OutsideScript;
    for (uint32_t flags : context.messageFlags) {
        if (flags & 0x8080) return CaptionGate::NativeMessage;
    }
    return CaptionGate::Allowed;
}

struct PlaybackClock {
    uint32_t capacity = 0;
    uint32_t bytesPerSecond = 0;
    uint32_t cursor = 0;
    uint64_t playedBytes = 0;
    ULONGLONG sampledAt = 0;
    bool running = false;
    bool valid = false;

    void Reset(uint32_t size, uint32_t rate, uint32_t position, ULONGLONG now) {
        capacity = size;
        bytesPerSecond = rate;
        cursor = position;
        playedBytes = position;
        sampledAt = now;
        running = true;
        valid = size && rate && position < size;
    }

    bool Sample(uint32_t position, ULONGLONG now, bool playing) {
        if (!valid || position >= capacity || now < sampledAt ||
            (running && (now - sampledAt) * bytesPerSecond >= static_cast<uint64_t>(capacity) * 1000)) {
            valid = false;
            return false;
        }
        playedBytes += (static_cast<uint64_t>(position) + capacity - cursor) % capacity;
        cursor = position;
        sampledAt = now;
        running = playing;
        return true;
    }

    uint64_t Milliseconds() const {
        return bytesPerSecond ? playedBytes * 1000 / bytesPerSecond : 0;
    }
};

static HMODULE localModule;
static std::wstring localDirectory;
static std::mutex hookMutex;
static std::vector<MethodHook> hooks;
static CreateDrawFunction originalCreateDraw;
static CreateDrawExFunction originalCreateDrawEx;
static ULONGLONG firstFrame;
static unsigned drawnFrames;
static unsigned presentedFrames;
static std::array<bool, 163> savedComparison = {};
static std::array<bool, 163> savedWindow = {};
static ULONGLONG lastCaptureAttempt;
static int reportedFontCue = -1;
static unsigned reportedFontTrack;
static thread_local bool insidePresentation;
static thread_local int renderedCue = -1;
static thread_local unsigned renderedTrack;
using LoadVoiceFunction = void(__cdecl*)(unsigned);
static LoadVoiceFunction originalLoadVoice;
static IDirectSoundBuffer* observedVoice;
static ULONGLONG lastVoiceSample;
static ULONGLONG voiceStartedAt;
static std::mutex voiceMutex;
static PlaybackClock voiceClock;
static int voiceSlot = -1;
static int activeCue = -1;
static HANDLE voiceTimer;
static std::vector<SubtitleTrack> subtitleTracks;
static std::vector<CaptionScript> captionScripts;
static std::vector<CaptionScene> captionScenes;
static const SubtitleTrack* activeTrack;
static const CaptionScript* activeScript;
static const CaptionScene* activeScene;

struct CaptionSelection {
    const SubtitleTrack* track = nullptr;
    const CaptionScript* script = nullptr;
    const VoiceBinding* voice = nullptr;
    const CaptionScene* scene = nullptr;
};

static CaptionSelection SelectCaption(uintptr_t caller, unsigned request, const char* archive,
    unsigned zeroBasedTrack, const SceneContext& context) {
    int archiveIndex = SubtitleArchiveIndex(archive);
    if (request >= 0x7f00 || archiveIndex < 0 || zeroBasedTrack >= archiveTrackLimits[archiveIndex]) return {};
    for (const auto& track : subtitleTracks) {
        if (track.number != zeroBasedTrack + 1 || _stricmp(track.archive.c_str(), archive)) continue;
        for (const auto& script : captionScripts) {
            if (EvaluateContext(context, script) != CaptionGate::Allowed) continue;
            for (const auto& voice : script.voices) {
                if (voice.caller == caller) return {&track, &script, &voice};
            }
        }
        for (const auto& scene : captionScenes) {
            if (EvaluateContext(context, nullptr, &scene) != CaptionGate::Allowed) continue;
            for (const auto& voice : scene.voices) {
                if (voice.caller == caller) return {&track, nullptr, &voice, &scene};
            }
        }
    }
    return {};
}

static void Log(const std::string& text) {
    std::string line = std::to_string(GetCurrentProcessId()) + " " + text + "\r\n";
    HANDLE output = CreateFileW((localDirectory + L"\\SubtitleTest.log").c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(output, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(output);
}

template<typename Function>
static Function Method(void* object, unsigned slot) {
    return reinterpret_cast<Function>((*static_cast<void***>(object))[slot]);
}

template<typename Function>
static Function Original(void* object, unsigned slot) {
    void* target = (*static_cast<void***>(object))[slot];
    std::lock_guard<std::mutex> guard(hookMutex);
    for (const auto& hook : hooks) {
        if (hook.target == target) return reinterpret_cast<Function>(hook.original);
    }
    return nullptr;
}

static void* Install(void* target, void* callback) {
    if (!target) return nullptr;
    std::lock_guard<std::mutex> guard(hookMutex);
    for (const auto& hook : hooks) {
        if (hook.target == target) return hook.original;
    }
    void* original = nullptr;
    MH_STATUS status = MH_CreateHook(target, callback, &original);
    if (status == MH_OK) {
        hooks.push_back({target, original});
        status = MH_EnableHook(target);
        if (status != MH_OK) {
            hooks.pop_back();
            MH_RemoveHook(target);
        }
    }
    if (status != MH_OK) {
        Log("Hook failed status=" + std::to_string(static_cast<int>(status)));
        return nullptr;
    }
    return original;
}

static bool IsPrimary(void* surface) {
    DDSCAPS2 caps = {};
    using GetCapsFunction = HRESULT(WINAPI*)(void*, DDSCAPS2*);
    return surface && SUCCEEDED(Method<GetCapsFunction>(surface, 14)(surface, &caps)) &&
        (caps.dwCaps & DDSCAPS_PRIMARYSURFACE);
}

template<typename Value>
static bool ReadGame(uintptr_t address, Value& value) {
    SIZE_T returned = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), &value,
        sizeof(value), &returned) && returned == sizeof(value);
}

static SceneContext ReadSceneContext() {
    SceneContext context;
    uint8_t header[4] = {};
    if (!ReadGame(0xa6fb40, header) || !ReadGame(0xa6fbec, context.script) ||
        !ReadGame(0xa6a7a8, context.descriptor)) return context;
    context.flags = header[0];
    context.type = header[1];
    for (size_t index = 0; index < context.messageFlags.size(); ++index) {
        if (!ReadGame(0xa78360 + index * 0x80, context.messageFlags[index])) return context;
    }
    context.readable = true;
    return context;
}

static void InitializeDirectory() {
    wchar_t filename[MAX_PATH] = {};
    GetModuleFileNameW(localModule, filename, MAX_PATH);
    localDirectory = filename;
    localDirectory.resize(localDirectory.find_last_of(L'\\'));
}

static bool ReadSubtitleJson(const std::wstring& filename, nlohmann::json& document) {
    HANDLE input = CreateFileW(filename.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) { Log("Subtitle data file not found."); return false; }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(input, &size) || size.QuadPart < 1 || size.QuadPart > 4 * 1024 * 1024) {
        CloseHandle(input);
        return false;
    }
    std::vector<char> contents(static_cast<size_t>(size.QuadPart));
    DWORD returned = 0;
    bool read = ReadFile(input, contents.data(), static_cast<DWORD>(contents.size()), &returned, nullptr) &&
        returned == contents.size();
    CloseHandle(input);
    if (!read) return false;
    document = nlohmann::json::parse(contents.begin(), contents.end(), nullptr, false);
    return !document.is_discarded() && document.is_object();
}

static bool ParseSubtitleTrack(nlohmann::json& document, unsigned number, SubtitleTrack& track) {
    if (!document.is_object() || document["version"] != 1 ||
        !document["archive"].is_string() || document["track"] != number ||
        document["language"] != "zh-Hant" ||
        !document["duration_ms"].is_number_unsigned() || document["duration_ms"] > 600000 ||
        !document["cues"].is_array() || document["cues"].empty() || document["cues"].size() > 128) return false;
    std::string archive = document["archive"].get<std::string>();
    int archiveIndex = SubtitleArchiveIndex(archive.c_str());
    if (archiveIndex < 0 || archive != subtitleArchives[archiveIndex] ||
        !number || number > archiveTrackLimits[archiveIndex]) return false;
    uint32_t duration = document["duration_ms"].get<uint32_t>();
    std::vector<SubtitleCue> parsed;
    uint32_t previousEnd = 0;
    for (const auto& entry : document["cues"]) {
        if (!entry.is_object() || !entry.contains("start_ms") || !entry.contains("end_ms") ||
            !entry.contains("text") || !entry["start_ms"].is_number_unsigned() ||
            !entry["end_ms"].is_number_unsigned() || !entry["text"].is_string() ||
            entry["start_ms"] >= entry["end_ms"] || entry["end_ms"] > duration ||
            entry["start_ms"] < previousEnd) return false;
        std::string utf8 = entry["text"].get<std::string>();
        if (utf8.empty() || utf8.size() > 512 || utf8.find('\0') != std::string::npos) return false;
        int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
            static_cast<int>(utf8.size()), nullptr, 0);
        if (!characters) return false;
        std::wstring text(characters, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
            text.data(), characters);
        unsigned lines = 1, lineLength = 0;
        for (wchar_t character : text) {
            if (character == L'\n') { ++lines; lineLength = 0; }
            else if (character == L'\r' || ++lineLength > 24) return false;
            if (lines > 2) return false;
        }
        parsed.push_back({entry["start_ms"].get<uint32_t>(), entry["end_ms"].get<uint32_t>(), std::move(text)});
        previousEnd = parsed.back().end;
    }
    track = {number, duration, std::move(parsed), std::move(archive)};
    return true;
}

static bool Unsigned32(const nlohmann::json& value) {
    return value.is_number_unsigned() && value <= 0xffffffffu;
}

static bool ParseSignature(const nlohmann::json& value, std::array<unsigned char, 16>& bytes) {
    if (!value.is_array() || value.size() != bytes.size()) return false;
    for (size_t index = 0; index < bytes.size(); ++index) {
        if (!value[index].is_number_unsigned() || value[index] > 255) return false;
        bytes[index] = value[index].get<unsigned char>();
    }
    return true;
}

static bool ParseVoiceBinding(nlohmann::json& source, VoiceBinding& voice) {
    if (!source.is_object() || !Unsigned32(source["caller"]) || !Unsigned32(source["callback"])) return false;
    voice.caller = source["caller"].get<uint32_t>();
    voice.callback = source["callback"].get<uint32_t>();
    if (voice.caller < 0x512cb0 || voice.caller >= 0x53ebc0 || voice.callback < 0x512ca0 ||
        voice.callback >= voice.caller || !ParseSignature(source["call_bytes"], voice.callBytes) ||
        !ParseSignature(source["callback_bytes"], voice.callbackBytes) || voice.callBytes[11] != 0xe8) return false;
    uint32_t displacement = 0;
    std::memcpy(&displacement, voice.callBytes.data() + 12, sizeof(displacement));
    if (voice.caller + displacement != 0x600ec0) return false;
    if (source.contains("dispatch")) {
        if (!Unsigned32(source["dispatch"]) || source["dispatch"] < 0x89b000 || source["dispatch"] > 0x8afff4 ||
            !source["dispatch_words"].is_array() || source["dispatch_words"].size() != 3) return false;
        voice.dispatch = source["dispatch"].get<uint32_t>();
        bool member = false;
        for (size_t index = 0; index < voice.dispatchWords.size(); ++index) {
            const auto& word = source["dispatch_words"][index];
            if (!Unsigned32(word) || word < 0x512ca0 || word >= 0x53ebc0) return false;
            voice.dispatchWords[index] = word.get<uint32_t>();
            member = member || voice.dispatchWords[index] == voice.callback;
        }
        if (!member) return false;
    }
    return true;
}

static bool LoadSubtitleCatalog() {
    nlohmann::json openingDocument, catalog;
    SubtitleTrack opening;
    if (!ReadSubtitleJson(localDirectory + L"\\subtitles\\opening.zh-Hant.json", openingDocument) ||
        !ParseSubtitleTrack(openingDocument, 1, opening) ||
        opening.archive != "XA02_37.DAT" ||
        !ReadSubtitleJson(localDirectory + L"\\subtitles\\story.zh-Hant.json", catalog)) return false;
    if (catalog["version"] != 1 || catalog["language"] != "zh-Hant" ||
        catalog["game_sha256"] != "48baddc9250dc6b99da7ac15b3ae68b0c088489b7351f79ffb990e3384dd0ebc" ||
        !catalog["tracks"].is_array() || catalog["tracks"].empty() || catalog["tracks"].size() > 105 ||
        !catalog["scripts"].is_array() || catalog["scripts"].empty() || catalog["scripts"].size() > 128 ||
        !catalog["scenes"].is_array() || catalog["scenes"].size() > 256) return false;
    std::vector<SubtitleTrack> tracks;
    std::vector<CaptionScript> scripts;
    std::vector<CaptionScene> scenes;
    tracks.push_back(std::move(opening));
    for (auto& entry : catalog["tracks"]) {
        if (!entry.is_object() || !Unsigned32(entry["track"]) || entry["track"] < 1 || entry["track"] > 30 ||
            entry["runtime_enabled"] != true || entry["status"] != "draft-script-guarded-runtime") return false;
        unsigned number = entry["track"].get<unsigned>();
        SubtitleTrack parsed;
        if (!ParseSubtitleTrack(entry, number, parsed)) return false;
        for (const auto& track : tracks) if (track.number == number && track.archive == parsed.archive) return false;
        tracks.push_back(std::move(parsed));
    }
    for (auto& entry : catalog["scripts"]) {
        if (!entry.is_object() || !Unsigned32(entry["start"]) || !Unsigned32(entry["end"]) ||
            !entry["words"].is_array() || !entry["voices"].is_array() || entry["voices"].empty() ||
            entry["voices"].size() > 128) return false;
        CaptionScript script;
        script.start = entry["start"].get<uint32_t>();
        script.end = entry["end"].get<uint32_t>();
        if (script.start < 0x89b000 || script.end >= 0x8b0000 || script.end <= script.start ||
            script.end - script.start > 8192 || (script.end - script.start) % 8 ||
            entry["words"].size() != (script.end - script.start) / 4) return false;
        for (const auto& existing : scripts) {
            if (script.start < existing.end && existing.start < script.end) return false;
        }
        for (const auto& word : entry["words"]) {
            if (!Unsigned32(word)) return false;
            script.words.push_back(word.get<uint32_t>());
        }
        if (script.words.front() != 0xffff0000) return false;
        for (size_t index = 1; index < script.words.size(); index += 2) {
            if (script.words[index] < 0x400000 || script.words[index] >= 0x830000) return false;
        }
        for (auto& source : entry["voices"]) {
            VoiceBinding voice;
            if (!ParseVoiceBinding(source, voice)) return false;
            bool registered = voice.dispatch != 0;
            for (size_t index = 1; index < script.words.size(); index += 2) {
                registered = registered || script.words[index] == voice.callback;
            }
            if (!registered) return false;
            for (const auto& existing : script.voices) if (existing.caller == voice.caller) return false;
            script.voices.push_back(voice);
        }
        scripts.push_back(std::move(script));
    }
    for (auto& entry : catalog["scenes"]) {
        if (!entry.is_object() || !Unsigned32(entry["type"]) || entry["type"] > 255 ||
            !Unsigned32(entry["wrapper"]) || entry["wrapper"] < 0x512b80 || entry["wrapper"] >= 0x53ebc0 ||
            !entry["voices"].is_array() || entry["voices"].empty() || entry["voices"].size() > 128) return false;
        CaptionScene scene;
        scene.type = entry["type"].get<uint8_t>();
        scene.wrapper = entry["wrapper"].get<uint32_t>();
        if (!ParseSignature(entry["wrapper_bytes"], scene.wrapperBytes)) return false;
        for (const auto& existing : scenes) if (existing.type == scene.type) return false;
        for (auto& source : entry["voices"]) {
            VoiceBinding voice;
            if (!ParseVoiceBinding(source, voice) || !voice.dispatch) return false;
            for (const auto& existing : scene.voices) if (existing.caller == voice.caller) return false;
            scene.voices.push_back(voice);
        }
        scenes.push_back(std::move(scene));
    }
    subtitleTracks = std::move(tracks);
    captionScripts = std::move(scripts);
    captionScenes = std::move(scenes);
    size_t cues = 0, voices = 0;
    for (const auto& track : subtitleTracks) cues += track.cues.size();
    for (const auto& script : captionScripts) voices += script.voices.size();
    for (const auto& scene : captionScenes) voices += scene.voices.size();
    Log("Loaded subtitle catalog tracks=" + std::to_string(subtitleTracks.size()) + " cues=" + std::to_string(cues) +
        " scripts=" + std::to_string(captionScripts.size()) + " direct_scenes=" + std::to_string(captionScenes.size()) +
        " voice_bindings=" + std::to_string(voices));
    return true;
}

static bool VerifySceneBinding(const CaptionSelection& selection) {
    std::array<unsigned char, 16> callBytes = {}, callbackBytes = {};
    std::array<uint32_t, 3> dispatchWords = {};
    if (selection.voice->dispatch && (!ReadGame(selection.voice->dispatch, dispatchWords) ||
        dispatchWords != selection.voice->dispatchWords)) return false;
    if (!ReadGame(selection.voice->caller - 16, callBytes) || callBytes != selection.voice->callBytes ||
        !ReadGame(selection.voice->callback, callbackBytes) || callbackBytes != selection.voice->callbackBytes) return false;
    if (selection.script) {
        std::vector<uint32_t> actual(selection.script->words.size());
        SIZE_T returned = 0;
        uint32_t terminator[2] = {};
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(selection.script->start), actual.data(),
            actual.size() * sizeof(uint32_t), &returned) && returned == actual.size() * sizeof(uint32_t) &&
            actual == selection.script->words && ReadGame(selection.script->end, terminator) &&
            terminator[0] == 255 && terminator[1] == 0;
    }
    if (!selection.scene) return false;
    uint32_t wrapper = 0;
    std::array<unsigned char, 16> wrapperBytes = {};
    return ReadGame(0x89a6d0 + selection.scene->type * 4, wrapper) && wrapper == selection.scene->wrapper &&
        ReadGame(wrapper, wrapperBytes) && wrapperBytes == selection.scene->wrapperBytes;
}

static int FindCue(const SubtitleTrack& track, uint64_t milliseconds) {
    for (size_t index = 0; index < track.cues.size(); ++index) {
        if (milliseconds < track.cues[index].start) break;
        if (milliseconds < track.cues[index].end) return static_cast<int>(index);
    }
    return -1;
}

static void ClearVoice(const char* reason) {
    if (observedVoice) {
        Log(std::string("Subtitle clock ended: ") + reason + " track=" + std::to_string(activeTrack ? DiagnosticTrackNumber(*activeTrack) : 0) +
            " media_ms=" + std::to_string(voiceClock.Milliseconds()));
        observedVoice->Release();
        observedVoice = nullptr;
    }
    voiceSlot = -1;
    voiceClock = {};
    activeCue = -1;
    activeTrack = nullptr;
    activeScript = nullptr;
    activeScene = nullptr;
}

static void LogSceneState() {
    SceneContext context = ReadSceneContext();
    std::ostringstream details;
    details << "Caption context readable=" << context.readable << " script=0x" << std::hex <<
        context.script << " scene=0x" << context.descriptor << " flags=0x" << static_cast<unsigned>(context.flags) <<
        " type=" << static_cast<unsigned>(context.type) << " messages=";
    for (uint32_t value : context.messageFlags) details << value << ":";
    Log(details.str());
}

static void __cdecl LoadVoice(unsigned request) {
    unsigned index = request & 0x7fff;
    uintptr_t entry = 0;
    uint32_t table = 0;
    if (request & 0x8000) {
        if (index >= 0x7f00) entry = 0x8f9848 + index * 4;
        else if (ReadGame(0xa6f7e0, table) && table) entry = table + index * 4;
    } else if (index < 0x7f00 && ReadGame(0xa68100, table) && table) {
        entry = table + index * 4;
    }
    uint16_t identifiers[2] = {};
    uint32_t nameAddress = 0;
    char archive[64] = {};
    bool identified = entry && ReadGame(entry, identifiers) && identifiers[0] < 256 &&
        ReadGame(0x9193f8 + identifiers[0] * 4, nameAddress) && nameAddress && ReadGame(nameAddress, archive);
    archive[sizeof(archive) - 1] = 0;
    uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    CaptionSelection selection = identified ? SelectCaption(caller, request, archive, identifiers[1], ReadSceneContext()) : CaptionSelection{};
    bool eligible = selection.track && VerifySceneBinding(selection);
    std::ostringstream context;
    context << "Voice context caller=0x" << std::hex << caller <<
        " request=0x" << request << " archive=" << (identified ? archive : "unknown") <<
        " track=" << std::dec << (identifiers[1] + 1);
    Log(context.str());
    LogSceneState();
    {
        std::lock_guard<std::mutex> guard(voiceMutex);
        ClearVoice("voice replaced");
    }
    originalLoadVoice(request);
    if (!eligible || EvaluateContext(ReadSceneContext(), selection.script, selection.scene) != CaptionGate::Allowed) {
        Log("Voice excluded from extra subtitles: audio, callback, script signature, or native message state not eligible.");
        return;
    }
    int slot = -1;
    IDirectSoundBuffer* buffer = nullptr;
    if (ReadGame(0x9193f4, slot) && slot >= 0 && slot < 8 && ReadGame(0xa2ae10 + slot * 24, buffer) && buffer) {
        buffer->AddRef();
        DSBCAPS caps = {};
        caps.dwSize = sizeof(caps);
        WAVEFORMATEX format = {};
        DWORD cursor = 0;
        if (FAILED(buffer->GetCaps(&caps)) || FAILED(buffer->GetFormat(&format, sizeof(format), nullptr)) ||
            FAILED(buffer->GetCurrentPosition(&cursor, nullptr)) || !caps.dwBufferBytes ||
            !format.nAvgBytesPerSec || format.wFormatTag != WAVE_FORMAT_PCM) {
            buffer->Release();
            Log("Audio format could not be verified; subtitles disabled for this playback.");
            return;
        }
        std::lock_guard<std::mutex> guard(voiceMutex);
        observedVoice = buffer;
        activeTrack = selection.track;
        activeScript = selection.script;
        activeScene = selection.scene;
        voiceSlot = slot;
        voiceStartedAt = GetTickCount64();
        lastVoiceSample = 0;
        voiceClock.Reset(caps.dwBufferBytes, format.nAvgBytesPerSec, cursor, voiceStartedAt);
        Log("Subtitle voice request=" + std::to_string(request) + " archive=" + archive +
            " track=" + std::to_string(identifiers[1] + 1) + " slot=" + std::to_string(slot) +
            " bufferBytes=" + std::to_string(caps.dwBufferBytes) +
            " bytesPerSecond=" + std::to_string(format.nAvgBytesPerSec));
    }
}

static VOID CALLBACK SampleVoiceClock(PVOID, BOOLEAN) {
    std::lock_guard<std::mutex> guard(voiceMutex);
    if (!observedVoice || !activeTrack || (!activeScript && !activeScene)) return;
    CaptionGate gate = EvaluateContext(ReadSceneContext(), activeScript, activeScene);
    if (gate == CaptionGate::Unknown || gate == CaptionGate::OutsideScript) {
        LogSceneState();
        ClearVoice("cinematic context ended or cannot be verified");
        return;
    }
    IDirectSoundBuffer* current = nullptr;
    uint32_t engineFlags = 0;
    if (!ReadGame(0xa2ae10 + voiceSlot * 24, current) || current != observedVoice ||
        !ReadGame(0xa2ae24 + voiceSlot * 24, engineFlags) || !(engineFlags & 4)) {
        ClearVoice("audio stopped or released");
        return;
    }
    DWORD cursor = 0, status = 0;
    if (FAILED(observedVoice->GetCurrentPosition(&cursor, nullptr)) || FAILED(observedVoice->GetStatus(&status)) ||
        (status & DSBSTATUS_BUFFERLOST)) {
        ClearVoice("audio device unavailable");
        return;
    }
    ULONGLONG now = GetTickCount64();
    bool wasRunning = voiceClock.running;
    bool playing = (status & DSBSTATUS_PLAYING) != 0;
    if (!voiceClock.Sample(cursor, now, playing)) {
        ClearVoice("ambiguous ring-buffer sample gap");
        return;
    }
    uint64_t milliseconds = voiceClock.Milliseconds();
    if (milliseconds > activeTrack->duration + 1000) { ClearVoice("subtitle duration exceeded"); return; }
    int selected = gate == CaptionGate::Allowed ? FindCue(*activeTrack, milliseconds) : -1;
    if (selected != activeCue) {
        activeCue = selected;
        Log("Track=" + std::to_string(DiagnosticTrackNumber(*activeTrack)) + " Cue=" + std::to_string(selected + 1) +
            " media_ms=" + std::to_string(milliseconds) + " gate=" + std::to_string(static_cast<int>(gate)));
    }
    if (wasRunning != playing) Log(playing ? "Subtitle audio resumed." : "Subtitle audio paused.");
    if (now - lastVoiceSample >= 10000) {
        Log("Subtitle clock track=" + std::to_string(DiagnosticTrackNumber(*activeTrack)) + " media_ms=" + std::to_string(milliseconds) + " wall_ms=" +
            std::to_string(now - voiceStartedAt) + " cursor=" + std::to_string(cursor));
        LogSceneState();
        lastVoiceSample = now;
    }
}

static void ObserveVoice() {
    if (!originalLoadVoice && reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) == 0x400000) {
        const unsigned char expected[] = {0x83, 0xec, 0x30, 0x53, 0x56, 0x8b, 0x74, 0x24, 0x3c, 0x57};
        unsigned char actual[sizeof(expected)] = {};
        const unsigned char expectedMessageQuery[] = {0x8b, 0x91, 0x60, 0x83, 0xa7, 0x00, 0xf6, 0xc2, 0x80};
        unsigned char messageQuery[sizeof(expectedMessageQuery)] = {};
        const unsigned char expectedDispatcher[] = {0xff, 0x14, 0x85, 0xd0, 0xa6, 0x89, 0x00};
        unsigned char dispatcher[sizeof(expectedDispatcher)] = {};
        const uint32_t expectedEntry[] = {0xffff0000, 0x00512e50};
        uint32_t entry[2] = {}, terminator[2] = {};
        if (ReadGame(0x600ec0, actual) && !std::memcmp(actual, expected, sizeof(expected)) &&
            ReadGame(0x557a94, messageQuery) && !std::memcmp(messageQuery, expectedMessageQuery, sizeof(messageQuery)) &&
            ReadGame(0x512b65, dispatcher) && !std::memcmp(dispatcher, expectedDispatcher, sizeof(dispatcher)) &&
            ReadGame(openingScriptStart, entry) && !std::memcmp(entry, expectedEntry, sizeof(entry)) &&
            ReadGame(openingScriptEnd, terminator) && terminator[0] == 0xff && terminator[1] == 0) {
            originalLoadVoice = reinterpret_cast<LoadVoiceFunction>(Install(
                reinterpret_cast<void*>(0x600ec0), reinterpret_cast<void*>(&LoadVoice)));
            if (originalLoadVoice) Log("Verified voice loader and native message-state bindings installed; each cinematic binding is checked at playback.");
        }
    }
}

static int CaptureSlot(unsigned track, int cue) {
    if (track > 1) return track <= 160 && cue == 0 ? static_cast<int>(track + 2) : -1;
    if (cue == 1) return 0;
    if (cue == 6) return 1;
    if (cue == 18) return 2;
    return -1;
}

static bool SaveFrame(HDC source, const RECT& bounds, const wchar_t* label, bool requireScene = false) {
    int width = bounds.right - bounds.left;
    int height = bounds.bottom - bounds.top;
    if (width < 1 || height < 1 || width > 4096 || height > 4096) return false;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = static_cast<DWORD>(width * height * 4);
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HDC memory = CreateCompatibleDC(source);
    if (!bitmap || !memory) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        return false;
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    bool saved = false;
    if (BitBlt(memory, 0, 0, width, height, source, bounds.left, bounds.top, SRCCOPY)) {
        GdiFlush();
        unsigned coloredPixels = 0;
        auto* colors = static_cast<unsigned char*>(pixels);
        for (int pixel = 0; pixel < width * height * 3 / 4; ++pixel) {
            if (colors[pixel * 4] > 16 || colors[pixel * 4 + 1] > 16 || colors[pixel * 4 + 2] > 16) ++coloredPixels;
        }
        if (requireScene && coloredPixels < static_cast<unsigned>(width * height / 200)) {
            SelectObject(memory, previous);
            DeleteDC(memory);
            DeleteObject(bitmap);
            return false;
        }
        BITMAPFILEHEADER header = {};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + info.bmiHeader.biSizeImage;
        std::wstring filename = localDirectory + L"\\build\\subtitle-test-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + label + L".bmp";
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
    }
    SelectObject(memory, previous);
    DeleteDC(memory);
    DeleteObject(bitmap);
    return saved;
}

static bool DrawSubtitle(void* surface) {
    ObserveVoice();
    if (!surface) return false;
    std::wstring text;
    int cueIndex = -1;
    unsigned trackNumber = 0;
    {
        std::lock_guard<std::mutex> guard(voiceMutex);
        if (!activeTrack || EvaluateContext(ReadSceneContext(), activeScript, activeScene) != CaptionGate::Allowed) return false;
        cueIndex = activeCue;
        trackNumber = DiagnosticTrackNumber(*activeTrack);
        if (cueIndex >= 0) text = activeTrack->cues[cueIndex].text;
    }
    if (text.empty()) return false;
    HDC context = nullptr;
    using GetDCFunction = HRESULT(WINAPI*)(void*, HDC*);
    using ReleaseDCFunction = HRESULT(WINAPI*)(void*, HDC);
    HRESULT result = Method<GetDCFunction>(surface, 17)(surface, &context);
    if (FAILED(result)) {
        static bool reported = false;
        if (!reported) { Log("Surface GetDC failed=" + std::to_string(result)); reported = true; }
        return false;
    }
    RECT bounds = {};
    int clipType = GetClipBox(context, &bounds);
    bool drawn = false;
    if (clipType != ERROR && bounds.right - bounds.left >= 320 && bounds.bottom - bounds.top >= 200) {
        ULONGLONG now = GetTickCount64();
        if (!firstFrame) firstFrame = now;
        int captureSlot = CaptureSlot(trackNumber, cueIndex);
        bool capture = captureSlot >= 0 && !savedComparison[captureSlot] && now - lastCaptureAttempt >= 1000;
        if (capture) lastCaptureAttempt = now;
        std::wstring capturePrefix = L"track-" + std::to_wstring(trackNumber) + L"-cue-" + std::to_wstring(cueIndex + 1);
        bool beforeSaved = capture && SaveFrame(context, bounds, (capturePrefix + L"-before").c_str(), true);
        int savedState = SaveDC(context);
        int height = bounds.bottom - bounds.top;
        int fontHeight = (std::max)(20, height / 20);
        HFONT font = CreateFontW(-fontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH, L"Microsoft JhengHei");
        if (savedState && font) {
            SelectObject(context, font);
            if (reportedFontTrack != trackNumber || reportedFontCue != cueIndex) {
                std::vector<WORD> glyphs(text.size());
                DWORD count = GetGlyphIndicesW(context, text.c_str(), static_cast<int>(text.size()), glyphs.data(),
                    GGI_MARK_NONEXISTING_GLYPHS);
                unsigned missing = 0;
                for (size_t index = 0; index < text.size(); ++index) {
                    if (text[index] != L'\n' && glyphs[index] == 0xffff) ++missing;
                }
                Log("Track=" + std::to_string(trackNumber) + " Cue=" + std::to_string(cueIndex + 1) + " Unicode glyphs missing=" + std::to_string(missing) +
                    " result=" + std::to_string(count) +
                    " surface=" + std::to_string(bounds.right - bounds.left) + "x" + std::to_string(height));
                reportedFontCue = cueIndex;
                reportedFontTrack = trackNumber;
            }
            RECT measured = {0, 0, bounds.right - bounds.left - 40, 0};
            DrawTextW(context, text.c_str(), -1, &measured, DT_CALCRECT | DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
            RECT textBounds = {bounds.left + 20, bounds.bottom - fontHeight - measured.bottom,
                bounds.right - 20, bounds.bottom - fontHeight};
            SetBkMode(context, TRANSPARENT);
            SetTextColor(context, RGB(0, 0, 0));
            for (int vertical = -2; vertical <= 2; vertical += 2) {
                for (int horizontal = -2; horizontal <= 2; horizontal += 2) {
                    if (!horizontal && !vertical) continue;
                    RECT outline = textBounds;
                    OffsetRect(&outline, horizontal, vertical);
                    DrawTextW(context, text.c_str(), -1, &outline, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
                }
            }
            SetTextColor(context, RGB(255, 255, 255));
            drawn = DrawTextW(context, text.c_str(), -1, &textBounds, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX) > 0;
        }
        if (savedState) RestoreDC(context, savedState);
        if (font) DeleteObject(font);
        if (drawn) { ++drawnFrames; renderedCue = cueIndex; renderedTrack = trackNumber; }
        if (beforeSaved && SaveFrame(context, bounds, (capturePrefix + L"-after").c_str())) {
            savedComparison[captureSlot] = true;
            Log("Saved same-frame subtitle images track=" + std::to_string(trackNumber) + " cue=" + std::to_string(cueIndex + 1));
        }
    }
    Method<ReleaseDCFunction>(surface, 26)(surface, context);
    return drawn;
}

static void RecordPresentation(HRESULT result, bool drawn, const char* method) {
    if (FAILED(result) || !drawn) return;
    ++presentedFrames;
    if (presentedFrames == 1 || presentedFrames == 300) {
        Log(std::string("Subtitle presentation ") + method + " success frames=" + std::to_string(presentedFrames));
    }
    int captureSlot = CaptureSlot(renderedTrack, renderedCue);
    if (captureSlot >= 0 && !savedWindow[captureSlot] && presentedFrames % 30 == 0) {
        HWND window = GetForegroundWindow();
        DWORD identifier = 0;
        GetWindowThreadProcessId(window, &identifier);
        wchar_t className[32] = {};
        GetClassNameW(window, className, 32);
        if (identifier == GetCurrentProcessId() && !wcscmp(className, L"Dash2")) {
            RECT bounds = {};
            POINT origin = {};
            if (GetClientRect(window, &bounds) && ClientToScreen(window, &origin)) {
                OffsetRect(&bounds, origin.x, origin.y);
                HDC screen = GetDC(nullptr);
                if (screen) {
                    std::wstring label = L"track-" + std::to_wstring(renderedTrack) + L"-cue-" + std::to_wstring(renderedCue + 1) + L"-displayed";
                    savedWindow[captureSlot] = SaveFrame(screen, bounds, label.c_str(), true);
                    ReleaseDC(nullptr, screen);
                    if (savedWindow[captureSlot]) Log("Saved displayed subtitle screenshot track=" + std::to_string(renderedTrack) + " cue=" + std::to_string(renderedCue + 1));
                }
            }
        }
    }
}

static HRESULT WINAPI Flip(void* surface, void* overrideSurface, DWORD flags) {
    auto original = Original<FlipFunction>(surface, 11);
    if (insidePresentation || !IsPrimary(surface)) return original(surface, overrideSurface, flags);
    insidePresentation = true;
    void* backBuffer = overrideSurface;
    bool acquired = false;
    if (!backBuffer) {
        DDSCAPS2 caps = {};
        caps.dwCaps = DDSCAPS_BACKBUFFER;
        using GetAttachedFunction = HRESULT(WINAPI*)(void*, DDSCAPS2*, void**);
        acquired = SUCCEEDED(Method<GetAttachedFunction>(surface, 12)(surface, &caps, &backBuffer));
    }
    bool drawn = DrawSubtitle(backBuffer);
    if (acquired) Method<ULONG(WINAPI*)(void*)>(backBuffer, 2)(backBuffer);
    HRESULT result = original(surface, overrideSurface, flags);
    RecordPresentation(result, drawn, "Flip");
    insidePresentation = false;
    return result;
}

static HRESULT WINAPI Blt(void* surface, RECT* destination, void* source, RECT* sourceBounds,
    DWORD flags, DDBLTFX* effects) {
    auto original = Original<BltFunction>(surface, 5);
    if (insidePresentation || !source || !IsPrimary(surface)) {
        return original(surface, destination, source, sourceBounds, flags, effects);
    }
    insidePresentation = true;
    bool drawn = DrawSubtitle(source);
    HRESULT result = original(surface, destination, source, sourceBounds, flags, effects);
    RecordPresentation(result, drawn, "Blt");
    insidePresentation = false;
    return result;
}

static void TrackDraw(void* object);
static void TrackSurface(void* object);

static HRESULT WINAPI Query(void* object, REFIID identifier, void** output) {
    auto original = Original<QueryFunction>(object, 0);
    HRESULT result = original(object, identifier, output);
    if (SUCCEEDED(result) && output && *output) {
        if (identifier == IID_IDirectDraw || identifier == IID_IDirectDraw2 ||
            identifier == IID_IDirectDraw4 || identifier == IID_IDirectDraw7) TrackDraw(*output);
        else if (identifier == IID_IDirectDrawSurface || identifier == IID_IDirectDrawSurface2 ||
            identifier == IID_IDirectDrawSurface3 || identifier == IID_IDirectDrawSurface4 ||
            identifier == IID_IDirectDrawSurface7) TrackSurface(*output);
    }
    return result;
}

static HRESULT WINAPI CreateSurface(void* object, DDSURFACEDESC2* description, void** output, IUnknown* outer) {
    auto original = Original<CreateSurfaceFunction>(object, 6);
    HRESULT result = original(object, description, output, outer);
    if (SUCCEEDED(result) && output && *output) TrackSurface(*output);
    return result;
}

static void TrackDraw(void* object) {
    Install(Method<void*>(object, 0), reinterpret_cast<void*>(&Query));
    Install(Method<void*>(object, 6), reinterpret_cast<void*>(&CreateSurface));
}

static void TrackSurface(void* object) {
    if (!IsPrimary(object)) return;
    Log("Primary presentation surface observed.");
    Install(Method<void*>(object, 0), reinterpret_cast<void*>(&Query));
    Install(Method<void*>(object, 11), reinterpret_cast<void*>(&Flip));
    Install(Method<void*>(object, 5), reinterpret_cast<void*>(&Blt));
}

static HRESULT WINAPI CreateDraw(GUID* identifier, void** output, IUnknown* outer) {
    HRESULT result = originalCreateDraw(identifier, output, outer);
    if (SUCCEEDED(result) && output && *output) TrackDraw(*output);
    return result;
}

static HRESULT WINAPI CreateDrawEx(GUID* identifier, void** output, REFIID interfaceId, IUnknown* outer) {
    HRESULT result = originalCreateDrawEx(identifier, output, interfaceId, outer);
    if (SUCCEEDED(result) && output && *output) {
        Log("DirectDrawCreateEx succeeded.");
        TrackDraw(*output);
    }
    return result;
}

extern "C" __declspec(dllexport) int __cdecl Diagnose() {
    InitializeDirectory();
    if (!LoadSubtitleCatalog()) { Log("Subtitle catalog validation: FAIL"); return 2; }
    PlaybackClock clock;
    clock.Reset(1000, 1000, 900, 1000);
    bool wrap = clock.Sample(100, 1200, true) && clock.Milliseconds() == 1100;
    bool pause = clock.Sample(100, 1210, false) && clock.Sample(100, 5000, false) && clock.Milliseconds() == 1100;
    bool resume = clock.Sample(200, 5100, true) && clock.Milliseconds() == 1200;
    bool gap = !clock.Sample(200, 7000, true) && !clock.valid;
    const SubtitleTrack& openingTrack = subtitleTracks.front();
    bool boundaries = FindCue(openingTrack, 0) == -1;
    for (const auto& track : subtitleTracks) {
        boundaries = boundaries && FindCue(track, track.cues.front().start) == 0 && FindCue(track, track.cues.back().end) == -1;
    }
    SubtitleTrack immediate = {2, 2000, {{0, 1000, L"First"}, {1500, 2000, L"Second"}}};
    boundaries = boundaries && FindCue(immediate, 0) == 0 && FindCue(immediate, 1000) == -1 &&
        FindCue(immediate, 1500) == 1 && FindCue(immediate, 2000) == -1 && FindCue(openingTrack, 0) == -1;
    bool eligibility = true;
    size_t bindingCases = 0;
    for (const auto& script : captionScripts) {
        for (const auto& voice : script.voices) {
            SceneContext context;
            context.readable = true;
            context.flags = 1;
            context.type = 3;
            context.script = script.start;
            context.descriptor = 0x8d3040;
            for (const auto& track : subtitleTracks) {
                CaptionSelection selected = SelectCaption(voice.caller, 0, track.archive.c_str(), track.number - 1, context);
                eligibility = eligibility && selected.track == &track && selected.script == &script;
                ++bindingCases;
            }
            eligibility = eligibility && !SelectCaption(0x55a6da, 0, "XA02_37.DAT", 0, context).track &&
                !SelectCaption(voice.caller, 0, "XA02_37.DAT", 30, context).track &&
                !SelectCaption(voice.caller, 0, "XA10_18.DAT", 0, context).track &&
                !SelectCaption(voice.caller, 0x8000, "XA02_37.DAT", 0, context).track &&
                !SelectCaption(voice.caller + 1, 0, "XA02_37.DAT", 0, context).track;
            for (unsigned scenario = 0; scenario < 7; ++scenario) {
                SceneContext rejected = context;
                if (scenario == 0) rejected.readable = false;
                if (scenario == 1) rejected.flags = 0;
                if (scenario == 2) rejected.flags = 0x40;
                if (scenario == 3) rejected.flags = 0x80;
                if (scenario == 4) rejected.script = script.end;
                if (scenario == 5) rejected.script += 1;
                if (scenario == 6) rejected.script = script.start - 8;
                eligibility = eligibility && !SelectCaption(voice.caller, 0, "XA02_37.DAT", 0, rejected).track;
            }
            SceneContext interior = context;
            interior.descriptor = 0x8d3098;
            interior.type = 4;
            interior.script = script.end - 8;
            eligibility = eligibility && EvaluateContext(interior, script) == CaptionGate::Allowed;
            for (size_t channel = 0; channel < context.messageFlags.size(); ++channel) {
                for (uint32_t flag : {0x80u, 0x8000u}) {
                    SceneContext dialog = context;
                    dialog.messageFlags[channel] = flag;
                    eligibility = eligibility && EvaluateContext(dialog, script) == CaptionGate::NativeMessage &&
                        !SelectCaption(voice.caller, 0, "XA02_37.DAT", 0, dialog).track;
                }
            }
        }
    }
    const auto& firstScript = captionScripts.front();
    for (const auto& scene : captionScenes) {
        SceneContext context;
        context.readable = true;
        context.flags = 1;
        context.type = scene.type;
        for (const auto& voice : scene.voices) {
            for (const auto& track : subtitleTracks) {
                CaptionSelection selected = SelectCaption(voice.caller, 0, track.archive.c_str(), track.number - 1, context);
                eligibility = eligibility && selected.track == &track && selected.scene == &scene;
                ++bindingCases;
            }
            eligibility = eligibility && !SelectCaption(0x55a6da, 0, "XA02_37.DAT", 0, context).track &&
                !SelectCaption(voice.caller, 0x8000, "XA02_37.DAT", 0, context).track;
            for (unsigned scenario = 0; scenario < 4; ++scenario) {
                SceneContext rejected = context;
                if (scenario == 0) rejected.readable = false;
                if (scenario == 1) rejected.flags = 0;
                if (scenario == 2) rejected.type = static_cast<uint8_t>(scene.type + 1);
                if (scenario == 3) rejected.script = openingScriptStart;
                eligibility = eligibility && EvaluateContext(rejected, nullptr, &scene) != CaptionGate::Allowed;
            }
            for (size_t channel = 0; channel < context.messageFlags.size(); ++channel) {
                for (uint32_t flag : {0x80u, 0x8000u}) {
                    SceneContext dialog = context;
                    dialog.messageFlags[channel] = flag;
                    eligibility = eligibility && EvaluateContext(dialog, nullptr, &scene) == CaptionGate::NativeMessage &&
                        !SelectCaption(voice.caller, 0, "XA02_37.DAT", 0, dialog).track;
                }
            }
        }
    }
    SceneContext opening;
    opening.readable = true;
    opening.flags = 1;
    opening.type = 3;
    opening.script = openingScriptStart;
    auto savedTracks = std::move(subtitleTracks);
    subtitleTracks = {
        {1, 1000, {{0, 500, L"Original bank"}}, "XA02_37.DAT"},
        {1, 1000, {{0, 500, L"Different bank"}}, "XA12_37.DAT"}
    };
    bool bankIdentity = SelectCaption(openingVoiceCaller, 0, "XA02_37.DAT", 0, opening).track == &subtitleTracks[0] &&
        SelectCaption(openingVoiceCaller, 0, "XA12_37.DAT", 0, opening).track == &subtitleTracks[1] &&
        !SelectCaption(openingVoiceCaller, 0, "XA15_37.DAT", 0, opening).track &&
        !SelectCaption(openingVoiceCaller, 0, "XA12_18.DAT", 0, opening).track &&
        !SelectCaption(openingVoiceCaller, 0, "XA12_37.DAT", 23, opening).track &&
        !SelectCaption(openingVoiceCaller, 0x8000, "XA12_37.DAT", 0, opening).track &&
        !SelectCaption(0x55a6da, 0, "XA12_37.DAT", 0, opening).track &&
        DiagnosticTrackNumber(subtitleTracks[0]) != DiagnosticTrackNumber(subtitleTracks[1]);
    for (size_t channel = 0; channel < opening.messageFlags.size(); ++channel) {
        SceneContext nativeDialog = opening;
        nativeDialog.messageFlags[channel] = 0x8080;
        bankIdentity = bankIdentity && !SelectCaption(openingVoiceCaller, 0, "XA12_37.DAT", 0, nativeDialog).track;
    }
    subtitleTracks = std::move(savedTracks);
    Log(std::string("Archive-qualified subtitle selection and cross-bank NPC exclusions: ") + (bankIdentity ? "PASS" : "FAIL"));
    eligibility = eligibility && firstScript.end == openingScriptEnd &&
        SelectCaption(openingVoiceCaller, 0, "XA02_37.DAT", 0, opening).track == &openingTrack &&
        SelectCaption(0x5130df, 3, "XA02_37.DAT", 3, opening).track != nullptr;
    activeTrack = &openingTrack;
    activeScript = &firstScript;
    activeCue = 0;
    ClearVoice("diagnostic transition");
    bool cleared = !activeTrack && !activeScript && !activeScene && activeCue == -1 && voiceSlot == -1 && !voiceClock.valid;
    Log("Cinematic gate selections=" + std::to_string(bindingCases) + " with wrong caller/audio/script and native-message exclusions: " +
        (eligibility && cleared ? "PASS" : "FAIL"));
    bool passed = wrap && pause && resume && gap && boundaries && eligibility && cleared && bankIdentity;
    Log(std::string("Subtitle clock wrap/pause/resume/stall and cue-boundary tests: ") + (passed ? "PASS" : "FAIL"));
    return passed ? 0 : 2;
}

extern "C" __declspec(dllexport) int __cdecl Setup() {
    wchar_t filename[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, filename, MAX_PATH);
    const wchar_t* basename = wcsrchr(filename, L'\\');
    if (!basename || _wcsicmp(basename + 1, L"dash2.exe")) return 0;
    InitializeDirectory();
    HMODULE compatibility = LoadLibraryW((localDirectory + L"\\LocalCompat.dll").c_str());
    using SetupFunction = int(__cdecl*)();
    auto setup = compatibility ? reinterpret_cast<SetupFunction>(GetProcAddress(compatibility, "Setup")) : nullptr;
    if (!setup || setup() != 100) return 0;
    if (!LoadSubtitleCatalog()) {
        Log("Invalid subtitle catalog; normal compatibility remains active without subtitles.");
        return 100;
    }
    Log("Opt-in story subtitles synchronized to live audio with cinematic-script and native-message guards.");
    if (MH_Initialize() != MH_OK) return 0;
    HMODULE directDraw = GetModuleHandleW(L"ddraw.dll");
    if (!directDraw) directDraw = LoadLibraryW(L"ddraw.dll");
    if (!directDraw) return 0;
    originalCreateDraw = reinterpret_cast<CreateDrawFunction>(Install(
        reinterpret_cast<void*>(GetProcAddress(directDraw, "DirectDrawCreate")), reinterpret_cast<void*>(&CreateDraw)));
    originalCreateDrawEx = reinterpret_cast<CreateDrawExFunction>(Install(
        reinterpret_cast<void*>(GetProcAddress(directDraw, "DirectDrawCreateEx")), reinterpret_cast<void*>(&CreateDrawEx)));
    bool timerStarted = CreateTimerQueueTimer(&voiceTimer, nullptr, &SampleVoiceClock, nullptr, 0, 20, WT_EXECUTEDEFAULT);
    return originalCreateDraw && originalCreateDrawEx && timerStarted ? 100 : 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        localModule = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}