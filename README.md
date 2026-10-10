# Scene Skipper

Skips scenes in The Last of Us Part I and Part II with one key: the cinematics
the game lets
the player skip, and scenes it gives no skip for, such as the car ride. While a
skip is offered, a prompt in a corner of the screen names the key. A skip is
made the way the pause menu's Skip Cinematic makes one: the game starts the
section the skip goes to from its checkpoint.

Version 1.1.0, for `tlou-i.exe` 1.1.5.0, and `tlou-ii.exe` and `tlou-ii-l.exe`
1.6.0. It loads through the
[TLOU Plugin Loader](https://github.com/mwith77/TLOU-Plugin-Loader), version
1.1.0 or later, and its settings appear in the game's menus through the
[Mod Menu Framework](https://github.com/mwith77/Mod-Menu-Framework), version
1.2.0 or later.

In Part II the key skips a cinematic wherever the game itself offers Skip
Cinematic, through the game's own skip, and the prompt shows as in Part I.
Part I's extra scenes below are Part I's only.

## What it skips

During any cinematic the game offers Skip Cinematic for, the key goes where the
pause menu's Skip Cinematic goes. During the scenes below it goes where the
table says instead, or skips where the game offers no skip at all (the phone
call, the study scene and the car ride):

| During | The key goes to | The prompt reads |
| --- | --- | --- |
| The opening cinematic | Past the phone call after it, to Sarah standing up, where the player first has control | Skip Cinematic |
| The phone call | Sarah standing up | Skip Scene |
| The study scene | The start of the car ride, past the cinematic of the escape from the house | Skip Scene |
| The car ride | After the crash that ends it | Skip Car Ride |
| The prologue's last cinematic | The game's title card at its end | Skip Cinematic |
| The title card and the titles | The 20 years later cinematic | Skip Titles |
| The 20 years later cinematic | The street, where the player takes control | Skip Cinematic |

The game's own skip from the prologue's last cinematic, or from the titles, goes
straight to the street; the key passes them one at a time.

The key does nothing while the pause menu, or a menu opened from it, is up.

## The pause menu

Whenever the key can skip, the pause menu offers a skip too. During the
scenes played in-game (the phone call, the study scene and the car ride), where
the game offers no skip, the pause menu shows its Skip Cinematic row with the
prompt's text (SKIP SCENE, SKIP CAR RIDE), and Yes in its confirm box makes the
same skip as the key. During a cinematic the row and its skip are the game's
own, so from the opening cinematic it goes to the phone call, and from the
prologue's last cinematic or the titles to the street.

## The speedrun timer

The game's speedrun timer stops during cinematics but counts the scenes played
in-game: the phone call, the study scene and the car ride. In speedrun mode, a
skip that passes over one of these adds to the timer the time it would still
have counted there, so a run reads the same with skips as without, give or take
the player's own pace. This includes the key's skip of the opening cinematic,
which passes over the phone call.

## Settings

| Setting | In the ini | Default | What it does |
| --- | --- | --- | --- |
| Enable Scene Skipper | `enable-scene-skipper` | ON (`1`) | Turns the Scene Skipper on or off. |
| Skip Button | `skip-button` | F9 (`0x78`) | The key that makes a skip; in the ini, a Windows virtual-key code. The menu refuses a key the game's own controls use in play. |
| Show Skip Prompt | `show-skip-prompt` | ON (`1`) | Shows or hides the prompt. The key skips either way. |
| Skip Prompt Corner | `skip-prompt-corner` | Bottom Left (`3`) | `1` top left, `2` top right, `3` bottom left, `4` bottom right. |
| Skip Prompt Preset | menu only | Default | Subtle, Default or Prominent sets the size, opacity and duration together. |
| Skip Prompt Size | `skip-prompt-size` | `100` | 50 to 200, as a percentage of the prompt's normal size. |
| Skip Prompt Opacity | `skip-prompt-opacity` | `60` | 10 to 100, as a percentage. |
| Skip Prompt Duration | `skip-prompt-duration` | `10` | Seconds on screen after fading in, 0 to 30; `0` never shows the prompt and `31` (Persistent) keeps it for the whole scene. The key still skips after the prompt has gone. |

The settings are kept in `scene_skipper.ini`, and a change to them, from the
menu or in the ini, takes effect while the game runs. The prompt plays no sound.

## Files

| File | Contents |
| --- | --- |
| `scene_skipper.cpp` | The Scene Skipper: the scenes it skips, the skip, the prompt, the pause menu's row and the speedrun timer. |
| `tlou_plugin_sdk.h` | The loader header the Scene Skipper includes. |
| `scene_skipper.rc` | The DLL's version resource: its author, product and version. |
| `scene_skipper.ini` | The settings, with their defaults. |
| `scene_skipper.menupage` | The settings page the framework draws. |
| `build_scene_skipper.bat` | Builds the DLL. |

## Building

From the x64 Native Tools Command Prompt for Visual Studio, which provides
`cl` and `rc`, run `build_scene_skipper.bat` in this folder. It builds
`scene_skipper.dll`.

## Installing

Laid out as it goes into the game folder:

    Mods\Scene Skipper\                     the DLL
    Mods\Mod Menu Framework\Mod Menus\      the Scene Skipper's menupage

At its first start the Scene Skipper writes `scene_skipper.ini` beside its DLL
where it is missing or empty, and adds any setting it lacks, so updating never
resets it. It needs the TLOU Plugin Loader, which loads it. With the Mod Menu
Framework 1.2.0 or later its settings show in the game's menus. Without the
framework the same settings are edited in the ini directly.

On any other build of either game the Scene Skipper does nothing, and says so
in its log.

Uninstalling is deleting `Mods\Scene Skipper` and the menupage. Nothing in a
save depends on the Scene Skipper.

## Logs

The Scene Skipper keeps one log, `Mods\Scene Skipper\scene_skipper.log`,
newest line first, at most 1 MB, and writes only failures and errors.
