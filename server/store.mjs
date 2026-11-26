import { randomBytes, timingSafeEqual } from "node:crypto";
import { homedir } from "node:os";
import { isAbsolute, join } from "node:path";
import { DatabaseSync } from "node:sqlite";

const nicknameChallengeLifetime = 120_000;
const maxCardChallenges = 8;
const maxChallenges = 4096;

export function boardDatabasePath() {
  const path = process.env.MK1_BOARD_DB ?? join(homedir(), ".local", "share", "mk1-board", "leaderboard.sqlite");
  if (!isAbsolute(path)) throw new Error("set MK1_BOARD_DB to an absolute database path");
  return path;
}

export function openBoard(path) {
  const db = new DatabaseSync(path);
  db.exec(`
    PRAGMA busy_timeout = 5000;
    PRAGMA journal_mode = WAL;
    PRAGMA foreign_keys = ON;
  `);
  db.exec(`BEGIN IMMEDIATE;
    CREATE TABLE IF NOT EXISTS cards (
      chip_id TEXT PRIMARY KEY,
      mac_key BLOB NOT NULL CHECK (length(mac_key) = 32),
      public_id TEXT NOT NULL UNIQUE,
      nickname TEXT
    ) STRICT;
    CREATE TABLE IF NOT EXISTS results (
      chip_id TEXT PRIMARY KEY REFERENCES cards (chip_id) ON DELETE CASCADE,
      programs BLOB NOT NULL,
      score INTEGER NOT NULL,
      solved INTEGER NOT NULL,
      instructions INTEGER NOT NULL,
      cycles INTEGER NOT NULL,
      ticks INTEGER NOT NULL,
      updated_at TEXT NOT NULL,
      quality INTEGER CHECK (quality BETWEEN 0 AND 128)
    ) STRICT;
    CREATE INDEX IF NOT EXISTS result_rank ON results (score DESC, solved DESC, instructions, cycles, ticks, updated_at);
    CREATE TABLE IF NOT EXISTS nickname_challenges (
      nonce TEXT PRIMARY KEY,
      chip_id TEXT NOT NULL REFERENCES cards (chip_id) ON DELETE CASCADE,
      nickname TEXT NOT NULL,
      expires_at INTEGER NOT NULL,
      UNIQUE (chip_id, nickname)
    ) STRICT;
    CREATE INDEX IF NOT EXISTS nickname_expiry ON nickname_challenges (expires_at);
  `);
  if (!db.prepare("SELECT 1 FROM pragma_table_info('cards') WHERE name = 'nickname'").get()) {
    db.exec("ALTER TABLE cards ADD COLUMN nickname TEXT");
  }
  if (!db.prepare("SELECT 1 FROM pragma_table_info('results') WHERE name = 'quality'").get()) {
    db.exec("ALTER TABLE results ADD COLUMN quality INTEGER CHECK (quality BETWEEN 0 AND 128)");
  }
  db.exec("COMMIT");

  const register = db.prepare(`
    INSERT INTO cards (chip_id, mac_key, public_id) VALUES (?, ?, ?)
    ON CONFLICT (chip_id) DO NOTHING
  `);
  const revoke = db.prepare("DELETE FROM cards WHERE chip_id = ?");
  const lookup = db.prepare("SELECT mac_key FROM cards WHERE chip_id = ?");
  const setInitialNickname = db.prepare("UPDATE cards SET nickname = ? WHERE chip_id = ? AND nickname IS NULL");
  const rename = db.prepare("UPDATE cards SET nickname = ? WHERE chip_id = ?");
  const record = db.prepare(`
    INSERT INTO results (chip_id, programs, score, solved, instructions, cycles, ticks, updated_at, quality)
    VALUES (?, ?, 0, ?, ?, ?, ?, ?, ?)
    ON CONFLICT (chip_id) DO UPDATE SET
      programs = excluded.programs,
      score = excluded.score,
      solved = excluded.solved,
      instructions = excluded.instructions,
      cycles = excluded.cycles,
      ticks = excluded.ticks,
      updated_at = excluded.updated_at,
      quality = excluded.quality
    WHERE (coalesce(excluded.quality, -1), excluded.solved, -excluded.instructions, -excluded.cycles, -excluded.ticks)
        > (coalesce(results.quality, -1), results.solved, -results.instructions, -results.cycles, -results.ticks)
  `);
  const goalRecord = db.prepare("SELECT max(quality) AS quality FROM results");
  const resultFields = `
    SELECT public_id AS card, nickname,
      CASE WHEN quality IS NULL THEN results.score
           ELSE 50 + coalesce(150 * quality / nullif(CAST($record AS INTEGER), 0), 0) END AS score,
      solved, instructions, cycles, ticks, updated_at AS updatedAt, quality
    FROM results INNER JOIN cards USING (chip_id)
  `;
  const entry = db.prepare(resultFields + "WHERE chip_id = ?");
  const hasEntry = db.prepare("SELECT 1 FROM results WHERE chip_id = ?");
  const entries = db.prepare(resultFields + `
    ORDER BY score DESC, solved DESC, instructions, cycles, ticks, updated_at, public_id
    LIMIT ? OFFSET ?
  `);
  const pruneChallenges = db.prepare("DELETE FROM nickname_challenges WHERE expires_at <= ?");
  const clearChallenges = db.prepare("DELETE FROM nickname_challenges WHERE chip_id = ?");
  const countChallenges = db.prepare("SELECT count(*) AS count FROM nickname_challenges");
  const countCardChallenges = db.prepare("SELECT count(*) AS count FROM nickname_challenges WHERE chip_id = ?");
  const pendingChallenge = db.prepare("SELECT nonce FROM nickname_challenges WHERE chip_id = ? AND nickname = ?");
  const insertChallenge = db.prepare("INSERT INTO nickname_challenges (nonce, chip_id, nickname, expires_at) VALUES (?, ?, ?, ?)");
  const challenge = db.prepare(`
    SELECT chip_id AS chipId, nickname_challenges.nickname, mac_key AS key
    FROM nickname_challenges INNER JOIN cards USING (chip_id)
    WHERE nonce = ? AND expires_at > ?
  `);

  function transaction(operation, write = true) {
    db.exec(write ? "BEGIN IMMEDIATE" : "BEGIN");
    try {
      const value = operation();
      db.exec("COMMIT");
      return value;
    } catch (error) {
      db.exec("ROLLBACK");
      throw error;
    }
  }

  function readEntry(chipId) {
    return entry.get({ record: goalRecord.get().quality }, chipId);
  }

  return {
    register(chipId, key) {
      const added = register.run(chipId, key, randomBytes(6).toString("hex"));
      if (added.changes) return true;
      const existing = lookup.get(chipId)?.mac_key;
      return !!existing && timingSafeEqual(existing, key);
    },
    revoke(chipId) {
      return revoke.run(chipId).changes !== 0;
    },
    key(chipId) {
      return lookup.get(chipId)?.mac_key;
    },
    publish(chipId, key, programs, result, nickname) {
      return transaction(() => {
        const current = lookup.get(chipId)?.mac_key;
        if (!current || !timingSafeEqual(current, key)) return;
        record.run(chipId, programs, result.solved, result.instructions, result.cycles,
          result.ticks, new Date().toISOString(), result.quality);
        if (nickname) setInitialNickname.run(nickname, chipId);
        return readEntry(chipId);
      });
    },
    issueNicknameChallenge(chipId, key, nickname, now) {
      return transaction(() => {
        const current = lookup.get(chipId)?.mac_key;
        if (!current || !timingSafeEqual(current, key) || !hasEntry.get(chipId)) return;
        pruneChallenges.run(now);
        const pending = pendingChallenge.get(chipId, nickname);
        if (pending) return pending.nonce;
        if (countCardChallenges.get(chipId).count >= maxCardChallenges || countChallenges.get().count >= maxChallenges) return;
        const nonce = randomBytes(16).toString("hex");
        insertChallenge.run(nonce, chipId, nickname, now + nicknameChallengeLifetime);
        return nonce;
      });
    },
    nicknameChallenge(nonce, now) {
      return challenge.get(nonce, now);
    },
    changeNickname(nonce, key, now) {
      return transaction(() => {
        const pending = challenge.get(nonce, now);
        if (!pending || !timingSafeEqual(pending.key, key)) return;
        clearChallenges.run(pending.chipId);
        rename.run(pending.nickname, pending.chipId);
        return readEntry(pending.chipId);
      });
    },
    standings(limit, offset) {
      return transaction(() => {
        const record = goalRecord.get().quality;
        return { entries: entries.all({ record }, limit, offset), record };
      }, false);
    },
    close() {
      db.close();
    },
  };
}
