import { readFile } from 'node:fs/promises';

export async function loadScorer(wasm = new URL('./scorer.wasm', import.meta.url)) {
    const { instance } = await WebAssembly.instantiate(await readFile(wasm));
    instance.exports._initialize();
    const { memory, score_input, score_capacity, score_verify } = instance.exports;
    const input = score_input();
    const capacity = score_capacity();

    return function verifyPrograms(entries) {
        if (entries.length > capacity) return null;
        new Uint8Array(memory.buffer, input, entries.length).set(entries);
        const result = score_verify(entries.length);
        if (!result) return null;

        const totals = new DataView(memory.buffer, result, 20);
        const qualityPlusOne = totals.getUint32(0, true);
        return {
            quality: qualityPlusOne ? qualityPlusOne - 1 : null,
            solved: totals.getUint32(4, true),
            instructions: totals.getUint32(8, true),
            cycles: totals.getUint32(12, true),
            ticks: totals.getUint32(16, true),
        };
    };
}
