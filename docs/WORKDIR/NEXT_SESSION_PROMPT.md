# Kickoff prompt for the next session

Paste everything below the line into a fresh session.

---

Read these three files first, in this order, before doing anything else:

- `~/GeneralsX-src/docs/WORKDIR/SCOPE_crossplay.md` — the goal, the six blockers in dependency
  order, the test ladder
- `~/GeneralsX-src/docs/WORKDIR/STATE_2026-07-27.md` — where the build stands and how it got here
- `git -C ~/GeneralsX-src log --oneline -25` — the commit messages carry the *why*, not just the what

**Goal:** my Mac, my iPad and my Windows PC — all on the same WiFi — sit in one LAN lobby, start a
match, and play it to completion without desyncing. Nothing short of that counts. "Windows
compiles" is a prerequisite, not progress.

**All three machines are mine and reachable right now:**

- Mac mini M4 — you are on it. Repo `~/GeneralsX-src`, game data `~/GeneralsX/GeneralsZH/`
- iPad Air 11-inch (M3) — wired, paired, developer mode on, UDID `00008122-000A31010CB9801C`
  (already in our provisioning profile). Generals ZH installed, assets seeded in Documents.
  Install updates directly: `xcrun devicectl device install app --device <id> <app>`
- Windows 11 `r0se-desktop` — `ssh User@192.168.10.89`, key auth, no password.
  Clone at `C:\dev\GeneralsX`, build with `cmd /c C:\dev\cb.bat win64 x64`

There is no friend's PC. Three devices, one LAN.

## Rules I care about

1. **Do not start new investigations while a big task is unfinished.** Finish, then move.
2. **Verify before claiming.** Invoke the `verification-before-completion` skill before saying
   anything is done — including anything a subagent reports. A subagent's summary is a claim;
   reproduce the measurement yourself.
3. **When something behaves unexpectedly, invoke `systematic-debugging` before your second theory.**
   Two wrong theories in a row means the tooling is lying to you.
4. **Keep work in background workflows** so it continues between turns, rather than one foreground
   call at a time.
5. **Never let the Mac or iOS build regress.** Prove it with the build script's *exit code*.
6. **Count the game processes you launch.** `pkill -f GeneralsXZH`, then confirm
   `pgrep -c -f GeneralsXZH` is 0. See the traps list in `~/.claude/CLAUDE.md`.
7. **Never write into the Steam folder on Windows.** Generals Online + EAC live there.

## Where to pick up

A workflow named `win64-drive-to-link` was running when the last session ended. Check whether it
finished — `git log origin/main` will show its commits (they are prefixed `build(win64):`). It had
already landed the crash-reporter x86 port and the Miles/Bink x64 stub gate. **Reproduce its final
numbers yourself** rather than trusting its report.

Then work the blockers in `SCOPE_crossplay.md` in order. Two deserve flagging up front:

- **Blocker 3 is the silent one.** SimID will falsely refuse Mac↔Windows even with byte-identical
  game data, because `m_iniCRC` is not platform-neutral. Fixing it moves the Mac's CRC too, so it
  must land together with re-deriving four retail checkpoint constants.
- **Add the instrumentation early** (bottom of the scope doc). SimID is computed and never logged,
  so a refused join currently cannot be diagnosed at all. Do that before the first cross-platform
  join attempt, not after it fails.

## One unresolved question — do not guess at it

Mac and iPad have `Data/INI/INIZH.big`; the PC does not. It is a *different* archive from the root
`INIZH.big` (different hash and size), it is not in git, no deploy script references it, and it
**is** being loaded (`StdBIGFileSystem.cpp:658` passes `searchSubdirectories = TRUE`). Identify
what is inside it before deciding whether to copy it to the PC or remove it from the Apple side.

## Deferred, not in scope

Keybind remapping UI in settings. Real feature, wants its own design pass after cross-play works.
