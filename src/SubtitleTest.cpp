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

enum class SubtitleTrackKind { Cinematic, GameplayRadio, RollBath, BossRadio, BattleRadio, EndingSong };

static constexpr std::array<const char*, 5> subtitleArchives = {
    "XA02_37.DAT", "XA12_37.DAT", "XA15_37.DAT", "XA1F_37.DAT", "XA40_37.DAT"
};
static constexpr std::array<unsigned, 5> archiveTrackLimits = {30, 23, 19, 26, 8};
static constexpr uint32_t cinematicCallbackEnd = 0x53ecc0;
static constexpr std::array<unsigned, 8> gameplayRadioTrackNumbers = {5, 6, 8, 9, 10, 29, 30, 31};
static constexpr std::array<unsigned, 9> bossRadioTrackNumbers = {31, 32, 38, 39, 33, 34, 35, 36, 37};
static constexpr std::array<unsigned, 5> battleRadioTrackNumbers = {25, 27, 29, 28, 34};
static constexpr unsigned rollBathDiagnosticTrack = 192;
static constexpr unsigned bossRadioDiagnosticStart = 193;
static constexpr unsigned battleRadioDiagnosticStart = bossRadioDiagnosticStart + bossRadioTrackNumbers.size();
static constexpr unsigned endingSongDiagnosticTrack = battleRadioDiagnosticStart + battleRadioTrackNumbers.size();
static constexpr unsigned maximumDiagnosticTrack = endingSongDiagnosticTrack;

static bool IsEndingSongTrack(const SubtitleTrack& track) {
    return track.archive == "STAFF.DAT" && track.number == 1;
}

static bool IsBattleRadioTrack(unsigned number) {
    return std::find(battleRadioTrackNumbers.begin(), battleRadioTrackNumbers.end(), number) != battleRadioTrackNumbers.end();
}

static bool IsBossRadioTrack(unsigned number) {
    return std::find(bossRadioTrackNumbers.begin(), bossRadioTrackNumbers.end(), number) != bossRadioTrackNumbers.end();
}

static bool IsRollBathTrack(const SubtitleTrack& track) {
    return track.archive == "XACOM_18.DAT" && track.number == 4;
}

static bool IsGameplayRadioTrack(unsigned number) {
    return std::find(gameplayRadioTrackNumbers.begin(), gameplayRadioTrackNumbers.end(), number) != gameplayRadioTrackNumbers.end();
}

static bool IsLateGameplayRadioTrack(unsigned number) {
    return number == 29 || number == 30 || number == 31;
}

static size_t GameplayRadioMessageChannel(unsigned number) {
    return number == 30 || number == 31 ? 2 : 4;
}

static uint32_t GameplayRadioDescriptor(unsigned number) {
    return number == 31 ? 0x8ebee0 : 0x8f4660;
}

static int SubtitleArchiveIndex(const char* archive) {
    if (!archive) return -1;
    for (size_t index = 0; index < subtitleArchives.size(); ++index) {
        if (!_stricmp(archive, subtitleArchives[index])) return static_cast<int>(index);
    }
    return -1;
}

