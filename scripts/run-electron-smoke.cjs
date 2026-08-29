const path = require('node:path');
const { spawnSync } = require('node:child_process');

const electronPath = require('electron');
const mainPath = path.resolve(__dirname, '..', 'hosts', 'electron', 'src', 'main.cjs');
const environment = { ...process.env };

delete environment.ELECTRON_RUN_AS_NODE;

const result = spawnSync(electronPath, [mainPath], {
  env: environment,
  stdio: 'inherit',
});

if (result.error) {
  throw result.error;
}

process.exit(result.status ?? 1);
