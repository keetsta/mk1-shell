import { createHash, createHmac, timingSafeEqual } from "node:crypto";
import { createServer } from "node:http";
import { isIP } from "node:net";
import { boardDatabasePath, openBoard } from "./store.mjs";

const route = "/mk1/api/leaderboard";
const maxTokenBytes = 1 + 8 + 1500 + 16;
const maxTokenLength = Math.ceil(maxTokenBytes * 4 / 3);
const maxBodyBytes = 2560;
const maxNicknameLength = 24;
const signatureBytes = 16;
const maxBoardPage = 100;
const maxBoardOffset = 10_000;
const noncePattern = /^[0-9a-f]{32}$/;
const proofPattern = /^[0-9a-f]{64}$/;

class RequestError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

function json(response, status, value, headers) {
  response.writeHead(status, {
    "Content-Type": "application/json; charset=utf-8",
    "Cache-Control": "no-store",
    "X-Content-Type-Options": "nosniff",
    ...headers,
  });
  response.end(JSON.stringify(value));
}

function minuteLimit(limit) {
  const clients = new Map();
  let window;
  return (ip) => {
    const minute = Math.floor(Date.now() / 60_000);
    if (minute !== window) {
      clients.clear();
      window = minute;
    }
    const count = clients.get(ip) ?? 0;
    if (count >= limit || (!count && clients.size >= 4096)) return false;
    clients.set(ip, count + 1);
    return true;
  };
}

function readStandings(board, query) {
  const params = new URLSearchParams(query);
  const limit = params.get("limit") ?? String(maxBoardPage);
  const offset = params.get("offset") ?? "0";
  if (!/^\d{1,3}$/.test(limit) || Number(limit) < 1 || Number(limit) > maxBoardPage ||
      !/^\d{1,5}$/.test(offset) || Number(offset) > maxBoardOffset ||
      params.getAll("limit").length > 1 || params.getAll("offset").length > 1) {
    throw new RequestError(400, "invalid leaderboard page");
  }
  const size = Number(limit);
  const start = Number(offset);
  const { entries, record } = board.standings(size + 1, start);
  const goal = { task: 6, name: "noise floor", maxQuality: 128, record };
  if (entries.length <= size) return { entries, goal };
  entries.pop();
  const nextOffset = start + size;
  return nextOffset <= maxBoardOffset ? { entries, goal, nextOffset } : { entries, goal };
}

async function readSubmission(request) {
  const { headers } = request;
  const type = headers["content-type"]?.split(";")[0].trim().toLowerCase();
  if (type !== "application/json" || headers["content-encoding"]) {
    throw new RequestError(415, "send the token as json");
  }
  if (Number(headers["content-length"]) > maxBodyBytes) {
    throw new RequestError(413, "submission is too large");
  }
  const chunks = [];
  let size = 0;
  for await (const chunk of request.iterator({ destroyOnReturn: false })) {
    size += chunk.length;
    if (size > maxBodyBytes) throw new RequestError(413, "submission is too large");
    chunks.push(chunk);
  }
  let input;
  try {
    input = JSON.parse(Buffer.concat(chunks, size).toString("utf8"));
  } catch {
    throw new RequestError(400, "invalid json");
  }
  if (!input || typeof input !== "object" || Array.isArray(input)) {
    throw new RequestError(400, "invalid submission");
  }
  return input;
}

function requireFields(input, fields) {
  if (Object.keys(input).some((field) => !fields.includes(field))) {
    throw new RequestError(400, "invalid submission fields");
  }
}

function readNickname(value) {
  if (typeof value !== "string") throw new RequestError(400, "invalid nickname");
  const nickname = value.trim().normalize("NFC");
  if (Array.from(nickname).length > maxNicknameLength || /[\p{Cc}\p{Cf}\p{Cs}\p{Zl}\p{Zp}]/u.test(nickname)) {
    throw new RequestError(400, "nickname must be at most 24 characters without control characters");
  }
  return nickname || null;
}

function readClaim(token, board) {
  if (typeof token !== "string" || token.length > maxTokenLength || !/^[A-Za-z0-9_-]+$/.test(token)) {
    throw new RequestError(400, "invalid token");
  }
  const bytes = Buffer.from(token, "base64url");
  if (bytes.length < 1 + 8 + signatureBytes || bytes.length > maxTokenBytes ||
      bytes[0] !== 1 || bytes.toString("base64url") !== token) {
    throw new RequestError(400, "invalid token");
  }
  const chipId = bytes.subarray(1, 9).toString("hex");
  const key = board.key(chipId);
  const data = bytes.subarray(0, bytes.length - signatureBytes);
  const mac = bytes.subarray(data.length);
  if (!key || !timingSafeEqual(createHmac("sha256", key).update(data).digest().subarray(0, signatureBytes), mac)) {
    throw new RequestError(403, "card signature could not be verified");
  }
  return { chipId, key, programs: data.subarray(9) };
}

function cachedVerification(verifyPrograms) {
  const cache = new Map();
  const allow = minuteLimit(6);
  return ({ chipId, programs }) => {
    const id = createHash("sha256").update(programs).digest("hex");
    if (cache.has(id)) return cache.get(id);
    if (!allow(chipId)) throw new RequestError(429, "too many new results for this card, try again in a minute");
    const result = verifyPrograms(programs);
    if (cache.size >= 256) cache.delete(cache.keys().next().value);
    cache.set(id, result);
    return result;
  };
}

