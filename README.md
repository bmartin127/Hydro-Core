# HydroCore

In-game mod loader for Hydro, a modding platform for Subnautica 2. UE4SS is
used as a bootstrapper for the initial DLL injection; everything past that
runs through HydroCore's own pure-C++ engine reflection layer, so the loader
isn't coupled to UE4SS APIs and can be ported off of it later without
rewriting the engine-facing code. Mounts mod pak files, parses each mod's
`hydromod.toml`, and hosts a LuaJIT runtime so mod scripts can spawn actors,
hook UFunctions, and read/write UObject properties through a reflection API.

## Build

Windows x64. You need:

- Visual Studio 2022 with C++ + CMake workloads
- Rust via rustup (for patternsleuth)
- A built UE4SS + LuaJIT under `deps/` (see `deps/README.md`)

Then:

```
build.bat
```

Output lands at `build/src/HydroCore.dll`. The Hydro launcher deploys it into
the game as `<Game>/Binaries/Win64/ue4ss/Mods/HydroCore/dlls/main.dll`.

## Layout

- `src/dllmain.cpp` is the UE4SS adapter. Only file that touches UE4SS headers.
- `src/EngineAPI.*` does pattern scanning, GUObjectArray iteration, and ProcessEvent dispatch.
- `src/LuaRuntime.*` and `src/LuaUObject.*` host the Lua VM and expose UObject reflection.
- `src/ModManifest.*` parses `hydromod.toml`.
- `src/PakLoader.*` handles runtime pak mounting via `FPakPlatformFile`.
- `src/api/` contains the `Hydro.Assets`, `Hydro.World`, and `Hydro.Events` Lua modules.
- `types/` has `.d.lua` type definitions for IDE autocomplete when writing mods.

## Status

Works. Reads the manifest, mounts paks, initializes Lua, registers the API
modules, runs mod init scripts, and spawns Blueprint actors.

Known rough edges:

- UFunction hooking supports simple param types, not FString/FVector out params
- Dependency checks are presence-only; semver range matching lives in the launcher
- Tier 1 sandbox restricts the Lua base library but doesn't yet deny C memory
  access through bound APIs
