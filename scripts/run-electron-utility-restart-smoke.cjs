const path = require('node:path');
const { spawnSync } = require('node:child_process');

const electronPath = require('electron');
const projectRoot = path.resolve(__dirname, '..');
const mainPath = path.join(projectRoot, 'hosts', 'electron', 'src', 'main-app.cjs');
const fixturePath = path.join(
  projectRoot,
  'tests',
  'corpus',
  'generated',
  'basic-rotated-cropbox.pdf',
);
const environment = {
  ...process.env,
  CONTEXT_READER_UTILITY_RESTART_FIXTURE: fixturePath,
};

delete environment.ELECTRON_RUN_AS_NODE;

const result = spawnSync(electronPath, [mainPath], {
  env: environment,
  stdio: 'inherit',
  timeout: 60_000,
});

if (result.error) throw result.error;
process.exit(result.status ?? 1);
