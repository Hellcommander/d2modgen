/*
 * Copyright (C) 2026 Smirnov Vladimir / mapron1@gmail.com
 * SPDX-License-Identifier: BSL-1.0
 * See LICENSE file for details.
 */

#include "ConfigHandler.hpp"
#include "FileIOUtils.hpp"
#include "SettingsHelp.hpp"

#include "Logger.hpp"
#include "MernelPlatform/FileFormatJson.hpp"
#include "MernelPlatform/Logger_details.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>

namespace D2ModGen {
namespace {

const char* const s_usage = R"(modgen_console - D2R mod generator without the UI.

  modgen_console [-v]          generate the mod set up in the UI (its saved settings), as before

Mod library commands (made for scripts and AI agents; JSON on stdout, logs on stderr,
exit code 0 = ok, 1 = error with {"ok": false, "error": ..., "hint": ...}):

  describe [--mod M] [--filter TEXT] [--changed]
        every setting: path, type, value, default, recommended, min/max, presets, caption, help;
        --changed lists only settings that differ from the default (the plain game)
  presets                               preset names for --preset
  list-mods                             mods in the output folder (<output>/mods)
  show --mod M                          a mod's game version and the settings it changes
  new NAME [--preset P] [PATH=VALUE ...] [--force] [--generate]
        start a mod: settings from the preset (default "none" = everything off), then the
        values given; saved as <output>/mods/NAME/d2modgen.json
  set --mod M PATH=VALUE [PATH=VALUE ...] [--generate]
        change settings of a mod (all are checked before anything is written)
  preset --mod M P [PATH=VALUE ...] [--generate]
        replace a mod's settings with a preset (then the values given)
  get --mod M PATH                      one setting
  generate --mod M                      write the mod's game files from its saved settings

  M is a mod name (looked up in <output>/mods) or the path of a mod folder / its d2modgen.json.
  PATH is a dotted setting path from describe, e.g. balanceMonsters.monsterHealth=200.

Options:
  --game DIR     D2R install folder (default: the one set in the UI)
  --output DIR   folder that gets mods/<name>/ (default: the game folder; a mod manager's
                 folder works too, e.g. D2RMM's, which then lists the mod as a data mod)
  --lod | --rotw target the D2R LoD or Reign of the Warlock tables (default: as set in the UI)
  -v             verbose log on stderr

See docs/agents.md for a walk-through.
)";

struct Args {
    std::string                                      command;
    std::vector<std::string>                         positional;
    std::vector<std::pair<std::string, std::string>> assignments;
    std::map<std::string, std::string>               options;
    std::set<std::string>                            flags;

    std::optional<std::string> option(const std::string& name) const
    {
        auto it = options.find(name);
        if (it == options.end())
            return std::nullopt;
        return it->second;
    }
};

class Failure : public std::runtime_error {
public:
    Failure(const std::string& error, std::string hint = {})
        : std::runtime_error(error)
        , m_hint(std::move(hint))
    {}
    std::string m_hint;
};

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

Mernel::PropertyTreeScalar scalar(std::string_view text)
{
    return Mernel::PropertyTreeScalar(std::string(text));
}

// a path as text with the platform's separators only (the app config mixes / and \)
Mernel::PropertyTreeScalar pathScalar(Mernel::std_path path)
{
    return scalar(path2string(path.lexically_normal().make_preferred()));
}

void printJson(const Mernel::PropertyTree& data)
{
    // compact: the pretty writer does not escape quotes inside strings
    std::string buffer;
    Mernel::writeJsonToBufferNoexcept(buffer, data, false);
    std::cout << buffer << std::endl;
}

Mernel::PropertyTree okResult()
{
    Mernel::PropertyTree result;
    result.convertToMap();
    result["ok"] = Mernel::PropertyTreeScalar(true);
    return result;
}

Args parseArgs(const std::vector<std::string>& argv)
{
    static const std::set<std::string> withValue{ "--mod", "--preset", "--game", "--output", "--filter" };
    static const std::set<std::string> flags{ "--generate", "--force", "--changed", "--lod", "--rotw", "-v" };
    Args                               args;
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& a = argv[i];
        if (withValue.contains(a)) {
            if (i + 1 >= argv.size())
                throw Failure(a + " needs a value");
            args.options[a] = argv[++i];
        } else if (flags.contains(a)) {
            args.flags.insert(a);
        } else if (a.starts_with("--")) {
            throw Failure("unknown option " + a, "run: modgen_console help");
        } else if (args.command.empty()) {
            args.command = a;
        } else if (auto eq = a.find('='); eq != std::string::npos) {
            args.assignments.emplace_back(a.substr(0, eq), a.substr(eq + 1));
        } else {
            args.positional.push_back(a);
        }
    }
    return args;
}

const SettingHelp* findHelp(std::string_view path)
{
    for (const auto& help : s_settingsHelp)
        if (help.path == path)
            return &help;
    return nullptr;
}

class Cli {
public:
    explicit Cli(const Args& args)
        : m_args(args)
    {
        m_handler.loadAppConfig();
        auto& app = m_handler.m_appConfig;
        if (auto game = m_args.option("--game"))
            app.m_inputPath = ensureTrailingSlash(*game);
        if (auto output = m_args.option("--output")) {
            app.m_outputPath = ensureTrailingSlash(*output);
            app.m_outputMode = static_cast<int64_t>(GenerationEnvironment::OutputMode::D2RMod);
        }
        applyTarget();
    }

