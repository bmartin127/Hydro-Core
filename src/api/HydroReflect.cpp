#include "HydroReflect.h"
#include "HydroEvents.h"
#include "ModuleRegistry.h"
#include "../HydroCore.h"
#include "../EngineAPI.h"
#include "../LuaUObject.h"

extern "C" {
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
}

#include <string>
#include <algorithm>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <cstring>

namespace Hydro::API {

// Minimal JSON string escaper. Writes the quoted, escaped form of `s`
// directly into the output stream. Handles ", \, control chars, and
// embedded nulls. Assumes input is UTF-8-compatible ASCII (UE FNames are).
static void writeJsonString(std::ostream& out, const std::string& s) {
    out << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out << buf;
                } else {
                    out << (char)c;
                }
        }
    }
    out << '"';
}

// Case-insensitive substring match
static bool containsCI(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) { return tolower(a) == tolower(b); });
    return it != haystack.end();
}

// Get a property type's display name from its FFieldClass name index
static std::string getPropertyTypeName(void* prop) {
    uint32_t typeIdx = Engine::getPropertyTypeNameIndex(prop);
    return Engine::getNameString(typeIdx);
}

// Build a flags string like "Parm|ReturnParm|OutParm"
static std::string formatPropertyFlags(uint64_t flags) {
    std::string s;
    if (flags & Engine::CPF_Parm)       { if (!s.empty()) s += "|"; s += "Parm"; }
    if (flags & Engine::CPF_OutParm)    { if (!s.empty()) s += "|"; s += "OutParm"; }
    if (flags & Engine::CPF_ReturnParm) { if (!s.empty()) s += "|"; s += "ReturnParm"; }
    return s.empty() ? "None" : s;
}

// Build a JSON array of FProperty flag names for the common UE5 CPF_* bits
// that matter for scaffolding and docs. Values from UE5 ObjectMacros.h.
static void writePropertyFlagsJsonArray(std::ostream& out, uint64_t flags) {
    out << '[';
    bool first = true;
    auto add = [&](const char* name) {
        if (!first) out << ',';
        out << '"' << name << '"';
        first = false;
    };
    if (flags & 0x0000000000000001ULL) add("Edit");
    if (flags & 0x0000000000000002ULL) add("ConstParm");
    if (flags & 0x0000000000000004ULL) add("BlueprintVisible");
    if (flags & 0x0000000000000008ULL) add("ExportObject");
    if (flags & 0x0000000000000010ULL) add("BlueprintReadOnly");
    if (flags & 0x0000000000000020ULL) add("Net");
    if (flags & 0x0000000000000080ULL) add("Parm");
    if (flags & 0x0000000000000100ULL) add("OutParm");
    if (flags & 0x0000000000000400ULL) add("ReturnParm");
    if (flags & 0x0000000000000800ULL) add("DisableEditOnTemplate");
    if (flags & 0x0000000000002000ULL) add("Transient");
    if (flags & 0x0000000000004000ULL) add("Config");
    if (flags & 0x0000000000010000ULL) add("DisableEditOnInstance");
    if (flags & 0x0000000000020000ULL) add("EditConst");
    if (flags & 0x0000000000080000ULL) add("BlueprintAssignable");
    if (flags & 0x0000000000100000ULL) add("Deprecated");
    if (flags & 0x0000000000800000ULL) add("Protected");
    if (flags & 0x0000000001000000ULL) add("BlueprintCallable");
    if (flags & 0x0000000008000000ULL) add("EditorOnly");
    if (flags & 0x0000000400000000ULL) add("SaveGame");
    out << ']';
}

// Build a JSON array of UClass flag names for common UE5 CLASS_* bits.
// Values from UE5 ObjectMacros.h (EClassFlags enum).
static void writeClassFlagsJsonArray(std::ostream& out, uint32_t flags) {
    out << '[';
    bool first = true;
    auto add = [&](const char* name) {
        if (!first) out << ',';
        out << '"' << name << '"';
        first = false;
    };
    if (flags & 0x00000001) add("Abstract");
    if (flags & 0x00000002) add("DefaultConfig");
    if (flags & 0x00000004) add("Config");
    if (flags & 0x00000008) add("Transient");
    if (flags & 0x00000080) add("Native");
    if (flags & 0x00000400) add("PerObjectConfig");
    if (flags & 0x00001000) add("EditInlineNew");
    if (flags & 0x00004000) add("Interface");
    if (flags & 0x00010000) add("Const");
    if (flags & 0x00040000) add("CompiledFromBlueprint");
    if (flags & 0x00080000) add("MinimalAPI");
    if (flags & 0x00100000) add("RequiredAPI");
    if (flags & 0x00200000) add("DefaultToInstanced");
    if (flags & 0x02000000) add("Deprecated");
    if (flags & 0x10000000) add("Intrinsic");
    out << ']';
}

