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
  let responseFaultOperation = null;

  process.parentPort.on('message', async (event) => {
    const message = event?.data ?? event;
    if (message?.kind === 'cancel-job' && typeof message.jobId === 'string') {
      readerNode.cancelJob(message.jobId);
      return;
    }
    if (message?.kind !== 'request' || !operations.has(message.operation)) {
      if (message?.kind === 'request' && message.operation === '__testArmResponseFault'
          && process.env.CONTEXT_READER_UTILITY_RESTART_FIXTURE) {
        const operation = message.arguments?.[0];
        if (!operations.has(operation)) {
          send({
            kind: 'response',
            requestId: message.requestId,
            ok: false,
            error: { code: 'INVALID_ARGUMENT', message: 'Fault operation is invalid' },
          });
          return;
        }
        responseFaultOperation = operation;
        send({ kind: 'response', requestId: message.requestId, ok: true, value: operation });
        return;
      }
      if (message?.kind === 'request' && message.operation === '__testDelay'
          && process.env.CONTEXT_READER_UTILITY_RESTART_FIXTURE) {
        const delay = Number(message.arguments?.[0] ?? 0);
        setTimeout(() => {
          send({ kind: 'response', requestId: message.requestId, ok: true, value: delay });
        }, delay);
      }
      return;
    }

    try {
      const args = [...(message.arguments ?? [])];
      if (message.jobId && jobOperations.has(message.operation)) args.push(message.jobId);
      const value = await readerNode[message.operation](...args);
      if (responseFaultOperation === message.operation) {
        responseFaultOperation = null;
        process.exit(86);
        return;
      }
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
