const api = window.contextReader;

const elements = Object.fromEntries([
  'create-workspace', 'open-workspace', 'import-pdf', 'status', 'document-count',
  'document-list', 'previous-page', 'next-page', 'page-indicator', 'zoom-out',
  'zoom-in', 'zoom-value', 'highlight-selection', 'reader-scroll', 'empty-state',
  'page-stage', 'page-image', 'annotation-layer', 'text-layer', 'selected-quote',
  'note-source', 'note-revision', 'save-note', 'delete-annotation',
  'cancel-job',
].map((id) => [id, document.getElementById(id)]));

const state = {
  workspace: null,
  documents: [],
  document: null,
  pageIndex: 0,
  pageInfo: null,
  pageText: null,
  annotations: [],
  notes: [],
  selectedAnnotationId: null,
  selectedLines: [],
  selectionText: '',
  highlightColor: 'yellow',
  zoom: 1,
  imageUrl: null,
  busy: false,
  activeJob: null,
};

function setStatus(message, isError = false) {
  elements.status.textContent = message;
  elements.status.classList.toggle('error', isError);
}

function errorMessage(error) {
  const code = typeof error?.code === 'string' ? `${error.code}: ` : '';
  return `${code}${error instanceof Error ? error.message : String(error)}`;
}

async function perform(label, operation) {
  if (state.busy) return null;
  state.busy = true;
  setStatus(label);
  updateControls();
  try {
    const result = await operation();
    setStatus(state.workspace ? `工作区 ${state.workspace.id.slice(0, 8)}` : '未打开工作区');
    return result;
  } catch (error) {
    if (error?.code === 'CANCELLED') {
      setStatus('操作已取消');
      return null;
    }
    setStatus(errorMessage(error), true);
    throw error;
  } finally {
    state.busy = false;
    updateControls();
  }
}

async function awaitJob(job) {
  state.activeJob = job;
  updateControls();
  try {
    return await job.result;
  } finally {
    if (state.activeJob?.id === job.id) state.activeJob = null;
    updateControls();
  }
}

function updateControls() {
  const hasWorkspace = Boolean(state.workspace);
  const hasDocument = Boolean(state.document);
  elements['import-pdf'].disabled = !hasWorkspace || state.busy;
  elements['previous-page'].disabled = !hasDocument || state.pageIndex === 0 || state.busy;
  elements['next-page'].disabled = !hasDocument
    || state.pageIndex + 1 >= state.document.pageCount || state.busy;
  elements['zoom-out'].disabled = !hasDocument || state.zoom <= 0.5 || state.busy;
  elements['zoom-in'].disabled = !hasDocument || state.zoom >= 3 || state.busy;
  elements['highlight-selection'].disabled = state.selectedLines.length === 0 || state.busy;
  const selected = selectedAnnotation();
  elements['note-source'].disabled = !selected || state.busy;
  elements['save-note'].disabled = !selected || state.busy;
  elements['delete-annotation'].disabled = !selected || state.busy;
  elements['cancel-job'].hidden = !state.activeJob;
  elements['cancel-job'].disabled = !state.activeJob;
}

function selectedAnnotation() {
  return state.annotations.find((annotation) => annotation.id === state.selectedAnnotationId) ?? null;
}

function selectedNote() {
  return state.notes.find((note) => note.annotationId === state.selectedAnnotationId) ?? null;
}

function renderDocuments() {
  elements['document-list'].replaceChildren();
  elements['document-count'].textContent = String(state.documents.length);
  for (const documentRecord of state.documents) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'document-item';
    button.classList.toggle('selected', state.document?.documentId === documentRecord.documentId);
    const title = document.createElement('span');
    title.className = 'document-title';
    title.textContent = documentRecord.title;
    const meta = document.createElement('span');
    meta.className = 'document-meta';
    meta.textContent = `${documentRecord.pageCount} 页`;
    button.append(title, meta);
    button.addEventListener('click', () => openDocument(documentRecord));
    elements['document-list'].append(button);
  }
}

