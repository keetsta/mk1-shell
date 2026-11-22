import { spawnSync } from 'node:child_process';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const core = 'src/core';
const args = [
    'cc', '-target', 'wasm32-wasi', '-mexec-model=reactor', '-O2', '-std=c11', '-Wall', '-Wextra',
    '-I', core, ...['vm.c', 'sha256.c', 'tasks.c', 'content_gen.c'].map(file => `${core}/${file}`),
    'server/scorer.c', '-s', '-o', 'server/scorer.wasm',
];
const options = {
    cwd: root,
    stdio: 'inherit',
    env: {
        ...process.env,
        ZIG_GLOBAL_CACHE_DIR: process.env.ZIG_GLOBAL_CACHE_DIR ?? join(root, 'build', 'zig-global-cache'),
        ZIG_LOCAL_CACHE_DIR: process.env.ZIG_LOCAL_CACHE_DIR ?? join(root, 'build', 'zig-local-cache'),
    },
};

let build = spawnSync(process.env.ZIG ?? 'zig', args, options);
if (!process.env.ZIG && build.error?.code === 'ENOENT') {
    const python = process.env.PYTHON ?? (process.platform === 'win32' ? 'python' : 'python3');
    build = spawnSync(python, ['-m', 'ziglang', ...args], options);
}
if (build.error) console.error(build.error.message);
process.exitCode = build.status ?? 1;
