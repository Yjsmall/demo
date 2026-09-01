const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

const { app, BrowserWindow, dialog, ipcMain, utilityProcess } = require('electron');

const projectRoot = path.resolve(__dirname, '..', '..', '..');
const addonPath = path.join(projectRoot, 'build', 'node-p2-ucrt64', 'reader_node.node');
const utilityPath = path.join(__dirname, 'utility-host.cjs');
const rendererPath = path.join(__dirname, '..', 'renderer', 'index.html');
const preloadPath = path.join(__dirname, 'preload.cjs');
const allowedOperations = new Set([
  'createWorkspace', 'openWorkspace', 'closeWorkspace', 'listDocuments',
  'closeDocument', 'pageInfo', 'extractPageText',
  'createAnnotation', 'listAnnotations', 'deleteAnnotation', 'updateNote', 'listNotes',
  'verifyWorkspace',
]);
const jobOperations = new Set(['importDocument', 'openDocument', 'renderPage']);

let window = null;
let child = null;
let ready = null;
let resolveReady = null;
let rejectReady = null;
let nextRequestId = 1;
const pending = new Map();
const grantedPaths = new Set();
const smokeFixture = process.env.CONTEXT_READER_SMOKE_FIXTURE || null;
const smokeWorkspacePath = process.env.CONTEXT_READER_SMOKE_WORKSPACE || null;
const smokePhase = process.env.CONTEXT_READER_SMOKE_PHASE || 'single';
const singleInstanceSmoke = process.env.CONTEXT_READER_SINGLE_INSTANCE_SMOKE === '1';
const utilityRestartFixture = process.env.CONTEXT_READER_UTILITY_RESTART_FIXTURE || null;
let smokeRoot = null;
let utilityGeneration = 0;
let desiredWorkspacePath = null;
let desiredDocumentId = null;
let utilityRestartAllowed = true;

if (singleInstanceSmoke && process.env.CONTEXT_READER_SINGLE_INSTANCE_USER_DATA) {
  app.setPath('userData', path.resolve(process.env.CONTEXT_READER_SINGLE_INSTANCE_USER_DATA));
}

function resetReady() {
  ready = new Promise((resolve, reject) => {
    resolveReady = resolve;
    rejectReady = reject;
  });
}

function rejectPending(error, generation) {
  for (const [requestId, request] of pending.entries()) {
    if (request.generation !== generation) continue;
    request.reject(error);
    pending.delete(requestId);
  }
}

function sendUtility(target, generation, operation, args, jobId = null) {
  if (!target || target !== child || generation !== utilityGeneration) {
    const error = new Error('Reader utility generation is no longer active');
    error.code = 'UTILITY_EXITED';
    return Promise.reject(error);
  }
  const requestId = nextRequestId;
  nextRequestId += 1;
  return new Promise((resolve, reject) => {
    pending.set(requestId, { resolve, reject, generation });
    target.postMessage({ kind: 'request', requestId, operation, arguments: args, jobId });
  });
}

async function restoreUtilityState(target, generation) {
  if (!desiredWorkspacePath) return;
  await sendUtility(target, generation, 'openWorkspace', [desiredWorkspacePath]);
  if (desiredDocumentId) {
    await sendUtility(target, generation, 'openDocument', [desiredDocumentId]);
  }
}

