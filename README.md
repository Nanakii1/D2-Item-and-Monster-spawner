# ItemDrop – in-game item & monster spawner for Diablo II: LoD 1.13c

A small DLL plugin that adds chat commands to **single player** Diablo II 1.13c. It is loaded through [PlugY](https://plugy.free.fr/) and works alongside BH.

> ⚠️ **Single player / offline only.** Never load it on Battle.net. Back up your characters first, because this patches game code in memory.

## Commands

```
.item <code> [quality] [ilvl] [sockets] [gem/rune codes...]
.mon  <class id> [count]
```

| Argument | Values |
|---|---|
| `code` | Item code from the game tables, e.g. `uap` (Shako) |
| `quality` | `low`, `normal`, `super`, `magic`, `set`, `rare`, `unique` (default `normal`) |
| `ilvl` | `1-99`, or `0` for automatic |
| `sockets` | `0-6` (clamped to what the base item can hold) |
| gems/runes | Up to 6 codes, e.g. `r01 r02 gpv`, inserted in order |
| `.mon` | Monster class ID, count capped at 5 |

Examples:

```
.item uap unique 85 4 r01 r02 gpv
.item xea rare
.mon 156 3
```

After typing a command, click or walk once. The spawn runs on the next server tick.

## How it works

1. **Chat hook:** the DLL patches the client's chat-input call so lines starting with `.item` or `.mon` are caught and not sent as normal chat. If another plugin (e.g. BH) already hooked that call, it is chained instead of overwritten.
2. **Item lookup:** item codes are resolved to table indexes through D2Common's item table, so no code list is hard-coded.
3. **Queue:** the parsed request goes into a thread-safe queue. Game functions must not be called from the UI thread.
4. **Server thread:** a small detour on D2Game's packet dispatcher drains the queue on the game's server thread, where the game's own functions are safe to call.
5. **Spawning:** the DLL calls the game's own routines, after checking their bytes at startup:
   - item creation (with quality)
   - monster spawn
   - item level, by temporarily overriding the level helper during the call
   - sockets, using D2Common's socket setter
   - gems/runes: pick up the gem to the cursor, then run the core of the game's "insert into socket" handler
6. **Safety:** every patch site is byte-checked first and disabled if it doesn't match. Game calls are wrapped in `__try/__except`, and counts are capped.

## Build

32-bit MSVC (VS x86 Developer Command Prompt):

```
cl /LD /O2 /MT ItemDropD2.cpp user32.lib
```

Copy the DLL next to PlugY and add it in `PlugY.ini`:

```
DllToLoad=ItemDrop.dll
```

Set `DEBUG_LOG` to `true` in the source to write `ItemDrop.log`.

## ItemCmd (command builder)

`ItemCmd.exe` is a small helper that builds the chat command for you. Pick an item, quality, ilvl, sockets and gems/runes, then copy the command into the game.

It runs a local web page on `127.0.0.1` only (nothing goes online) and exits when you close the tab. The source is Go, standard library only:

```
go build -ldflags "-s -w" -o ItemCmd.exe .
```

## Compatibility

Only tested on **1.13c** with PlugY and BH. All offsets are for the 1.13c DLLs, and the byte checks make the plugin disable a feature instead of crashing if a build differs.

## Disclaimer

For offline, personal use. Not affiliated with Blizzard Entertainment.
