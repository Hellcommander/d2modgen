/*
 * Copyright (C) 2026 Smirnov Vladimir / mapron1@gmail.com
 * SPDX-License-Identifier: BSL-1.0
 * See LICENSE file for details.
 */
#pragma once

#include "MernelPlatform/PropertyTree.hpp"
#include "MernelPlatform/FsUtils.hpp"

#include "GenerationEnvironment.hpp"

#include "ModgenCoreExport.hpp"
#include "Config.hpp"
#include "AppConfig.hpp"

namespace D2ModGen {
class StorageCache;
class MODGENCORE_EXPORT ConfigHandler {
public:
    explicit ConfigHandler();
    ConfigHandler(const ConfigHandler&) = delete;
    ~ConfigHandler();

    bool loadAppConfig();
    bool saveAppConfig() const;

    bool loadConfig(const Mernel::std_path& filename);
    bool saveConfig(const Mernel::std_path& filename) const;

    bool loadConfig(const Mernel::PropertyTree& data);
    bool saveConfig(Mernel::PropertyTree& data) const;

    void updateSeeds(bool forceUpdate);

    bool generate(const GenerationEnvironment& env, std::string& crc);

    // Mod library: every generated mod keeps the settings that made it (d2modgen.json in its folder,
    // next to <name>.mpq, where neither the game nor mod managers read it), so it can be opened again
    // for editing. loadConfig(tree) also accepts that file.
    static constexpr std::string_view s_modSettingsName   = "d2modgen.json";
    static constexpr std::string_view s_modSettingsFormat = "d2modgen-mod/1";

    struct ModEntry {
        std::string      name;
        Mernel::std_path folder;
        bool             hasSettings = false; // made (or saved) by this version: can be opened
    };
    // Where a mod's d2modgen.json goes: <output>/mods/<name> for D2R mods, else the output folder.
    static Mernel::std_path modFolder(const GenerationEnvironment& env);
    // D2R mods in <output>/mods (folders holding <name>.mpq or a d2modgen.json), sorted by name.
    static std::vector<ModEntry> listMods(const GenerationEnvironment& env);

    // The current settings plus the app settings that shape the mod, as stored in d2modgen.json.
    void makeModSettings(Mernel::PropertyTree& data, const std::string& crc) const;
    bool saveModSettings(const Mernel::std_path& folder, const std::string& crc) const;
    // Loads the settings saved with a mod (its folder or the d2modgen.json itself): the settings, the
    // mod id (from the file, else the folder name), game version and language, and, when the folder
    // sits in a "mods" folder, the output folder, so the next generate rewrites that mod.
    // `message` says what was loaded or what is wrong.
    bool openMod(const Mernel::std_path& folderOrFile, std::string& message);

    AppConfig m_appConfig;

    Config m_config;

    const std::unique_ptr<StorageCache> m_mainStorageCache;

    const Mernel::std_path m_appData;
    const Mernel::std_path m_defaultPath;
};

}