function startUtility() {
  resetReady();
  const generation = utilityGeneration + 1;
  utilityGeneration = generation;
  const utility = utilityProcess.fork(utilityPath, [addonPath], {
    serviceName: 'Context Reader Kernel',
    stdio: 'pipe',
  });
  child = utility;
  utility.stdout?.pipe(process.stdout);
  utility.stderr?.pipe(process.stderr);

  utility.on('message', (message) => {
    if (utility !== child || generation !== utilityGeneration) return;
    if (message?.kind === 'ready') {
      restoreUtilityState(utility, generation).then(
        () => {
          resolveReady({ ...message.runtimeInfo, utilityGeneration: generation });
        },
        (error) => {
          utilityRestartAllowed = false;
          rejectReady(error);
          utility.kill();
        },
      );
      return;
    }
    if (message?.kind === 'startup-error') {
      const error = new Error(message.error?.message || 'Reader utility failed to start');
      error.code = message.error?.code || 'INTERNAL';
      rejectReady(error);
      return;
    }
    if (message?.kind !== 'response') {
      return;
    }
    const request = pending.get(message.requestId);
    if (!request || request.generation !== generation) {
      return;
    }
    pending.delete(message.requestId);
    if (message.ok) {
      request.resolve(message.value);
    } else {
      const error = new Error(message.error?.message || 'Reader operation failed');
      error.code = message.error?.code || 'INTERNAL';
      request.reject(error);
    }
  });

  utility.on('exit', (code) => {
    if (utility !== child || generation !== utilityGeneration) return;
    const error = new Error(`Reader utility exited with code ${code}`);
    error.code = 'UTILITY_EXITED';
    rejectReady(error);
    rejectPending(error, generation);
    child = null;
    if (!app.isQuitting && utilityRestartAllowed) {
      startUtility();
    }
  });
}

async function requestUtility(operation, args, jobId = null) {
  if (operation === 'closeWorkspace') {
    desiredWorkspacePath = null;
    desiredDocumentId = null;
  } else if (operation === 'closeDocument') {
    desiredDocumentId = null;
  }
  await ready;
  const target = child;
  const generation = utilityGeneration;
  if (!target) {
    const error = new Error('Reader utility is unavailable');
    error.code = 'UTILITY_UNAVAILABLE';
    throw error;
  }
  const value = await sendUtility(target, generation, operation, args, jobId);
  if (operation === 'createWorkspace' || operation === 'openWorkspace') {
    desiredWorkspacePath = path.resolve(args[0]);
  } else if (operation === 'openDocument') {
    desiredDocumentId = args[0];
  }
  return value;
}

async function armResponseFault(operation) {
  await ready;
  const target = child;
  const generation = utilityGeneration;
  if (!target) throw new Error('Reader utility is unavailable');
  await sendUtility(target, generation, '__testArmResponseFault', [operation]);
}

async function waitForUtilityRestart(previousGeneration) {
  while (utilityGeneration === previousGeneration) {
    await new Promise((resolve) => setTimeout(resolve, 10));
  }
  return ready;
}

async function expectUtilityExit(operation) {
  try {
    await operation;
  } catch (error) {
    if (error?.code === 'UTILITY_EXITED') return;
    throw error;
  }
  throw new Error('Faulted Utility operation unexpectedly returned a result');
}

