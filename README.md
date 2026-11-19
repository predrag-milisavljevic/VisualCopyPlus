# VisualCopy+

A tiny Windows tray app (C++/Win32, single statically linked exe) that gives a visual cue whenever something is copied to the clipboard.

- **Flash** the window you copied from: Border, Glow, Fill or Corners; accent or custom color; opacity; duration; fade on/off.
- **Toast** near that window showing what was copied: text snippet with character/line count, image thumbnail with size, or file names with count.
  Content marked sensitive (`ExcludeClipboardContentFromMonitorProcessing`, `Clipboard Viewer Ignore`, `CanIncludeInClipboardHistory = 0`) is never previewed, and clearing the clipboard shows nothing.
- **Ignore list** per app, matched by the clipboard owner's process (`KeePassXC.exe`, ...).
- Optional **sound** (built-in tick, Windows notification, or your own WAV).
- Settings window, tray menu (pause, ignore last app, exit) and **Start with Windows**.
- Per-monitor DPI aware (PerMonitorV2).

Settings are stored in `HKCU\Software\VisualCopyPlus`.

## Build

Needs MinGW-w64 GCC (`winget install BrechtSanders.WinLibs.POSIX.UCRT`).

```powershell
.\build.ps1        # build\VisualCopyPlus.exe
.\build.ps1 -Run   # restart the running copy with the new build
```

Command-line switches: `--settings` opens settings in the running instance, `--exit` closes it.

## Release

GitHub Actions builds every push. Pushing a tag like `v1.0.0` attaches `VisualCopyPlus.exe` to a GitHub release.