    int run()
    {
        const auto& c = m_args.command;
        if (c == "describe")
            return describe();
        if (c == "presets")
            return presets();
        if (c == "list-mods")
            return listMods();
        if (c == "show")
            return show();
        if (c == "new")
            return create();
        if (c == "set")
            return set();
        if (c == "preset")
            return preset();
        if (c == "get")
            return get();
        if (c == "generate")
            return generate();
        throw Failure("unknown command '" + c + "'", "run: modgen_console help");
    }

private:
    void applyTarget()
    {
        auto& app = m_handler.m_appConfig;
        if (m_args.flags.contains("--lod") || m_args.flags.contains("--rotw")) {
            app.m_versionMajor = static_cast<int64_t>(GenerationEnvironment::VersionMajor::D2R);
            app.m_versionD2R   = static_cast<int64_t>(m_args.flags.contains("--lod") ? GenerationEnvironment::VersionD2R::LoD : GenerationEnvironment::VersionD2R::RotW);
        }
    }

    GenerationEnvironment env() const
    {
        GenerationEnvironment env;
        if (!env.init(m_handler.m_appConfig))
            throw Failure("the game folder is not set or not found ('" + m_handler.m_appConfig.m_inputPath + "')",
                          "pass --game <folder with D2R.exe>, or set the input folder in the UI once");
        return env;
    }

    // --mod: a name in <output>/mods, or a path to a mod folder or its d2modgen.json.
    void openMod()
    {
        auto mod = m_args.option("--mod");
        if (!mod)
            throw Failure("this command needs --mod <name or folder>", "list the mods with: modgen_console list-mods");
        std::error_code  ec;
        Mernel::std_path path   = string2path(*mod);
        const bool       isPath = mod->find('/') != std::string::npos || mod->find('\\') != std::string::npos;
        if (!isPath) {
            auto e    = env();
            e.modName = *mod;
            path      = ConfigHandler::modFolder(e);
        }
        std::string message;
        if (!m_handler.openMod(path, message))
            throw Failure(message, "list the mods with: modgen_console list-mods; start one with: modgen_console new <name>");
        applyTarget();
        m_modFolder = Mernel::std_fs::is_regular_file(path, ec) ? path.parent_path() : path;
    }

    // PATH=VALUE pairs; every one is checked before any setting is changed.
    void applyAssignments()
    {
        std::vector<std::string> errors;
        Config                   probe;
        {
            Mernel::PropertyTree current;
            m_handler.m_config.writeToProperty(current);
            probe.readFromProperty(current);
        }
        for (const auto& [path, value] : m_args.assignments) {
            auto error = probe.setByPath(path, value);
            if (!error.empty())
                errors.push_back(error + suggestion(path));
        }
        if (!errors.empty()) {
            std::string all;
            for (auto& e : errors)
                all += (all.empty() ? "" : "; ") + e;
            throw Failure(all, "see the valid paths, types and ranges with: modgen_console describe --filter <word>");
        }
        for (const auto& [path, value] : m_args.assignments)
            m_handler.m_config.setByPath(path, value);
    }

