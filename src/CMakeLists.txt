set(TARGET HydroCore)

add_library(${TARGET} SHARED
    dllmain.cpp
    EngineAPI.cpp
    engine/Internal.cpp
    engine/Shaders.cpp
    engine/GMalloc.cpp
    engine/IoStoreOnDemand.cpp
    engine/GUObjectArray.cpp
    engine/FName.cpp
    engine/Layout.cpp
    engine/Reflection.cpp
    engine/ProcessEvent.cpp
    engine/ObjectLookup.cpp
    engine/AssetRegistry.cpp
    engine/Spawn.cpp
    engine/Duplicate.cpp
    engine/WidgetInit.cpp
    HydroCore.cpp
    LuaRuntime.cpp
    LuaUObject.cpp
    Manifest.cpp
    PropertyMarshal.cpp
    ModManifest.cpp
    PakLoader.cpp
    FArchiveLoader.cpp
    RawFunctions.cpp
    registry/Registry.cpp
    registry/RegistryManager.cpp
    api/HydroAssets.cpp
    api/HydroWorld.cpp
    api/HydroEvents.cpp
    api/HydroRegistry.cpp
    api/HydroReflect.cpp
    api/HydroNet.cpp
    api/HydroHUD.cpp
    api/HydroUI.cpp
    api/HydroPak.cpp
    api/ModuleRegistry.cpp
)

# LuaJIT static library (built via deps/luajit/build_luajit.bat)
set(LUAJIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../deps/luajit/src")

target_include_directories(${TARGET} PRIVATE
    .
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
    "${LUAJIT_DIR}"
)

target_link_libraries(${TARGET} PUBLIC UE4SS)
target_link_libraries(${TARGET} PRIVATE "${LUAJIT_DIR}/lua51.lib")

# Zydis - x86-64 disassembler used by installInlineHookAt() for correct
# prologue decoding (including rip-relative instruction relocation). The
# library is pulled in as a transitive dep of PolyHook via UE4SS's deps
# tree; we link directly so our own hooking logic can use it without
# taking on PolyHook's full framework.
target_link_libraries(${TARGET} PRIVATE Zydis::Zydis)
