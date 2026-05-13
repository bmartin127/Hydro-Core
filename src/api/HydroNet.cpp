/// @module Hydro.Net
/// @description Engine-agnostic networking primitives: role queries, RPC
///   dispatch, modpack identity hash, and lifecycle hooks. Built on
///   reflected UFunctions and FProperty reads only, no per-game bindings.

#include "HydroNet.h"
#include "ModuleRegistry.h"
#include "../HydroCore.h"
#include "../Manifest.h"
#include "../EngineAPI.h"
#include "../LuaUObject.h"
#include "../PropertyMarshal.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace Hydro::API {

constexpr int ROLE_Authority = 3;

// FUNC_NetClient shifted between UE 4 and UE 5; values here are UE5.
constexpr uint32_t FUNC_Net           = 0x00000040;
constexpr uint32_t FUNC_NetMulticast  = 0x00004000;
constexpr uint32_t FUNC_NetServer     = 0x00010000;
constexpr uint32_t FUNC_NetClient     = 0x01000000;

static std::wstring toWide(const char* s) {
    return std::wstring(s, s + std::strlen(s));
}

static const char* netModeName(int m) {
    switch (m) {
        case 0: return "standalone";
        case 1: return "dedicated_server";
        case 2: return "listen_server";
        case 3: return "client";
        default: return "unknown";
    }
}

static int readActorRole(void* actor, const wchar_t* propName) {
    if (!actor) return -1;
    void* cls = Engine::getClass(actor);
    if (!cls) return -1;
    void* prop = Engine::findProperty(cls, propName);
    if (!prop) return -1;
    int32_t off = Engine::getPropertyOffset(prop);
    if (off < 0) return -1;
    return (int)*((uint8_t*)actor + off);
}

static int l_net_isHost(lua_State* L) {
    int mode = Hydro::Engine::getNetMode();
    lua_pushboolean(L, mode == 1 || mode == 2);
    return 1;
}

static int l_net_mode(lua_State* L) {
    lua_pushstring(L, netModeName(Hydro::Engine::getNetMode()));
    return 1;
}

static int l_net_getLocalRole(lua_State* L) {
    lua_pushinteger(L, readActorRole(Lua::checkUObject(L, 1), L"Role"));
    return 1;
}

static int l_net_getRemoteRole(lua_State* L) {
    lua_pushinteger(L, readActorRole(Lua::checkUObject(L, 1), L"RemoteRole"));
    return 1;
}

static int l_net_hasAuthority(lua_State* L) {
    int role = readActorRole(Lua::checkUObject(L, 1), L"Role");
    lua_pushboolean(L, role == ROLE_Authority);
    return 1;
}

static int l_net_isFunctionReplicated(lua_State* L) {
    void* actor = Lua::checkUObject(L, 1);
    const char* fname = luaL_checkstring(L, 2);
    if (!actor) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "none");
        return 2;
    }
    void* cls = Engine::getClass(actor);
    auto wname = toWide(fname);
    void* fn = cls ? Engine::findFunction(cls, wname.c_str()) : nullptr;
    if (!fn) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "none");
        return 2;
    }
    uint32_t flags = Engine::getUFunctionFlags(fn);
    lua_pushboolean(L, (flags & FUNC_Net) != 0);
    if      (flags & FUNC_NetServer)    lua_pushstring(L, "server");
    else if (flags & FUNC_NetClient)    lua_pushstring(L, "client");
    else if (flags & FUNC_NetMulticast) lua_pushstring(L, "multicast");
    else                                lua_pushstring(L, "none");
    return 2;
}

static int l_net_callRemote(lua_State* L) {
    void* actor = Lua::checkUObject(L, 1);
    const char* fname = luaL_checkstring(L, 2);
    if (!actor) {
        Hydro::logWarn("Hydro.Net.callRemote: actor is nil");
        lua_pushboolean(L, 0);
        return 1;
    }
    void* cls = Engine::getClass(actor);
    auto wname = toWide(fname);
    void* fn = cls ? Engine::findFunction(cls, wname.c_str()) : nullptr;
    if (!fn) {
        Hydro::logWarn("Hydro.Net.callRemote: UFunction '%s' not found on actor", fname);
        lua_pushboolean(L, 0);
        return 1;
    }

    uint16_t parmsSize = Engine::getUFunctionParmsSize(fn);
    uint8_t* params = (uint8_t*)alloca(parmsSize > 0 ? parmsSize : 8);
    std::memset(params, 0, parmsSize > 0 ? parmsSize : 8);

    int argIdx = 3;
    void* paramProp = Engine::getChildProperties(fn);
    while (paramProp) {
        uint64_t pflags = Engine::getPropertyFlags(paramProp);
        if (pflags & Engine::CPF_ReturnParm) {
            paramProp = Engine::getNextProperty(paramProp);
            continue;
        }
        if (argIdx <= lua_gettop(L)) {
            int32_t off = Engine::getPropertyOffset(paramProp);
            if (off >= 0) {
                PropertyMarshal::writeFromLua(L, paramProp, params + off, argIdx);
            }
            argIdx++;
        }
        paramProp = Engine::getNextProperty(paramProp);
    }

    lua_pushboolean(L, Engine::callFunction(actor, fn, params) ? 1 : 0);
    return 1;
}

static bool isCDO(void* obj) {
    std::string n = Engine::getObjectName(obj);
    return n.rfind("Default__", 0) == 0;
}

