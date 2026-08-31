const path = require('node:path');
const { spawnSync } = require('node:child_process');

const electronPath = require('electron');
const mainPath = path.resolve(__dirname, '..', 'hosts', 'electron', 'src', 'main-node-contract.cjs');
const environment = { ...process.env };
const args = process.argv.slice(2);
let outputPath = '';
if (args.length === 2 && args[0] === '--output') outputPath = path.resolve(args[1]);
else if (args.length !== 0) {
  process.stderr.write('Usage: node-contract [--output <summary.json>]\n');
  process.exit(2);
}

delete environment.ELECTRON_RUN_AS_NODE;

const result = spawnSync(electronPath, [mainPath, outputPath], {
  env: environment,
  stdio: 'inherit',
  timeout: 90_000,
});

if (result.error) throw result.error;
process.exit(result.status ?? 1);
