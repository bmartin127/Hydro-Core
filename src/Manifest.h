#pragma once

#include <string>
#include <vector>

namespace Hydro {

struct ManifestFile {
    std::string fileName;
    std::string filePath;
    std::string fileType;   // "pak", "script", "manifest", "other"
    std::string sha256;
};

struct ManifestMod {
    std::string modId;
    std::string versionId;
    std::string versionNumber;
    bool enabled = true;
    std::vector<ManifestFile> files;
};

class Manifest {
public:
    Manifest() = default;
    ~Manifest() = default;

    // Load and parse hydro_mods.json
    bool loadFromFile(const std::string& path);

    // Accessors
    const std::string& getProfileId() const { return m_profileId; }
    const std::string& getProfileName() const { return m_profileName; }
    const std::string& getGeneratedAt() const { return m_generatedAt; }
    const std::vector<ManifestMod>& getMods() const { return m_mods; }
    size_t getModCount() const { return m_mods.size(); }

private:
    std::string m_profileId;
    std::string m_profileName;
    std::string m_generatedAt;
    std::vector<ManifestMod> m_mods;
};

} // namespace Hydro
