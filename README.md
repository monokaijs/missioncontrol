# MissionControl

A lightweight, portable **Windows x64** Telegram control panel built with C++20 and native Win32 controls. Originally requested as **remotepc**, published as MissionControl. No installer, Electron, .NET, service, inbound port, or administrator prompt. C++ runtime is statically linked; Windows provides WinHTTP, WIC, Core Audio, DPAPI and tray APIs.

## Download and setup

1. Download the Windows x64 ZIP from [Releases](https://github.com/monokaijs/missioncontrol/releases), compare its SHA-256 with `SHA256SUMS.txt`, and extract it to a permanent folder. The executable is unsigned; verify the source and artifact before running it. Windows may display an unsigned-app warning. Do not change system security settings.
2. Create a dedicated bot using Telegram's official [@BotFather](https://t.me/BotFather). Open its **private** conversation and send `/start` so the bot may message you. Do not reuse a bot managed by another polling process or webhook.
3. Obtain your numeric Telegram user ID and private-chat ID from a trusted source under your control (for example the [Bot API getUpdates](https://core.telegram.org/bots/api#getupdates) response before running MissionControl). Do not share the token or paste it into a public website. In a direct private conversation these IDs normally match; enter both explicitly.
4. Run `MissionControl.exe`, paste the token, and enter both ID lists in the local GUI. There is no remote pairing. Commas separate multiple IDs; every sender must match the user list **and** the chat list. Never enter someone else's ID unless you intend to authorize them.
5. Enable only the permissions you need. **Every permission starts off**, including shell execution and each direction of file transfer. For transfers, create a dedicated ordinary folder such as `C:\RemoteFiles` and add one existing root per line. Do not use system, startup, configuration, sync-junction, or sensitive folders.
6. Click **Save & start**. You receive an inline control message in each configured private chat. Saved valid settings connect again on the next application launch. **Save settings** saves and leaves the current connection stopped.
7. Optionally check **Start MissionControl when I sign in to Windows**. This writes only the current user's Run entry with `--startup`; the app starts in the visible notification area and sends its controls after sign-in/network connection. This is never enabled by default. Moving the executable requires toggling startup off then on again.

Closing the window hides it to its visible tray icon. Open the tray menu to **Stop remote control**, reopen settings, or **Quit**. Saving changed permissions stops the old session; use Save & start to reconnect. Stop cancels HTTP requests, terminates the current shell job and descendants, waits for the worker, clears pending confirmations and window/output capabilities. Actions already delivered to Windows cannot be undone (for example a confirmed power request). The activity tab and rotating local log show action names and authorized IDs, never tokens, command text, captured output, file contents, or full request URLs.

## Controls and commands

| Control | Permission | Behavior |
|---|---|---|
| `/menu`, `/start`, `/help` | Authorized peer | Inline controls and help |
| `/status` | System status | Computer name, uptime, memory, system-drive free space and AC status |
| `/screen` | Screenshots | Virtual desktop PNG document |
| `/windows` | List / close windows | Up to 40 visible windows, with close and (when capture is enabled) capture buttons valid for two minutes |
| Capture selected window | Screenshots | Captures its current desktop rectangle; overlaps can appear and minimized windows are rejected |
| Close selected window | List / close windows | Graceful `WM_CLOSE`; never force-kills. Unsaved-work prompts can require local input |
| `/volume 0..100`, volume buttons, `/mute` | Volume / mute | Default playback device through Core Audio |
| `/lock` | Lock workstation | Locks the current interactive session |
| `/shutdown`, `/restart` | Shutdown / restart | Requires a **new explicit inline confirmation**, bound to the requester and private chat, one use, 30-second expiry |
| `/cancel` | Shutdown / restart | Clears pending power confirmations |
| `/exec <command>` | Shell (separate opt-in) | `cmd.exe /d /s /c`, current account, 15 seconds, 64 KiB combined output, job object and child process limit. Captures exit/error output; offers a two-minute output-document button for long results |
| `/roots` | Either transfer direction | Numbered configured roots |
| `/get 1 reports/example.txt` | PC to Telegram (separate opt-in) | Sends an existing file beneath root 1, at most 20 MiB |
| Document with caption `/put 1 inbox` | Telegram to PC (separate opt-in) | Writes its plain filename into existing `inbox` beneath root 1. `/put 1` writes at the root. At most 20 MiB; existing files never overwritten; uploaded files never opened or executed |

Root numbers start at 1. Remote paths are relative; subfolders must already exist. Spaces in filenames and UTF-8 names are supported. Transfer operations reject traversal, absolute/UNC/device paths, alternate data streams, reserved names, junctions/symlinks/reparse points in roots or descendants, and hard-linked files. Existing directory handles are held without write/delete sharing during the operation. Missing/busy/denied folders fail closed. Shell access is intentionally **not constrained by file roots**: it grants the full authority of the current Windows account, including modifying files/settings or executing programs. Keep it disabled unless you explicitly need and trust that capability.

## Security and limits

Read [the security model](docs/SECURITY.md) before enabling remote control and [the acceptance checklist](docs/ACCEPTANCE.md) before trusting an installation. Tokens are encrypted with current-user **Windows DPAPI** in `%LOCALAPPDATA%\MissionControl\config.json`, rather than written beside the portable executable. Settings are account-specific, not transferable secrets. Remove the startup toggle before deleting the extracted app; remove the local settings directory separately if desired. Revoke the token with BotFather if compromised. Telegram bot conversations are cloud chats, not end-to-end encrypted; captures/files pass through Telegram infrastructure. `protect_content` is requested but cannot prevent recipients from copying data.

Windows 10/11 x64, interactive signed-in session and HTTPS access to `api.telegram.org` are required. No UAC elevation, credential collection, stealth, antivirus exclusions or security-setting changes. Lock/UAC/protected desktops cannot be captured. Closing privileged apps or shutting down may be refused by Windows policies. Shutdown is graceful and apps can block it. Operations serialize in one worker; a running command can occupy it for up to 15 seconds. File cap is deliberately conservative at 20 MiB in both directions. No remote browsing, overwrite mode, upload execution, live video, arbitrary process-kill, or autoinstall support.

## Build and release

Install Visual Studio 2022 C++ desktop build tools and CMake 3.24+ on an authorized Windows development machine. From a Developer PowerShell:

```powershell
./scripts/build.ps1 -Version 0.0.1
```

This configures x64 `/MT`, builds, runs security/Windows-fixture/offline-smoke tests, checks the GUI subsystem and DLL imports, and creates `dist/MissionControl-0.0.1-windows-x64.zip` plus SHA-256. Only the pinned MIT JSON header is fetched at configure time; no bot token, account IDs, or settings enter the executable/package. The independent policy tests also compile on macOS/Linux with C++20; the native app requires Windows.

The **Windows portable release** Actions workflow is manual (`workflow_dispatch`) with a `version` input. It rejects invalid versions/leading zeroes, values outside Windows version fields, and existing `v<version>` tags; builds/tests/inspects before tagging and publishing the ZIP, checksum and import report. Concurrent release runs serialize. It uses the ephemeral Actions token only; no persistent credential is generated. Source stays on `main`, with version tags; no feature/task branch is needed.

Primary protocol reference: [Telegram Bot API](https://core.telegram.org/bots/api), checked 2026-10-22: HTTPS, long polling offsets, accessible private-chat callbacks, 1–64-byte callback data, 4096-character messages, and document transfer limits. See [protocol notes](docs/PROTOCOL.md).
