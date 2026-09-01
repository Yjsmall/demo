# Context Reader v0.1 internal release

This Windows x64 internal build adds virtualized continuous PDF reading,
generation-safe RGBA tiles, precise text selection, highlights, Markdown and
KaTeX notes, content-addressed PNG/JPEG attachments, unified PDF/note search,
workspace backup/restore, redacted diagnostics, and Squirrel installation.

Known limitations:

- The installer is unsigned and may trigger SmartScreen.
- There is no automatic update service.
- `readerpkg` backups are not encrypted and restore only into an empty folder.
- Note attachments accept PNG and JPEG only; OCR is not included.
- This development release accepts workspace schema v4 only and does not migrate
  older workspaces.
- The internal packaged executable remains named `electron.exe` because the
  MinGW N-API import boundary binds to that module name. Product and installer
  branding remain Context Reader.
