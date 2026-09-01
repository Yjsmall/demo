export {};

type HighlightColor = 'yellow' | 'green' | 'blue' | 'pink';

interface PageRect {
  x: number;
  y: number;
  width: number;
  height: number;
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
  extractPageText(pageIndex: number): Promise<{
    text: string;
    lines: Array<{ text: string; bounds: PageRect; vertical: boolean }>;
  }>;
  createAnnotation(command: {
    documentVersionId: string;
    pageIndex: number;
    quads: PageRect[];
    quote: { exact: string; prefix: string; suffix: string };
    layoutVersion: string;
    color: HighlightColor;
  }): Promise<AnnotationRecord>;
  listAnnotations(documentVersionId: string): Promise<AnnotationRecord[]>;
  deleteAnnotation(annotationId: string): Promise<void>;
  updateNote(command: {
    annotationId: string;
    expectedRevision: number;
    markdownSource: string;
  }): Promise<NoteRecord>;
  listNotes(documentVersionId: string): Promise<NoteRecord[]>;
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
