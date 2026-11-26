import assert from "node:assert/strict";
import { createHmac } from "node:crypto";
import { once } from "node:events";
import { mkdtempSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { DatabaseSync } from "node:sqlite";
import test from "node:test";
import { createBoardServer } from "./main.mjs";
import { openBoard } from "./store.mjs";

const chip = "0102030405060708";
const key = Buffer.alloc(32, 7);
const programs = Buffer.of(1, 2, 3, 4);
const verified = { quality: null, solved: 2, instructions: 17, cycles: 120, ticks: 90 };
const partialPrograms = Buffer.of(1, 2, 3);
const partial = { quality: null, solved: 1, instructions: 11, cycles: 70, ticks: 40 };
const goal = { task: 6, name: "noise floor", maxQuality: 128, record: null };

function token(entries = programs, id = chip, secret = key) {
  const data = Buffer.concat([Buffer.of(1), Buffer.from(id, "hex"), entries]);
  const mac = createHmac("sha256", secret).update(data).digest().subarray(0, 16);
  return Buffer.concat([data, mac]).toString("base64url");
}

function nicknameProof(nonce, nickname) {
  return createHmac("sha256", key)
    .update("mk1/board-nickname/v1\0", "ascii")
    .update(Buffer.from(chip, "hex"))
    .update(Buffer.from(nonce, "hex"))
    .update(nickname, "utf8")
    .digest("hex");
}

async function fixture(t, trustCloudflare = false, verifyGoal) {
  const board = openBoard(":memory:");
  let verificationCalls = 0;
  assert.ok(board.register(chip, key));
  const server = createBoardServer({
    board,
    verifyPrograms(entries) {
      verificationCalls++;
      if (entries.equals(programs)) return verified;
      if (entries.equals(partialPrograms)) return partial;
      return verifyGoal?.(entries) ?? null;
    },
    trustCloudflare,
  });
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  t.after(async () => {
    const done = once(server, "close");
    server.close();
    await done;
    board.close();
  });
  const url = `http://127.0.0.1:${server.address().port}/mk1/api/leaderboard`;
  return {
    board,
    verificationCalls: () => verificationCalls,
    request: (query = "", headers) => fetch(url + query, { headers }),
    read: (query = "") => fetch(url + query).then((res) => res.json()),
    submit: (input, headers) => fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json", ...headers },
      body: JSON.stringify(input),
    }),
  };
}

test("publishes only verified data and keeps one entry per card", async (t) => {
  const { board, read, submit } = await fixture(t);
  assert.deepEqual(await read(), { entries: [], goal });
  const valid = token();
  const first = await submit({ token: valid });
  assert.equal(first.status, 200);
  const { entry } = await first.json();
  assert.match(entry.card, /^[0-9a-f]{12}$/);
  assert.notEqual(entry.card, chip);
  assert.deepEqual(entry, { ...verified, score: 0, card: entry.card, nickname: null, updatedAt: entry.updatedAt });
  assert.deepEqual(await (await submit({ token: valid })).json(), { entry });
  assert.deepEqual(await read(), { entries: [entry], goal });
  assert.ok(board.register(chip, key));
  assert.equal(board.register(chip, Buffer.alloc(32, 8)), false);
  assert.ok(board.revoke(chip));
  assert.deepEqual(await read(), { entries: [], goal });
  assert.equal((await submit({ token: valid })).status, 403);
});

test("publishes partial progress and preserves the nickname on replay", async (t) => {
  const { read, submit } = await fixture(t);
  const first = await (await submit({ token: token(partialPrograms), nickname: "  cafe\u0301  " })).json();
  assert.equal(first.entry.nickname, "café");
  assert.equal(first.entry.solved, 1);
  const better = await (await submit({ token: token() })).json();
  assert.equal(better.entry.solved, 2);
  assert.equal(better.entry.nickname, "café");
  assert.deepEqual(await (await submit({ token: token(partialPrograms), nickname: "new name" })).json(), better);
  assert.deepEqual(await read(), { entries: [better.entry], goal });
});

