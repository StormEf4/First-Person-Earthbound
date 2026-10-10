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
- **Hardware reality:** a stock SNES CPU (~3.58 MHz 65816) can only render a small, low-framerate raycast view. A full-screen, playable first-person view realistically needs an enhancement chip (SA-1 or Super FX), or an emulator-assisted approach. Both chips run on the FXPak Pro flash cart and in major emulators.
- **Distribution:** patch file only (.bps). No ROM data in the repo (enforced by `.gitignore`).

## Open questions

Asked 2026-10-10 (session 1). Answers go in the Decision Log.

- Q1. User's role, skills, and weekly time budget
- Q2. ROM version on hand (US EarthBound vs. Japanese Mother 2), headered or not
- Q3. Target platform and enhancement chip (stock CPU / SA-1 / Super FX / emulator-assisted)
- Q4. What "3D/2D hybrid" means (raycaster with sprite billboards / Mode 7 floor only / first-person in some areas only)
- Q5. Movement model (grid-step like Etrian Odyssey / free movement like Wolfenstein)
- Q6. First milestone scope (which area for the vertical slice)
- Q7. Battles (keep original / restyle / real-time)
- Q8. Party followers in first person
- Q9. Art source for wall/floor textures
- Q10. Dev environment (OS, emulator, debugger) and license/release plans

## Decision Log

| # | Date | Decision | Why |
|---|------|----------|-----|
| D1 | 2026-10-10 | Ship as a .bps patch only; never commit ROM data | Legal distribution; standard practice for ROM hacks |
| D2 | 2026-10-10 | Keep this log and `CLAUDE.md` so future sessions don't re-ask questions | User request |

## Session History

### Session 1 — 2026-10-10
- Set up the repo: README, `.gitignore` (blocks ROMs), `CLAUDE.md` (session rules), this log, and a draft roadmap.
- Asked the user Q1–Q10.
- **Next:** record the answers, finalize the roadmap, then start Phase 0 (toolchain + build pipeline).
