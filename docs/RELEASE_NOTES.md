First MissionControl portable Windows x64 release (original project name: remotepc).

- Native C++20/Win32 GUI and visible tray with local bot settings, DPAPI token storage, explicit user AND private-chat allowlists, per-feature permissions, and optional Windows sign-in startup.
- Telegram inline controls for system status, desktop/window captures, graceful window close, Core Audio volume/mute, lock, and one-time 30-second shutdown/restart confirmations.
- Separate default-off shell and each file-transfer direction. Bounded shell jobs, output documents, configured file roots, no overwrites or automatic upload execution.
- Manual Windows build/test/release workflow, portable ZIP, SHA-256 and runtime import report.

Validation: automated policy tests, Windows temporary-file and bounded-process fixtures, offline executable smoke, x64 GUI subsystem and system-DLL import checks run in the linked Actions workflow. No real Telegram bot, desktop screenshots, power operation, startup toggle, or end-user Windows desktop was exercised during development. Interactive acceptance remains required; see docs/ACCEPTANCE.md.

Known limits: unsigned executable; Windows 10/11 x64 interactive session only; visible-pixel window capture (overlaps possible); 20 MiB transfer cap; one action worker; shell grants full current-account access; graceful close/power actions may be refused or prompt for unsaved work. No installer or elevation.