// findAllOf is exact-class-name; shipping games subclass APlayerController
// (BP_ThirdPersonPlayerController, ASN2PlayerController, ...), so a raw
// "PlayerController" walk misses them. Derive from the pawn's Controller
// property instead.
static void* derivePCFromPawn(int playerIndex) {
    void* pawn = Engine::getPlayerCharacter(playerIndex);
    if (!pawn) pawn = Engine::getPlayerPawn(playerIndex);
    if (!pawn) return nullptr;
    void* pawnClass = Engine::getObjClass(pawn);
    if (!pawnClass) return nullptr;
    void* controllerProp = Engine::findProperty(pawnClass, L"Controller");
    if (!controllerProp) return nullptr;
    int32_t off = Engine::getPropertyOffset(controllerProp);
    void* pc = nullptr;
    if (Engine::readPtr((uint8_t*)pawn + off, &pc) && pc && !isCDO(pc)) return pc;
    return nullptr;
}

static int l_net_getLocalPlayerController(lua_State* L) {
    if (void* pc = derivePCFromPawn(0)) {
        Lua::pushUObject(L, pc);
        return 1;
    }
    void* out[16];
    int n = Engine::findAllOf(L"PlayerController", out, 16);
    for (int i = 0; i < n; i++) {
        if (!isCDO(out[i])) {
            Lua::pushUObject(L, out[i]);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

static int l_net_getPlayerControllers(lua_State* L) {
    lua_newtable(L);
    int idx = 1;
    void* seen[64] = {};
    int seenN = 0;

    auto pushUnique = [&](void* pc) {
        if (!pc) return;
        for (int i = 0; i < seenN; i++) if (seen[i] == pc) return;
        if (seenN < 64) seen[seenN++] = pc;
        Lua::pushUObject(L, pc);
        lua_rawseti(L, -2, idx++);
    };

    for (int i = 0; i < 8; i++) pushUnique(derivePCFromPawn(i));

    void* out[64];
    int n = Engine::findAllOf(L"PlayerController", out, 64);
    for (int i = 0; i < n; i++) {
        if (!isCDO(out[i])) pushUnique(out[i]);
    }
    return 1;
}

static uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ULL;
    }
    return h;
}

static int l_net_getModpackHash(lua_State* L) {
    const Manifest* mf = Hydro::getCurrentManifest();
    if (!mf || mf->getModCount() == 0) {
        lua_pushstring(L, "0000000000000000");
        return 1;
    }
    std::vector<const ManifestMod*> sorted;
    sorted.reserve(mf->getModCount());
    for (const auto& m : mf->getMods()) {
        if (m.enabled) sorted.push_back(&m);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const ManifestMod* a, const ManifestMod* b) {
                  return a->modId < b->modId;
              });
    std::string buf;
    buf.reserve(sorted.size() * 80);
    for (const auto* m : sorted) {
        buf += m->modId;
        buf += ':';
        buf += m->versionId;
        buf += '|';
    }
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                  static_cast<unsigned long long>(fnv1a64(buf)));
    lua_pushstring(L, hex);
    return 1;
}

static const luaL_Reg net_functions[] = {
    {"isHost",                    l_net_isHost},
    {"mode",                      l_net_mode},
    {"getLocalRole",              l_net_getLocalRole},
    {"getRemoteRole",             l_net_getRemoteRole},
    {"hasAuthority",              l_net_hasAuthority},
    {"isFunctionReplicated",      l_net_isFunctionReplicated},
    {"callRemote",                l_net_callRemote},
    {"getLocalPlayerController",  l_net_getLocalPlayerController},
    {"getPlayerControllers",      l_net_getPlayerControllers},
    {"getModpackHash",            l_net_getModpackHash},
    {nullptr,                     nullptr}
};

// onNetModeChanged is polled because UE has no reflectable global event
// for net-mode transitions. 250ms is cheap and instant enough for UI.
static const char* NET_LUA_BOOTSTRAP = R"LUA(
local Net    = ...
local Events = require("Hydro.Events")

Net.ROLE_None            = 0
Net.ROLE_SimulatedProxy  = 1
Net.ROLE_AutonomousProxy = 2
Net.ROLE_Authority       = 3

function Net.onPlayerJoin(callback)
    return Events.hook(
        "/Script/Engine.GameModeBase:K2_PostLogin",
        function(self, newPlayer) callback(newPlayer) end
    )
end

function Net.onPlayerLeave(callback)
    return Events.hook(
        "/Script/Engine.GameModeBase:K2_OnLogout",
        function(self, exitingPlayer) callback(exitingPlayer) end
    )
end

function Net.onNetModeChanged(callback)
    local last = Net.mode()
    local function watch()
        while true do
            wait(0.25)
            local cur = Net.mode()
            if cur ~= last then
                callback(cur, last)
                last = cur
            end
        end
    end
    local co = coroutine.create(watch)
    coroutine.resume(co)
    return co
end
)LUA";

void registerNetModule(lua_State* L) {
    buildModuleTable(L, net_functions);
    if (luaL_loadstring(L, NET_LUA_BOOTSTRAP) != 0) {
        Hydro::logError("Hydro.Net: bootstrap compile failed: %s",
                        lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    lua_pushvalue(L, -2);
    if (lua_pcall(L, 1, 0, 0) != 0) {
        Hydro::logError("Hydro.Net: bootstrap exec failed: %s",
                        lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

} // namespace Hydro::API
