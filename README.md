> **Hotfix fork.** Community continuation of
> [flaf1x/Butterscotch360-Refresh](https://github.com/flaf1x/Butterscotch360-Refresh)
> (archived), with a new OXDK-based build for Xbox 360. Deltarune is playable to the
> end on real hardware; Undertale support is experimental.
> See [HOTFIX.md](HOTFIX.md) for what changed and how to build.
> No game files or XDK files are included. You must provide your own.

# Butterscotch360-Refresh

Butterscotch360-Refresh is an experimental Xbox 360 refresh of
[Butterscotch](https://github.com/ButterscotchRunner/Butterscotch), an open
source reimplementation of the GameMaker: Studio runner. The practical goal is
simple: make Undertale-era GameMaker games run on real Xbox 360 hardware with a
maintainable source tree instead of relying on one old binary.

This repository started from
[ceilingtilefan/Butterscotch-360](https://github.com/ceilingtilefan/Butterscotch-360),
but it is not just a rebuild of that old fork. It refreshes the port against a
much newer Butterscotch codebase and adds the Xbox 360-specific runtime pieces
needed for current testing:

- Visual Studio 2010 / Xbox 360 SDK project files.
- Xbox 360 D3D9 renderer with point-sampled pixel output.
- XAudio2 audio backend with streamed OGG music playback.
- Startup splash/progress screen.
- Runtime diagnostics written to `bs360_refresh.log`.
- Toggleable on-screen diagnostics with `LB + RB`.
- Undertale and NXTale compatibility fixes tested on real hardware.

Also, yes, this is double vibe-coded in the funniest possible way: the original
Butterscotch-360 port was reportedly brought up with Claude Code, and this
refresh was revived, debugged, and iterated with ChatGPT Codex. The intent is
still serious: keep the source available so other people can build, inspect,
improve, and preserve the port.

## Current Status

The port has been tested primarily with Undertale v1.08 and NXTale on a real
Xbox 360. Vanilla Undertale is playable in the tested areas, and NXTale now has
working console-style borders, controls, sprites, audio, and spear rotation.

This is still experimental software. Expect missing GameMaker functions, game
specific quirks, and Xbox 360 memory limits to matter.

## Files On The Console

Place the runner and game files in the same directory on the Xbox 360. During
testing the directory was:

```text
Hdd1:/Btrsctch
```

Expected files:

```text
Butterscotch360-Refresh.xex
data.win
*.ogg / *.wav audio files used by the game
splash.png (optional)
CONFIG.JSN (optional)
```

`splash.png` is shown during startup while `data.win` is parsed. The progress
bar and stage text are drawn over the splash.

## Controls

Default Undertale-style keyboard mapping through the Xbox 360 controller:

- D-pad / left stick: movement
- A: confirm / Enter
- B: cancel / Shift
- X: Control
- Y: X key
- Start / Back: Escape
- Right trigger: temporary speed-up for testing
- LB + RB: toggle the on-screen diagnostic overlay

The diagnostic overlay shows FPS, frame timing, room name/index, instance count,
application surface state, controller state, and memory usage.

## Optional CONFIG.JSN

`CONFIG.JSN` can be placed next to `data.win`.

Example:

```json
{
  "gamepadApi": false,
  "deferDrawToAfterAllSteps": false,
  "controllerMappings": {
    "4096": 13,
    "8192": 16
  }
}
```

Notes:

- `gamepadApi` defaults to `false` because some Undertale/NXTale builds read
  both keyboard-style input and GameMaker gamepad input, which can duplicate
  movement.
- `os_type` is kept as Windows by default for compatibility with tested builds.
- `controllerMappings` uses XInput button masks as keys and GameMaker key codes
  as values.

## Building

Requirements:

- Windows
- Visual Studio 2010
- Xbox 360 SDK

Build command used during testing:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\vcvarsall.bat" x86
"C:\Windows\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe" Butterscotch360.sln /t:Rebuild /p:Configuration=Release /p:Platform="Xbox 360" /m:1
```

The built binary is produced as:

```text
Release/Butterscotch.xex
```

For distribution/testing it is usually copied or renamed to:

```text
Butterscotch360-Refresh.xex
```

## What Changed From Butterscotch-360

- Refreshed core runner code from a newer Butterscotch base.
- Added guarded `data.win` parsing diagnostics for Xbox crashes.
- Added Xbox 360 startup splash/progress rendering.
- Added D3D9 support for real `application_surface` rendering.
- Fixed byte-order and texture-page parsing issues seen on Xbox 360.
- Added lazy texture-page loading and reduced texture memory pressure.
- Reworked audio around streamed OGG playback to fix speed, stutter, and missing
  music issues.
- Fixed point sampling and 720p presentation for crisp pixel output.
- Added NXTale-specific compatibility improvements without changing default
  `os_type` away from Windows.
- Added on-screen diagnostics for long hardware test sessions.

## Credits

- Upstream runner: [ButterscotchRunner/Butterscotch](https://github.com/ButterscotchRunner/Butterscotch)
- Original Xbox 360 port: [ceilingtilefan/Butterscotch-360](https://github.com/ceilingtilefan/Butterscotch-360)
- Refresh/testing: flaf1x with ChatGPT Codex

## License

This project follows the licensing of the upstream Butterscotch project. See
[LICENSE](LICENSE).
