const { contextBridge, ipcRenderer } = require('electron');

const readerError = (code, message) => Object.freeze({ code, message });

const invoke = (operation, ...args) => ipcRenderer.invoke('reader:request', {
  operation,
  arguments: args,
}).then((response) => {
  if (response?.ok) return response.value;
  throw readerError(
    response?.error?.code || 'INTERNAL',
    response?.error?.message || 'Reader operation failed',
  );
});

let dataPort = null;
let dataPortGeneration = 0;
const tileJobs = new Map();

function rejectTileJobs(generation, code, message) {
  for (const [jobId, job] of tileJobs) {
    if (generation !== undefined && job.generation !== generation) continue;
    tileJobs.delete(jobId);
    job.reject(readerError(code, message));
  }
}

ipcRenderer.on('reader:data-port', (event, message) => {
  const generation = Number(message?.generation ?? 0);
  if (dataPort) {
    rejectTileJobs(dataPortGeneration, 'UTILITY_EXITED', 'Reader utility data channel was replaced');
  }
  dataPort?.close();
  const port = event.ports[0] ?? null;
  dataPort = port;
  dataPortGeneration = generation;
  if (!dataPort) return;
  port.onmessage = ({ data }) => {
    if (port !== dataPort || generation !== dataPortGeneration) return;
    if (data?.kind !== 'tile-response') return;
    const job = tileJobs.get(data.jobId);
    if (!job || job.generation !== generation) return;
    tileJobs.delete(data.jobId);
    if (data.ok) job.resolve(data.value);
    else {
      job.reject(readerError(
        data.error?.code || 'INTERNAL',
        data.error?.message || 'Tile render failed',
      ));
    }
  };
  port.onmessageerror = () => {
    if (port !== dataPort) return;
    rejectTileJobs(generation, 'UTILITY_EXITED', 'Reader utility data channel failed');
    port.close();
    dataPort = null;
  };
  port.start();
});

ipcRenderer.on('reader:data-port-closed', (_event, message) => {
  const generation = Number(message?.generation ?? 0);
  rejectTileJobs(generation, 'UTILITY_EXITED', 'Reader utility exited');
  if (generation === dataPortGeneration) {
    dataPort?.close();
    dataPort = null;
  }
});

let nextJobId = 1;
const startJob = (operation, ...args) => {
  const jobId = `renderer-${Date.now()}-${nextJobId}`;
  nextJobId += 1;
  const result = ipcRenderer.invoke('reader:start-job', {
    jobId,
    operation,
    arguments: args,
  }).then((response) => {
    if (response?.ok) return response.value;
    throw readerError(
      response?.error?.code || 'INTERNAL',
      response?.error?.message || 'Reader Job failed',
    );
  });
  return Object.freeze({
    id: jobId,
    result,
    cancel: () => ipcRenderer.send('reader:cancel-job', jobId),
  });
};

const startTileJob = (request) => {
  const jobId = `tile-${Date.now()}-${nextJobId}`;
  nextJobId += 1;
  let resolve;
  let reject;
  const result = new Promise((resolveValue, rejectValue) => {
    resolve = resolveValue;
    reject = rejectValue;
  });
  if (!dataPort) {
    reject(readerError('UTILITY_UNAVAILABLE', 'Tile data channel is unavailable'));
  } else {
    const port = dataPort;
    const generation = dataPortGeneration;
    tileJobs.set(jobId, { resolve, reject, port, generation });
    port.postMessage({ kind: 'tile-request', jobId, request });
  }
  return Object.freeze({
    id: jobId,
    result,
    cancel: () => {
      const job = tileJobs.get(jobId);
      if (job) {
        tileJobs.delete(jobId);
        job.reject(readerError('CANCELLED', 'Tile render was cancelled'));
      }
      job?.port.postMessage({ kind: 'cancel-job', jobId });
    },
  });
};

contextBridge.exposeInMainWorld('contextReader', Object.freeze({
  chooseWorkspace: (mode) => ipcRenderer.invoke('reader:choose-workspace', mode),
  choosePdf: () => ipcRenderer.invoke('reader:choose-pdf'),
  chooseNoteAsset: () => ipcRenderer.invoke('reader:choose-note-asset'),
  chooseBackupExport: () => ipcRenderer.invoke('reader:choose-backup-export'),
  chooseBackupOpen: () => ipcRenderer.invoke('reader:choose-backup-open'),
  chooseRestoreTarget: () => ipcRenderer.invoke('reader:choose-restore-target'),
  chooseDiagnosticsExport: () => ipcRenderer.invoke('reader:choose-diagnostics-export'),
  createWorkspace: (workspacePath) => invoke('createWorkspace', workspacePath),
  openWorkspace: (workspacePath) => invoke('openWorkspace', workspacePath),
  closeWorkspace: () => invoke('closeWorkspace'),
  importDocument: (sourcePath) => startJob('importDocument', sourcePath),
  listDocuments: () => invoke('listDocuments'),
  openDocument: (documentId) => startJob('openDocument', documentId),
  closeDocument: () => invoke('closeDocument'),
  pageInfo: (pageIndex) => invoke('pageInfo', pageIndex),
  renderPage: (pageIndex, pixelsPerPoint) => startJob('renderPage', pageIndex, pixelsPerPoint),
  renderTile: (request) => startTileJob(request),
  extractPageText: (pageIndex) => invoke('extractPageText', pageIndex),
  pageTextLayout: (pageIndex) => invoke('pageTextLayout', pageIndex),
  selectText: (pageIndex, startPoint, endPoint) => invoke('selectText', pageIndex, startPoint, endPoint),
  createAnnotation: (command) => invoke('createAnnotation', command),
  listAnnotations: (documentVersionId) => invoke('listAnnotations', documentVersionId),
  deleteAnnotation: (annotationId) => invoke('deleteAnnotation', annotationId),
  updateNote: (command) => invoke('updateNote', command),
  listNotes: (documentVersionId) => invoke('listNotes', documentVersionId),
  rebuildSearchIndex: () => startJob('rebuildSearchIndex'),
  search: (request) => invoke('search', request),
  importNoteAsset: (annotationId, sourcePath) => startJob('importNoteAsset', annotationId, sourcePath),
  readAsset: (assetId) => invoke('readAsset', assetId),
  exportWorkspace: (destination) => startJob('exportWorkspace', destination),
  inspectBackup: (packagePath) => invoke('inspectBackup', packagePath),
  restoreWorkspace: (packagePath, target) => startJob('restoreWorkspace', packagePath, target),
  exportDiagnostics: (destination) => startJob('exportDiagnostics', destination),
  verifyWorkspace: () => invoke('verifyWorkspace'),
  smokeConfig: () => ipcRenderer.invoke('reader:smoke-config'),
  reportSmokeResult: (result) => ipcRenderer.invoke('reader:smoke-result', result),
}));