function transformPoint(point) {
  const { widthPoints: width, heightPoints: height, rotation } = state.pageInfo;
  if (rotation === 90) return { x: height - point.y, y: point.x };
  if (rotation === 180) return { x: width - point.x, y: height - point.y };
  if (rotation === 270) return { x: point.y, y: width - point.x };
  return point;
}

function transformRect(rect) {
  const corners = [
    transformPoint({ x: rect.x, y: rect.y }),
    transformPoint({ x: rect.x + rect.width, y: rect.y }),
    transformPoint({ x: rect.x, y: rect.y + rect.height }),
    transformPoint({ x: rect.x + rect.width, y: rect.y + rect.height }),
  ];
  const xs = corners.map((point) => point.x);
  const ys = corners.map((point) => point.y);
  return {
    x: Math.min(...xs) * state.zoom,
    y: Math.min(...ys) * state.zoom,
    width: (Math.max(...xs) - Math.min(...xs)) * state.zoom,
    height: (Math.max(...ys) - Math.min(...ys)) * state.zoom,
  };
}

function place(element, rect) {
  const transformed = transformRect(rect);
  element.style.left = `${transformed.x}px`;
  element.style.top = `${transformed.y}px`;
  element.style.width = `${Math.max(1, transformed.width)}px`;
  element.style.height = `${Math.max(1, transformed.height)}px`;
  return transformed;
}

function renderLayers() {
  elements['text-layer'].replaceChildren();
  elements['annotation-layer'].replaceChildren();
  if (!state.pageInfo || !state.pageText) return;

  state.pageText.lines.forEach((line, index) => {
    const span = document.createElement('span');
    span.className = 'text-line';
    span.dataset.lineIndex = String(index);
    span.textContent = line.text;
    const bounds = place(span, line.bounds);
    span.style.fontSize = `${Math.max(4, bounds.height * 0.76)}px`;
    elements['text-layer'].append(span);
  });

  for (const annotation of state.annotations.filter((item) => item.pageIndex === state.pageIndex)) {
    annotation.quads.forEach((quad) => {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'annotation-quad';
      button.dataset.color = annotation.color;
      button.title = annotation.quote.exact;
      button.classList.toggle('selected', annotation.id === state.selectedAnnotationId);
      button.addEventListener('click', () => selectAnnotation(annotation.id));
      place(button, quad);
      elements['annotation-layer'].append(button);
    });
  }
}

function renderNote() {
  const annotation = selectedAnnotation();
  const note = selectedNote();
  elements['selected-quote'].textContent = annotation?.quote.exact || '选择一个高亮';
  elements['note-source'].value = note?.markdownSource || '';
  elements['note-revision'].textContent = `revision ${note?.revision ?? 0}`;
  updateControls();
}

function selectAnnotation(annotationId) {
  state.selectedAnnotationId = annotationId;
  renderLayers();
  renderNote();
}

function selectionChanged() {
  const selection = window.getSelection();
  state.selectedLines = [];
  state.selectionText = '';
  if (!selection || selection.isCollapsed || selection.rangeCount === 0) {
    updateControls();
    return;
  }
  const range = selection.getRangeAt(0);
  for (const line of elements['text-layer'].querySelectorAll('.text-line')) {
    if (range.intersectsNode(line)) {
      state.selectedLines.push(Number(line.dataset.lineIndex));
    }
  }
  state.selectionText = selection.toString().trim();
  updateControls();
}

async function refreshDocuments() {
  state.documents = await api.listDocuments();
  renderDocuments();
}

async function openWorkspaceAt(workspacePath, create) {
  await perform(create ? '正在创建工作区' : '正在打开工作区', async () => {
    if (state.workspace) {
      if (state.document) await api.closeDocument();
      await api.closeWorkspace();
    }
    state.workspace = create
      ? await api.createWorkspace(workspacePath)
      : await api.openWorkspace(workspacePath);
    state.document = null;
    state.annotations = [];
    state.notes = [];
    await refreshDocuments();
  });
  updateControls();
}