async function runUtilityRestartSmoke() {
  const temporaryRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'context-reader-utility-restart-'));
  const workspacePath = path.join(temporaryRoot, 'workspace');
  try {
    grantedPaths.add(path.resolve(workspacePath));
    grantedPaths.add(path.resolve(utilityRestartFixture));
    await requestUtility('createWorkspace', [workspacePath]);
    const imported = await requestUtility('importDocument', [utilityRestartFixture], 'restart-import');
    await requestUtility('openDocument', [imported.document.documentId], 'restart-open');

    const previousGeneration = utilityGeneration;
    utilityRestartAllowed = true;
    const interrupted = requestUtility('__testDelay', [30_000]);
    await new Promise((resolve) => setImmediate(resolve));
    child.kill();
    let interruptedCode = null;
    try {
      await interrupted;
    } catch (error) {
      interruptedCode = error?.code;
    }
    const runtime = await waitForUtilityRestart(previousGeneration);
    const documents = await requestUtility('listDocuments', []);
    const page = await requestUtility('pageInfo', [0]);
    const rendered = await requestUtility('renderPage', [0, 1], 'restart-fresh-render');
    if (interruptedCode !== 'UTILITY_EXITED' || runtime.utilityGeneration <= previousGeneration
        || documents.length !== 1 || page.rotation !== 90 || !(rendered.png instanceof Uint8Array)
        || rendered.png.length <= 8) {
      throw new Error('Utility restart did not restore state or isolate the stale request');
    }

    const pageText = await requestUtility('extractPageText', [0]);
    const annotation = await requestUtility('createAnnotation', [{
      documentVersionId: imported.document.versionId,
      pageIndex: 0,
      quads: [pageText.lines[0].bounds],
      quote: { exact: pageText.lines[0].text, prefix: '', suffix: '' },
      layoutVersion: 'mupdf-1.28.3',
      color: 'yellow',
    }]);
    const initialNote = await requestUtility('updateNote', [{
      annotationId: annotation.id,
      expectedRevision: 0,
      markdownSource: 'restart revision one',
    }]);

    let faultGeneration = utilityGeneration;
    await armResponseFault('updateNote');
    await expectUtilityExit(requestUtility('updateNote', [{
      annotationId: annotation.id,
      expectedRevision: initialNote.revision,
      markdownSource: 'committed before response loss',
    }]));
    await waitForUtilityRestart(faultGeneration);
    const recoveredNotes = await requestUtility('listNotes', [imported.document.versionId]);
    let staleRevisionCode = null;
    try {
      await requestUtility('updateNote', [{
        annotationId: annotation.id,
        expectedRevision: initialNote.revision,
        markdownSource: 'stale generation overwrite',
      }]);
    } catch (error) {
      staleRevisionCode = error?.code;
    }
    const recoveredNote = recoveredNotes.find((note) => note.annotationId === annotation.id);
    const freshNote = await requestUtility('updateNote', [{
      annotationId: annotation.id,
      expectedRevision: recoveredNote?.revision,
      markdownSource: 'fresh generation revision',
    }]);
    if (recoveredNote?.revision !== 2
        || recoveredNote.markdownSource !== 'committed before response loss'
        || staleRevisionCode !== 'CONFLICT' || freshNote.revision !== 3) {
      throw new Error('Utility restart did not preserve authoritative note revision semantics');
    }

    faultGeneration = utilityGeneration;
    await armResponseFault('closeDocument');
    await expectUtilityExit(requestUtility('closeDocument', []));
    await waitForUtilityRestart(faultGeneration);
    let closedDocumentCode = null;
    try {
      await requestUtility('pageInfo', [0]);
    } catch (error) {
      closedDocumentCode = error?.code;
    }
    if (closedDocumentCode !== 'NOT_FOUND') {
      throw new Error('Document close intent was not preserved after response loss');
    }

    await requestUtility('openDocument', [imported.document.documentId], 'restart-reopen');
    faultGeneration = utilityGeneration;
    await armResponseFault('closeWorkspace');
    await expectUtilityExit(requestUtility('closeWorkspace', []));
    await waitForUtilityRestart(faultGeneration);
    let closedWorkspaceCode = null;
    try {
      await requestUtility('listDocuments', []);
    } catch (error) {
      closedWorkspaceCode = error?.code;
    }
    if (closedWorkspaceCode !== 'NOT_FOUND') {
      throw new Error('Workspace close intent was not preserved after response loss');
    }
    await requestUtility('openWorkspace', [workspacePath]);
    const recoveredDocuments = await requestUtility('listDocuments', []);
    if (recoveredDocuments.length !== 1) {
      throw new Error('Workspace did not reopen after close response loss');
    }
    process.stdout.write(
      `Electron Utility restart smoke passed: generation ${previousGeneration}`
        + ` -> ${utilityGeneration}, note revision ${freshNote.revision}\n`,
    );
  } finally {
    if (desiredDocumentId) {
      try { await requestUtility('closeDocument', []); } catch {}
    }
    if (desiredWorkspacePath) {
      try { await requestUtility('closeWorkspace', []); } catch {}
    }
    desiredDocumentId = null;
    desiredWorkspacePath = null;
    await fs.rm(temporaryRoot, { recursive: true, force: true, maxRetries: 20, retryDelay: 100 });
  }
}

