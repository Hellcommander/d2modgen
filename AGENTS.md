# Project Instructions

## Build & Verification
- Standard build: `cmake -B build && cmake --build build -j`
- Build with config codegen (required after editing `Config.yaml`): `cmake -B build -DENABLE_SCHEMA_GEN=ON && cmake --build build -j`
- Run tests: `ctest --test-dir build --output-on-failure`
- Format code: `clang-format -i --style=file:src/.clang-format <modified-files>`

## Configuration Schema & Codegen
- **Single source of truth:** `src/Config/Config.yaml` defines all module configuration structs, member names, defaults, min/max bounds, and presets.
- **Adding or changing settings:** Edit `src/Config/Config.yaml`, then rebuild with `-DENABLE_SCHEMA_GEN=ON` to update the tracked C++/Qt config files.
- **Naming mapping:** A YAML path like `structs -> RandoItems -> members -> enabled` (mounted under root `itemRandomizer`) maps to `config.m_itemRandomizer.m_enabled` in C++ and `config.itemRandomizer.enabled` in QML.
- **Never edit generated files:** Even though generated C++/Qt config files are committed to git for indexing, never modify them directly.

- **Setting help for tools:** `src/Config/SettingsHelp.hpp` (captions and tooltips per setting path, used by `modgen_console describe`) is generated from `qml/Modules/*.qml` by `src/Config/update_help.py`; rerun it (or build with `-DENABLE_SCHEMA_GEN=ON`) after changing captions or tooltips, and never edit it by hand.

## Using the generator (agents making mods)
- Use `modgen_console` commands (`describe`, `new`, `set`, `show`, `generate`); see `docs/agents.md`. They print JSON on stdout and never touch the UI's own settings.
- A generated mod keeps its settings in `mods/<name>/d2modgen.json`; `ConfigHandler::openMod` loads them back (File -> Open mod in the UI).

## Architecture & Mapping
- Core C++ generator logic lives in `src/CoreLib/Modules/`.
- UI configurations live in `qml/Modules/` using the naming convention `Module-<Name>.qml`.
- Whenever you add or modify a module in `src/CoreLib/Modules/`, update or create its corresponding `qml/Modules/Module-<Name>.qml` file.

## Conventions & Boundaries
- Format all modified C++ headers and source files in `src/` using `src/.clang-format` before finishing a task.
- Do not modify `src/.clang-format` or manually alter formatting to override it.
- Keep C++ logic separated from QML UI definitions.