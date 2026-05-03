#pragma once

struct lua_State;

namespace Hydro::API {

void registerReflectModule(lua_State* L);

/// Called every game tick to process the next batch of a reflection dump.
/// No-op if no dump is in progress.
void tickDump();

} // namespace Hydro::API
