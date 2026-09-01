const path = require('node:path');
const fs = require('node:fs/promises');

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
  'renderTile',
  'extractPageText',
  'pageTextLayout',
  'selectText',
  'createAnnotation',
  'listAnnotations',
  'deleteAnnotation',
  'updateNote',
  'listNotes',
  'rebuildSearchIndex',
  'search',
  'importNoteAsset',
  'readAsset',
  'exportWorkspace',
  'inspectBackup',
  'restoreWorkspace',
  'verifyWorkspace',
  'exportDiagnostics',
]);
const jobOperations = new Set(['importDocument', 'openDocument', 'renderPage', 'renderTile', 'rebuildSearchIndex', 'importNoteAsset', 'exportWorkspace', 'restoreWorkspace', 'exportDiagnostics']);

function send(message) {
  process.parentPort.postMessage(message);
}

try {
  const addonPath = path.resolve(process.argv[2]);
  const readerNode = require(addonPath);
  let responseFaultOperation = null;
  let dataPort = null;
  const recentErrorCodes = [];

  process.parentPort.on('message', async (event) => {
    const message = event?.data ?? event;
    if (message?.kind === 'data-port' && event?.ports?.[0]) {
      dataPort?.close();
      dataPort = event.ports[0];
      dataPort.on('message', async (dataEvent) => {
        const request = dataEvent?.data ?? dataEvent;
        if (request?.kind === 'cancel-job' && typeof request.jobId === 'string') {
          readerNode.cancelJob(request.jobId);
          return;
        }
        if (request?.kind !== 'tile-request' || typeof request.jobId !== 'string') return;
        try {
          if (process.env.CONTEXT_READER_UTILITY_RESTART_FIXTURE) {
            const testDelay = Number(request.request?.testDelayMs ?? 0);
            if (Number.isFinite(testDelay) && testDelay > 0) {
              await new Promise((resolve) => setTimeout(resolve, Math.min(testDelay, 30_000)));
            }
          }
          const value = await readerNode.renderTile(request.request, request.jobId);
          dataPort.postMessage({ kind: 'tile-response', jobId: request.jobId, ok: true, value });
        } catch (error) {
          dataPort.postMessage({
            kind: 'tile-response', jobId: request.jobId, ok: false,
            error: { code: typeof error?.code === 'string' ? error.code : 'INTERNAL', message: String(error?.message || error) },
          });
        }
      });
      dataPort.start();
      return;
    }
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
      if (message.operation === 'exportDiagnostics') {
        const destination = message.arguments?.[0];
        if (typeof destination !== 'string' || destination.length === 0) {
          const error = new Error('Diagnostics destination must be a path');
          error.code = 'INVALID_ARGUMENT';
          throw error;
        }
        const runtime = readerNode.runtimeInfo();
        const diagnostics = {
          format: 'context-reader-diagnostics',
          version: 1,
          buildId: runtime.buildId,
          applicationVersion: runtime.version,
          applicationApiVersion: runtime.applicationApiVersion,
          workspaceSchemaVersion: 4,
          capabilities: runtime.capabilities,
          taskStats: { active: 0 },
          cacheStats: { redacted: true },
          recentErrorCodes: [...recentErrorCodes],
        };
        await fs.writeFile(destination, `${JSON.stringify(diagnostics, null, 2)}\n`, { flag: 'wx' });
        send({ kind: 'response', requestId: message.requestId, ok: true, value: { version: 1 } });
        return;
      }
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
      const errorCode = typeof error?.code === 'string' ? error.code : 'INTERNAL';
      recentErrorCodes.push(errorCode);
      if (recentErrorCodes.length > 20) recentErrorCodes.shift();
      send({
        kind: 'response',
        requestId: message.requestId,
        ok: false,
        error: {
          code: errorCode,
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