function registerIpc() {
  ipcMain.handle('reader:request', async (event, request) => {
    if (event.sender !== window?.webContents || !allowedOperations.has(request?.operation)
        || !Array.isArray(request?.arguments)) {
      const error = new Error('Reader IPC request is invalid');
      error.code = 'INVALID_ARGUMENT';
      throw error;
    }
    if (['createWorkspace', 'openWorkspace', 'importDocument'].includes(request.operation)) {
      const requestedPath = request.arguments[0];
      if (typeof requestedPath !== 'string' || !grantedPaths.has(path.resolve(requestedPath))) {
        const error = new Error('Reader path has not been authorized by the host');
        error.code = 'INVALID_ARGUMENT';
        throw error;
      }
    }
    return requestUtility(request.operation, request.arguments);
  });

  ipcMain.handle('reader:start-job', async (event, request) => {
    if(event.sender !== window?.webContents || !jobOperations.has(request?.operation)
        || !Array.isArray(request?.arguments) || typeof request?.jobId !== 'string'
        || request.jobId.length === 0) {
      const error = new Error('Reader Job request is invalid');
      error.code = 'INVALID_ARGUMENT';
      throw error;
    }
    if(request.operation === 'importDocument') {
      const requestedPath = request.arguments[0];
      if(typeof requestedPath !== 'string' || !grantedPaths.has(path.resolve(requestedPath))) {
        const error = new Error('Reader path has not been authorized by the host');
        error.code = 'INVALID_ARGUMENT';
        throw error;
      }
    }
    try {
      return {
        ok: true,
        value: await requestUtility(request.operation, request.arguments, request.jobId),
      };
    } catch (error) {
      return {
        ok: false,
        error: {
          code: typeof error?.code === 'string' ? error.code : 'INTERNAL',
          message: error instanceof Error ? error.message : String(error),
        },
      };
    }
  });

  ipcMain.on('reader:cancel-job', (event, jobId) => {
    if(event.sender === window?.webContents && typeof jobId === 'string' && jobId.length > 0) {
      child?.postMessage({ kind: 'cancel-job', jobId });
    }
  });

  ipcMain.handle('reader:choose-workspace', async (event, mode) => {
    if (event.sender !== window?.webContents || (mode !== 'create' && mode !== 'open')) {
      return null;
    }
    const result = await dialog.showOpenDialog(window, {
      title: mode === 'create' ? '选择新工作区目录' : '打开工作区',
      properties: ['openDirectory', mode === 'create' ? 'createDirectory' : 'dontAddToRecent'],
    });
    if (result.canceled) return null;
    const selectedPath = path.resolve(result.filePaths[0]);
    grantedPaths.add(selectedPath);
    return selectedPath;
  });

  ipcMain.handle('reader:choose-pdf', async (event) => {
    if (event.sender !== window?.webContents) {
      return null;
    }
    const result = await dialog.showOpenDialog(window, {
      title: '导入 PDF',
      properties: ['openFile'],
      filters: [{ name: 'PDF', extensions: ['pdf'] }],
    });
    if (result.canceled) return null;
    const selectedPath = path.resolve(result.filePaths[0]);
    grantedPaths.add(selectedPath);
    return selectedPath;
  });

  ipcMain.handle('reader:smoke-config', async (event) => {
    if (event.sender !== window?.webContents || !smokeFixture) {
      return null;
    }
    smokeRoot ??= smokeWorkspacePath
      ? path.dirname(path.resolve(smokeWorkspacePath))
      : await fs.mkdtemp(path.join(os.tmpdir(), 'context-reader-renderer-'));
    const workspacePath = smokeWorkspacePath
      ? path.resolve(smokeWorkspacePath)
      : path.join(smokeRoot, 'workspace');
    const fixturePath = path.resolve(smokeFixture);
    grantedPaths.add(path.resolve(workspacePath));
    grantedPaths.add(fixturePath);
    return { workspacePath, fixturePath, phase: smokePhase };
  });

  ipcMain.handle('reader:smoke-result', async (event, result) => {
    if (event.sender !== window?.webContents || !smokeFixture) {
      return null;
    }
    try {
      if (result?.status !== 'ok' || result.phase !== smokePhase) {
        throw new Error(`Unexpected renderer result: ${JSON.stringify(result)}`);
      }
      if (smokePhase === 'setup' && result.stage === 'content') {
        if (result.documentCount !== 1 || result.annotationCount !== 1
            || result.noteRevision !== 1 || result.noteMarkdown !== 'Renderer **autosaved** note'
            || result.selectedLineCount !== 1 || result.uiContract?.palette !== true
            || result.uiContract?.island !== true || result.uiContract?.layout !== true) {
          throw new Error(`Unexpected renderer setup content: ${JSON.stringify(result)}`);
        }
      } else if (smokePhase === 'setup' && result.stage === 'closed') {
        if (result.closed !== true || desiredWorkspacePath !== null || desiredDocumentId !== null) {
          throw new Error(`Renderer setup did not close cleanly: ${JSON.stringify(result)}`);
        }
        setImmediate(() => app.exit(0));
        return { ok: true };
      } else if (smokePhase === 'recovery' && result.stage === 'recovered') {
        if (result.documentCount !== 1 || result.annotationCount !== 1 || result.noteCount !== 1
            || result.noteRevision !== 1 || result.noteMarkdown !== 'Renderer **autosaved** note'
            || result.workspaceValid !== true) {
          throw new Error(`Unexpected renderer recovery content: ${JSON.stringify(result)}`);
        }
      } else {
        throw new Error(`Unexpected renderer smoke stage: ${JSON.stringify(result)}`);
      }

      await new Promise((resolve) => setTimeout(resolve, 150));
      const image = await window.webContents.capturePage();
      const bitmap = image.toBitmap();
      const sampledBytes = new Set();
      const stride = Math.max(4, Math.floor(bitmap.length / 4096));
      for (let index = 0; index < bitmap.length; index += stride) sampledBytes.add(bitmap[index]);
      if (image.isEmpty() || bitmap.length === 0 || sampledBytes.size < 8) {
        throw new Error('Renderer screenshot is blank');
      }
      const outputPath = path.join(projectRoot, 'build', 'renderer-p2-smoke.png');
      await fs.writeFile(outputPath, image.toPNG());
      if (smokePhase === 'recovery') {
        process.stdout.write(`Electron renderer P2 recovery smoke passed: ${outputPath}\n`);
        setImmediate(() => app.exit(0));
      }
      return { ok: true };
    } catch (error) {
      process.stderr.write(`${error instanceof Error ? error.stack : String(error)}\n`);
      setImmediate(() => app.exit(1));
      throw error;
    }
  });
}

