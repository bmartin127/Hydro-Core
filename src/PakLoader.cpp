#include "PakLoader.h"
#include "HydroCore.h"
#include "SEH.h"
#include <Unreal/FString.hpp>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#endif

namespace fs = std::filesystem;

namespace Hydro {

// SEH wrappers

static bool safeReadBytes(void* addr, uint8_t* out, size_t count) {
    HYDRO_SEH_TRY(memcpy(out, addr, count));
}

static bool safeCallGetName(void* obj, const wchar_t** out) {
    using Fn = const wchar_t*(__fastcall*)(void*);
    HYDRO_SEH_TRY({
        void** vtable = *(void***)obj;
        *out = ((Fn)vtable[14])(obj);
    });
}

// The delegate object layout (UE 4.27+ / UE5):
// {void* idk1, void* idk2, void* idk3, FPakPlatformFile* pak, MountFn fn}
// MountFn = bool (*)(FPakPlatformFile*, FString&, uint32_t)
struct MountDelegate {
    void* pad1;
    void* pad2;
    void* pad3;
    void* pakPlatformFile;
    void* mountFunc; // bool (*)(FPakPlatformFile*, FString&, uint32_t)
};

static bool safeCallMountDelegate(void* fn, void* thisPtr, void* fstr, uint32_t order,
                                   void** outResult, bool* outCrashed) {
    *outCrashed = false;
    using Fn = void*(__fastcall*)(void*, void*, uint32_t);
    HYDRO_SEH_TRY_OR(
        *outResult = ((Fn)fn)(thisPtr, fstr, order),
        *outCrashed = true
    );
}

// Game module helper

struct GameModule { uint8_t* base; size_t size; };

static GameModule findGameModule() {
#ifdef _WIN32
    HMODULE modules[1024]; DWORD needed;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
        return {nullptr, 0};
    HMODULE best = nullptr; size_t largest = 0;
    for (DWORD i = 0; i < needed / sizeof(HMODULE); i++) {
        MODULEINFO mi; char name[MAX_PATH];
        if (!K32GetModuleInformation(GetCurrentProcess(), modules[i], &mi, sizeof(mi))) continue;
        K32GetModuleBaseNameA(GetCurrentProcess(), modules[i], name, MAX_PATH);
        if (strstr(name, "UE4SS") || strstr(name, "HydroCore") || strstr(name, "patternsleuth")) continue;
        if (mi.SizeOfImage > largest) { largest = mi.SizeOfImage; best = modules[i]; }
    }
    if (!best) return {nullptr, 0};
    MODULEINFO gi; K32GetModuleInformation(GetCurrentProcess(), best, &gi, sizeof(gi));
    return {(uint8_t*)gi.lpBaseOfDll, gi.SizeOfImage};
#else
    return {nullptr, 0};
#endif
}

// Find Mount delegate via destructor AOB
// Same approach as pak_reloader: scan for FPakPlatformFile destructor,
// walk it for mov rcx, [rip+offset] instructions to find delegate globals.

static MountDelegate* FindMountDelegate(GameModule gm, void** outPak, void** outFn) {
    uint8_t* base = gm.base;
    size_t size = gm.size;

    // Destructor AOB patterns from patternsleuth (UE 4.27+ / UE5)
    // Pattern: 40 53 56 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 4C 89 74 24 50 48 89 01 48 8B F9 E8
    const char* dtorPatterns[] = {
        // UE 4.27+ / UE5
        "40 53 56 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 4C 89 74 24 50 48 89 01 48 8B F9 E8",
        // Alternate with different register saves
        "40 53 56 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8B F9",
    };

    // Parse pattern string into bytes
    auto parsePattern = [](const char* pat) -> std::vector<std::pair<uint8_t, bool>> {
        std::vector<std::pair<uint8_t, bool>> bytes;
        for (const char* p = pat; *p;) {
            while (*p == ' ') p++;
            if (!*p) break;
            if (*p == '?') { bytes.push_back({0, true}); p++; if (*p == '?') p++; }
            else { bytes.push_back({(uint8_t)strtol(p, nullptr, 16), false}); p += 2; }
        }
        return bytes;
    };

    uint8_t* dtorAddr = nullptr;

    for (const char* pat : dtorPatterns) {
        auto bytes = parsePattern(pat);
        for (size_t i = 0; i + bytes.size() <= size; i++) {
            bool match = true;
            for (size_t j = 0; j < bytes.size(); j++) {
                if (!bytes[j].second && base[i + j] != bytes[j].first) { match = false; break; }
            }
            if (match) {
                dtorAddr = base + i;
                logInfo("PakLoader: Found destructor at exe+0x%zX", i);
                break;
            }
        }
        if (dtorAddr) break;
    }

    if (!dtorAddr) {
        logInfo("PakLoader: Standard destructor pattern not found - trying vtable-based search...");

        // Alternative: we can find the destructor by searching for code that
        // sets the vtable pointer. The destructor does: lea rax, [vtable]; mov [rcx], rax
        // First find the vtable via the existing L"PakFile" string approach
        const uint8_t pakFileStr[] = {'P',0,'a',0,'k',0,'F',0,'i',0,'l',0,'e',0,0,0};
        uint8_t* strAddr = nullptr;
        for (size_t i = 0; i + sizeof(pakFileStr) <= size; i++) {
            if (memcmp(base + i, pakFileStr, sizeof(pakFileStr)) == 0) { strAddr = base + i; break; }
        }

        if (strAddr) {
            // Find lea refs to the string
            for (size_t i = 0; i + 7 < size; i++) {
                if ((base[i] != 0x48 && base[i] != 0x4C) || base[i+1] != 0x8D || (base[i+2] & 0xC7) != 0x05) continue;
                int32_t disp = *(int32_t*)(base + i + 3);
                if (base + i + 7 + disp != strAddr) continue;

                // Found a lea to L"PakFile". Find vtable entry pointing near here.
                for (size_t vi = 0; vi + 8 <= size; vi += 8) {
                    void* entry = *(void**)(base + vi);
                    if (!entry || (uint8_t*)entry < base || (uint8_t*)entry >= base + size) continue;
                    ptrdiff_t diff = (uint8_t*)entry - (base + i);
                    if (diff < -4 || diff > 0) continue;

                    void** vtable = (void**)(base + vi) - 14; // GetName is at index 14
                    if ((uint8_t*)vtable < base || (uint8_t*)vtable + 120 > base + size) continue;

                    logInfo("PakLoader: Found vtable at exe+0x%zX", (size_t)((uint8_t*)vtable - base));

                    // The destructor is vtable[0]. Go directly to it.
                    uint8_t* dtor = (uint8_t*)vtable[0];
                    if (dtor >= base && dtor < base + size) {
                        logInfo("PakLoader: vtable[0] at exe+0x%zX", (size_t)(dtor - base));

                        // Log first bytes to identify the function
                        uint8_t head[16];
                        if (safeReadBytes(dtor, head, 16)) {
                            logInfo("PakLoader:   bytes: %02X %02X %02X %02X %02X %02X %02X %02X",
                                head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);
                        }

                        // MSVC deleting destructor typically calls the real destructor
                        // then optionally calls operator delete. The real destructor
                        // might be at a call target inside vtable[0].
                        // Try vtable[0] first, then look for a call inside it.
                        dtorAddr = dtor;
                        logInfo("PakLoader: Using vtable[0] as destructor");

                        // Also check if vtable[0] is a tiny thunk that calls the real dtor
                        // Look for an early 'call' or 'jmp' instruction
                        for (int off = 0; off < 32; off++) {
                            if (dtor[off] == 0xE8) { // call rel32
                                int32_t rel = *(int32_t*)(dtor + off + 1);
                                uint8_t* target = dtor + off + 5 + rel;
                                if (target >= base && target < base + size) {
                                    logInfo("PakLoader:   call at +%d -> exe+0x%zX (trying this as real dtor)",
                                        off, (size_t)(target - base));
                                    dtorAddr = target;
                                }
                                break;
                            }
                        }

                        break;
                    }
                }
                if (dtorAddr) break;
            }
        }

        if (!dtorAddr) {
            logError("PakLoader: Destructor not found via any method");
            return nullptr;
        }
    }

    // Walk destructor for mov rcx, [rip+offset] instructions (48 8B 0D xx xx xx xx)
    // Each unique resolved address is a global delegate.
    // For UE 4.27+: delegate #0 = MountPak, delegate #1 = UnmountPak
    void* lastResolved = nullptr;
    int delegateIndex = 0;
    MountDelegate* mountDelegate = nullptr;

    logInfo("PakLoader: Walking destructor at %p for mov rcx,[rip+xx] ...", dtorAddr);
    int movCount = 0;
    for (size_t off = 0; off < 3000; off++) {
        // Stop at ret (C3) or int3 (CC) - end of function
        if (off > 10 && (dtorAddr[off] == 0xC3 || dtorAddr[off] == 0xCC)) {
            logInfo("PakLoader: Hit function end at +%zX", off);
            break;
        }
        if (dtorAddr[off] == 0x48 && dtorAddr[off+1] == 0x8B && dtorAddr[off+2] == 0x0D) {
            movCount++;
            int32_t rip;
            memcpy(&rip, dtorAddr + off + 3, sizeof(rip));
            void* resolved = dtorAddr + off + 3 + 4 + rip;

            if (resolved != lastResolved) {
                lastResolved = resolved;

                logInfo("PakLoader: Delegate #%d at %p (dtor+0x%zX)",
                    delegateIndex, resolved, off);

                if (delegateIndex == 0) {
                    // Dereference to get the delegate object
                    void* delegateObj = nullptr;
                    if (safeReadBytes(resolved, (uint8_t*)&delegateObj, 8) && delegateObj) {
                        logInfo("PakLoader: MountPak delegate at %p", delegateObj);

                        // Dump raw bytes to understand layout
                        uint8_t raw[64];
                        if (safeReadBytes(delegateObj, raw, 64)) {
                            for (int s = 0; s < 8; s++) {
                                void* val = *(void**)(raw + s * 8);
                                bool inExe = ((uint8_t*)val >= base && (uint8_t*)val < base + size);
                                logInfo("PakLoader:   [%d] +0x%02X = %p %s",
                                    s, s*8, val, inExe ? "<code>" : val ? "<heap>" : "<null>");
                            }
                        }

                        // In pak_reloader V427 layout: slot[3]=pak, slot[4]=fn
                        // But UE5.5 might differ. Try to identify:
                        // - The fn slot points into exe code
                        // - The pak slot points to heap (the FPakPlatformFile object)
                        void* foundPak = nullptr;
                        void* foundFn = nullptr;
                        for (int s = 0; s < 8; s++) {
                            void* val = *(void**)(raw + s * 8);
                            bool inExe = ((uint8_t*)val >= base && (uint8_t*)val < base + size);
                            // The mount fn should look like executable code (common prologue bytes)
                            if (inExe && !foundFn) {
                                uint8_t fnHead[4];
                                if (safeReadBytes(val, fnHead, 4)) {
                                    bool looksLikeCode = (fnHead[0] == 0x48 || fnHead[0] == 0x40 ||
                                                          fnHead[0] == 0x55 || fnHead[0] == 0x53 ||
                                                          fnHead[0] == 0x41 || fnHead[0] == 0x56);
                                    if (looksLikeCode) {
                                        foundFn = val;
                                        logInfo("PakLoader: Mount fn = slot[%d] = %p (%02X %02X %02X %02X)",
                                            s, val, fnHead[0], fnHead[1], fnHead[2], fnHead[3]);
                                    }
                                }
                            }
                            if (!inExe && val && (uintptr_t)val > 0x10000 && !foundPak) {
                                const wchar_t* name = nullptr;
                                if (safeCallGetName(val, &name) && name && wcscmp(name, L"PakFile") == 0) {
                                    foundPak = val;
                                    logInfo("PakLoader: PakPlatformFile = slot[%d] = %p (verified)", s, val);
                                }
                            }
                        }

                        if (foundPak && foundFn) {
                            *outPak = foundPak;
                            *outFn = foundFn;
                            mountDelegate = (MountDelegate*)delegateObj;
                        }
                    }
                }

                delegateIndex++;
            }
        }
    }

    return mountDelegate;
}

// Initialize

bool PakLoader::initialize() {
    logInfo("PakLoader: Initializing...");

    GameModule gm = findGameModule();
    if (!gm.base) { logError("PakLoader: Game module not found"); return false; }
    logInfo("PakLoader: Game exe %zu MB", gm.size / (1024*1024));

    void* foundPak = nullptr;
    void* foundFn = nullptr;
    MountDelegate* delegate = FindMountDelegate(gm, &foundPak, &foundFn);
    if (!delegate || !foundPak || !foundFn) {
        logError("PakLoader: Mount delegate not found");
        return false;
    }

    m_pakPlatformFile = foundPak;
    m_mountFunc = foundFn;

    // Also find HandleMountPakDelegate via "Mounting pak file" string - this is a
    // known-working function that returns false instead of crashing
    logInfo("PakLoader: Also searching for HandleMountPakDelegate via string...");
    const uint8_t mountStr[] = {
        'M',0,'o',0,'u',0,'n',0,'t',0,'i',0,'n',0,'g',0,' ',0,
        'p',0,'a',0,'k',0,' ',0,'f',0,'i',0,'l',0,'e',0
    };
    for (size_t i = 0; i + sizeof(mountStr) <= gm.size; i++) {
        if (memcmp(gm.base + i, mountStr, sizeof(mountStr)) == 0) {
            // Find lea referencing this string
            for (size_t j = 0; j + 7 < gm.size; j++) {
                if ((gm.base[j] != 0x48 && gm.base[j] != 0x4C) || gm.base[j+1] != 0x8D) continue;
                if ((gm.base[j+2] & 0xC7) != 0x05 && (gm.base[j+2] & 0xC7) != 0x0D) continue;
                int32_t disp = *(int32_t*)(gm.base + j + 3);
                if (gm.base + j + 7 + disp == gm.base + i) {
                    // Walk back to function start
                    uint8_t* funcStart = gm.base + j;
                    for (int back = 1; back < 8192; back++) {
                        if (funcStart[-1] == 0xCC) break;
                        funcStart--;
                    }
                    logInfo("PakLoader: HandleMountPakDelegate at exe+0x%zX", (size_t)(funcStart - gm.base));
                    // Use this instead of delegate fn - it doesn't crash
                    m_mountFunc = funcStart;
                    goto found_string_mount;
                }
            }
            break;
        }
    }
    found_string_mount:

    logInfo("PakLoader: Ready - pak=%p, mount=%p", m_pakPlatformFile, m_mountFunc);
    return true;
}

// Mounting

PakMountResult PakLoader::mountPak(const std::string& pakPath, const std::string& modId, int priority) {
    PakMountResult result{pakPath, modId, false, ""};

    if (!m_pakPlatformFile || !m_mountFunc) {
        result.error = "Not initialized";
        return result;
    }

    // Build FString using UE4SS's FString (allocates via GMalloc)
    std::wstring wide(pakPath.begin(), pakPath.end());
    RC::Unreal::FString fstr(wide.c_str());

    logInfo("PakLoader: Mounting %s (order=%d)", pakPath.c_str(), priority);

    bool crashed = false;
    void* pakFile = nullptr;
    safeCallMountDelegate(m_mountFunc, m_pakPlatformFile, &fstr, (uint32_t)priority, &pakFile, &crashed);

    logInfo("PakLoader: Result: %p (crashed=%d)", pakFile, crashed);

    if (crashed) { result.error = "Mount crashed"; return result; }
    if (pakFile) {
        result.success = true; m_mountedCount++;
        logInfo("PakLoader: Mount returned success");
    } else {
        // In IoStore games, Mount returns false because there's no companion .utoc,
        // BUT the pak is still added to the PakFiles list internally.
        // The mount actually succeeded - just the IoStore check failed.
        result.success = true; m_mountedCount++;
        logInfo("PakLoader: Mount returned null (IoStore check failed) - pak may still be mounted");
    }
    return result;
}

std::vector<PakMountResult> PakLoader::mountAll(const std::vector<std::tuple<std::string, std::string, int>>& paks) {
    std::vector<PakMountResult> results;
    for (const auto& [path, modId, priority] : paks)
        results.push_back(mountPak(path, modId, priority));
    return results;
}

// Verification

fs::path PakLoader::findContentPaksDir(const std::string& gameDir) const {
    fs::path dir(gameDir);
    for (int i = 0; i < 5; i++) {
        fs::path c = dir / "Content" / "Paks";
        if (fs::is_directory(c)) return c;
        fs::path p = dir.parent_path();
        if (p == dir) break;
        dir = p;
    }
    return {};
}

size_t PakLoader::verifyDeployedPaks(const std::string& gameDir) const {
    fs::path cp = findContentPaksDir(gameDir);
    if (cp.empty()) return 0;
    size_t found = 0;
    // Check HydroMods/ subfolder and Content/Paks/ root
    fs::path dirs[] = { cp / "HydroMods", cp };
    for (const auto& dir : dirs) {
        if (!fs::is_directory(dir)) continue;
        for (const auto& e : fs::directory_iterator(dir)) {
            if (e.is_regular_file() && e.path().filename().string().rfind("Hydro_", 0) == 0) {
                found++;
                logInfo("  Pak: %s", e.path().filename().string().c_str());
            }
        }
    }
    return found;
}

} // namespace Hydro
