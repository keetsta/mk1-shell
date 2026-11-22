import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { posix } from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseArgs } from 'node:util';

const otpRows = Array.from({ length: 12 }, (_, index) => '0x' + index.toString(16));
const usage = 'usage: node server/register-card.mjs (--local | <ssh-target> <absolute-remote-mk1-shell-dir> [--remote-node <absolute-path>]) [--serial <bootsel-serial>] [--picotool <path>]';
const readError = 'could not read a complete card identity in bootsel';

class RegistrationError extends Error {}

function readIdentity(output) {
  const words = new Map();
  let row;
  if (/\bwarning\b/i.test(output)) throw new RegistrationError(readError);

  for (const line of output.split(/\r?\n/)) {
    const header = /^\s*ROW\s+0x([0-9a-f]{4})(?=[:\s]|$)/i.exec(line);
    if (header) {
      row = Number.parseInt(header[1], 16);
      if (row >= otpRows.length || words.has(row)) throw new RegistrationError(readError);
      words.set(row, null);
      continue;
    }

    if (!/^\s*VALUE\b/i.test(line)) continue;
    const value = /^\s*VALUE\s+0x([0-9a-f]{4})\s*$/i.exec(line);
    if (!value || words.get(row) !== null) throw new RegistrationError(readError);
    words.set(row, Number.parseInt(value[1], 16));
  }
  if (words.size !== otpRows.length || [...words.values()].includes(null)) {
    throw new RegistrationError(readError);
  }

  const chipId = [3, 2, 1, 0].map(index => words.get(index).toString(16).padStart(4, '0')).join('');
  const randid = Buffer.alloc(16);
  for (let index = 0; index < 8; index++) randid.writeUInt16LE(words.get(index + 4), 2 * index);

  try {
    if (/^(?:0{16}|f{16})$/.test(chipId) || randid.every(byte => byte === 0) || randid.every(byte => byte === 255)) {
      throw new RegistrationError(readError);
    }
    const key = createHash('sha256').update('mk1/card').update(randid).digest('hex');
    return { chipId, key };
  } finally {
    randid.fill(0);
  }
}

function quoteRemotePath(path) {
  return "'" + path.replaceAll("'", "'\\''") + "'";
}

function isRemotePath(path) {
  return typeof path === 'string' && posix.isAbsolute(path) && !/[\x00-\x1f\x7f]/.test(path);
}

export function registerCard({ local = false, target, remoteDirectory, remoteNode, serial, picotool = 'picotool' }, run = spawnSync) {
  const targetPattern = /^(?:[a-z0-9_][a-z0-9_.-]*@)?(?:[a-z0-9_][a-z0-9_.-]*|\[[a-f0-9:.]+\]|[a-f0-9]*:[a-f0-9:.]+)$/i;
  const validRemoteNode = remoteNode === undefined ||
    (isRemotePath(remoteNode) && !remoteNode.endsWith('/') && !['.', '..'].includes(posix.basename(remoteNode)));
  const validDestination = local
    ? target === undefined && remoteDirectory === undefined && remoteNode === undefined
    : typeof target === 'string' && targetPattern.test(target) && isRemotePath(remoteDirectory) && validRemoteNode;
  if (!validDestination ||
      (serial !== undefined && !/^[a-z0-9][a-z0-9_.-]{0,127}$/i.test(serial)) ||
      typeof picotool !== 'string' || !picotool || picotool.includes('\0')) {
    throw new RegistrationError(usage);
  }

  const selectors = serial ? ['--ser', serial] : [];
  const result = run(picotool, ['otp', 'get', '--ecc', '--no-descriptions', ...selectors, ...otpRows], {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
    maxBuffer: 64 * 1024,
    timeout: 15000,
  });
  if (result.error || result.status !== 0 || typeof result.stdout !== 'string' || /\bwarning\b/i.test(result.stderr)) {
    throw new RegistrationError(readError);
  }

  const identity = readIdentity(result.stdout);
  const command = local ? process.execPath : 'ssh';
  const remoteExecutable = remoteNode === undefined ? 'node' : quoteRemotePath(remoteNode);
  const args = local
    ? [fileURLToPath(new URL('./admin.mjs', import.meta.url)), 'register']
    : ['-T', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '--', target,
        remoteExecutable + ' ' + quoteRemotePath(posix.join(remoteDirectory, 'server', 'admin.mjs')) + ' register'];
  const registration = run(command, args, {
    input: JSON.stringify(identity),
    stdio: ['pipe', 'ignore', 'ignore'],
    timeout: 60000,
  });
  if (registration.error || registration.status !== 0) {
    throw new RegistrationError(local ? 'card registration failed in the local database' : 'card registration failed over ssh');
  }
}

function options(args) {
  let parsed;
  try {
    parsed = parseArgs({
      args,
      allowPositionals: true,
      options: {
        local: { type: 'boolean' },
        'remote-node': { type: 'string' },
        serial: { type: 'string' },
        picotool: { type: 'string' },
      },
    });
  } catch {
    throw new RegistrationError(usage);
  }
  if (parsed.positionals.length !== (parsed.values.local ? 0 : 2)) throw new RegistrationError(usage);
  const [target, remoteDirectory] = parsed.positionals;
  const { 'remote-node': remoteNode, ...flags } = parsed.values;
  return { target, remoteDirectory, remoteNode, ...flags };
}

if (import.meta.main) {
  try {
    registerCard(options(process.argv.slice(2)));
    console.log('card registered');
  } catch (error) {
    console.error(error instanceof RegistrationError ? error.message : 'card registration failed');
    process.exitCode = 1;
  }
}
