# Native messaging protocol

Chrome/Brave native messaging, on stdin/stdout of the host.

## Transport

```
┌──────────────┬──────────────────────────────┐
│ uint32 length│ UTF-8 JSON payload           │
│ native-endian (little-endian on x86/ARM64)  │
└──────────────┴──────────────────────────────┘
```

**stdout is protocol-only.** All logging must go to **stderr**, otherwise it corrupts the
stream and the browser tears the host down.

## Request

```json
{ "id": "7", "cmd": "fetch", "params": { "url": "https://..." } }
```

| cmd | params | reply |
|---|---|---|
| `ping` | — | `ok` |
| `fetch` | `url`, `cookies`? | `formats` |
| `download` | `url`, `format_id`, `out_dir`, `cookies`?, `container`?, `player_clients`? | `item`, then events |
| `queue` | — | `state` |

`state` carries `items` in **insertion order** (oldest first); each item has
`created_at` (Unix milliseconds), which the extension uses to list the queue
newest-first.
| `cancel` | `download_id` | `ok` |
| `shutdown` | — | `ok`, then exit |

## Responses

Every response echoes the request `id`. Progress events are unsolicited (no `id`).

```jsonc
{ "id": "7", "type": "ok" }
{ "id": "7", "type": "error", "error": "No video could be found in this tweet" }

{ "id": "7", "type": "formats", "title": "...", "channel": "...",
  "duration": 212, "player_clients": "",
  "formats": [ { "id": "137", "label": "1920x1080 mp4",
                 "audio_only": false, "filesize": 84211000, "acodec": "none" } ] }

{ "id": "8", "type": "item", "download_id": "3", "url": "...",
  "title": "...", "format_id": "137", "out_dir": "/home/me/Videos",
  "created_at": 1732000000000 }

{ "download_id": "3", "type": "progress",
  "pct": 42.5, "speed": "2.50MiB/s", "eta": "00:30" }

{ "download_id": "3", "type": "done" }
{ "download_id": "3", "type": "error", "error": "..." }
```

## Cookies

The extension reads cookies with `chrome.cookies` and sends them per request; the host
writes a temporary Netscape `cookies.txt` (mode 0600) and passes it to yt-dlp
(`--cookies` / `cookiefile`), deleting it afterwards.

```jsonc
{ "id": "9", "cmd": "fetch",
  "params": { "url": "...",
    "cookies": "# Netscape HTTP Cookie File\n.youtube.com\tTRUE\t/\tTRUE\t0\tID\tvalue\n" } }
```

This replaces `--cookies-from-browser`: no browser profile, no keyring, works while the
browser is closed. It is what resolves *"Sign in to confirm you're not a bot"*.

## Optional download params

- `container` — `""` (default) keeps whatever container yt-dlp wrote. `"opus"`
  or `"aac"` repackage an **audio-only** download with `ffmpeg -c copy`: Ogg
  Opus for `.opus`, ADTS for `.aac`. Both are stream copies, so the audio is
  identical (verified: the decoded-audio SHA-256 is unchanged; the ADTS muxer
  only adds a 7-byte header per frame). Ignored for video jobs, and a failure
  leaves the original file in place — a repackaging problem never fails a
  download. Note that `.m4a` is already the native AAC container, so `""` is the
  more compatible choice for AAC; `.aac` (ADTS) is more widely *recognised* as
  "an audio file" but plays in fewer players.
- `player_clients` — the `youtube:player_client=…` value the fetch ended up
  using (`fetch` returns it as `player_clients`). The download repeats it,
  because YouTube answers differently per player client and a format id listed by
  one can be rejected by another.

Both are optional, so a panel that does not send them keeps the old behaviour.

## Rules

1. `stdout` = JSON only; logging → stderr.
2. Never trust the extension: `url`, `out_dir` and `format_id` are user input.
   Validate `format_id` against `^[A-Za-z0-9+._-]+$`, `player_clients` against
   `^youtube:player_client=[A-Za-z0-9_+\-,]*$` and `container` against
   `container` against `{"" , "opus", "aac"}`, and shell-quote every path.
3. One long-lived connection; requests may be pipelined.
4. `download` returns `item` immediately, then streams `progress` events.