static unsigned DiagnosticTrackNumber(const SubtitleTrack& track) {
    if (IsEndingSongTrack(track)) return endingSongDiagnosticTrack;
    if (track.archive == "XA40_18.DAT") {
        for (size_t index = 0; index < battleRadioTrackNumbers.size(); ++index) {
            if (track.number == battleRadioTrackNumbers[index]) return battleRadioDiagnosticStart + index;
        }
    }
    if (track.archive == "XA2D_18.DAT") {
        for (size_t index = 0; index < bossRadioTrackNumbers.size(); ++index) {
            if (track.number == bossRadioTrackNumbers[index]) return bossRadioDiagnosticStart + index;
        }
    }
    if (IsRollBathTrack(track)) return rollBathDiagnosticTrack;
    if (track.archive == "XA26_18.DAT" && IsGameplayRadioTrack(track.number)) {
        return 160 + track.number;
    }
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

struct CaptionContinuation {
    CaptionScript script;
    uint32_t registration = 0;
    std::array<unsigned char, 5> registrationBytes = {};
    uint32_t callback = 0;
    std::array<unsigned char, 16> callbackBytes = {};
    uint32_t scriptPush = 0;
    std::array<unsigned char, 5> scriptPushBytes = {};
    uint32_t contextPush = 0;
    std::array<unsigned char, 5> contextPushBytes = {};
};

struct CaptionScene {
    uint8_t type = 0;
    uint32_t wrapper = 0;
    std::array<unsigned char, 16> wrapperBytes = {};
    std::vector<VoiceBinding> voices;
    std::vector<CaptionContinuation> continuations;
};

struct BossRadioBinding {
    std::array<unsigned char, 16> callBytes = {};
    std::array<unsigned char, 16> wrapperBytes = {};
    std::array<uint32_t, 3> dispatchWords = {};
    CaptionContinuation continuation;
};

struct EndingSongBinding {
    BossRadioBinding controller;
    std::array<unsigned char, 16> playBytes = {};
    std::array<unsigned char, 16> creditsCallBytes = {};
    std::array<unsigned char, 16> creditsCallbackBytes = {};
    std::array<std::array<unsigned char, 64>, 3> audioProbes = {};
};

static constexpr uint32_t endingSongBytes = 3502208;
static constexpr std::array<uint32_t, 3> endingSongProbeOffsets = {0, 1751104, 3502144};

struct BattleRadioControllerSpec {
    uint8_t type;
    uint32_t wrapper;
    uint32_t dispatch;
    std::array<uint32_t, 3> dispatchWords;
    uint32_t scriptStart;
    uint32_t scriptEnd;
    uint32_t registration;
    uint32_t scriptPush;
    uint32_t contextPush;
};

static constexpr std::array<BattleRadioControllerSpec, 2> battleRadioControllers = {{
    {0x59, 0x533dc0, 0x8aa27c, {0x533de0, 0x533fc0, 0x534000}, 0x8a9f80, 0x8aa060, 0x533f31, 0x533f1c, 0x533f21},
    {0x5a, 0x535a40, 0x8aa4c0, {0x535a60, 0x535b80, 0x535be0}, 0x8aa490, 0x8aa4b8, 0x535ae1, 0x535ad2, 0x535ad7}
}};

struct SceneContext {
    bool readable = false;
    uint8_t flags = 0;
    uint8_t type = 0;
    uint32_t script = 0;
    uint32_t descriptor = 0;
    std::array<uint32_t, 5> messageFlags = {};
};

enum class CaptionGate { Allowed, Unknown, OutsideScript, NativeMessage };

static CaptionGate EvaluateEndingSong(const SceneContext& context) {
    if (!context.readable) return CaptionGate::Unknown;
    if (context.type != 0x6e || (context.flags != 1 && context.flags != 5) || context.script != 0x8acc80) {
        return CaptionGate::OutsideScript;
    }
    for (uint32_t flags : context.messageFlags) {
        if (flags & 0x8080) return CaptionGate::NativeMessage;
    }
    return CaptionGate::Allowed;
}

static constexpr uintptr_t openingVoiceCaller = 0x512eb3;
static constexpr uint32_t openingScriptStart = 0x89b790;
static constexpr uint32_t openingScriptEnd = 0x89b890;
static constexpr uint32_t gameplayRadioScriptStart = 0x8a48f0;
static constexpr uint32_t gameplayRadioScriptEnd = 0x8a4928;
static constexpr uint32_t gameplayRadioIdleScriptStart = 0x88b788;
static constexpr uint32_t gameplayRadioIdleScriptEnd = 0x88b7a0;
static constexpr uint32_t gameplayRadioPlaybackBits = 0x3000 | 0x1040;
static constexpr uint32_t rollBathScriptStart = 0x8ad1e8;
static constexpr uint32_t rollBathScriptEnd = 0x8ad200;
static constexpr uint32_t bossRadioScriptStart = 0x8a86b0;
static constexpr uint32_t bossRadioScriptEnd = 0x8a86b8;

static CaptionGate EvaluateBattleRadio(const SceneContext& context, unsigned number) {
    if (!context.readable) return CaptionGate::Unknown;
    if (!IsBattleRadioTrack(number) || context.descriptor != 0x90924c) return CaptionGate::OutsideScript;
    const bool transition = context.type == 0x5a;
    if (transition && number != 25) return CaptionGate::OutsideScript;
    const auto& controller = battleRadioControllers[transition ? 1 : 0];
    if (context.type != controller.type) return CaptionGate::OutsideScript;
    if (transition) {
        if ((context.flags != 1 && context.flags != 5) || (context.script &&
            (context.script < controller.scriptStart || context.script >= controller.scriptEnd ||
                (context.script - controller.scriptStart) % 8))) return CaptionGate::OutsideScript;
    } else if (context.flags != 0 || context.script < controller.scriptStart || context.script > controller.scriptEnd ||
        (context.script - controller.scriptStart) % 8) return CaptionGate::OutsideScript;
    for (size_t channel = 0; channel < 4; ++channel) {
        if (context.messageFlags[channel] & 0x8080) return CaptionGate::NativeMessage;
    }
    const uint32_t mode = context.messageFlags[4] & ~gameplayRadioPlaybackBits;
    if (context.messageFlags[4] != 0 && mode != (0x300c3u & ~gameplayRadioPlaybackBits)) return CaptionGate::NativeMessage;
    return CaptionGate::Allowed;
}

static CaptionGate EvaluateBossRadio(const SceneContext& context) {
    if (!context.readable) return CaptionGate::Unknown;
    if (context.flags != 0 || context.type != 0x4a || context.descriptor != 0x8f2064 ||
        (context.script != bossRadioScriptStart && context.script != bossRadioScriptEnd)) return CaptionGate::OutsideScript;
    for (size_t channel = 0; channel < 4; ++channel) {
        if (context.messageFlags[channel] & 0x8080) return CaptionGate::NativeMessage;
    }
    const uint32_t mode = context.messageFlags[4] & ~gameplayRadioPlaybackBits;
    if (context.messageFlags[4] != 0 && mode != (0x300c3u & ~gameplayRadioPlaybackBits)) return CaptionGate::NativeMessage;
    return CaptionGate::Allowed;
}

static CaptionGate EvaluateGameplayRadio(const SceneContext& context, unsigned number) {
    if (!context.readable) return CaptionGate::Unknown;
    if (!IsGameplayRadioTrack(number) || context.flags != 0 ||
        context.descriptor != GameplayRadioDescriptor(number)) return CaptionGate::OutsideScript;
    if (IsLateGameplayRadioTrack(number)) {
        if (context.type != 0 || context.script != gameplayRadioIdleScriptEnd) return CaptionGate::OutsideScript;
    } else if (context.type != 0x2d ||
        context.script < gameplayRadioScriptStart || context.script > gameplayRadioScriptEnd ||
        (context.script - gameplayRadioScriptStart) % 8) return CaptionGate::OutsideScript;
    const size_t radioChannel = GameplayRadioMessageChannel(number);
    for (size_t channel = 0; channel < context.messageFlags.size(); ++channel) {
        if (channel != radioChannel && (context.messageFlags[channel] & 0x8080)) return CaptionGate::NativeMessage;
    }
    const uint32_t radioMode = context.messageFlags[radioChannel] & ~gameplayRadioPlaybackBits;
    if (context.messageFlags[radioChannel] != 0 && radioMode != (0x300c3u & ~gameplayRadioPlaybackBits)) return CaptionGate::NativeMessage;
    return CaptionGate::Allowed;
}

static CaptionGate EvaluateContext(const SceneContext& context, const CaptionScript& script) {
    if (!context.readable) return CaptionGate::Unknown;
    if (!(context.flags & 1) || context.script < script.start || context.script >= script.end ||
        (context.script - script.start) % 8) return CaptionGate::OutsideScript;
    for (uint32_t flags : context.messageFlags) {
        if (flags & 0x8080) return CaptionGate::NativeMessage;
    }
    return CaptionGate::Allowed;
}

static CaptionGate EvaluateContext(const SceneContext& context, const CaptionScript* script, const CaptionScene* scene,
    const SubtitleTrack* scopedTrack = nullptr) {
    if (scopedTrack && IsEndingSongTrack(*scopedTrack)) return EvaluateEndingSong(context);
    if (scopedTrack && IsRollBathTrack(*scopedTrack)) {
        if (!context.readable) return CaptionGate::Unknown;
        if (context.descriptor != 0x8d4acc || context.type != 0x70 ||
            (context.flags != 1 && context.flags != 5) || !script ||
            script->start != rollBathScriptStart || script->end != rollBathScriptEnd) return CaptionGate::OutsideScript;
    } else if (scopedTrack && scopedTrack->archive == "XA26_18.DAT") {
        return EvaluateGameplayRadio(context, scopedTrack->number);
    } else if (scopedTrack && scopedTrack->archive == "XA2D_18.DAT") {
        return IsBossRadioTrack(scopedTrack->number) ? EvaluateBossRadio(context) : CaptionGate::OutsideScript;
    } else if (scopedTrack && scopedTrack->archive == "XA40_18.DAT") {
        return EvaluateBattleRadio(context, scopedTrack->number);
    }
    if (script) {
        if (scene && context.readable && context.type != scene->type) return CaptionGate::OutsideScript;
        return EvaluateContext(context, *script);
    }
    if (!scene || !context.readable) return CaptionGate::Unknown;
    if (!(context.flags & 1) || context.type != scene->type) return CaptionGate::OutsideScript;
    if (context.script) {
        for (const auto& continuation : scene->continuations) {
            if (context.script >= continuation.script.start && context.script < continuation.script.end) {
                return EvaluateContext(context, continuation.script);
            }
        }
        return CaptionGate::OutsideScript;
    }
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
static std::array<bool, maximumDiagnosticTrack + 3> savedComparison = {};
static std::array<bool, maximumDiagnosticTrack + 3> savedWindow = {};
static ULONGLONG lastCaptureAttempt;
static int reportedFontCue = -1;
static unsigned reportedFontTrack;
static thread_local bool insidePresentation;
static thread_local int renderedCue = -1;
static thread_local unsigned renderedTrack;
using LoadVoiceFunction = void(__cdecl*)(unsigned);
static LoadVoiceFunction originalLoadVoice;
using PlayStreamFunction = uintptr_t(__cdecl*)(unsigned);
static PlayStreamFunction originalPlayStream;
static IDirectSoundBuffer* observedVoice;
static ULONGLONG lastVoiceSample;
static ULONGLONG voiceStartedAt;
static std::mutex voiceMutex;
static PlaybackClock voiceClock;
static int voiceSlot = -1;
static int activeCue = -1;
static HANDLE voiceTimer;
static std::vector<SubtitleTrack> subtitleTracks;
static std::vector<SubtitleTrack> gameplayRadioTracks;
static std::vector<SubtitleTrack> bossRadioTracks;
static BossRadioBinding bossRadioBinding;
static std::vector<SubtitleTrack> battleRadioTracks;
static std::array<BossRadioBinding, 2> battleRadioBindings;
static std::vector<SubtitleTrack> rollBathTracks;
static std::vector<SubtitleTrack> endingSongTracks;
static EndingSongBinding endingSongBinding;
static std::array<unsigned char, 16> gameplayRadioCallBytes = {};
static std::array<unsigned char, 32> gameplayRadioIdleBytes = {};
static std::vector<CaptionScript> captionScripts;
static std::vector<CaptionScene> captionScenes;
static const SubtitleTrack* activeTrack;
static const CaptionScript* activeScript;
static const CaptionScene* activeScene;
static bool activeGameplayRadio = false;

struct CaptionSelection {
    const SubtitleTrack* track = nullptr;
    const CaptionScript* script = nullptr;
    const VoiceBinding* voice = nullptr;
    const CaptionScene* scene = nullptr;
    bool gameplayRadio = false;
};

static CaptionSelection SelectCaption(uintptr_t caller, unsigned request, const char* archive,
    unsigned zeroBasedTrack, const SceneContext& context) {
    if (caller == 0x55a6da && archive && !_stricmp(archive, "XA40_18.DAT") &&
        IsBattleRadioTrack(zeroBasedTrack + 1) && request == (0x8000u | zeroBasedTrack) &&
        context.messageFlags[4] == 0x300c3 && (context.type != 0x5a || (context.flags == 5 && !context.script)) &&
        EvaluateBattleRadio(context, zeroBasedTrack + 1) == CaptionGate::Allowed) {
        for (const auto& track : battleRadioTracks) {
            if (track.number == zeroBasedTrack + 1) return {&track, nullptr, nullptr, nullptr, true};
        }
    }
    if (caller == 0x55a6da && archive && !_stricmp(archive, "XA2D_18.DAT") &&
        IsBossRadioTrack(zeroBasedTrack + 1) && request == (0x8000u | zeroBasedTrack) &&
        context.messageFlags[4] == 0x300c3 && EvaluateBossRadio(context) == CaptionGate::Allowed) {
        for (const auto& track : bossRadioTracks) {
            if (track.number == zeroBasedTrack + 1) return {&track, nullptr, nullptr, nullptr, true};
        }
    }
    if (!rollBathTracks.empty() && caller == 0x53d35c && request == 0xff03 && zeroBasedTrack == 3 &&
        archive && !_stricmp(archive, "XACOM_18.DAT") && context.flags == 5 && context.script == rollBathScriptStart) {
        const auto& track = rollBathTracks.front();
        for (const auto& script : captionScripts) {
            if (EvaluateContext(context, &script, nullptr, &track) != CaptionGate::Allowed) continue;
            for (const auto& voice : script.voices) {
                if (voice.caller == caller && voice.callback == 0x53d320) return {&track, &script, &voice};
            }
        }
    }
    if (caller == 0x55a6da && archive && !_stricmp(archive, "XA26_18.DAT") &&
        IsGameplayRadioTrack(zeroBasedTrack + 1) &&
        request == (0x8000u | zeroBasedTrack) &&
        context.messageFlags[GameplayRadioMessageChannel(zeroBasedTrack + 1)] == 0x300c3 &&
        EvaluateGameplayRadio(context, zeroBasedTrack + 1) == CaptionGate::Allowed) {
        for (const auto& track : gameplayRadioTracks) {
            if (track.number == zeroBasedTrack + 1) return {&track, nullptr, nullptr, nullptr, true};
        }
    }
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
            for (const auto& continuation : scene.continuations) {
                if (EvaluateContext(context, continuation.script) != CaptionGate::Allowed) continue;
                for (const auto& voice : continuation.script.voices) {
                    if (voice.caller == caller) return {&track, &continuation.script, &voice, &scene};
                }
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

static bool ParseSubtitleTrack(nlohmann::json& document, unsigned number, SubtitleTrack& track,
    SubtitleTrackKind kind = SubtitleTrackKind::Cinematic) {
    if (!document.is_object() || document["version"] != 1 ||
        !document["archive"].is_string() || document["track"] != number ||
        document["language"] != "zh-Hant" ||
        !document["duration_ms"].is_number_unsigned() || document["duration_ms"] > 600000 ||
        !document["cues"].is_array() || document["cues"].empty() || document["cues"].size() > 128) return false;
    std::string archive = document["archive"].get<std::string>();
    int archiveIndex = SubtitleArchiveIndex(archive.c_str());
    if (kind == SubtitleTrackKind::GameplayRadio) {
        if (archive != "XA26_18.DAT" || !IsGameplayRadioTrack(number) ||
            document["duration_ms"] > 15000 || document["cues"].size() > 16) return false;
    } else if (kind == SubtitleTrackKind::BossRadio) {
        const size_t expectedCues = number == 33 || number == 34 ? 2 : 1;
        if (archive != "XA2D_18.DAT" || !IsBossRadioTrack(number) || document["duration_ms"] == 0 ||
            document["duration_ms"] > 15000 || document["cues"].size() != expectedCues) return false;
    } else if (kind == SubtitleTrackKind::BattleRadio) {
        if (archive != "XA40_18.DAT" || !IsBattleRadioTrack(number) || document["duration_ms"] == 0 ||
            document["duration_ms"] > 5000 || document["cues"].size() != 1) return false;
    } else if (kind == SubtitleTrackKind::RollBath) {
        if (archive != "XACOM_18.DAT" || number != 4 || document["duration_ms"] == 0 ||
            document["duration_ms"] > 5000 || document["cues"].size() != 1) return false;
    } else if (kind == SubtitleTrackKind::EndingSong) {
        if (archive != "STAFF.DAT" || number != 1 || document["duration_ms"] != 218888 ||
            document["cues"].size() != 16) return false;
    } else if (archiveIndex < 0 || archive != subtitleArchives[archiveIndex] ||
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

template<size_t Size>
static bool ParseSignature(const nlohmann::json& value, std::array<unsigned char, Size>& bytes) {
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
    if (voice.caller < 0x512cb0 || voice.caller >= cinematicCallbackEnd || voice.callback < 0x512ca0 ||
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
            if (!Unsigned32(word) || word < 0x512ca0 || word >= cinematicCallbackEnd) return false;
            voice.dispatchWords[index] = word.get<uint32_t>();
            member = member || voice.dispatchWords[index] == voice.callback;
        }
        if (!member) return false;
    }
    return true;
}

static bool ParseContinuation(nlohmann::json& source, CaptionContinuation& continuation) {
    if (!source.is_object() || !Unsigned32(source["start"]) || !Unsigned32(source["end"]) ||
        !Unsigned32(source["registration"]) || !Unsigned32(source["callback"]) ||
        !Unsigned32(source["script_push"]) || !Unsigned32(source["context_push"]) || !source["words"].is_array() ||
        !ParseSignature(source["registration_bytes"], continuation.registrationBytes) ||
        !ParseSignature(source["callback_bytes"], continuation.callbackBytes) ||
        !ParseSignature(source["script_push_bytes"], continuation.scriptPushBytes) ||
        !ParseSignature(source["context_push_bytes"], continuation.contextPushBytes)) return false;
    auto& script = continuation.script;
    script.start = source["start"].get<uint32_t>();
    script.end = source["end"].get<uint32_t>();
    continuation.registration = source["registration"].get<uint32_t>();
    continuation.callback = source["callback"].get<uint32_t>();
    continuation.scriptPush = source["script_push"].get<uint32_t>();
    continuation.contextPush = source["context_push"].get<uint32_t>();
    if (script.start < 0x89b000 || script.start % 4 || script.end >= 0x8b0000 || script.end <= script.start ||
        script.end - script.start > 8192 || (script.end - script.start) % 8 ||
        source["words"].size() != (script.end - script.start) / 4 ||
        continuation.callback < 0x512ca0 || continuation.callback >= cinematicCallbackEnd ||
        continuation.registration <= continuation.callback || continuation.registration >= cinematicCallbackEnd ||
        continuation.registration - continuation.callback > 0x4000 ||
        continuation.scriptPush < continuation.callback || continuation.scriptPush > continuation.registration - 15 ||
        continuation.contextPush < continuation.scriptPush + 5 || continuation.contextPush > continuation.registration - 10) return false;
    for (const auto& word : source["words"]) {
        if (!Unsigned32(word)) return false;
        script.words.push_back(word.get<uint32_t>());
    }
    for (size_t index = 1; index < script.words.size(); index += 2) {
        if (script.words[index] && (script.words[index] < 0x400000 || script.words[index] >= 0x830000)) return false;
    }
    if (source.contains("voices")) {
        if (!source["voices"].is_array() || source["voices"].empty() || source["voices"].size() > 128) return false;
        for (auto& entry : source["voices"]) {
            VoiceBinding voice;
            if (!ParseVoiceBinding(entry, voice) || voice.dispatch) return false;
            bool member = false;
            for (size_t index = 1; index < script.words.size(); index += 2) member = member || script.words[index] == voice.callback;
            if (!member) return false;
            for (const auto& existing : script.voices) if (existing.caller == voice.caller) return false;
            script.voices.push_back(voice);
        }
    }
    if (continuation.scriptPushBytes[0] != 0x68 || continuation.contextPushBytes[0] != 0x68 ||
        continuation.registrationBytes[0] != 0xe8) return false;
    uint32_t target = 0, context = 0, displacement = 0;
    std::memcpy(&target, continuation.scriptPushBytes.data() + 1, sizeof(target));
    std::memcpy(&context, continuation.contextPushBytes.data() + 1, sizeof(context));
    std::memcpy(&displacement, continuation.registrationBytes.data() + 1, sizeof(displacement));
    return target == script.start && context >= 0x89b000 && context < 0x8b0000 &&
        continuation.registration + displacement == 0x511630;
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
    std::vector<SubtitleTrack> radioTracks;
    std::vector<SubtitleTrack> bossTracks;
    BossRadioBinding bossBinding;
    std::vector<SubtitleTrack> battleTracks;
    std::array<BossRadioBinding, 2> battleBindings;
    std::vector<SubtitleTrack> bathTracks;
    std::vector<SubtitleTrack> endingTracks;
    EndingSongBinding endingBinding;
    std::array<unsigned char, 16> radioCallBytes = {};
    std::array<unsigned char, 32> radioIdleBytes = {};
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
            !Unsigned32(entry["wrapper"]) || entry["wrapper"] < 0x512b80 || entry["wrapper"] >= cinematicCallbackEnd ||
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
        if (entry.contains("continuations")) {
            if (!entry["continuations"].is_array() || entry["continuations"].empty() ||
                entry["continuations"].size() > 16) return false;
            for (auto& source : entry["continuations"]) {
                CaptionContinuation continuation;
                if (!ParseContinuation(source, continuation)) return false;
                bool owned = false;
                for (const auto& voice : scene.voices) {
                    for (uint32_t callback : voice.dispatchWords) owned = owned || callback == continuation.callback;
                }
                if (!owned) return false;
                for (const auto& existing : scene.continuations) {
                    if (continuation.script.start < existing.script.end &&
                        existing.script.start < continuation.script.end) return false;
                }
                scene.continuations.push_back(std::move(continuation));
            }
        }
        scenes.push_back(std::move(scene));
    }
    if (catalog.contains("gameplay_radio")) {
        auto& radio = catalog["gameplay_radio"];
        if (!radio.is_object() || radio["version"] != 1 || radio["scope"] != "xa26-lava-radio-v1" ||
            radio["caller"] != 0x55a6da || !ParseSignature(radio["call_bytes"], radioCallBytes) ||
            radioCallBytes[11] != 0xe8 || !radio["tracks"].is_array() ||
            (radio["tracks"].size() != 3 && radio["tracks"].size() != 5 && radio["tracks"].size() != 7 &&
                radio["tracks"].size() != gameplayRadioTrackNumbers.size())) return false;
        if (radio["tracks"].size() >= 7) {
            if (!ParseSignature(radio["idle_script_bytes"], radioIdleBytes)) return false;
            for (size_t index = 24; index < radioIdleBytes.size(); ++index) {
                if (radioIdleBytes[index] != (index == 24 ? 255 : 0)) return false;
            }
        } else if (radio.contains("idle_script_bytes")) return false;
        uint32_t displacement = 0;
        std::memcpy(&displacement, radioCallBytes.data() + 12, sizeof(displacement));
        if (0x55a6da + displacement != 0x600ec0) return false;
        for (size_t index = 0; index < radio["tracks"].size(); ++index) {
            SubtitleTrack track;
            if (!ParseSubtitleTrack(radio["tracks"][index], gameplayRadioTrackNumbers[index], track,
                SubtitleTrackKind::GameplayRadio)) return false;
            radioTracks.push_back(std::move(track));
        }
    }
    if (catalog.contains("boss_radio")) {
        auto& radio = catalog["boss_radio"];
        if (!radio.is_object() || radio["version"] != 1 || radio["scope"] != "xa2d-boss-radio-v1" ||
            radio["caller"] != 0x55a6da || radio["descriptor"] != 0x8f2064 || radio["message_channel"] != 4 ||
            !ParseSignature(radio["call_bytes"], bossBinding.callBytes) || bossBinding.callBytes[11] != 0xe8 ||
            !radio["tracks"].is_array() ||
            (radio["tracks"].size() != 4 && radio["tracks"].size() != bossRadioTrackNumbers.size())) return false;
        uint32_t displacement = 0;
        std::memcpy(&displacement, bossBinding.callBytes.data() + 12, sizeof(displacement));
        if (0x55a6da + displacement != 0x600ec0) return false;
        auto& controller = radio["controller"];
        if (!controller.is_object() || controller["type"] != 0x4a || controller["wrapper"] != 0x5301d0 ||
            controller["dispatch"] != 0x8a86c0 || !ParseSignature(controller["wrapper_bytes"], bossBinding.wrapperBytes) ||
            !controller["dispatch_words"].is_array() || controller["dispatch_words"].size() != 3 ||
            !ParseContinuation(controller["continuation"], bossBinding.continuation)) return false;
        for (size_t index = 0; index < bossBinding.dispatchWords.size(); ++index) {
            const auto& word = controller["dispatch_words"][index];
            if (!Unsigned32(word) || word < 0x512ca0 || word >= 0x53ebc0) return false;
            bossBinding.dispatchWords[index] = word.get<uint32_t>();
        }
        const auto& continuation = bossBinding.continuation;
        if (bossBinding.dispatchWords[0] != 0x5301f0 || continuation.callback != 0x5301f0 ||
            continuation.registration != 0x530213 || continuation.scriptPush != 0x530204 ||
            continuation.contextPush != 0x530209 || continuation.script.start != bossRadioScriptStart ||
            continuation.script.end != bossRadioScriptEnd || !continuation.script.voices.empty() ||
            continuation.script.words != std::vector<uint32_t>{0x1c20000, 0x530300}) return false;
        for (size_t index = 0; index < radio["tracks"].size(); ++index) {
            SubtitleTrack track;
            if (!ParseSubtitleTrack(radio["tracks"][index], bossRadioTrackNumbers[index], track,
                SubtitleTrackKind::BossRadio)) return false;
            bossTracks.push_back(std::move(track));
        }
    }
    if (catalog.contains("battle_radio")) {
        auto& radio = catalog["battle_radio"];
        std::array<unsigned char, 16> callBytes = {};
        if (!radio.is_object() || radio["version"] != 1 || radio["scope"] != "xa40-battle-radio-v1" ||
            radio["caller"] != 0x55a6da || radio["descriptor"] != 0x90924c || radio["message_channel"] != 4 ||
            !ParseSignature(radio["call_bytes"], callBytes) || callBytes[11] != 0xe8 ||
            !radio["tracks"].is_array() || (radio["tracks"].size() != 3 && radio["tracks"].size() != battleRadioTrackNumbers.size()) ||
            !radio["controllers"].is_array() || radio["controllers"].size() != battleRadioControllers.size() ||
            !radio["provisional_tracks"].is_array() || radio["provisional_tracks"].size() != (radio["tracks"].size() == 3 ? 1 : 2) ||
            radio["provisional_tracks"][0] != 29 ||
            (radio["tracks"].size() != 3 && radio["provisional_tracks"][1] != 28)) return false;
        uint32_t displacement = 0;
        std::memcpy(&displacement, callBytes.data() + 12, sizeof(displacement));
        if (0x55a6da + displacement != 0x600ec0) return false;
        for (size_t index = 0; index < battleRadioControllers.size(); ++index) {
            const auto& expected = battleRadioControllers[index];
            auto& controller = radio["controllers"][index];
            auto& binding = battleBindings[index];
            binding.callBytes = callBytes;
            if (!controller.is_object() || controller["type"] != expected.type || controller["wrapper"] != expected.wrapper ||
                controller["dispatch"] != expected.dispatch || !ParseSignature(controller["wrapper_bytes"], binding.wrapperBytes) ||
                !controller["dispatch_words"].is_array() || controller["dispatch_words"].size() != 3 ||
                !ParseContinuation(controller["continuation"], binding.continuation)) return false;
            for (size_t word = 0; word < binding.dispatchWords.size(); ++word) {
                if (controller["dispatch_words"][word] != expected.dispatchWords[word]) return false;
            }
            binding.dispatchWords = expected.dispatchWords;
            const auto& continuation = binding.continuation;
            if (continuation.callback != expected.dispatchWords[0] || continuation.registration != expected.registration ||
                continuation.scriptPush != expected.scriptPush || continuation.contextPush != expected.contextPush ||
                continuation.script.start != expected.scriptStart || continuation.script.end != expected.scriptEnd ||
                !continuation.script.voices.empty()) return false;
        }
        for (size_t index = 0; index < radio["tracks"].size(); ++index) {
            SubtitleTrack track;
            if (!ParseSubtitleTrack(radio["tracks"][index], battleRadioTrackNumbers[index], track,
                SubtitleTrackKind::BattleRadio)) return false;
            if ((track.number == 29 || track.number == 28) &&
                radio["tracks"][index]["translation_status"] != "user-approved-provisional") return false;
            battleTracks.push_back(std::move(track));
        }
    }
    if (catalog.contains("roll_bath")) {
        auto& event = catalog["roll_bath"];
        if (!event.is_object() || event["version"] != 1 || event["scope"] != "roll-bath-v1" ||
            event["caller"] != 0x53d35c || event["request"] != 0xff03 || event["descriptor"] != 0x8d4acc ||
            event["controller_type"] != 0x70 || event["script_start"] != rollBathScriptStart ||
            event["script_end"] != rollBathScriptEnd) return false;
        SubtitleTrack track;
        if (!ParseSubtitleTrack(event["track"], 4, track, SubtitleTrackKind::RollBath)) return false;
        bool bound = false;
        for (const auto& script : scripts) {
            if (script.start != rollBathScriptStart || script.end != rollBathScriptEnd) continue;
            for (const auto& voice : script.voices) {
                const std::array<unsigned char, 5> request = {0x68, 0x03, 0xff, 0x00, 0x00};
                if (voice.caller == 0x53d35c && voice.callback == 0x53d320 &&
                    std::equal(request.begin(), request.end(), voice.callBytes.begin() + 6)) bound = true;
            }
        }
        if (!bound) return false;
        bathTracks.push_back(std::move(track));
    }
    if (catalog.contains("ending_song")) {
        auto& song = catalog["ending_song"];
        auto& binding = endingBinding.controller;
        if (!song.is_object() || song["version"] != 1 || song["scope"] != "staff-ending-lyrics-v1" ||
            song["caller"] != 0x40dcdf || song["play_function"] != 0x40ae40 || song["credits_caller"] != 0x53c074 ||
            song["resource_bytes"] != endingSongBytes || !ParseSignature(song["call_bytes"], binding.callBytes) ||
            !ParseSignature(song["play_bytes"], endingBinding.playBytes) ||
            !ParseSignature(song["credits_call_bytes"], endingBinding.creditsCallBytes) ||
            !ParseSignature(song["credits_callback_bytes"], endingBinding.creditsCallbackBytes) ||
            !song["audio_probes"].is_array() || song["audio_probes"].size() != 3) return false;
        uint32_t playDisplacement = 0, creditsDisplacement = 0;
        std::memcpy(&playDisplacement, binding.callBytes.data() + 12, sizeof(playDisplacement));
        std::memcpy(&creditsDisplacement, endingBinding.creditsCallBytes.data() + 12, sizeof(creditsDisplacement));
        if (binding.callBytes[11] != 0xe8 || 0x40dcdf + playDisplacement != 0x40ae40 ||
            endingBinding.creditsCallBytes[11] != 0xe8 || 0x53c074 + creditsDisplacement != 0x40dc90) return false;
        for (size_t index = 0; index < endingSongProbeOffsets.size(); ++index) {
            const auto& probe = song["audio_probes"][index];
            if (probe["offset"] != endingSongProbeOffsets[index] || !ParseSignature(probe["bytes"], endingBinding.audioProbes[index])) return false;
        }
        auto& controller = song["controller"];
        if (!controller.is_object() || controller["type"] != 0x6e || controller["wrapper"] != 0x53bd40 ||
            controller["dispatch"] != 0x8acc90 || !ParseSignature(controller["wrapper_bytes"], binding.wrapperBytes) ||
            !controller["dispatch_words"].is_array() || controller["dispatch_words"].size() != 3 ||
            !ParseContinuation(controller["continuation"], binding.continuation)) return false;
        const std::array<uint32_t, 3> expectedDispatch = {0x53bd60, 0x53bea0, 0x53bf20};
        for (size_t index = 0; index < expectedDispatch.size(); ++index) {
            if (controller["dispatch_words"][index] != expectedDispatch[index]) return false;
        }
        binding.dispatchWords = expectedDispatch;
        const auto& continuation = binding.continuation;
        if (continuation.callback != 0x53bd60 || continuation.registration != 0x53bdee ||
            continuation.scriptPush != 0x53bddf || continuation.contextPush != 0x53bde4 ||
            continuation.script.start != 0x8acc80 || continuation.script.end != 0x8acc88 ||
            continuation.script.words != std::vector<uint32_t>{0xffff0000, 0x53c020} ||
            !continuation.script.voices.empty()) return false;
        SubtitleTrack track;
        if (!ParseSubtitleTrack(song["track"], 1, track, SubtitleTrackKind::EndingSong)) return false;
        endingTracks.push_back(std::move(track));
    }
    subtitleTracks = std::move(tracks);
    gameplayRadioTracks = std::move(radioTracks);
    bossRadioTracks = std::move(bossTracks);
    bossRadioBinding = std::move(bossBinding);
    battleRadioTracks = std::move(battleTracks);
    battleRadioBindings = std::move(battleBindings);
    rollBathTracks = std::move(bathTracks);
    endingSongTracks = std::move(endingTracks);
    endingSongBinding = std::move(endingBinding);
    gameplayRadioCallBytes = radioCallBytes;
    gameplayRadioIdleBytes = radioIdleBytes;
    captionScripts = std::move(scripts);
    captionScenes = std::move(scenes);
    size_t cues = 0, voices = 0, continuations = 0, continuationVoices = 0;
    for (const auto& track : subtitleTracks) cues += track.cues.size();
    for (const auto& track : gameplayRadioTracks) cues += track.cues.size();
    for (const auto& track : bossRadioTracks) cues += track.cues.size();
    for (const auto& track : battleRadioTracks) cues += track.cues.size();
    for (const auto& track : rollBathTracks) cues += track.cues.size();
    for (const auto& track : endingSongTracks) cues += track.cues.size();
    for (const auto& script : captionScripts) voices += script.voices.size();
    for (const auto& scene : captionScenes) {
        voices += scene.voices.size();
        continuations += scene.continuations.size();
        for (const auto& continuation : scene.continuations) continuationVoices += continuation.script.voices.size();
    }
    Log("Loaded subtitle catalog tracks=" + std::to_string(subtitleTracks.size() + gameplayRadioTracks.size() +
        bossRadioTracks.size() + battleRadioTracks.size() + rollBathTracks.size() + endingSongTracks.size()) + " cues=" + std::to_string(cues) +
        " scripts=" + std::to_string(captionScripts.size()) + " direct_scenes=" + std::to_string(captionScenes.size()) +
        " voice_bindings=" + std::to_string(voices) + " continuations=" + std::to_string(continuations) +
        " continuation_voices=" + std::to_string(continuationVoices) +
        " scoped_gameplay_radio_tracks=" + std::to_string(gameplayRadioTracks.size()) +
        " scoped_roll_bath_tracks=" + std::to_string(rollBathTracks.size()) +
        " scoped_xa2d_boss_tracks=" + std::to_string(bossRadioTracks.size()) +
        " scoped_xa40_battle_tracks=" + std::to_string(battleRadioTracks.size()) +
        " ending_song_tracks=" + std::to_string(endingSongTracks.size()));
    return true;
}

static bool VerifyScriptTable(const CaptionScript& script) {
    std::vector<uint32_t> actual(script.words.size());
    SIZE_T returned = 0;
    uint32_t terminator[2] = {};
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(script.start), actual.data(),
        actual.size() * sizeof(uint32_t), &returned) && returned == actual.size() * sizeof(uint32_t) &&
        actual == script.words && ReadGame(script.end, terminator) && terminator[0] == 255 && terminator[1] == 0;
}

static bool VerifyContinuationBinding(const CaptionContinuation& continuation) {
    std::array<unsigned char, 5> registration = {}, scriptPush = {}, contextPush = {};
    std::array<unsigned char, 16> callback = {};
    return ReadGame(continuation.registration - registration.size(), registration) && registration == continuation.registrationBytes &&
        ReadGame(continuation.callback, callback) && callback == continuation.callbackBytes &&
        ReadGame(continuation.scriptPush, scriptPush) && scriptPush == continuation.scriptPushBytes &&
        ReadGame(continuation.contextPush, contextPush) && contextPush == continuation.contextPushBytes &&
        VerifyScriptTable(continuation.script);
}

static bool VerifyEndingSongBinding(unsigned slot) {
    uint32_t source = 0, length = 0, handle = 0, wrapper = 0;
    std::array<unsigned char, 16> actual = {};
    std::array<uint32_t, 3> dispatch = {};
    const auto& binding = endingSongBinding.controller;
    if (slot < 1 || slot >= 8 || !ReadGame(0xa6b900, source) || source < 0x10000 ||
        !ReadGame(0xa6b904, length) || length != endingSongBytes || source > 0xffffffffu - length ||
        !ReadGame(0xa6b90c, handle) || handle != slot ||
        (!originalPlayStream && (!ReadGame(0x40ae40, actual) || actual != endingSongBinding.playBytes)) ||
        !ReadGame(0x40dccf, actual) || actual != binding.callBytes ||
        !ReadGame(0x53c064, actual) || actual != endingSongBinding.creditsCallBytes ||
        !ReadGame(0x53c050, actual) || actual != endingSongBinding.creditsCallbackBytes ||
        !ReadGame(0x89a6d0 + 0x6e * 4, wrapper) || wrapper != 0x53bd40 ||
        !ReadGame(wrapper, actual) || actual != binding.wrapperBytes ||
        !ReadGame(0x8acc90, dispatch) || dispatch != binding.dispatchWords ||
        !VerifyContinuationBinding(binding.continuation)) return false;
    for (size_t index = 0; index < endingSongProbeOffsets.size(); ++index) {
        std::array<unsigned char, 64> probe = {};
        if (!ReadGame(source + endingSongProbeOffsets[index], probe) || probe != endingSongBinding.audioProbes[index]) return false;
    }
    return true;
}

static CaptionSelection SelectEndingSong(uintptr_t caller, unsigned slot, const SceneContext& context) {
    if (endingSongTracks.empty() || caller != 0x40dcdf || EvaluateEndingSong(context) != CaptionGate::Allowed ||
        !VerifyEndingSongBinding(slot)) return {};
    return {&endingSongTracks.front()};
}

static bool VerifySceneBinding(const CaptionSelection& selection) {
    if (selection.track && IsEndingSongTrack(*selection.track)) {
        uint32_t slot = 0;
        return ReadGame(0xa6b90c, slot) && VerifyEndingSongBinding(slot);
    }
    if (selection.gameplayRadio) {
        std::array<unsigned char, 16> actual = {};
        if (!selection.track || !ReadGame(0x55a6ca, actual)) return false;
        if (selection.track->archive == "XA40_18.DAT") {
            if (!IsBattleRadioTrack(selection.track->number)) return false;
            const size_t count = selection.track->number == 25 ? 2 : 1;
            for (size_t index = 0; index < count; ++index) {
                const auto& expected = battleRadioControllers[index];
                const auto& binding = battleRadioBindings[index];
                uint32_t wrapper = 0;
                std::array<unsigned char, 16> wrapperBytes = {};
                std::array<uint32_t, 3> dispatch = {};
                if (actual != binding.callBytes || !ReadGame(0x89a6d0 + expected.type * 4, wrapper) || wrapper != expected.wrapper ||
                    !ReadGame(wrapper, wrapperBytes) || wrapperBytes != binding.wrapperBytes ||
                    !ReadGame(expected.dispatch, dispatch) || dispatch != binding.dispatchWords ||
                    !VerifyContinuationBinding(binding.continuation)) return false;
            }
            return true;
        }
        if (selection.track->archive == "XA2D_18.DAT") {
            uint32_t wrapper = 0;
            std::array<unsigned char, 16> wrapperBytes = {};
            std::array<uint32_t, 3> dispatch = {};
            return IsBossRadioTrack(selection.track->number) && actual == bossRadioBinding.callBytes &&
                ReadGame(0x89a6d0 + 0x4a * 4, wrapper) && wrapper == 0x5301d0 &&
                ReadGame(wrapper, wrapperBytes) && wrapperBytes == bossRadioBinding.wrapperBytes &&
                ReadGame(0x8a86c0, dispatch) && dispatch == bossRadioBinding.dispatchWords &&
                VerifyContinuationBinding(bossRadioBinding.continuation);
        }
        if (selection.track->archive != "XA26_18.DAT" || !IsGameplayRadioTrack(selection.track->number) ||
            actual != gameplayRadioCallBytes) return false;
        if (IsLateGameplayRadioTrack(selection.track->number)) {
            std::array<unsigned char, 32> idle = {};
            if (!ReadGame(gameplayRadioIdleScriptStart, idle) || idle != gameplayRadioIdleBytes) return false;
        }
        const bool chase = selection.track->number == 31;
        const uint8_t type = chase ? 0x47 : 0x2d;
        const uint32_t scriptStart = chase ? 0x8a8418 : gameplayRadioScriptStart;
        const uint32_t scriptEnd = chase ? 0x8a8448 : gameplayRadioScriptEnd;
        for (const auto& scene : captionScenes) {
            if (scene.type == type && !scene.voices.empty()) {
                for (const auto& continuation : scene.continuations) {
                    if (continuation.script.start == scriptStart && continuation.script.end == scriptEnd) {
                        return VerifySceneBinding({selection.track, nullptr, &scene.voices.front(), &scene});
                    }
                }
            }
        }
        return false;
    }
    std::array<unsigned char, 16> callBytes = {}, callbackBytes = {};
    std::array<uint32_t, 3> dispatchWords = {};
    if (selection.voice->dispatch && (!ReadGame(selection.voice->dispatch, dispatchWords) ||
        dispatchWords != selection.voice->dispatchWords)) return false;
    if (!ReadGame(selection.voice->caller - 16, callBytes) || callBytes != selection.voice->callBytes ||
        !ReadGame(selection.voice->callback, callbackBytes) || callbackBytes != selection.voice->callbackBytes) return false;
    if (selection.script && !VerifyScriptTable(*selection.script)) return false;
    if (!selection.scene) return selection.script != nullptr;
    uint32_t wrapper = 0;
    std::array<unsigned char, 16> wrapperBytes = {};
    if (!ReadGame(0x89a6d0 + selection.scene->type * 4, wrapper) || wrapper != selection.scene->wrapper ||
        !ReadGame(wrapper, wrapperBytes) || wrapperBytes != selection.scene->wrapperBytes) return false;
    for (const auto& voice : selection.scene->voices) {
        if (!ReadGame(voice.dispatch, dispatchWords) || dispatchWords != voice.dispatchWords) return false;
    }
    for (const auto& continuation : selection.scene->continuations) {
        if (!VerifyContinuationBinding(continuation)) return false;
    }
    return true;
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
    activeGameplayRadio = false;
}

static void LogSceneState(const SceneContext& context) {
    std::ostringstream details;
    details << "Caption context readable=" << context.readable << " script=0x" << std::hex <<
        context.script << " scene=0x" << context.descriptor << " flags=0x" << static_cast<unsigned>(context.flags) <<
        " type=" << static_cast<unsigned>(context.type) << " messages=";
    for (uint32_t value : context.messageFlags) details << value << ":";
    Log(details.str());
}

static void LogSceneState() {
    LogSceneState(ReadSceneContext());
}

static void AcquireCaptionClock(const CaptionSelection& selection, int slot, const std::string& description) {
    IDirectSoundBuffer* buffer = nullptr;
    if (!selection.track || slot < 0 || slot >= 8 || !ReadGame(0xa2ae10 + slot * 24, buffer) || !buffer) return;
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
    activeGameplayRadio = selection.gameplayRadio;
    voiceSlot = slot;
    voiceStartedAt = GetTickCount64();
    lastVoiceSample = 0;
    voiceClock.Reset(caps.dwBufferBytes, format.nAvgBytesPerSec, cursor, voiceStartedAt);
    Log(description + " slot=" + std::to_string(slot) + " bufferBytes=" + std::to_string(caps.dwBufferBytes) +
        " bytesPerSecond=" + std::to_string(format.nAvgBytesPerSec) +
        " scoped_gameplay_radio=" + std::to_string(selection.gameplayRadio) +
        " scoped_roll_bath=" + std::to_string(IsRollBathTrack(*selection.track)) +
        " ending_song=" + std::to_string(IsEndingSongTrack(*selection.track)));
}

static uintptr_t __cdecl PlayStream(unsigned slot) {
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const SceneContext before = ReadSceneContext();
    const CaptionSelection selection = SelectEndingSong(caller, slot, before);
    const bool creditsCall = caller == 0x40dcdf && before.type == 0x6e;
    if (creditsCall) {
        Log("Credits music event slot=" + std::to_string(slot) + " verified=" + std::to_string(selection.track != nullptr));
        LogSceneState(before);
    }
    const uintptr_t result = originalPlayStream(slot);
    const SceneContext after = creditsCall ? ReadSceneContext() : SceneContext{};
    if (!selection.track || EvaluateEndingSong(after) != CaptionGate::Allowed) {
        if (creditsCall) {
            Log("Credits captions excluded: source identity, native binding or scene state not eligible.");
            LogSceneState(after);
        }
        return result;
    }
    {
        std::lock_guard<std::mutex> guard(voiceMutex);
        ClearVoice("ending song started");
    }
    AcquireCaptionClock(selection, static_cast<int>(slot), "Ending song accepted: verified Staff resource 0");
    return result;
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
    if (!eligible || EvaluateContext(ReadSceneContext(), selection.script, selection.scene,
        selection.track) != CaptionGate::Allowed) {
        Log("Voice excluded from extra subtitles: audio, callback, script signature, or native message state not eligible.");
        return;
    }
    int slot = -1;
    if (ReadGame(0x9193f4, slot)) AcquireCaptionClock(selection, slot, "Subtitle voice request=" +
        std::to_string(request) + " archive=" + archive + " track=" + std::to_string(identifiers[1] + 1));
}

static VOID CALLBACK SampleVoiceClock(PVOID, BOOLEAN) {
    std::lock_guard<std::mutex> guard(voiceMutex);
    if (!observedVoice || !activeTrack || (!activeScript && !activeScene && !activeGameplayRadio && !IsEndingSongTrack(*activeTrack))) return;
    const SceneContext sceneContext = ReadSceneContext();
    CaptionGate gate = EvaluateContext(sceneContext, activeScript, activeScene, activeTrack);
    if (gate == CaptionGate::Unknown || gate == CaptionGate::OutsideScript) {
        LogSceneState(sceneContext);
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
        if (activeGameplayRadio || IsRollBathTrack(*activeTrack) || IsEndingSongTrack(*activeTrack)) LogSceneState(sceneContext);
    }
    if (wasRunning != playing) Log(playing ? "Subtitle audio resumed." : "Subtitle audio paused.");
    if (now - lastVoiceSample >= 10000) {
        Log("Subtitle clock track=" + std::to_string(DiagnosticTrackNumber(*activeTrack)) + " media_ms=" + std::to_string(milliseconds) + " wall_ms=" +
            std::to_string(now - voiceStartedAt) + " cursor=" + std::to_string(cursor));
        LogSceneState(sceneContext);
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
    if (!originalPlayStream && !endingSongTracks.empty() && reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) == 0x400000) {
        std::array<unsigned char, 16> actual = {};
        if (ReadGame(0x40ae40, actual) && actual == endingSongBinding.playBytes) {
            originalPlayStream = reinterpret_cast<PlayStreamFunction>(Install(
                reinterpret_cast<void*>(0x40ae40), reinterpret_cast<void*>(&PlayStream)));
            if (originalPlayStream) Log("Verified credits music-start hook installed; other audio calls remain uncaptioned.");
        }
    }
}

static int CaptureSlot(unsigned track, int cue) {
    if (track > 1) return track <= maximumDiagnosticTrack && cue == 0 ? static_cast<int>(track + 2) : -1;
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
        if (!activeTrack || EvaluateContext(ReadSceneContext(), activeScript, activeScene,
            activeTrack) != CaptionGate::Allowed) return false;
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
    size_t continuationCases = 0;
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
            for (const auto& continuation : scene.continuations) {
                SceneContext scripted = context;
                scripted.flags = 5;
                for (uint32_t position = continuation.script.start; position < continuation.script.end; position += 8) {
                    scripted.script = position;
                    for (const auto& track : subtitleTracks) {
                        CaptionSelection selected = SelectCaption(voice.caller, 0, track.archive.c_str(), track.number - 1, scripted);
                        bool sameScript = selected.script && selected.script->start == continuation.script.start &&
                            selected.script->end == continuation.script.end;
                        eligibility = eligibility && selected.track == &track &&
                            ((selected.scene == &scene && !selected.script) || sameScript);
                        ++continuationCases;
                    }
                }
                for (unsigned scenario = 0; scenario < 6; ++scenario) {
                    SceneContext rejected = scripted;
                    if (scenario == 0) rejected.script = continuation.script.end;
                    if (scenario == 1) rejected.script = continuation.script.start - 8;
                    if (scenario == 2) rejected.script = continuation.script.start + 1;
                    if (scenario == 3) rejected.type = static_cast<uint8_t>(scene.type + 1);
                    if (scenario == 4) rejected.flags = 0;
                    if (scenario == 5) rejected.readable = false;
                    eligibility = eligibility && EvaluateContext(rejected, nullptr, &scene) != CaptionGate::Allowed;
                }
                for (size_t channel = 0; channel < scripted.messageFlags.size(); ++channel) {
                    for (uint32_t flag : {0x80u, 0x8000u}) {
                        SceneContext dialog = scripted;
                        dialog.messageFlags[channel] = flag;
                        eligibility = eligibility && EvaluateContext(dialog, nullptr, &scene) == CaptionGate::NativeMessage &&
                            !SelectCaption(voice.caller, 0, "XA02_37.DAT", 0, dialog).track;
                    }
                }
                eligibility = eligibility && !SelectCaption(0x55a6da, 0, "XA02_37.DAT", 0, scripted).track &&
                    !SelectCaption(voice.caller, 0x8000, "XA02_37.DAT", 0, scripted).track &&
                    !SelectCaption(voice.caller, 0, "XA12_18.DAT", 0, scripted).track;
            }
        }
    }
    bool continuationVoicesPassed = true;
    size_t continuationVoiceCases = 0;
    for (const auto& scene : captionScenes) {
        for (const auto& continuation : scene.continuations) {
            const auto& script = continuation.script;
            SceneContext context;
            context.readable = true;
            context.flags = 5;
            context.type = scene.type;
            for (const auto& voice : script.voices) {
                for (uint32_t address = script.start; address < script.end; address += 8) {
                    context.script = address;
                    for (const auto& track : subtitleTracks) {
                        CaptionSelection selected = SelectCaption(voice.caller, 0, track.archive.c_str(), track.number - 1, context);
                        continuationVoicesPassed = continuationVoicesPassed && selected.track == &track && selected.script &&
                            selected.script->start == script.start && selected.script->end == script.end;
                        ++continuationVoiceCases;
                    }
                }
                for (size_t channel = 0; channel < context.messageFlags.size(); ++channel) {
                    SceneContext dialog = context;
                    dialog.messageFlags[channel] = 0x8080;
                    continuationVoicesPassed = continuationVoicesPassed &&
                        EvaluateContext(dialog, &script, &scene) == CaptionGate::NativeMessage &&
                        !SelectCaption(voice.caller, 0, "XA15_37.DAT", 8, dialog).track;
                }
                for (unsigned scenario = 0; scenario < 7; ++scenario) {
                    SceneContext rejected = context;
                    if (scenario == 0) rejected.script = 0;
                    if (scenario == 1) rejected.script = script.end;
                    if (scenario == 2) rejected.script = script.start + 1;
                    if (scenario == 3) rejected.script = script.start - 8;
                    if (scenario == 4) rejected.type = static_cast<uint8_t>(scene.type + 1);
                    if (scenario == 5) rejected.flags = 0;
                    if (scenario == 6) rejected.readable = false;
                    continuationVoicesPassed = continuationVoicesPassed &&
                        EvaluateContext(rejected, &script, &scene) != CaptionGate::Allowed;
                }
            }
        }
    }
    SceneContext airshipContext;
    airshipContext.readable = true;
    airshipContext.flags = 5;
    airshipContext.type = 0x39;
    airshipContext.script = 0x8a5de0;
    CaptionSelection airship = SelectCaption(0x52a490, 8, "XA15_37.DAT", 8, airshipContext);
    bool airshipPassed = airship.track && airship.script && airship.scene &&
        airship.script->start == 0x8a5dc8 && airship.scene->type == 0x39;
    size_t airshipCues = 0;
    if (airshipPassed) {
        PlaybackClock airshipClock;
        airshipClock.Reset(147456, 176400, 0, 1000);
        int previousCue = -1;
        for (uint64_t milliseconds = 10; milliseconds <= airship.track->cues.back().end; milliseconds += 10) {
            uint32_t cursor = static_cast<uint32_t>((milliseconds * 176400 / 1000) % 147456);
            airshipPassed = airshipPassed && EvaluateContext(airshipContext, airship.script, airship.scene) == CaptionGate::Allowed &&
                airshipClock.Sample(cursor, 1000 + milliseconds, true);
            int cue = FindCue(*airship.track, airshipClock.Milliseconds());
            if (cue >= 0 && cue != previousCue) ++airshipCues;
            previousCue = cue;
        }
        airshipPassed = airshipPassed && airshipCues == airship.track->cues.size();
        for (uint32_t script : {0u, 0x8a5df8u, 0x8a2320u, 0x8a5de1u}) {
            SceneContext rejected = airshipContext;
            rejected.script = script;
            airshipPassed = airshipPassed && !SelectCaption(0x52a490, 8, "XA15_37.DAT", 8, rejected).track;
        }
        airshipContext.type = 0x38;
        airshipPassed = airshipPassed && !SelectCaption(0x52a490, 8, "XA15_37.DAT", 8, airshipContext).track;
    }
    Log("Continuation-owned voice selections=" + std::to_string(continuationVoiceCases) + ": " +
        (continuationVoicesPassed ? "PASS" : "FAIL"));
    Log("Recorded rescue/airship callback replay cues=" + std::to_string(airshipCues) + ": " + (airshipPassed ? "PASS" : "FAIL"));
    bool radioPassed = true;
    size_t radioCues = 0, radioRejections = 0;
    for (const auto& track : gameplayRadioTracks) {
        SceneContext context;
        context.readable = true;
        const bool lateRadio = IsLateGameplayRadioTrack(track.number);
        const size_t radioChannel = GameplayRadioMessageChannel(track.number);
        const uint32_t start = lateRadio ? gameplayRadioIdleScriptEnd : gameplayRadioScriptStart;
        const uint32_t end = lateRadio ? gameplayRadioIdleScriptEnd : gameplayRadioScriptEnd;
        context.type = lateRadio ? 0 : 0x2d;
        context.descriptor = GameplayRadioDescriptor(track.number);
        context.messageFlags[radioChannel] = 0x300c3;
        const unsigned request = 0x8000u | (track.number - 1);
        for (uint32_t script = start; script <= end; script += 8) {
            context.script = script;
            CaptionSelection selected = SelectCaption(0x55a6da, request, track.archive.c_str(), track.number - 1, context);
            radioPassed = radioPassed && selected.track == &track && selected.gameplayRadio && !selected.voice &&
                EvaluateContext(context, selected.script, selected.scene, selected.gameplayRadio ? selected.track : nullptr) == CaptionGate::Allowed;
            for (uint32_t flags : {0x300c3u, 0x310c3u, 0x320c3u, 0x330c3u,
                0x30083u, 0x31083u, 0x32083u, 0x33083u}) {
                SceneContext playback = context;
                playback.messageFlags[radioChannel] = flags;
                radioPassed = radioPassed && EvaluateContext(playback, selected.script, selected.scene,
                    selected.gameplayRadio ? selected.track : nullptr) == CaptionGate::Allowed;
            }
        }
        for (unsigned scenario = 0; scenario < 23; ++scenario) {
            SceneContext rejected = context;
            uintptr_t caller = 0x55a6da;
            unsigned voiceRequest = request, audioTrack = track.number - 1;
            const char* archive = "XA26_18.DAT";
            if (scenario == 0) rejected.readable = false;
            if (scenario == 1) rejected.descriptor += 4;
            if (scenario == 2) rejected.type += 1;
            if (scenario == 3) rejected.flags = 1;
            if (scenario == 4) rejected.script = 0x88b960;
            if (scenario == 5) rejected.messageFlags[radioChannel] = 0x100c3;
            if (scenario == 6) rejected.messageFlags[radioChannel] = 0;
            if (scenario == 7) rejected.messageFlags[0] = 0x80;
            if (scenario == 8) rejected.messageFlags[3] = 0x8000;
            if (scenario == 9) caller += 1;
            if (scenario == 10) archive = "XA1F_18.DAT";
            if (scenario == 11) voiceRequest &= 0x7fff;
            if (scenario == 12) { audioTrack = 6; voiceRequest = 0x8006; }
            if (scenario == 13) rejected.script = start - 8;
            if (scenario == 14) rejected.script = end + 8;
            if (scenario == 15) rejected.script = start + 1;
            if (scenario == 16) rejected.messageFlags[radioChannel] = 0x130c3;
            if (scenario == 17) rejected.messageFlags[radioChannel] = 0x380c3;
            if (scenario == 18) { audioTrack = 10; voiceRequest = 0x800a; }
            if (scenario == 19) {
                rejected.type = lateRadio ? 0x2d : 0;
                rejected.script = lateRadio ? gameplayRadioScriptStart : gameplayRadioIdleScriptEnd;
            }
            if (scenario == 20) {
                rejected.messageFlags[radioChannel] = 0;
                rejected.messageFlags[radioChannel == 4 ? 2 : 4] = 0x300c3;
            }
            if (scenario == 21) { audioTrack = 31; voiceRequest = 0x801f; }
            if (scenario == 22) rejected.descriptor = track.number == 31 ? 0x8f4660 : 0x8ebee0;
            radioPassed = radioPassed && !SelectCaption(caller, voiceRequest, archive, audioTrack, rejected).track;
            if (scenario == 5 || scenario == 7 || scenario == 8 || scenario == 16 || scenario == 17 || scenario == 20) {
                radioPassed = radioPassed && EvaluateGameplayRadio(rejected, track.number) == CaptionGate::NativeMessage;
            }
            ++radioRejections;
        }
        for (const auto& cue : track.cues) {
            radioPassed = radioPassed && FindCue(track, cue.start) >= 0 &&
                EvaluateContext(context, nullptr, nullptr, &track) == CaptionGate::Allowed;
            ++radioCues;
        }
        for (size_t channel = 0; channel < context.messageFlags.size(); ++channel) {
            if (channel == radioChannel) continue;
            SceneContext dialog = context;
            dialog.messageFlags[channel] = 0x8080;
            radioPassed = radioPassed && EvaluateContext(dialog, nullptr, nullptr, &track) == CaptionGate::NativeMessage &&
                !SelectCaption(0x55a6da, request, track.archive.c_str(), track.number - 1, dialog).track;
        }
        context.descriptor = 0;
        radioPassed = radioPassed && EvaluateContext(context, nullptr, nullptr, &track) == CaptionGate::OutsideScript;
    }
    Log("Scoped gameplay radio cues=" + std::to_string(radioCues) + " rejected_cases=" + std::to_string(radioRejections) +
        ": " + (radioPassed ? "PASS" : "FAIL"));
    bool bossPassed = true;
    size_t bossRejections = 0, bossCues = 0;
    for (const auto& track : bossRadioTracks) {
        SceneContext context;
        context.readable = true;
        context.type = 0x4a;
        context.descriptor = 0x8f2064;
        context.script = bossRadioScriptEnd;
        context.messageFlags[4] = 0x300c3;
        const unsigned request = 0x8000u | (track.number - 1);
        for (uint32_t position : {bossRadioScriptStart, bossRadioScriptEnd}) {
            context.script = position;
            const auto selected = SelectCaption(0x55a6da, request, track.archive.c_str(), track.number - 1, context);
            bossPassed = bossPassed && selected.track == &track && selected.gameplayRadio && !selected.script && !selected.scene;
            for (uint32_t flags : {0u, 0x300c3u, 0x310c3u, 0x320c3u, 0x330c3u, 0x30083u, 0x31083u, 0x32083u, 0x33083u}) {
                SceneContext playback = context;
                playback.messageFlags[4] = flags;
                bossPassed = bossPassed && EvaluateContext(playback, nullptr, nullptr, &track) == CaptionGate::Allowed;
            }
        }
        for (unsigned scenario = 0; scenario < 23; ++scenario) {
            SceneContext rejected = context;
            uintptr_t caller = 0x55a6da;
            unsigned voiceRequest = request, audioTrack = track.number - 1;
            const char* archive = "XA2D_18.DAT";
            if (scenario == 0) rejected.readable = false;
            if (scenario == 1) rejected.descriptor = 0x8f4660;
            if (scenario == 2) rejected.type = 0;
            if (scenario == 3) rejected.flags = 1;
            if (scenario == 4) rejected.script = bossRadioScriptStart - 8;
            if (scenario == 5) rejected.script += 8;
            if (scenario == 6) rejected.script += 1;
            if (scenario == 7) rejected.script = 0x88b7a0;
            if (scenario == 8) rejected.messageFlags[4] = 0x100c3;
            if (scenario == 9) rejected.messageFlags[4] = 0x130c3;
            if (scenario == 10) rejected.messageFlags[4] = 0x380c3;
            if (scenario == 11) rejected.messageFlags[4] = 0;
            if (scenario == 12) caller += 1;
            if (scenario == 13) voiceRequest &= 0x7fff;
            if (scenario == 14) { audioTrack = 29; voiceRequest = 0x801d; }
            if (scenario == 15) { audioTrack = 39; voiceRequest = 0x8027; }
            if (scenario == 16) archive = "XA26_18.DAT";
            if (scenario == 17) archive = "XA12_37.DAT";
            if (scenario == 18) { rejected.messageFlags[4] = 0; rejected.messageFlags[2] = 0x300c3; }
            if (scenario >= 19) rejected.messageFlags[scenario - 19] = 0x8080;
            bossPassed = bossPassed && !SelectCaption(caller, voiceRequest, archive, audioTrack, rejected).track;
            ++bossRejections;
        }
        for (size_t index = 0; index < track.cues.size(); ++index) {
            const auto& cue = track.cues[index];
            const int next = index + 1 < track.cues.size() && track.cues[index + 1].start == cue.end ?
                static_cast<int>(index + 1) : -1;
            bossPassed = bossPassed && FindCue(track, cue.start) == static_cast<int>(index) && FindCue(track, cue.end) == next;
            ++bossCues;
        }
    }
    Log("Scoped XA2D boss cues=" + std::to_string(bossCues) + " rejected_cases=" +
        std::to_string(bossRejections) + ": " + (bossPassed ? "PASS" : "FAIL"));
    bool battlePassed = true;
    size_t battleRejections = 0, battlePositions = 0;
    for (const auto& track : battleRadioTracks) {
        const bool transition = track.number == 25;
        const auto& controller = battleRadioControllers[transition ? 1 : 0];
        SceneContext context;
        context.readable = true;
        context.type = controller.type;
        context.flags = transition ? 5 : 0;
        context.descriptor = 0x90924c;
        context.script = transition ? 0 : controller.scriptEnd;
        context.messageFlags[4] = 0x300c3;
        const unsigned request = 0x8000u | (track.number - 1);
        const auto selected = SelectCaption(0x55a6da, request, track.archive.c_str(), track.number - 1, context);
        battlePassed = battlePassed && selected.track == &track && selected.gameplayRadio && !selected.script && !selected.scene;
        std::vector<uint32_t> positions;
        if (transition) positions.push_back(0);
        for (uint32_t position = controller.scriptStart; position < controller.scriptEnd; position += 8) positions.push_back(position);
        if (!transition) positions.push_back(controller.scriptEnd);
        for (uint32_t position : positions) {
            for (unsigned phase = 0; phase < (transition ? 2u : 1u); ++phase) {
                SceneContext playback = context;
                playback.script = position;
                playback.flags = transition ? (phase ? 5 : 1) : 0;
                for (uint32_t flags : {0u, 0x300c3u, 0x310c3u, 0x320c3u, 0x330c3u, 0x30083u, 0x31083u, 0x32083u, 0x33083u}) {
                    playback.messageFlags[4] = flags;
                    battlePassed = battlePassed && EvaluateContext(playback, nullptr, nullptr, &track) == CaptionGate::Allowed;
                    ++battlePositions;
                }
                if (!transition) {
                    playback.messageFlags[4] = 0x300c3;
                    battlePassed = battlePassed && SelectCaption(0x55a6da, request, track.archive.c_str(), track.number - 1, playback).track == &track;
                }
            }
        }
        if (transition) {
            const auto& combat = battleRadioControllers[0];
            SceneContext playback = context;
            playback.type = combat.type;
            playback.flags = 0;
            for (uint32_t position = combat.scriptStart; position <= combat.scriptEnd; position += 8) {
                playback.script = position;
                playback.messageFlags[4] = 0x300c3;
                battlePassed = battlePassed && SelectCaption(0x55a6da, request, track.archive.c_str(), track.number - 1, playback).track == &track;
                for (uint32_t flags : {0u, 0x300c3u, 0x310c3u, 0x320c3u, 0x330c3u, 0x30083u, 0x31083u, 0x32083u, 0x33083u}) {
                    playback.messageFlags[4] = flags;
                    battlePassed = battlePassed && EvaluateContext(playback, nullptr, nullptr, &track) == CaptionGate::Allowed;
                    ++battlePositions;
                }
            }
        }
        for (unsigned scenario = 0; scenario < 25; ++scenario) {
            SceneContext rejected = context;
            uintptr_t caller = 0x55a6da;
            unsigned voiceRequest = request, audioTrack = track.number - 1;
            const char* archive = "XA40_18.DAT";
            if (scenario == 0) rejected.readable = false;
            if (scenario == 1) rejected.descriptor = 0x8f2064;
            if (scenario == 2) rejected.type = transition ? 0x59 : 0x5a;
            if (scenario == 3) rejected.flags = 2;
            if (scenario == 4) rejected.script = controller.scriptStart - 8;
            if (scenario == 5) rejected.script = controller.scriptEnd + 8;
            if (scenario == 6) rejected.script = controller.scriptStart + 1;
            if (scenario == 7) rejected.script = 0x88b7a0;
            if (scenario == 8) rejected.messageFlags[4] = 0x100c3;
            if (scenario == 9) rejected.messageFlags[4] = 0x130c3;
            if (scenario == 10) rejected.messageFlags[4] = 0x380c3;
            if (scenario == 11) rejected.messageFlags[4] = 0;
            if (scenario == 12) caller += 1;
            if (scenario == 13) voiceRequest &= 0x7fff;
            if (scenario == 14) archive = "XA26_18.DAT";
            if (scenario == 15) archive = "XA40_37.DAT";
            if (scenario == 16) { rejected.messageFlags[4] = 0; rejected.messageFlags[2] = 0x300c3; }
            if (scenario == 17) rejected.flags = transition ? 1 : 5;
            if (scenario == 18) { audioTrack = transition ? 26 : 29; voiceRequest = 0x8000u | audioTrack; }
            if (scenario == 19) { audioTrack = 25; voiceRequest = 0x8019; }
            if (scenario == 20) { audioTrack = 34; voiceRequest = 0x8022; }
            if (scenario >= 21) rejected.messageFlags[scenario - 21] = 0x8080;
            battlePassed = battlePassed && !SelectCaption(caller, voiceRequest, archive, audioTrack, rejected).track;
            ++battleRejections;
        }
        const auto& cue = track.cues.front();
        battlePassed = battlePassed && FindCue(track, cue.start) == 0 && FindCue(track, cue.end) == -1;
    }
    Log("Scoped XA40 battle cues=" + std::to_string(battleRadioTracks.size()) + " playback_contexts=" +
        std::to_string(battlePositions) + " rejected_cases=" + std::to_string(battleRejections) + ": " + (battlePassed ? "PASS" : "FAIL"));
    bool bathPassed = true;
    size_t bathRejections = 0;
    for (const auto& track : rollBathTracks) {
        SceneContext context;
        context.readable = true;
        context.flags = 5;
        context.type = 0x70;
        context.descriptor = 0x8d4acc;
        context.script = rollBathScriptStart;
        const auto selected = SelectCaption(0x53d35c, 0xff03, "XACOM_18.DAT", 3, context);
        bathPassed = bathPassed && selected.track == &track && selected.script && selected.voice &&
            !selected.gameplayRadio && !selected.scene;
        for (uint8_t flags : {1, 5}) {
            for (uint32_t position = rollBathScriptStart; position < rollBathScriptEnd; position += 8) {
                SceneContext playback = context;
                playback.flags = flags;
                playback.script = position;
                bathPassed = bathPassed && EvaluateContext(playback, selected.script, nullptr, &track) == CaptionGate::Allowed;
            }
        }
        for (unsigned scenario = 0; scenario < 22; ++scenario) {
            SceneContext rejected = context;
            uintptr_t caller = 0x53d35c;
            unsigned request = 0xff03, audioTrack = 3;
            const char* archive = "XACOM_18.DAT";
            if (scenario == 0) rejected.readable = false;
            if (scenario == 1) rejected.descriptor = 0x8ebee0;
            if (scenario == 2) rejected.type = 0;
            if (scenario == 3) rejected.flags = 0;
            if (scenario == 4) rejected.flags = 4;
            if (scenario == 5) rejected.flags = 3;
            if (scenario == 6) rejected.script = rollBathScriptStart - 8;
            if (scenario == 7) rejected.script = rollBathScriptEnd;
            if (scenario == 8) rejected.script += 1;
            if (scenario == 9) rejected.script = 0x88b7a0;
            if (scenario == 10) caller += 1;
            if (scenario == 11) caller = 0x55a6da;
            if (scenario == 12) request = 0xff02;
            if (scenario == 13) request = 0xff04;
            if (scenario == 14) request = 0x8003;
            if (scenario == 15) request = 0x7f03;
            if (scenario == 16) audioTrack = 2;
            if (scenario == 17) audioTrack = 4;
            if (scenario == 18) archive = "XA26_18.DAT";
            if (scenario == 19) archive = "XA02_37.DAT";
            if (scenario == 20) rejected.flags = 1;
            if (scenario == 21) rejected.script += 8;
            bathPassed = bathPassed && !SelectCaption(caller, request, archive, audioTrack, rejected).track;
            ++bathRejections;
        }
        for (size_t channel = 0; channel < context.messageFlags.size(); ++channel) {
            SceneContext dialog = context;
            dialog.messageFlags[channel] = 0x8080;
            bathPassed = bathPassed && !SelectCaption(0x53d35c, 0xff03, "XACOM_18.DAT", 3, dialog).track &&
                EvaluateContext(dialog, selected.script, nullptr, &track) == CaptionGate::NativeMessage;
            ++bathRejections;
        }
    }
    Log("Scoped Roll event tracks=" + std::to_string(rollBathTracks.size()) + " rejected_cases=" +
        std::to_string(bathRejections) + ": " + (bathPassed ? "PASS" : "FAIL"));
    SceneContext handoffContext;
    handoffContext.readable = true;
    handoffContext.flags = 5;
    handoffContext.type = 0x1c;
    CaptionSelection handoff = SelectCaption(0x5203ec, 7, "XA12_37.DAT", 7, handoffContext);
    bool handoffPassed = handoff.track && handoff.scene && !handoff.script;
    size_t replayedCues = 0;
    if (handoffPassed) {
        PlaybackClock handoffClock;
        handoffClock.Reset(147456, 176400, 0, 1000);
        int previousCue = -1;
        for (uint64_t milliseconds = 10; milliseconds <= handoff.track->cues.back().end; milliseconds += 10) {
            if (milliseconds >= 1070) handoffContext.script = 0x8a2320;
            uint32_t position = static_cast<uint32_t>((milliseconds * 176400 / 1000) % 147456);
            handoffPassed = handoffPassed && EvaluateContext(handoffContext, handoff.script, handoff.scene) == CaptionGate::Allowed &&
                handoffClock.Sample(position, 1000 + milliseconds, true) && handoffClock.Milliseconds() == milliseconds;
            int cue = FindCue(*handoff.track, handoffClock.Milliseconds());
            if (cue >= 0 && cue != previousCue) ++replayedCues;
            previousCue = cue;
        }
        handoffPassed = handoffPassed && replayedCues == handoff.track->cues.size();
        handoffContext.script = 0x8a2380;
        handoffPassed = handoffPassed && EvaluateContext(handoffContext, handoff.script, handoff.scene) == CaptionGate::OutsideScript;
    }
    Log("Registered continuation gate selections=" + std::to_string(continuationCases) + ": " + (eligibility ? "PASS" : "FAIL"));
    Log("Recorded 43:48 handoff deterministic replay cues=" + std::to_string(replayedCues) + ": " + (handoffPassed ? "PASS" : "FAIL"));
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
    bool cleared = !activeTrack && !activeScript && !activeScene && !activeGameplayRadio && activeCue == -1 && voiceSlot == -1 && !voiceClock.valid;
    Log("Cinematic gate selections=" + std::to_string(bindingCases) + " with wrong caller/audio/script and native-message exclusions: " +
        (eligibility && cleared ? "PASS" : "FAIL"));
    bool passed = wrap && pause && resume && gap && boundaries && eligibility && cleared && bankIdentity && handoffPassed &&
        continuationVoicesPassed && airshipPassed && radioPassed && bathPassed && bossPassed && battlePassed;
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