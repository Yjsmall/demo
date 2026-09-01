export {};

type HighlightColor = 'yellow' | 'green' | 'blue' | 'pink';

interface PageRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

interface PagePoint { x: number; y: number; }
interface PageQuad {
  upperLeft: PagePoint;
  upperRight: PagePoint;
  lowerLeft: PagePoint;
  lowerRight: PagePoint;
}

interface DocumentRecord {
  documentId: string;
  versionId: string;
  title: string;
  pageCount: number;
}

interface AnnotationRecord {
  id: string;
  documentVersionId: string;
  pageIndex: number;
  quads: PageRect[];
  quote: { exact: string; prefix: string; suffix: string };
  layoutVersion: string;
  color: HighlightColor;
  anchorVersion: 1 | 2;
  textStart?: number;
  textEnd?: number;
  direction: 'ltr' | 'rtl' | 'ttb';
}

interface NoteRecord {
  id: string;
  annotationId: string;
  markdownSource: string;
  revision: number;
}

interface ReaderJob<T> {
  readonly id: string;
  readonly result: Promise<T>;
  cancel(): void;
}

interface ContextReaderApi {
  chooseWorkspace(mode: 'create' | 'open'): Promise<string | null>;
  choosePdf(): Promise<string | null>;
  chooseNoteAsset(): Promise<string | null>;
  chooseBackupExport(): Promise<string | null>;
  chooseBackupOpen(): Promise<string | null>;
  chooseRestoreTarget(): Promise<string | null>;
  chooseDiagnosticsExport(): Promise<string | null>;
  createWorkspace(path: string): Promise<{ id: string; schemaVersion: number }>;
  openWorkspace(path: string): Promise<{ id: string; schemaVersion: number }>;
  closeWorkspace(): Promise<void>;
  importDocument(path: string): ReaderJob<{ document: DocumentRecord; reusedExisting: boolean }>;
  listDocuments(): Promise<DocumentRecord[]>;
  openDocument(documentId: string): ReaderJob<DocumentRecord>;
  closeDocument(): Promise<void>;
  pageInfo(pageIndex: number): Promise<{
    index: number;
    widthPoints: number;
    heightPoints: number;
    rotation: 0 | 90 | 180 | 270;
  }>;
  renderPage(pageIndex: number, pixelsPerPoint: number): ReaderJob<{
    widthPixels: number;
    heightPixels: number;
    pixelsPerPoint: number;
    png: Uint8Array;
  }>;
  renderTile(request: {
    pageIndex: number;
    pixelsPerPoint: number;
    xPixels: number;
    yPixels: number;
    widthPixels: number;
    heightPixels: number;
    generation: number;
  }): ReaderJob<{
    pageIndex: number;
    xPixels: number;
    yPixels: number;
    widthPixels: number;
    heightPixels: number;
    pixelsPerPoint: number;
    generation: number;
    rgba: ArrayBuffer;
  }>;
  extractPageText(pageIndex: number): Promise<{
    text: string;
    lines: Array<{ text: string; bounds: PageRect; vertical: boolean }>;
  }>;
  pageTextLayout(pageIndex: number): Promise<{
    pageIndex: number;
    layoutVersion: number;
    text: string;
    units: Array<{
      logicalStart: number;
      logicalEnd: number;
      text: string;
      direction: 'ltr' | 'rtl' | 'ttb';
      lineIndex: number;
      quad: PageQuad;
    }>;
    lines: Array<{ text: string; bounds: PageRect; vertical: boolean }>;
  }>;
  selectText(pageIndex: number, startPoint: PagePoint, endPoint: PagePoint): Promise<{
    pageIndex: number;
    layoutVersion: number;
    logicalStart: number;
    logicalEnd: number;
    direction: 'ltr' | 'rtl' | 'ttb';
    text: string;
    quads: PageQuad[];
    quote: { exact: string; prefix: string; suffix: string };
  }>;
  createAnnotation(command: {
    documentVersionId: string;
    pageIndex: number;
    quads: PageRect[];
    quote: { exact: string; prefix: string; suffix: string };
    layoutVersion: string;
    color: HighlightColor;
    anchorVersion?: 2;
    textStart?: number;
    textEnd?: number;
    direction?: 'ltr' | 'rtl' | 'ttb';
  }): Promise<AnnotationRecord>;
  listAnnotations(documentVersionId: string): Promise<AnnotationRecord[]>;
  deleteAnnotation(annotationId: string): Promise<void>;
  updateNote(command: {
    annotationId: string;
    expectedRevision: number;
    markdownSource: string;
  }): Promise<NoteRecord>;
  listNotes(documentVersionId: string): Promise<NoteRecord[]>;
  rebuildSearchIndex(): ReaderJob<void>;
  search(request: { query: string; limit: number }): Promise<{
    indexStatus: 'not_built' | 'building' | 'ready';
    results: Array<{
      kind: 'pdfPage' | 'note';
      documentVersionId: string;
      noteId?: string;
      pageIndex?: number;
      title: string;
      excerpt: string;
    }>;
  }>;
  importNoteAsset(annotationId: string, sourcePath: string): ReaderJob<{
    id: string;
    contentSha256: string;
    mediaType: 'image/png' | 'image/jpeg';
    byteLength: number;
    width: number;
    height: number;
  }>;
  readAsset(assetId: string): Promise<{
    asset: { id: string; mediaType: 'image/png' | 'image/jpeg'; width: number; height: number };
    bytes: ArrayBuffer;
  }>;
  exportWorkspace(destination: string): ReaderJob<{
    valid: boolean; formatVersion: 1; fileCount: number; totalUncompressedBytes: number; issues: string[];
  }>;
  inspectBackup(packagePath: string): Promise<{
    valid: boolean; formatVersion: 1; fileCount: number; totalUncompressedBytes: number; issues: string[];
  }>;
  restoreWorkspace(packagePath: string, emptyTarget: string): ReaderJob<{ id: string; schemaVersion: 4 }>;
  exportDiagnostics(destination: string): ReaderJob<{ version: 1 }>;
  verifyWorkspace(): Promise<{ valid: boolean; issues: string[] }>;
  smokeConfig(): Promise<{
    workspacePath: string;
    fixturePath: string;
    phase: 'setup' | 'recovery';
  } | null>;
  reportSmokeResult(result: unknown): Promise<unknown>;
}

declare global {
  interface Window {
    contextReader: ContextReaderApi;
  }
}