// Build a JSON array of UFunction flag names for common UE5 FUNC_* bits.
// Values from UE5 ObjectMacros.h (EFunctionFlags enum).
static void writeFunctionFlagsJsonArray(std::ostream& out, uint32_t flags) {
    out << '[';
    bool first = true;
    auto add = [&](const char* name) {
        if (!first) out << ',';
        out << '"' << name << '"';
        first = false;
    };
    if (flags & 0x00000001) add("Final");
    if (flags & 0x00000002) add("RequiredAPI");
    if (flags & 0x00000004) add("BlueprintAuthorityOnly");
    if (flags & 0x00000008) add("BlueprintCosmetic");
    if (flags & 0x00000040) add("Net");
    if (flags & 0x00000080) add("NetReliable");
    if (flags & 0x00000100) add("NetRequest");
    if (flags & 0x00000200) add("Exec");
    if (flags & 0x00000400) add("Native");
    if (flags & 0x00000800) add("Event");
    if (flags & 0x00001000) add("NetResponse");
    if (flags & 0x00002000) add("Static");
    if (flags & 0x00004000) add("NetMulticast");
    if (flags & 0x00010000) add("MulticastDelegate");
    if (flags & 0x00020000) add("Public");
    if (flags & 0x00040000) add("Private");
    if (flags & 0x00080000) add("Protected");
    if (flags & 0x00100000) add("Delegate");
    if (flags & 0x00200000) add("NetServer");
    if (flags & 0x00400000) add("HasOutParms");
    if (flags & 0x00800000) add("HasDefaults");
    if (flags & 0x01000000) add("NetClient");
    if (flags & 0x02000000) add("DLLImport");
    if (flags & 0x04000000) add("BlueprintCallable");
    if (flags & 0x08000000) add("BlueprintEvent");
    if (flags & 0x10000000) add("BlueprintPure");
    if (flags & 0x20000000) add("EditorOnly");
    if (flags & 0x40000000) add("Const");
    out << ']';
}

// Class cache: rebuilt when object count grows by >500 (catches map transitions without constant rebuilds).
static std::unordered_map<void*, std::string> s_classCache;
static bool s_classCacheBuilt = false;
static int32_t s_classCacheObjCount = 0;   // total at last build

static void buildClassCache() {
    int32_t total = Engine::getObjectCount();
    if (s_classCacheBuilt && total <= s_classCacheObjCount + 500) return;
    if (s_classCacheBuilt) {
        Hydro::logInfo("[Hydro.Reflect] Class cache rebuild: %d -> %d objects (+%d)",
                       s_classCacheObjCount, total, total - s_classCacheObjCount);
        s_classCache.clear();
    }

    // Collect unique class pointers from all live objects
    std::unordered_set<void*> classSet;
    for (int32_t i = 0; i < total; i++) {
        void* obj = Engine::getObjectAt(i);
        if (!obj) continue;
        void* cls = Engine::getObjClass(obj);
        if (cls) classSet.insert(cls);
    }

    // Phase 2: walk SuperStruct chains (with depth limit for safety)
    std::vector<void*> toWalk(classSet.begin(), classSet.end());
    for (void* cls : toWalk) {
        void* super = Engine::getSuper(cls);
        int depth = 0;
        while (super && depth < 32) {
            classSet.insert(super);
            super = Engine::getSuper(super);
            depth++;
        }
    }

    // Phase 3: resolve all names. With FNamePool direct reading this is
    // instant (just memory reads). Falls back to Conv_NameToString if pool
    // is unavailable (slower but still works).
    for (void* cls : classSet) {
        std::string name = Engine::getObjectName(cls);
        if (!name.empty() && name[0] != '<') {
            s_classCache[cls] = std::move(name);
        }
    }

    Hydro::logInfo("[Hydro.Reflect] Class cache built: %zu classes from %d objects",
                   s_classCache.size(), total);
    s_classCacheObjCount = total;
    s_classCacheBuilt = true;
}

// findClass(query) -> array of { name }
// Enumerates classes by collecting unique ClassPrivate pointers from every
// object in GUObjectArray. Class set is cached after first call.
// Filters by case-insensitive substring match on the class name.
static int l_findClass(lua_State* L) {
    const char* query = luaL_checkstring(L, 1);
    std::string q(query);

    buildClassCache();

    lua_newtable(L);
    int count = 0;
    for (const auto& [cls, name] : s_classCache) {
        if (count >= 500) break;
        if (!q.empty() && !containsCI(name, q)) continue;

        count++;
        lua_createtable(L, 0, 1);
        lua_pushstring(L, name.c_str());
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, count);
    }
    return 1;
}

