const { contextBridge, ipcRenderer } = require('electron');

const invoke = (operation, ...args) => ipcRenderer.invoke('reader:request', {
  operation,
  arguments: args,
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

contextBridge.exposeInMainWorld('contextReader', Object.freeze({
  chooseWorkspace: (mode) => ipcRenderer.invoke('reader:choose-workspace', mode),
  choosePdf: () => ipcRenderer.invoke('reader:choose-pdf'),
  createWorkspace: (workspacePath) => invoke('createWorkspace', workspacePath),
  openWorkspace: (workspacePath) => invoke('openWorkspace', workspacePath),
  closeWorkspace: () => invoke('closeWorkspace'),
  importDocument: (sourcePath) => startJob('importDocument', sourcePath),
  listDocuments: () => invoke('listDocuments'),
  openDocument: (documentId) => startJob('openDocument', documentId),
  closeDocument: () => invoke('closeDocument'),
  pageInfo: (pageIndex) => invoke('pageInfo', pageIndex),
  renderPage: (pageIndex, pixelsPerPoint) => startJob('renderPage', pageIndex, pixelsPerPoint),
  extractPageText: (pageIndex) => invoke('extractPageText', pageIndex),
  createAnnotation: (command) => invoke('createAnnotation', command),
  listAnnotations: (documentVersionId) => invoke('listAnnotations', documentVersionId),
  deleteAnnotation: (annotationId) => invoke('deleteAnnotation', annotationId),
  updateNote: (command) => invoke('updateNote', command),
  listNotes: (documentVersionId) => invoke('listNotes', documentVersionId),
  verifyWorkspace: () => invoke('verifyWorkspace'),
  smokeConfig: () => ipcRenderer.invoke('reader:smoke-config'),
  reportSmokeResult: (result) => ipcRenderer.invoke('reader:smoke-result', result),
}));
