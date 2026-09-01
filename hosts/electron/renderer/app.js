const api = window.contextReader;

const elements = Object.fromEntries([
  'create-workspace', 'open-workspace', 'export-workspace', 'restore-workspace', 'export-diagnostics', 'import-pdf', 'status', 'document-count',
  'document-list', 'previous-page', 'next-page', 'page-indicator', 'zoom-out',
  'zoom-in', 'zoom-value', 'highlight-selection', 'reader-scroll', 'empty-state',
  'pages-container', 'selected-quote',
  'note-source', 'note-revision', 'save-note', 'delete-annotation',
  'cancel-job', 'note-edit-tab', 'note-preview-tab', 'note-preview', 'insert-note-asset',
].map((id) => [id, document.getElementById(id)]));

const state = {
  workspace: null,
  documents: [],
  document: null,
  pageIndex: 0,
  pageInfos: [],
  pageViews: new Map(),
  annotations: [],
  notes: [],
  selectedAnnotationId: null,
  pendingSelection: null,
  highlightColor: 'yellow',
  zoom: 1,
  generation: 0,
  tileJobs: new Map(),
  tileQueue: [],
  activeTileCount: 0,
  tileCache: new Map(),
  tileCacheBytes: 0,
  pageObserver: null,
  scrollRenderTimer: null,
  devicePixelRatio: window.devicePixelRatio || 1,
  busy: false,
  activeJob: null,
  noteDirty: false,
  noteAutosaveTimer: null,
  noteSavingPromise: null,
  noteMode: 'edit',
};

const noteAutosaveDelayMs = 350;
const tileEdge = 512;
const maximumConcurrentTiles = 8;
const tileCacheBudgetBytes = 256 * 1024 * 1024;

function setStatus(message, isError = false) {
  elements.status.textContent = message;
  elements.status.classList.toggle('error', isError);
}

