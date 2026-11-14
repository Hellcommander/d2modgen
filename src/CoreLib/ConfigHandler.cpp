/*
 * Copyright (C) 2026 Smirnov Vladimir / mapron1@gmail.com
 * SPDX-License-Identifier: BSL-1.0
 * See LICENSE file for details.
 */
#include "ConfigHandler.hpp"

#include "DataContext.hpp"
#include "FileIOUtils.hpp"
#include "Logger.hpp"

#include "MernelPlatform/ScopeExit.hpp"
#include "MernelPlatform/ChronoPoint.hpp"

#include "Modules/ModuleGlobal.hpp"

#include "Storage/StorageCache.hpp"
#include "Storage/FolderStorage.hpp"

#include "MernelPlatform/FileFormatJson.hpp"

#include "MernelPlatform/AppLocations.hpp"

#include <algorithm>
#include <random>
#include <format>

namespace D2ModGen {

ConfigHandler::ConfigHandler()
    : m_mainStorageCache(std::make_unique<StorageCache>())
    , m_appData(Mernel::AppLocations("D2R mod generator").getAppdataDir())
    , m_defaultPath(m_appData / "config.json")
{
}

bool ConfigHandler::loadAppConfig()
{
    const Mernel::std_path filename = m_appData / "app.json";
    Logger() << "Read: " << path2string(filename);
    m_appConfig.resetToZero();

    std::string          buffer;
    Mernel::PropertyTree data;
    if (!Mernel::readFileIntoBufferNoexcept((filename), buffer) || !readJsonFromBufferNoexcept(buffer, data)) {
        return false;
    }

    m_appConfig.readFromProperty(data);
    return true;
}

bool ConfigHandler::saveAppConfig() const
{
    const Mernel::std_path filename = m_appData / "app.json";
    Logger() << "Write: " << path2string(filename);

    if (!createDirectoriesForFile((filename))) {
        Logger(Logger::Err) << "Failed to create directories for: " << path2string(filename);
        return false;
    }

    Mernel::PropertyTree data;
    m_appConfig.writeToProperty(data);

    std::string buffer;
    Mernel::writeJsonToBufferNoexcept(buffer, data, true);
    if (!Mernel::writeFileFromBufferNoexcept(filename, buffer)) {
        Logger(Logger::Err) << "Failed to write: " << path2string(filename);
        return false;
    }
    return true;
}

bool ConfigHandler::loadConfig(const Mernel::std_path& filename)
{
    Logger() << "Read: " << path2string(filename);

    std::string          buffer;
    Mernel::PropertyTree data;
    if (!Mernel::readFileIntoBufferNoexcept((filename), buffer) || !readJsonFromBufferNoexcept(buffer, data)) {
        return false;
    }

    return loadConfig(data);
}

bool ConfigHandler::saveConfig(const Mernel::std_path& filename) const
{
    Logger() << "Write: " << path2string(filename);
    Mernel::PropertyTree data;
    data.convertToMap();
    saveConfig(data);
    if (!createDirectoriesForFile((filename))) {
        Logger(Logger::Err) << "Failed to create directories for: " << path2string(filename);
        return false;
    }

    std::string buffer;
    Mernel::writeJsonToBufferNoexcept(buffer, data, true);
    if (!Mernel::writeFileFromBufferNoexcept((filename), buffer)) {
        Logger(Logger::Err) << "Failed to write: " << path2string(filename);
        return false;
    }
    return true;
}

bool ConfigHandler::loadConfig(const Mernel::PropertyTree& data)
{
    // a mod's d2modgen.json wraps the settings: { "format": ..., "app": {...}, "config": {...} }
    const bool isModSettings = data.isMap() && data.contains("format") && data["format"].isScalar()
                               && data["format"].getScalar().toString() == s_modSettingsFormat;
    if (isModSettings && data.contains("config"))
        m_config.readFromProperty(data["config"]);
    else
        m_config.readFromProperty(data);
    updateSeeds(false);

    m_config.notifyChangeInner();

    return true;
}

bool ConfigHandler::saveConfig(Mernel::PropertyTree& data) const
{
    data.convertToMap();
    m_config.writeToProperty(data);
    return true;
}

void ConfigHandler::updateSeeds(bool forceUpdate)
{
    std::random_device rd;
    if (m_config.m_itemRandomizer.m_seed == 0 || forceUpdate) {
        m_config.m_itemRandomizer.m_seed = rd();
        m_config.m_itemRandomizer.notifyChangeInner();
    }
    if (m_config.m_monsterRandomizer.m_seed == 0 || forceUpdate) {
        m_config.m_monsterRandomizer.m_seed = rd();
        m_config.m_monsterRandomizer.notifyChangeInner();
    }
    if (m_config.m_skillRandomizer.m_seed == 0 || forceUpdate) {
        m_config.m_skillRandomizer.m_seed = rd();
        m_config.m_skillRandomizer.notifyChangeInner();
    }
    if (m_config.m_globalSeed == 0 || forceUpdate) {
        m_config.m_globalSeed = rd();
        m_config.notifyChangeInner();
    }
}

bool ConfigHandler::generate(const GenerationEnvironment& env, std::string& crc)
{
    auto startAll = Mernel::ChronoPoint(true);
    updateSeeds(m_appConfig.m_refreshSeed);

    FolderStorage outDebugStorage(m_appData / "debug");
    FolderStorage outStorage(env.outputPath, env.outputMode);
    outStorage.setModName(env.modName);
    outStorage.setSilentTrace(false);

    DataContext previousData;
    previousData.useBaseTable = env.needDataSubfolder;

    Logger(Logger::Notice) << "started in: " << path2string(env.outputPath);

    IStorage::StoredData previousStoredData = outStorage.readData();

    if (previousStoredData.valid) {
        previousData.parseData(previousStoredData, false);
    }

    DataContext output;
    output.useBaseTable = env.needDataSubfolder;
    output.silent       = false;

    DataContext initialOutput;

    Logger() << "Loading data from main storage...";
    const IStorage::StoredData inData = m_mainStorageCache->load({ env.inputMode, env.inputPath });
    if (!inData.valid) {
        Logger(Logger::Err) << "Failed to read data files from D2 folder.";
        return false;
    }
    Logger(Logger::Info) << "Load files from game storage time: " << startAll.GetElapsedTime().ToProfilingTime();

    Logger() << "Parsing json and txt...";
    if (!output.parseData(inData)) {
        Logger(Logger::Err) << "Failed parse D2 data files.";
        return false;
    }
    if (env.extraTrace.contains("dump_diff")) {
        initialOutput = output.makeSilent();
    }
    if (env.extraTrace.contains("dump_in")) {
        outDebugStorage.setFileSuffix(".in");
        if (outDebugStorage.create())
            outDebugStorage.writeMainData(output.makeSilent().writeDebug());
    }

    Logger() << "prepare ended; Starting generate phase.";

    {
        auto start = Mernel::ChronoPoint(true);

        ConfigHasher hasher;
        m_config.calchash(hasher);
        crc = std::format("{:08X}", hasher.hashv);

        ModuleGlobal all(crc, output, m_config, env);
        try {
            all.generate();
        }
        catch (const std::exception& ex) {
            Logger(Logger::Err) << ex.what();
            return false;
        }

        Logger(Logger::Info) << "generate logic finished in: " << start.GetElapsedTime().ToProfilingTime();
    }

    if (env.extraTrace.contains("dump_out")) {
        outDebugStorage.setFileSuffix(".out");
        if (outDebugStorage.create())
            outDebugStorage.writeMainData(output.makeSilent().writeDebug());
    }

    if (env.extraTrace.contains("dump_diff")) {
        outDebugStorage.setFileSuffix(".diff");
        if (outDebugStorage.create())
            outDebugStorage.writeMainData(output.calculateDiff(initialOutput).makeSilent().writeDebug());
    }

    IStorage::StoredData outData;
    if (!output.serializeData(outData))
        return false;

    if (output.useBaseTable) {
        // if we target pre-RotW, still generate copy of monstats tables in excel/. Need to use new column layout, that's why we read them again.
        IStorage::StoredData outDataCopy;
        DataContext          outputCopy;
        outputCopy.useBaseTable = false; // switch to tablesMain read/write
        outputCopy.parseData(inData);
        outputCopy.monstats.rows   = output.monstats.rows;
        outputCopy.monstatsEx.rows = output.monstatsEx.rows;

        outputCopy.silent = true;
        if (!outputCopy.serializeData(outDataCopy))
            return false;

        outData.tablesMain.clear();
        outData.tablesMain[TableId::monstats]  = outDataCopy.tablesMain[TableId::monstats];
        outData.tablesMain[TableId::monstats2] = outDataCopy.tablesMain[TableId::monstats2];
    }

    if (!outStorage.create())
        return false;

    if (env.extraTrace.contains("dump_bmp")) {
        FolderStorage outDebugStorageBmp(m_appData / "debugBitmaps");
        if (outDebugStorageBmp.create())
            outDebugStorageBmp.writeBitmaps(outData);
    }

    if (!outStorage.writeMainData(outData) || !outStorage.writeMeta(outData))
        return false;

    // Keep the settings with the mod so it can be opened for editing later.
    if (env.outputMode != StorageType::None && !saveModSettings(modFolder(env), crc))
        Logger(Logger::Warning) << "The mod is written, but its settings could not be saved next to it.";

    Logger(Logger::Info) << "Total time: " << startAll.GetElapsedTime().ToProfilingTime();
    return true;
}

Mernel::std_path ConfigHandler::modFolder(const GenerationEnvironment& env)
{
    if (env.outputMode == StorageType::D2ResurrectedModFolder)
        return env.outputPath / "mods" / Mernel::string2path(env.modName);
    return env.outputPath;
}

std::vector<ConfigHandler::ModEntry> ConfigHandler::listMods(const GenerationEnvironment& env)
{
    std::vector<ModEntry> result;
    if (env.outputMode != StorageType::D2ResurrectedModFolder)
        return result;
    const auto      mods = env.outputPath / "mods";
    std::error_code ec;
    if (!Mernel::std_fs::is_directory(mods, ec))
        return result;
    for (const auto& it : Mernel::std_fs::directory_iterator(mods, ec)) {
        if (!it.is_directory(ec))
            continue;
        ModEntry entry;
        entry.folder      = it.path();
        entry.name        = Mernel::path2string(it.path().filename());
        entry.hasSettings = Mernel::std_fs::is_regular_file(entry.folder / s_modSettingsName, ec);
        const bool isMod  = Mernel::std_fs::is_directory(entry.folder / Mernel::string2path(entry.name + ".mpq"), ec);
        if (entry.hasSettings || isMod)
            result.push_back(std::move(entry));
    }
    std::sort(result.begin(), result.end(), [](const ModEntry& l, const ModEntry& r) { return l.name < r.name; });
    return result;
}

void ConfigHandler::makeModSettings(Mernel::PropertyTree& data, const std::string& crc) const
{
    data = {};
    data.convertToMap();
    data["format"] = Mernel::PropertyTreeScalar(std::string(s_modSettingsFormat));

    Mernel::PropertyTree app;
    app.convertToMap();
    app["modname"]      = Mernel::PropertyTreeScalar(m_appConfig.m_modname);
    app["versionMajor"] = Mernel::PropertyTreeScalar(m_appConfig.m_versionMajor);
    app["versionD2R"]   = Mernel::PropertyTreeScalar(m_appConfig.m_versionD2R);
    app["d2rLocaleId"]  = Mernel::PropertyTreeScalar(m_appConfig.m_d2rLocaleId);
    data["app"]         = std::move(app);
    if (!crc.empty())
        data["crc"] = Mernel::PropertyTreeScalar(crc);

    Mernel::PropertyTree config;
    saveConfig(config);
    data["config"] = std::move(config);
}

bool ConfigHandler::saveModSettings(const Mernel::std_path& folder, const std::string& crc) const
{
    Mernel::PropertyTree data;
    makeModSettings(data, crc);
    const auto  filename = folder / s_modSettingsName;
    std::string buffer;
    Mernel::writeJsonToBufferNoexcept(buffer, data, true);
    if (!createDirectoriesForFile(filename) || !Mernel::writeFileFromBufferNoexcept(filename, buffer)) {
        Logger(Logger::Err) << "Failed to write: " << path2string(filename);
        return false;
    }
    Logger() << "Mod settings saved: " << path2string(filename);
    return true;
}

bool ConfigHandler::openMod(const Mernel::std_path& folderOrFile, std::string& message)
{
    std::error_code ec;
    const bool      isFile = Mernel::std_fs::is_regular_file(folderOrFile, ec);
    auto            folder = isFile ? folderOrFile.parent_path() : folderOrFile;
    // picked mods/<name>/<name>.mpq instead of mods/<name>: the settings sit one level up
    if (!isFile && !Mernel::std_fs::exists(folder / s_modSettingsName, ec) && Mernel::std_fs::exists(folder.parent_path() / s_modSettingsName, ec))
        folder = folder.parent_path();
    const auto filename = isFile ? folderOrFile : folder / s_modSettingsName;

    std::string          buffer;
    Mernel::PropertyTree data;
    if (!Mernel::std_fs::is_regular_file(filename, ec)) {
        message = "No " + std::string(s_modSettingsName) + " in " + path2string(folder)
                  + ": the mod was not made (or not yet regenerated) by a d2modgen that saves its settings.";
        return false;
    }
    if (!Mernel::readFileIntoBufferNoexcept(filename, buffer) || !Mernel::readJsonFromBufferNoexcept(buffer, data) || !data.isMap()) {
        message = "Could not read " + path2string(filename);
        return false;
    }

    loadConfig(data);

    std::string name = Mernel::path2string(folder.filename());
    if (data.contains("app") && data["app"].isMap()) {
        const auto& app = data["app"];
        name            = app.value("modname", Mernel::PropertyTreeScalar(name)).toString();
        app.value("versionMajor", Mernel::PropertyTreeScalar(m_appConfig.m_versionMajor)).convertTo(m_appConfig.m_versionMajor);
        app.value("versionD2R", Mernel::PropertyTreeScalar(m_appConfig.m_versionD2R)).convertTo(m_appConfig.m_versionD2R);
        app.value("d2rLocaleId", Mernel::PropertyTreeScalar(m_appConfig.m_d2rLocaleId)).convertTo(m_appConfig.m_d2rLocaleId);
    }
    m_appConfig.m_modname = name;

    // <root>/mods/<name>: write the next generate to that same root (a game folder, or a mod
    // manager's folder such as D2RMM's).
    std::string where;
    if (!isFile || folder.filename() == Mernel::string2path(name)) {
        const auto parent = folder.parent_path();
        if (parent.filename() == "mods") {
            const auto root          = parent.parent_path();
            const auto inputPath     = Mernel::string2path(m_appConfig.m_inputPath);
            const bool sameAsGame    = !m_appConfig.m_inputPath.empty() && Mernel::std_fs::equivalent(root, inputPath, ec);
            m_appConfig.m_outputMode = static_cast<int64_t>(GenerationEnvironment::OutputMode::D2RMod);
            m_appConfig.m_outputPath = sameAsGame ? std::string() : ensureTrailingSlash(path2string(root));
            where                    = path2string(root);
        }
    }
    m_appConfig.validateAndClamp();
    m_appConfig.notifyChangeInner();

    message = "Opened mod '" + name + "'" + (where.empty() ? std::string() : " in " + where) + "; generate to update it.";
    return true;
}

ConfigHandler::~ConfigHandler() = default;

}
