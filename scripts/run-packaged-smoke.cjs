const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

const projectRoot = path.resolve(__dirname, '..');
const executable = path.join(projectRoot, 'out', 'ContextReader-win32-x64', 'electron.exe');
const fixturePath = path.join(projectRoot, 'tests', 'corpus', 'generated', 'basic-rotated-cropbox.pdf');
const temporaryRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'context-reader-package-smoke-'));
const workspacePath = path.join(temporaryRoot, 'workspace');
const screenshotPath = path.join(temporaryRoot, 'package-smoke.png');

try {
  if (!fs.existsSync(executable)) throw new Error(`Packaged executable is missing: ${executable}`);
  for (const phase of ['setup', 'recovery']) {
    const environment = {
      ...process.env,
      CONTEXT_READER_SMOKE_FIXTURE: fixturePath,
      CONTEXT_READER_SMOKE_WORKSPACE: workspacePath,
      CONTEXT_READER_SMOKE_PHASE: phase,
      CONTEXT_READER_SMOKE_OUTPUT: screenshotPath,
    };
    delete environment.ELECTRON_RUN_AS_NODE;
    const result = spawnSync(executable, [], { env: environment, stdio: 'inherit', timeout: 30_000 });
    if (result.error) throw result.error;
    if (result.status !== 0) process.exit(result.status ?? 1);
  }
  const screenshot = fs.statSync(screenshotPath);
  if (screenshot.size < 1024) throw new Error('Packaged renderer screenshot is missing or empty');
  process.stdout.write('Packaged candidate setup/recovery smoke passed\n');
} finally {
  fs.rmSync(temporaryRoot, { recursive: true, force: true, maxRetries: 20, retryDelay: 100 });
}
