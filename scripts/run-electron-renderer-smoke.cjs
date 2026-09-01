const fs = require('node:fs');
const os = require('node:os');
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
const temporaryRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'context-reader-renderer-e2e-'));
const workspacePath = path.join(temporaryRoot, 'workspace');

try {
  for (const phase of ['setup', 'recovery']) {
    const environment = {
      ...process.env,
      CONTEXT_READER_SMOKE_FIXTURE: fixturePath,
      CONTEXT_READER_SMOKE_WORKSPACE: workspacePath,
      CONTEXT_READER_SMOKE_PHASE: phase,
    };
    delete environment.ELECTRON_RUN_AS_NODE;
    const result = spawnSync(electronPath, [mainPath], {
      env: environment,
      stdio: 'inherit',
      timeout: 30_000,
    });
    if (result.error) throw result.error;
    if (result.status !== 0) process.exit(result.status ?? 1);
  }
  process.stdout.write('Electron renderer P2 end-to-end restart smoke passed\n');
} finally {
  fs.rmSync(temporaryRoot, { recursive: true, force: true, maxRetries: 20, retryDelay: 100 });
}
