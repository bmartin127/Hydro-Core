///
/// @module Hydro.Net
/// @description Network role detection and modpack identity.
///
///   Three functions for now:
///     Net.isHost()         -> boolean
///     Net.mode()           -> "standalone" | "listen_server" | "dedicated_server" | "client" | "unknown"
///     Net.getModpackHash() -> string  (16-char hex, FNV-1a over sorted mod_id:version_id)
///
///   At top-level mod init the game has no session yet, so isHost() returns
///   false and mode() returns "standalone". Mods that care about role should
///   query from a BeginPlay hook or later event so the session is established.
///
/// @depends EngineAPI::getNetMode, getCurrentManifest
/// @engine_systems UWorld
///

#include "HydroNet.h"
#include "ModuleRegistry.h"
#include "../HydroCore.h"
#include "../Manifest.h"
#include "../EngineAPI.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Hydro::API {

// ENetMode -> string. Matches UE5's ENetMode enum order.
static const char* netModeName(int m) {
    switch (m) {
        case 0: return "standalone";
        case 1: return "dedicated_server";
        case 2: return "listen_server";
        case 3: return "client";
        default: return "unknown";
    }
}

static int l_net_isHost(lua_State* L) {
    int mode = Hydro::Engine::getNetMode();
    // Authoritative roles: ListenServer (host of P2P session) and
    // DedicatedServer. Standalone is single-player - not "host" in the
    // multiplayer sense, but mods often want host-style behavior there
    // too. Keep that distinction explicit: standalone != host.
    lua_pushboolean(L, mode == 2 || mode == 1);
    return 1;
}

static int l_net_mode(lua_State* L) {
    lua_pushstring(L, netModeName(Hydro::Engine::getNetMode()));
    return 1;
}

// FNV-1a 64-bit. Same algorithm HydroCore.cpp uses for pak chunk naming -
// good enough for modpack identity (deterministic, fast, vanishingly low
// collision risk for realistic mod-set sizes). Not cryptographic.
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

    // Copy + sort by modId so order doesn't matter - two players with the
    // same enabled set in any order produce the same hash.
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

// Module registration

static const luaL_Reg net_functions[] = {
    {"isHost",          l_net_isHost},
    {"mode",            l_net_mode},
    {"getModpackHash",  l_net_getModpackHash},
    {nullptr,           nullptr}
};

void registerNetModule(lua_State* L) {
    buildModuleTable(L, net_functions);
}

} // namespace Hydro::API
