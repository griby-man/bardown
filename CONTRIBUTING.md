# Contributing

Bug reports, fixes and findings are all welcome. A few things keep it workable.

## Reporting a problem

Open an issue with:

- what you did and what happened (a screenshot or a short clip helps for anything visual);
- your system: Linux distribution or Windows version, CPU, GPU and driver version;
- the newest log from the `logs` folder beside bardown (on Windows it may be in `%LOCALAPPDATA%\bardown\logs`).

Before reporting a crash or a wrong picture, check the disc is the base Xbox 360 release (title ID `454109BE`,
no title update). Other versions aren't supported.

## No game data, ever

Nothing from NHL 14 goes in the repository or in an issue: no disc images, no `default.xex`, no `.big`
archives, no extracted textures, models or sounds, and none of the generated C++ in `generated/`. Screenshots
and short clips of the game running are fine. Pull requests carrying game data are closed.

## Building and testing

[Building](README.md#building) in the README has the steps. Codegen and the first build take a while; after that,
changes to bardown's own code rebuild in seconds.

- Test against your own dump in `dumps/game`, with the native renderer (the default). If a change touches
  rendering, say what you checked: menus, a match, replays, cutscenes.
- `tools/lint.sh` runs cppcheck over bardown's code and should come back clean. The build uses `-Wall -Wextra`
  and should stay free of warnings.
- Changes to the SDK or the shader translator go in `sdk/bardown-sdk.patch` or `sdk/xenosrecomp.patch`,
  regenerated from the submodule with `git diff`, not as edits to the submodules themselves.

## Code

- Match the code around you. Names are plain, functions short, and C++20 is fine.
- Comments explain why, not what, and only where it isn't obvious. Short and lowercase is the house style.
- Debug switches are environment variables named `BARDOWN_*`, off unless set, and documented where they're used.
- Findings about the game (addresses, structures, behaviour) say how they were established: measured at
  runtime, read from the generated code, or inferred. [docs/D3D.md](docs/D3D.md) shows the convention.

## Commits and pull requests

- One change per pull request, described in a sentence or two: what it fixes and how you checked it.
- Commit messages are a short line in the present tense.
- By sending a contribution you agree it's released under the project's licence, GPL-3.0.
