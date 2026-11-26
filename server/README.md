# mk-1 leaderboard

The service verifies signatures from registered cards and runs their submitted programs in WebAssembly built from the shell core and `content_gen.c`. It requires Node.js 24 and Zig.

## Run the service

```sh
install -d -m 700 "$HOME/.local/share/mk1-board"
node server/build-score.mjs
node --test server/*.test.mjs
node server/main.mjs
```

The service listens on `127.0.0.1:8787` by default. Set `MK1_BOARD_PORT` to change the port or `MK1_BOARD_DB` to specify an absolute path to the SQLite database. The default database path is `~/.local/share/mk1-board/leaderboard.sqlite`. Keep the database directory outside the web root and use the same path for the service and card registration.

Configure the reverse proxy to forward `/mk1/api/leaderboard` with the original path. With `MK1_BOARD_TRUST_CF_IP=1`, the proxy must set a verified `CF-Connecting-IP` on every request. Trust the incoming header only from Cloudflare IP addresses. For other connections, set it to the IP address of the client connecting to the proxy. By default, rate limits use the peer IP of the connection to the service.

## Register a card

Put a card in BOOTSEL mode and run the command on a computer with picotool 2.3.1. Specify a trusted SSH host with its host key pinned in `known_hosts` and the absolute path to the backend directory on the server.

```sh
node server/register-card.mjs board-vds /srv/mk1-shell
```

If several boards are connected, select one with `--serial`. The registration tool passes the card key to the server over SSH standard input. It enforces strict host key checking and runs SSH in batch mode. Use `--picotool` and `--remote-node` to override the executable paths. To revoke a card, pass its chip ID to `node server/admin.mjs revoke` on the server through standard input.

To register against a local database in PowerShell, run the following commands.

```powershell
$env:MK1_BOARD_DB = Join-Path (Get-Location) 'build/leaderboard-local/leaderboard.sqlite'
node server/register-card.mjs --local --picotool (Join-Path $env:USERPROFILE '.pico-sdk/picotool/2.3.1/picotool/picotool.exe')
```

## Scoring and trust

- `GET` returns `entries` and `goal`. The goal contains `task`, `name`, `maxQuality`, and `record`. `record` is `null` until a card qualifies. Set `limit` to a page size from 1 to 100 and `offset` to a starting position from 0 to 10,000. The default page size is 100. The response includes `nextOffset` when another page exists. Per client IP, the service allows 120 reads and 12 submissions or confirmations per minute.
- `POST` accepts JSON with a `token` and an optional `nickname` up to 24 characters long. The token can set a card's first nickname. Changing an existing nickname requires a fresh signature from the card.
- The server verifies the HMAC, decodes the bytecode, and reruns each submitted solution. One solution is enough to publish a result.
- The server reuses verification results for identical sets of programs while the process is running. It limits each card to six new verifications per minute.
- Manual gates award zero points. Each result has `quality`, the number of noisy packets decoded by a qualifying `noise floor` program, or `null` for a result without a qualifying program.
- `noise floor` awards 50 points for qualifying and `floor(150 × quality / record)` bonus points while the record is positive. Scores use the current record across registered cards, so a new record can lower the points of earlier submissions.
- The leaderboard keeps one best result per card. It prefers higher quality, then more solved tasks, fewer instructions, fewer cycles, and fewer ticks. Standings sort by score first, followed by solved tasks and the same tie-breakers, with earlier update times first.
- Keep your `top` link private. It includes the submitted programs and can be reused to publish them.
- The HMAC proves possession of the card key. Verifying the running firmware requires a separate hardware chain of trust.

## Change a nickname

Cards with an existing result can change their nickname through two JSON requests to `POST /mk1/api/leaderboard`.

1. `{"action":"nickname-challenge","token": "…","nickname": "…"}` returns a `nonce` of 32 lowercase hexadecimal characters and the normalized `nickname`.
2. The card uses its existing HMAC key to compute HMAC-SHA256 over the ASCII string `mk1/board-nickname/v1`, the byte `0x00`, the eight-byte chip ID, the 16-byte `nonce`, and the `nickname` encoded as UTF-8, in that order.
3. `{"action":"set-nickname","nonce": "…","proof": "…"}` sends the signature as 64 lowercase hexadecimal characters. The response contains the updated `entry`.

A nonce expires after two minutes. Successful verification updates the nickname and invalidates all pending challenges for the card in one transaction. Revoking the card also removes its active challenges.
