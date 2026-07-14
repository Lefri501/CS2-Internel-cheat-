# TempleWare-CS2 — Project Memory

## Project
Internal CS2 (Counter-Strike 2) cheat DLL. C++23, VS2022 (v143), Windows SDK 10.0.26100.0, x64 only.

## Build
`TempleWare-CS2.sln` → `TempleWare.dll` (DynamicLibrary). Deps: `d3d11.lib`, `dxgi.lib`. Preprocessor: `ZYDIS_STATIC_BUILD`. Exception: `/EHa`. External: `external/` (imgui DX11/Win32, kiero, safetyhook+Zydis, lz4, stb_image, nlohmann/json).

## Entry & Init
`main.cpp` → DllMain detects manual map via PEB walk → `MainThread` → waits for `d3d11.dll` + `client.dll` → kiero binds `IDXGISwapChain::Present` index 8 → `hkPresent` is the frame callback (ImGui newframe → menu → HUD → ESP → weather). `TempleWare::init()` order: modules → schema → interfaces → visuals → chams materials → hooks → VAC monitor → memory hardening.

## Hooks (SafetyHook mid-function)
Installed in `Hooks::init()`. All hooks use `CInlineHookObj<T>` wrapper. SEH-guarded. Key hooks: `FrameStageNotify`, `CreateMove`, `DrawArray` (chams), `DrawGlow`, `OverrideView`, `FireEventClientSide` (skin changer killfeed), `LevelInit`, `SetupMapInfo` (weather), `GetRenderFov`, `MouseInputEnabled`, `IsRelativeMouseMode`.

## Feature Map
- **ESP**: Box/hp/name/skeleton/flags, world-to-screen via copied view matrix
- **Aimbot**: Per-weapon profiles (7 groups), FOV/smoothing/RCS/multipoint/visibility/humanization
- **Triggerbot**: Per-weapon, hitchance seed-based, silent aim, autowall
- **Autofire**: Fires on valid target with hitchance + nospread seed mode
- **Movement**: BHop, autostrafe (mouse + vectorial/silent)
- **Chams**: Flat/Illuminated/Glow/Ghost/Latex, xqz, arm+viewmodel
- **Skinchanger**: Knife/gloves/weapon skins (70 indexed slots), agent models, killfeed spoof
- **Glow**: DrawGlow property manipulation, per-player + world items
- **Visuals**: FOV/viewmodel/thirdperson/antiflash/scope removal/legs/smoke/decals/particles removal
- **World**: Night mode/skybox/light/map tint/weather (rain/snow/ash)
- **NadePred**: Throw preview + in-air + landing radius
- **Config**: Flat `Config::` namespace (~200 vars), JSON via nlohmann, per-weapon profiles
- **Notifications**: On-screen notify overlay
- **GameMode**: Auto-disable team checks in DM/FFA

## Security
- **Manual map**: SEH table reg (.pdata walk), security cookie init, thread start spoof → `kernel32!SleepEx`
- **Memory**: `MemSecurity::HardenAfterInit()` converts RWX sections to RX
- **Strings**: Compile-time XOR (`XS()` macro via `xorstr.h`)
- **VAC monitor**: Background thread watching VAC scanning behavior → soft-pause
- **SEH**: `__try/__except` around all hook bodies

## CS2 SDK
External dump at `cs2 sdk dump all you need/` (build 14169, 502/505 sigs). Single-include `cs2.hpp`. Per-module schema classes for 17 DLLs. Dumped offsets, interfaces, patterns, protobufs.

## Player System
`players/` manages player list (CCSPlayerController + C_CSPlayerPawn). `playerHook/` for entity callbacks. Updates each frame via FrameStageNotify.

## Key Patterns
- `CInlineHookObj<SafetyHook>` for all detours
- `GAA(x, offset)` (GetAbsoluteAddress) for RVA resolution
- Pattern scanning via `patternscan/`
- Interface resolution via `utils/memory/Interface/`
- Schema binding via `utils/schema/`
- `renderer/` owns ImGui init, menu + HUD + visual draw
- Console output, file logging available
