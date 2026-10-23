# Contributing

Bug reports and fixes are welcome.

## Bugs

Open an issue with what happened (a screenshot or clip helps for anything visual), your OS, CPU, GPU and
driver, and the newest log from `logs/` (or `%LOCALAPPDATA%\bardown\logs` on Windows).

Check it's the base disc first (title ID `454109BE`, no title update). Nothing else is supported.

## No game data

No disc images, `default.xex`, `.big` files, extracted assets or anything from `generated/`, in PRs or
issues. Screenshots and short clips are fine. PRs with game data get closed.

## Code

- Build steps are in the [README](README.md#building).
- `tools/lint.sh` (cppcheck) should come back clean, and the build should have no warnings.
- SDK and XenosRecomp changes go in `sdk/bardown-sdk.patch` / `sdk/xenosrecomp.patch`, not straight into
  the submodules.
- Match the code around you. Short comments, only where something isn't obvious.
- Debug switches are `BARDOWN_*` env vars, off unless set.
- If you touch rendering, say what you checked (menus, a match, replays, cutscenes).
- One change per PR. Contributions are GPL-3.0 like the rest.
