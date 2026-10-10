# Project Log

The single source of truth for decisions. Newest entries at the bottom of each section.

## Known facts (established, don't re-research)

- **Base game:** EarthBound (US, SNES, 1995). HiROM cartridge, 24 Mbit, no enhancement chip.
- **Battles are already first-person** in the original (enemies drawn over animated backgrounds, party not shown). The main conversion work is the overworld.
- **Overworld data:** maps are tile-based in an oblique top-down view, with per-tile collision data. Collision is the most likely source for generating 3D walls automatically.
- **Existing community tools:**
  - CoilSnake: decompiles the ROM into an editable project (maps, sprites, text via CCScript) and recompiles it.
  - ebsrc: a community disassembly of the game's code, built with ca65. Useful for finding the routines to hook (overworld render, movement, collision, NPC interaction).
  - asar / ca65: 65816 assemblers. Flips/Floating IPS: BPS patch creation.
- **Hardware reality:** a stock SNES CPU (~3.58 MHz 65816) can only render a small, low-framerate raycast view. That's why we chose the emulator-assisted route (D5).
- **Distribution:** our own code plus, if needed, a small .bps patch. No ROM data and no emulator binaries in the repo (`.gitignore` keeps ROMs out).
- **RAM symbols:** ebsrc's `src/bankconfig/common/ram.asm` lays out all of WRAM ($7E0000 and up) by name, e.g. `GAME_STATE`, `CURRENT_LEADER_DIRECTION`, `BATTLE_MODE`, `PLAYER_POSITION_BUFFER`. We build those into an address table instead of guessing addresses.
- **Cloud session network:** github.com git clones work; GitHub release downloads and buildbot.libretro.com are blocked; PyPI works. So we build tools from source (cc65, snes9x core, raylib).

## Open questions

Q1–Q10 were answered in session 1 (see D3–D12). Still open:

- Q11. Testing workflow: will the user upload their ROM to the cloud session (never committed) so Claude can test against the real game, or test each build themselves on Windows?
- Q12. Target date and public release plans (left over from Q1 and Q10)
- Q13. Visual style of the 3D world: chunky low-res pixels (PS1/Doom feel) vs. smooth and high-res
- Q14 (deferred to the Phase 4 checkpoint): battle background concepts. Direction so far: "trippy and fun to look at".

## Decision Log

| # | Date | Decision | Why |
|---|------|----------|-----|
| D1 | 2026-10-10 | Never commit ROM data. (Originally "ship as a .bps patch only"; revised by D5: we now ship a PC program, plus a .bps only if game-side tweaks are needed.) | Legal distribution |
| D2 | 2026-10-10 | Keep this log and `CLAUDE.md` so future sessions don't re-ask questions | User request |
| D3 | 2026-10-10 | User's role: **creative direction only**. Claude writes all code, tools, and builds. User playtests and decides look and feel. | Q1 |
| D4 | 2026-10-10 | User has their own EarthBound ROM (stays on their machine). Version to be confirmed with `tools/check_rom.py`. | Q2 |
| D5 | 2026-10-10 | **Emulator-assisted.** A standalone Windows program runs EarthBound inside it through the Snes9x libretro core. It reads game memory every frame and draws a 3D first-person view in place of the top-down one. Not playable on a real SNES or through RetroArch's normal menu. | Q3 (user chose D over SA-1/Super FX) |
| D6 | 2026-10-10 | Wolfenstein/Doom-style hybrid: 3D walls, floor, and ceiling; characters, enemies, and items as 2D sprite billboards | Q4 |
| D7 | 2026-10-10 | Free movement and free turning | Q5 |
| D8 | 2026-10-10 | Vertical slice = the opening night: Ness's house, the meteor hilltop, Onett at night | Q6 |
| D9 | 2026-10-10 | Keep EarthBound's battle system; replace backgrounds with new ones that match the 3D world and stay **trippy and fun to look at** | Q7 |
| D10 | 2026-10-10 | Party followers walk behind Ness in the 3D world, visible when you turn around | Q8 |
| D11 | 2026-10-10 | Textures are generated automatically from EarthBound's own tile art, with hand fixes in key spots | Q9 |
| D12 | 2026-10-10 | User setup: Windows 10, RetroArch with the Snes9x core. No flash cart. | Q10 |
| D13 | 2026-10-10 | Stack (Claude's call; user can override): a **C program using raylib** that hosts the libretro core. It loads `snes9x_libretro.dll` from the user's existing RetroArch install, so no emulator is bundled. Windows .exe built by GitHub Actions. Look-and-feel settings live in hot-reloading text config and shader files. | Small and fast. Claude can build and run it end to end in the cloud session (Godot can't be downloaded here). |
| D14 | 2026-10-10 | RAM addresses come from ebsrc's `ram.asm`, assembled with ca65 into a generated table | Exact addresses, no guessing |
| D15 | 2026-10-10 | The ROM check lives inside FPEB.exe, not in a Python tool. Setup is drag-and-drop (core .dll and ROM onto the window), remembered in `fpeb.ini`. | User is creative-only on Windows, with no Python to run |

## Session History

### Session 1 — 2026-10-10
- Set up the repo: README, `.gitignore` (blocks ROMs), `CLAUDE.md` (session rules), this log, and a draft roadmap.
- Asked the user Q1–Q10.
- User answered Q1–Q10. Recorded as D3–D12; Claude decided D13–D15. Roadmap rewritten for the emulator-assisted approach.
- Phase 0 built: libretro host (`src/lr_host.c`), main app with drag-and-drop setup, gamepad and keyboard, audio, SRAM saves, F5/F9 save states, F1 RAM debug panel (`src/main.c`), ROM check plus unit test, RAM symbol generator, Windows CI build.
- Verified in the cloud: Snes9x core built from source, plus a homebrew test ROM (`tests/testrom/`) that writes to Ness's X/Y addresses. `FPEB --frames 120 --debug --screenshot` showed the values changing; setup and rejection screens render correctly.
- Not yet verified: the real EarthBound ROM (Q11), the Windows build on the user's machine, and audio (no sound device in the cloud).
- **Next:** the user tests the Windows zip (Phase 0 🟡). Then Phase 1: ROM map/tileset/collision extraction for Onett.

### How to test in a cloud session (so it doesn't need re-deriving)
- Tools from source into /tmp: `git clone --branch V2.19 https://github.com/cc65/cc65 && make -C cc65 bin`; `git clone https://github.com/libretro/snes9x && make -C snes9x/libretro`.
- Linux build needs: `apt-get install libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev libasound2-dev`.
- Test ROM: `ca65 tests/testrom/testrom.s -o t.o && ld65 -C tests/testrom/testrom.cfg -o testrom.sfc t.o`.
- Run: `xvfb-run -a ./build/FPEB --rom /abs/testrom.sfc --core /abs/snes9x_libretro.so --any-rom --debug --frames 120 --screenshot shot.png`. raylib saves the screenshot in the program's folder, whatever path you give.
