[![Build](https://github.com/sumatrapdfreader/sumatrapdf/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/sumatrapdfreader/sumatrapdf/actions/workflows/build.yml)
## SumatraPDF Reader with sidecar

SumatraPDF is a multi-format (PDF, EPUB, MOBI, CBZ, CBR, FB2, CHM, XPS, DjVu) reader
for Windows under (A)GPLv3 license, with some code under BSD license (see
AUTHORS).

More Information:
* [Website](https://www.sumatrapdfreader.org/free-pdf-reader)
* [Manual](https://www.sumatrapdfreader.org/manual)
* [Developer Information](https://www.sumatrapdfreader.org/docs/Contribute-to-SumatraPDF)



[简体中文](read-zh.md) | English

A modified version of SumatraPDF: annotations are not written into the PDF, but saved to a JSON file stored alongside it.

Upstream repository: [sumatrapdfreader/SumatraPDF](https://github.com/sumatrapdfreader/SumatraPDF). This repository is forked from the official master, with changes concentrated in `src/Sidecar.cpp` (a new file) and 6 mounting points marked with `// SIDECAR:` comments.

A PDF annotation sidecar (companion file) means annotation data is not written into the PDF itself, but saved as a separate file with the same name in the same directory (e.g., `book.pdf` → `book.json`). When opening the PDF, the sidecar file is loaded automatically; when editing annotations, only this small (a few KB) file is rewritten, leaving the PDF untouched — especially useful for large PDFs stored on sync services like OneDrive, since a single annotation edit won’t trigger a full re-upload of a 600 MB file. The trade-off: other PDF readers won’t show the annotations, because the data isn’t inside the PDF.



## Why

My PDF library contains several single-file scanned documents of hundreds of megabytes, synced via OneDrive. Once SumatraPDF's annotation feature is enabled, every stroke rewrites the entire PDF — for a 600MB file, changing a single byte means OneDrive has to re-upload the whole thing. Turning off annotations, on the other hand, means giving up half the value of a reader.

The official codebase of the PDF implementation was removed a few years ago. This repository re-implements it the PDF way, storing JSON.

The result: opening a PDF with annotations leaves the modification time of the PDF file itself untouched. All annotation data lives in the adjacent `.json` file.

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

```ini
Annotations [
    ....
	SeparateSave = true
	CentralFolder = E:\Downloads\Claw
]
```



## Supported Annotation Types

14 types, with full round-trip property support:

Text (sticky note), FreeText (text box), Highlight, Underline, Squiggly, StrikeOut, Line, Square, Circle, Polygon, PolyLine, Ink, Caret, Redact

FreeText supports font, font size, text color, alignment, bold, italic, underline, and transparent background. For all types, colors (including none/transparent), opacity, borders, author, and timestamps are preserved.

## Known Limitations

- **Stamp and FileAttachment are not supported.** Their data consists of embedded images and files, which are not placed in the JSON as binaries. When saving, these two types of annotations are skipped and counted; the status bar will indicate how many were skipped.
- **No conflict merging.** If two machines modify the same sidecar simultaneously, the last save overwrites the earlier one. Fine for single-user use.
- Large annotations have defensive limits: 512 vertices for polygons/polylines, 64 strokes for ink, with at most 2048 points per stroke. Anything beyond these limits is truncated.
- Other readers won't see the annotations when opening this PDF — the data lives in the JSON, not in the PDF. This is by design.

## License

GPLv3, same as upstream.
