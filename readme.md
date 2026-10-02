[![Build](https://github.com/AlexShyXie/sumatrapdf-sidecar/actions/workflows/build.yml/badge.svg?branch=add_sidecar)](https://github.com/sumatrapdfreader/sumatrapdf/actions/workflows/build.yml)

## SumatraPDF Reader with sidecar

SumatraPDF is a multi-format (PDF, EPUB, MOBI, CBZ, CBR, FB2, CHM, XPS, DjVu) reader
for Windows under (A)GPLv3 license, with some code under BSD license (see
AUTHORS).

More Information:
* [Website](https://www.sumatrapdfreader.org/free-pdf-reader)
* [Manual](https://www.sumatrapdfreader.org/manual)
* [Developer Information](https://www.sumatrapdfreader.org/docs/Contribute-to-SumatraPDF)



[简体中文](readme-zh.md) | English

A modified version of SumatraPDF: annotations are not written into the PDF, but saved to a JSON file stored alongside it.

Upstream repository: [sumatrapdfreader/SumatraPDF](https://github.com/sumatrapdfreader/SumatraPDF). This repository is forked from the official master, with changes concentrated in `src/Sidecar.cpp` (a new file) and 6 mounting points marked with `// SIDECAR:` comments.

A PDF annotation sidecar (companion file) means annotation data is not written into the PDF itself, but saved as a separate file with the same name in the same directory (e.g., `book.pdf` → `book.json`). When opening the PDF, the sidecar file is loaded automatically; when editing annotations, only this small (a few KB) file is rewritten, leaving the PDF untouched — especially useful for large PDFs stored on sync services like OneDrive, since a single annotation edit won’t trigger a full re-upload of a 600 MB file. The trade-off: other PDF readers won’t show the annotations, because the data isn’t inside the PDF.

## Why

> SumatraPDF originally supported saving annotations as pure text files with the same name as the PDF, with the last supported version being 3.2. 
> Okular, in its early stages, saved annotations as hidden XML files, with the last supported version being 1.2. Both companies actively removed this feature around 2018 and explicitly stated that it would not be restored. 
> The core reasons for abandoning this feature were threefold: the sidecar file easily becomes disconnected after renaming or saving the file under a different name, causing a perceived "data loss"; the pure text format could not support complex types of annotations; and the maintenance cost was high due to the lack of unified coordinate units across different formats. The maturity of embedded annotations in the standard PDF ultimately led to the complete discontinuation of this independent storage approach.

The industry consensus is that annotations should follow the file, but in some scenarios, this is precisely a disaster: my PDF library contains hundreds of megabytes of single-file scanned documents, synchronized on OneDrive. Once the annotation feature of SumatraPDF is enabled, modifying the entire PDF every time a line is drawn - a 600MB file, with OneDrive needing to re-upload the entire file even for a single byte change; disabling annotations is equivalent to giving up half the value of the reader.
Therefore, I prefer the sidecar approach: when opening a PDF with annotations, the modification time of the PDF file itself is zero. All annotation data is stored in a separate `.json` file.

## How It Works

- **Import**: When opening a PDF, if a `.json` file with the same name exists in the same directory (e.g., `book.pdf` → `book.json`), annotations are loaded automatically without touching the PDF itself.
- **Auto-save**: After annotation changes, a 2-second debounce writes to disk — JSON only.
- **Ctrl+S**: With sidecar enabled, Ctrl+S saves the JSON and no longer pops up "Save As copy".
- **Central folder mode** (optional): After setting `Annotations.centralFolder`, all sidecars are stored in one place, named as `parentFolderName/fileName.json`. Suitable for libraries spread across multiple folders, or when you want to sync your annotation library separately.

Settings (AdvancedSettings):

| Setting                     | Default | Description                               |
| --------------------------- | ------- | ----------------------------------------- |
| `Annotations.separateSave`  | false   | Enable the sidecar feature                |
| `Annotations.centralFolder` | (empty) | Central directory for storing annotations |
| `Annotations.separateSaveAsMd` | false | Save sidecar as Markdown instead of JSON (**experimental**, see below) |

```ini
Annotations [
    ....
	SeparateSave = true
	CentralFolder = E:\Downloads\Claw
	SeparateSaveAsMd = true
]
```



## Supported Annotation Types

16 types, with full round-trip property support:

Text (sticky note), FreeText (text box), Highlight, Underline, Squiggly, StrikeOut, Line, Square, Circle, Polygon, PolyLine, Ink, Caret, Redact, Stamp, FileAttachment

FreeText supports font, font size, text color, alignment, bold, italic, underline, and transparent background. For all types, colors (including none/transparent), opacity, borders, author, and timestamps are preserved.

Stamp and FileAttachment carry binary payloads (images, embedded files), which the JSON never holds: payloads are written to an `assets/` folder next to the JSON, named by content hash (`assets/<hash>.<ext>`) and referenced by relative path. Identical payloads are deduplicated, and asset files no longer referenced by any sidecar in the folder are deleted on save. Sidecar files from the previous format (no asset fields) still load.

## Known Limitations

- **No conflict merging.** If two machines modify the same sidecar simultaneously, the last save overwrites the earlier one. Fine for single-user use.
- Large annotations have defensive limits: 512 vertices for polygons/polylines, 64 strokes for ink, with at most 2048 points per stroke. Anything beyond these limits is truncated.
- Other readers won't see the annotations when opening this PDF — the data lives in the JSON, not in the PDF. This is by design.

## Markdown sidecar (experimental 🧪)
With `SeparateSaveAsMd = true`, annotations are saved as `book.md` instead of `book.json`: each annotation is an Obsidian callout, editable with any Markdown editor.
```markdown
---
sumatrapdf_sidecar: 2
generator: SumatraPDF-sidecar/2-md
file: book.pdf
---
# mybooknote
> [!Note]
> type: highlight
> page: 11
> rect: [58.4,695.2,299.6,708.4]
> quads: [[58.4,708.4,299.6,695.2,58.4,695.2,299.6,708.4]]
> text: highlighted words
> contents: highlighted words
> author: AlexShy
```
Rules:
- **Everything outside callouts is yours.** Headings, prose, ordinary quote blocks — SumatraPDF preserves them verbatim on read/write, touching only the machine lines in `key: value` form inside callouts. Annotation data and annotation notes now live in the same file.
- **The `contents` line is always present.** When there's no annotation note, it is filled with the highlighted text; when both are empty it's `contents: ""` — fill it in by hand in Obsidian, and it takes effect when the document is reopened.
- **Two-way sync, bounded by reopening.** Delete a callout after closing the document, and that annotation is gone on reopen; hand-write a valid callout (not recommended — coordinates are hard to compute precisely) and it becomes a real annotation on reopen. If you edit the md while the document is open, your changes will be overwritten by session data on save — close the document before editing externally.
- **Automatic migration.** When enabled, `.md` is read first, falling back to `.json` if absent; the next save writes `.md`, and the old `.json` is left untouched and no longer updated.
- **Atomic writes.** The md is mixed with your notes, so saving goes through a temp file + replace — a mid-save crash won't corrupt the file.
- **Querying**: front matter is YAML; `key:: value` double-colon syntax inside callouts is compatible with Dataview.
⚠️ **Experimental notice**: the format may still change (a version field is included to keep old files readable); validation in single-user scenarios is limited.

## License

GPLv3, same as upstream.
