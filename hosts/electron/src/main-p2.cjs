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
const timeoutMs = 30_000;

let child = null;
let completed = false;

function finish(exitCode, message) {
  if (completed) {
    return;
  }
  completed = true;
  if (message) {
    (exitCode === 0 ? process.stdout : process.stderr).write(`${message}\n`);
  }
  child?.kill();
  app.exit(exitCode);
}

app.whenReady().then(() => {
  child = utilityProcess.fork(utilityPath, [addonPath, fixturePath], {
    serviceName: 'Context Reader P2 Workspace Smoke',
    stdio: 'pipe',
  });
  child.stdout?.pipe(process.stdout);
  child.stderr?.pipe(process.stderr);

  const timeout = setTimeout(() => {
    finish(1, `Electron P2 utility process timed out after ${timeoutMs} ms`);
  }, timeoutMs);

  child.on('message', (message) => {
    clearTimeout(timeout);
    if (
      message?.status === 'ok'
      && message.processType === 'utility'
      && message.runtimeInfo?.applicationApiVersion === 3
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
      && message.pageText?.firstLine?.bounds?.width > 0
    ) {
      finish(
        0,
        `Electron utility reopened and rendered workspace ${message.workspaceId} (${message.rendered.widthPixels}x${message.rendered.heightPixels})`,
      );
      return;
    }
    finish(1, `Unexpected P2 utility response: ${JSON.stringify(message)}`);
  });

  child.on('exit', (code) => {
    clearTimeout(timeout);
    if (!completed) {
      finish(1, `Electron P2 utility process exited before validation with code ${code}`);
    }
  });
});

app.on('window-all-closed', () => {});
