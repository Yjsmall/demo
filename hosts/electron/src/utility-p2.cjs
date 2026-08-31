const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

function send(message) {
  process.parentPort.postMessage(message);
}

async function run() {
  const addonPath = path.resolve(process.argv[2]);
  const fixturePath = path.resolve(process.argv[3]);
  const temporaryRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'context-reader-p2-'));
  const workspacePath = path.join(temporaryRoot, 'workspace-unicode-\u9605\u8bfb');

  try {
    const readerNode = require(addonPath);
    const runtimeInfo = readerNode.runtimeInfo();
    const workspace = await readerNode.createWorkspace(workspacePath);
    const imported = await readerNode.importDocument(fixturePath);
    const duplicate = await readerNode.importDocument(fixturePath);

    if (!duplicate.reusedExisting || duplicate.document.documentId !== imported.document.documentId) {
      throw new Error('Duplicate import did not reuse the existing document');
    }

    await readerNode.closeWorkspace();
    const reopened = await readerNode.openWorkspace(workspacePath);
    const documents = await readerNode.listDocuments();
    const verification = await readerNode.verifyWorkspace();
    await readerNode.closeWorkspace();

    send({
      status: 'ok',
      processType: process.type,
      runtimeInfo,
      workspaceId: workspace.id,
      reopenedWorkspaceId: reopened.id,
      documentCount: documents.length,
      contentSha256: documents[0]?.contentSha256,
      verification,
    });
  } finally {
    await fs.rm(temporaryRoot, { recursive: true, force: true });
  }
}

run().catch((error) => {
  send({
    status: 'error',
    processType: process.type,
    message: error instanceof Error ? error.stack : String(error),
  });
});
