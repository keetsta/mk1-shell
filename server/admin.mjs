import { boardDatabasePath, openBoard } from "./store.mjs";

const chipPattern = /^[0-9a-f]{16}$/;

async function readInput() {
  let input = "";
  process.stdin.setEncoding("utf8");
  for await (const chunk of process.stdin) {
    input += chunk;
    if (input.length > 256) throw new Error("invalid registration input");
  }
  return input.trim();
}

async function main() {
  const [action, ...extra] = process.argv.slice(2);
  if (extra.length || (action !== "register" && action !== "revoke")) {
    throw new Error("usage: node server/admin.mjs register|revoke < record");
  }
  const input = await readInput();
  let registration;
  if (action === "register") {
    try {
      registration = JSON.parse(input);
    } catch {
      throw new Error("invalid registration record");
    }
    if (!registration || typeof registration.chipId !== "string" || !chipPattern.test(registration.chipId) ||
        typeof registration.key !== "string" || !/^[0-9a-f]{64}$/.test(registration.key)) {
      throw new Error("invalid registration record");
    }
  } else if (!chipPattern.test(input)) {
    throw new Error("invalid chip id");
  }

  process.umask(0o077);
  const board = openBoard(boardDatabasePath());
  try {
    if (action === "register") {
      if (!board.register(registration.chipId, Buffer.from(registration.key, "hex"))) {
        throw new Error("card already registered with a different key");
      }
      console.log("card registered");
    } else {
      if (!board.revoke(input)) throw new Error("card not registered");
      console.log("card revoked");
    }
  } finally {
    board.close();
  }
}

main().catch((error) => {
  console.error(error.code ? "leaderboard database error" : error.message);
  process.exitCode = 1;
});
