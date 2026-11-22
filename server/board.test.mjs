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

async function fixture(t, trustCloudflare = false) {
  const board = openBoard(":memory:");
  assert.ok(board.register(chip, key));
  const server = createBoardServer({
    board,
    verifyPrograms(entries) {
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
    read: () => fetch(url).then((res) => res.json()),
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

test("publishes partial progress and renames without replacing a better result", async (t) => {
  const { read, submit } = await fixture(t);
  const first = await (await submit({ token: token(partialPrograms), nickname: "  cafe\u0301  " })).json();
  assert.equal(first.entry.nickname, "café");
  assert.equal(first.entry.solved, 1);
  const better = await (await submit({ token: token() })).json();
  assert.equal(better.entry.solved, 2);
  assert.equal(better.entry.nickname, "café");
  const renamed = await (await submit({ token: token(partialPrograms), nickname: "new name" })).json();
  assert.deepEqual(renamed.entry, { ...better.entry, nickname: "new name" });
  assert.deepEqual(await read(), { entries: [renamed.entry] });
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