function processSubmission(input, board, verify) {
  switch (input.action) {
    case undefined: {
      requireFields(input, ["token", "nickname"]);
      const nickname = Object.hasOwn(input, "nickname") ? readNickname(input.nickname) : null;
      const claim = readClaim(input.token, board);
      const result = verify(claim);
      if (!result) throw new RequestError(422, "solutions did not pass verification");
      const entry = board.publish(claim.chipId, claim.key, claim.programs, result, nickname);
      if (!entry) throw new RequestError(403, "card signature could not be verified");
      return { entry };
    }
    case "nickname-challenge": {
      requireFields(input, ["action", "token", "nickname"]);
      const nickname = readNickname(input.nickname);
      if (!nickname) throw new RequestError(400, "enter a nickname");
      const { chipId, key } = readClaim(input.token, board);
      const nonce = board.issueNicknameChallenge(chipId, key, nickname, Date.now());
      if (!nonce) throw new RequestError(409, "nickname change is unavailable, publish a result or try again later");
      return { nonce, nickname };
    }
    case "set-nickname": {
      requireFields(input, ["action", "nonce", "proof"]);
      const { nonce, proof } = input;
      if (typeof nonce !== "string" || !noncePattern.test(nonce) || typeof proof !== "string" || !proofPattern.test(proof)) {
        throw new RequestError(400, "invalid nickname proof");
      }
      const pending = board.nicknameChallenge(nonce, Date.now());
      if (!pending) throw new RequestError(403, "nickname confirmation is invalid or expired");
      const expected = createHmac("sha256", pending.key)
        .update("mk1/board-nickname/v1\0", "ascii")
        .update(Buffer.from(pending.chipId, "hex"))
        .update(Buffer.from(nonce, "hex"))
        .update(pending.nickname, "utf8")
        .digest();
      if (!timingSafeEqual(expected, Buffer.from(proof, "hex"))) {
        throw new RequestError(403, "nickname confirmation is invalid or expired");
      }
      const entry = board.changeNickname(nonce, pending.key, Date.now());
      if (!entry) throw new RequestError(403, "nickname confirmation is invalid or expired");
      return { entry };
    }
    default:
      throw new RequestError(400, "invalid submission action");
  }
}

export function createBoardServer({ board, verifyPrograms, trustCloudflare = false }) {
  const allowRead = minuteLimit(120);
  const allowSubmission = minuteLimit(12);
  const verify = cachedVerification(verifyPrograms);
  return createServer({ requestTimeout: 10_000, headersTimeout: 5_000, connectionsCheckingInterval: 1000 }, async (request, response) => {
    try {
      const [path, query] = request.url.split("?", 2);
      if (path !== route) {
        json(response, 404, { error: "not found" });
      } else if (request.method === "GET") {
        const proxyAddress = trustCloudflare && request.headers["cf-connecting-ip"];
        const ip = typeof proxyAddress === "string" && isIP(proxyAddress) ? proxyAddress : request.socket.remoteAddress;
        if (!allowRead(ip)) throw new RequestError(429, "too many leaderboard requests, try again in a minute");
        json(response, 200, readStandings(board, query));
      } else if (request.method === "POST") {
        const ip = trustCloudflare ? request.headers["cf-connecting-ip"] : request.socket.remoteAddress;
        if (typeof ip !== "string" || !isIP(ip)) throw new RequestError(403, "client address is unavailable");
        if (!allowSubmission(ip)) throw new RequestError(429, "too many submissions, try again in a minute");

        json(response, 200, processSubmission(await readSubmission(request), board, verify));
      } else {
        json(response, 405, { error: "method not allowed" }, { Allow: "GET, POST" });
      }
    } catch (error) {
      if (response.destroyed || response.headersSent) return;
      if (error instanceof RequestError) {
        const headers = error.status === 429 ? { "Retry-After": "60" } : {};
        if (!request.complete) {
          headers.Connection = "close";
          request.resume();
        }
        json(response, error.status, { error: error.message }, headers);
      } else {
        console.error("leaderboard request failed");
        json(response, 500, { error: "leaderboard is unavailable" });
      }
    }
  });
}

async function main() {
  const port = Number(process.env.MK1_BOARD_PORT ?? 8787);
  if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error("invalid MK1_BOARD_PORT");
  const { loadScorer } = await import("./scoring.mjs");
  const verifyPrograms = await loadScorer();
  process.umask(0o077);
  const board = openBoard(boardDatabasePath());
  const server = createBoardServer({
    board,
    verifyPrograms,
    trustCloudflare: process.env.MK1_BOARD_TRUST_CF_IP === "1",
  });
  server.on("close", () => board.close());
  for (const signal of ["SIGINT", "SIGTERM"]) process.once(signal, () => server.close());
  server.on("error", (error) => {
    console.error("leaderboard listener failed", error.code);
    board.close();
    process.exitCode = 1;
  });
  server.listen(port, "127.0.0.1", () => console.log(`leaderboard listening on 127.0.0.1:${port}`));
}

if (import.meta.main) {
  main().catch((error) => {
    console.error(error.message);
    process.exitCode = 1;
  });
}
