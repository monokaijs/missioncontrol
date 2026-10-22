# Telegram protocol notes (checked 2026-10-22)

Primary documentation: https://core.telegram.org/bots/api

- Requests use HTTPS on the fixed `api.telegram.org` host, JSON POST for methods and multipart POST for documents. File download uses GET under `/file/bot<token>/<file_path>`. Tokens are never logged.
- `getUpdates` uses an offset one greater than each processed update ID, `allowed_updates` limited to `message` and `callback_query`, and a 20-second long poll with longer HTTP receive timeout. Startup uses negative offset to discard backlog. Delivery is not an exactly-once distributed transaction; actions are serial and in-memory offsets are advanced before dispatch, and new sessions drop stale queues.
- Long polling cannot coexist with webhooks. MissionControl checks `getWebhookInfo` and refuses a configured webhook rather than deleting it. Use a dedicated bot.
- The chat must be private and locally allowed; `from.id` must independently be locally allowed. `is_bot` must be false. Callback queries must include a full accessible message (`date != 0`) and chat; inline-message callbacks are rejected.
- Inline callback data must be 1–64 UTF-8 bytes. MissionControl's fixed verbs plus 32-character random identifiers fit. Callback queries are answered after authorization.
- `sendMessage` supports up to 4096 characters. MissionControl limits window listing size and offers bounded long command output as a document. No parse mode or remotely supplied HTML is used.
- Telegram's standard hosted `getFile` download limit is 20 MB; `sendDocument` supports 50 MB. MissionControl deliberately caps both directions at 20 MiB and checks received bytes as well as advertised size; near-limit server refusals are reported safely. API limits may change. Images are sent as PNG documents for original quality and one consistent transfer cap.
- Bots cannot initiate private contact with an unknown user. The owner must open/start the bot in Telegram first. Telegram bot conversations are not end-to-end encrypted.
