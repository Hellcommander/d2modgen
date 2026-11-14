# Making mods with modgen_console (for scripts and AI agents)

`modgen_console` (next to `modgen.exe`) can create, inspect, change and generate mods without the UI.
Every command prints one JSON object on stdout and nothing else; the generator's log goes to stderr.
Exit code 0 means `"ok": true`; on errors the code is 1 and the object says what is wrong and what to
run instead:

```json
{"ok": false, "error": "'monsterHealth' must be 10..1000, got 5000", "hint": "see the valid paths, types and ranges with: modgen_console describe --filter <word>"}
```

Run without arguments it still does what it always did: generate the mod that is set up in the UI.

## The loop

1. Find the settings that match the request:

   ```
   modgen_console describe --filter rune
   ```

   Each entry: `path` (what you set), `type` (`int`, `bool`, `string`), `value`, `default` (the plain game),
   `recommended`, `min`/`max`, `percent` (the number is a percentage: 100 = unchanged), `presets`
   (`{"crazy": 1000, ...}`), `caption` and `help` (the UI's label and tooltip; for choice settings the help
   lists `Values: 0 = ..., 1 = ...`) and `section` (the UI tab). `describe` without `--filter` returns all
   ~250 settings and the tab descriptions (`sections`).

2. Start a mod. `--preset none` (the default) turns everything off, so only what you set changes the game;
   `recommended` is the author's balanced set, `strong`/`weak`/`crazy`/`pain` start from it.

   ```
   modgen_console new more_runes droprate.runes.enabled=true droprate.runes.zodFactor=50 droprate.runes.countessSpecialRuneMore=true --generate
   ```

   The mod's settings are saved as `<output>/mods/more_runes/d2modgen.json`; `--generate` also writes the
   game files (`<output>/mods/more_runes/more_runes.mpq/`).

3. Adjust and regenerate:

   ```
   modgen_console set --mod more_runes balanceMonsters.monsterHealth=150 balanceMonsters.monsterDamage=120 --generate
   modgen_console show --mod more_runes          # everything this mod changes, with captions
   modgen_console get --mod more_runes balanceMonsters.monsterHealth
   ```

   `set` checks every `PATH=VALUE` (name, type, range) before it writes anything; an unknown name gets
   suggestions (`did you mean balanceMonsters.monsterHealth?`). Results stay short: `applied` (what this call
   set) and `changedCount`; `show` lists all changed settings.

4. Play: the result's `launch` field has the command line (`D2R.exe -mod more_runes -txt`).

Other commands: `presets`, `list-mods` (mods in the output folder; `hasSettings: false` means made without
saved settings, so not editable), `preset --mod M <name>` (replace a mod's settings with a preset),
`generate --mod M`. `modgen_console help` prints the full usage.

## Where things go

- The game folder and the output folder come from the UI's settings (`%LOCALAPPDATA%\D2R mod generator\app.json`).
  Override them with `--game <folder with D2R.exe>` and `--output <folder>`; mods go to `<output>/mods/<name>/`.
  For D2RMM, pass D2RMM's folder as `--output`: it lists `mods/<name>/<name>.mpq` as a data mod.
- `--mod` takes a mod name (looked up in `<output>/mods`) or a path to a mod folder or its `d2modgen.json`.
- `--lod` / `--rotw` pick the D2R table layout (Lord of Destruction or Reign of the Warlock); otherwise the
  UI's choice is used, and a mod remembers the one it was made with.
- The console never changes the UI's own settings (`config.json`, `app.json`); it only reads and writes the
  mod's `d2modgen.json` and the mod's game files.

## Tips

- Values are numbers or `true`/`false`; percent settings use 100 for "unchanged" (`monsterHealth=200` = double HP).
- Seeds: a new mod gets fresh random seeds and keeps them, so regenerating gives the same result
  (`crc` in the result). Set `itemRandomizer.seed=0` (or the other seeds) to roll new ones on the next generate.
- Section switches: whole tabs such as the randomizers only act when their `enabled` setting is true
  (`itemRandomizer.enabled=true`).
- The UI opens the same mods (File -> Open mod), so a person can take over or check an agent's work.

## For contributors

`describe` takes types, ranges and presets from the settings structs at run time and the captions and
help from `src/Config/SettingsHelp.hpp`, which `src/Config/update_help.py` writes from the tooltips in
`qml/Modules/*.qml`. After changing those, run the script (or build with `-DENABLE_SCHEMA_GEN=ON`).
