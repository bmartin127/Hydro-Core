#include "Manifest.h"
#include "HydroCore.h" // for logInfo/logError
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace Hydro {

bool Manifest::loadFromFile(const std::string& path) {
    // Read the entire file into a string first, then parse.
    // This gives a clear error if the file can't be read vs can't be parsed.
    std::ifstream file(path);
    if (!file.is_open()) {
        logError("Cannot open manifest: %s", path.c_str());
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();

    std::string content = buffer.str();
    if (content.empty()) {
        logWarn("Manifest is empty: %s", path.c_str());
        return false;
    }

    // Parse JSON with full error handling
    json doc;
    try {
        doc = json::parse(content);
    } catch (const json::parse_error& e) {
        logError("Manifest JSON parse error: %s", e.what());
        return false;
    }

    if (!doc.is_object()) {
        logError("Manifest root is not a JSON object");
        return false;
    }

    try {
        // Top-level fields - all optional with defaults
        m_profileId = doc.value("profile_id", "");
        m_profileName = doc.value("profile_name", "");
        m_generatedAt = doc.value("generated_at", "");

        // Mods array
        m_mods.clear();

        if (!doc.contains("mods")) {
            logInfo("Manifest has no mods key");
            return true;
        }

        const auto& modsVal = doc["mods"];
        if (!modsVal.is_array()) {
            logWarn("Manifest 'mods' is not an array, skipping");
            return true;
        }

        for (size_t i = 0; i < modsVal.size(); i++) {
            const auto& modObj = modsVal[i];
            if (!modObj.is_object()) {
                logWarn("Manifest mod entry %zu is not an object, skipping", i);
                continue;
            }

            ManifestMod mod;
            mod.modId = modObj.value("mod_id", "");
            mod.versionId = modObj.value("version_id", "");
            mod.versionNumber = modObj.value("version_number", "");
            mod.enabled = modObj.value("enabled", true);

            if (mod.modId.empty()) {
                logWarn("Manifest mod entry %zu has no mod_id, skipping", i);
                continue;
            }

            // Files array
            if (modObj.contains("files") && modObj["files"].is_array()) {
                for (size_t j = 0; j < modObj["files"].size(); j++) {
                    const auto& fileObj = modObj["files"][j];
                    if (!fileObj.is_object()) {
                        logWarn("Mod '%s' file entry %zu is not an object, skipping",
                            mod.modId.c_str(), j);
                        continue;
                    }

                    ManifestFile f;
                    f.fileName = fileObj.value("file_name", "");
                    f.filePath = fileObj.value("file_path", "");
                    f.fileType = fileObj.value("file_type", "other");
                    f.sha256 = fileObj.value("sha256", "");

                    if (f.filePath.empty()) {
                        logWarn("Mod '%s' file '%s' has no path, skipping",
                            mod.modId.c_str(), f.fileName.c_str());
                        continue;
                    }

                    mod.files.push_back(std::move(f));
                }
            }

            m_mods.push_back(std::move(mod));
        }

    } catch (const json::type_error& e) {
        logError("Manifest type error: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        logError("Manifest processing error: %s", e.what());
        return false;
    }

    logInfo("Parsed manifest: %zu mods from profile '%s'",
        m_mods.size(), m_profileName.c_str());
    return true;
}

} // namespace Hydro
