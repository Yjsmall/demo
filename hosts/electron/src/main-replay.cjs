const path = require('node:path');
const { app, utilityProcess } = require('electron');

const projectRoot = path.resolve(__dirname, '..', '..', '..');
const addonPath = path.join(projectRoot, 'build', 'node-p2-ucrt64', 'reader_node.node');
const fixturePath = path.join(
  projectRoot, 'tests', 'corpus', 'generated', 'basic-rotated-cropbox.pdf',
);
const utilityPath = path.join(__dirname, 'utility-replay.cjs');
const replayPath = process.argv[2];
const outputPath = process.argv[3] || '';

app.whenReady().then(() => {
  const child = utilityProcess.fork(
    utilityPath,
    [addonPath, replayPath, fixturePath, outputPath],
    { serviceName: 'Context Reader Replay', stdio: 'pipe' },
  );
  child.stdout?.pipe(process.stdout);
  child.stderr?.pipe(process.stderr);
  const timer = setTimeout(() => {
    child.kill();
    process.stderr.write('reader-replay timed out\n');
    app.exit(1);
  }, 60_000);
  child.on('message', (message) => {
    clearTimeout(timer);
    child.kill();
    if (message?.status !== 'ok' || message.cancelledJobCount !== 1
        || message.workspaceClosed !== true || message.eventCount < 10) {
      process.stderr.write(`Replay failed: ${JSON.stringify(message)}\n`);
      app.exit(1);
      return;
    }
    process.stdout.write(`${JSON.stringify({ ok: true, command: 'replay', ...message })}\n`);
    app.exit(0);
  });
});

app.on('window-all-closed', () => {});
