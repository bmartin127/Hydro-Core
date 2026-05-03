set(TARGET HydroCore)

add_library(${TARGET} SHARED
    dllmain.cpp
    EngineAPI.cpp
    HydroCore.cpp
    LuaRuntime.cpp
    LuaUObject.cpp
    Manifest.cpp
    ModManifest.cpp
    PakLoader.cpp
    registry/Registry.cpp
    registry/RegistryManager.cpp
    api/HydroAssets.cpp
    api/HydroWorld.cpp
    api/HydroEvents.cpp
    api/HydroRegistry.cpp
    api/HydroReflect.cpp
    api/HydroNet.cpp
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