    std::string suggestion(const std::string& path) const
    {
        Mernel::PropertyTree list;
        m_handler.m_config.describeSettings(list);
        const auto               dot  = path.rfind('.');
        const std::string        leaf = lowered(dot == std::string::npos ? path : path.substr(dot + 1));
        std::vector<std::string> hits;
        for (const auto& item : list.getList()) {
            const std::string p = item["path"].getScalar().toString();
            if (!leaf.empty() && lowered(p).find(leaf) != std::string::npos)
                hits.push_back(p);
        }
        if (hits.empty() || hits.size() > 8)
            return {};
        std::string out = " (did you mean ";
        for (size_t i = 0; i < hits.size(); ++i)
            out += (i ? ", " : "") + hits[i];
        return out + "?)";
    }

    void applyPreset(const std::string& name)
    {
        if (name == "none" || name == "vanilla") {
            m_handler.m_config.resetToZero();
        } else if (name == "recommended") {
            m_handler.m_config.resetToRecommended();
        } else {
            const int index = CommonStruct::getPresetIndex(name);
            if (index < 0)
                throw Failure("unknown preset '" + name + "'", "run: modgen_console presets");
            // same as the UI: the author's recommended settings, then the preset's changes
            m_handler.m_config.resetToRecommended();
            m_handler.m_config.activatePreset(index);
        }
    }

    void save()
    {
        if (!m_handler.saveModSettings(m_modFolder, {}))
            throw Failure("could not write " + path2string(m_modFolder / ConfigHandler::s_modSettingsName));
    }

    Mernel::PropertyTree doGenerate()
    {
        auto        e = env();
        std::string crc;
        if (!m_handler.generate(e, crc))
            throw Failure("generation failed", "run again with -v to see the log");
        auto result      = okResult();
        result["mod"]    = scalar(m_handler.m_appConfig.m_modname);
        result["folder"] = pathScalar(ConfigHandler::modFolder(e));
        result["crc"]    = scalar(crc);
        result["launch"] = scalar("D2R.exe -mod " + m_handler.m_appConfig.m_modname + " -txt");
        return result;
    }

    bool isSetting(std::string_view path) const
    {
        Mernel::PropertyTree list;
        m_handler.m_config.describeSettings(list);
        for (const auto& item : list.getList())
            if (item["path"].getScalar().toString() == path)
                return true;
        return false;
    }

    Mernel::PropertyTree settingsList(bool changedOnly, const std::string& filter) const
    {
        Mernel::PropertyTree all, list;
        m_handler.m_config.describeSettings(all);
        list.convertToList();
        const std::string needle = lowered(filter);
        for (auto item : all.getList()) {
            const std::string path = item["path"].getScalar().toString();
            if (changedOnly && item["value"] == item["default"])
                continue;
            if (const auto* help = findHelp(path)) {
                if (!help->caption.empty())
                    item["caption"] = scalar(help->caption);
                if (!help->help.empty())
                    item["help"] = scalar(help->help);
            }
            if (const auto* section = findHelp(path.substr(0, path.rfind('.'))))
                item["section"] = scalar(section->caption);
            if (!needle.empty()) {
                const std::string hay = lowered(path + " " + item.value("caption", scalar("")).toString() + " " + item.value("help", scalar("")).toString());
                if (hay.find(needle) == std::string::npos)
                    continue;
            }
            list.append(std::move(item));
        }
        return list;
    }

    Mernel::PropertyTree appInfo() const
    {
        const auto&          app = m_handler.m_appConfig;
        Mernel::PropertyTree info;
        info.convertToMap();
        info["game"]   = scalar(app.m_versionMajor == 1 ? (app.m_versionD2R == 1 ? "D2R RotW" : "D2R LoD") : "D2 Legacy");
        info["output"] = pathScalar(string2path(app.m_outputPath.empty() ? app.m_inputPath : app.m_outputPath));
        return info;
    }

