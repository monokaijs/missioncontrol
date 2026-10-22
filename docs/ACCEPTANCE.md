# Acceptance and verification

## Automated tests

`policy_tests`: authorization matrix for messages/callbacks/documents, user AND chat/type checks, disabled permissions, stale/inline callbacks, confirmation binding/single-use/expiry/replacement/Stop clearing, root/path/device/traversal cases, command bounds and a mocked dispatch counter. These tests never connect to Telegram or act on a desktop.

`windows_tests`: temporary root fixture read/write, overwrite/traversal/ADS/reparse/hardlink rejection, cancelled partial-file cleanup. Harmless command fixtures run **only on the disposable Windows CI runner**, checking output and error exit code, output cap, 15-second timeout and Stop cancellation. No real bot, screenshots, power/lock/startup actions, or user's PC commands. Symlink support is required for the runner's reparse tests; lack of support fails the test rather than silently skipping it.

`MissionControl --self-test`: validates a rejected shell update with empty configuration, exits without GUI, network, settings reads/writes, tray or autorun. `MissionControl --ui-test`: creates the actual native controls with blank fixture configuration, verifies nine unchecked permissions and required control handles, then destroys the window. Skips settings, startup registry, Telegram and tray. Runs only on the Windows runner. This is a construction smoke test, not a visual review.

Binary inspection verifies x64 Windows GUI subsystem and only Windows system DLL imports; static MSVC runtime is required.

## Manual acceptance — not performed by automated CI

Only run on an authorized interactive Windows 10/11 x64 desktop with a dedicated test bot and exact approval for remote captures/commands/file transfers. Never use private live content for test screenshots.

- [ ] Verify ZIP SHA-256; extract and open without installer, admin prompt or console. Check 100%, 150%, 200% DPI and short displays for reachable controls, tab/keyboard navigation and clear text.
- [ ] Confirm all feature permissions default off. Save token and IDs; restart and verify token reload for the same Windows user. Inspect settings/logs for absent plaintext token and absent command/output/file contents.
- [ ] Authorized private chat receives controls on Start; a disallowed user, disallowed chat, group, inline callback and upload each produce no action. No automatic stranger pairing.
- [ ] Test each permission off/on independently; Save settings stops old polling; Save & start reconnects with new settings. Try a stale callback after Stop/restart.
- [ ] Use a benign prepared desktop to test desktop/multi-monitor PNG and visible selected-window capture; minimize a target and check refusal. Confirm overlap behavior is accurately described.
- [ ] List windows, close a disposable unsaved document gracefully, confirm local save prompt; stale/closed window references must fail. Do not force-kill.
- [ ] Test default-device volume/mute, status and lock. Power controls: cancel, wait 30 seconds, cross-user/chat reuse, double-click reuse; only confirm actual shutdown/restart with exact approval and saved work.
- [ ] Enable shell explicitly, use harmless `echo`/nonzero-exit fixtures, request long output document; check 15s timeout, 64 KiB cap, two-minute/peer-bound document buttons and Stop killing child jobs.
- [ ] Enable download only; upload must remain denied. Then enable upload and send a harmless text fixture into a dedicated allowed root. Confirm original file cannot be overwritten, traversal/ADS/junctions/symlinks/hardlinks fail, uploads never execute, oversized files fail and cancellation leaves no partial file.
- [ ] Stop during long polling, shell execution and transfer; after Stop returns no remote action worker remains. Quit fully removes tray/polling. Closing the window alone preserves the visible tray.
- [ ] Deliberately toggle startup on locally, sign out/in on the test PC, verify visible tray and private control message, then toggle startup off and confirm removal of the current user's Run entry. No host startup toggle was exercised during development.
- [ ] Test offline/reconnect, wrong token, existing webhook, second poller, denied folders and absent audio device; errors must be safe and actionable.

The GitHub Windows runner is not a substitute for this interactive desktop acceptance. Release publication certifies the listed automated checks only, not this unchecked manual list.
