///
/// @module Hydro.Pak
/// @description Runtime pak mounting. Mount a .pak (and its IoStore companion
///   .utoc/.ucas, plus optional .AssetRegistry.bin sidecar) without restarting
///   the game. Backed by Engine::mountPakAtRuntime - calls
///   FPakPlatformFile::Mount on the engine's reverse-discovered instance,
///   then merges the AR.bin via IAssetRegistry::AppendState.
///
/// @depends EngineAPI (mountPakAtRuntime), AppendState bridge (must run
///   at least once before mount is callable - it captures the
///   FFilePackageStoreBackend pointer that drives FPakPlatformFile
///   reverse-discovery).
///

#include "HydroPak.h"
#include "ModuleRegistry.h"
#include "../HydroCore.h"
#include "../EngineAPI.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <string>

namespace Hydro::API {

static std::wstring toWide(const char* s) {
    std::wstring w;
    if (!s) return w;
    while (*s) w.push_back((wchar_t)(unsigned char)*s++);
    return w;
}

/// Mount a pak file at runtime.
///
/// @param pakPath string - absolute filesystem path to the .pak file
///        (the .utoc/.ucas siblings must be at the same path with their
///         respective extensions for IoStore content to register).
/// @param arBinPath string|nil - optional absolute path to the cooked
///        AssetRegistry.bin sidecar. If provided, merged into the live
///        AR via the AppendState bridge after Mount succeeds.
/// @param priority integer|nil - UE pak load order (default 1000). Higher
///        mounts later (overrides earlier paks).
/// @returns boolean - true if Mount returned true AND (if arBinPath given)
///        AppendState succeeded. false otherwise; check log for details.
/// @example
/// local Pak = require("Hydro.Pak")
/// local ok = Pak.mount(
///     "D:/.../HydroPaks/mymod.pak",
///     "D:/.../HydroPaks/mymod.AssetRegistry.bin",
///     1000)
/// if ok then
///     local cls = require("Hydro.Assets").load("/Game/Mods/MyMod/Foo.Foo_C")
///     -- ... spawn etc.
/// end
static int l_pak_mount(lua_State* L) {
    const char* pakPath = luaL_checkstring(L, 1);
    const char* arBinPath = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
    int prio = (int)luaL_optinteger(L, 3, 1000);

    std::wstring wpak = toWide(pakPath);
    std::wstring war  = arBinPath ? toWide(arBinPath) : std::wstring();

    bool ok = Hydro::Engine::mountPakAtRuntime(
        wpak.c_str(),
        war.empty() ? nullptr : war.c_str(),
        (uint32_t)prio);

    Hydro::logInfo("[Hydro.Pak] mount('%s', %s, prio=%d) = %s",
                   pakPath,
                   arBinPath ? arBinPath : "<no AR.bin>",
                   prio,
                   ok ? "OK" : "FAIL");
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/// Diagnostic: returns the runtime FPakPlatformFile instance pointer as
/// a lightuserdata, or nil if discovery hasn't completed yet (the
/// AppendState bridge must run at least once first).
static int l_pak_getInstance(lua_State* L) {
    void* p = Hydro::Engine::getFPakPlatformFile();
    if (p) lua_pushlightuserdata(L, p);
    else lua_pushnil(L);
    return 1;
}

// Module registration

static const luaL_Reg pak_functions[] = {
    {"mount",       l_pak_mount},
    {"getInstance", l_pak_getInstance},
    {nullptr, nullptr}
};

void registerPakModule(lua_State* L) {
    buildModuleTable(L, pak_functions);
}

} // namespace Hydro::API
