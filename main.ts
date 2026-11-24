// site-generator-speed-01, Akun 1: a freestanding native binary does the
// whole build; this driver only spawns it and forwards its exit code.
// The binary reads MAKEFUN_INPUT_DIR/cases.json and writes
// MAKEFUN_OUTPUT_DIR/answers.json.
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const BIN = join(dirname(fileURLToPath(import.meta.url)), 'site');
const r = spawnSync(BIN, [], { stdio: 'inherit' });
process.exit(r.status ?? 1);
