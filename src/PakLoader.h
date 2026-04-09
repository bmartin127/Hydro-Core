#pragma once

#include <string>
#include <vector>
#include <tuple>
#include <filesystem>
#include <cstdint>

namespace Hydro {

struct PakMountResult {
    std::string pakPath;
    std::string modId;
    bool success;
    std::string error;
};

/// Mounts pak files at runtime via FPakPlatformFile::Mount.
///
/// Finds the FPakPlatformFile singleton via vtable walking (version-stable).
/// Finds Mount via the "Mounting pak file" log string (version-stable).
/// MUST call mount from the game thread (via on_update).
class PakLoader {
public:
    /// Find singleton + Mount function. Call from any thread.
    bool initialize();

    /// Mount a pak file. MUST be called from game thread.
    PakMountResult mountPak(const std::string& pakPath, const std::string& modId, int priority);

    /// Mount all paks.
    std::vector<PakMountResult> mountAll(const std::vector<std::tuple<std::string, std::string, int>>& paks);

    /// Verify deployed paks in Content/Paks/LogicMods/.
    std::filesystem::path findContentPaksDir(const std::string& gameDir) const;
    size_t verifyDeployedPaks(const std::string& gameDir) const;

    bool isReady() const { return m_pakPlatformFile && m_mountFunc; }
    size_t getMountedCount() const { return m_mountedCount; }

private:
    void* m_pakPlatformFile = nullptr;
    void** m_vtable = nullptr;
    void* m_mountFunc = nullptr;
    size_t m_mountedCount = 0;
};

} // namespace Hydro