test("updates all points as records change and keeps each card's best quality", async (t) => {
  const attempts = new Map([
    [0, { ...partial, quality: 0 }],
    [1, { ...verified, quality: 41 }],
    [2, { ...partial, quality: 39, instructions: 1, cycles: 1 }],
    [3, { ...verified, quality: 100 }],
    [4, { ...partial, quality: 60 }],
  ]);
  const { board, read, submit } = await fixture(t, false, (entries) => attempts.get(entries[2]));
  const secondChip = "0102030405060709";
  assert.ok(board.register(secondChip, key));
  const submitGoal = async (attempt, id = chip) => {
    const response = await submit({ token: token(Buffer.of(6, 1, attempt), id) });
    assert.equal(response.status, 200);
    return (await response.json()).entry;
  };

  const zero = await submitGoal(0);
  assert.equal(zero.quality, 0);
  assert.equal(zero.score, 50);
  assert.deepEqual((await read()).goal, { ...goal, record: 0 });
  const first = await submitGoal(1);
  assert.equal(first.quality, 41);
  assert.equal(first.score, 200);
  assert.deepEqual(await submitGoal(2), first);

  const second = await submitGoal(3, secondChip);
  assert.equal(second.quality, 100);
  assert.equal(second.score, 200);
  assert.deepEqual(await read(), { entries: [second, { ...first, score: 111 }], goal: { ...goal, record: 100 } });
  assert.deepEqual(await submitGoal(1), { ...first, score: 111 });

  const improved = await submitGoal(4);
  assert.equal(improved.quality, 60);
  assert.equal(improved.score, 140);
  assert.ok(board.revoke(secondChip));
  assert.deepEqual(await read(), { entries: [{ ...improved, score: 200 }], goal: { ...goal, record: 60 } });
  assert.deepEqual(await read("?offset=1"), { entries: [], goal: { ...goal, record: 60 } });
});

test("migrates old saved leaderboard results in place", (t) => {
  const dir = mkdtempSync(join(tmpdir(), "mk1-board-"));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const path = join(dir, "board.sqlite");
  const legacy = new DatabaseSync(path);
  legacy.exec(`
    CREATE TABLE cards (chip_id TEXT PRIMARY KEY, mac_key BLOB NOT NULL,
      public_id TEXT NOT NULL UNIQUE, nickname TEXT) STRICT;
    CREATE TABLE results (chip_id TEXT PRIMARY KEY REFERENCES cards (chip_id) ON DELETE CASCADE,
      programs BLOB NOT NULL, score INTEGER NOT NULL, solved INTEGER NOT NULL, instructions INTEGER NOT NULL,
      cycles INTEGER NOT NULL, ticks INTEGER NOT NULL, updated_at TEXT NOT NULL) STRICT;
  `);
  const card = "123456789abc";
  const updatedAt = "2026-01-01T00:00:00.000Z";
  legacy.prepare("INSERT INTO cards VALUES (?, ?, ?, ?)").run(chip, key, card, "legacy");
  legacy.prepare("INSERT INTO results VALUES (?, ?, ?, ?, ?, ?, ?, ?)").run(chip, programs, 0,
    verified.solved, verified.instructions, verified.cycles, verified.ticks, updatedAt);
  legacy.close();

  const board = openBoard(path);
  try {
    const entry = { ...verified, score: 0, card, nickname: "legacy", updatedAt };
    const standings = board.standings(10, 0);
    assert.equal(standings.record, null);
    assert.equal(standings.entries.length, 1);
    assert.deepEqual({ ...standings.entries[0] }, entry);
    assert.deepEqual({ ...board.publish(chip, key, partialPrograms, partial) }, entry);
  } finally {
    board.close();
  }
});

test("changing a nickname requires a one-time card proof for the canonical nickname", async (t) => {
  const { read, submit } = await fixture(t);
  const request = { action: "nickname-challenge", token: token(), nickname: "  cafe\u0301  " };
  assert.equal((await submit(request)).status, 409);
  const { entry } = await (await submit({ token: token(), nickname: "first name" })).json();
  const challenge = await (await submit(request)).json();
  assert.match(challenge.nonce, /^[0-9a-f]{32}$/);
  assert.equal(challenge.nickname, "café");
  assert.deepEqual(await (await submit(request)).json(), challenge);
  const setName = { action: "set-nickname", nonce: challenge.nonce };
  assert.equal((await submit({ ...setName, proof: nicknameProof(challenge.nonce, "other name") })).status, 403);
  assert.deepEqual(await read(), { entries: [entry], goal });

  const signed = { ...setName, proof: nicknameProof(challenge.nonce, challenge.nickname) };
  const changed = { ...entry, nickname: challenge.nickname };
  assert.deepEqual(await (await submit(signed)).json(), { entry: changed });
  assert.equal((await submit(signed)).status, 403);
  assert.deepEqual(await read(), { entries: [changed], goal });
});

