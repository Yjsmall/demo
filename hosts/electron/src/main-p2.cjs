const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

const { app, utilityProcess } = require('electron');

const projectRoot = path.resolve(__dirname, '..', '..', '..');
const addonPath = path.join(projectRoot, 'build', 'node-p2-ucrt64', 'reader_node.node');
const fixturePath = path.join(
  projectRoot,
  'tests',
  'corpus',
  'generated',
  'basic-rotated-cropbox.pdf',
);
const utilityPath = path.join(__dirname, 'utility-p2.cjs');
const faultUtilityPath = path.join(__dirname, 'utility-fault-p2.cjs');
const timeoutMs = 30_000;

function runUtility(script, args, expectedExitCode = null) {
  return new Promise((resolve, reject) => {
    const child = utilityProcess.fork(script, args, {
      serviceName: 'Context Reader P2 Workspace Smoke',
      stdio: 'pipe',
    });
    child.stdout?.pipe(process.stdout);
    child.stderr?.pipe(process.stderr);
    let settled = false;
    const timer = setTimeout(() => {
      if (settled) return;
      settled = true;
      child.kill();
      reject(new Error(`Electron P2 Utility Process timed out after ${timeoutMs} ms`));
    }, timeoutMs);

    child.on('message', (message) => {
      if (settled || expectedExitCode !== null) return;
      settled = true;
      clearTimeout(timer);
      child.kill();
      if (message?.status === 'error') {
        reject(new Error(message.message || 'Utility Process reported an error'));
      } else {
        resolve(message);
      }
    });

    child.on('exit', (code) => {
      if (settled) return;
      settled = true;
      clearTimeout(timer);
      if (expectedExitCode !== null && code === expectedExitCode) {
        resolve({ exitCode: code });
      } else {
        reject(new Error(`Utility Process exited unexpectedly with code ${code}`));
      }
    });
  });
}

function validateNormalSmoke(message) {
  return message?.status === 'ok'
    && message.processType === 'utility'
    && message.runtimeInfo?.applicationApiVersion === 5
    && message.workspaceId === message.reopenedWorkspaceId
    && message.documentCount === 1
    && message.verification?.valid === true
    && message.contentSha256?.length === 64
    && message.page?.widthPoints === 540
    && message.page?.heightPoints === 648
    && message.page?.rotation === 90
    && message.rendered?.widthPixels === 648
    && message.rendered?.heightPixels === 540
    && message.rendered?.byteLength > 8
    && message.pageText?.text?.includes('Context Reader P1')
    && message.pageText?.lineCount === 1
    && message.annotationCount === 1
    && message.noteCount === 1
    && message.note?.revision === 2
    && message.note?.markdownSource === 'Restored **context** note'
    && message.updatedNoteRevision === 2
    && message.conflictCode === 'CONFLICT'
    && message.cancellationAccepted === true
    && message.cancellationCode === 'CANCELLED';
}

async function run() {
  const normal = await runUtility(utilityPath, [addonPath, fixturePath]);
  if (!validateNormalSmoke(normal)) {
    throw new Error(`Unexpected P2 Utility response: ${JSON.stringify(normal)}`);
  }

  const temporaryRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'context-reader-p2-fault-'));
  const workspacePath = path.join(temporaryRoot, 'workspace');
  try {
    const commonArguments = [addonPath, fixturePath, workspacePath];
    await runUtility(faultUtilityPath, [...commonArguments, 'setup']);
    await runUtility(faultUtilityPath, [...commonArguments, 'before-commit'], 86);
    const beforeRecovery = await runUtility(
      faultUtilityPath,
      [...commonArguments, 'verify', '0'],
    );
    await runUtility(faultUtilityPath, [...commonArguments, 'after-commit'], 86);
    const afterRecovery = await runUtility(
      faultUtilityPath,
      [...commonArguments, 'verify', '1'],
    );
    process.stdout.write(
      `Electron P2 passed: cancellable Jobs and import recovery `
        + `(before=${beforeRecovery.documentCount}, after=${afterRecovery.documentCount})\n`,
    );
  } finally {
    await fs.rm(temporaryRoot, { recursive: true, force: true });
  }
}

app.whenReady().then(async () => {
  try {
    await run();
    app.exit(0);
  } catch (error) {
    process.stderr.write(`${error instanceof Error ? error.stack : String(error)}\n`);
    app.exit(1);
  }
});

app.on('window-all-closed', () => {});
