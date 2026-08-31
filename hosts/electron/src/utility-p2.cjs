const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');

function send(message) {
  process.parentPort.postMessage(message);
}

async function run() {
  const addonPath = path.resolve(process.argv[2]);
  const fixturePath = path.resolve(process.argv[3]);
  const temporaryRoot = await fs.mkdtemp(path.join(os.tmpdir(), 'context-reader-p2-'));
  const workspacePath = path.join(temporaryRoot, 'workspace-unicode-\u9605\u8bfb');

  try {
    const readerNode = require(addonPath);
    const runtimeInfo = readerNode.runtimeInfo();
    const workspace = await readerNode.createWorkspace(workspacePath);
    const imported = await readerNode.importDocument(fixturePath);
    const duplicate = await readerNode.importDocument(fixturePath);

    if (!duplicate.reusedExisting || duplicate.document.documentId !== imported.document.documentId) {
      throw new Error('Duplicate import did not reuse the existing document');
    }

    await readerNode.openDocument(imported.document.documentId);
    await readerNode.closeDocument();
    await readerNode.closeWorkspace();
    const reopened = await readerNode.openWorkspace(workspacePath);
    const documents = await readerNode.listDocuments();
    await readerNode.openDocument(documents[0].documentId);
    const page = await readerNode.pageInfo(0);
    const rendered = await readerNode.renderPage(0, 1);
    const pageText = await readerNode.extractPageText(0);
    const annotation = await readerNode.createAnnotation({
      documentVersionId: documents[0].versionId,
      pageIndex: 0,
      quads: [pageText.lines[0].bounds],
      quote: {
        exact: pageText.lines[0].text,
        prefix: '',
        suffix: '',
      },
      layoutVersion: 'mupdf-1.28.3',
      color: 'yellow',
    });
    const initialNote = await readerNode.updateNote({
      annotationId: annotation.id,
      expectedRevision: 0,
      markdownSource: 'First **context** note',
    });
    let conflictCode = null;
    try {
      await readerNode.updateNote({
        annotationId: annotation.id,
        expectedRevision: 0,
        markdownSource: 'stale note',
      });
    } catch (error) {
      conflictCode = error?.code;
    }
    const updatedNote = await readerNode.updateNote({
      annotationId: annotation.id,
      expectedRevision: initialNote.revision,
      markdownSource: 'Restored **context** note',
    });
    await readerNode.closeDocument();
    await readerNode.closeWorkspace();
    await readerNode.openWorkspace(workspacePath);
    const annotations = await readerNode.listAnnotations(documents[0].versionId);
    const notes = await readerNode.listNotes(documents[0].versionId);
    const verification = await readerNode.verifyWorkspace();
    await readerNode.closeWorkspace();

    const expectedPngSignature = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
    if (!Buffer.isBuffer(rendered.png) || !rendered.png.subarray(0, 8).equals(expectedPngSignature)) {
      throw new Error('Rendered page did not return a PNG Buffer');
    }

    send({
      status: 'ok',
      processType: process.type,
      runtimeInfo,
      workspaceId: workspace.id,
      reopenedWorkspaceId: reopened.id,
      documentCount: documents.length,
      contentSha256: documents[0]?.contentSha256,
      page,
      rendered: {
        widthPixels: rendered.widthPixels,
        heightPixels: rendered.heightPixels,
        pixelsPerPoint: rendered.pixelsPerPoint,
        byteLength: rendered.png.length,
      },
      pageText: {
        text: pageText.text,
        lineCount: pageText.lines.length,
        firstLine: pageText.lines[0],
      },
      annotationCount: annotations.length,
      annotation: annotations[0],
      noteCount: notes.length,
      note: notes[0],
      updatedNoteRevision: updatedNote.revision,
      conflictCode,
      verification,
    });
  } finally {
    await fs.rm(temporaryRoot, { recursive: true, force: true });
  }
}

run().catch((error) => {
  send({
    status: 'error',
    processType: process.type,
    message: error instanceof Error ? error.stack : String(error),
  });
});
