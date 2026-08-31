const path = require('node:path');
const { spawnSync } = require('node:child_process');

function usage() {
  process.stderr.write('Usage: reader-replay replay <trace.jsonl> [--output <actual.jsonl>]\n');
  process.exit(2);
}

const args = process.argv.slice(2);
if (args.length < 2 || args[0] !== 'replay') usage();
let outputPath = '';
if (args.length === 4 && args[2] === '--output') outputPath = path.resolve(args[3]);
else if (args.length !== 2) usage();

const electronPath = require('electron');
const mainPath = path.resolve(__dirname, '..', 'hosts', 'electron', 'src', 'main-replay.cjs');
const environment = { ...process.env };
delete environment.ELECTRON_RUN_AS_NODE;
const result = spawnSync(electronPath, [mainPath, path.resolve(args[1]), outputPath], {
  env: environment,
  stdio: 'inherit',
  timeout: 90_000,
});
if (result.error) throw result.error;
process.exit(result.status ?? 1);