async function createWindow() {
  window = new BrowserWindow({
    width: 1440,
    height: 900,
    minWidth: 960,
    minHeight: 640,
    show: !smokeFixture && !singleInstanceSmoke && !utilityRestartFixture,
    backgroundColor: '#f4f4f5',
    webPreferences: {
      preload: preloadPath,
      nodeIntegration: false,
      contextIsolation: true,
      sandbox: true,
      webSecurity: true,
      backgroundThrottling: false,
    },
  });
  window.removeMenu();
  window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }));
  window.webContents.on('will-navigate', (event) => event.preventDefault());
  await window.loadFile(rendererPath);
}

function focusMainWindow() {
  if (!window || window.isDestroyed()) return;
  if (window.isMinimized()) window.restore();
  window.show();
  window.focus();
}

const hasSingleInstanceLock = app.requestSingleInstanceLock();
if (!hasSingleInstanceLock) {
  if (singleInstanceSmoke) process.stdout.write('single-instance-secondary-exit\n');
  app.quit();
} else {
  app.isQuitting = false;
  app.on('second-instance', () => {
    if (singleInstanceSmoke) {
      process.stdout.write('single-instance-secondary-rejected\n');
    }
    focusMainWindow();
    if (singleInstanceSmoke) process.stdout.write('single-instance-window-focused\n');
  });

  app.whenReady().then(async () => {
    registerIpc();
    startUtility();
    await createWindow();
    if (singleInstanceSmoke) process.stdout.write('single-instance-primary-ready\n');
    if (utilityRestartFixture) {
      try {
        await runUtilityRestartSmoke();
        app.exit(0);
      } catch (error) {
        process.stderr.write(`${error instanceof Error ? error.stack : String(error)}\n`);
        app.exit(1);
      }
    }
  });

  app.on('before-quit', () => {
    app.isQuitting = true;
    child?.kill();
  });

  app.on('window-all-closed', () => app.quit());
}
