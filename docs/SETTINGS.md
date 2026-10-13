# Settings

**F1** opens them, over the game. Pick a section on the left; the search box finds a setting by name.
Most changes take effect at once. A few, marked, need a restart: resolution is one.

They're saved in `bardown.toml`, next to the program, but only the ones you've changed. Delete a line,
or the whole file, to go back to the default.

## The picture

- **Native resolution scale** (`native_resolution_scale`): 2 by default, so the game draws at 2560x1440
  instead of the Xbox's 1280x720. 3 draws at 3840x2160; 1 is the Xbox's own. Takes effect on restart.
- **Native renderer** (`native_renderer`): on by default. Off, bardown emulates the Xbox's GPU instead,
  which is slower; its own resolution setting, `resolution_scale`, applies then (2 holds 60, 3 runs in the
  high 40s). Takes effect on restart.
- **Present effect**: how the drawn picture is stretched to your screen.

## Display

- **Vsync** (`vsync_display`): on by default, so frames go to the screen in step with your display.
  Off, they go out as fast as the GPU allows, which on a fast card is thousands a second for a game that
  makes 60. Takes effect on restart.
- **Frame limit** (`present_frame_limit`): the most frames a second sent to the screen: off (the
  default, no limit beyond vsync), 30, 60, 120, 144 or 240. The game makes 60, so 60 matches it; 30
  shows every other frame. Takes effect at once.

## NHL 14

Every tuning value the game has, read out of the running game once it's booted (a second or two after
the title screen comes up), grouped the way EA grouped them: `AI/Defense`, `AI/Passing`,
`Physics/Puck`, `Animation/Skating` and so on. Floats, whole numbers and switches each get one setting;
the game's slider arrays, like the ones that change with difficulty, get one setting per entry
(`nhl_gPassInterceptDifficultyCpu_0` is Rookie, `_3` Superstar).

These are EA's own names for EA's own values, and most of them were never meant to be touched. Change
one, see what it does, change it back if it's not for you. Some worth a look:

| Setting | What it does |
|---|---|
| `nhl_gPassInterceptDifficultyCpu_0` to `_3` | how readily the CPU picks off passes, by difficulty |
| `nhl_gPressurePuck_LookAheadMax` | how far ahead of the carrier a pressuring CPU skater aims |
| `nhl_gBoostValueToUseHustleAnim` | how much boost a skater needs before he hustles |
| `nhl_gAllowFightsDuringGameplay` | fights, on or off |

## Keys

| Key | What it opens |
|---|---|
| F1 | settings |
| F3 | frame times |
| backtick | console |
| F7 | achievements |

Any of them can be rebound under Keybinds.

## Starting it differently

Every setting can also be given on the command line, `--name=value`, which beats the saved file:

```sh
./bardown.AppImage --native_resolution_scale=3 --audio_mute=true
```
