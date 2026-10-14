# Troubleshooting

Where to look:

- **`TownfallVideoTuner\tf-video.log`**: what the mod found and did: the videos, their stations, opening, playing,
  stopping, and the sound's route. Each game start begins a new one; the one before is `tf-video.previous.log`.
- **`...\Win64\ue4ss\UE4SS.log`**: the in-game part's lines, starting with `[TF-VIDEO]`.
- **`TownfallVideoTuner\Videos\stations-auto.ini`**: the frequency each video got.

## No magenta marks on the dial

- Load your save first: the marks come once the level's own signals are known, so the stations can keep clear of
  them.
- Check that the videos are directly in `Videos` and end in `.mp4` or `.mkv`. `tf-video.log` has a
  `Discovered Videos/...` line for each video it found, and a `Station [...]` line with its frequency.
- With many videos, the dial's ends can run out of room: `tf-video.log` then says how many videos have no station.
  Use fewer videos, or give some a frequency of their own in `stations.ini` (see the [README](../README.md#settings)).
- The stations can move when a level or a new monster takes their place on the dial. `stations-auto.ini` always has
  the current ones.
- Restart the game after adding or removing videos.

## A video plays without sound

The game's player can't read its sound. Videos with Dolby sound (AC-3 or E-AC-3: often "DD5.1", "DDP5.1" or "EAC3"
in the file name), DTS or TrueHD play without sound; AAC works. For an MKV file, `tf-video.log` says so at the
start:

```
No sound from Videos/<file>: its audio is A_EAC3; the game's player reads MKV sound only in AAC, Opus, FLAC or MP3
```

Convert the sound to AAC: see [Your videos](../README.md#your-videos).

If no video has sound: `tf-video.log` should say `FMOD initialized: CRTV_Videos bus`. `Windows PCM fallback` means
the sound goes straight to Windows instead of through the game: you still hear it, but not through the game's volume
settings, and Townfall Companion's phone doesn't get it.

## A video doesn't play

Tune away and back to try again. If it still doesn't play, the game's player can't read its picture: use a version
with H.264 or H.265 video. `tf-video.log` shows the opening and why it stopped.

## The game stutters while a video plays

Use smaller videos: 480p or 720p looks the same on the CRTV's 640×480 screen as 4K, and is far lighter to play.
`tf-video.log` has a `Playback timing` line every 5 seconds while a video plays: `media` should keep up with
`wall`.

## After a game update

At the first start after an update, `tf-video.log` says `No game profile for this game version yet: reading the
game's symbols`. The mod's DLL reads the game's symbol file (`Townfall-Win64-Shipping.pdb`, which comes with the
game) in a process of its own, through Windows' `rundll32.exe`, for a few seconds; it takes about 1.7 GB of memory
for that time. Then the log says `Game profile: written for this game version` and `Game profile ready: the stations
are on`, and later starts need none of this.

If the stations don't come on:

- `this game version changed what the mod relies on (...)`: the update changed one of the game's functions or
  structures the mod uses. The mod stays off, so the game runs as usual, until the mod is updated too. Please report
  that line.
- `couldn't read the game's symbols: no Townfall-Win64-Shipping.pdb ...`: the symbol file is missing. In Steam,
  right-click Townfall → *Properties* → *Installed Files* → *Verify integrity of game files*.
- `Cannot start reading the game's symbols`, or no `Game profile:` line follows: an antivirus may have stopped it.
  Allow the mod's folder (`...\ue4ss\Mods\TownfallVideoTuner`) and start the game again.

## Codes in UE4SS.log

`UE4SS.log` names a problem by its code (`Native mod disabled (code ...)`, or `error=...`):

| Code | Meaning |
| --- | --- |
| 1 | No game profile for this game version: see [After a game update](#after-a-game-update). The mod is off. |
| 7 | The mod is reading the game's symbols after a game update; the stations come on in a few seconds. |
| 2 | `stations.ini` has a mistake: `tf-video.log` names its line. Fix it, or delete the file for the defaults. |
| 3 | The mod was called from the wrong thread (report it). |
| 4 | The game's video objects weren't there yet; the mod tries again. |
| 5 | A video couldn't be opened or played. Tune away before trying again. |
| 6 | The in-game part and the DLL couldn't pass their data (report it). |

## Still stuck?

Report it with `tf-video.log` (and `tf-video.previous.log` after a crash) and the `[TF-VIDEO]` lines from
`UE4SS.log`.
