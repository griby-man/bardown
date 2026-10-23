# Settings

F1 opens them over the game. There's a search box. Most changes apply right away; the ones that need a
restart say so.

Only what you change gets saved, in `bardown.toml` next to the program. Delete a line (or the file) to
go back to defaults.

## The picture

- `native_resolution_scale`: 2 by default (2560x1440). 3 is 3840x2160, 1 is the Xbox's 720p. Restart.
- `native_renderer`: on by default. Off uses the emulated GPU, which is a lot slower and uses its own
  `resolution_scale` instead. Restart.
- Present effect: how the picture gets stretched to your screen.

## Display

- `vsync_display`: on by default. Off, it presents as fast as the GPU can, which is thousands a second
  on a fast card for a game that only makes 60. Restart.
- `present_frame_limit`: off (default), 30, 60, 120, 144 or 240. The game runs at 60.

## NHL 14

Every tuning value in the game (about 13,000), pulled from memory a second or two after the title
screen, grouped the way EA grouped them (`AI/Defense`, `Physics/Puck`, `Animation/Skating`...). Arrays
that change with difficulty get one entry each, so `nhl_gPassInterceptDifficultyCpu_0` is Rookie and
`_3` is Superstar.

These are EA's internal values and most were never meant to be touched. Mess around, change it back if
you don't like it. A few fun ones:

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

Any setting works on the command line as `--name=value`, and that wins over the file:

```sh
./bardown.AppImage --native_resolution_scale=3 --audio_mute=true
```
