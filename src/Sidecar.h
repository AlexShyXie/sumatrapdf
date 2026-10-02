/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// JSON sidecar persistence for annotations ("SeparateSave" mode).
//
// When SumatraPDF-settings.txt contains
//
//   Annotations [
//     SeparateSave = true
//     CentralFolder = D:\notes\sidecar   (optional)
//   ]
//
// annotations of a PDF are persisted as a JSON file next to the PDF
// (<pdf name>.json, "sibling") or, when no sibling file exists, inside
// <CentralFolder>/<pdf parent folder name>/<pdf name without ext>.json
// ("central"). The PDF itself is never modified.
//
// Loading order mirrors the companion PDF-XChange scripts: sibling first,
// then central. Saving resolves the target the same way, so both stay
// consistent without extra per-tab state.
//
// With Annotations->SeparateSaveAsMd the same data is stored as Markdown:
// annotations become Obsidian-style callouts and everything else in the
// file is preserved verbatim for the user's own notes. Import prefers the
// .md file and falls back to .json (a legacy .json is never rewritten);
// see the "Markdown sidecar" section in Sidecar.cpp for the format.
//
// Schema (version 2), one file per document:
//
//   {
//     "version": 2,
//     "title": "<pdf Info/Title, optional>",
//     "annotations": [ { ... }, ... ]
//   }
//
// Common annotation fields (all optional except type/page/rect):
//   type         text|freetext|line|square|circle|polygon|polyline|
//                highlight|underline|squiggly|strikeout|caret|ink|redact|
//                stamp|fileattachment
//   page         0-based page number
//   rect         [x0,y0,x1,y1]  PDF user space, like mupdf
//   quads        [[ulX,ulY,urX,urY,llX,llY,lrX,lrY], ...]  markup+redact
//   start/end    [x,y]           line endpoints
//   vertices     [[x,y], ...]    polygon / polyline
//   inkList      [[[x,y], ...], ...]  ink strokes
//   color / interiorColor / textColor   [r,g,b]  0-255
//   opacity      0..1 (omitted = 1)
//   borderWidth  points
//   lineStart/lineEnd  line ending style names (Square, Circle, ...)
//   icon         Text note icon name; stamp name ("Approved"...) for
//                stamps; icon name ("PushPin"...) for file attachments
//   isOpen       Text note popup state
//   fontSize / textColor / textAlign  FreeText default appearance (/DA)
//   fontFamily / textStyle   FreeText rich text (/DS CSS): family name and
//                style bits 1=bold 2=italic 4=underline (see kFreeText*)
//   author / subject / contents / name(/NM) / text(excerpt under quads)
//   creationDate / modDate   ISO-8601 UTC ("2026-09-28T12:00:00Z")
//   flags        PDF annotation flags bits (print|nozoom|...)
//
// Stamp (image) and FileAttachment annotations carry binary payloads.
// The JSON never holds binary data: payloads are written as
// <json dir>/assets/<fnv1a-64-hex>.<ext> (content-addressed, deduped) and
// referenced by "asset" ("assets/<hex>.<ext>", relative to the JSON, so
// pdf+json+assets move together). Attachment metadata lives in an
// "attachment" object (filename/mimeType/size/created/modified). On save
// asset files that no sidecar in the folder references anymore are
// deleted. Version 1 files (no asset fields) still parse.
//
// Why JSON and not XFDF: XFDF interop between viewers is de-facto, not
// de-jure (quad ordering differs between implementations, richtext
// contents are non-uniform, PDF-XChange itself loses/garbles some fields
// on re-import). JSON keeps the format under our control while the
// migration story stays "save back into the PDF" (disable SeparateSave,
// reopen, save normally).
//
// All logic lives in Sidecar.cpp. Integration points in existing files are
// marked with "// SIDECAR:" so the change stays easy to re-apply after
// pulling newer upstream code.

class EngineBase; // must match EngineBase.h ("class"), else C4099
struct WindowTab;

// EngineMupdf.cpp is compiled into several standalone targets (PdfFilter,
// PdfPreview) that do NOT link Sidecar.cpp. The annotation-changed
// notification therefore goes through a hook that the application installs
// (lazily, from SidecarMaybeImport/SidecarSaveTab); without an installed hook
// the notification is a no-op. Both functions are implemented in
// EngineMupdf.cpp, so every target that pulls in EngineMupdf.obj resolves
// them.
void SidecarSetAnnotsChangedHook(void (*fn)(EngineBase* engine));
void SidecarNotifyAnnotsChanged(EngineBase* engine);

enum class SidecarResult {
    NotHandled, // feature off or engine is not a mupdf PDF: caller uses the normal path
    Saved,      // JSON written (or nothing to write)
    Failed,     // writing failed (error already reported to the user)
};

// true if gSettings->annotations.separateSave is set
bool SidecarSeparateSaveEnabled();

// separateSave is on and engine is a mupdf PDF engine
bool SidecarWantsRedirect(EngineBase* engine);

// called right after a document engine has been created and before it is
// displayed: imports the JSON sidecar into the engine (sibling first, then
// central). Never marks the document as modified.
void SidecarMaybeImport(EngineBase* engine);

// write the JSON sidecar for the tab's document; also clears the "modified"
// state on success (like the PDF annotation save does). With allowCreate
// false, an existing sidecar file is updated but no new file is created
// (used by the debounced auto-save).
SidecarResult SidecarSaveTab(WindowTab* tab, bool allowCreate = true);

// annotations changed: (re)arm the debounced auto-save timer; only does
// anything when a sidecar target was already established for the document
void SidecarNotifyChanged(EngineBase* engine);
