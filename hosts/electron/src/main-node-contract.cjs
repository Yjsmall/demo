const fs = require('node:fs');
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
const utilityPath = path.join(__dirname, 'utility-node-contract.cjs');
const outputPath = process.argv[2] || '';

app.whenReady().then(() => {
  const child = utilityProcess.fork(utilityPath, [addonPath, fixturePath], {
    serviceName: 'Context Reader Node Contract',
    stdio: 'pipe',
  });
  child.stdout?.pipe(process.stdout);
  child.stderr?.pipe(process.stderr);
  const timer = setTimeout(() => {
    child.kill();
    process.stderr.write('Node contract smoke timed out\n');
    app.exit(1);
  }, 60_000);
  child.on('message', (message) => {
    clearTimeout(timer);
    child.kill();
    const valid = message?.status === 'ok'
      && message.runtimeApiVersion === 6
      && message.parameterCheckCount === 6
      && message.unopenedCode === 'NOT_FOUND'
      && message.workspaceSchemaVersion === 4
      && message.workspaceConflict === 'CONFLICT'
      && message.documentCount === 1
      && message.duplicateJobCode === 'CONFLICT'
      && message.cancellationAccepted === true
      && message.cancellationCode === 'CANCELLED'
      && message.unknownCancellation === false
      && message.pngByteLength > 8
      && message.pngValid === true
      && message.tileValid === true
      && message.selectionValid === true
      && message.searchValid === true
      && message.revisionConflict === 'CONFLICT'
      && message.annotationCount === 1
      && message.noteCount === 1
      && message.noteRevision === 1
      && message.workspaceValid === true
      && message.closedDocumentCode === 'NOT_FOUND'
      && message.closedWorkspaceCode === 'NOT_FOUND';
    if (!valid) {
      process.stderr.write(`Unexpected Node contract result: ${JSON.stringify(message)}\n`);
      app.exit(1);
      return;
    }
    if (outputPath) {
      fs.writeFileSync(outputPath, `${JSON.stringify({
        runtimeApiVersion: message.runtimeApiVersion,
        workspaceSchemaVersion: message.workspaceSchemaVersion,
        documentCount: message.documentCount,
        pngValid: message.pngValid,
        annotationCount: message.annotationCount,
        noteCount: message.noteCount,
        noteRevision: message.noteRevision,
        workspaceValid: message.workspaceValid,
        closedDocumentCode: message.closedDocumentCode,
        closedWorkspaceCode: message.closedWorkspaceCode,
      })}\n`, 'utf8');
    }
    process.stdout.write('Node-API P3 contract passed\n');
    app.exit(0);
  });
  child.on('exit', (code) => {
    if (code !== 0) {
      clearTimeout(timer);
      process.stderr.write(`Node contract Utility exited with code ${code}\n`);
      app.exit(1);
    }
  });
});

app.on('window-all-closed', () => {});
