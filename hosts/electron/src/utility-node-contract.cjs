const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

function send(message) {
  process.parentPort.postMessage(message);
}

async function expectCode(action, expectedCode) {
  try {
    await action();
  } catch (error) {
    if (error?.code === expectedCode) return expectedCode;
    throw new Error(`Expected ${expectedCode}, received ${error?.code}: ${error?.message}`);
  }
  throw new Error(`Expected ${expectedCode}, but the operation succeeded`);
}

async function run() {
  const addonPath = path.resolve(process.argv[2]);
  const fixturePath = path.resolve(process.argv[3]);
  const readerNode = require(addonPath);
  const temporaryRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'context-reader-node-contract-'));
  const workspacePath = path.join(temporaryRoot, 'workspace');
  let contractResult = null;

  try {
    const runtime = readerNode.runtimeInfo();
    const parameterCodes = [];
    parameterCodes.push(await expectCode(() => readerNode.createWorkspace(), 'INVALID_ARGUMENT'));
    parameterCodes.push(await expectCode(() => readerNode.createWorkspace(42), 'INVALID_ARGUMENT'));
    parameterCodes.push(await expectCode(() => readerNode.openDocument('bad-id'), 'INVALID_ARGUMENT'));
    parameterCodes.push(await expectCode(() => readerNode.renderPage(-1, 1), 'INVALID_ARGUMENT'));
    parameterCodes.push(await expectCode(() => readerNode.renderPage(0, Number.NaN), 'INVALID_ARGUMENT'));
    parameterCodes.push(await expectCode(() => readerNode.cancelJob(''), 'INVALID_ARGUMENT'));

    const unopenedCode = await expectCode(() => readerNode.listDocuments(), 'NOT_FOUND');
    const workspace = await readerNode.createWorkspace(workspacePath);
    const workspaceConflict = await expectCode(
      () => readerNode.openWorkspace(workspacePath),
      'CONFLICT',
    );
    const imported = await readerNode.importDocument(fixturePath, 'contract-import');
    const documents = await readerNode.listDocuments();
    await readerNode.openDocument(imported.document.documentId, 'contract-open');

    const firstRender = await readerNode.renderPage(0, 1, 'contract-render');
    if (!Buffer.isBuffer(firstRender.png) || firstRender.png.length <= 8
        || firstRender.png.subarray(0, 8).toString('hex') !== '89504e470d0a1a0a') {
      throw new Error('renderPage did not return an owned PNG Buffer');
    }
    firstRender.png[0] = 0;
    const secondRender = await readerNode.renderPage(0, 1, 'contract-render-copy');
    if (secondRender.png[0] !== 0x89) {
      throw new Error('renderPage reused mutable Buffer storage');
    }
    const page = await readerNode.pageInfo(0);
    const tile = await readerNode.renderTile({
      pageIndex: 0,
      pixelsPerPoint: 1,
      xPixels: 0,
      yPixels: 0,
      widthPixels: Math.min(128, Math.floor(page.heightPoints)),
      heightPixels: Math.min(128, Math.floor(page.widthPoints)),
      generation: 7,
    }, 'contract-tile');
    if (!(tile.rgba instanceof ArrayBuffer) || tile.rgba.byteLength !== tile.widthPixels * tile.heightPixels * 4
        || tile.generation !== 7) {
      throw new Error('renderTile did not return an owned generation-bound RGBA ArrayBuffer');
    }
    const layout = await readerNode.pageTextLayout(0);
    if (!layout.units.length || !layout.lines.length) throw new Error('pageTextLayout returned no selection units');
    const firstUnit = layout.units[0];
    const selection = await readerNode.selectText(0, firstUnit.quad.upperLeft, firstUnit.quad.lowerRight);
    if (!selection.text || selection.logicalEnd <= selection.logicalStart || !selection.quads.length) {
      throw new Error('selectText returned an invalid character anchor');
    }

    const activeRender = readerNode.renderPage(0, 16, 'contract-duplicate-job');
    let admissionCode = null;
    try {
      readerNode.renderTile({
        pageIndex: 0,
        pixelsPerPoint: 1,
        xPixels: 0,
        yPixels: 0,
        widthPixels: 128,
        heightPixels: 128,
        generation: 8,
      }, 'contract-admission');
    } catch (error) {
      admissionCode = error?.code;
    }
    if (admissionCode !== 'RESOURCE_EXHAUSTED') {
      throw new Error(`Expected RESOURCE_EXHAUSTED admission result, received ${admissionCode}`);
    }
    const duplicateJobCode = await expectCode(
      () => readerNode.renderPage(0, 1, 'contract-duplicate-job'),
      'CONFLICT',
    );
    const cancellationAccepted = readerNode.cancelJob('contract-duplicate-job');
    const cancellationCode = await expectCode(() => activeRender, 'CANCELLED');
    const unknownCancellation = readerNode.cancelJob('missing-job');

    const annotation = await readerNode.createAnnotation({
      documentVersionId: imported.document.versionId,
      pageIndex: 0,
      quads: [{ x: 72, y: 96, width: 180, height: 18 }],
      quote: { exact: 'Context Reader P1', prefix: '', suffix: ' fixture' },
      layoutVersion: 'mupdf-1.28.3',
      color: 'yellow',
    });
    const note = await readerNode.updateNote({
      annotationId: annotation.id,
      expectedRevision: 0,
      markdownSource: 'Context note',
    });
    await readerNode.rebuildSearchIndex('contract-search-index');
    const search = await readerNode.search({ query: 'Context', limit: 20 });
    if (search.indexStatus !== 'ready' || !search.results.some((entry) => entry.kind === 'pdfPage')
        || !search.results.some((entry) => entry.kind === 'note')) {
      throw new Error('search did not return combined PDF and note results');
    }
    const revisionConflict = await expectCode(
      () => readerNode.updateNote({
        annotationId: annotation.id,
        expectedRevision: 0,
        markdownSource: 'stale note',
      }),
      'CONFLICT',
    );
    const annotations = await readerNode.listAnnotations(imported.document.versionId);
    const notes = await readerNode.listNotes(imported.document.versionId);
    const verification = await readerNode.verifyWorkspace();

    await readerNode.closeDocument();
    const closedDocumentCode = await expectCode(() => readerNode.pageInfo(0), 'NOT_FOUND');
    await readerNode.closeWorkspace();
    const closedWorkspaceCode = await expectCode(() => readerNode.listDocuments(), 'NOT_FOUND');

    contractResult = {
      status: 'ok',
      runtimeApiVersion: runtime.applicationApiVersion,
      parameterCheckCount: parameterCodes.length,
      unopenedCode,
      workspaceSchemaVersion: workspace.schemaVersion,
      workspaceConflict,
      documentCount: documents.length,
      duplicateJobCode,
      admissionCode,
      cancellationAccepted,
      cancellationCode,
      unknownCancellation,
      pngByteLength: secondRender.png.length,
      pngValid: secondRender.png.subarray(0, 8).toString('hex') === '89504e470d0a1a0a',
      tileValid: tile.rgba.byteLength === tile.widthPixels * tile.heightPixels * 4,
      selectionValid: selection.quads.length > 0,
      searchValid: search.indexStatus === 'ready' && search.results.length >= 2,
      revisionConflict,
      annotationCount: annotations.length,
      noteCount: notes.length,
      noteRevision: note.revision,
      workspaceValid: verification.valid,
      closedDocumentCode,
      closedWorkspaceCode,
    };
  } finally {
    try { await readerNode.closeDocument(); } catch {}
    try { await readerNode.closeWorkspace(); } catch {}
    await fs.rm(temporaryRoot, { recursive: true, force: true, maxRetries: 20, retryDelay: 100 });
  }
  send(contractResult);
}

run().catch((error) => {
  send({ status: 'error', message: error instanceof Error ? error.stack : String(error) });
});
