import assert from "node:assert/strict";
import { createHmac } from "node:crypto";
import { once } from "node:events";
import test from "node:test";
import { createBoardServer } from "./main.mjs";
import { openBoard } from "./store.mjs";

const chip = "0102030405060708";
const key = Buffer.alloc(32, 7);
const programs = Buffer.of(1, 2, 3, 4);
const verified = { score: 0, solved: 2, instructions: 17, cycles: 120, ticks: 90 };
const partialPrograms = Buffer.of(1, 2, 3);
const partial = { score: 0, solved: 1, instructions: 11, cycles: 70, ticks: 40 };

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

async function fixture(t, trustCloudflare = false) {
  const board = openBoard(":memory:");
  let verificationCalls = 0;
  assert.ok(board.register(chip, key));
  const server = createBoardServer({
    board,
    verifyPrograms(entries) {
      verificationCalls++;
      if (entries.equals(programs)) return verified;
      if (entries.equals(partialPrograms)) return partial;
      return null;
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
  assert.deepEqual(await read(), { entries: [] });
  const valid = token();
  const first = await submit({ token: valid });
  assert.equal(first.status, 200);
  const { entry } = await first.json();
  assert.match(entry.card, /^[0-9a-f]{12}$/);
  assert.notEqual(entry.card, chip);
  assert.deepEqual(entry, { ...verified, card: entry.card, nickname: null, updatedAt: entry.updatedAt });
  assert.deepEqual(await (await submit({ token: valid })).json(), { entry });
  assert.deepEqual(await read(), { entries: [entry] });
  assert.ok(board.register(chip, key));
  assert.equal(board.register(chip, Buffer.alloc(32, 8)), false);
  assert.ok(board.revoke(chip));
  assert.deepEqual(await read(), { entries: [] });
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
  assert.deepEqual(await read(), { entries: [better.entry] });
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
  assert.deepEqual(await read(), { entries: [entry] });

  const signed = { ...setName, proof: nicknameProof(challenge.nonce, challenge.nickname) };
  const changed = { ...entry, nickname: challenge.nickname };
  assert.deepEqual(await (await submit(signed)).json(), { entry: changed });
  assert.equal((await submit(signed)).status, 403);
  assert.deepEqual(await read(), { entries: [changed] });
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
  assert.deepEqual(await read(), { entries: [] });
});

test("proxy submissions require a valid Cloudflare address", async (t) => {
  const { submit } = await fixture(t, true);
  assert.equal((await submit({ token: token() })).status, 403);
  assert.equal((await submit({ token: token() }, { "CF-Connecting-IP": "invalid" })).status, 403);
  assert.equal((await submit({ token: token() }, { "CF-Connecting-IP": "192.0.2.1" })).status, 200);
});

test("bounds leaderboard pages and keeps their ranking order", async (t) => {
  const { board, read, request } = await fixture(t);
  for (const score of [1, 3, 2]) {
    const id = `${chip.slice(0, -1)}${score}`;
    assert.ok(board.register(id, key));
    assert.ok(board.publish(id, key, programs, { ...verified, score }));
  }
  const first = await read("?limit=2");
  assert.deepEqual(first.entries.map((entry) => entry.score), [3, 2]);
  assert.equal(first.nextOffset, 2);
  const second = await read("?limit=2&offset=2");
  assert.deepEqual(second.entries.map((entry) => entry.score), [1]);
  assert.equal(second.nextOffset, undefined);
  for (const query of ["?limit=101", "?offset=10001", "?limit=1&limit=2"]) {
    assert.equal((await request(query)).status, 400);
  }
});

test("limits public reads per address", async (t) => {
  t.mock.timers.enable({ apis: ["Date"], now: 1_000 });
  const { request } = await fixture(t, true);
  for (let i = 0; i < 120; i++) {
    assert.deepEqual(await (await request("", { "CF-Connecting-IP": "192.0.2.1" })).json(), { entries: [] });
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