// functions(classPath) -> array of { name, params[], parmsSize, returnOffset, native }
static int l_functions(lua_State* L) {
    const char* classPath = luaL_checkstring(L, 1);
    std::wstring widePath(classPath, classPath + strlen(classPath));

    void* uclass = Engine::findObject(widePath.c_str());
    if (!uclass) {
        // Try as a class name via findFirstOf
        uclass = Engine::findFirstOf(widePath.c_str());
    }
    if (!uclass) {
        lua_newtable(L);
        return 1;
    }

    lua_newtable(L);
    int count = 0;

    // Walk the UClass children chain to find UFunctions. In UE5, the
    // Children field (at the same offset as ChildProperties for UStruct)
    // holds UField children which includes UFunctions. We use the existing
    // findFunction infrastructure but iterate instead of searching by name.
    // UStruct::Children is at offset 0x48 (after SuperStruct at 0x40).
    // Each child is a UField with a Next pointer at offset 0x28.
    constexpr int CHILDREN_OFFSET = 0x48;
    constexpr int UFIELD_NEXT = 0x28;

    void* child = nullptr;
    Engine::readPtr((uint8_t*)uclass + 0x48, &child);

    uint32_t functionNameIdx = Engine::makeFName(L"Function");

    while (child) {
        // Check if this child is a UFunction by checking its class name
        void* childClass = Engine::getObjClass(child);
        uint32_t childClassNameIdx = childClass ? Engine::getNameIndex(childClass) : 0;

        if (childClassNameIdx == functionNameIdx) {
            count++;
            std::string funcName = Engine::getObjectName(child);
            uint16_t parmsSize = Engine::getUFunctionParmsSize(child);
            uint16_t retOffset = Engine::getUFunctionRetOffset(child);
            uint32_t funcFlags = Engine::getUFunctionFlags(child);
            bool isNative = (funcFlags & 0x400) != 0; // FUNC_Native

            lua_createtable(L, 0, 5);
            lua_pushstring(L, funcName.c_str());
            lua_setfield(L, -2, "name");
            lua_pushinteger(L, parmsSize);
            lua_setfield(L, -2, "parmsSize");
            lua_pushinteger(L, retOffset);
            lua_setfield(L, -2, "returnOffset");
            lua_pushboolean(L, isNative ? 1 : 0);
            lua_setfield(L, -2, "native");

            // Walk the UFunction's own ChildProperties for parameter info
            lua_newtable(L); // params array
            int paramCount = 0;
            void* prop = Engine::getChildProperties(child);
            while (prop) {
                paramCount++;
                std::string pName = Engine::getFieldName(prop);
                std::string pType = getPropertyTypeName(prop);
                int32_t pSize = Engine::getPropertyElementSize(prop);
                uint64_t pFlags = Engine::getPropertyFlags(prop);

                lua_createtable(L, 0, 4);
                lua_pushstring(L, pName.c_str());
                lua_setfield(L, -2, "name");
                lua_pushstring(L, pType.c_str());
                lua_setfield(L, -2, "type");
                lua_pushinteger(L, pSize);
                lua_setfield(L, -2, "size");
                lua_pushstring(L, formatPropertyFlags(pFlags).c_str());
                lua_setfield(L, -2, "flags");

                lua_rawseti(L, -2, paramCount);
                prop = Engine::getNextProperty(prop);
            }
            lua_setfield(L, -2, "params");

            lua_rawseti(L, -2, count);
        }

        // Next child in the UField chain
        void* next = nullptr;
        Engine::readPtr((uint8_t*)child + UFIELD_NEXT, &next);
        child = next;
    }

    return 1;
}

// properties(classPath) -> array of { name, type, offset, size }
static int l_properties(lua_State* L) {
    const char* classPath = luaL_checkstring(L, 1);
    std::wstring widePath(classPath, classPath + strlen(classPath));

    void* uclass = Engine::findObject(widePath.c_str());
    if (!uclass) uclass = Engine::findFirstOf(widePath.c_str());
    if (!uclass) {
        lua_newtable(L);
        return 1;
    }

    lua_newtable(L);
    int count = 0;

    void* prop = Engine::getChildProperties(uclass);
    while (prop) {
        count++;
        std::string pName = Engine::getFieldName(prop);
        std::string pType = getPropertyTypeName(prop);
        int32_t offset = Engine::getPropertyOffset(prop);
        int32_t size = Engine::getPropertyElementSize(prop);

        lua_createtable(L, 0, 4);
        lua_pushstring(L, pName.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, pType.c_str());
        lua_setfield(L, -2, "type");
        lua_pushinteger(L, offset);
        lua_setfield(L, -2, "offset");
        lua_pushinteger(L, size);
        lua_setfield(L, -2, "size");

        lua_rawseti(L, -2, count);
        prop = Engine::getNextProperty(prop);
    }

    return 1;
}

// inspect(uobject) -> table of { propName = value, ... }
static int l_inspect(lua_State* L) {
    void* obj = Lua::checkUObject(L, 1);
    if (!obj) {
        lua_newtable(L);
        return 1;
    }

    void* cls = Engine::getClass(obj);
    if (!cls) {
        lua_newtable(L);
        return 1;
    }

    lua_newtable(L);

    // Walk the class property chain and read each value
    void* prop = Engine::getChildProperties(cls);
    while (prop) {
        std::string pName = Engine::getObjectName(prop);
        int32_t offset = Engine::getPropertyOffset(prop);
        int32_t size = Engine::getPropertyElementSize(prop);

        // Use LuaUObject's pushPropertyValue to read the value in the
        // right type. It pushes one value onto the stack.
        Lua::pushPropertyValue(L, obj, prop);
        if (!lua_isnil(L, -1)) {
            lua_setfield(L, -2, pName.c_str());
        } else {
            lua_pop(L, 1);
        }

        prop = Engine::getNextProperty(prop);
    }

    return 1;
}