    int describe()
    {
        const bool withMod = m_args.option("--mod").has_value();
        if (withMod)
            openMod();
        auto result = okResult();
        if (withMod)
            result["mod"] = scalar(m_handler.m_appConfig.m_modname);
        result["settings"] = settingsList(m_args.flags.contains("--changed"), m_args.option("--filter").value_or(""));
        if (!m_args.option("--filter") && !m_args.flags.contains("--changed")) {
            Mernel::PropertyTree sections;
            sections.convertToList();
            for (const auto& help : s_settingsHelp) {
                if (isSetting(help.path))
                    continue;
                Mernel::PropertyTree s;
                s.convertToMap();
                s["path"]    = scalar(help.path);
                s["caption"] = scalar(help.caption);
                if (!help.help.empty())
                    s["help"] = scalar(help.help);
                sections.append(std::move(s));
            }
            result["sections"] = std::move(sections);
        }
        printJson(result);
        return 0;
    }

    int presets()
    {
        auto                 result = okResult();
        Mernel::PropertyTree list;
        list.convertToList();
        list.append(scalar("none"));
        list.append(scalar("recommended"));
        for (const auto& name : CommonStruct::s_presetNames)
            list.append(scalar(name));
        result["presets"] = std::move(list);
        result["notes"]   = scalar("none = every setting off (the plain game); recommended = the author's settings; "
                                   "the others start from recommended and change some values (see 'presets' in describe).");
        printJson(result);
        return 0;
    }

    int listMods()
    {
        auto                 e      = env();
        auto                 result = okResult();
        Mernel::PropertyTree list;
        list.convertToList();
        for (const auto& mod : ConfigHandler::listMods(e)) {
            Mernel::PropertyTree item;
            item.convertToMap();
            item["name"]        = scalar(mod.name);
            item["folder"]      = pathScalar(mod.folder);
            item["hasSettings"] = Mernel::PropertyTreeScalar(mod.hasSettings);
            list.append(std::move(item));
        }
        result["modsFolder"] = pathScalar(e.outputPath / "mods");
        result["mods"]       = std::move(list);
        result["notes"]      = scalar("hasSettings = false: made without saved settings (another tool, or an older d2modgen), so it cannot be opened for editing.");
        printJson(result);
        return 0;
    }

    int show()
    {
        openMod();
        auto result       = okResult();
        result["mod"]     = scalar(m_handler.m_appConfig.m_modname);
        result["folder"]  = pathScalar(m_modFolder);
        result["app"]     = appInfo();
        result["changed"] = settingsList(true, "");
        printJson(result);
        return 0;
    }

    int create()
    {
        if (m_args.positional.size() != 1)
            throw Failure("new needs exactly one mod name", "modgen_console new my_mod --preset recommended");
        const std::string name = m_args.positional[0];
        if (name.find_first_of("\\/:*?\"<>| ") != std::string::npos)
            throw Failure("'" + name + "' cannot be a mod name (no spaces or \\/:*?\"<>|)");
        m_handler.m_appConfig.m_modname = name;
        auto e                          = env();
        m_modFolder                     = ConfigHandler::modFolder(e);
        std::error_code ec;
        if (Mernel::std_fs::exists(m_modFolder, ec) && !m_args.flags.contains("--force"))
            throw Failure("mod '" + name + "' already exists in " + path2string(m_modFolder),
                          "edit it with: modgen_console set --mod " + name + " PATH=VALUE, or pass --force to start it over");
        applyPreset(m_args.option("--preset").value_or("none"));
        m_handler.updateSeeds(true);
        applyAssignments();
        save();
        return finish("created");
    }

    int set()
    {
        openMod();
        if (m_args.assignments.empty())
            throw Failure("set needs PATH=VALUE pairs", "modgen_console set --mod my_mod balanceMonsters.monsterHealth=200");
        applyAssignments();
        save();
        return finish("updated");
    }

    int preset()
    {
        openMod();
        if (m_args.positional.size() != 1)
            throw Failure("preset needs one preset name", "run: modgen_console presets");
        applyPreset(m_args.positional[0]);
        applyAssignments();
        save();
        return finish("updated");
    }