async function chooseWorkspace(mode) {
  const workspacePath = await api.chooseWorkspace(mode);
  if (workspacePath) await openWorkspaceAt(workspacePath, mode === 'create');
}

async function importPdfAt(sourcePath) {
  const imported = await perform(
    '正在导入 PDF',
    () => awaitJob(api.importDocument(sourcePath)),
  );
  if (!imported) return null;
  await refreshDocuments();
  await openDocument(imported.document);
  return imported;
}

async function choosePdf() {
  const sourcePath = await api.choosePdf();
  if (sourcePath) await importPdfAt(sourcePath);
}

async function openDocument(documentRecord) {
  await perform('正在打开文档', async () => {
    if (state.document) await api.closeDocument();
    state.document = null;
    state.document = await awaitJob(api.openDocument(documentRecord.documentId));
    state.pageIndex = 0;
    state.selectedAnnotationId = null;
    state.annotations = await api.listAnnotations(state.document.versionId);
    state.notes = await api.listNotes(state.document.versionId);
    await renderPage();
  });
  renderDocuments();
  updateControls();
}

async function renderPage() {
  const [pageInfo, rendered, pageText] = await Promise.all([
    api.pageInfo(state.pageIndex),
    awaitJob(api.renderPage(state.pageIndex, state.zoom)),
    api.extractPageText(state.pageIndex),
  ]);
  state.pageInfo = pageInfo;
  state.pageText = pageText;
  if (state.imageUrl) URL.revokeObjectURL(state.imageUrl);
  const png = rendered.png?.data ? new Uint8Array(rendered.png.data) : new Uint8Array(rendered.png);
  state.imageUrl = URL.createObjectURL(new Blob([png], { type: 'image/png' }));
  elements['page-image'].src = state.imageUrl;
  elements['page-stage'].style.width = `${rendered.widthPixels}px`;
  elements['page-stage'].style.height = `${rendered.heightPixels}px`;
  elements['page-stage'].hidden = false;
  elements['empty-state'].hidden = true;
  elements['page-indicator'].textContent = `${state.pageIndex + 1} / ${state.document.pageCount}`;
  elements['zoom-value'].textContent = `${Math.round(state.zoom * 100)}%`;
  renderLayers();
  renderNote();
}

async function changePage(delta) {
  const target = state.pageIndex + delta;
  if (target < 0 || target >= state.document.pageCount) return;
  await perform('正在加载页面', async () => {
    state.pageIndex = target;
    state.selectedAnnotationId = null;
    await renderPage();
  });
  updateControls();
}

async function changeZoom(delta) {
  const next = Math.min(3, Math.max(0.5, Number((state.zoom + delta).toFixed(2))));
  if (next === state.zoom) return;
  await perform('正在缩放页面', async () => {
    state.zoom = next;
    await renderPage();
  });
  updateControls();
}

async function createHighlight() {
  if (state.selectedLines.length === 0) return null;
  const lines = state.selectedLines.map((index) => state.pageText.lines[index]);
  const annotation = await perform('正在保存高亮', () => api.createAnnotation({
    documentVersionId: state.document.versionId,
    pageIndex: state.pageIndex,
    quads: lines.map((line) => line.bounds),
    quote: {
      exact: state.selectionText || lines.map((line) => line.text).join('\n'),
      prefix: '',
      suffix: '',
    },
    layoutVersion: 'mupdf-1.28.3',
    color: state.highlightColor,
  }));
  if (!annotation) return null;
  state.annotations.push(annotation);
  state.selectedAnnotationId = annotation.id;
  state.selectedLines = [];
  state.selectionText = '';
  window.getSelection()?.removeAllRanges();
  renderLayers();
  renderNote();
  return annotation;
}