test("expired and revoked nickname challenges cannot be used", async (t) => {
  const { board, submit } = await fixture(t);
  assert.equal((await submit({ token: token() })).status, 200);
  const expired = board.issueNicknameChallenge(chip, key, "stale", Date.now() - 121_000);
  assert.equal((await submit({ action: "set-nickname", nonce: expired, proof: nicknameProof(expired, "stale") })).status, 403);
  const pending = await (await submit({ action: "nickname-challenge", token: token(), nickname: "active" })).json();
  assert.ok(board.revoke(chip));
  assert.ok(board.register(chip, key));
  assert.equal((await submit({ action: "set-nickname", nonce: pending.nonce, proof: nicknameProof(pending.nonce, "active") })).status, 403);
});

test("rejects unsigned, unregistered, malformed and unverified submissions", async (t) => {
  const { read, submit } = await fixture(t);
  assert.equal((await submit({ token: token(programs, "1112131415161718") })).status, 403);
  assert.equal((await submit({ token: token(programs, chip, Buffer.alloc(32, 8)) })).status, 403);
  assert.equal((await submit({ token: token() + "=" })).status, 400);
  assert.equal((await submit({ token: token(), score: 1_000_000 })).status, 400);
  assert.equal((await submit({ token: "A".repeat(2700) })).status, 413);
  assert.equal((await submit({ token: token(), nickname: 42 })).status, 400);
  assert.equal((await submit({ token: token(), nickname: "a".repeat(25) })).status, 400);
  assert.equal((await submit({ token: token(), nickname: "bad\u202e" })).status, 400);
  assert.equal((await submit({ token: token(Buffer.of(1, 0)) })).status, 422);
  assert.deepEqual(await read(), { entries: [], goal });
});

test("proxy submissions require a valid Cloudflare address", async (t) => {
  const { submit } = await fixture(t, true);
  assert.equal((await submit({ token: token() })).status, 403);
  assert.equal((await submit({ token: token() }, { "CF-Connecting-IP": "invalid" })).status, 403);
  assert.equal((await submit({ token: token() }, { "CF-Connecting-IP": "192.0.2.1" })).status, 200);
});

test("bounds leaderboard pages and keeps their ranking order", async (t) => {
  const { board, read, request } = await fixture(t);
  for (const quality of [1, 3, 2]) {
    const id = `${chip.slice(0, -1)}${quality}`;
    assert.ok(board.register(id, key));
    assert.ok(board.publish(id, key, programs, { ...verified, quality }));
  }
  const first = await read("?limit=2");
  assert.deepEqual(first.entries.map((entry) => entry.score), [200, 150]);
  assert.equal(first.nextOffset, 2);
  const second = await read("?limit=2&offset=2");
  assert.deepEqual(second.entries.map((entry) => entry.score), [100]);
  assert.equal(second.nextOffset, undefined);
  for (const query of ["?limit=101", "?offset=10001", "?limit=1&limit=2"]) {
    assert.equal((await request(query)).status, 400);
  }
});

test("limits public reads per address", async (t) => {
  t.mock.timers.enable({ apis: ["Date"], now: 1_000 });
  const { request } = await fixture(t, true);
  for (let i = 0; i < 120; i++) {
    assert.deepEqual(await (await request("", { "CF-Connecting-IP": "192.0.2.1" })).json(), { entries: [], goal });
  }
  const limited = await request("", { "CF-Connecting-IP": "192.0.2.1" });
  assert.equal(limited.status, 429);
  assert.equal(limited.headers.get("retry-after"), "60");
  assert.equal((await request("", { "CF-Connecting-IP": "192.0.2.2" })).status, 200);
});

test("reuses verification for identical programs but limits new work per card", async (t) => {
  t.mock.timers.enable({ apis: ["Date"], now: 1_000 });
  const { submit, verificationCalls } = await fixture(t, true);
  const headers = { "CF-Connecting-IP": "192.0.2.1" };
  assert.equal((await submit({ token: token() }, headers)).status, 200);
  assert.equal((await submit({ token: token() }, headers)).status, 200);
  assert.equal(verificationCalls(), 1);
  for (let i = 0; i < 5; i++) {
    assert.equal((await submit({ token: token(Buffer.of(i)) }, headers)).status, 422);
  }
  assert.equal((await submit({ token: token(Buffer.of(5)) }, { "CF-Connecting-IP": "192.0.2.2" })).status, 429);
  assert.equal(verificationCalls(), 6);
  assert.equal((await submit({ token: token() }, headers)).status, 200);
});