    int get()
    {
        openMod();
        if (m_args.positional.size() != 1)
            throw Failure("get needs one setting path");
        const auto list = settingsList(false, "");
        for (const auto& item : list.getList()) {
            if (item["path"].getScalar().toString() == m_args.positional[0]) {
                auto result       = okResult();
                result["mod"]     = scalar(m_handler.m_appConfig.m_modname);
                result["setting"] = item;
                printJson(result);
                return 0;
            }
        }
        throw Failure("no setting '" + m_args.positional[0] + "'" + suggestion(m_args.positional[0]), "see: modgen_console describe --filter <word>");
    }

    int generate()
    {
        openMod();
        printJson(doGenerate());
        return 0;
    }

    // Kept short for agents: what this call set, and how many settings now differ from the plain
    // game (`show` lists them all).
    int finish(const char* what)
    {
        Mernel::PropertyTree result;
        if (m_args.flags.contains("--generate")) {
            result = doGenerate();
        } else {
            result           = okResult();
            result["mod"]    = scalar(m_handler.m_appConfig.m_modname);
            result["folder"] = pathScalar(m_modFolder);
            result["next"]   = scalar("modgen_console generate --mod " + m_handler.m_appConfig.m_modname);
        }
        result["settings"] = scalar(what);
        if (auto preset = m_args.option("--preset"); preset || m_args.command == "preset")
            result["preset"] = scalar(preset ? *preset : m_args.positional.at(0));
        Mernel::PropertyTree applied;
        applied.convertToMap();
        for (const auto& [path, value] : m_args.assignments) {
            for (const auto& item : settingsList(false, "").getList())
                if (item["path"].getScalar().toString() == path)
                    applied[path] = item["value"];
        }
        result["applied"]      = std::move(applied);
        result["changedCount"] = Mernel::PropertyTreeScalar(static_cast<int64_t>(settingsList(true, "").getList().size()));
        printJson(result);
        return 0;
    }

    const Args&      m_args;
    ConfigHandler    m_handler;
    Mernel::std_path m_modFolder;
};

int legacyGenerate(bool verbose)
{
    Logger::SetLoggerBackend(std::make_unique<Mernel::LoggerBackendConsole>(verbose ? Logger::Debug : Logger::Info, false, false, false, Mernel::LoggerBackendConsole::Type::Cout));

    ConfigHandler configHandler;
    configHandler.loadAppConfig();
    configHandler.loadConfig(configHandler.m_defaultPath);
    GenerationEnvironment env;
    if (!env.init(configHandler.m_appConfig))
        return EXIT_FAILURE;

    std::string crc;
    if (!configHandler.generate(env, crc)) {
        return EXIT_FAILURE;
    }
    configHandler.saveConfig(configHandler.m_defaultPath);
    Logger(Logger::Info) << "Success. CRC: " << crc;
    return EXIT_SUCCESS;
}

Mernel::PropertyTree failureResult(const std::string& error, const std::string& hint)
{
    Mernel::PropertyTree result;
    result.convertToMap();
    result["ok"]    = Mernel::PropertyTreeScalar(false);
    result["error"] = scalar(error);
    if (!hint.empty())
        result["hint"] = scalar(hint);
    return result;
}

}
}

int main(int argc, char* argv[])
{
    using namespace D2ModGen;

    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || (args.size() == 1 && args[0] == "-v"))
        return legacyGenerate(!args.empty());
    if (args[0] == "help" || args[0] == "--help" || args[0] == "-h") {
        std::cout << s_usage;
        return EXIT_SUCCESS;
    }

    try {
        const Args parsed = parseArgs(args);
        // stdout carries only the JSON result; the generator's log goes to stderr.
        Logger::SetLoggerBackend(std::make_unique<Mernel::LoggerBackendConsole>(parsed.flags.contains("-v") ? Logger::Debug : Logger::Warning,
                                                                                false,
                                                                                false,
                                                                                false,
                                                                                Mernel::LoggerBackendConsole::Type::Cerr));
        Cli cli(parsed);
        return cli.run();
    }
    catch (const Failure& failure) {
        printJson(failureResult(failure.what(), failure.m_hint));
    }
    catch (const std::exception& ex) {
        printJson(failureResult(ex.what(), {}));
    }
    return EXIT_FAILURE;
}