async function saveNote() {
  const annotation = selectedAnnotation();
  if (!annotation) return null;
  const existing = selectedNote();
  const note = await perform('正在保存笔记', () => api.updateNote({
    annotationId: annotation.id,
    expectedRevision: existing?.revision ?? 0,
    markdownSource: elements['note-source'].value,
  }));
  if (!note) return null;
  state.notes = state.notes.filter((item) => item.annotationId !== annotation.id);
  state.notes.push(note);
  renderNote();
  return note;
}

async function deleteAnnotation() {
  const annotation = selectedAnnotation();
  if (!annotation) return;
  await perform('正在删除高亮', () => api.deleteAnnotation(annotation.id));
  state.annotations = state.annotations.filter((item) => item.id !== annotation.id);
  state.notes = state.notes.filter((item) => item.annotationId !== annotation.id);
  state.selectedAnnotationId = null;
  renderLayers();
  renderNote();
}

function inspectUiContract() {
  const shell = document.querySelector('.window-shell');
  const island = document.querySelector('.content-island');
  const library = document.querySelector('.library-pane');
  const reader = document.querySelector('.reader-pane');
  const notes = document.querySelector('.note-pane');
  const shellStyle = getComputedStyle(shell);
  const islandStyle = getComputedStyle(island);
  const islandRect = island.getBoundingClientRect();
  const libraryRect = library.getBoundingClientRect();
  const readerRect = reader.getBoundingClientRect();
  const notesRect = notes.getBoundingClientRect();
  return {
    palette: shellStyle.backgroundColor === 'rgb(244, 244, 245)'
      && islandStyle.backgroundColor === 'rgb(255, 255, 255)',
    island: islandStyle.borderRadius === '12px' && islandRect.width > 700,
    layout: libraryRect.width >= 220 && readerRect.width > notesRect.width
      && islandRect.right <= window.innerWidth && islandRect.bottom <= window.innerHeight,
  };
}

elements['create-workspace'].addEventListener('click', () => chooseWorkspace('create'));
elements['open-workspace'].addEventListener('click', () => chooseWorkspace('open'));
elements['import-pdf'].addEventListener('click', choosePdf);
elements['previous-page'].addEventListener('click', () => changePage(-1));
elements['next-page'].addEventListener('click', () => changePage(1));
elements['zoom-out'].addEventListener('click', () => changeZoom(-0.25));
elements['zoom-in'].addEventListener('click', () => changeZoom(0.25));
elements['highlight-selection'].addEventListener('click', createHighlight);
elements['save-note'].addEventListener('click', saveNote);
elements['delete-annotation'].addEventListener('click', deleteAnnotation);
elements['cancel-job'].addEventListener('click', () => {
  if (!state.activeJob) return;
  state.activeJob.cancel();
  setStatus('正在取消操作');
  elements['cancel-job'].disabled = true;
});
elements['text-layer'].addEventListener('mouseup', selectionChanged);

for (const swatch of document.querySelectorAll('.swatch')) {
  swatch.addEventListener('click', () => {
    state.highlightColor = swatch.dataset.color;
    for (const item of document.querySelectorAll('.swatch')) {
      item.classList.toggle('selected', item === swatch);
    }
  });
}

async function runSmoke() {
  const config = await api.smokeConfig();
  if (!config) return;
  try {
    await openWorkspaceAt(config.workspacePath, true);
    await importPdfAt(config.fixturePath);
    state.selectedLines = [0];
    state.selectionText = state.pageText.lines[0].text;
    const annotation = await createHighlight();
    elements['note-source'].value = 'Renderer **context** note';
    const note = await saveNote();
    api.reportSmokeResult({
      status: 'ok',
      documentCount: state.documents.length,
      annotationCount: annotation ? 1 : 0,
      noteRevision: note?.revision,
      uiContract: inspectUiContract(),
    });
  } catch (error) {
    api.reportSmokeResult({ status: 'error', message: errorMessage(error) });
  }
}

updateControls();
runSmoke();
