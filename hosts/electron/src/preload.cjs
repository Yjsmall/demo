const { contextBridge, ipcRenderer } = require('electron');

const invoke = (operation, ...args) => ipcRenderer.invoke('reader:request', {
  operation,
  arguments: args,
});

let dataPort = null;
const tileJobs = new Map();
ipcRenderer.on('reader:data-port', (event) => {
  dataPort?.close();
  dataPort = event.ports[0] ?? null;
  if (!dataPort) return;
  dataPort.onmessage = ({ data }) => {
    if (data?.kind !== 'tile-response') return;
    const job = tileJobs.get(data.jobId);
    if (!job) return;
    tileJobs.delete(data.jobId);
    if (data.ok) job.resolve(data.value);
    else {
      const error = new Error(data.error?.message || 'Tile render failed');
      error.code = data.error?.code || 'INTERNAL';
      job.reject(error);
    }
  };
  dataPort.start();
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
    const error = new Error(response?.error?.message || 'Reader Job failed');
    error.code = response?.error?.code || 'INTERNAL';
    throw error;
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
    const error = new Error('Tile data channel is unavailable');
    error.code = 'UTILITY_UNAVAILABLE';
    reject(error);
  } else {
    tileJobs.set(jobId, { resolve, reject });
    dataPort.postMessage({ kind: 'tile-request', jobId, request });
  }
  return Object.freeze({
    id: jobId,
    result,
    cancel: () => {
      const job = tileJobs.get(jobId);
      if (job) {
        tileJobs.delete(jobId);
        const error = new Error('Tile render was cancelled');
        error.code = 'CANCELLED';
        job.reject(error);
      }
      dataPort?.postMessage({ kind: 'cancel-job', jobId });
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
