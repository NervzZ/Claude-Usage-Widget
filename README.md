# Claude Usage Widget

A tiny Windows 11 taskbar widget that shows your Claude subscription usage at a glance — three stacked bars: **5‑hour session**, **weekly (all models)**, and **weekly Fable**. Native Win32 + GDI+, no runtime dependencies, ~1 MB RAM and ~0 % idle CPU.

![screenshot](screenshot.png)

The bars go green → amber → red as you approach each limit. The session bar's label is a live countdown to its reset (`H:MM`, updated once a minute; shows `5h` when no reset time is known). The widget sits directly in your taskbar next to the system tray, is borderless, stays pinned to the taskbar band, and auto‑hides when a fullscreen app is on that monitor.

## Requirements

- **Windows 10/11 (x64)**
- **Claude Code**, installed and logged in. The widget reads your OAuth token from `~/.claude/.credentials.json` (the same file Claude Code manages) — there are **no API keys to configure**.
- To build: **Visual Studio 2022** with the *Desktop development with C++* workload (gives you MSVC + the Windows SDK).

## Build

```sh
git clone git@github.com:NervzZ/Claude-Usage-Widget.git
cd Claude-Usage-Widget
native\build.bat app\ClaudeWidget.exe
```

`build.bat` runs `vcvars64.bat` and compiles a single self‑contained `app\ClaudeWidget.exe` (~230 KB, static CRT — no DLLs to ship). If your Visual Studio isn't the 2022 **Community** edition, edit the `vcvars64.bat` path at the top of `native\build.bat`.

Then run `app\ClaudeWidget.exe`. Right‑click the tray icon for options (Refresh, Start with Windows, Reset position, Quit). Drag the widget to reposition it along the taskbar.

## How it works

- It reads your access token from `~/.claude/.credentials.json` and polls `https://api.anthropic.com/api/oauth/usage` every 3 minutes.
- **⚠️ That usage endpoint is undocumented and strictly rate‑limited per token** — it shares a quota with Claude Code's own `/usage`. The widget polls no faster than every **180 s** and honours rate‑limit back‑offs. **Do not lower the interval**, or you risk an hour‑long lockout. (It must also send `User-Agent: claude-code/<version>`, or the endpoint throttles almost immediately.)
- The last good response is cached to `app\cache.json`, so bars survive restarts and cooldowns (shown dimmed while stale).
- It renders as a topmost overlay locked to the taskbar. Windows 11's taskbar can't host custom‑drawn content natively (its buttons are a XAML/DirectComposition layer that draws over any embedded window), so a topmost overlay is the clean way to get an always‑visible bar.

## Configuration

On first run the widget creates `app\config.json`:

| Key | Meaning |
|-----|---------|
| `PollSeconds` | Poll interval in seconds (**minimum 180**). |
| `UserAgent` | Sent as the request `User-Agent`; keep it `claude-code/<version>`. |
| `X`, `Y`, `ManualPosition` | Saved window position. |
| `AutoResume` | Auto‑resume feature (see below). **Ships off.** |
| `ResumeBufferSec` | Seconds after the reset before auto‑resume fires (default 90). |
| `ResumeLeadingEnter` | Press Enter once before typing (clears Claude's wait prompt). |
| `ResumeMessage` | The text auto‑resume types into your sessions. |

## Optional: auto‑resume on reset

If you run long Claude Code sessions that hit the 5‑hour limit, `AutoResume` will — **the moment your limit resets** — focus **every open terminal window** and type `ResumeMessage` + Enter, so blocked sessions pick back up unattended. It's **off by default**; enable it in `config.json` or the tray menu ("Auto‑resume on 5h reset"). "Test resume now" in the tray fires it on demand.

**Before enabling, understand:**

- It types into **all** terminal windows it finds (Windows Terminal / conhost), not only Claude ones. A non‑Claude terminal just receives a harmless unknown command.
- It **only ever presses Enter and types text — never arrow keys** — so a blind Enter can only select the default‑highlighted menu option (normally "wait"); it cannot navigate to a paid "upgrade credits" option.
- When it fires it briefly focuses each terminal, then restores your previous foreground window.

## License

MIT — see [LICENSE](LICENSE).