function errorMessage(error) {
  const code = typeof error?.code === 'string' ? `${error.code}: ` : '';
  const message = typeof error?.message === 'string' ? error.message : String(error);
  return `${code}${message}`;
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
  elements['export-workspace'].disabled = !hasWorkspace || state.busy;
  elements['restore-workspace'].disabled = state.busy;
  elements['export-diagnostics'].disabled = state.busy;
  elements['previous-page'].disabled = !hasDocument || state.pageIndex === 0 || state.busy;
  elements['next-page'].disabled = !hasDocument
    || state.pageIndex + 1 >= state.document.pageCount || state.busy;
  elements['zoom-out'].disabled = !hasDocument || state.zoom <= 0.5 || state.busy;
  elements['zoom-in'].disabled = !hasDocument || state.zoom >= 3 || state.busy;
  elements['highlight-selection'].disabled = !state.pendingSelection || state.busy;
  const selected = selectedAnnotation();
  elements['note-source'].disabled = !selected || state.busy;
  elements['save-note'].disabled = !selected || state.busy;
  elements['delete-annotation'].disabled = !selected || state.busy;
  elements['insert-note-asset'].disabled = !selected || state.busy;
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

function transformRect(rect, info) {
  const transformPoint = (point) => {
    if (info.rotation === 90) return { x: info.heightPoints - point.y, y: point.x };
    if (info.rotation === 180) return { x: info.widthPoints - point.x, y: info.heightPoints - point.y };
    if (info.rotation === 270) return { x: point.y, y: info.widthPoints - point.x };
    return point;
  };
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

function place(element, rect, info) {
  const transformed = transformRect(rect, info);
  element.style.left = `${transformed.x}px`;
  element.style.top = `${transformed.y}px`;
  element.style.width = `${Math.max(1, transformed.width)}px`;
  element.style.height = `${Math.max(1, transformed.height)}px`;
  return transformed;
}

function quadBounds(quad) {
  const xs = [quad.upperLeft.x, quad.upperRight.x, quad.lowerLeft.x, quad.lowerRight.x];
  const ys = [quad.upperLeft.y, quad.upperRight.y, quad.lowerLeft.y, quad.lowerRight.y];
  return {
    x: Math.min(...xs),
    y: Math.min(...ys),
    width: Math.max(...xs) - Math.min(...xs),
    height: Math.max(...ys) - Math.min(...ys),
  };
}

function pageDisplaySize(info) {
  return info.rotation === 90 || info.rotation === 270
    ? { width: info.heightPoints, height: info.widthPoints }
    : { width: info.widthPoints, height: info.heightPoints };
}

function renderLayers(pageIndex) {
  const view = state.pageViews.get(pageIndex);
  if (!view) return;
  view.textLayer.replaceChildren();
  view.annotationLayer.replaceChildren();
  if (!view.layout) return;

  view.layout.units.forEach((unit, index) => {
    const span = document.createElement('span');
    span.className = 'text-unit';
    span.dataset.pageIndex = String(pageIndex);
    span.dataset.unitIndex = String(index);
    span.textContent = unit.text;
    const bounds = place(span, quadBounds(unit.quad), view.info);
    span.style.fontSize = `${Math.max(4, bounds.height * 0.76)}px`;
    view.textLayer.append(span);
  });

  for (const annotation of state.annotations.filter((item) => item.pageIndex === pageIndex)) {
    annotation.quads.forEach((quad) => {
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'annotation-quad';
      button.dataset.color = annotation.color;
      button.title = annotation.quote.exact;
      button.classList.toggle('selected', annotation.id === state.selectedAnnotationId);
      button.addEventListener('click', () => { void selectAnnotation(annotation.id); });
      place(button, quad, view.info);
      view.annotationLayer.append(button);
    });
  }
}

function renderNote() {
  const annotation = selectedAnnotation();
  const note = selectedNote();
  elements['selected-quote'].textContent = annotation?.quote.exact || '选择一个高亮';
  if (!state.noteDirty && !state.noteSavingPromise) {
    elements['note-source'].value = note?.markdownSource || '';
  }
  elements['note-revision'].textContent = `revision ${note?.revision ?? 0}`;
  renderNotePreview();
  elements['note-source'].hidden = state.noteMode !== 'edit';
  elements['note-preview'].hidden = state.noteMode !== 'preview';
  elements['note-edit-tab'].classList.toggle('selected', state.noteMode === 'edit');
  elements['note-preview-tab'].classList.toggle('selected', state.noteMode === 'preview');
  elements['note-edit-tab'].setAttribute('aria-selected', String(state.noteMode === 'edit'));
  elements['note-preview-tab'].setAttribute('aria-selected', String(state.noteMode === 'preview'));
  updateControls();
}

function renderNotePreview() {
  const preview = elements['note-preview'];
  for (const image of preview.querySelectorAll('img[data-object-url]')) {
    URL.revokeObjectURL(image.dataset.objectUrl);
  }
  preview.innerHTML = window.contextReaderMarkdown.render(elements['note-source'].value);
  for (const image of preview.querySelectorAll('img[src^="reader-asset:"]')) {
    const assetId = image.getAttribute('src').slice('reader-asset:'.length);
    void api.readAsset(assetId).then(({ asset, bytes }) => {
      const objectUrl = URL.createObjectURL(new Blob([bytes], { type: asset.mediaType }));
      image.src = objectUrl;
      image.dataset.objectUrl = objectUrl;
    }).catch(() => image.remove());
  }
}

async function insertNoteAsset() {
  const annotation = selectedAnnotation();
  if (!annotation) return;
  const sourcePath = await api.chooseNoteAsset();
  if (!sourcePath) return;
  const asset = await perform('正在导入图片', () => awaitJob(api.importNoteAsset(annotation.id, sourcePath)));
  if (!asset) return;
  const source = elements['note-source'];
  const markdown = `![image](reader-asset:${asset.id})`;
  const start = source.selectionStart;
  const end = source.selectionEnd;
  source.setRangeText(markdown, start, end, 'end');
  source.dispatchEvent(new Event('input', { bubbles: true }));
}

function setNoteMode(mode) {
  state.noteMode = mode;
  renderNote();
}

async function selectAnnotation(annotationId) {
  if (annotationId !== state.selectedAnnotationId) await flushPendingNote();
  state.selectedAnnotationId = annotationId;
  const annotation = selectedAnnotation();
  if (annotation) renderLayers(annotation.pageIndex);
  renderNote();
}

function clearNoteAutosaveTimer() {
  if (state.noteAutosaveTimer !== null) {
    clearTimeout(state.noteAutosaveTimer);
    state.noteAutosaveTimer = null;
  }
}

function scheduleNoteAutosave() {
  if (!selectedAnnotation()) return;
  state.noteDirty = true;
  clearNoteAutosaveTimer();
  setStatus('笔记尚未保存');
  state.noteAutosaveTimer = setTimeout(() => {
    state.noteAutosaveTimer = null;
    void saveNote(true).catch(() => {});
  }, noteAutosaveDelayMs);
}

async function flushPendingNote() {
  clearNoteAutosaveTimer();
  if (state.noteSavingPromise) await state.noteSavingPromise;
  if (state.noteDirty) return saveNote(true);
  return selectedNote();
}

function selectionUnitFromNode(node) {
  const element = node?.nodeType === Node.ELEMENT_NODE ? node : node?.parentElement;
  return element?.closest?.('.text-unit') ?? null;
}

async function selectionChanged() {
  const selection = window.getSelection();
  state.pendingSelection = null;
  if (!selection || selection.isCollapsed || selection.rangeCount === 0) {
    updateControls();
    return;
  }
  const startElement = selectionUnitFromNode(selection.anchorNode);
  const endElement = selectionUnitFromNode(selection.focusNode);
  if (!startElement || !endElement || startElement.dataset.pageIndex !== endElement.dataset.pageIndex) {
    updateControls();
    return;
  }
  const pageIndex = Number(startElement.dataset.pageIndex);
  const view = state.pageViews.get(pageIndex);
  const startUnit = view?.layout?.units[Number(startElement.dataset.unitIndex)];
  const endUnit = view?.layout?.units[Number(endElement.dataset.unitIndex)];
  if (!startUnit || !endUnit) return;
  const center = (quad) => {
    const bounds = quadBounds(quad);
    return { x: bounds.x + bounds.width / 2, y: bounds.y + bounds.height / 2 };
  };
  const generation = state.generation;
  try {
    const exact = await api.selectText(pageIndex, center(startUnit.quad), center(endUnit.quad));
    if (generation !== state.generation) return;
    state.pendingSelection = exact;
    state.pageIndex = pageIndex;
    updatePageIndicator();
  } catch (error) {
    setStatus(errorMessage(error), true);
  }
  updateControls();
}

async function refreshDocuments() {
  state.documents = await api.listDocuments();
  renderDocuments();
}

function resetWorkspaceUiState() {
  state.documents = [];
  state.pageInfos = [];
  state.annotations = [];
  state.notes = [];
  state.selectedAnnotationId = null;
  state.pendingSelection = null;
  state.noteDirty = false;
  renderDocuments();
  renderNote();
  updatePageIndicator();
}

async function closeCurrentWorkspace() {
  clearPageViews();
  if (state.document) {
    await api.closeDocument();
    state.document = null;
  }
  if (state.workspace) {
    await api.closeWorkspace();
    state.workspace = null;
  }
  resetWorkspaceUiState();
}

async function openWorkspaceAt(workspacePath, create) {
  await flushPendingNote();
  await perform(create ? '正在创建工作区' : '正在打开工作区', async () => {
    if (state.workspace || state.document) await closeCurrentWorkspace();
    else {
      clearPageViews();
      resetWorkspaceUiState();
    }
    state.workspace = create
      ? await api.createWorkspace(workspacePath)
      : await api.openWorkspace(workspacePath);
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

async function exportWorkspace() {
  const destination = await api.chooseBackupExport();
  if (!destination) return;
  await perform('正在导出工作区', () => awaitJob(api.exportWorkspace(destination)));
}

async function restoreWorkspace() {
  const packagePath = await api.chooseBackupOpen();
  if (!packagePath) return;
  const inspection = await api.inspectBackup(packagePath);
  if (!inspection.valid) throw new Error('备份验证失败');
  const target = await api.chooseRestoreTarget();
  if (!target) return;
  await flushPendingNote();
  await perform('正在恢复工作区', async () => {
    if (state.workspace || state.document) await closeCurrentWorkspace();
    else {
      clearPageViews();
      resetWorkspaceUiState();
    }
    state.workspace = await awaitJob(api.restoreWorkspace(packagePath, target));
    await refreshDocuments();
  });
}

async function exportDiagnostics() {
  const destination = await api.chooseDiagnosticsExport();
  if (!destination) return;
  await perform('正在导出诊断包', () => awaitJob(api.exportDiagnostics(destination)));
}

async function openDocument(documentRecord) {
  await flushPendingNote();
  await perform('正在打开文档', async () => {
    if (state.document) await api.closeDocument();
    state.document = null;
    state.document = await awaitJob(api.openDocument(documentRecord.documentId));
    state.pageIndex = 0;
    state.selectedAnnotationId = null;
    state.noteDirty = false;
    state.annotations = await api.listAnnotations(state.document.versionId);
    state.notes = await api.listNotes(state.document.versionId);
    state.pageInfos = [];
    for (let start = 0; start < state.document.pageCount; start += 16) {
      const indexes = Array.from(
        { length: Math.min(16, state.document.pageCount - start) },
        (_, offset) => start + offset,
      );
      state.pageInfos.push(...await Promise.all(indexes.map((index) => api.pageInfo(index))));
    }
    initializePageViews();
    const firstView = state.pageViews.get(0);
    if (firstView) {
      firstView.near = true;
      await activatePage(0);
    }
  });
  renderDocuments();
  updateControls();
}

function cancelTileWork() {
  state.tileQueue.length = 0;
  for (const job of state.tileJobs.values()) job.cancel();
  state.tileJobs.clear();
}

function bumpGeneration(clearTiles = false) {
  state.generation += 1;
  cancelTileWork();
  state.pendingSelection = null;
  if (clearTiles) {
    for (const view of state.pageViews.values()) view.tileLayer.replaceChildren();
  }
}

function updatePageIndicator() {
  elements['page-indicator'].textContent = state.document
    ? `${state.pageIndex + 1} / ${state.document.pageCount}` : '0 / 0';
  elements['zoom-value'].textContent = `${Math.round(state.zoom * 100)}%`;
}

function clearPageViews() {
  state.pageObserver?.disconnect();
  state.pageObserver = null;
  bumpGeneration(true);
  state.pageViews.clear();
  elements['pages-container'].replaceChildren();
  elements['pages-container'].hidden = true;
  elements['empty-state'].hidden = false;
}

function initializePageViews() {
  clearPageViews();
  const fragment = document.createDocumentFragment();
  for (const info of state.pageInfos) {
    const display = pageDisplaySize(info);
    const stage = document.createElement('section');
    stage.className = 'page-stage';
    stage.dataset.pageIndex = String(info.index);
    stage.setAttribute('aria-label', `第 ${info.index + 1} 页`);
    stage.style.width = `${display.width * state.zoom}px`;
    stage.style.height = `${display.height * state.zoom}px`;
    const tileLayer = document.createElement('div');
    tileLayer.className = 'tile-layer';
    const annotationLayer = document.createElement('div');
    annotationLayer.className = 'annotation-layer';
    const textLayer = document.createElement('div');
    textLayer.className = 'text-layer';
    stage.append(tileLayer, annotationLayer, textLayer);
    state.pageViews.set(info.index, {
      info, stage, tileLayer, annotationLayer, textLayer, layout: null, near: false,
    });
    fragment.append(stage);
  }
  elements['pages-container'].append(fragment);
  elements['pages-container'].hidden = false;
  elements['empty-state'].hidden = true;
  state.pageObserver = new IntersectionObserver((entries) => {
    for (const entry of entries) {
      const pageIndex = Number(entry.target.dataset.pageIndex);
      const view = state.pageViews.get(pageIndex);
      if (!view) continue;
      view.near = entry.isIntersecting || pageIndex === state.pageIndex;
      if (view.near) void activatePage(pageIndex);
      else {
        view.tileLayer.replaceChildren();
        view.textLayer.replaceChildren();
        view.annotationLayer.replaceChildren();
      }
    }
  }, { root: elements['reader-scroll'], rootMargin: '800px 0px', threshold: 0 });
  for (const view of state.pageViews.values()) state.pageObserver.observe(view.stage);
  updatePageIndicator();
  renderNote();
}

function tileKey(request) {
  return [state.document.versionId, request.pageIndex, request.pixelsPerPoint,
    request.xPixels, request.yPixels, request.widthPixels, request.heightPixels].join(':');
}

function cacheTile(key, tile) {
  const pixels = new Uint8ClampedArray(tile.rgba);
  const existing = state.tileCache.get(key);
  if (existing) state.tileCacheBytes -= existing.pixels.byteLength;
  state.tileCache.delete(key);
  state.tileCache.set(key, { ...tile, pixels });
  state.tileCacheBytes += pixels.byteLength;
  while (state.tileCacheBytes > tileCacheBudgetBytes && state.tileCache.size > 1) {
    const oldestKey = state.tileCache.keys().next().value;
    const oldest = state.tileCache.get(oldestKey);
    state.tileCache.delete(oldestKey);
    state.tileCacheBytes -= oldest.pixels.byteLength;
  }
  return state.tileCache.get(key);
}

function drawTile(view, tile, dpr) {
  if (!view.near || view.tileLayer.querySelector(`[data-tile-key="${tile.key}"]`)) return;
  const canvas = document.createElement('canvas');
  canvas.className = 'tile-canvas';
  canvas.dataset.tileKey = tile.key;
  canvas.width = tile.widthPixels;
  canvas.height = tile.heightPixels;
  canvas.style.left = `${tile.xPixels / dpr}px`;
  canvas.style.top = `${tile.yPixels / dpr}px`;
  canvas.style.width = `${tile.widthPixels / dpr}px`;
  canvas.style.height = `${tile.heightPixels / dpr}px`;
  canvas.getContext('2d', { alpha: false }).putImageData(
    new ImageData(tile.pixels, tile.widthPixels, tile.heightPixels), 0, 0,
  );
  view.tileLayer.append(canvas);
}

function pumpTileQueue() {
  while (state.activeTileCount < maximumConcurrentTiles && state.tileQueue.length > 0) {
    const item = state.tileQueue.shift();
    if (item.generation !== state.generation || !item.view.near) continue;
    const job = api.renderTile(item.request);
    state.tileJobs.set(job.id, job);
    state.activeTileCount += 1;
    job.result.then((result) => {
      if (result.generation !== state.generation || item.generation !== state.generation) return;
      const tile = cacheTile(item.key, result);
      tile.key = item.key;
      drawTile(item.view, tile, item.dpr);
    }).catch((error) => {
      if (error?.code !== 'CANCELLED') setStatus(errorMessage(error), true);
    }).finally(() => {
      state.tileJobs.delete(job.id);
      state.activeTileCount -= 1;
      pumpTileQueue();
    });
  }
}

function schedulePageTiles(pageIndex) {
  const view = state.pageViews.get(pageIndex);
  if (!view?.near) return;
  const dpr = window.devicePixelRatio || 1;
  const scale = state.zoom * dpr;
  const display = pageDisplaySize(view.info);
  const pageWidth = Math.ceil(display.width * scale);
  const pageHeight = Math.ceil(display.height * scale);
  for (let y = 0; y < pageHeight; y += tileEdge) {
    for (let x = 0; x < pageWidth; x += tileEdge) {
      const request = {
        pageIndex, pixelsPerPoint: scale, xPixels: x, yPixels: y,
        widthPixels: Math.min(tileEdge, pageWidth - x),
        heightPixels: Math.min(tileEdge, pageHeight - y), generation: state.generation,
      };
      const key = tileKey(request);
      const cached = state.tileCache.get(key);
      if (cached) {
        state.tileCache.delete(key);
        state.tileCache.set(key, cached);
        cached.key = key;
        drawTile(view, cached, dpr);
      } else {
        state.tileQueue.push({ request, key, view, dpr, generation: state.generation });
      }
    }
  }
  pumpTileQueue();
}

async function activatePage(pageIndex) {
  const view = state.pageViews.get(pageIndex);
  if (!view?.near) return;
  schedulePageTiles(pageIndex);
  if (!view.layout) {
    const generation = state.generation;
    try {
      const layout = await api.pageTextLayout(pageIndex);
      if (!view.near || generation !== state.generation) return;
      view.layout = layout;
    } catch (error) {
      setStatus(errorMessage(error), true);
      return;
    }
  }
  renderLayers(pageIndex);
}

async function waitForTileIdle(timeoutMs = 5000) {
  const deadline = performance.now() + timeoutMs;
  while (performance.now() < deadline) {
    const hasCanvas = elements['pages-container'].querySelector('.tile-canvas') !== null;
    if (hasCanvas && state.activeTileCount === 0 && state.tileQueue.length === 0) return;
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
  throw new Error(
    `Visible Tile rendering timed out (active=${state.activeTileCount}, queued=${state.tileQueue.length}, generation=${state.generation}, status=${elements.status.textContent})`,
  );
}

function inspectTilePixels() {
  let minimum = 255;
  let maximum = 0;
  let canvasCount = 0;
  for (const canvas of elements['pages-container'].querySelectorAll('.tile-canvas')) {
    canvasCount += 1;
    const pixels = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
    const stride = Math.max(4, Math.floor(pixels.length / 4096 / 4) * 4);
    for (let index = 0; index < pixels.length; index += stride) {
      minimum = Math.min(minimum, pixels[index], pixels[index + 1], pixels[index + 2]);
      maximum = Math.max(maximum, pixels[index], pixels[index + 1], pixels[index + 2]);
    }
  }
  return { canvasCount, minimum, maximum };
}

function visiblePageIndex() {
  const viewport = elements['reader-scroll'].getBoundingClientRect();
  let best = state.pageIndex;
  let bestDistance = Number.POSITIVE_INFINITY;
  for (const [pageIndex, view] of state.pageViews) {
    const rect = view.stage.getBoundingClientRect();
    if (rect.bottom < viewport.top || rect.top > viewport.bottom) continue;
    const distance = Math.abs((rect.top + rect.bottom) / 2 - (viewport.top + viewport.bottom) / 2);
    if (distance < bestDistance) {
      best = pageIndex;
      bestDistance = distance;
    }
  }
  return best;
}

async function changePage(delta) {
  const target = state.pageIndex + delta;
  if (target < 0 || target >= state.document.pageCount) return;
  await flushPendingNote();
  state.pageIndex = target;
  state.selectedAnnotationId = null;
  state.pageViews.get(target)?.stage.scrollIntoView({ block: 'start' });
  updatePageIndicator();
  updateControls();
  renderNote();
}

async function changeZoom(delta) {
  const next = Math.min(3, Math.max(0.5, Number((state.zoom + delta).toFixed(2))));
  if (next === state.zoom) return;
  state.zoom = next;
  initializePageViews();
  state.pageViews.get(state.pageIndex)?.stage.scrollIntoView({ block: 'start' });
  updateControls();
}

async function createHighlight() {
  const selection = state.pendingSelection;
  if (!selection) return null;
  await flushPendingNote();
  const annotation = await perform('正在保存高亮', () => api.createAnnotation({
    documentVersionId: state.document.versionId,
    pageIndex: selection.pageIndex,
    quads: selection.quads.map(quadBounds),
    quote: selection.quote,
    layoutVersion: String(selection.layoutVersion),
    color: state.highlightColor,
    anchorVersion: 2,
    textStart: selection.logicalStart,
    textEnd: selection.logicalEnd,
    direction: selection.direction,
  }));
  if (!annotation) return null;
  state.annotations.push(annotation);
  state.selectedAnnotationId = annotation.id;
  state.pendingSelection = null;
  window.getSelection()?.removeAllRanges();
  renderLayers(selection.pageIndex);
  renderNote();
  return annotation;
}

async function saveNote(automatic = false) {
  const annotation = selectedAnnotation();
  if (!annotation) return null;
  clearNoteAutosaveTimer();
  if (state.noteSavingPromise) return state.noteSavingPromise;
  if (automatic && !state.noteDirty) return selectedNote();
  if (state.busy) {
    if (automatic) scheduleNoteAutosave();
    return null;
  }
  const existing = selectedNote();
  const markdownSource = elements['note-source'].value;
  state.noteDirty = false;
  const saving = perform(automatic ? '正在自动保存笔记' : '正在保存笔记', () => api.updateNote({
    annotationId: annotation.id,
    expectedRevision: existing?.revision ?? 0,
    markdownSource,
  })).then((note) => {
    if (!note) return null;
    state.notes = state.notes.filter((item) => item.annotationId !== annotation.id);
    state.notes.push(note);
    renderNote();
    return note;
  }).catch((error) => {
    state.noteDirty = true;
    throw error;
  });
  state.noteSavingPromise = saving;
  try {
    return await saving;
  } finally {
    if (state.noteSavingPromise === saving) state.noteSavingPromise = null;
  }
}

async function deleteAnnotation() {
  const annotation = selectedAnnotation();
  if (!annotation) return;
  clearNoteAutosaveTimer();
  state.noteDirty = false;
  await perform('正在删除高亮', () => api.deleteAnnotation(annotation.id));
  state.annotations = state.annotations.filter((item) => item.id !== annotation.id);
  state.notes = state.notes.filter((item) => item.annotationId !== annotation.id);
  state.selectedAnnotationId = null;
  renderLayers(annotation.pageIndex);
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
elements['export-workspace'].addEventListener('click', exportWorkspace);
elements['restore-workspace'].addEventListener('click', () => restoreWorkspace().catch((error) => setStatus(errorMessage(error), true)));
elements['export-diagnostics'].addEventListener('click', exportDiagnostics);
elements['import-pdf'].addEventListener('click', choosePdf);
elements['previous-page'].addEventListener('click', () => changePage(-1));
elements['next-page'].addEventListener('click', () => changePage(1));
elements['zoom-out'].addEventListener('click', () => changeZoom(-0.25));
elements['zoom-in'].addEventListener('click', () => changeZoom(0.25));
elements['highlight-selection'].addEventListener('click', createHighlight);
elements['save-note'].addEventListener('click', () => saveNote(false));
elements['delete-annotation'].addEventListener('click', deleteAnnotation);
elements['note-source'].addEventListener('input', () => {
  renderNotePreview();
  scheduleNoteAutosave();
});
elements['note-edit-tab'].addEventListener('click', () => setNoteMode('edit'));
elements['note-preview-tab'].addEventListener('click', () => setNoteMode('preview'));
elements['insert-note-asset'].addEventListener('click', insertNoteAsset);
elements['note-preview'].addEventListener('click', (event) => {
  if (event.target.closest('a')) event.preventDefault();
});
elements['cancel-job'].addEventListener('click', () => {
  if (!state.activeJob) return;
  state.activeJob.cancel();
  setStatus('正在取消操作');
  elements['cancel-job'].disabled = true;
});
elements['pages-container'].addEventListener('mouseup', () => { void selectionChanged(); });
elements['reader-scroll'].addEventListener('scroll', () => {
  if (!state.document) return;
  bumpGeneration(false);
  state.pageIndex = visiblePageIndex();
  updatePageIndicator();
  updateControls();
  if (state.scrollRenderTimer !== null) clearTimeout(state.scrollRenderTimer);
  state.scrollRenderTimer = setTimeout(() => {
    state.scrollRenderTimer = null;
    for (const [pageIndex, view] of state.pageViews) {
      if (view.near) void activatePage(pageIndex);
    }
  }, 60);
}, { passive: true });
window.addEventListener('resize', () => {
  const nextDpr = window.devicePixelRatio || 1;
  if (!state.document || nextDpr === state.devicePixelRatio) return;
  state.devicePixelRatio = nextDpr;
  initializePageViews();
  state.pageViews.get(state.pageIndex)?.stage.scrollIntoView({ block: 'start' });
});

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
    if (config.phase === 'setup') {
      await openWorkspaceAt(config.workspacePath, true);
      await importPdfAt(config.fixturePath);
      await waitForTileIdle();
      const firstLine = elements['pages-container'].querySelector('.text-unit');
      const range = document.createRange();
      range.selectNodeContents(firstLine);
      const selection = window.getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
      firstLine.dispatchEvent(new MouseEvent('mouseup', { bubbles: true }));
      await selectionChanged();
      const selectedLineCount = state.pendingSelection ? 1 : 0;
      const annotation = await createHighlight();
      elements['note-source'].value = 'Renderer **autosaved** note';
      elements['note-source'].dispatchEvent(new Event('input', { bubbles: true }));
      await new Promise((resolve) => setTimeout(resolve, noteAutosaveDelayMs + 100));
      if (state.noteSavingPromise) await state.noteSavingPromise;
      const note = selectedNote();
      await api.reportSmokeResult({
        status: 'ok',
        phase: config.phase,
        stage: 'content',
        documentCount: state.documents.length,
        annotationCount: annotation ? 1 : 0,
        selectedLineCount,
        noteRevision: note?.revision,
        noteMarkdown: note?.markdownSource,
        uiContract: inspectUiContract(),
        tileContract: inspectTilePixels(),
      });
      let switchFailureCode = null;
      try {
        await openWorkspaceAt(config.workspacePath, true);
      } catch (error) {
        switchFailureCode = error?.code || null;
      }
      const switchStateCleared = state.workspace === null && state.document === null
        && state.documents.length === 0 && state.pageViews.size === 0;
      await api.reportSmokeResult({
        status: 'ok', phase: config.phase, stage: 'closed', closed: true,
        switchFailureCode, switchStateCleared,
      });
      return;
    }

    if (config.phase === 'recovery') {
      await openWorkspaceAt(config.workspacePath, false);
      await openDocument(state.documents[0]);
      const verification = await api.verifyWorkspace();
      const note = state.notes[0];
      if (state.annotations[0]) await selectAnnotation(state.annotations[0].id);
      await api.reportSmokeResult({
        status: 'ok',
        phase: config.phase,
        stage: 'recovered',
        documentCount: state.documents.length,
        annotationCount: state.annotations.length,
        noteCount: state.notes.length,
        noteRevision: note?.revision,
        noteMarkdown: note?.markdownSource,
        workspaceValid: verification.valid,
      });
      return;
    }
    throw new Error(`Unknown renderer smoke phase: ${config.phase}`);
  } catch (error) {
    try {
      await api.reportSmokeResult({
        status: 'error', phase: config.phase, message: errorMessage(error),
      });
    } catch {}
  }
}

updateControls();
runSmoke();
