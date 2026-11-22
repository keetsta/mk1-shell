import assert from 'node:assert/strict';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
import { registerCard } from './register-card.mjs';

const fixture = [
  0x0708, 0x0506, 0x0304, 0x0102, 0x0100, 0x0302, 0x0504, 0x0706, 0x0908, 0x0b0a, 0x0d0c, 0x0f0e,
].map((word, row) => {
  const address = row.toString(16).padStart(4, '0');
  const field = row < 4 ? 'CHIPID' + row : 'RANDID' + (row - 4);
  const value = word.toString(16).padStart(4, '0');
  return 'ROW 0x' + address + ': OTP_DATA_' + field + ' (ECC)\r\n    VALUE 0x' + value;
}).join('\r\n\r\n');

const config = {
  target: 'admin@mk1-server',
  remoteDirectory: "/srv/mk1 owner's repo",
  serial: 'A1B2C3D4',
  picotool: 'tools/picotool',
};

const identity = {
  chipId: '0102030405060708',
  key: 'edea6373c65c49344c370445cb3b091eafe000d733d0b936f891bbaf6030fd22',
};

test('derives the firmware identity and transfers it only through ssh stdin', () => {
  const calls = [];
  registerCard(config, (command, args, options) => {
    calls.push({ command, args, options });
    return { status: 0, stdout: fixture, stderr: '' };
  });

  assert.equal(calls.length, 2);
  assert.equal(calls[0].command, 'tools/picotool');
  assert.deepEqual(calls[0].args, [
    'otp', 'get', '--ecc', '--no-descriptions', '--ser', 'A1B2C3D4',
    '0x0', '0x1', '0x2', '0x3', '0x4', '0x5', '0x6', '0x7', '0x8', '0x9', '0xa', '0xb',
  ]);
  assert.deepEqual(calls[0].options.stdio, ['ignore', 'pipe', 'pipe']);
  assert.equal(calls[1].command, 'ssh');
  assert.deepEqual(calls[1].args, [
    '-T', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '--', 'admin@mk1-server',
    "node '/srv/mk1 owner'\\''s repo/server/admin.mjs' register",
  ]);
  assert.deepEqual(calls[1].options.stdio, ['pipe', 'ignore', 'ignore']);
  assert.deepEqual(JSON.parse(calls[1].options.input), identity);
});

test('local registration pipes the identity to the local admin process', () => {
  const calls = [];
  registerCard({ local: true, picotool: config.picotool }, (command, args, options) => {
    calls.push({ command, args, options });
    return { status: 0, stdout: fixture, stderr: '' };
  });

  assert.equal(calls.length, 2);
  assert.equal(calls[0].command, config.picotool);
  assert.equal(calls[1].command, process.execPath);
  assert.deepEqual(calls[1].args, [fileURLToPath(new URL('./admin.mjs', import.meta.url)), 'register']);
  assert.deepEqual(calls[1].options.stdio, ['pipe', 'ignore', 'ignore']);
  assert.deepEqual(JSON.parse(calls[1].options.input), identity);
});

test('ssh registration quotes the configured remote node executable', () => {
  const calls = [];
  registerCard({ ...config, remoteNode: "/srv/node owner's bin/node" }, (command, args, options) => {
    calls.push({ command, args, options });
    return { status: 0, stdout: fixture, stderr: '' };
  });

  assert.equal(calls.length, 2);
  assert.equal(calls[1].command, 'ssh');
  assert.deepEqual(calls[1].args, [
    '-T', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes', '--', 'admin@mk1-server',
    "'/srv/node owner'\\''s bin/node' '/srv/mk1 owner'\\''s repo/server/admin.mjs' register",
  ]);
});

test('stops before ssh when picotool reports an incomplete, duplicate, or invalid OTP read', () => {
  const badReads = [
    { status: 1, stdout: fixture, stderr: 'multiple devices' },
    { status: 0, stdout: fixture.replace('VALUE 0x0f0e', ''), stderr: '' },
    { status: 0, stdout: fixture.replace('ROW 0x000b', 'ROW 0x000a'), stderr: '' },
    { status: 0, stdout: fixture.replace('VALUE 0x0f0e', 'VALUE 0x000f0e'), stderr: '' },
    { status: 0, stdout: fixture + '\r\nWARNING - ECC IS INVALID', stderr: '' },
  ];

  for (const result of badReads) {
    const calls = [];
    assert.throws(() => registerCard(config, command => {
      calls.push(command);
      return result;
    }), /^Error: could not read a complete card identity in bootsel$/);
    assert.deepEqual(calls, ['tools/picotool']);
  }
});

test('validates the registration destination before spawning a process', () => {
  const unexpectedRun = () => assert.fail('no process should start');
  for (const overrides of [
    { local: true },
    { target: '-oProxyCommand=x' },
    { target: 'mk1-server; exit' },
    { remoteDirectory: 'relative/path' },
    { remoteDirectory: '/path\nexit' },
    { remoteNode: 'relative/node' },
    { remoteNode: '/bin/node\nexit' },
    { remoteNode: '/bin/' },
    { serial: '--force' },
  ]) {
    assert.throws(() => registerCard({ ...config, ...overrides }, unexpectedRun), /^Error: usage:/);
  }
  assert.throws(() => registerCard({ local: true, remoteNode: '/usr/bin/node' }, unexpectedRun), /^Error: usage:/);
});
