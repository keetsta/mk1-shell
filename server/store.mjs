import { randomBytes, timingSafeEqual } from "node:crypto";
import { homedir } from "node:os";
import { isAbsolute, join } from "node:path";
import { DatabaseSync } from "node:sqlite";

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
      updated_at TEXT NOT NULL
    ) STRICT;
  `);
  if (!db.prepare("SELECT 1 FROM pragma_table_info('cards') WHERE name = 'nickname'").get()) {
    db.exec("ALTER TABLE cards ADD COLUMN nickname TEXT");
  }
  db.exec("COMMIT");

  const register = db.prepare(`
    INSERT INTO cards (chip_id, mac_key, public_id) VALUES (?, ?, ?)
    ON CONFLICT (chip_id) DO NOTHING
  `);
  const revoke = db.prepare("DELETE FROM cards WHERE chip_id = ?");
  const lookup = db.prepare("SELECT mac_key FROM cards WHERE chip_id = ?");
  const rename = db.prepare("UPDATE cards SET nickname = ? WHERE chip_id = ?");
  const record = db.prepare(`
    INSERT INTO results (chip_id, programs, score, solved, instructions, cycles, ticks, updated_at)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT (chip_id) DO UPDATE SET
      programs = excluded.programs,
      score = excluded.score,
      solved = excluded.solved,
      instructions = excluded.instructions,
      cycles = excluded.cycles,
      ticks = excluded.ticks,
      updated_at = excluded.updated_at
    WHERE (excluded.score, excluded.solved, -excluded.instructions, -excluded.cycles, -excluded.ticks)
        > (results.score, results.solved, -results.instructions, -results.cycles, -results.ticks)
  `);
  const resultFields = `
    SELECT public_id AS card, nickname, score, solved, instructions, cycles, ticks, updated_at AS updatedAt
    FROM results INNER JOIN cards USING (chip_id)
  `;
  const entry = db.prepare(resultFields + "WHERE chip_id = ?");
  const entries = db.prepare(resultFields + `
    ORDER BY score DESC, solved DESC, instructions, cycles, ticks, updated_at, public_id
  `);

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
      db.exec("BEGIN IMMEDIATE");
      try {
        let published;
        const current = lookup.get(chipId)?.mac_key;
        if (current && timingSafeEqual(current, key)) {
          record.run(chipId, programs, result.score, result.solved, result.instructions, result.cycles,
            result.ticks, new Date().toISOString());
          if (nickname) rename.run(nickname, chipId);
          published = entry.get(chipId);
        }
        db.exec("COMMIT");
        return published;
      } catch (error) {
        db.exec("ROLLBACK");
        throw error;
      }
    },
    entries() {
      return entries.all();
    },
    close() {
      db.close();
    },
  };
}
