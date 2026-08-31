const path = require('node:path');

const operations = new Set([
  'createWorkspace',
  'openWorkspace',
  'closeWorkspace',
  'importDocument',
  'listDocuments',
  'openDocument',
  'closeDocument',
  'pageInfo',
  'renderPage',
  'extractPageText',
  'createAnnotation',
  'listAnnotations',
  'deleteAnnotation',
  'updateNote',
  'listNotes',
  'verifyWorkspace',
]);
const jobOperations = new Set(['importDocument', 'openDocument', 'renderPage']);

function send(message) {
  process.parentPort.postMessage(message);
}

try {
  const addonPath = path.resolve(process.argv[2]);
  const readerNode = require(addonPath);

  process.parentPort.on('message', async (event) => {
    const message = event?.data ?? event;
    if (message?.kind === 'cancel-job' && typeof message.jobId === 'string') {
      readerNode.cancelJob(message.jobId);
      return;
    }
    if (message?.kind !== 'request' || !operations.has(message.operation)) {
      return;
    }

    try {
      const args = [...(message.arguments ?? [])];
      if (message.jobId && jobOperations.has(message.operation)) args.push(message.jobId);
      const value = await readerNode[message.operation](...args);
      send({ kind: 'response', requestId: message.requestId, ok: true, value });
    } catch (error) {
      send({
        kind: 'response',
        requestId: message.requestId,
        ok: false,
        error: {
          code: typeof error?.code === 'string' ? error.code : 'INTERNAL',
          message: error instanceof Error ? error.message : String(error),
        },
      });
    }
  });

  send({ kind: 'ready', runtimeInfo: readerNode.runtimeInfo() });
} catch (error) {
  send({
    kind: 'startup-error',
    error: {
      code: 'INTERNAL',
      message: error instanceof Error ? error.message : String(error),
    },
  });
}