// findAll(classPath) -> array of UObject userdata
static int l_findAll(lua_State* L) {
    const char* classPath = luaL_checkstring(L, 1);
    std::wstring widePath(classPath, classPath + strlen(classPath));

    void* results[256] = {};
    int count = Engine::findAllOf(widePath.c_str(), results, 256);

    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        Lua::pushUObject(L, results[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// hierarchy(classPath) -> array of class name strings
static int l_hierarchy(lua_State* L) {
    const char* classPath = luaL_checkstring(L, 1);
    std::wstring widePath(classPath, classPath + strlen(classPath));

    void* cls = Engine::findObject(widePath.c_str());
    if (!cls) cls = Engine::findFirstOf(widePath.c_str());
    if (!cls) {
        lua_newtable(L);
        return 1;
    }

    lua_newtable(L);
    int count = 0;

    void* cur = cls;
    while (cur && count < 32) {
        count++;
        std::string name = Engine::getObjectName(cur);
        lua_pushstring(L, name.c_str());
        lua_rawseti(L, -2, count);
        cur = Engine::getSuper(cur);
    }

    return 1;
}

// assets(query) -> array of { path, className }
static int l_assets(lua_State* L) {
    const char* query = luaL_optstring(L, 1, "");
    std::string q(query);

    lua_newtable(L);
    int count = 0;
    int total = Engine::getObjectCount();

    for (int32_t i = 0; i < total && count < 500; i++) {
        void* obj = Engine::getObjectAt(i);
        if (!obj) continue;

        std::string name = Engine::getObjectName(obj);
        if (name.empty()) continue;

        // Filter to interesting objects (skip internal engine objects)
        if (!q.empty() && !containsCI(name, q)) continue;

        void* cls = Engine::getObjClass(obj);
        std::string className = cls ? Engine::getObjectName(cls) : "Unknown";

        // Skip very common uninteresting classes to avoid flooding
        if (className == "Function" || className == "Package" ||
            className == "ScriptStruct" || className == "Enum") continue;

        count++;
        lua_createtable(L, 0, 2);
        lua_pushstring(L, name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, className.c_str());
        lua_setfield(L, -2, "className");
        lua_rawseti(L, -2, count);
    }

    return 1;
}

// dump(outputDir) -> { classes, files }
// Full reflection dump. With FNamePool direct reading, name resolution is
// just memory reads - no ProcessEvent calls. The entire dump of ~10k classes
// completes in under a second with no game stutter.
// Build the sanitised package-relative path for a given classPath like
// "/Script/Engine.Actor" -> "Script/Engine/Actor". Returns empty string if
// the path is unusable (contains FName garbage, None, or is entirely empty).
// The outer's FName may contain slashes (e.g. "/Script/Engine"); those
// become directory separators naturally.
static std::string classPathToRelPath(const std::string& classPath) {
    if (classPath.empty()) return {};
    if (classPath.find("<FName") != std::string::npos) return {};
    if (classPath.find("/None") != std::string::npos) return {};

    std::string relPath;
    for (char c : classPath) {
        if (c == '.') relPath += '/';
        else if (c == '<' || c == '>' || c == ':' || c == '"' ||
                 c == '|' || c == '?' || c == '*') continue;
        else relPath += c;
    }
    while (!relPath.empty() && relPath[0] == '/') relPath = relPath.substr(1);
    if (relPath.empty() || relPath == "None") return {};
    return relPath;
}

// Write a single class's JSON representation to the given stream.
// Schema: {schemaVersion, kind, name, path, super, hierarchy, flags,
//          properties: [{name, type, offset, size, flags}],
//          functions: [{name, native, parmsSize, returnOffset, flags, params: [...]}]}
static void writeClassJson(std::ostream& out, void* cls, const std::string& className,
                            const std::string& classPath, uint32_t functionNameIdx) {
    out << "{\n";
    out << "  \"schemaVersion\": 1,\n";
    out << "  \"kind\": \"class\",\n";
    out << "  \"name\": "; writeJsonString(out, className); out << ",\n";
    out << "  \"path\": "; writeJsonString(out, classPath); out << ",\n";

    // Hierarchy chain - outer super first? No: class-first, walking up via
    // getSuper. Example for AActor: ["Actor", "Object"].
    out << "  \"hierarchy\": [";
    {
        bool first = true;
        void* cur = cls;
        int depth = 0;
        while (cur && depth < 32) {
            std::string name = Engine::getObjectName(cur);
            if (name.empty() || name[0] == '<') break;
            if (!first) out << ", ";
            writeJsonString(out, name);
            first = false;
            cur = Engine::getSuper(cur);
            depth++;
        }
    }
    out << "],\n";

    // Super (immediate parent) for quick lookup
    {
        void* super = Engine::getSuper(cls);
        if (super) {
            std::string superName = Engine::getObjectName(super);
            if (!superName.empty() && superName[0] != '<') {
                out << "  \"super\": "; writeJsonString(out, superName); out << ",\n";
            } else {
                out << "  \"super\": null,\n";
            }
        } else {
            out << "  \"super\": null,\n";
        }
    }

    // Class flags
    uint32_t cflags = Engine::getClassFlags(cls);
    out << "  \"classFlags\": ";
    writeClassFlagsJsonArray(out, cflags);
    out << ",\n";

    // Properties array
    out << "  \"properties\": [";
    {
        bool first = true;
        void* prop = Engine::getChildProperties(cls);
        int maxProps = 500;
        while (prop && maxProps-- > 0) {
            std::string pName = Engine::getFieldName(prop);
            if (pName.empty() || pName == "None" || pName[0] == '<') {
                prop = Engine::getNextProperty(prop);
                continue;
            }
            if (!first) out << ",";
            out << "\n    {";
            out << "\"name\": "; writeJsonString(out, pName);
            out << ", \"type\": "; writeJsonString(out, getPropertyTypeName(prop));
            out << ", \"offset\": " << Engine::getPropertyOffset(prop);
            out << ", \"size\": " << Engine::getPropertyElementSize(prop);
            out << ", \"flags\": ";
            writePropertyFlagsJsonArray(out, Engine::getPropertyFlags(prop));
            out << "}";
            first = false;
            prop = Engine::getNextProperty(prop);
        }
    }
    out << "\n  ],\n";

    // Functions array - walk UClass::Children linked list at offset 0x48
    out << "  \"functions\": [";
    {
        bool firstFn = true;
        void* child = nullptr;
        Engine::readPtr((uint8_t*)cls + 0x48, &child);
        int maxChildren = 1000;
        while (child && maxChildren-- > 0) {
            void* childClass = Engine::getObjClass(child);
            uint32_t ccni = childClass ? Engine::getNameIndex(childClass) : 0;
            if (ccni == functionNameIdx) {
                std::string fName = Engine::getObjectName(child);
                if (!fName.empty() && fName != "None" && fName[0] != '<') {
                    if (!firstFn) out << ",";
                    uint32_t ff = Engine::getUFunctionFlags(child);
                    out << "\n    {";
                    out << "\"name\": "; writeJsonString(out, fName);
                    out << ", \"native\": " << ((ff & 0x400) ? "true" : "false");
                    out << ", \"parmsSize\": " << Engine::getUFunctionParmsSize(child);
                    out << ", \"returnOffset\": " << Engine::getUFunctionRetOffset(child);
                    out << ", \"flags\": ";
                    writeFunctionFlagsJsonArray(out, ff);
                    out << ", \"params\": [";

                    void* fp = Engine::getChildProperties(child);
                    bool firstParam = true;
                    int maxParams = 64;
                    while (fp && maxParams-- > 0) {
                        std::string fpName = Engine::getFieldName(fp);
                        if (!fpName.empty() && fpName != "None" && fpName[0] != '<') {
                            if (!firstParam) out << ", ";
                            out << "{\"name\": ";
                            writeJsonString(out, fpName);
                            out << ", \"type\": ";
                            writeJsonString(out, getPropertyTypeName(fp));
                            out << ", \"flags\": ";
                            writePropertyFlagsJsonArray(out, Engine::getPropertyFlags(fp));
                            out << "}";
                            firstParam = false;
                        }
                        fp = Engine::getNextProperty(fp);
                    }
                    out << "]}";
                    firstFn = false;
                }
            }
            void* next = nullptr;
            Engine::readPtr((uint8_t*)child + 0x28, &next);
            child = next;
        }
    }
    out << "\n  ]\n";
    out << "}\n";
}

// Write a UScriptStruct as JSON. Structs share UStruct layout with UClass,
// so the property-walking logic is the same. Difference: no functions chain,
// no super hierarchy (structs don't inherit meaningfully), just a property
// list. Schema: {schemaVersion, kind: "struct", name, path, size, properties}.
static void writeStructJson(std::ostream& out, void* ustruct,
                             const std::string& name, const std::string& path) {
    out << "{\n";
    out << "  \"schemaVersion\": 1,\n";
    out << "  \"kind\": \"struct\",\n";
    out << "  \"name\": "; writeJsonString(out, name); out << ",\n";
    out << "  \"path\": "; writeJsonString(out, path); out << ",\n";

    // Super struct (if any) - e.g. FRotator's super is FStruct, etc.
    {
        void* super = Engine::getSuper(ustruct);
        if (super) {
            std::string superName = Engine::getObjectName(super);
            if (!superName.empty() && superName[0] != '<') {
                out << "  \"super\": "; writeJsonString(out, superName); out << ",\n";
            } else {
                out << "  \"super\": null,\n";
            }
        } else {
            out << "  \"super\": null,\n";
        }
    }

    // Properties array
    out << "  \"properties\": [";
    {
        bool first = true;
        void* prop = Engine::getChildProperties(ustruct);
        int maxProps = 500;
        while (prop && maxProps-- > 0) {
            std::string pName = Engine::getFieldName(prop);
            if (pName.empty() || pName == "None" || pName[0] == '<') {
                prop = Engine::getNextProperty(prop);
                continue;
            }
            if (!first) out << ",";
            out << "\n    {";
            out << "\"name\": "; writeJsonString(out, pName);
            out << ", \"type\": "; writeJsonString(out, getPropertyTypeName(prop));
            out << ", \"offset\": " << Engine::getPropertyOffset(prop);
            out << ", \"size\": " << Engine::getPropertyElementSize(prop);
            out << ", \"flags\": ";
            writePropertyFlagsJsonArray(out, Engine::getPropertyFlags(prop));
            out << "}";
            first = false;
            prop = Engine::getNextProperty(prop);
        }
    }
    out << "\n  ]\n";
    out << "}\n";
}

// Write a UEnum as JSON. Reads the Names TArray via readEnumNames and
// emits each (name, value) pair. Schema: {schemaVersion, kind: "enum",
// name, path, values: [{name, value}]}.
// Enum entry names come from the FNamePool as fully-qualified
// "EnumName::EntryName" strings because that's how UE stores them
// internally. We strip the "EnumName::" prefix so consumers see clean
// short names ("Walking" instead of "EMovementMode::Walking").
static void writeEnumJson(std::ostream& out, void* uenum,
                           const std::string& name, const std::string& path) {
    out << "{\n";
    out << "  \"schemaVersion\": 1,\n";
    out << "  \"kind\": \"enum\",\n";
    out << "  \"name\": "; writeJsonString(out, name); out << ",\n";
    out << "  \"path\": "; writeJsonString(out, path); out << ",\n";

    std::vector<std::pair<uint32_t, int64_t>> entries;
    Engine::readEnumNames(uenum, entries);

    const std::string prefix = name + "::";

    out << "  \"values\": [";
    bool first = true;
    for (const auto& [idx, val] : entries) {
        std::string entryName = Engine::getNameString(idx);
        if (entryName.empty() || entryName[0] == '<') continue;
        // Strip the "EnumName::" qualifier so downstream consumers get
        // short names. Keep the full name as a fallback if the entry
        // doesn't use the expected prefix format.
        std::string shortName =
            (entryName.rfind(prefix, 0) == 0) ? entryName.substr(prefix.size())
                                              : entryName;
        if (!first) out << ",";
        out << "\n    {\"name\": ";
        writeJsonString(out, shortName);
        out << ", \"value\": " << val << "}";
        first = false;
    }
    out << "\n  ]\n";
    out << "}\n";
}

// Legacy text format (kept for debugging via Reflect.dump(dir, "text")).
static void writeClassText(std::ostream& out, void* cls, const std::string& className,
                            const std::string& classPath, uint32_t functionNameIdx) {
    out << "Class: " << className << "\n";
    out << "Path: " << classPath << "\n";

    out << "Hierarchy: ";
    {
        void* cur = cls;
        int depth = 0;
        while (cur && depth < 32) {
            if (depth > 0) out << " -> ";
            std::string name = Engine::getObjectName(cur);
            if (name.empty() || name[0] == '<') break;
            out << name;
            cur = Engine::getSuper(cur);
            depth++;
        }
    }
    out << "\n\n";

    int propCount = 0;
    std::ostringstream propBuf;
    void* prop = Engine::getChildProperties(cls);
    int maxProps = 500;
    while (prop && maxProps-- > 0) {
        std::string pName = Engine::getFieldName(prop);
        if (pName.empty() || pName == "None" || pName[0] == '<') {
            prop = Engine::getNextProperty(prop);
            continue;
        }
        propCount++;
        char buf[256];
        snprintf(buf, sizeof(buf), "  %-40s [%-20s] offset=0x%04X size=%d\n",
                 pName.c_str(), getPropertyTypeName(prop).c_str(),
                 Engine::getPropertyOffset(prop),
                 Engine::getPropertyElementSize(prop));
        propBuf << buf;
        prop = Engine::getNextProperty(prop);
    }
    out << "=== Properties (" << propCount << ") ===\n" << propBuf.str() << "\n";

    int funcCount = 0;
    std::ostringstream funcBuf;
    void* child = nullptr;
    Engine::readPtr((uint8_t*)cls + 0x48, &child);
    int maxChildren = 1000;
    while (child && maxChildren-- > 0) {
        void* childClass = Engine::getObjClass(child);
        uint32_t ccni = childClass ? Engine::getNameIndex(childClass) : 0;
        if (ccni == functionNameIdx) {
            std::string fName = Engine::getObjectName(child);
            if (!fName.empty() && fName != "None" && fName[0] != '<') {
                funcCount++;
                uint32_t ff = Engine::getUFunctionFlags(child);
                funcBuf << "  " << fName << "(";
                void* fp = Engine::getChildProperties(child);
                bool firstP = true;
                int maxParams = 64;
                while (fp && maxParams-- > 0) {
                    std::string fpName = Engine::getFieldName(fp);
                    if (!fpName.empty() && fpName != "None" && fpName[0] != '<') {
                        if (!firstP) funcBuf << ", ";
                        funcBuf << fpName << ": " << getPropertyTypeName(fp) << " ["
                                << formatPropertyFlags(Engine::getPropertyFlags(fp)) << "]";
                        firstP = false;
                    }
                    fp = Engine::getNextProperty(fp);
                }
                funcBuf << ")";
                if (ff & 0x400) funcBuf << " [native]";
                funcBuf << "  parmsSize=" << Engine::getUFunctionParmsSize(child)
                        << " retOff=" << Engine::getUFunctionRetOffset(child) << "\n";
            }
        }
        void* next = nullptr;
        Engine::readPtr((uint8_t*)child + 0x28, &next);
        child = next;
    }
    out << "=== Functions (" << funcCount << ") ===\n" << funcBuf.str();
}

// Find the "Class" metaclass - the only UClass whose ClassPrivate points
// to itself. All real UClass objects have ClassPrivate == this metaclass.
// Shared helper used by both the dump and future Slice 2 extensions.
static void* findClassMetaclass() {
    void* uobjectClass = Engine::findObject(L"/Script/CoreUObject.Object");
    if (!uobjectClass) return nullptr;
    void* meta = Engine::getObjClass(uobjectClass);
    if (!meta) return nullptr;
    if (Engine::getObjClass(meta) != meta) return nullptr;
    return meta;
}

// trace{target, seconds, output} - log every PE/PI/BP call on the given
// target UObject for `seconds` seconds to a JSONL file at `output`.
// target: UObject userdata (from Reflect.findAll / findFirstOf) OR a path
//         string (e.g. "/Script/Engine.Actor") resolved via findObject.
// seconds: trace duration in wall-clock seconds (approximately 60 game ticks).
// output: path to the output JSONL file. Parent directories are created.
// Returns {ok, expireTick, target} on success or raises a Lua error.
static int l_trace(lua_State* L) {
    if (!lua_istable(L, 1)) {
        return luaL_error(L, "Reflect.trace expects a table: {target, seconds, output}");
    }

    // target - userdata or string
    void* target = nullptr;
    lua_getfield(L, 1, "target");
    if (lua_isuserdata(L, -1)) {
        target = Lua::checkUObject(L, -1);
    } else if (lua_isstring(L, -1)) {
        const char* path = lua_tostring(L, -1);
        std::wstring widePath(path, path + strlen(path));
        target = Engine::findObject(widePath.c_str());
        if (!target) target = Engine::findFirstOf(widePath.c_str());
    }
    lua_pop(L, 1);

    if (!target) {
        return luaL_error(L, "Reflect.trace: target is nil or not found");
    }

    // seconds (default 10)
    lua_getfield(L, 1, "seconds");
    int seconds = (int)luaL_optinteger(L, -1, 10);
    lua_pop(L, 1);
    if (seconds <= 0 || seconds > 3600) {
        return luaL_error(L, "Reflect.trace: seconds must be 1..3600");
    }

    // output (required)
    lua_getfield(L, 1, "output");
    const char* output = luaL_checkstring(L, -1);
    std::string outPath(output);
    lua_pop(L, 1);

    if (!Hydro::API::addTrace(target, seconds, outPath)) {
        return luaL_error(L, "Reflect.trace: failed to install (check log)");
    }

    lua_createtable(L, 0, 3);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "ok");
    lua_pushinteger(L, seconds);
    lua_setfield(L, -2, "seconds");
    lua_pushlightuserdata(L, target);
    lua_setfield(L, -2, "target");
    return 1;
}

// dump(outputDir [, format]) - walks GUObjectArray ONCE, dispatches each
// object into classes, structs, enums, or the asset list based on its
// metaclass and path. format defaults to "json"; pass "text" for legacy
// human-readable output. After the walk, writes _dump_complete.json with
// summary stats. The single-pass design means iteration cost scales
// linearly with GUObjectArray size (917k on real games) instead of 3x.
static int l_dump(lua_State* L) {
    const char* outputDir = luaL_checkstring(L, 1);
    const char* format = (lua_gettop(L) >= 2 && lua_isstring(L, 2)) ? lua_tostring(L, 2) : "json";
    bool asJson = (strcmp(format, "text") != 0);
    const char* ext = asJson ? ".json" : ".txt";

    namespace fs = std::filesystem;
    fs::path outRoot(outputDir);
    fs::create_directories(outRoot);

    // Resolve the three kind-metaclasses up front. A UObject belongs to
    // one "kind" (class / struct / enum / asset) based on its ClassPrivate:
    //   - ClassPrivate == classMetaclass  -> it IS a UClass
    //   - ClassPrivate == structMetaclass -> it IS a UScriptStruct
    //   - ClassPrivate == enumMetaclass   -> it IS a UEnum
    //   - otherwise                       -> it's a regular instance;
    //                                        check path prefix for /Game/
    void* classMetaclass  = findClassMetaclass();
    void* structMetaclass = asJson ? Engine::findObject(L"/Script/CoreUObject.ScriptStruct") : nullptr;
    void* enumMetaclass   = asJson ? Engine::findObject(L"/Script/CoreUObject.Enum")         : nullptr;

    uint32_t functionNameIdx = Engine::makeFName(L"Function");
    auto startTick = std::chrono::steady_clock::now();

    // Classes need a two-pass approach because we walk SuperStruct chains
    // to pull in parent classes that may not be instantiated directly.
    // Structs/enums/assets don't need that - they're emitted inline.
    std::unordered_set<void*> classSet;
    int assetsWritten = 0;
    int structsWritten = 0;
    int enumsWritten = 0;
    int skipped = 0;

    // Open the assets stream once - only written in JSON mode.
    std::ofstream assetsFile;
    if (asJson) {
        assetsFile.open(outRoot / "_assets.jsonl");
    }

    // -- Single pass over GUObjectArray ---
    int32_t total = Engine::getObjectCount();
    int rawCount = 0;
    for (int32_t i = 0; i < total; i++) {
        void* obj = Engine::getObjectAt(i);
        if (!obj) continue;
        void* objCls = Engine::getObjClass(obj);
        if (!objCls) continue;
        rawCount++;

        // Route 1: obj's class is the "Class" metaclass -> obj is a UClass
        if (classMetaclass && objCls == classMetaclass) {
            classSet.insert(obj);
            continue;
        }

        // For struct/enum/asset routes we're only interested in JSON output.
        if (!asJson) continue;

        // Route 2: UScriptStruct -> emit struct JSON inline
        if (structMetaclass && objCls == structMetaclass) {
            std::string name;
            try { name = Engine::getObjectName(obj); } catch (...) { continue; }
            if (name.empty() || name[0] == '<' || name == "None") continue;
            std::string path;
            try { path = Engine::getObjectPath(obj); } catch (...) { continue; }
            std::string relPath = classPathToRelPath(path);
            if (relPath.empty()) continue;
            fs::path filePath = outRoot / (relPath + ext);
            try { fs::create_directories(filePath.parent_path()); } catch (...) { continue; }
            try {
                std::ofstream file(filePath);
                if (file.is_open()) {
                    writeStructJson(file, obj, name, path);
                    structsWritten++;
                }
            } catch (...) {}
            continue;
        }

        // Route 3: UEnum -> emit enum JSON inline
        if (enumMetaclass && objCls == enumMetaclass) {
            std::string name;
            try { name = Engine::getObjectName(obj); } catch (...) { continue; }
            if (name.empty() || name[0] == '<' || name == "None") continue;
            std::string path;
            try { path = Engine::getObjectPath(obj); } catch (...) { continue; }
            std::string relPath = classPathToRelPath(path);
            if (relPath.empty()) continue;
            fs::path filePath = outRoot / (relPath + ext);
            try { fs::create_directories(filePath.parent_path()); } catch (...) { continue; }
            try {
                std::ofstream file(filePath);
                if (file.is_open()) {
                    writeEnumJson(file, obj, name, path);
                    enumsWritten++;
                }
            } catch (...) {}
            continue;
        }

        // Route 4: /Game/ asset -> append to _assets.jsonl
        if (!assetsFile.is_open()) continue;
        std::string path;
        try { path = Engine::getObjectPath(obj); } catch (...) { continue; }
        if (path.empty() || path.rfind("/Game/", 0) != 0) continue;
        if (path.find("/Transient") != std::string::npos) continue;
        if (path.find("<FName") != std::string::npos) continue;

        std::string className;
        try { className = Engine::getObjectName(objCls); } catch (...) {}
        if (className.empty() || className[0] == '<') continue;

        std::string parentName;
        void* super = Engine::getSuper(obj);
        if (super) {
            try { parentName = Engine::getObjectName(super); } catch (...) {}
        }

        assetsFile << "{\"path\":";
        writeJsonString(assetsFile, path);
        assetsFile << ",\"className\":";
        writeJsonString(assetsFile, className);
        if (!parentName.empty() && parentName[0] != '<') {
            assetsFile << ",\"parentClass\":";
            writeJsonString(assetsFile, parentName);
        }
        assetsFile << "}\n";
        assetsWritten++;
    }

    if (assetsFile.is_open()) assetsFile.close();

    // -- Walk class SuperStruct chains ---
    // Parents of live classes that may not themselves have instances in
    // GUObjectArray. Walking super chains here instead of at insertion
    // time avoids modifying classSet while iterating the array.
    std::vector<void*> toWalk(classSet.begin(), classSet.end());
    for (void* cls : toWalk) {
        void* super = Engine::getSuper(cls);
        int depth = 0;
        while (super && depth < 32) {
            if (classMetaclass && Engine::getObjClass(super) != classMetaclass) break;
            classSet.insert(super);
            super = Engine::getSuper(super);
            depth++;
        }
    }

    Hydro::logInfo("[Hydro.Reflect] Dumping %zu validated classes (from %d raw) to '%s' (%s)...",
                   classSet.size(), rawCount, outputDir, asJson ? "JSON" : "text");

    // -- Emit class files ---
    int filesWritten = 0;
    for (void* cls : classSet) {
        std::string className;
        try { className = Engine::getObjectName(cls); } catch (...) { continue; }
        if (className.empty() || className[0] == '<' || className == "None") continue;

        std::string classPath;
        try { classPath = Engine::getObjectPath(cls); } catch (...) { continue; }

        std::string relPath = classPathToRelPath(classPath);
        if (relPath.empty()) { skipped++; continue; }

        fs::path filePath = outRoot / (relPath + ext);
        try { fs::create_directories(filePath.parent_path()); } catch (...) { skipped++; continue; }

        try {
            std::ofstream file(filePath);
            if (!file.is_open()) { skipped++; continue; }
            if (asJson) {
                writeClassJson(file, cls, className, classPath, functionNameIdx);
            } else {
                writeClassText(file, cls, className, classPath, functionNameIdx);
            }
            filesWritten++;
        } catch (...) {
            skipped++;
            continue;
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTick).count();

    // Write completion sentinel - hydro analyze uses this to verify the
    // dump finished cleanly vs. the game crashing mid-dump.
    if (asJson) {
        try {
            std::ofstream sentinel(outRoot / "_dump_complete.json");
            if (sentinel.is_open()) {
                auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                char tbuf[64] = {};
                std::strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
                sentinel << "{\n";
                sentinel << "  \"schemaVersion\": 1,\n";
                sentinel << "  \"finishedAt\": \"" << tbuf << "\",\n";
                sentinel << "  \"elapsedMs\": " << elapsed << ",\n";
                sentinel << "  \"counts\": {\n";
                sentinel << "    \"classes\": " << filesWritten << ",\n";
                sentinel << "    \"structs\": " << structsWritten << ",\n";
                sentinel << "    \"enums\": " << enumsWritten << ",\n";
                sentinel << "    \"assets\": " << assetsWritten << ",\n";
                sentinel << "    \"skipped\": " << skipped << ",\n";
                sentinel << "    \"totalValidated\": " << classSet.size() << ",\n";
                sentinel << "    \"rawGuObjectArray\": " << rawCount << "\n";
                sentinel << "  }\n";
                sentinel << "}\n";
            }
        } catch (...) {}
    }

    Hydro::logInfo("[Hydro.Reflect] Dump complete: %d classes, %d structs, %d enums, %d assets, %d skipped, %lldms",
                   filesWritten, structsWritten, enumsWritten, assetsWritten, skipped, (long long)elapsed);

    lua_createtable(L, 0, 6);
    lua_pushinteger(L, classSet.size());
    lua_setfield(L, -2, "classes");
    lua_pushinteger(L, filesWritten);
    lua_setfield(L, -2, "files");
    lua_pushinteger(L, structsWritten);
    lua_setfield(L, -2, "structs");
    lua_pushinteger(L, enumsWritten);
    lua_setfield(L, -2, "enums");
    lua_pushinteger(L, assetsWritten);
    lua_setfield(L, -2, "assets");
    lua_pushinteger(L, (lua_Integer)elapsed);
    lua_setfield(L, -2, "elapsedMs");
    return 1;
}

// tickDump: no-op now that dump is synchronous with pool reading.
void tickDump() {}

// Module registration

static const luaL_Reg reflect_functions[] = {
    {"findClass",  l_findClass},
    {"functions",  l_functions},
    {"properties", l_properties},
    {"inspect",    l_inspect},
    {"findAll",    l_findAll},
    {"hierarchy",  l_hierarchy},
    {"assets",     l_assets},
    {"dump",       l_dump},
    {"trace",      l_trace},
    {nullptr,      nullptr}
};

void registerReflectModule(lua_State* L) {
    buildModuleTable(L, reflect_functions);
}

} // namespace Hydro::API
