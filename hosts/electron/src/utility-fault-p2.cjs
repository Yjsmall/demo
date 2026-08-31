const path = require('node:path');

function send(message) {
  process.parentPort.postMessage(message);
}

async function run() {
  const addonPath = path.resolve(process.argv[2]);
  const fixturePath = path.resolve(process.argv[3]);
  const workspacePath = path.resolve(process.argv[4]);
  const phase = process.argv[5];
  const expectedDocumentCount = Number(process.argv[6] ?? 0);

  if (phase === 'before-commit' || phase === 'after-commit') {
    process.env.CONTEXT_READER_TEST_IMPORT_FAULT = phase;
  }
  const readerNode = require(addonPath);

  if (phase === 'setup') {
    await readerNode.createWorkspace(workspacePath);
    await readerNode.closeWorkspace();
    send({ status: 'ok', phase });
    return;
  }

  if (phase === 'before-commit' || phase === 'after-commit') {
    await readerNode.openWorkspace(workspacePath);
    await readerNode.importDocument(fixturePath);
    throw new Error(`Fault point ${phase} did not terminate the Utility Process`);
  }

  if (phase === 'verify') {
    await readerNode.openWorkspace(workspacePath);
    const documents = await readerNode.listDocuments();
    const verification = await readerNode.verifyWorkspace();
    await readerNode.closeWorkspace();
    if (documents.length !== expectedDocumentCount || !verification.valid) {
      throw new Error(
        `Recovery mismatch: expected ${expectedDocumentCount} documents, got ${documents.length}`,
      );
    }
    send({ status: 'ok', phase, documentCount: documents.length, verification });
    return;
  }

  throw new Error(`Unknown fault smoke phase: ${phase}`);
}

run().catch((error) => {
  send({
    status: 'error',
    message: error instanceof Error ? error.stack : String(error),
  });
});
