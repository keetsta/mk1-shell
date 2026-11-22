import { createHmac, timingSafeEqual } from "node:crypto";
import { createServer } from "node:http";
import { isIP } from "node:net";
import { boardDatabasePath, openBoard } from "./store.mjs";

const route = "/mk1/api/leaderboard";
const maxTokenBytes = 1 + 8 + 1500 + 16;
const maxTokenLength = Math.ceil(maxTokenBytes * 4 / 3);
const maxBodyBytes = 2560;
const maxNicknameLength = 24;
const signatureBytes = 16;

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

function submissionLimit() {
  const clients = new Map();
  let window;
  return (ip) => {
    const minute = Math.floor(Date.now() / 60_000);
    if (minute !== window) {
      clients.clear();
      window = minute;
    }
    const count = clients.get(ip) ?? 0;
    if (count >= 12 || (!count && clients.size >= 4096)) return false;
    clients.set(ip, count + 1);
    return true;
  };
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
  if (!input || typeof input.token !== "string" ||
      Object.keys(input).some((field) => field !== "token" && field !== "nickname")) {
    throw new RequestError(400, "submit a token");
  }
  let nickname = null;
  if (Object.hasOwn(input, "nickname")) {
    if (typeof input.nickname !== "string") throw new RequestError(400, "invalid nickname");
    nickname = input.nickname.trim().normalize("NFC");
    if (Array.from(nickname).length > maxNicknameLength || /[\p{Cc}\p{Cf}\p{Cs}\p{Zl}\p{Zp}]/u.test(nickname)) {
      throw new RequestError(400, "nickname must be at most 24 characters without control characters");
    }
  }
  return { token: input.token, nickname: nickname || null };
}

function readClaim(token, board) {
  if (token.length > maxTokenLength || !/^[A-Za-z0-9_-]+$/.test(token)) {
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

export function createBoardServer({ board, verifyPrograms, trustCloudflare = false }) {
  const allowSubmission = submissionLimit();
  return createServer({ requestTimeout: 10_000, headersTimeout: 5_000 }, async (request, response) => {
    try {
      if (request.url.split("?")[0] !== route) {
        json(response, 404, { error: "not found" });
      } else if (request.method === "GET") {
        json(response, 200, { entries: board.entries() });
      } else if (request.method === "POST") {
        const ip = trustCloudflare ? request.headers["cf-connecting-ip"] : request.socket.remoteAddress;
        if (typeof ip !== "string" || !isIP(ip)) throw new RequestError(403, "client address is unavailable");
        if (!allowSubmission(ip)) throw new RequestError(429, "too many submissions, try again in a minute");

        const { token, nickname } = await readSubmission(request);
        const { chipId, key, programs } = readClaim(token, board);
        const result = verifyPrograms(programs);
        if (!result) throw new RequestError(422, "solutions did not pass verification");
        const entry = board.publish(chipId, key, programs, result, nickname);
        if (!entry) throw new RequestError(403, "card signature could not be verified");
        json(response, 200, { entry });
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
