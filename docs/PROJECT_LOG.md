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
- **Map data (headerless US ROM file offsets):** map rows via the 3-byte pointer at `0xA1DB` (8 row groups); extra tile bits at `0x175000`; sector byte (tileset<<3 | palette) at `0x17A800`, 32x80 sectors of 8x4 map tiles; map tileset to drawing tileset at `0x2F101B`; pointer tables for graphics `0x2F105B`, arrangements `0x2F10AB`, palettes `0x2F10FB` (0xC0 bytes each, 6 subpalettes; an arrangement's palette bits minus 2 gives the subpalette), and collision `0x2F117B` (u16 offsets into `0x180000`). Graphics and arrangements are HAL-compressed. Collision bit 0x80 = solid, 0x10 = ladder/stairs, 0x08/0x0C = water, 0x01/0x02 = hides Ness's lower/upper body. Palettes with a nonzero u16 at +0 have an event-flag variant at `0x1A0000 | u16@+0x20`; **for Onett that variant is the night palette.**
- **Ness's front door** is at about pixel (2400, 343).
- **Indoor flag:** the sector misc table at `0x17B200` has 2 bytes per sector; `byte0 & 7` is the setting (1 = indoors, 7 = indoors with butterflies).
- **3D object classes** (world3d.c `analyse_world`): props are ≤6x6 minitiles and ≤30 cells, outdoors. Foliage is ≥50% green pixels on the day palette, or thin (≤2 wide or tall). Terrain is wider or taller than 48, or area >200 with fill <0.5. Everything else is a building. Debug with `--class-map`: yellow prop, cyan foliage, blue terrain, red building.
- **Reference photos:** shot list = `y{py}_x{px}_d{deg}.png  px py yawdeg pitchdeg [height]` for py 64..2047 and px 704..3199 (step 128), plus overhead (height 30, pitch -40, step 384). Cells inside solid objects are skipped (825 images, about 45 s). Generator: see Session 2.
- **Walk-behind cells:** collision flags 0x01/0x02 (`ebw_foreground_at`) mark the art drawn in front of Ness: house roofs, tree canopies (upper part only), sign boards, and tops of tall buildings. The SNES priority bit is NOT used for this in EarthBound (it's always 0 on map tiles). Debug: `EBMAPDUMP_FG=1 ebmapdump rom out.png tx ty tw th 1`.
- **Cloud session network:** github.com git clones work; GitHub release downloads and buildbot.libretro.com are blocked; PyPI works. So we build tools from source (cc65, snes9x core, raylib).

## Open questions

Q1–Q13 are answered (D3–D12, D17–D19). Still open:

- Q14 (deferred to the Phase 4 checkpoint): battle background concepts. Direction so far: "trippy and fun to look at". Offer them as switchable options (D20).
- Public release of v0.1: not decided. Ask at the Phase 5 checkpoint.

## Decision Log

| # | Date | Decision | Why |
|---|------|----------|-----|
| D1 | 2026-10-10 | Never commit ROM data. (Originally "ship as a .bps patch only"; revised by D5: we now ship a PC program, plus a .bps only if game-side tweaks are needed.) | Legal distribution |
| D2 | 2026-10-10 | Keep this log and `CLAUDE.md` so future sessions don't re-ask questions | User request |
| D3 | 2026-10-10 | User's role: **creative direction only**. Claude writes all code, tools, and builds. User playtests and decides look and feel. | Q1 |
| D4 | 2026-10-10 | User has their own US EarthBound ROM. FPEB's built-in check accepted it during playtest #2. | Q2 |
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
| D16 | 2026-10-10 | Debug panel is **on by default**. Tab or F1 toggles it, and the choice is saved as `debug=` in `fpeb.ini`. When hidden, a faint "Tab: show debug info" hint stays on screen. Prefer non-F-keys for anything the user must press. | User couldn't see the panel (F1-only and hidden by default; laptop F-keys often need Fn) |
| D17 | 2026-10-10 | The user will upload their ROM to the cloud session for Claude to test with. Keep it **outside the repo** (scratchpad or the git-ignored `roms/`). Cloud containers are temporary, so a later session may need to ask for it again. If the ROM is missing, say so and ask; don't re-ask the policy itself. | Q11 |
| D18 | 2026-10-10 | No deadline; the user runs several projects. Work at a strong, steady pace and ship good builds regularly. | Q12 |
| D19 | 2026-10-10 | Visual style: **chunky low-res pixels** (Doom/PS1/boomer-shooter feel) | Q13 |
| D20 | 2026-10-10 | Major creative decisions are playtested before moving on. Ship them as switchable options inside one build (hotkey or settings), not as separate builds. | Q12: user asked to playtest options for major creative choices |
| D21 | 2026-10-10 | Phase 1 picks: **640x360** resolution, **Facades** wall style, **Light** fog. Performance was fine: steady high fps on the user's PC. | Phase 1 🟡 playtest |
| D22 | 2026-10-10 | Facades must be "rounded out": no staircase or voxel look where every pixel row adds height. Objects get clean, consistent shapes. Most things look ugly and need touching up; Claude audits visually with its own cloud screenshot pipeline and fixes without waiting. | User feedback on Phase 1 |
| D23 | 2026-10-10 | Wall height is decided by context, not chosen by the player: **low indoors, tall outdoors**. Use the sector's Indoors setting. | User |
| D24 | 2026-10-10 | Lighting mood and day/night palette **change with context** (scene, story, mood), driven by game state rather than a player setting. The Phase 1 hotkeys stay as debug overrides. | User |
| D25 | 2026-10-10 | Claude has no access to the user's screen or capture software in cloud sessions. Visual checks use FPEB screenshots taken in the cloud with the user's ROM (see the "How to test" notes). | The user believed screen capture had been set up; it isn't reachable from here |
| D26 | 2026-10-11 | 3D conversion follows EarthBound's oblique projection. A building's collision is its footprint, and its wall height is how far its art rises above that footprint. The whole art stands up as the front wall, at one height per building (median over its columns), and the copy painted on the ground behind it is erased. Bulky cells (inside a solid 3x3) and thin cells are grouped separately, so fences and hedges don't fuse with buildings. Hand blueprints (`src/blueprints.c`) can override any building that is still wrong. | User: "the entire town is a jumbled mess". The reference survey showed misclassified merged objects and road art cut out with signs |
| D27 | 2026-10-11 | Claude keeps a large reference-photo set to judge visuals: `FPEB --shot-list` batch renders an Onett grid (128 px step, 4 directions, plus overhead views) into the scratchpad `refs/` folder, alongside `--class-map` and `ebmapdump` top-down maps. Regenerate after every visual change and compare before and after. | User: "take as many reference photos for urself as possible" |
| D28 | 2026-10-11 | **The walk-behind flags are ground truth for "what stands up".** EarthBound marks the cells Ness walks behind (collision bits 0x01/0x02, which hide part of his sprite). Above a solid footprint those cells are the roof or canopy, and roads and lawns are never marked. Buildings: solid rows = front wall height W, walk-behind rows = roof and depth R. Each building is one model with a pitched gable (saturated roof colour, R ≥ 3) or a flat roof. Cut-outs take only walk-behind cells, plus the two rows under them for leafy things. Any object made of thin (non-bulky) cells is a cut-out strip. | User: fences blend with grass, tree roots awkward, roofs should be sloped like in the game |

## Session History

### Session 1 — 2026-10-10
- Set up the repo: README, `.gitignore` (blocks ROMs), `CLAUDE.md` (session rules), this log, and a draft roadmap.
- Asked the user Q1–Q10.
- User answered Q1–Q10. Recorded as D3–D12; Claude decided D13–D15. Roadmap rewritten for the emulator-assisted approach.
- Phase 0 built: libretro host (`src/lr_host.c`), main app with drag-and-drop setup, gamepad and keyboard, audio, SRAM saves, F5/F9 save states, F1 RAM debug panel (`src/main.c`), ROM check plus unit test, RAM symbol generator, Windows CI build.
- Verified in the cloud: Snes9x core built from source, plus a homebrew test ROM (`tests/testrom/`) that writes to Ness's X/Y addresses. `FPEB --frames 120 --debug --screenshot` showed the values changing; setup and rejection screens render correctly.
- Not yet verified: the real EarthBound ROM (Q11), the Windows build on the user's machine, and audio (no sound device in the cloud).
- **Next:** the user tests the Windows zip (Phase 0 🟡). Then Phase 1: ROM map/tileset/collision extraction for Onett.
- User playtest #1: the game ran, but "no info HUD visible". Fixed per D16; new build pushed.
- User playtest #2: **Phase 0 passed.** Game, sound, Ness X/Y, and facing are all accurate on the real ROM on Windows. The user confirmed facing values 0 up, 6 left, 4 down, 2 right. Q11–Q13 answered (D17–D19, D20).
- **Next:** the user uploads the ROM. Then Phase 1: decode Onett's map, tileset, palette, and collision from the ROM, and build the free-fly 3D view with resolution presets.
- ROM uploaded and copied to `roms/earthbound.smc` (git-ignored). It's the clean US dump (CRC32 DC9BB451, with a 512-byte header). The original upload is at `/root/.claude/uploads/<session>/…Earthbound_U_.smc`.
- Phase 1 built: ROM map decoder, 3D world builder (Facades and Blocks styles), free-fly viewer with six creative options. Verified with the real ROM in the cloud: screenshots at Ness's door, downtown Onett, and overhead, in both palettes and several moods.
- **Next:** the user picks the Phase 1 options (🟡). Then Phase 2: camera follows Ness from RAM, free movement fed to the game, and game text boxes overlaid. Pick the palette automatically from the event flag in RAM.
- User playtest #3 (Phase 1): picks recorded as D21–D24. The user believed screen capture had been set up for Claude; it isn't reachable from a cloud session (D25), so Claude used its own FPEB screenshot audit (`/tmp/audit.sh`-style: 8 fixed Onett views at the user's settings).
- "Rounded out" pass on world3d.c: object labelling (connected solid areas), with classes prop / foliage and thin (cut-outs) / building (uniform storeys) / terrain (low ledges). Ground-colour keying with a floor patch erases the flat copy under cut-outs, and side walls borrow siding art. Before and after screenshots were sent to the user.
- **Next:** the user re-checks the look. Then Phase 2 (camera follows Ness, movement, text overlay, palette and mood from game state).

### How to test in a cloud session (so it doesn't need re-deriving)
- Tools from source into /tmp: `git clone --branch V2.19 https://github.com/cc65/cc65 && make -C cc65 bin`; `git clone https://github.com/libretro/snes9x && make -C snes9x/libretro`.
- Linux build needs: `apt-get install libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev libasound2-dev`.
- Test ROM: `ca65 tests/testrom/testrom.s -o t.o && ld65 -C tests/testrom/testrom.cfg -o testrom.sfc t.o`.
- Screenshot the 3D view with the real ROM: `FPEB --rom $PWD/../roms/earthbound.smc --core ... --view3d --cam PX PY YAWDEG PITCHDEG [--cam-y UNITS] [--set walls=1 ...] --frames 45 --screenshot x.png`. Put `debug=0` in `build/fpeb.ini` to hide the panel. Run these one at a time, since they share fpeb.ini.
- Run: `xvfb-run -a ./build/FPEB --rom /abs/testrom.sfc --core /abs/snes9x_libretro.so --any-rom --debug --frames 120 --screenshot shot.png`. raylib saves the screenshot in the program's folder, whatever path you give.

### Session 2 — 2026-10-11
- User: "the entire town is a jumbled mess", then "take as many reference photos for urself as possible". Added the `--shot-list` batch renderer (native 640x360 frames, completes chunk building before saving), `--dump-objects`, and the `--class-map` legend. Captured 825 eye-level and overhead shots of Onett plus top-down class and art maps.
- Root causes found in the survey:
  1. Signs and stop signs cut out together with the road around them (giant pale slabs).
  2. Buildings fused with fences, hedges, and lawn trees, then misclassified as cliffs and flattened.
  3. The mansion and forest colour tests were wrong.
  4. Building heights didn't follow the projection.
- Fixes:
  - Cut-outs only take open pixels above the object, with strict ground keys (dominant colours) and loose keys at the trunk base for painted shadows.
  - Bulky/thin split before grouping.
  - Forest class plus per-cell tree walls inside cliff and forest masses.
  - Walls drawn wherever a neighbour is lower (height field).
  - Building height from the art above the footprint (D26), with floor erasure.
  - Side and back walls repeat the lower half (walls), so roof art only shows on the front.
- **Next:** the user reviews the new build. Blueprint any remaining bad buildings. Then Phase 2.
- Regenerate refs: write the list with the Python snippet (grid as above) to `/tmp/ref_list.txt`, then `printf "res=4\nfog=2\n" > build/fpeb.ini; xvfb-run -a build/FPEB --rom $PWD/roms/earthbound.smc --core build/snes9x_libretro.so --shot-list /tmp/ref_list.txt`.
- Round 3 (same session): the user asked for see-through fences, better tree roots, sloped roofs, and 3D-sensible design tweaks.
  - Found the walk-behind signal (D28) and rebuilt building heights and cut-outs on it.
  - Buildings are now whole models with gable or flat roofs (`emit_building`).
  - Fences get loose ground keying, thin cells always become strips, and the ground sample never takes leafy cells (single-cell trunks were losing their canopies).
  - Art-less blocker cells are hidden.
  - Verified with full reference passes.
  - Building depth = max(roof rows, wall/2), capped by the solid footprint (`building_depth`). Side walls are drawn wherever a neighbouring column's footprint doesn't cover the slice, so staggered blocks like the motel close up instead of showing slivers.
  - Known leftovers: hills aren't raised plateaus (cliffs are 2-level ledges); forest masses are flat-topped walls; an odd striped tall object near (1000,1470).
