const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { spawn } = require('node:child_process');

const electronPath = require('electron');
const projectRoot = path.resolve(__dirname, '..');
const mainPath = path.join(projectRoot, 'hosts', 'electron', 'src', 'main-app.cjs');
const timeoutMs = 30_000;

function startElectron(environment) {
  return spawn(electronPath, [mainPath], {
    env: environment,
    stdio: ['ignore', 'pipe', 'pipe'],
    windowsHide: true,
  });
}

function waitForExit(child) {
  return new Promise((resolve, reject) => {
    child.once('error', reject);
    child.once('exit', (code, signal) => resolve({ code, signal }));
  });
}

function rejectAfter(timeout, message) {
  return new Promise((resolve, reject) => {
    const timeoutHandle = setTimeout(() => reject(new Error(message())), timeout);
    timeoutHandle.unref();
  });
}

function waitForText(stream, expected, currentOutput) {
  if (currentOutput().includes(expected)) return Promise.resolve();
  return new Promise((resolve) => {
    const onData = () => {
      if (currentOutput().includes(expected)) {
        stream.off('data', onData);
        resolve();
      }
    };
    stream.on('data', onData);
  });
}

async function run() {
  const temporaryRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'context-reader-single-instance-'));
  const environment = {
    ...process.env,
    CONTEXT_READER_SINGLE_INSTANCE_SMOKE: '1',
    CONTEXT_READER_SINGLE_INSTANCE_USER_DATA: path.join(temporaryRoot, 'user-data'),
  };
  delete environment.ELECTRON_RUN_AS_NODE;

  let primary = null;
  let secondary = null;
  let timer = null;
  try {
    primary = startElectron(environment);
    let primaryOutput = '';
    primary.stdout.on('data', (chunk) => { primaryOutput += chunk.toString(); });
    primary.stderr.on('data', (chunk) => { primaryOutput += chunk.toString(); });

    await new Promise((resolve, reject) => {
      timer = setTimeout(() => reject(new Error(`Primary Electron instance timed out:\n${primaryOutput}`)), timeoutMs);
      primary.stdout.on('data', (chunk) => {
        if (primaryOutput.includes('single-instance-primary-ready')) resolve();
      });
      primary.once('exit', (code) => reject(new Error(
        `Primary Electron instance exited before it was ready (${code}):\n${primaryOutput}`,
      )));
    });
    clearTimeout(timer);
    timer = null;

    secondary = startElectron(environment);
    let secondaryOutput = '';
    secondary.stdout.on('data', (chunk) => { secondaryOutput += chunk.toString(); });
    secondary.stderr.on('data', (chunk) => { secondaryOutput += chunk.toString(); });

    const secondaryResult = await Promise.race([
      waitForExit(secondary),
      rejectAfter(timeoutMs, () => (
        `Secondary Electron instance did not exit.\n`
          + `Primary output:\n${primaryOutput}\nSecondary output:\n${secondaryOutput}`
      )),
    ]);
    if (secondaryResult.code !== 0
        || !secondaryOutput.includes('single-instance-secondary-exit')) {
      throw new Error(`Secondary instance did not exit cleanly:\n${secondaryOutput}`);
    }
    await Promise.race([
      waitForText(
        primary.stdout,
        'single-instance-window-focused',
        () => primaryOutput,
      ),
      rejectAfter(timeoutMs, () => `Primary instance did not receive second-instance:\n${primaryOutput}`),
    ]);
    const primaryExit = waitForExit(primary);
    primary.kill();
    await Promise.race([
      primaryExit,
      rejectAfter(timeoutMs, () => `Primary instance did not terminate after the smoke:\n${primaryOutput}`),
    ]);
    process.stdout.write('Electron single-instance smoke passed\n');
  } finally {
    if (timer) clearTimeout(timer);
    if (primary && primary.exitCode === null) primary.kill();
    if (secondary && secondary.exitCode === null) secondary.kill();
    await fs.rm(temporaryRoot, {
      recursive: true,
      force: true,
      maxRetries: 20,
      retryDelay: 100,
    });
  }
}

run().catch((error) => {
  process.stderr.write(`${error instanceof Error ? error.stack : String(error)}\n`);
  process.exitCode = 1;
});
