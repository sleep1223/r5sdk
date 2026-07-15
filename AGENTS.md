# Repository Guidelines

## Related Repositories

- Backend: `D:\Project\r5\r5-server-bot`
- Frontend: `D:\Project\r5\r5-server-panel`

## Project Structure & Module Organization

This is a Windows-focused C++17 SDK built with CMake and Visual Studio. Primary source lives in `src/`, with each subsystem in its own module directory and usually its own `CMakeLists.txt` (for example `src/engine`, `src/networksystem`, `src/sdklauncher`, `src/game`). Public headers are under `src/public`, build macros are in `src/cmake`, bundled dependencies are in `src/thirdparty`, and runtime resources are in `src/resource`. Build output and local game files go to `game/`; intermediates go to `build_intermediate/`; libraries go to `lib/`; logs go to `log/`. Treat these generated/local directories as non-source.

## Build, Test, and Development Commands

- `CreateSolution22.bat`: configures an x64 Visual Studio solution in `build_intermediate/`.
- `BuildSolution22.bat Debug [target]`: regenerates, builds Debug, syncs SDK vscripts to `game/platform/scripts/vscripts`, logs to `log/debug_latest.log`, and moves symbols to `symbols_debug/latest`.
- `BuildSolution22.bat Release [target]`: same flow for Release, with logs in `log/latest.log` and symbols in `symbols_release/latest`.
- `cmake --build build_intermediate --config Release --target ALL_BUILD -- /m`: direct build command after configuration.

Use Visual Studio 2017 or newer with Desktop Development for C++, Windows SDK 10.0.10240.0+, and x64 tooling.

## Coding Style & Naming Conventions

Match nearby code first. The project uses C++17, MSVC `/permissive-`, `/W4`, and warnings-as-errors by default for non-vendored targets. C++ code commonly uses tabs for block indentation, Source-style names such as `CServer`, `ConVar`, `PlayerAccessCheckRequest_t`, `m_` members, `sv` strings, and `n` integers. Pair headers with implementation files where practical, and add new module files to the local `CMakeLists.txt`. CMake files use 4-space indentation and spaced commands such as `set( NAME value )`.

### R5 VScript Rules

- Do not execute functions at file top level. The script compiler rejects global statements such as `SomeInit()` with `Global variable definition is followed by "("`. Declare/init functions in the script file, load them before callers in `scripts.rson`, and invoke them from an existing initialization entry point such as a `*_LevelInit()` or other established `*_Init()` function.
- Do not add `global function` declarations for functions implemented in another file. The compiler requires a global function declaration to be implemented in the same file.
- Respect `scripts.rson` load order. Do not directly reference a later-loaded global from an earlier-loaded file; either call it from an established later init point or use a guarded `getroottable()` lookup so load order does not create compile-time undefined symbol errors.
- Do not `expect` or cast to complex types such as structs, static arrays, or `functionref`. For guarded `getroottable()` dynamic function calls, keep the table value as `var`, call it only after confirming the key exists, and cast only the returned simple value when a typed wrapper requires it, for example `return expect bool( dynamicFunc( arg ) )`.
- Make new `*_Init()` functions idempotent with a file-level bool guard before registering callbacks or starting threads.
- Give every long-lived thread a single owner, appropriate `EndSignal`/cleanup behavior, and validity checks inside `OnThreadEnd`; never dereference a disconnected or destroyed entity just for debug logging.
- Prefer standalone compatibility layers for cross-version fixes. Do not directly modify existing functions or behavior that may be called by other scripts unless the user explicitly asks for that change.
- Keep shared structs backward compatible across Flowstate/script versions. Do not remove public fields used by older scripts; when replacing an `entity` reference with a handle, preserve the old field or provide a compatibility sync path.
- Keep duplicated SDK and Flowstate script contracts synchronized. Before changing a shared struct, global function signature, enum, or tracker field, search both this repository and `D:\Project\r5\sleep1223_r5_flowstate`, then migrate both deployable copies together.
- Treat shared enum values as an ABI: append new values instead of inserting before existing values, and update every server, client, UI, switch, comparison, and networked-state consumer.
- Prefer UID or `GetEncodedEHandle()` for state that may outlive the current frame, player connection, or entity validity. Validate `entity` values with `IsValid()` and `IsPlayer()` before use.
- Validate indexes before reading or writing arrays such as `file.PlayerMetricsArray`; use helpers such as `IsValidPlayerMetricsIndex()` instead of indexing directly from `GetPlayerMetricsIndex*()` results.
- Guard runtime inputs before recording stats or match reports: reject invalid players, empty UIDs, null `damageInfo`, empty weapon names, and non-positive damage where applicable.
- Avoid nested mutable data in hot damage-event structs when a scalar can represent the same state. Store/update damage events through a helper such as `Tracker_StoreDamageEvent()` so array history and lookup maps stay in sync.
- `RandomIntRange( min, max )` uses an exclusive upper bound. Use `array.len()` for array indexes and `count + 1` when selecting an inclusive one-based identifier.
- Every playlist/config variable must have a live code consumer, and its value must be used instead of a hardcoded equivalent. Validate contradictory combinations such as disabling every allowed input method; keep server-specific features default-off unless the repository is explicitly dedicated to that deployment.
- The engine loads the canonical `playlists_r5_patch.txt`; an alternate patch filename has no effect unless a documented build/deploy step explicitly selects or renames it.
- When manually building serialized strings or JSON, skip invalid/empty entries and use an explicit `firstItem`/`firstPlayer` flag for comma placement instead of relying on array indexes.
- Keep `#if SERVER`, `#if TRACKER`, and `#if STUB` contracts aligned. Shared structs and public function surfaces must compile under every relevant preprocessor branch.

## Testing Guidelines

There is no project-owned automated test harness in the main tree; search hits are vendored third-party tests. Do not run build or compile commands for verification; perform static code checks only, such as targeted source review, `rg` searches, and `git diff --check`. If runtime validation is needed, report the recommended manual scenario for the user to run.

## Version & Build Reporting

When changing project source code or scripts, increment the version in `src\core\build_version.h`. The version should be date-based: use `YYYYMMDDNN`, where `YYYYMMDD` is today's date and `NN` is a same-day sequence number starting at `01` and incrementing for each further change that day. After every change, report whether `--no-generate` should be used and provide the run command as `python .\scripts\r5_release.py all --config Debug --all-servers --no-generate`.

For crash debugging, `scripts\r5_release.py` may be used to retrieve the `game` files, PDB symbols, and source snapshot for a specified version.

Do not proactively run `python .\scripts\r5_release.py ...`; only provide the command unless the user explicitly asks to execute it.

## Commit & Pull Request Guidelines

Recent history mostly follows conventional commits, often `type(scope): description`, for example `feat(player-access): ...`, `build(vs2022): ...`, and `refactor(player-access): ...`. Pull requests should describe behavior changes, list build/manual verification, mention configuration or resource changes, and include screenshots or logs for UI, launcher, or runtime-visible changes.

## Security & Configuration Tips

Do not commit real credentials, private endpoints, personal game files, generated binaries, symbols, or logs. Review `scripts/deploy.toml` before using deploy tooling and keep profile secrets local.
