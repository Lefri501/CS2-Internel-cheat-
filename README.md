# Lefrizzel Ai — CS2 Internal Cheat

> **DISCLAIMER:** This project is for educational/research purposes only. Using it in Counter-Strike 2 violates Valve's ToS and can get you VAC banned. Use at your own risk.

100% AI-generated. Every line written by AI. All reverse engineering done with IDA Pro MCP.

---

## Features

| Category | Details |
|----------|---------|
| **Aimbot** | Per-weapon profiles, FOV/smoothing/RCS, multipoint hitboxes, visibility check, hitchance, target switching delays, reaction & first-shot humanization, sticky hitbox |
| **Triggerbot** | Per-weapon config, seed-based hitchance, silent aim, autowall |
| **Autofire** | Auto fires on valid target with hitchance + nospread seed |
| **ESP** | Box, health, name, skeleton, flags — world-to-screen via copied view matrix |
| **Chams** | Flat, illuminated, glow, ghost, latex. XQZ, arm & viewmodel support |
| **Glow** | Per-player + world item glow via DrawGlow property manipulation |
| **Visuals** | FOV changer, viewmodel offset, thirdperson, antiflash, scope removal, smoke removal, decal/particle removal |
| **World** | Night mode, skybox swapping, light & map tint, weather (rain/snow/ash) |
| **Skinchanger** | Knife, gloves, weapon skins (70 indexed slots), agent models, killfeed spoofing, custom knife model support |
| **Movement** | BHop, autostrafe (mouse + silent), jumpbug |
| **Backtrack** | Per-target head lag compensation |
| **NadePred** | Grenade throw preview with in-air path and landing radius |
| **GameMode** | Auto-disables team checks in Deathmatch / FFA |
| **Config** | JSON-based config with per-weapon profile system |
| **Security** | Manual-map detection, thread spoofing, section hardening, VAC monitor (auto soft-pause) |
| **Menu** | ImGui DX11, tabbed layout |

---

## Build

### Requirements
- Visual Studio 2022 (v143 toolset)
- Windows SDK 10.0.26100.0
- DirectX SDK (June 2010)

### Steps
1. Open `Lefrizzel-Ai.sln`
2. Select **Release | x64**
3. Build

Output: `Lefrizzel Ai.dll`

### Dependencies (vendored in `external/`)
| Library | Use |
|---------|-----|
| imgui | Menu & UI rendering |
| kiero | DX11 hooking |
| safetyhook | Mid-function detours |
| nlohmann/json | Config serialization |
| lz4 | Compression |
| stb_image | Image loading |

---

## Inject

Use any manual-map injector. The DLL detects manual-map injection and initializes safely.

**Known to work with:**
- Process Hacker Native Injector
- Extreme Injector (manual map mode)


---

## Project Structure

```
Lefrizzel-Ai.sln                  # Solution file
Lefrizzel Ai/
├── external/                     # Vendored dependencies
├── source/
│   ├── main.cpp                  # DllMain, Present hook
│   ├── cs2/                      # CS2 SDK data types, entities
│   └── lefrizzel Ai/
│       ├── features/             # All cheat features
│       ├── hooks/                # Detour hooks
│       ├── interfaces/           # CS2 interface wrappers
│       ├── menu/                 # ImGui menu + HUD
│       ├── config/               # Config system
│       ├── players/              # Player list management
│       ├── renderer/             # Renderer init
│       └── utils/                # Math, memory, security, schema
cs2 sdk dump all you need/        # CS2 SDK dump (offsets, patterns, schemas)
```

---

## Credits

- **AI (Claude/GPT)** — entire codebase
- **IDA Pro MCP** — all reverse engineering and pattern discovery
- The CS2 reversing community for public SDK references

---

## License

MIT — do whatever you want with it.
See [LICENSE](LICENSE) for details.
