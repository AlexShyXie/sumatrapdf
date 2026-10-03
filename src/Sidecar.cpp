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
// <CentralFolder>/<pdf parent folder name>/<pdf name w/o ext>.json
// ("central"). The PDF itself is never modified.
//
// Loading order mirrors the companion PDF-XChange scripts: sibling first,
// then central. Saving resolves the target the same way, so both stay
// consistent without extra per-tab state.
//
// Why JSON and not XFDF: XFDF interop between viewers is de-facto, not
// de-jure (quad ordering differs between implementations, richtext
// contents are non-uniform, PDF-XChange itself loses/garbles some fields
// on re-import). The sidecar is a SumatraPDF-private cache; the
// interoperability path is "save back into the PDF" (turn SeparateSave
// off, reopen, save when prompted), which writes standard PDF annotations
// every viewer understands.
//
// Schema (version 1) -- see also SerializeAnnotJson for the field order:
// {
//   "version": 1,
//   "generator": "SumatraPDF-sidecar/1",
//   "file": "report.pdf",             // base name of the PDF (identification)
//   "title": "…",                     // PDF metadata title, optional
//   "annotations": [
//     {
//       "type": "highlight",          // text freetext line square circle polygon
//                                      // polyline underline squiggly strikeout
//                                      // caret ink redact stamp fileattachment
//       "page": 29,                   // 0-based page number
//       "rect": [53.81,502.57,476.23,527.08],   // PDF user space [llx,lly,urx,ury]
//       "quads": [[ul.x,ul.y,ur.x,ur.y,ll.x,ll.y,lr.x,lr.y],…],  // text markups
//       "vertices": [[x,y],…],        // polygon / polyline
//       "start": [x,y], "end": [x,y], // line
//       "inkList": [[[x,y],…],…],     // ink: array of strokes
//       "color": [255,164,0],         // 0-255 RGB
//       "interiorColor": [255,237,153],
//       "textColor": [0,0,0],         // freetext
//       "opacity": 0.5,               // 0..1, default 1
//       "borderWidth": 2,
//       "lineStart": "OpenArrow",      // line ending styles
//       "lineEnd": "None",
//       "icon": "Comment",            // text annotation icon; stamp name
//                                      // ("Approved"...) for stamps, icon for
//                                      // file attachments
//       "asset": "assets/1a2b...png", // stamp/attachment payload, relative
//                                      // to this JSON (content-addressed)
//       "attachment": {               // file attachment metadata
//         "filename": "notes.txt", "mimeType": "text/plain",
//         "size": 1234,
//         "created": "…", "modified": "…",
//         "asset": "assets/9ab1...txt"
//       },
//       "isOpen": false,              // text annotation popup open
//       "fontSize": 12,               // freetext
//       "textAlign": 0,               // freetext: 0 left, 1 center, 2 right
//       "flags": 4,                   // PDF annotation flag bits
//       "author": "…", "subject": "…",
//       "name": "uuid",               // PDF /NM: stable identity for dedup
//       "text": "…",                  // excerpt of the text under the markup
//                                      // quads (display/indexing only, never
//                                      // written back into the PDF)
//       "contents": "…",              // note body
//       "creationDate": "2026-09-28T12:34:56Z",
//       "modDate": "2026-09-28T12:34:56Z"
//     }
//   ]
// }
//
// Unknown fields are ignored on read (forward compatibility). All fields
// except type/page/rect are optional. Coordinates are PDF user space
// (origin bottom-left, points), exactly as MuPDF reports them -- no
// axis flipping anywhere.
//
// All logic lives in Sidecar.cpp. Integration points in existing files are
// marked with "// SIDECAR:" comments (SumatraPDF.cpp x2, EngineMupdf.cpp x1,
// Settings.h fields, premake5.files.lua entry).

#include "base/Base.h"
#include "base/DirScan.h"
#include "base/File.h"
#include "base/GuessFileType.h"
#include "base/Win.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern "C" {
#include <mupdf/pdf.h>
}

#include "gui/UIModels.h"
#include "Settings.h"
#include "Annotation.h"
#include "AnnotEditToolbar.h"
#include "AnnotTextPopup.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "EngineMupdf.h"
#include "DisplayModel.h"
#include "Translations.h"
#include "SumatraPDF.h"
#include "MainWindow.h"
#include "WindowTab.h"
#include "Notifications.h"
#include "Commands.h"
#include "Toolbar.h"
#include "AppSettings.h"

#include "Sidecar.h"

// ---------------------------------------------------------------------------
// settings

bool SidecarSeparateSaveEnabled() {
    return gSettings && gSettings->annotations.separateSave;
}

// md mode: sidecar files are Markdown (.md) instead of JSON. The format
// implementation lives in the "Markdown sidecar" section below.
static bool SidecarMdEnabled() {
    return gSettings && gSettings->annotations.separateSaveAsMd;
}

bool SidecarWantsRedirect(EngineBase* engine) {
    if (!SidecarSeparateSaveEnabled()) {
        return false;
    }
    if (!engine || engine->kind != kindEngineMupdf) {
        return false;
    }
    EngineMupdf* e = AsEngineMupdf(engine);
    return e && e->pdfdoc != nullptr;
}

// ---------------------------------------------------------------------------
// path resolution (mirrors the PDF-XChange v5 scripts)
//
// sibling: <dir of pdf>/<pdf name>.json (".pdf" extension replaced)
// central: <CentralFolder>/<pdf parent folder name>/<pdf name w/o ext>.json

struct SidecarTarget {
    Str path;
    bool central = false;
    bool exists = false;
};

static char PathSepFor(Str dir) {
    if (len(dir) > 0 && dir.s[len(dir) - 1] == '/') {
        return '/';
    }
    return '\\';
}

// "D:\books\report.pdf" -> "D:\books\report.json" (or .md in md mode)
static SidecarTarget ResolveSiblingTargetExt(Str pdfPath, const char* ext) {
    SidecarTarget res;
    TempStr dir = path::GetDirTemp(pdfPath);
    TempStr base = path::GetBaseNameTemp(pdfPath);
    if (len(dir) == 0 || len(base) == 0) {
        return res;
    }
    Str name = Str(base);
    if (str::EndsWithI(name, StrL(".pdf"))) {
        name = Str(name.s, len(name) - 4);
    }
    str::Builder b;
    b.Append(Str(dir));
    char sep = PathSepFor(Str(dir));
    b.AppendChar(sep);
    b.Append(name);
    b.Append(Str(ext));
    res.path = b.TakeStr();
    res.central = false;
    res.exists = file::Exists(res.path);
    return res;
}

// "D:\books\sub\report.pdf" + "D:\notes" -> "D:\notes\sub\report.json" (or .md)
static SidecarTarget ResolveCentralTargetExt(Str pdfPath, const char* ext) {
    SidecarTarget res;
    Str folder = gSettings->annotations.centralFolder;
    if (len(folder) == 0) {
        return res;
    }
    TempStr dir = path::GetDirTemp(pdfPath);
    if (len(dir) == 0) {
        return res;
    }
    Str dirS = Str(dir);
    // parent folder name = last path segment of the pdf's directory
    int idx = str::LastIndexOfChar(dirS, '\\');
    int idxF = str::LastIndexOfChar(dirS, '/');
    if (idxF > idx) {
        idx = idxF;
    }
    if (idx < 0 || idx + 1 >= len(dirS)) {
        return res;
    }
    Str parentName = Str(dirS.s + idx + 1, len(dirS) - idx - 1);

    TempStr base = path::GetBaseNameTemp(pdfPath);
    if (len(base) == 0) {
        return res;
    }
    // name without extension (v5 scripts strip the last extension)
    Str name = Str(base);
    int dot = str::LastIndexOfChar(name, '.');
    if (dot > 0) {
        name = Str(name.s, dot);
    }

    str::Builder b;
    b.Append(folder);
    char sep = PathSepFor(folder);
    b.AppendChar(sep);
    b.Append(parentName);
    b.AppendChar(sep);
    b.Append(name);
    b.Append(Str(ext));
    res.path = b.TakeStr();
    res.central = true;
    res.exists = file::Exists(res.path);
    return res;
}

static SidecarTarget ResolveSiblingTarget(Str pdfPath) {
    return ResolveSiblingTargetExt(pdfPath, SidecarMdEnabled() ? ".md" : ".json");
}

static SidecarTarget ResolveCentralTarget(Str pdfPath) {
    return ResolveCentralTargetExt(pdfPath, SidecarMdEnabled() ? ".md" : ".json");
}

// order of preference: existing sibling wins, then existing central
// (both load and save stay consistent with what is already on disk).
// When CREATING a new sidecar, a configured CentralFolder is where it
// goes - the book folders stay clean and all notes live in one place
// (v5 semantics); without one the sibling file is the only option.
static SidecarTarget ResolveSidecarTarget(Str pdfPath, bool allowCreate) {
    SidecarTarget sib = ResolveSiblingTarget(pdfPath);
    SidecarTarget cen = ResolveCentralTarget(pdfPath);
    if (sib.exists) {
        return sib;
    }
    if (cen.exists) {
        return cen;
    }
    if (allowCreate) {
        if (len(cen.path) > 0) {
            return cen;
        }
        if (len(sib.path) > 0) {
            return sib;
        }
    }
    return SidecarTarget{};
}

// ---------------------------------------------------------------------------
// type mapping

// annotations whose /Rect is directly settable (mupdf's rect_subtypes
// whitelist intersection with the types we support). Everything else -
// text markups (QuadPoints), Line (endpoints), Polygon/PolyLine
// (Vertices), Ink (InkList) - has a derived /Rect and rejects
// pdf_set_annot_rect with "argument error: ... have no Rect property".
static bool SidecarAnnotOwnsRect(AnnotationType tp) {
    switch (tp) {
        case AnnotationType::Text:
        case AnnotationType::FreeText:
        case AnnotationType::Square:
        case AnnotationType::Circle:
        case AnnotationType::Redact:
        case AnnotationType::Caret:
        case AnnotationType::Stamp:
        case AnnotationType::FileAttachment:
            return true;
        default:
            return false;
    }
}

static const char* kLineEndingNames[] = {"None",        "Square", "Circle",     "Diamond",      "OpenArrow",
                                         "ClosedArrow", "Butt",   "ROpenArrow", "RClosedArrow", "Slash"};

struct SidecarAnnot {
    AnnotationType type = AnnotationType::Unknown;
    int pageNo = 0; // 1-based, 0 = invalid
    RectF bounds;
    Vec<fz_quad> quads;         // text markups, MuPDF native ul/ur/ll/lr corners
    float opacity = 1.0f;       // 0..1
    float color[3] = {0, 0, 0}; // 0..1 per channel
    bool hasColor = false;
    float interiorCol[3] = {0, 0, 0};
    bool hasInterior = false;
    Str contents;
    Str author;
    Str subject;
    Str name; // PDF /NM
    Str icon;
    bool isOpen = false; // text annotation popup
    int flags = -1;      // PDF annotation flag bits
    time_t creationDate = 0;
    time_t modDate = 0;
    // stamp / file attachment payload: the bytes live in an assets/ folder
    // next to the JSON ("assets/<fnv1a-64-hex>.<ext>", content-addressed so
    // identical payloads share one file); "asset" in the JSON is relative to
    // the JSON itself, so pdf+json+assets move together
    Str assetName;                 // "assets/1a2b3c....png", empty when no payload
    fz_buffer* assetBuf = nullptr; // owned payload bytes while in flight
    bool assetOk = true;           // false: payload expected but unextractable
    Str attachName;                // file attachment: original file name
    Str attachMime;                // file attachment: MIME type
    int attachSize = -1;           // file attachment: payload size in bytes
    time_t attachCreated = 0;
    time_t attachModified = 0;
    // free text extras
    int textSize = -1;
    float textColor[3] = {0, 0, 0};
    bool hasTextCol = false;
    int quadding = -1; // 0 left, 1 center, 2 right
    Str fontFamily;    // CSS font family from /DS (base-14 fallback when absent)
    int fontStyle = 0; // kFreeTextBold | kFreeTextItalic | kFreeTextUnderline
    int borderWidth = -1;
    // line
    bool hasLine = false;
    PointF lineA;
    PointF lineB;
    int lineStart = 0; // pdf_line_ending
    int lineEnd = 0;
    // polygon / polyline
    Vec<PointF> vertices;
    // ink
    Vec<Vec<PointF>> inkStrokes;
    // excerpt of the text under the markup quads (display only)
    Str text;
};

// defensive caps when reading external files (also bound the arrays we pass
// to mupdf; annotation data from the wild is untrusted)
static constexpr int kMaxQuads = 256;
static constexpr int kMaxVertices = 512;
static constexpr int kMaxInkPoints = 2048;
static constexpr int kMaxInkStrokes = 64;

// true while a sidecar is being imported; suppresses the auto-save arming
// that MarkNotificationAsModified would trigger for every created annot
static bool gSidecarImporting = false;

// SidecarAnnot string fields are heap-dups on both the collect and the read
// path; release them once the entry is no longer needed
static void FreeSidecarAnnotStrings(fz_context* ctx, SidecarAnnot& sa) {
    if (ctx && sa.assetBuf) {
        fz_drop_buffer(ctx, sa.assetBuf);
        sa.assetBuf = nullptr;
    }
    str::Free(sa.contents);
    str::Free(sa.author);
    str::Free(sa.subject);
    str::Free(sa.name);
    str::Free(sa.icon);
    str::Free(sa.text);
    str::Free(sa.fontFamily);
    str::Free(sa.assetName);
    str::Free(sa.attachName);
    str::Free(sa.attachMime);
    sa.contents = {};
    sa.author = {};
    sa.subject = {};
    sa.name = {};
    sa.icon = {};
    sa.text = {};
    sa.assetName = {};
    sa.attachName = {};
    sa.attachMime = {};
}

static bool SidecarTypeSupported(AnnotationType tp) {
    switch (tp) {
        case AnnotationType::Text:
        case AnnotationType::FreeText:
        case AnnotationType::Line:
        case AnnotationType::Square:
        case AnnotationType::Circle:
        case AnnotationType::Polygon:
        case AnnotationType::PolyLine:
        case AnnotationType::Highlight:
        case AnnotationType::Underline:
        case AnnotationType::Squiggly:
        case AnnotationType::StrikeOut:
        case AnnotationType::Caret:
        case AnnotationType::Ink:
        case AnnotationType::Redact:
        case AnnotationType::Stamp:
        case AnnotationType::FileAttachment:
            return true;
        default:
            // Link / Popup / Widget / Sound / Movie / RichMedia / media
            // types: not supported. Stamps and file attachments are
            // supported since schema version 2: their payloads live in an
            // assets/ folder next to the JSON, not in the JSON itself
            return false;
    }
}

static const char* SidecarNameForType(AnnotationType tp) {
    switch (tp) {
        case AnnotationType::Text:
            return "text";
        case AnnotationType::FreeText:
            return "freetext";
        case AnnotationType::Line:
            return "line";
        case AnnotationType::Square:
            return "square";
        case AnnotationType::Circle:
            return "circle";
        case AnnotationType::Polygon:
            return "polygon";
        case AnnotationType::PolyLine:
            return "polyline";
        case AnnotationType::Highlight:
            return "highlight";
        case AnnotationType::Underline:
            return "underline";
        case AnnotationType::Squiggly:
            return "squiggly";
        case AnnotationType::StrikeOut:
            return "strikeout";
        case AnnotationType::Caret:
            return "caret";
        case AnnotationType::Ink:
            return "ink";
        case AnnotationType::Redact:
            return "redact";
        case AnnotationType::Stamp:
            return "stamp";
        case AnnotationType::FileAttachment:
            return "fileattachment";
        default:
            return nullptr;
    }
}

static AnnotationType SidecarTypeFromName(const char* name) {
    for (int t = 0; t <= (int)AnnotationType::Last; t++) {
        AnnotationType tp = (AnnotationType)t;
        const char* n = SidecarNameForType(tp);
        if (n && str::EqI(Str(n), Str(name))) {
            return tp;
        }
    }
    return AnnotationType::Unknown;
}

static int LineEndingFromName(Str s) {
    for (int i = 0; i < dimofi(kLineEndingNames); i++) {
        if (str::EqI(Str(kLineEndingNames[i]), s)) {
            return i;
        }
    }
    return 0; // None
}

// ---------------------------------------------------------------------------
// JSON string escaping (writer side; parsing is done by MuPDF's fz_parse_json)

static void AppendEscapedJson(str::Builder& b, Str s) {
    b.AppendChar('"');
    for (int i = 0; i < len(s); i++) {
        char c = s.s[i];
        switch (c) {
            case '"':
                b.Append(StrL("\\\""));
                break;
            case '\\':
                b.Append(StrL("\\\\"));
                break;
            case '\n':
                b.Append(StrL("\\n"));
                break;
            case '\r':
                b.Append(StrL("\\r"));
                break;
            case '\t':
                b.Append(StrL("\\t"));
                break;
            case '\b':
                b.Append(StrL("\\b"));
                break;
            case '\f':
                b.Append(StrL("\\f"));
                break;
            default:
                if ((unsigned char)c < 0x20) {
                    // control characters must not appear raw in JSON
                    b.Append(fmt("\\u%04x", (int)(unsigned char)c));
                } else {
                    b.AppendChar(c);
                }
        }
    }
    b.AppendChar('"');
}

// ---------------------------------------------------------------------------
// ISO 8601 dates ("2026-09-28T12:34:56Z", always UTC) <-> time_t

static Str FormatIsoDate(time_t t) {
    if (t <= 0) {
        return {};
    }
    struct tm* ut = gmtime(&t);
    if (!ut) {
        return {};
    }
    return fmt("%04d-%02d-%02dT%02d:%02d:%02dZ", ut->tm_year + 1900, ut->tm_mon + 1, ut->tm_mday, ut->tm_hour,
               ut->tm_min, ut->tm_sec);
}

static time_t ParseIsoDate(Str s) {
    if (len(s) < 19) {
        return 0;
    }
    char* z = CStrTemp(s);
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    // "YYYY-MM-DD?HH:MM:SS" -- the separator between date and time is
    // usually 'T'; accept any single character
    int n = sscanf(z, "%4d-%2d-%2d%*c%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &sec);
    if (n != 6 || y <= 0) {
        return 0;
    }
    struct tm t{};
    t.tm_year = y - 1900;
    t.tm_mon = (mo >= 1 && mo <= 12) ? mo - 1 : 0;
    t.tm_mday = (d >= 1 && d <= 31) ? d : 1;
    t.tm_hour = (h >= 0 && h <= 23) ? h : 0;
    t.tm_min = (mi >= 0 && mi <= 59) ? mi : 0;
    t.tm_sec = (sec >= 0 && sec <= 59) ? sec : 0;
    return _mkgmtime(&t);
}

// ---------------------------------------------------------------------------
// color helpers (PDF colors are float 0..1 in MuPDF, 0..255 ints in JSON)

static void PdfColorToF(PdfColor col, float out[3]) {
    u8 r, g, b, a;
    UnpackPdfColor(col, r, g, b, a);
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
}

// ---------------------------------------------------------------------------
// assets: binary payloads of stamp and file attachment annotations.
// The JSON never holds binary data; payloads are written as
// <json dir>/assets/<fnv1a-64-hex>.<ext> and referenced from the JSON by
// their relative path. Content-addressed names dedup identical payloads
// and make the cleanup in SaveTab safe.

static uint64_t Fnv1a64(const u8* d, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= (uint64_t)d[i];
        h *= 1099511628211ull;
    }
    return h;
}

static Str AssetPathForData(const u8* d, size_t n, const char* ext) {
    uint64_t h = Fnv1a64(d, n);
    str::Builder b;
    // FmtArg rejects raw char*: wrap the extension (NUL-terminated) in a Str
    b.Append(fmt("assets/%08x%08x.%s", (uint32_t)(h >> 32), (uint32_t)h, Str(ext)));
    return b.TakeStr();
}

// "1a2b3c4d5e6f7080.png": 16 lowercase hex chars, a dot, 1-12 lowercase
// alphanumeric extension. Only files matching this exact pattern are ever
// touched by the cleanup, so anything a user puts into assets/ stays put
static bool IsHexAssetName(Str name) {
    if (len(name) < 16 + 2 || len(name) > 16 + 1 + 12) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        char c = name.s[i];
        bool hexChar = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hexChar) {
            return false;
        }
    }
    if (name.s[16] != '.') {
        return false;
    }
    for (int i = 17; i < len(name); i++) {
        char c = name.s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!ok) {
            return false;
        }
    }
    return true;
}

// only exact "assets/<hex name>" references are accepted; the writer always
// produces this shape, so anything else ("..", absolute paths, drive
// letters) is rejected before it ever reaches the file system
static bool IsSafeAssetRelPath(Str p) {
    constexpr int kPrefix = 7; // "assets/"
    if (len(p) < kPrefix + 16 + 2 || len(p) > kPrefix + 16 + 1 + 12) {
        return false;
    }
    if (!str::StartsWith(p, StrL("assets/"))) {
        return false;
    }
    return IsHexAssetName(Str(p.s + kPrefix, len(p) - kPrefix));
}

static bool StrContains(Str hay, Str needle) {
    if (len(needle) == 0 || len(hay) < len(needle)) {
        return false;
    }
    int max = len(hay) - len(needle);
    for (int i = 0; i <= max; i++) {
        if (0 == memcmp(hay.s + i, needle.s, len(needle))) {
            return true;
        }
    }
    return false;
}

// picks the asset file extension for an attachment's file name: last
// dot-separated token, sanitized to [a-z0-9], max 10 chars, "bin" fallback
static void AssetExtForFileName(Str fn, char* out, size_t outCap) {
    int start = -1;
    for (int i = len(fn) - 1; i >= 0; i--) {
        if (fn.s[i] == '.') {
            start = i + 1;
            break;
        }
    }
    int n = 0;
    if (start > 0) {
        for (int i = start; i < len(fn) && n < (int)outCap - 1; i++) {
            char c = fn.s[i];
            if (c >= 'A' && c <= 'Z') {
                c = (char)(c - 'A' + 'a');
            }
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                out[n++] = c;
            }
        }
    }
    if (n == 0) {
        out[0] = 'b';
        out[1] = 'i';
        out[2] = 'n';
        out[3] = 0;
        return;
    }
    out[n] = 0;
}

// decodes a stamp image into a PNG buffer. fz_new_buffer_from_image_as_png
// alone drops the /SMask transparency (it decodes the base image only), so
// the soft mask is composited into the PNG alpha channel here
static fz_buffer* PngFromStampImage(fz_context* ctx, fz_image* img) {
    fz_pixmap* pm = nullptr;
    fz_pixmap* mask = nullptr;
    fz_pixmap* out = nullptr;
    fz_buffer* res = nullptr;
    fz_var(pm);
    fz_var(mask);
    fz_var(out);
    fz_var(res);
    fz_try(ctx) {
        pm = fz_get_pixmap_from_image(ctx, img, nullptr, nullptr, nullptr, nullptr);
        if (!pm || pm->w <= 0 || pm->h <= 0) {
            fz_throw(ctx, FZ_ERROR_FORMAT, "stamp image decode failed");
        }
        // exotic colorspaces (CMYK, Indexed, ...) become gray/rgb: the PNG
        // writer only handles those
        fz_colorspace* cs = pm->colorspace;
        bool gray = !cs || fz_colorspace_is_gray(ctx, cs);
        if (!gray && !fz_colorspace_is_rgb(ctx, cs)) {
            fz_pixmap* cv =
                fz_convert_pixmap(ctx, pm, fz_device_rgb(ctx), nullptr, nullptr, fz_default_color_params, 0);
            fz_drop_pixmap(ctx, pm);
            pm = cv;
            gray = fz_colorspace_is_gray(ctx, pm->colorspace);
        }
        int useMask = 0;
        if (img->mask) {
            mask = fz_get_pixmap_from_image(ctx, img->mask, nullptr, nullptr, nullptr, nullptr);
            useMask = (mask && mask->w == pm->w && mask->h == pm->h) ? 1 : 0;
        }
        int ncol = pm->n - pm->alpha - pm->s;
        if (ncol > 3) {
            ncol = 3;
        }
        if (ncol < 1) {
            ncol = 1;
        }
        // a PDF image renders as base color x /SMask; pdf_add_image stores
        // straight-alpha pixmaps that way (base = color * 255 / alpha). To
        // make the PNG a correct straight-alpha image AND the re-import
        // (which runs the same pdf_add_image conversion) reproduce the same
        // base, the mask is multiplied back into the colors here
        int hasAlpha = useMask || pm->alpha;
        out = fz_new_pixmap(ctx, gray ? fz_device_gray(ctx) : fz_device_rgb(ctx), pm->w, pm->h, nullptr, hasAlpha);
        for (int y = 0; y < pm->h; y++) {
            const u8* s = pm->samples + (size_t)y * pm->stride;
            const u8* m = useMask ? (mask->samples + (size_t)y * mask->stride) : nullptr;
            u8* d = out->samples + (size_t)y * out->stride;
            for (int x = 0; x < pm->w; x++) {
                if (useMask) {
                    int a = m[x * (mask->n > 0 ? mask->n : 1)];
                    for (int c = 0; c < ncol; c++) {
                        d[c] = (u8)((s[c] * a + 127) / 255);
                    }
                    d[ncol] = (u8)a;
                } else {
                    for (int c = 0; c < ncol; c++) {
                        d[c] = s[c];
                    }
                    if (out->alpha) {
                        d[ncol] = pm->alpha ? s[ncol] : 255;
                    }
                }
                s += pm->n;
                d += out->n;
            }
        }
        res = fz_new_buffer_from_pixmap_as_png(ctx, out, fz_default_color_params);
    }
    fz_always(ctx) {
        fz_drop_pixmap(ctx, pm);
        fz_drop_pixmap(ctx, mask);
        fz_drop_pixmap(ctx, out);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        fz_drop_buffer(ctx, res);
        res = nullptr;
    }
    return res;
}

// extracts the payload bytes of a stamp (image XObject) or a file
// attachment (embedded file) into sa.assetBuf and computes sa.assetName.
// A stamp without an image (rubber stamp: "Approved" etc.) carries no
// payload at all. Sets assetOk=false when a payload exists but cannot be
// extracted; the entry is then skipped and counted on save.
static void CollectAssetPayload(Annotation* a, SidecarAnnot& sa) {
    EngineMupdf* e = a->engine;
    if (!e || !a->pdfannot) {
        return;
    }
    AutoUnlockRecursiveMutex cs(&e->docLock);
    fz_context* ctx = e->Ctx();
    fz_buffer* buf = nullptr;
    fz_var(buf);
    fz_try(ctx) {
        if (sa.type == AnnotationType::Stamp) {
            pdf_obj* imgobj = pdf_annot_stamp_image_obj(ctx, a->pdfannot);
            if (imgobj) {
                const char* ext = "png";
                fz_image* img = pdf_load_image(ctx, e->pdfdoc, imgobj);
                fz_try(ctx) {
                    fz_compressed_buffer* cb = fz_compressed_image_buffer(ctx, img);
                    if (cb && cb->params.type == FZ_IMAGE_JPEG && !img->use_decode && !img->mask) {
                        // a plain DCT stream is a standalone JPEG file: keep the
                        // original bytes for a byte-identical round-trip
                        buf = fz_keep_buffer(ctx, cb->buffer);
                        ext = "jpg";
                    } else {
                        buf = PngFromStampImage(ctx, img);
                    }
                    if (!buf || buf->len == 0) {
                        fz_throw(ctx, FZ_ERROR_FORMAT, "stamp image extraction failed");
                    }
                }
                fz_always(ctx) {
                    fz_drop_image(ctx, img);
                }
                fz_catch(ctx) {
                    fz_rethrow(ctx);
                }
                sa.assetName = AssetPathForData(buf->data, buf->len, ext);
                sa.assetBuf = buf;
                buf = nullptr; // ownership moved into sa
            }
            // no image: a rubber stamp ("Approved" etc.) is /Name + rect
            // only - no asset file (never return from inside fz_try: the
            // exception frame is only popped by fz_catch)
        } else if (sa.type == AnnotationType::FileAttachment) {
            pdf_obj* fs = pdf_annot_filespec(ctx, a->pdfannot);
            if (fs && pdf_is_embedded_file(ctx, fs)) {
                pdf_filespec_params p{};
                pdf_get_filespec_params(ctx, fs, &p);
                if (p.filename && *p.filename) {
                    sa.attachName = str::Dup(Str(p.filename));
                }
                if (p.mimetype && *p.mimetype) {
                    sa.attachMime = str::Dup(Str(p.mimetype));
                }
                sa.attachSize = p.size;
                if (p.created > 0) {
                    sa.attachCreated = (time_t)p.created;
                }
                if (p.modified > 0) {
                    sa.attachModified = (time_t)p.modified;
                }
                buf = pdf_load_embedded_file_contents(ctx, fs);
                if (!buf || buf->len == 0) {
                    fz_throw(ctx, FZ_ERROR_FORMAT, "attachment is empty");
                }
                char ext[12] = {};
                AssetExtForFileName(sa.attachName, ext, dimofi(ext));
                sa.assetName = AssetPathForData(buf->data, buf->len, ext);
                sa.assetBuf = buf;
                buf = nullptr; // ownership moved into sa
            } else {
                // external file specification: points at a file on the
                // author's machine, nothing embeddable to persist
                sa.assetOk = false;
            }
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        fz_drop_buffer(ctx, buf);
        logf("sidecar: failed to extract %s payload on page %d\n",
             sa.type == AnnotationType::Stamp ? StrL("stamp") : StrL("attachment"), sa.pageNo);
        sa.assetOk = false;
    }
}

// writes the payload files referenced by entries into assets/ next to the
// JSON. Returns the number of failed writes. Content-addressed names make
// this idempotent: unchanged payloads are simply overwritten in place
static int WriteSidecarAssets(Str jsonPath, const Vec<SidecarAnnot>& entries) {
    bool any = false;
    for (const SidecarAnnot& sa : entries) {
        if (sa.assetBuf && len(sa.assetName) > 0) {
            any = true;
            break;
        }
    }
    if (!any) {
        return 0;
    }
    TempStr jdir = path::GetDirTemp(jsonPath);
    if (len(jdir) == 0) {
        return 1;
    }
    TempStr assetsDir = path::JoinTemp(jdir, StrL("assets"));
    if (!dir::Exists(assetsDir) && !dir::CreateAll(assetsDir)) {
        logf("sidecar: cannot create '%s'\n", assetsDir);
        return 1;
    }
    int failed = 0;
    Vec<Str> written; // borrowed names, dedups within one save
    for (const SidecarAnnot& sa : entries) {
        if (!sa.assetBuf || len(sa.assetName) == 0) {
            continue;
        }
        bool done = false;
        for (Str w : written) {
            if (str::Eq(w, sa.assetName)) {
                done = true;
                break;
            }
        }
        if (done) {
            continue;
        }
        VecAppend(written, sa.assetName);
        TempStr dst = path::JoinTemp(jdir, sa.assetName);
        Str data((char*)sa.assetBuf->data, (int)sa.assetBuf->len);
        if (!file::WriteFile(dst, data)) {
            logf("sidecar: failed to write asset '%s'\n", dst);
            failed++;
        }
    }
    return failed;
}

// reads the payload files referenced by entries (relative to the JSON's
// directory). Entries whose file is missing or unreadable get assetOk=false
// and are skipped by the import; every entry owns its buffer copy
static void LoadSidecarAssetBuffers(fz_context* ctx, Str jsonPath, Vec<SidecarAnnot>& entries) {
    TempStr jdir = path::GetDirTemp(jsonPath);
    if (len(jdir) == 0) {
        return;
    }
    for (SidecarAnnot& sa : entries) {
        if (len(sa.assetName) == 0 || !sa.assetOk) {
            continue;
        }
        TempStr p = path::JoinTemp(jdir, sa.assetName);
        Str data = file::ReadFile(p);
        if (len(data) == 0) {
            logf("sidecar: asset '%s' is missing or empty\n", sa.assetName);
            sa.assetOk = false;
            continue;
        }
        fz_buffer* buf = nullptr;
        fz_try(ctx) {
            buf = fz_new_buffer_from_copied_data(ctx, (const unsigned char*)data.s, (size_t)data.len);
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
            buf = nullptr;
        }
        str::Free(data);
        if (!buf) {
            sa.assetOk = false;
            continue;
        }
        sa.assetBuf = buf;
    }
}

// after a save: delete files in assets/ that look like ours (16-hex names)
// and are not referenced by ANY sidecar JSON in the folder (raw text scan:
// the hex string appears verbatim in the referencing JSON, so assets
// shared with other documents' sidecars are never removed)
static void CleanupSidecarAssets(Str jsonPath) {
    TempStr jdir = path::GetDirTemp(jsonPath);
    if (len(jdir) == 0) {
        return;
    }
    TempStr assetsDir = path::JoinTemp(jdir, StrL("assets"));
    if (!dir::Exists(assetsDir)) {
        return;
    }
    Vec<Str> jsons; // owned
    for (DirIterEntry* de : DirIter(jdir)) {
        // md sidecars reference their assets the same way (in callout kv
        // lines), so both suffixes must be scanned or a still-referenced
        // payload would be deleted
        if (!de->isFile || !(str::EndsWithI(de->name, StrL(".json")) || str::EndsWithI(de->name, StrL(".md")))) {
            continue;
        }
        Str data = file::ReadFile(de->filePath);
        // an unreadable or empty json simply contributes no references
        if (len(data) > 0) {
            VecAppend(jsons, data);
        }
    }
    int nDeleted = 0;
    for (DirIterEntry* de : DirIter(assetsDir)) {
        if (!de->isFile || !IsHexAssetName(de->name)) {
            continue; // never touch anything that doesn't match our pattern
        }
        TempStr hex = Str(de->name.s, 16);
        bool referenced = false;
        for (Str j : jsons) {
            if (StrContains(j, hex)) {
                referenced = true;
                break;
            }
        }
        if (!referenced) {
            if (file::Delete(de->filePath)) {
                nDeleted++;
            } else {
                logf("sidecar: cleanup could not delete '%s'\n", de->filePath);
            }
        }
    }
    for (Str j : jsons) {
        str::Free(j);
    }
    if (nDeleted > 0) {
        logf("sidecar: removed %d unreferenced asset file(s) from '%s'\n", nDeleted, assetsDir);
    }
}

// ---------------------------------------------------------------------------
// collect: Annotation -> SidecarAnnot

// reads the fields that have no SumatraPDF wrapper: /NM, /Subj, creation
// date, flags, popup open state. Runs one docLock scope per annotation.
static void CollectAnnotExtras(Annotation* a, SidecarAnnot& sa) {
    EngineMupdf* e = a->engine;
    if (!e || !a->pdfannot) {
        return;
    }
    AutoUnlockRecursiveMutex cs(&e->docLock);
    fz_context* ctx = e->Ctx();
    fz_try(ctx) {
        const char* nm = pdf_annot_name(ctx, a->pdfannot);
        if (nm && *nm) {
            sa.name = str::Dup(Str(nm));
        }
        if (pdf_annot_has_subject(ctx, a->pdfannot)) {
            const char* sub = pdf_annot_subject(ctx, a->pdfannot);
            if (sub && *sub) {
                sa.subject = str::Dup(Str(sub));
            }
        }
        int64_t cd = pdf_annot_creation_date(ctx, a->pdfannot);
        if (cd > 0) {
            sa.creationDate = (time_t)cd;
        }
        sa.flags = pdf_annot_flags(ctx, a->pdfannot);
        if (a->type == AnnotationType::Text) {
            sa.isOpen = pdf_annot_is_open(ctx, a->pdfannot) != 0;
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logf("sidecar: failed to read annotation extras\n");
    }
}

// minimal UTF-8 encoder for a single unicode code point (stext chars are
// full code points, no surrogate handling needed)
static void AppendUtf8CodePoint(str::Builder& b, int c) {
    if (c < 0 || c > 0x10FFFF) {
        c = '?';
    }
    if (c < 0x80) {
        b.AppendChar((char)c);
    } else if (c < 0x800) {
        b.AppendChar((char)(0xC0 | (c >> 6)));
        b.AppendChar((char)(0x80 | (c & 0x3F)));
    } else if (c < 0x10000) {
        b.AppendChar((char)(0xE0 | (c >> 12)));
        b.AppendChar((char)(0x80 | ((c >> 6) & 0x3F)));
        b.AppendChar((char)(0x80 | (c & 0x3F)));
    } else {
        b.AppendChar((char)(0xF0 | (c >> 18)));
        b.AppendChar((char)(0x80 | ((c >> 12) & 0x3F)));
        b.AppendChar((char)(0x80 | ((c >> 6) & 0x3F)));
        b.AppendChar((char)(0x80 | (c & 0x3F)));
    }
}

static bool PointInQuadBBox(float x, float y, fz_quad q) {
    float x0 = std::min(std::min(q.ul.x, q.ur.x), std::min(q.ll.x, q.lr.x));
    float x1 = std::max(std::max(q.ul.x, q.ur.x), std::max(q.ll.x, q.lr.x));
    float y0 = std::min(std::min(q.ul.y, q.ur.y), std::min(q.ll.y, q.lr.y));
    float y1 = std::max(std::max(q.ul.y, q.ur.y), std::max(q.ll.y, q.lr.y));
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

static bool PointInAnyQuad(float x, float y, const Vec<fz_quad>& quads) {
    for (const fz_quad& q : quads) {
        if (PointInQuadBBox(x, y, q)) {
            return true;
        }
    }
    return false;
}

// one-entry cache: the export walks annotations grouped by page in the
// common case, so the previous stext page can be reused
struct StextCache {
    EngineMupdf* e = nullptr;
    int pageNo = 0; // 1-based page of tp
    fz_stext_page* tp = nullptr;
};

static void StextCacheReset(StextCache& c) {
    if (c.tp) {
        AutoUnlockRecursiveMutex cs(&c.e->docLock);
        fz_drop_stext_page(c.e->Ctx(), c.tp);
        c.tp = nullptr;
        c.pageNo = 0;
    }
}

static fz_stext_page* StextCacheGet(StextCache& c, int pageNo) {
    if (c.tp && c.pageNo == pageNo) {
        return c.tp;
    }
    StextCacheReset(c);
    // takes pagesLock internally: must run OUTSIDE docLock (import path
    // uses GetFzPageInfo the same way)
    FzPageInfo* pi = c.e->GetFzPageInfo(pageNo, true);
    if (!pi || !pi->page) {
        return nullptr;
    }
    fz_context* ctx = c.e->Ctx();
    fz_stext_page* tp = nullptr;
    AutoUnlockRecursiveMutex cs(&c.e->docLock);
    fz_try(ctx) {
        tp = fz_new_stext_page_from_page(ctx, pi->page, nullptr);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        tp = nullptr;
    }
    if (!tp) {
        return nullptr;
    }
    c.tp = tp;
    c.pageNo = pageNo;
    return tp;
}

// excerpt of the text under the markup quads: for each stext line, keep
// the characters whose quad center falls inside one of the annotation
// quads; lines are joined with '\n'. Empty when the page has no text
// layer (scanned PDFs without OCR).
static Str ExtractTextUnderQuads(StextCache& cache, int pageNo, const Vec<fz_quad>& quads) {
    if (len(quads) == 0) {
        return {};
    }
    fz_stext_page* tp = StextCacheGet(cache, pageNo);
    if (!tp) {
        return {};
    }
    str::Builder res;
    bool anyLine = false;
    for (fz_stext_block* blk = tp->first_block; blk; blk = blk->next) {
        if (blk->type != FZ_STEXT_BLOCK_TEXT) {
            continue;
        }
        for (fz_stext_line* ln = blk->u.t.first_line; ln; ln = ln->next) {
            str::Builder line;
            bool anyChar = false;
            for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next) {
                const fz_quad& cq = ch->quad;
                float cx = (cq.ul.x + cq.ur.x + cq.ll.x + cq.lr.x) / 4.0f;
                float cy = (cq.ul.y + cq.ur.y + cq.ll.y + cq.lr.y) / 4.0f;
                if (PointInAnyQuad(cx, cy, quads)) {
                    AppendUtf8CodePoint(line, ch->c);
                    anyChar = true;
                }
            }
            if (anyChar) {
                if (anyLine) {
                    res.AppendChar('\n');
                }
                Str ls = line.TakeStr();
                res.Append(ls);
                str::Free(ls);
                anyLine = true;
            }
        }
    }
    return res.TakeStr();
}

// GetColor()/InteriorColor() cannot tell "key absent", "empty array" and
// "black" apart: mupdf reports n == 0 with a zeroed color array both for a
// missing /C and for an EMPTY /C [] - and "none"/transparent backgrounds
// are stored exactly as /C [] (SetColor with c == 0 writes an empty array).
// PdfColorFromFloat() then turns that into opaque black, so the round-trip
// would give every color-less annotation a black fill. Ask for the number
// of components instead: only n > 0 is a real color.
static int AnnotColorComponents(Annotation* a, bool interior) {
    if (!a || !AnnotationIsLive(a)) {
        return 0;
    }
    EngineMupdf* e = a->engine;
    pdf_annot* pa = a->pdfannot;
    if (!e || !pa) {
        return 0;
    }
    fz_context* ctx = e->Ctx();
    AutoUnlockRecursiveMutex cs(&e->docLock);
    int n = 0;
    float color[4]{};
    fz_try(ctx) {
        // pdf_annot_interior_color() rejects types without /IC support
        // (whitelist); that means "no interior color" for us
        if (interior) {
            pdf_annot_interior_color(ctx, pa, &n, color);
        } else {
            pdf_annot_color(ctx, pa, &n, color);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        n = 0;
    }
    return n;
}

static void CollectSidecarAnnot(Annotation* a, SidecarAnnot& sa, StextCache& cache) {
    sa.type = a->type;
    sa.pageNo = PageNo(a);
    sa.bounds = GetBounds(a);
    // the string wrappers return temp-arena memory: copy to the heap so the
    // entries stay valid no matter how much temp memory follows
    Str t;
    t = Author(a);
    sa.author = len(t) > 0 ? str::Dup(t) : Str{};
    t = Contents(a);
    sa.contents = len(t) > 0 ? str::Dup(t) : Str{};
    t = IconName(a);
    sa.icon = len(t) > 0 ? str::Dup(t) : Str{};
    sa.modDate = ModificationDate(a);
    // stamps and file attachments carry a binary payload: extract it into
    // an owned buffer + compute the content-addressed asset name
    if (a->type == AnnotationType::Stamp || a->type == AnnotationType::FileAttachment) {
        CollectAssetPayload(a, sa);
    }

    // only write color when the annotation has a REAL one (n > 0): an
    // absent /C or the empty /C [] of a "none"/transparent background must
    // not become opaque black in the round-trip
    if (AnnotColorComponents(a, false) > 0) {
        PdfColor c = GetColor(a);
        if (c != (PdfColor)kColorUnset) {
            PdfColorToF(c, sa.color);
            sa.hasColor = true;
        }
    }
    // interiorColor: same empty-array trap as color
    if (AnnotColorComponents(a, true) > 0) {
        PdfColor ic = InteriorColor(a);
        if (ic != (PdfColor)kColorUnset) {
            PdfColorToF(ic, sa.interiorCol);
            sa.hasInterior = true;
        }
    }
    int op = Opacity(a);
    sa.opacity = (op >= 0 && op < 100) ? op / 100.0f : 1.0f;
    int bw = BorderWidth(a);
    sa.borderWidth = (bw >= 0) ? bw : -1;

    if (a->type == AnnotationType::Line) {
        PointF s, e2;
        if (GetLinePoints(a, s, e2)) {
            sa.hasLine = true;
            sa.lineA = s;
            sa.lineB = e2;
        }
        int ls = 0, le = 0;
        GetLineEndingStyles(a, &ls, &le);
        sa.lineStart = ls;
        sa.lineEnd = le;
    } else if (a->type == AnnotationType::FreeText) {
        int ts = DefaultAppearanceTextSize(a);
        sa.textSize = (ts > 0) ? ts : -1;
        sa.quadding = Quadding(a);
        // /DA text color; n == 0 (absent) must not become black
        PdfColor tc = DefaultAppearanceTextColor(a);
        if (tc != (PdfColor)kColorUnset) {
            PdfColorToF(tc, sa.textColor);
            sa.hasTextCol = true;
        }
        // font family + bold/italic/underline come from /DS (or the base-14
        // fallback of /DA); the sidebar's Text Font / B / I / U chips read
        // exactly these
        Str fam = FreeTextFontFamily(a);
        sa.fontFamily = len(fam) > 0 ? str::Dup(fam) : Str{};
        sa.fontStyle = FreeTextFontStyle(a);
    }

    // quads / vertices / ink via wrappers (Redact holds quads too: a
    // region-marking redact stores its coverage exactly like a markup)
    if (AnnotationIsTextMarkup(a->type) || a->type == AnnotationType::Redact) {
        EngineMupdf* e = a->engine;
        if (e && a->pdfannot) {
            AutoUnlockRecursiveMutex cs(&e->docLock);
            fz_context* ctx = e->Ctx();
            fz_try(ctx) {
                int n = pdf_annot_quad_point_count(ctx, a->pdfannot);
                for (int i = 0; i < n && i < kMaxQuads; i++) {
                    VecAppend(sa.quads, pdf_annot_quad_point(ctx, a->pdfannot, i));
                }
            }
            fz_catch(ctx) {
                fz_report_error(ctx);
            }
        }
    } else if (a->type == AnnotationType::Polygon || a->type == AnnotationType::PolyLine) {
        Vec<PointF> v = GetVertices(a);
        for (int i = 0; i < len(v) && i < kMaxVertices; i++) {
            VecAppend(sa.vertices, v[i]);
        }
    } else if (a->type == AnnotationType::Ink) {
        Vec<int> counts;
        Vec<PointF> pts;
        GetInkList(a, counts, pts);
        int off = 0;
        int total = 0; // cap across strokes, mirrors the reader
        for (int sIdx = 0; sIdx < len(counts); sIdx++) {
            Vec<PointF> stroke;
            int n = counts[sIdx];
            for (int i = 0; i < n; i++, off++) {
                if (total < kMaxInkPoints && off < len(pts)) {
                    VecAppend(stroke, pts[off]);
                    total++;
                }
            }
            if (len(stroke) > 0) {
                VecAppend(sa.inkStrokes, std::move(stroke));
            }
        }
    }

    CollectAnnotExtras(a, sa);

    if (AnnotationIsTextMarkup(a->type)) {
        sa.text = ExtractTextUnderQuads(cache, sa.pageNo, sa.quads);
    }
}

// ---------------------------------------------------------------------------
// JSON writer

static void AppendFloat2(str::Builder& b, float f) {
    if (!isfinite(f)) {
        f = 0; // never emit nan/inf: they would corrupt the JSON
    }
    b.Append(fmt("%.2f", f));
}

static void AppendColorArray(str::Builder& b, const float col[3]) {
    // the collect side already rounds to 1/255 steps
    int r = (int)(col[0] * 255.0f + 0.5f);
    int g = (int)(col[1] * 255.0f + 0.5f);
    int bl = (int)(col[2] * 255.0f + 0.5f);
    if (r < 0) {
        r = 0;
    }
    if (r > 255) {
        r = 255;
    }
    if (g < 0) {
        g = 0;
    }
    if (g > 255) {
        g = 255;
    }
    if (bl < 0) {
        bl = 0;
    }
    if (bl > 255) {
        bl = 255;
    }
    b.Append(fmt("[%d,%d,%d]", r, g, bl));
}

static void SerializeAnnotJson(str::Builder& b, const SidecarAnnot& sa) {
    const char* tname = SidecarNameForType(sa.type);
    ReportIf(!tname);
    b.Append(StrL("    {\n"));
    b.Append(StrL("      \"type\": "));
    AppendEscapedJson(b, Str(tname));
    b.Append(fmt(",\n      \"page\": %d,\n", sa.pageNo - 1));

    b.Append(StrL("      \"rect\": ["));
    AppendFloat2(b, sa.bounds.x);
    b.AppendChar(',');
    AppendFloat2(b, sa.bounds.y);
    b.AppendChar(',');
    AppendFloat2(b, sa.bounds.x + sa.bounds.dx);
    b.AppendChar(',');
    AppendFloat2(b, sa.bounds.y + sa.bounds.dy);
    b.Append(StrL("]"));

    if ((AnnotationIsTextMarkup(sa.type) || sa.type == AnnotationType::Redact) && len(sa.quads) > 0) {
        b.Append(StrL(",\n      \"quads\": ["));
        for (int i = 0; i < len(sa.quads); i++) {
            if (i > 0) {
                b.AppendChar(',');
            }
            const fz_quad& q = sa.quads[i];
            b.Append(StrL("\n        "));
            b.AppendChar('[');
            AppendFloat2(b, q.ul.x);
            b.AppendChar(',');
            AppendFloat2(b, q.ul.y);
            b.AppendChar(',');
            AppendFloat2(b, q.ur.x);
            b.AppendChar(',');
            AppendFloat2(b, q.ur.y);
            b.AppendChar(',');
            AppendFloat2(b, q.ll.x);
            b.AppendChar(',');
            AppendFloat2(b, q.ll.y);
            b.AppendChar(',');
            AppendFloat2(b, q.lr.x);
            b.AppendChar(',');
            AppendFloat2(b, q.lr.y);
            b.AppendChar(']');
        }
        b.Append(StrL("\n      ]"));
    }

    if (sa.type == AnnotationType::Line && sa.hasLine) {
        b.Append(StrL(",\n      \"start\": ["));
        AppendFloat2(b, sa.lineA.x);
        b.AppendChar(',');
        AppendFloat2(b, sa.lineA.y);
        b.Append(StrL("],\n      \"end\": ["));
        AppendFloat2(b, sa.lineB.x);
        b.AppendChar(',');
        AppendFloat2(b, sa.lineB.y);
        b.Append(StrL("]"));
    }

    if ((sa.type == AnnotationType::Polygon || sa.type == AnnotationType::PolyLine) && len(sa.vertices) > 0) {
        b.Append(StrL(",\n      \"vertices\": ["));
        for (int i = 0; i < len(sa.vertices); i++) {
            if (i > 0) {
                b.AppendChar(',');
            }
            b.AppendChar('[');
            AppendFloat2(b, sa.vertices[i].x);
            b.AppendChar(',');
            AppendFloat2(b, sa.vertices[i].y);
            b.AppendChar(']');
        }
        b.AppendChar(']');
    }

    if (sa.type == AnnotationType::Ink && len(sa.inkStrokes) > 0) {
        b.Append(StrL(",\n      \"inkList\": ["));
        for (int sIdx = 0; sIdx < len(sa.inkStrokes); sIdx++) {
            if (sIdx > 0) {
                b.AppendChar(',');
            }
            const Vec<PointF>& stroke = sa.inkStrokes[sIdx];
            b.AppendChar('[');
            for (int i = 0; i < len(stroke); i++) {
                if (i > 0) {
                    b.AppendChar(',');
                }
                b.AppendChar('[');
                AppendFloat2(b, stroke[i].x);
                b.AppendChar(',');
                AppendFloat2(b, stroke[i].y);
                b.AppendChar(']');
            }
            b.AppendChar(']');
        }
        b.AppendChar(']');
    }

    if (sa.hasColor) {
        b.Append(StrL(",\n      \"color\": "));
        AppendColorArray(b, sa.color);
    }
    if (sa.hasInterior && AnnotationSupportsInteriorColor(sa.type)) {
        b.Append(StrL(",\n      \"interiorColor\": "));
        AppendColorArray(b, sa.interiorCol);
    }
    if (sa.opacity < 0.999f) {
        b.Append(StrL(",\n      \"opacity\": "));
        b.Append(fmt("%.2f", sa.opacity));
    }
    if (sa.borderWidth > 0) {
        b.Append(fmt(",\n      \"borderWidth\": %d", sa.borderWidth));
    }
    if (sa.type == AnnotationType::Line && (sa.lineStart != 0 || sa.lineEnd != 0)) {
        int ls = sa.lineStart;
        int le = sa.lineEnd;
        int lastIdx = (int)dimofi(kLineEndingNames) - 1;
        if (ls < 0) {
            ls = 0;
        }
        if (ls > lastIdx) {
            ls = lastIdx;
        }
        if (le < 0) {
            le = 0;
        }
        if (le > lastIdx) {
            le = lastIdx;
        }
        b.Append(fmt(",\n      \"lineStart\": \"%s\",\n      \"lineEnd\": \"%s\"", Str(kLineEndingNames[ls]),
                     Str(kLineEndingNames[le])));
    }
    if ((sa.type == AnnotationType::Text || sa.type == AnnotationType::Stamp ||
         sa.type == AnnotationType::FileAttachment) &&
        len(sa.icon) > 0) {
        b.Append(StrL(",\n      \"icon\": "));
        AppendEscapedJson(b, sa.icon);
    }
    if (len(sa.assetName) > 0) {
        // relative to this JSON: "assets/<fnv1a-64-hex>.<ext>"
        b.Append(StrL(",\n      \"asset\": "));
        AppendEscapedJson(b, sa.assetName);
    }
    if (sa.type == AnnotationType::FileAttachment && len(sa.attachName) > 0) {
        b.Append(StrL(",\n      \"attachment\": {\n        \"filename\": "));
        AppendEscapedJson(b, sa.attachName);
        if (len(sa.attachMime) > 0) {
            b.Append(StrL(",\n        \"mimeType\": "));
            AppendEscapedJson(b, sa.attachMime);
        }
        if (sa.attachSize >= 0) {
            b.Append(fmt(",\n        \"size\": %d", sa.attachSize));
        }
        if (sa.attachCreated > 0) {
            Str d = FormatIsoDate(sa.attachCreated);
            if (len(d) > 0) {
                b.Append(StrL(",\n        \"created\": "));
                AppendEscapedJson(b, d);
            }
        }
        if (sa.attachModified > 0) {
            Str d = FormatIsoDate(sa.attachModified);
            if (len(d) > 0) {
                b.Append(StrL(",\n        \"modified\": "));
                AppendEscapedJson(b, d);
            }
        }
        b.Append(StrL("\n      }"));
    }
    if (sa.type == AnnotationType::Text && sa.isOpen) {
        b.Append(StrL(",\n      \"isOpen\": true"));
    }
    if (sa.type == AnnotationType::FreeText) {
        if (sa.textSize > 0) {
            b.Append(fmt(",\n      \"fontSize\": %d", sa.textSize));
        }
        if (sa.hasTextCol) {
            b.Append(StrL(",\n      \"textColor\": "));
            AppendColorArray(b, sa.textColor);
        }
        if (sa.quadding >= 0) {
            b.Append(fmt(",\n      \"textAlign\": %d", sa.quadding));
        }
        if (len(sa.fontFamily) > 0) {
            b.Append(StrL(",\n      \"fontFamily\": "));
            AppendEscapedJson(b, sa.fontFamily);
        }
        if (sa.fontStyle != 0) {
            b.Append(fmt(",\n      \"textStyle\": %d", sa.fontStyle));
        }
    }
    if (sa.flags >= 0) {
        b.Append(fmt(",\n      \"flags\": %d", sa.flags));
    }
    if (len(sa.author) > 0) {
        b.Append(StrL(",\n      \"author\": "));
        AppendEscapedJson(b, sa.author);
    }
    if (len(sa.subject) > 0) {
        b.Append(StrL(",\n      \"subject\": "));
        AppendEscapedJson(b, sa.subject);
    }
    if (len(sa.name) > 0) {
        b.Append(StrL(",\n      \"name\": "));
        AppendEscapedJson(b, sa.name);
    }
    if (AnnotationIsTextMarkup(sa.type) && len(sa.text) > 0) {
        b.Append(StrL(",\n      \"text\": "));
        AppendEscapedJson(b, sa.text);
    }
    if (len(sa.contents) > 0) {
        b.Append(StrL(",\n      \"contents\": "));
        AppendEscapedJson(b, sa.contents);
    }
    Str cdate = FormatIsoDate(sa.creationDate);
    if (len(cdate) > 0) {
        b.Append(StrL(",\n      \"creationDate\": "));
        AppendEscapedJson(b, cdate);
    }
    Str mdate = FormatIsoDate(sa.modDate);
    if (len(mdate) > 0) {
        b.Append(StrL(",\n      \"modDate\": "));
        AppendEscapedJson(b, mdate);
    }
    b.Append(StrL("\n    }"));
}

static Str DocTitle(EngineMupdf* e) {
    if (!e || !e->pdfdoc) {
        return {};
    }
    AutoUnlockRecursiveMutex cs(&e->docLock);
    fz_context* ctx = e->Ctx();
    char buf[512] = {};
    int bufSize = dimofi(buf);
    int n = 0;
    fz_try(ctx) {
        // n is the size needed (like snprintf): clamp on truncation, then
        // drop the trailing NUL before dup'ing -- same as EngineMupdf.cpp
        n = pdf_lookup_metadata(ctx, e->pdfdoc, FZ_META_INFO_TITLE, buf, bufSize);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        n = 0;
    }
    if (n <= 0) {
        return {};
    }
    if (n > bufSize) {
        n = bufSize - 1;
        buf[bufSize - 1] = 0;
    }
    return str::Dup(Str(buf, n - 1));
}

static constexpr int kSidecarVersion = 2;

// collect the live annotations; shared by the JSON and the Markdown writer
static void CollectSidecarEntries(EngineMupdf* e, const Vec<Annotation*>& annots, int& nSkippedUnsupported,
                                  Vec<SidecarAnnot>& entries) {
    nSkippedUnsupported = 0;
    StextCache cache;
    cache.e = e;
    int nWritten = 0;
    for (Annotation* a : annots) {
        if (!a || !AnnotationIsLive(a)) {
            continue;
        }
        if (!SidecarTypeSupported(a->type)) {
            // unsupported types (link / popup / widget / media ...) and
            // payloads that could not be extracted are counted so the user
            // learns why they don't come back after a reopen
            nSkippedUnsupported++;
            continue;
        }
        SidecarAnnot sa;
        CollectSidecarAnnot(a, sa, cache);
        if (sa.pageNo < 1 || sa.bounds.IsEmpty() || !sa.assetOk) {
            if (!sa.assetOk) {
                nSkippedUnsupported++;
            }
            FreeSidecarAnnotStrings(e->Ctx(), sa);
            continue;
        }
        VecAppend(entries, std::move(sa));
        nWritten++;
    }
    StextCacheReset(cache);
    logf("sidecar: exported %d annotations\n", nWritten);
}

static Str BuildSidecarJson(EngineMupdf* e, Str pdfPath, const Vec<SidecarAnnot>& entries) {
    str::Builder b;
    b.Append(fmt("{\n  \"version\": %d,\n  \"generator\": \"SumatraPDF-sidecar/2\",\n", kSidecarVersion));
    TempStr base = path::GetBaseNameTemp(pdfPath);
    if (base && len(base) > 0) {
        b.Append(StrL("  \"file\": "));
        AppendEscapedJson(b, Str(base));
        b.Append(StrL(",\n"));
    }
    Str title = DocTitle(e);
    if (len(title) > 0) {
        b.Append(StrL("  \"title\": "));
        AppendEscapedJson(b, title);
        str::Free(title);
        b.Append(StrL(",\n"));
    }
    b.Append(StrL("  \"annotations\": ["));
    bool first = true;
    for (const SidecarAnnot& sa : entries) {
        if (!first) {
            b.AppendChar(',');
        }
        b.AppendChar('\n');
        SerializeAnnotJson(b, sa);
        first = false;
    }
    if (!first) {
        b.AppendChar('\n');
    }
    b.Append(StrL("  ]\n}\n"));
    return b.TakeStr();
}

// ---------------------------------------------------------------------------
// JSON reader (MuPDF's fz_parse_json produces a DOM owned by a pool)

static fz_json* JsonGet(fz_context* ctx, fz_json* o, const char* key) {
    return fz_json_object_get(ctx, o, key);
}

static const char* JsonStr(fz_context* ctx, fz_json* o, const char* key) {
    fz_json* v = JsonGet(ctx, o, key);
    if (!v || !fz_json_is_string(ctx, v)) {
        return nullptr;
    }
    return fz_json_to_string(ctx, v);
}

static double JsonNum(fz_context* ctx, fz_json* o, const char* key, double dflt) {
    fz_json* v = JsonGet(ctx, o, key);
    if (!v || !fz_json_is_number(ctx, v)) {
        return dflt;
    }
    return fz_json_to_number(ctx, v);
}

static fz_json* JsonArr(fz_context* ctx, fz_json* o, const char* key) {
    fz_json* v = JsonGet(ctx, o, key);
    if (!v || !fz_json_is_array(ctx, v)) {
        return nullptr;
    }
    return v;
}

static int JsonInt(fz_context* ctx, fz_json* o, const char* key, int dflt) {
    // the default is returned verbatim when the key is missing / not a
    // number, so "unset" sentinels (-1) survive
    double d = JsonNum(ctx, o, key, (double)dflt);
    if (!(d > -2147483648.0 && d < 2147483647.0)) {
        return dflt; // out of range or nan
    }
    return (int)lround(d);
}

static bool JsonBool(fz_context* ctx, fz_json* o, const char* key, bool dflt) {
    fz_json* v = JsonGet(ctx, o, key);
    if (!v) {
        return dflt;
    }
    if (fz_json_is_boolean(ctx, v)) {
        return fz_json_to_boolean(ctx, v) != 0;
    }
    return dflt;
}

static double ArrNum(fz_context* ctx, fz_json* arr, int ix) {
    fz_json* v = fz_json_array_get(ctx, arr, ix);
    if (!v || !fz_json_is_number(ctx, v)) {
        return 0;
    }
    return fz_json_to_number(ctx, v);
}

// fills sa from a JSON object; returns false when the entry is unusable.
// strings are duped out of the json pool (the pool dies right after the
// parse) -- the caller must free them with FreeSidecarAnnotStrings.
static bool JsonToSidecarAnnot(fz_context* ctx, fz_json* o, SidecarAnnot& sa) {
    const char* tn = JsonStr(ctx, o, "type");
    if (!tn || !*tn) {
        return false;
    }
    sa.type = SidecarTypeFromName(tn);
    if (!SidecarTypeSupported(sa.type)) {
        return false;
    }
    sa.pageNo = JsonInt(ctx, o, "page", -1) + 1;
    if (sa.pageNo < 1 || sa.pageNo > 100000) {
        return false;
    }
    fz_json* r = JsonArr(ctx, o, "rect");
    if (!r || fz_json_array_length(ctx, r) < 4) {
        return false;
    }
    float x0 = (float)ArrNum(ctx, r, 0);
    float y0 = (float)ArrNum(ctx, r, 1);
    float x1 = (float)ArrNum(ctx, r, 2);
    float y1 = (float)ArrNum(ctx, r, 3);
    sa.bounds = RectF::FromXY(x0, y0, x1, y1);
    if (sa.bounds.IsEmpty()) {
        return false;
    }

    fz_json* qc = JsonArr(ctx, o, "quads");
    if (qc) {
        int n = fz_json_array_length(ctx, qc);
        if (n > kMaxQuads) {
            n = kMaxQuads;
        }
        for (int i = 0; i < n; i++) {
            fz_json* q = fz_json_array_get(ctx, qc, i);
            if (!q || !fz_json_is_array(ctx, q) || fz_json_array_length(ctx, q) < 8) {
                continue;
            }
            fz_quad fq = fz_make_quad((float)ArrNum(ctx, q, 0), (float)ArrNum(ctx, q, 1), (float)ArrNum(ctx, q, 2),
                                      (float)ArrNum(ctx, q, 3), (float)ArrNum(ctx, q, 4), (float)ArrNum(ctx, q, 5),
                                      (float)ArrNum(ctx, q, 6), (float)ArrNum(ctx, q, 7));
            VecAppend(sa.quads, fq);
        }
    }

    fz_json* vs = JsonArr(ctx, o, "vertices");
    if (vs) {
        int n = fz_json_array_length(ctx, vs);
        if (n > kMaxVertices) {
            n = kMaxVertices;
        }
        for (int i = 0; i < n; i++) {
            fz_json* v = fz_json_array_get(ctx, vs, i);
            if (!v || !fz_json_is_array(ctx, v) || fz_json_array_length(ctx, v) < 2) {
                continue;
            }
            VecAppend(sa.vertices, PointF{(float)ArrNum(ctx, v, 0), (float)ArrNum(ctx, v, 1)});
        }
    }

    fz_json* st = JsonArr(ctx, o, "start");
    fz_json* en = JsonArr(ctx, o, "end");
    if (st && en && fz_json_array_length(ctx, st) >= 2 && fz_json_array_length(ctx, en) >= 2) {
        sa.hasLine = true;
        sa.lineA = PointF{(float)ArrNum(ctx, st, 0), (float)ArrNum(ctx, st, 1)};
        sa.lineB = PointF{(float)ArrNum(ctx, en, 0), (float)ArrNum(ctx, en, 1)};
    }

    fz_json* ink = JsonArr(ctx, o, "inkList");
    if (ink) {
        int nStrokes = fz_json_array_length(ctx, ink);
        int total = 0;
        for (int sIdx = 0; sIdx < nStrokes; sIdx++) {
            fz_json* stroke = fz_json_array_get(ctx, ink, sIdx);
            if (!stroke || !fz_json_is_array(ctx, stroke)) {
                continue;
            }
            Vec<PointF> pts;
            int n = fz_json_array_length(ctx, stroke);
            for (int i = 0; i < n && total < kMaxInkPoints; i++, total++) {
                fz_json* p = fz_json_array_get(ctx, stroke, i);
                if (!p || !fz_json_is_array(ctx, p) || fz_json_array_length(ctx, p) < 2) {
                    continue;
                }
                VecAppend(pts, PointF{(float)ArrNum(ctx, p, 0), (float)ArrNum(ctx, p, 1)});
            }
            if (len(pts) > 0) {
                VecAppend(sa.inkStrokes, std::move(pts));
            }
        }
    }

    fz_json* col = JsonArr(ctx, o, "color");
    if (col && fz_json_array_length(ctx, col) >= 3) {
        for (int i = 0; i < 3; i++) {
            float f = (float)(ArrNum(ctx, col, i) / 255.0);
            if (f < 0) {
                f = 0;
            }
            if (f > 1) {
                f = 1;
            }
            sa.color[i] = f;
        }
        sa.hasColor = true;
    }
    fz_json* icol = JsonArr(ctx, o, "interiorColor");
    if (icol && fz_json_array_length(ctx, icol) >= 3) {
        for (int i = 0; i < 3; i++) {
            float f = (float)(ArrNum(ctx, icol, i) / 255.0);
            if (f < 0) {
                f = 0;
            }
            if (f > 1) {
                f = 1;
            }
            sa.interiorCol[i] = f;
        }
        sa.hasInterior = true;
    }
    fz_json* tcol = JsonArr(ctx, o, "textColor");
    if (tcol && fz_json_array_length(ctx, tcol) >= 3) {
        for (int i = 0; i < 3; i++) {
            float f = (float)(ArrNum(ctx, tcol, i) / 255.0);
            if (f < 0) {
                f = 0;
            }
            if (f > 1) {
                f = 1;
            }
            sa.textColor[i] = f;
        }
        sa.hasTextCol = true;
    }

    double op = JsonNum(ctx, o, "opacity", 1.0);
    if (op >= 0 && op <= 1.0) {
        sa.opacity = (float)op;
    }
    const char* ff = JsonStr(ctx, o, "fontFamily");
    if (ff && *ff) {
        sa.fontFamily = str::Dup(Str(ff));
    }
    sa.fontStyle = JsonInt(ctx, o, "textStyle", 0);
    sa.borderWidth = JsonInt(ctx, o, "borderWidth", -1);
    const char* ls = JsonStr(ctx, o, "lineStart");
    if (ls && *ls) {
        sa.lineStart = LineEndingFromName(Str(ls));
    }
    const char* le = JsonStr(ctx, o, "lineEnd");
    if (le && *le) {
        sa.lineEnd = LineEndingFromName(Str(le));
    }
    const char* icon = JsonStr(ctx, o, "icon");
    if (icon && *icon) {
        sa.icon = str::Dup(Str(icon));
    }
    sa.isOpen = JsonBool(ctx, o, "isOpen", false);
    sa.textSize = JsonInt(ctx, o, "fontSize", -1);
    sa.quadding = JsonInt(ctx, o, "textAlign", -1);
    if (sa.quadding < 0 || sa.quadding > 2) {
        sa.quadding = -1;
    }
    sa.flags = JsonInt(ctx, o, "flags", -1);

    const char* author = JsonStr(ctx, o, "author");
    if (author && *author) {
        sa.author = str::Dup(Str(author));
    }
    const char* subject = JsonStr(ctx, o, "subject");
    if (subject && *subject) {
        sa.subject = str::Dup(Str(subject));
    }
    const char* nm = JsonStr(ctx, o, "name");
    if (nm && *nm) {
        sa.name = str::Dup(Str(nm));
    }
    const char* text = JsonStr(ctx, o, "text");
    if (text && *text) {
        sa.text = str::Dup(Str(text));
    }
    const char* contents = JsonStr(ctx, o, "contents");
    if (contents && *contents) {
        sa.contents = str::Dup(Str(contents));
    }
    const char* cd = JsonStr(ctx, o, "creationDate");
    if (cd && *cd) {
        sa.creationDate = ParseIsoDate(Str(cd));
    }
    const char* md = JsonStr(ctx, o, "modDate");
    if (md && *md) {
        sa.modDate = ParseIsoDate(Str(md));
    }

    // stamp / attachment payload reference: strict shape check, the file
    // system is only touched for exact "assets/<16-hex>.<ext>" paths
    const char* asset = JsonStr(ctx, o, "asset");
    if (asset && *asset) {
        if (!IsSafeAssetRelPath(Str(asset))) {
            return false;
        }
        sa.assetName = str::Dup(Str(asset));
    }
    fz_json* att = JsonGet(ctx, o, "attachment");
    if (att && fz_json_is_object(ctx, att)) {
        const char* fn = JsonStr(ctx, att, "filename");
        if (fn && *fn) {
            sa.attachName = str::Dup(Str(fn));
        }
        const char* mime = JsonStr(ctx, att, "mimeType");
        if (mime && *mime) {
            sa.attachMime = str::Dup(Str(mime));
        }
        sa.attachSize = JsonInt(ctx, att, "size", -1);
        const char* ac = JsonStr(ctx, att, "created");
        if (ac && *ac) {
            sa.attachCreated = ParseIsoDate(Str(ac));
        }
        const char* am = JsonStr(ctx, att, "modified");
        if (am && *am) {
            sa.attachModified = ParseIsoDate(Str(am));
        }
    }
    // a file attachment without its payload is just a dead icon: drop it
    if (sa.type == AnnotationType::FileAttachment && len(sa.assetName) == 0) {
        return false;
    }

    // sanity per type: a markup without quads, a line without points, a
    // polygon without vertices and an ink without strokes are useless
    if (AnnotationIsTextMarkup(sa.type) && len(sa.quads) == 0) {
        return false;
    }
    if (sa.type == AnnotationType::Line && !sa.hasLine) {
        return false;
    }
    if ((sa.type == AnnotationType::Polygon || sa.type == AnnotationType::PolyLine) && len(sa.vertices) == 0) {
        return false;
    }
    if (sa.type == AnnotationType::Ink && len(sa.inkStrokes) == 0) {
        return false;
    }
    return true;
}

// parses a sidecar file; returns false on hard parse errors. An empty or
// annotation-less file parses fine with zero entries.
static bool ParseSidecarJson(fz_context* ctx, char* data, Vec<SidecarAnnot>& out) {
    fz_pool* pool = fz_new_pool(ctx);
    bool ok = false;
    fz_try(ctx) {
        fz_json* root = fz_parse_json(ctx, pool, data);
        if (!fz_json_is_object(ctx, root)) {
            fz_throw(ctx, FZ_ERROR_FORMAT, "sidecar: root is not a JSON object");
        }
        double vers = JsonNum(ctx, root, "version", 0);
        if (vers > kSidecarVersion) {
            logf("sidecar: file version %.1f > %d, reading anyway\n", vers, kSidecarVersion);
        }
        fz_json* arr = JsonArr(ctx, root, "annotations");
        if (arr) {
            int n = fz_json_array_length(ctx, arr);
            for (int i = 0; i < n; i++) {
                fz_json* o = fz_json_array_get(ctx, arr, i);
                if (!o || !fz_json_is_object(ctx, o)) {
                    continue;
                }
                SidecarAnnot sa;
                if (JsonToSidecarAnnot(ctx, o, sa)) {
                    VecAppend(out, std::move(sa));
                } else {
                    // strings may already have been dup'ed before the
                    // per-type sanity check rejected the entry
                    FreeSidecarAnnotStrings(ctx, sa);
                }
            }
        }
        ok = true;
    }
    fz_always(ctx) {
        fz_drop_pool(ctx, pool);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Markdown sidecar (Annotations->SeparateSaveAsMd = true)
//
// The same data as the JSON sidecar, stored as Markdown so it can live in
// an Obsidian vault: annotations become Obsidian-style callouts, everything
// else in the file belongs to the user. Structure:
//
//   ---                       front matter (created once, then verbatim)
//   sumatrapdf_sidecar: 2
//   generator: SumatraPDF-sidecar/2-md
//   file: report.pdf
//   ---
//   # user content ...        anything; preserved verbatim on save
//
//   > [!Note] optional title  one callout per annotation
//   > type: highlight         body lines "key: value" with a known key
//   > page: 11                are machine-owned, rewritten on save
//   > rect: [58.4,695.2,299.6,708.4]
//
// Semantics (the executable spec is md_sidecar_ref.py / test_md_sidecar.py):
// - a callout is a maximal run of '>' lines whose first starts with "> [!";
//   body lines matching key ':' with a known key are machine kv lines,
//   every other body line is user text kept verbatim
// - values are plain scalars (unquoted) or JSON literals (numbers,
//   "escaped strings", [arrays], {objects}); MdIsPlainScalar decides
// - import prefers .md and falls back to .json (read-only; the next manual
//   save writes the .md - automatic migration; the .json is never touched)
// - save merges: matched callouts are rewritten in place (engine data
//   wins over edits made to kv values in the editor), callouts of deleted
//   annotations are dropped, new annotations are appended at the end;
//   deleting a callout takes effect at the next document open (the reopen
//   is the sync point - it is the import that creates the annotations)
// - callouts are matched to live annotations by /NM first, then by
//   type+page+rect proximity
// - the write goes through a temp file + MoveFileExW so a crash mid-save
//   can never destroy the user's notes

// the keys SerializeAnnotJson emits; anything else in a callout body is
// user text
static const char* kMdKnownKeys[] = {
    "type",     "page",       "rect",     "quads",      "start",       "end",
    "vertices", "inkList",    "color",    "interiorColor", "opacity",  "borderWidth",
    "lineStart", "lineEnd",   "icon",     "asset",      "attachment",  "isOpen",
    "fontSize", "textColor",  "textAlign", "fontFamily", "textStyle",  "flags",
    "author",   "subject",    "name",     "text",       "contents",    "creationDate",
    "modDate",
};

static bool MdIsKnownKey(Str k) {
    for (int i = 0; i < dimofi(kMdKnownKeys); i++) {
        if (str::Eq(k, Str(kMdKnownKeys[i]))) {
            return true;
        }
    }
    return false;
}

static bool MdIsDigit(char c) {
    return c >= '0' && c <= '9';
}

static bool MdIsAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool MdIsAlnum(char c) {
    return MdIsDigit(c) || MdIsAlpha(c);
}

// true when s can be written unquoted and re-parsed as a string value:
// non-empty, no JSON-literal lookalike start, not exactly true/false/null,
// no '"' and no control characters (symmetric with AppendMdLiteral)
static bool MdIsPlainScalar(Str s) {
    if (len(s) == 0) {
        return false;
    }
    char c0 = s.s[0];
    if (c0 == '"' || c0 == '[' || c0 == '{' || c0 == '-' || c0 == '.' || MdIsDigit(c0)) {
        return false;
    }
    if (str::Eq(s, StrL("true")) || str::Eq(s, StrL("false")) || str::Eq(s, StrL("null"))) {
        return false;
    }
    for (int i = 0; i < len(s); i++) {
        if (s.s[i] == '"' || (u8)s.s[i] < 0x20) {
            return false;
        }
    }
    return true;
}

// a JSON-style string; UTF-8 stays raw so the file stays readable
static void AppendMdJsonString(str::Builder& b, Str s) {
    b.AppendChar('"');
    for (int i = 0; i < len(s); i++) {
        char c = s.s[i];
        if (c == '"' || c == '\\') {
            b.AppendChar('\\');
            b.AppendChar(c);
        } else if (c == '\n') {
            b.Append(StrL("\\n"));
        } else if (c == '\r') {
            b.Append(StrL("\\r"));
        } else if (c == '\t') {
            b.Append(StrL("\\t"));
        } else if ((u8)c < 0x20) {
            b.Append(fmt("\\u%04x", (int)(u8)c));
        } else {
            b.AppendChar(c);
        }
    }
    b.AppendChar('"');
}

// numbers keep the JSON serializer's two decimals, trailing zeros trimmed
// (58.40 -> 58.4, integral values stay integral). TempStr is a struct
// {char*,int}, so the C string functions go through CStrTemp's char*
static void AppendMdNumber(str::Builder& b, double v) {
    if (v == (double)(long long)v && v > -1e15 && v < 1e15) {
        b.Append(fmt("%lld", (long long)v));
        return;
    }
    char* z = CStrTemp(fmt("%.2f", v));
    char* dot = strchr(z, '.');
    if (!dot) {
        b.Append(Str(z));
        return;
    }
    char* endp = z + strlen(z) - 1;
    while (endp > dot && *endp == '0') {
        endp--;
    }
    if (endp > dot) {
        endp++; // keep the last non-zero digit
    }
    b.Append(Str(z, (int)(endp - z)));
}

// compact one-line JSON for arrays / objects (fz_json's layout is public)
static void AppendMdJsonCompact(str::Builder& b, const fz_json* v) {
    switch (v->type) {
        case FZ_JSON_NULL:
            b.Append(StrL("null"));
            break;
        case FZ_JSON_TRUE:
            b.Append(StrL("true"));
            break;
        case FZ_JSON_FALSE:
            b.Append(StrL("false"));
            break;
        case FZ_JSON_NUMBER:
            AppendMdNumber(b, v->u.number);
            break;
        case FZ_JSON_STRING:
            AppendMdJsonString(b, Str(v->u.string));
            break;
        case FZ_JSON_ARRAY:
            b.AppendChar('[');
            for (const fz_json_array* a = v->u.array; a; a = a->next) {
                if (a != v->u.array) {
                    b.AppendChar(',');
                }
                AppendMdJsonCompact(b, a->value);
            }
            b.AppendChar(']');
            break;
        case FZ_JSON_OBJECT:
            b.AppendChar('{');
            for (const fz_json_object* kv = v->u.object; kv; kv = kv->next) {
                if (kv != v->u.object) {
                    b.AppendChar(',');
                }
                AppendMdJsonString(b, Str(kv->key));
                b.AppendChar(':');
                AppendMdJsonCompact(b, kv->value);
            }
            b.AppendChar('}');
            break;
    }
}

// the "> key: value" body of a callout for one annotation. Fields come
// from SerializeAnnotJson via a JSON round-trip so the two formats can
// never drift apart: one place (the JSON serializer) defines them.
//
// MD-only nicety: the contents line is ALWAYS present so the file can be
// annotated by hand in Obsidian. When the PDF annotation has no note the
// line is filled from the markup's text (the highlighted words); when
// there is no text either it is left empty. The value flows back on the
// next import (empty never sets anything on the PDF side).
static void AppendMdContentsLine(str::Builder& b, Str fill, Str eol) {
    b.Append(StrL("> contents: "));
    if (len(fill) > 0) {
        if (MdIsPlainScalar(fill)) {
            b.Append(fill);
        } else {
            AppendMdJsonString(b, fill);
        }
    } else {
        b.Append(StrL("\"\"")); // explicit empty: ready for a hand-written note
    }
    b.Append(eol);
}

static void AppendAnnotMdLines(str::Builder& b, fz_context* ctx, const SidecarAnnot& sa, bool crlf) {
    str::Builder jb;
    SerializeAnnotJson(jb, sa);
    Str js = jb.TakeStr();
    Str eol = crlf ? StrL("\r\n") : StrL("\n");
    fz_pool* pool = fz_new_pool(ctx);
    fz_try(ctx) {
        fz_json* o = fz_parse_json(ctx, pool, CStrTemp(js));
        if (o && o->type == FZ_JSON_OBJECT) {
            // pass 1: does a non-empty contents exist, and what is text?
            bool hasContents = false;
            Str textFill; // view into the DOM; the pool outlives this loop
            for (const fz_json_object* kv = o->u.object; kv; kv = kv->next) {
                if (kv->value->type == FZ_JSON_STRING) {
                    Str key = Str(kv->key);
                    Str val = Str(kv->value->u.string);
                    if (str::Eq(key, StrL("contents")) && len(val) > 0) {
                        hasContents = true;
                    } else if (str::Eq(key, StrL("text")) && len(val) > 0) {
                        textFill = val;
                    }
                }
            }
            // pass 2: emit; the contents fill goes where the JSON would
            // have it (right before the dates, which follow text/contents
            // in SerializeAnnotJson's field order), or after the last field
            bool contentsEmitted = hasContents;
            for (const fz_json_object* kv = o->u.object; kv; kv = kv->next) {
                if (!contentsEmitted) {
                    Str key = Str(kv->key);
                    if (str::Eq(key, StrL("creationDate")) || str::Eq(key, StrL("modDate"))) {
                        AppendMdContentsLine(b, textFill, eol);
                        contentsEmitted = true;
                    }
                }
                b.Append(StrL("> "));
                b.Append(Str(kv->key));
                b.Append(StrL(": "));
                if (kv->value->type == FZ_JSON_STRING && MdIsPlainScalar(Str(kv->value->u.string))) {
                    b.Append(Str(kv->value->u.string));
                } else {
                    AppendMdJsonCompact(b, kv->value);
                }
                b.Append(eol);
            }
            if (!contentsEmitted) {
                AppendMdContentsLine(b, textFill, eol);
            }
        }
    }
    fz_always(ctx) {
        fz_drop_pool(ctx, pool);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
    }
    str::Free(js);
}

// output wrapper tracking the last few bytes written, so the merge can
// look at the tail (blank-line separation) without reaching into
// str::Builder internals
struct MdOut {
    str::Builder b;
    char tail[4] = {};
    bool any = false;

    void Append(Str s) {
        if (len(s) == 0) {
            return;
        }
        b.Append(s);
        any = true;
        int n = (int)len(s);
        if (n >= 4) {
            memcpy(tail, s.s + n - 4, 4);
        } else {
            memmove(tail, tail + n, 4 - n);
            memcpy(tail + 4 - n, s.s, n);
        }
    }

    bool EndsWith(const char* suf, int sufLen) {
        if (!any || sufLen > 4) {
            return false;
        }
        return 0 == memcmp(tail + (4 - sufLen), suf, (size_t)sufLen);
    }
};

// separate a callout from whatever precedes it with one blank line
// (mirrors the reference implementation: file start / heading, blank,
// callout)
static void MdEnsureBlankBefore(MdOut& o, bool crlf) {
    if (!o.any) {
        return;
    }
    if (crlf) {
        if (!o.EndsWith("\r\n", 2)) {
            o.Append(StrL("\r\n"));
        }
        if (!o.EndsWith("\r\n\r\n", 4)) {
            o.Append(StrL("\r\n"));
        }
    } else {
        if (!o.EndsWith("\n", 1)) {
            o.Append(StrL("\n"));
        }
        if (!o.EndsWith("\n\n", 2)) {
            o.Append(StrL("\n"));
        }
    }
}

struct MdBodyLine {
    Str line;    // owned, the full "> ..." line without eol
    bool kv = false;
    Str key;     // owned when kv
    Str val;     // owned when kv
};

struct MdSeg {
    bool isCallout = false;
    Str text;   // user segment: verbatim bytes including eols (owned)
    Str header; // callout header line without eol (owned)
    Vec<MdBodyLine> body;
    bool isAnnot = false; // callout parsed to a valid annotation
    SidecarAnnot annot;  // valid when isAnnot (strings owned)
};

struct MdDoc {
    bool hasFm = false;
    Str fm; // owned, verbatim including both --- lines
    Vec<MdSeg> segs;
    bool crlf = false;
};

static void MdFreeDoc(fz_context* ctx, MdDoc& doc) {
    str::Free(doc.fm);
    for (MdSeg& seg : doc.segs) {
        str::Free(seg.text);
        str::Free(seg.header);
        for (MdBodyLine& bl : seg.body) {
            str::Free(bl.line);
            if (bl.kv) {
                str::Free(bl.key);
                str::Free(bl.val);
            }
        }
        if (seg.isAnnot) {
            FreeSidecarAnnotStrings(ctx, seg.annot);
        }
    }
}

static Str MdDupLineNoEol(const char* start, const char* endp) {
    while (endp > start && (endp[-1] == '\n' || endp[-1] == '\r')) {
        endp--;
    }
    return str::Dup(Str(start, (int)(endp - start)));
}

static bool MdLineIsFmDelim(const char* start, const char* endp) {
    // line.strip() == '---'
    while (endp > start && (endp[-1] == '\n' || endp[-1] == '\r' || endp[-1] == ' ' || endp[-1] == '\t')) {
        endp--;
    }
    while (start < endp && (*start == ' ' || *start == '\t')) {
        start++;
    }
    return (endp - start == 3) && start[0] == '-' && start[1] == '-' && start[2] == '-';
}

// classify "key: value" / "key:: value" body lines (known keys only; the
// lenient double colon accepts dataview-style hand edits)
static bool MdParseKvLine(Str inner, Str& key, Str& val) {
    int n = (int)len(inner);
    if (n < 1 || !MdIsAlpha(inner.s[0])) {
        return false;
    }
    int i = 1;
    while (i < n && MdIsAlnum(inner.s[i])) {
        i++;
    }
    Str k = Str(inner.s, i);
    if (!MdIsKnownKey(k)) {
        return false;
    }
    if (i >= n || inner.s[i] != ':') {
        return false;
    }
    i++;
    if (i < n && inner.s[i] == ':') {
        i++;
    }
    if (i < n && inner.s[i] == ' ') {
        i++;
    }
    int endp = n;
    while (endp > i && (inner.s[endp - 1] == ' ' || inner.s[endp - 1] == '\t')) {
        endp--;
    }
    key = str::Dup(k);
    val = str::Dup(Str(inner.s + i, endp - i));
    return true;
}

// value -> JSON literal for the synthesized callout object; a malformed
// literal (e.g. a number with garbage after it) fails the whole callout
// parse, which demotes the callout to user content
static void AppendMdLiteral(str::Builder& b, Str v) {
    int e = len(v);
    while (e > 0 && (v.s[e - 1] == ' ' || v.s[e - 1] == '\t' || v.s[e - 1] == '\r')) {
        e--;
    }
    Str s = Str(v.s, e);
    if (len(s) == 0) {
        b.Append(StrL("\"\""));
        return;
    }
    char c0 = s.s[0];
    if (c0 == '"' || c0 == '[' || c0 == '{' || MdIsDigit(c0) || c0 == '-' || c0 == '.') {
        b.Append(s);
        return;
    }
    if (str::Eq(s, StrL("true")) || str::Eq(s, StrL("false")) || str::Eq(s, StrL("null"))) {
        b.Append(s);
        return;
    }
    AppendMdJsonString(b, s);
}

static void MdParseDoc(fz_context* ctx, const char* data, MdDoc& doc) {
    doc.crlf = strstr(data, "\r\n") != nullptr;
    const char* end = data + strlen(data);

    const char* ls = data; // line start
    const char* le = data; // line content end (before the eol)
    const char* ne = data; // next line start

    // optional front matter: '---' line closed by another '---' within
    // the next 50 lines; unterminated '---' is not front matter
    le = ls;
    while (le < end && *le != '\n') {
        le++;
    }
    ne = (le < end) ? le + 1 : le;
    if (MdLineIsFmDelim(ls, le)) {
        const char* fmStart = ls;
        const char* fmEnd = nullptr;
        int n = 0;
        const char* l2 = ne;
        while (l2 < end && n < 50) {
            const char* l2e = l2;
            while (l2e < end && *l2e != '\n') {
                l2e++;
            }
            const char* l2n = (l2e < end) ? l2e + 1 : l2e;
            n++;
            if (MdLineIsFmDelim(l2, l2e)) {
                fmEnd = l2n;
                break;
            }
            l2 = l2n;
        }
        if (fmEnd) {
            doc.hasFm = true;
            doc.fm = str::Dup(Str(fmStart, (int)(fmEnd - fmStart)));
            ls = fmEnd;
        }
    }

    const char* textStart = nullptr;
    while (ls < end) {
        le = ls;
        while (le < end && *le != '\n') {
            le++;
        }
        ne = (le < end) ? le + 1 : le;

        bool calloutStart = (le - ls >= 4) && ls[0] == '>' && ls[1] == ' ' && ls[2] == '[' && ls[3] == '!';
        if (calloutStart) {
            if (textStart && textStart < ls) {
                MdSeg seg;
                seg.text = str::Dup(Str(textStart, (int)(ls - textStart)));
                VecAppend(doc.segs, std::move(seg));
            }
            textStart = nullptr;
            MdSeg seg;
            seg.isCallout = true;
            seg.header = MdDupLineNoEol(ls, le);
            ls = ne;
            while (ls < end) {
                const char* bls = ls;
                const char* ble = bls;
                while (ble < end && *ble != '\n') {
                    ble++;
                }
                const char* bne = (ble < end) ? ble + 1 : ble;
                if (ble == bls || bls[0] != '>') {
                    break; // callout ends at the first non-'>' line
                }
                const char* is = bls + 1;
                if (is < ble && *is == ' ') {
                    is++;
                }
                MdBodyLine bl;
                bl.line = MdDupLineNoEol(bls, ble);
                if (MdParseKvLine(Str(is, (int)(ble - is)), bl.key, bl.val)) {
                    bl.kv = true;
                }
                VecAppend(seg.body, std::move(bl));
                ls = bne;
            }
            VecAppend(doc.segs, std::move(seg));
            textStart = (ls < end) ? ls : nullptr;
        } else {
            if (!textStart) {
                textStart = ls;
            }
            ls = ne;
        }
    }
    if (textStart && textStart < end) {
        MdSeg seg;
        seg.text = str::Dup(Str(textStart, (int)(end - textStart)));
        VecAppend(doc.segs, std::move(seg));
    }

    // parse valid callouts into annotations through the JSON importer
    // (synthesizing a JSON object from the kv lines reuses
    // JsonToSidecarAnnot's field semantics verbatim)
    for (MdSeg& seg : doc.segs) {
        if (!seg.isCallout) {
            continue;
        }
        bool anyKv = false;
        str::Builder jb;
        jb.AppendChar('{');
        for (MdBodyLine& bl : seg.body) {
            if (!bl.kv) {
                continue;
            }
            if (anyKv) {
                jb.AppendChar(',');
            }
            anyKv = true;
            AppendMdJsonString(jb, bl.key);
            jb.AppendChar(':');
            AppendMdLiteral(jb, bl.val);
        }
        if (!anyKv) {
            continue; // pure user callout
        }
        jb.AppendChar('}');
        Str js = jb.TakeStr();
        fz_pool* pool = fz_new_pool(ctx);
        fz_try(ctx) {
            fz_json* o = fz_parse_json(ctx, pool, CStrTemp(js));
            SidecarAnnot sa;
            if (o && fz_json_is_object(ctx, o) && JsonToSidecarAnnot(ctx, o, sa)) {
                seg.isAnnot = true;
                seg.annot = std::move(sa);
            } else {
                // may have dup'ed strings before the sanity check failed
                FreeSidecarAnnotStrings(ctx, sa);
            }
        }
        fz_always(ctx) {
            fz_drop_pool(ctx, pool);
        }
        fz_catch(ctx) {
            // a value that looks like a literal but does not parse: the
            // callout is demoted to user content, never dropped
            fz_report_error(ctx);
        }
        str::Free(js);
    }

    if (doc.hasFm) {
        const char* v = strstr(doc.fm.s, "sumatrapdf_sidecar:");
        if (v) {
            v += strlen("sumatrapdf_sidecar:");
            while (*v == ' ') {
                v++;
            }
            int ver = atoi(v);
            if (ver > kSidecarVersion) {
                logf("sidecar: file version %d > %d, reading anyway\n", ver, kSidecarVersion);
            }
        }
    }
}

// import a .md sidecar: parse, hand the valid annotations to the caller
static bool ParseSidecarMd(fz_context* ctx, char* data, Vec<SidecarAnnot>& out) {
    MdDoc doc;
    MdParseDoc(ctx, data, doc);
    for (MdSeg& seg : doc.segs) {
        if (seg.isCallout && seg.isAnnot) {
            VecAppend(out, std::move(seg.annot));
            // the strings moved with the struct; keep MdFreeDoc away from
            // the moved-from copy
            seg.isAnnot = false;
        }
    }
    MdFreeDoc(ctx, doc);
    return true;
}

// callout <-> live-entry proximity for the merge: same type and page,
// centers within 4pt and dimensions within 3pt. Looser than PageHasAnnot
// because the file may have been reformatted by an editor (or the entry
// nudged by hand) in between
static bool MdNearAnnot(const SidecarAnnot& a, const SidecarAnnot& b) {
    if (a.type != b.type || a.pageNo != b.pageNo) {
        return false;
    }
    RectF ra = a.bounds, rb = b.bounds;
    float cax = ra.x + ra.dx / 2, cay = ra.y + ra.dy / 2;
    float cbx = rb.x + rb.dx / 2, cby = rb.y + rb.dy / 2;
    if (fabsf(cax - cbx) > 4 || fabsf(cay - cby) > 4) {
        return false;
    }
    if (fabsf(ra.dx - rb.dx) > 3 || fabsf(ra.dy - rb.dy) > 3) {
        return false;
    }
    return true;
}

// res[i] = index of the callout matched to entry i, or -1
static void MdMatchEntries(const Vec<SidecarAnnot>& entries, const MdDoc& doc, Vec<int>& res) {
    for (int i = 0; i < len(entries); i++) {
        VecAppend(res, -1);
    }
    Vec<bool> used;
    for (int i = 0; i < len(doc.segs); i++) {
        VecAppend(used, false);
    }
    // pass 1: exact /NM
    for (int ei = 0; ei < len(entries); ei++) {
        if (len(entries[ei].name) == 0) {
            continue;
        }
        for (int si = 0; si < len(doc.segs); si++) {
            if (used[si] || !doc.segs[si].isAnnot) {
                continue;
            }
            if (str::Eq(doc.segs[si].annot.name, entries[ei].name)) {
                res[ei] = si;
                used[si] = true;
                break;
            }
        }
    }
    // pass 2: proximity
    for (int ei = 0; ei < len(entries); ei++) {
        if (res[ei] >= 0) {
            continue;
        }
        for (int si = 0; si < len(doc.segs); si++) {
            if (used[si] || !doc.segs[si].isAnnot) {
                continue;
            }
            if (MdNearAnnot(entries[ei], doc.segs[si].annot)) {
                res[ei] = si;
                used[si] = true;
                break;
            }
        }
    }
}

// write through a same-directory temp file + MoveFileExW(REPLACE_EXISTING):
// a Markdown sidecar also carries the user's own notes, so a crash or
// power loss mid-write must never leave a half-written file behind
// (pattern: PngOptimizer.cpp). Falls back to a plain write when the
// atomic replace is refused (unusual filesystems); the temp name carries
// the pid so concurrent processes never fight over one temp file
static bool WriteSidecarFileAtomic(Str path, Str data) {
    TempStr dir = path::GetDirTemp(path);
    TempStr base = path::GetBaseNameTemp(path);
    if (len(dir) == 0 || len(base) == 0) {
        return file::WriteFile(path, data);
    }
    str::Builder b;
    b.Append(dir);
    b.AppendChar(PathSepFor(Str(dir)));
    b.AppendChar('.');
    b.Append(Str(base));
    b.Append(fmt(".tmp%d", (int)GetCurrentProcessId()));
    Str tmpPath = b.TakeStr();
    bool ok = file::WriteFile(tmpPath, data);
    if (ok) {
        if (MoveFileExW(CWStrTemp(tmpPath), CWStrTemp(path), MOVEFILE_REPLACE_EXISTING)) {
            str::Free(tmpPath);
            return true;
        }
        logf("sidecar: atomic replace failed (err=%u), falling back to plain write\n", GetLastError());
        ok = file::WriteFile(path, data);
    }
    file::Delete(tmpPath);
    str::Free(tmpPath);
    return ok;
}

// build the merged .md for the tab's document: existing file parsed and
// preserved (front matter, user text, callout headers and user body lines),
// matched callouts rewritten from the live entries, deleted annotations'
// callouts dropped, new entries appended
static Str BuildSidecarMd(EngineMupdf* e, Str pdfPath, const Vec<SidecarAnnot>& entries, Str existingMd) {
    fz_context* ctx = e->Ctx();
    MdDoc doc;
    if (len(existingMd) > 0) {
        char* z = CStrTemp(existingMd);
        MdParseDoc(ctx, z, doc);
    }
    Vec<int> res;
    MdMatchEntries(entries, doc, res);

    bool crlf = doc.crlf;
    Str eol = crlf ? StrL("\r\n") : StrL("\n");
    MdOut out;
    if (doc.hasFm) {
        out.Append(doc.fm);
    } else {
        str::Builder fb;
        fb.Append(StrL("---"));
        fb.Append(eol);
        fb.Append(fmt("sumatrapdf_sidecar: %d", kSidecarVersion));
        fb.Append(eol);
        fb.Append(StrL("generator: SumatraPDF-sidecar/2-md"));
        fb.Append(eol);
        TempStr base = path::GetBaseNameTemp(pdfPath);
        if (base && len(base) > 0) {
            fb.Append(StrL("file: "));
            fb.Append(Str(base));
            fb.Append(eol);
        }
        Str title = DocTitle(e);
        if (len(title) > 0) {
            fb.Append(StrL("title: "));
            fb.Append(title);
            fb.Append(eol);
            str::Free(title);
        }
        fb.Append(StrL("---"));
        fb.Append(eol);
        out.Append(fb.TakeStr());
    }

    Vec<int> segToEntry;
    for (int i = 0; i < len(doc.segs); i++) {
        VecAppend(segToEntry, -1);
    }
    for (int ei = 0; ei < len(res); ei++) {
        if (res[ei] >= 0) {
            segToEntry[res[ei]] = ei;
        }
    }

    for (int si = 0; si < len(doc.segs); si++) {
        MdSeg& seg = doc.segs[si];
        if (!seg.isCallout) {
            out.Append(seg.text);
            continue;
        }
        int ei = segToEntry[si];
        if (ei >= 0) {
            // rewrite this callout in place from the live entry; the
            // header line and user body lines survive untouched
            MdEnsureBlankBefore(out, crlf);
            out.Append(seg.header);
            out.Append(eol);
            str::Builder kb;
            AppendAnnotMdLines(kb, ctx, entries[ei], crlf);
            out.Append(kb.TakeStr());
            for (MdBodyLine& bl : seg.body) {
                if (!bl.kv) {
                    out.Append(bl.line);
                    out.Append(eol);
                }
            }
        } else if (seg.isAnnot) {
            // the annotation is gone: drop the callout
        } else {
            // user-content callout: preserved verbatim
            MdEnsureBlankBefore(out, crlf);
            out.Append(seg.header);
            out.Append(eol);
            for (MdBodyLine& bl : seg.body) {
                out.Append(bl.line);
                out.Append(eol);
            }
        }
    }
    for (int ei = 0; ei < len(entries); ei++) {
        if (res[ei] < 0) {
            // new annotation: append at the end
            MdEnsureBlankBefore(out, crlf);
            out.Append(StrL("> [!Note]"));
            out.Append(eol);
            str::Builder kb;
            AppendAnnotMdLines(kb, ctx, entries[ei], crlf);
            out.Append(kb.TakeStr());
        }
    }
    if (out.any && !out.EndsWith(crlf ? "\r\n" : "\n", crlf ? 2 : 1)) {
        out.Append(eol);
    }
    MdFreeDoc(ctx, doc);
    return out.b.TakeStr();
}

// ---------------------------------------------------------------------------
// import (creates PDF annotations in the freshly loaded engine)

// returns true if the page already contains this annotation: exact /NM
// match first, then the type+rect heuristic (prevents duplicates when the
// PDF embeds annotations that also exist in the sidecar)
//
// CRASH FIX: this whole scan must be exception-safe.
//  - The raw pdf_annot_rect getter THROWS "argument error: ... have no
//    Rect property" for derived-rect types (text markups / Ink / Line /
//    Polygon) in this mupdf, and there is NO enclosing fz_try between
//    here and the document-open code: an escaping longjmp would skip the
//    C++ docLock guard in the caller and take the process down (observed:
//    crash when importing the 2nd sidecar annotation of the same markup
//    type, e.g. the second squiggly).
//  - PdfAnnotBounds (pdf_bound_annot, incl. the ink-stroke special case)
//    reads /Rect through the display-rect path with no subtype whitelist
//    and is exactly what the exporter measured for "rect", so the
//    heuristic now compares like with like (the old pdf_annot_rect also
//    subtracted /RD, which the exported bounds never did).
static bool PageHasAnnot(fz_context* ctx, pdf_page* page, const SidecarAnnot& a) {
    bool found = false;
    fz_try(ctx) {
        pdf_annot* pa = pdf_first_annot(ctx, page);
        if (len(a.name) > 0) {
            while (pa) {
                const char* nm = pdf_annot_name(ctx, pa);
                if (nm && str::Eq(Str(nm), a.name)) {
                    found = true;
                    break;
                }
                pa = pdf_next_annot(ctx, pa);
            }
            if (!found) {
                pa = pdf_first_annot(ctx, page);
            }
        }
        while (!found && pa) {
            auto pt = pdf_annot_type(ctx, pa);
            if ((AnnotationType)pt == a.type) {
                RectF rb = PdfAnnotBounds(ctx, pa);
                if (fabsf(rb.x - a.bounds.x) < 1.0f && fabsf(rb.y - a.bounds.y) < 1.0f &&
                    fabsf((rb.x + rb.dx) - (a.bounds.x + a.bounds.dx)) < 1.0f &&
                    fabsf((rb.y + rb.dy) - (a.bounds.y + a.bounds.dy)) < 1.0f) {
                    found = true;
                    break;
                }
            }
            pa = pdf_next_annot(ctx, pa);
        }
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logf("sidecar: dedup scan failed on page %d, importing anyway\n", a.pageNo);
        found = false;
    }
    return found;
}

// returns a kept pdf_annot* for MakeAnnotationWrapper, or nullptr on failure
// (a half-created annotation is deleted from the page again)
static pdf_annot* CreateAnnotFromEntry(fz_context* ctx, pdf_page* page, const SidecarAnnot& a) {
    pdf_annot* pa = nullptr;
    fz_var(pa);
    fz_try(ctx) {
        pa = pdf_create_annot(ctx, page, (enum pdf_annot_type)a.type);
        if (!pa) {
            return nullptr;
        }
        // pdf_set_annot_rect rejects annotations whose /Rect is derived
        // from other data (QuadPoints / Vertices / InkList / Line
        // endpoints, mupdf's rect_subtypes whitelist) with
        // "argument error: ... annotations have no Rect property";
        // mirror the official creation code (EngineMupdfCreateAnnotation)
        // and only set the rect on types that own it. The rest get their
        // /Rect derived from their geometry setters below.
        if (!a.bounds.IsEmpty() && SidecarAnnotOwnsRect(a.type)) {
            pdf_set_annot_rect(ctx, pa, ToFzRect(a.bounds));
        }
        if (len(a.quads) > 0 && (AnnotationIsTextMarkup(a.type) || a.type == AnnotationType::Redact)) {
            int n = (int)len(a.quads);
            if (n > kMaxQuads) {
                n = kMaxQuads;
            }
            fz_quad quads[kMaxQuads];
            for (int i = 0; i < n; i++) {
                quads[i] = a.quads[i];
            }
            pdf_set_annot_quad_points(ctx, pa, n, quads);
        }
        if (a.type == AnnotationType::Line) {
            if (a.hasLine) {
                pdf_set_annot_line(ctx, pa, fz_make_point(a.lineA.x, a.lineA.y), fz_make_point(a.lineB.x, a.lineB.y));
            }
            if (a.lineStart != 0 || a.lineEnd != 0) {
                pdf_set_annot_line_ending_styles(ctx, pa, (pdf_line_ending)a.lineStart, (pdf_line_ending)a.lineEnd);
            }
        }
        if ((a.type == AnnotationType::Polygon || a.type == AnnotationType::PolyLine) && len(a.vertices) > 0) {
            int n = (int)len(a.vertices);
            if (n > kMaxVertices) {
                n = kMaxVertices;
            }
            fz_point verts[kMaxVertices];
            for (int i = 0; i < n; i++) {
                verts[i] = fz_make_point(a.vertices[i].x, a.vertices[i].y);
            }
            pdf_set_annot_vertices(ctx, pa, n, verts);
        }
        if (a.type == AnnotationType::Ink && len(a.inkStrokes) > 0) {
            // counts must match the points actually emitted: when the caps
            // truncate, mupdf must not read past the array (this fixes a
            // latent bug that existed in the XFDF version of this code)
            int counts[kMaxInkStrokes];
            fz_point pts[kMaxInkPoints];
            int nStrokes = (int)len(a.inkStrokes);
            if (nStrokes > kMaxInkStrokes) {
                nStrokes = kMaxInkStrokes;
            }
            int nPts = 0;
            for (int i = 0; i < nStrokes; i++) {
                const Vec<PointF>& stroke = a.inkStrokes[i];
                int kept = 0;
                for (int j = 0; j < len(stroke) && nPts < kMaxInkPoints; j++) {
                    pts[nPts++] = fz_make_point(stroke[j].x, stroke[j].y);
                    kept++;
                }
                counts[i] = kept;
            }
            pdf_set_annot_ink_list(ctx, pa, nStrokes, counts, pts);
        }
        if (a.hasColor && AnnotationSupportsColor(a.type)) {
            pdf_set_annot_color(ctx, pa, 3, a.color);
        }
        if (a.hasInterior && AnnotationSupportsInteriorColor(a.type)) {
            pdf_set_annot_interior_color(ctx, pa, 3, a.interiorCol);
        }
        if (a.opacity < 0.999f && AnnotationSupportsOpacity(a.type)) {
            pdf_set_annot_opacity(ctx, pa, a.opacity);
        }
        if (len(a.contents) > 0) {
            // CStrTemp allocates from the temp arena: mupdf copies the bytes,
            // the arena reclaims the memory on its own
            pdf_set_annot_contents(ctx, pa, CStrTemp(a.contents));
        }
        if (len(a.author) > 0) {
            pdf_set_annot_author(ctx, pa, CStrTemp(a.author));
        }
        if (len(a.subject) > 0) {
            pdf_set_annot_subject(ctx, pa, CStrTemp(a.subject));
        }
        if (len(a.name) > 0) {
            pdf_set_annot_name(ctx, pa, CStrTemp(a.name));
        }
        if (a.creationDate > 0) {
            pdf_set_annot_creation_date(ctx, pa, (int64_t)a.creationDate);
        }
        if (a.modDate > 0) {
            pdf_set_annot_modification_date(ctx, pa, (int64_t)a.modDate);
        }
        if (a.flags >= 0) {
            pdf_set_annot_flags(ctx, pa, a.flags);
        }
        if (a.type == AnnotationType::Text || a.type == AnnotationType::Stamp ||
            a.type == AnnotationType::FileAttachment) {
            // icon: text note icons, stamp names ("Approved" ...), file
            // attachment icons ("PushPin", "Graph" ...) all live in /Name
            if (len(a.icon) > 0) {
                pdf_set_annot_icon_name(ctx, pa, CStrTemp(a.icon));
            }
        }
        if (a.type == AnnotationType::Text && a.isOpen) {
            pdf_set_annot_is_open(ctx, pa, 1);
        }
        if (a.type == AnnotationType::Stamp && a.assetBuf) {
            // image stamp: embed the image and let mupdf rebuild /AP from
            // it (pdf_set_annot_stamp_image_obj resets the appearance);
            // JPEG bytes stay compressed, PNG decodes to a pixmap
            fz_image* img = fz_new_image_from_buffer(ctx, a.assetBuf);
            pdf_set_annot_stamp_image(ctx, pa, img);
            fz_drop_image(ctx, img);
        }
        if (a.type == AnnotationType::FileAttachment && a.assetBuf) {
            // recreate the embedded file exactly like the app's own
            // "attach file" code (Annotation.cpp): the filespec keeps the
            // original name / MIME type / dates, the buffer is copied by
            // mupdf so the entry keeps owning its bytes
            Str fn = a.attachName;
            if (len(fn) == 0) {
                fn = StrL("attachment.bin");
            }
            pdf_obj* fs = pdf_add_embedded_file(ctx, page->doc, CStrTemp(fn),
                                                len(a.attachMime) > 0 ? CStrTemp(a.attachMime) : nullptr, a.assetBuf,
                                                (int64_t)a.attachCreated, (int64_t)a.attachModified, 1);
            pdf_set_annot_filespec(ctx, pa, fs);
            pdf_drop_obj(ctx, fs);
        }
        if (a.type == AnnotationType::FreeText) {
            // note: font family is not preserved (Helvetica fallback);
            // size/color/alignment are
            if (a.textSize > 0) {
                const float black[3] = {0, 0, 0};
                pdf_set_annot_default_appearance(ctx, pa, "Helvetica", (float)a.textSize, 3,
                                                 a.hasTextCol ? a.textColor : black);
            }
            if (a.quadding >= 0) {
                pdf_set_annot_quadding(ctx, pa, a.quadding);
            }
            // rich text style (/DS): MUST be written after the /DA block
            // above - pdf_set_annot_default_appearance() deletes /DS as
            // "not supported" (pdf-annot.c). This is also why the
            // sidebar's SetFreeTextFont() cannot be used from the import
            // loop: it is guarded by AnnotationIsLive(), which requires
            // the annotation to already be in the engine's page list, and
            // the wrapper is only registered by MarkNotificationAsModified
            // (Add) after this loop - the write has to happen right here
            // at the mupdf level, mirroring WriteFreeTextFontLocked.
            if (len(a.fontFamily) > 0 || a.fontStyle != 0) {
                const char* daFont = nullptr;
                float daSize = 0;
                int nCol = 0;
                float daColor[4]{};
                pdf_annot_default_appearance(ctx, pa, &daFont, &daSize, &nCol, daColor);
                int r = 0, g = 0, b = 0;
                if (nCol >= 3) {
                    r = (int)(daColor[0] * 255.0f + 0.5f);
                    g = (int)(daColor[1] * 255.0f + 0.5f);
                    b = (int)(daColor[2] * 255.0f + 0.5f);
                } else if (nCol == 1) {
                    r = g = b = (int)(daColor[0] * 255.0f + 0.5f);
                }
                if (daSize <= 0) {
                    daSize = 12;
                }
                int q = (a.quadding >= 0 && a.quadding <= 2) ? a.quadding : 0;
                Str align = (q == 1) ? StrL("center") : (q == 2) ? StrL("right") : StrL("left");
                TempStr cssFam = str::DupTemp(a.fontFamily);
                if (len(cssFam) == 0) {
                    cssFam = str::DupTemp(StrL("Helvetica"));
                }
                cssFam.len -= str::RemoveCharsInPlace(cssFam, StrL("'\";{}"));
                str::Builder ds;
                ds.Append(fmt("font-family:'%s';font-size:%gpt;color:#%02x%02x%02x;text-align:%s", cssFam, daSize, r, g,
                              b, align));
                if (a.fontStyle & kFreeTextBold) {
                    ds.Append(StrL(";font-weight:bold"));
                }
                if (a.fontStyle & kFreeTextItalic) {
                    ds.Append(StrL(";font-style:italic"));
                }
                if (a.fontStyle & kFreeTextUnderline) {
                    ds.Append(StrL(";text-decoration:underline"));
                }
                pdf_set_annot_rich_defaults(ctx, pa, CStrTemp(ToStrTemp(ds)));
            }
        }
        if (a.borderWidth > 0 && AnnotationSupportsBorder(a.type)) {
            pdf_set_annot_border_width(ctx, pa, (float)a.borderWidth);
        }
        pdf_update_annot(ctx, pa);
    }
    fz_catch(ctx) {
        fz_report_error(ctx);
        logf("sidecar: failed to create annotation on page %d\n", a.pageNo);
        if (pa) {
            if (page) {
                pdf_delete_annot(ctx, page, pa);
            }
            pdf_drop_annot(ctx, pa);
            pa = nullptr;
        }
    }
    return pa;
}

// imports entries; returns the number of created annotations
static int ImportSidecarEntries(EngineMupdf* e, const Vec<SidecarAnnot>& entries) {
    fz_context* ctx = e->Ctx();
    int imported = 0;
    // stable order: sort entry indices by page so each page is visited once
    Vec<int> order;
    for (int i = 0; i < len(entries); i++) {
        VecAppend(order, i);
    }
    std::stable_sort(order.begin(), order.end(),
                     [&entries](int a, int b) { return entries[a].pageNo < entries[b].pageNo; });
    int prevPage = -1;
    FzPageInfo* pi = nullptr;
    for (int ix : order) {
        const SidecarAnnot& a = entries[ix];
        // payload (stamp image / attachment file) unreadable: importing a
        // hollow annotation would silently lose the content
        if (!a.assetOk) {
            continue;
        }
        // GetFzPageInfo takes pagesLock: must run OUTSIDE docLock
        if (a.pageNo != prevPage) {
            pi = e->GetFzPageInfo(a.pageNo, true);
            prevPage = a.pageNo;
        }
        if (!pi || !pi->page) {
            continue;
        }
        pdf_page* page = pdf_page_from_fz_page(ctx, pi->page);
        if (!page) {
            continue;
        }
        pdf_annot* pa = nullptr;
        {
            // annotation mutations are document-scope mupdf operations: docLock
            AutoUnlockRecursiveMutex cs(&e->docLock);
            if (PageHasAnnot(ctx, page, a)) {
                logf("sidecar: skipping duplicate annot on page %d\n", a.pageNo);
                continue;
            }
            pa = CreateAnnotFromEntry(ctx, page, a);
        }
        if (!pa) {
            continue;
        }
        Annotation* wa = MakeAnnotationWrapper(e, pa, a.pageNo);
        if (!wa) {
            AutoUnlockRecursiveMutex cs(&e->docLock);
            pdf_delete_annot(ctx, page, pa);
            pdf_drop_annot(ctx, pa);
            continue;
        }
        // (FreeText styling is written inside CreateAnnotFromEntry at the
        // mupdf level - NOT here via SetFreeTextFont(): that helper is
        // guarded by AnnotationIsLive(), which is false until
        // MarkNotificationAsModified(Add) below registers this wrapper in
        // the engine's page list, so calling it here was a silent no-op and
        // the /DS never landed. Fixed by writing /DS directly.)
        // the engine's own bookkeeping: appends to pageInfo->annotations and
        // rebuilds the page comments, exactly like a user-created annotation
        MarkNotificationAsModified(e, wa, AnnotationChange::Add);
        imported++;
    }
    // the import mirrors the JSON on disk, it is not an unsaved change
    e->modifiedAnnotations = false;
    return imported;
}

// ---------------------------------------------------------------------------
// public API

// ---------------------------------------------------------------------------
// external sidecar changes: fingerprint + poll-reload
//
// The sidecar is a two-way source of truth: we write it after annotation
// changes AND other devices (OneDrive sync, another viewer) may modify it
// while the document is open. Without watching, the next auto-save would
// blindly overwrite those external edits.
//
// Model (no merge analysis, plain "external wins" overwrite - same
// semantics as closing and reopening the document):
//  - Every sync point (import, save, reload) records the file fingerprint
//    (mtime + size) and a "base" identity set (name / type / page / bounds
//    per annotation) that the file content corresponds to.
//  - A 2s poll timer per window stats the file; fingerprint drift means an
//    external change and triggers a full reload from disk ("clear +
//    replay").
//  - Clear step: an annotation matched by the parsed file content is
//    deleted (replayed from it); an annotation only present in the base
//    set was deleted externally (deleted too). An annotation in NEITHER
//    set is a local edit that was never written to disk: it survives the
//    reload and the next debounced save writes it into the sidecar.
//  - The reload is skipped while the user is actively editing (annotation
//    selected, text popup shown, or mouse button down); it is retried on
//    the next tick, so edits always win locally and are then overwritten
//    only after they had a chance to reach the file.
//  - SaveTab re-checks the fingerprint right before writing (closing the
//    2s poll window): if the file changed behind our back, the reload runs
//    first and the debounced save follows.

struct SidecarSyncEntry {
    Str name;      // may be empty: program-drawn annotations have no /NM
    int type = -1; // AnnotationType
    int pageNo = 0;
    RectF bounds;
    Str sig; // serialized machine line (JSON): content signature, lets the
             // reload classify external edits vs adds vs deletes
};

static void FreeSidecarSyncEntry(SidecarSyncEntry& e) {
    str::Free(e.name);
    e.name = {};
    str::Free(e.sig);
    e.sig = {};
}

// identity comparison, same matching rules as PageHasAnnot / MdMatchEntries:
// exact /NM match first, then type + page + rect overlap
static bool SidecarSyncEntryMatches(const SidecarSyncEntry& a, const SidecarSyncEntry& b) {
    if (len(a.name) > 0 && len(b.name) > 0) {
        return str::Eq(a.name, b.name);
    }
    return a.type == b.type && a.pageNo == b.pageNo && fabsf(a.bounds.x - b.bounds.x) < 1.0f &&
           fabsf(a.bounds.y - b.bounds.y) < 1.0f &&
           fabsf((a.bounds.x + a.bounds.dx) - (b.bounds.x + b.bounds.dx)) < 1.0f &&
           fabsf((a.bounds.y + a.bounds.dy) - (b.bounds.y + b.bounds.dy)) < 1.0f;
}

struct SidecarSyncState {
    Str path;
    i64 size = -1;
    FILETIME modTime{};
    Vec<SidecarSyncEntry> base;
};

static Vec<SidecarSyncState> gSidecarSync; // few entries, linear scan

static SidecarSyncState* FindSidecarSyncState(Str path) {
    for (SidecarSyncState& st : gSidecarSync) {
        if (str::Eq(st.path, path)) {
            return &st;
        }
    }
    return nullptr;
}

// note: the returned pointer is invalidated by a later GetSidecarSyncState
// (vector growth); callers use it within a single function scope
static SidecarSyncState* GetSidecarSyncState(Str path) {
    SidecarSyncState* st = FindSidecarSyncState(path);
    if (st) {
        return st;
    }
    SidecarSyncState ns;
    ns.path = str::Dup(path);
    VecAppend(gSidecarSync, std::move(ns));
    return &gSidecarSync[len(gSidecarSync) - 1];
}

static void SidecarSyncSetBase(SidecarSyncState* st, Vec<SidecarSyncEntry>&& newBase) {
    for (SidecarSyncEntry& e : st->base) {
        FreeSidecarSyncEntry(e);
    }
    st->base = std::move(newBase);
}

static bool SidecarFileFingerprint(Str path, i64& sizeOut, FILETIME& ftOut) {
    i64 sz = file::GetSize(path);
    if (sz < 0) {
        return false;
    }
    sizeOut = sz;
    ftOut = file::GetModificationTime(path);
    return true;
}

// lightweight identity of a live annotation (no StextCache / asset work)
static SidecarSyncEntry SidecarSyncEntryFromAnnot(Annotation* a) {
    SidecarSyncEntry e;
    e.type = (int)a->type;
    e.pageNo = PageNo(a);
    e.bounds = GetBounds(a);
    EngineMupdf* eng = a->engine;
    if (eng && a->pdfannot) {
        AutoUnlockRecursiveMutex cs(&eng->docLock);
        fz_context* ctx = eng->Ctx();
        fz_try(ctx) {
            const char* nm = pdf_annot_name(ctx, a->pdfannot);
            if (nm && *nm) {
                e.name = str::Dup(Str(nm));
            }
        }
        fz_catch(ctx) {
            fz_report_error(ctx);
        }
    }
    return e;
}

// identities as they exist in the parsed sidecar file; entries with an
// unreadable payload are excluded so their live counterparts are NOT
// deleted (they cannot be replayed, deleting them would lose content)
// content signature: the single-source machine line of an entry, same
// serialization the sidecar writer uses - identity-matched entries with
// different sigs were externally changed
static Str SidecarSyncEntrySig(const SidecarAnnot& sa) {
    str::Builder b;
    SerializeAnnotJson(b, sa);
    return b.TakeStr();
}

static Vec<SidecarSyncEntry> SidecarSyncEntriesFromParsed(const Vec<SidecarAnnot>& entries) {
    Vec<SidecarSyncEntry> res;
    for (const SidecarAnnot& sa : entries) {
        if (!sa.assetOk) {
            continue;
        }
        SidecarSyncEntry e;
        e.name = len(sa.name) > 0 ? str::Dup(sa.name) : Str{};
        e.type = (int)sa.type;
        e.pageNo = sa.pageNo;
        e.bounds = sa.bounds;
        e.sig = SidecarSyncEntrySig(sa);
        VecAppend(res, std::move(e));
    }
    return res;
}

// full reload from disk: delete what the file owns (replayed from it) or
// what was externally deleted (in base, absent from file); keep local edits
// that were never synced. Returns true if the session changed.
static bool SidecarReloadFromDisk(WindowTab* tab, EngineMupdf* e, const SidecarTarget& tgt, SidecarSyncState* st) {
    MainWindow* win = tab->win;
    Str data = file::ReadFile(tgt.path);
    if (len(data) == 0) {
        // read failure (e.g. OneDrive holding the file): retry on the next
        // tick; the fingerprint is left untouched so we come back
        return false;
    }
    fz_context* ctx = e->Ctx();
    char* z = CStrTemp(data);
    Vec<SidecarAnnot> entries;
    bool parseOk =
        str::EndsWithI(tgt.path, StrL(".md")) ? ParseSidecarMd(ctx, z, entries) : ParseSidecarJson(ctx, z, entries);
    str::Free(data);
    if (!parseOk) {
        // half-written file or format change: retry on the next tick
        return false;
    }
    LoadSidecarAssetBuffers(ctx, tgt.path, entries);
    Vec<SidecarSyncEntry> mdIds = SidecarSyncEntriesFromParsed(entries);

    // change classification: diff the new file content against the identity
    // + signature set recorded at the last sync point. The clear+replay
    // below rebuilds far more annotations than the external change actually
    // touched, so deleted/imported counts are meaningless to the user
    int nAdded = 0, nRemoved = 0, nUpdated = 0;
    if (st) {
        for (SidecarSyncEntry& m : mdIds) {
            bool found = false;
            for (SidecarSyncEntry& b : st->base) {
                if (SidecarSyncEntryMatches(m, b)) {
                    found = true;
                    if (!str::Eq(m.sig, b.sig)) {
                        nUpdated++;
                    }
                    break;
                }
            }
            if (!found) {
                nAdded++;
            }
        }
        for (SidecarSyncEntry& b : st->base) {
            bool found = false;
            for (SidecarSyncEntry& m : mdIds) {
                if (SidecarSyncEntryMatches(b, m)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                nRemoved++;
            }
        }
    }

    Vec<Annotation*> annots;
    EngineMupdfGetAnnotations((EngineBase*)e, annots);
    Vec<Annotation*> toDelete;
    int nKept = 0;
    for (Annotation* a : annots) {
        if (!a || !AnnotationIsLive(a)) {
            continue;
        }
        SidecarSyncEntry id = SidecarSyncEntryFromAnnot(a);
        bool mdHas = false, baseHas = false;
        for (SidecarSyncEntry& m : mdIds) {
            if (SidecarSyncEntryMatches(id, m)) {
                mdHas = true;
                break;
            }
        }
        if (!mdHas && st) {
            for (SidecarSyncEntry& b : st->base) {
                if (SidecarSyncEntryMatches(id, b)) {
                    baseHas = true;
                    break;
                }
            }
        }
        // file owns it (replay from file) or external delete (in base, gone
        // from file): both are cleared. Annotations in neither set are
        // local edits that were never written: keep them.
        if (mdHas || baseHas) {
            VecAppend(toDelete, a);
        } else {
            nKept++;
        }
        FreeSidecarSyncEntry(id);
    }

    // suppress the auto-save arming that MarkNotificationAsModified would
    // do for the deletions and the replayed imports
    gSidecarImporting = true;
    int nDeleted = 0;
    for (Annotation* a : toDelete) {
        DetachAnnotationFromUI(a);
        DeleteAnnotation(a);
        nDeleted++;
    }
    int nImported = ImportSidecarEntries(e, entries);
    gSidecarImporting = false;
    for (SidecarAnnot& sa : entries) {
        FreeSidecarAnnotStrings(ctx, sa);
    }

    // record the new sync point: base = file content, fingerprint = file now
    if (!st) {
        st = GetSidecarSyncState(tgt.path);
    }
    SidecarSyncSetBase(st, std::move(mdIds));
    SidecarFileFingerprint(tgt.path, st->size, st->modTime);

    bool changed = nDeleted > 0 || nImported > 0;
    int nTouched = nAdded + nRemoved + nUpdated;
    if (changed) {
        // user-facing summary of what the external change did, counted from
        // the content diff (not from the clear+replay rebuild); an
        // equivalent content change (OneDrive touched mtime) stays silent
        if (nTouched > 0) {
            Str keptSuffix = nKept > 0 ? fmt(", %d local unsaved kept", nKept) : Str{};
            auto what = nRemoved > 0 && nAdded == 0 && nUpdated == 0
                            ? StrL("Removed")
                            : (nAdded > 0 && nRemoved == 0 && nUpdated == 0 ? StrL("Loaded") : StrL("Reloaded"));
            auto why = nRemoved > 0 && nAdded == 0 && nUpdated == 0 ? StrL("deleted externally")
                                                                    : StrL("from external changes");
            ShowPlainNotification(win->hwndCanvas,
                                  fmt("%s %d annotation%s %s%s", what, nTouched,
                                      nTouched == 1 ? StrL("") : StrL("s"), why, keptSuffix),
                                  5000);
        }
        RefreshAnnotationLists(tab);
        if (IsMainWindowValidAndNotClosing(win)) {
            MainWindowRerender(win);
        }
        logf("sidecar: reloaded from '%s' (%d added, %d removed, %d updated; %d deleted, %d imported, %d kept)\n",
             tgt.path, nAdded, nRemoved, nUpdated, nDeleted, nImported, nKept);
    }
    return changed;
}

static void CALLBACK SidecarPollTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time);

static constexpr UINT_PTR kSidecarPollTimerId = 0x513D;
static constexpr UINT kSidecarPollDelayMs = 2000; // 2s poll

static void SidecarEnsurePollTimer(MainWindow* win) {
    if (!SidecarSeparateSaveEnabled()) {
        return;
    }
    if (!win || !win->hwndFrame) {
        return;
    }
    // periodic timer: fires every 2s until killed with the window
    SetTimer(win->hwndFrame, kSidecarPollTimerId, kSidecarPollDelayMs, SidecarPollTimerProc);
}

static void SidecarPollTab(WindowTab* tab) {
    if (!SidecarSeparateSaveEnabled()) {
        return;
    }
    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        return;
    }
    EngineBase* engine = dm->GetEngine();
    if (!engine || engine->kind != kindEngineMupdf) {
        return;
    }
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !e->pdfdoc) {
        return;
    }
    // active editing: skip this tick, retry in 2s
    if (tab->selectedAnnotation) {
        return;
    }
    if (IsAnnotationTextPopupShown(tab->win)) {
        return;
    }
    if (GetKeyState(VK_LBUTTON) & 0x8000) {
        return;
    }

    Str pdfPath = engine->FilePath();
    if (len(pdfPath) == 0) {
        return; // memory / embedded documents have no sidecar
    }
    SidecarTarget tgt = ResolveSidecarTarget(pdfPath, /*allowCreate=*/false);
    if (len(tgt.path) == 0 || !tgt.exists) {
        // absent (never created) or gone (deleted / not yet synced in):
        // hands off, we must not resurrect or preempt the file
        return;
    }
    i64 sz = 0;
    FILETIME ft{};
    if (!SidecarFileFingerprint(tgt.path, sz, ft)) {
        return;
    }
    SidecarSyncState* st = FindSidecarSyncState(tgt.path);
    if (!st) {
        // no sync point yet (file adopted externally / pre-existing): adopt
        // the current fingerprint, the next change reloads
        st = GetSidecarSyncState(tgt.path);
        st->size = sz;
        st->modTime = ft;
        return;
    }
    if (st->size == sz && FileTimeEq(st->modTime, ft)) {
        return;
    }

    if (SidecarReloadFromDisk(tab, e, tgt, st)) {
        // write back local edits that survived the reload (debounced)
        SidecarNotifyChanged((EngineBase*)e);
    }
}

static void CALLBACK SidecarPollTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time) {
    (void)msg;
    (void)id;
    (void)time;
    for (MainWindow* w : gWindows) {
        if (w->hwndFrame == hwnd) {
            Vec<WindowTab*> tabs = w->Tabs();
            for (WindowTab* t : tabs) {
                SidecarPollTab(t);
            }
            return;
        }
    }
}

// record the sync point after a successful import: base = parsed content,
// fingerprint = file now. call after the entries were consumed but while
// their strings are still alive
static MainWindow* FindWindowForEngine(EngineBase* engine); // defined below

static void SidecarRecordImportedSync(EngineBase* engine, const SidecarTarget& use, const Vec<SidecarAnnot>& entries) {
    MainWindow* win = FindWindowForEngine(engine);
    SidecarSyncState* st = GetSidecarSyncState(use.path);
    SidecarSyncSetBase(st, SidecarSyncEntriesFromParsed(entries));
    SidecarFileFingerprint(use.path, st->size, st->modTime);
    SidecarEnsurePollTimer(win);
}

// record the sync point after a successful save: base = what we wrote
// (entries are still alive here), fingerprint = file now
static void SidecarRecordSavedSync(EngineBase* engine, const SidecarTarget& tgt, const Vec<SidecarAnnot>& entries) {
    SidecarSyncState* st = GetSidecarSyncState(tgt.path);
    SidecarSyncSetBase(st, SidecarSyncEntriesFromParsed(entries));
    SidecarFileFingerprint(tgt.path, st->size, st->modTime);
    SidecarEnsurePollTimer(FindWindowForEngine(engine));
}

// lazily route the engine-side annotation-changed notification to us: the
// hook lives in EngineMupdf.cpp (compiled into every target) and must be
// installed by the one target that links Sidecar.cpp (the application). Any
// document open / save goes through SidecarMaybeImport or SidecarSaveTab
// first, so by the time an annotation can change the hook is in place.
static void SidecarInstallNotifyHook() {
    static bool installed = false;
    if (!installed) {
        installed = true;
        SidecarSetAnnotsChangedHook(&SidecarNotifyChanged);
    }
}

void SidecarMaybeImport(EngineBase* engine) {
    SidecarInstallNotifyHook();
    if (!SidecarSeparateSaveEnabled()) {
        return;
    }
    if (!engine || engine->kind != kindEngineMupdf) {
        return;
    }
    Str pdfPath = engine->FilePath();
    if (len(pdfPath) == 0) {
        return; // memory / embedded documents have no sidecar
    }
    EngineMupdf* e = AsEngineMupdf(engine);
    if (!e || !e->pdfdoc) {
        return;
    }

    SidecarTarget sib = ResolveSiblingTarget(pdfPath);
    SidecarTarget cen = ResolveCentralTarget(pdfPath);
    if (SidecarMdEnabled() && !sib.exists && !cen.exists) {
        // no .md anywhere: fall back to .json (read-only; the next manual
        // save writes the .md - automatic migration, the .json is kept)
        sib = ResolveSiblingTargetExt(pdfPath, ".json");
        cen = ResolveCentralTargetExt(pdfPath, ".json");
    }
    SidecarTarget use;
    if (sib.exists) {
        use = sib;
        if (cen.exists) {
            logf("sidecar: both sibling and central JSON exist for '%s', using sibling\n", pdfPath);
        }
    } else if (cen.exists) {
        use = cen;
    } else {
        logf("sidecar: no JSON sidecar found for '%s'\n", pdfPath);
        return;
    }

    Str data = file::ReadFile(use.path);
    if (len(data) == 0) {
        logf("sidecar: failed to read '%s'\n", use.path);
        return;
    }
    // CStrTemp comes from the temp arena; no explicit free
    char* z = CStrTemp(data);
    Vec<SidecarAnnot> entries;
    fz_context* ctx = e->Ctx();
    bool parseOk = str::EndsWithI(use.path, StrL(".md")) ? ParseSidecarMd(ctx, z, entries)
                                                         : ParseSidecarJson(ctx, z, entries);
    if (!parseOk) {
        logf("sidecar: failed to parse '%s'\n", use.path);
    }
    str::Free(data); // entry strings were dup'ed out of the JSON DOM
    if (len(entries) == 0) {
        // still a valid sync point (empty file): record it so the poller
        // does not adopt a stale fingerprint later
        SidecarRecordImportedSync(engine, use, entries);
        logf("sidecar: no supported annotations in '%s'\n", use.path);
        return;
    }
    // stamp / attachment payloads live in assets/ next to the JSON; load
    // them (entries whose file is missing are skipped by the import)
    LoadSidecarAssetBuffers(ctx, use.path, entries);
    // suppress the auto-save arming that MarkNotificationAsModified would
    // do during import (import may also run off the UI thread)
    gSidecarImporting = true;
    int n = ImportSidecarEntries(e, entries);
    gSidecarImporting = false;
    SidecarRecordImportedSync(engine, use, entries);
    for (SidecarAnnot& sa : entries) {
        FreeSidecarAnnotStrings(ctx, sa);
    }
    if (n > 0) {
        logf("sidecar: imported %d annotations from '%s' (%s)\n", n, use.path,
             use.central ? StrL("central") : StrL("sibling"));
    }
    // note: we deliberately do NOT mark the engine as modified; the imported
    // annotations behave like annotations that came with the PDF
}

SidecarResult SidecarSaveTab(WindowTab* tab, bool allowCreate) {
    SidecarInstallNotifyHook();
    if (!tab) {
        return SidecarResult::NotHandled;
    }
    DisplayModel* dm = tab->AsFixed();
    if (!dm) {
        return SidecarResult::NotHandled;
    }
    EngineBase* engine = dm->GetEngine();
    if (!SidecarWantsRedirect(engine)) {
        return SidecarResult::NotHandled;
    }
    MainWindow* win = tab->win;
    if (!win) {
        return SidecarResult::NotHandled;
    }
    EngineMupdf* e = AsEngineMupdf(engine);

    Str pdfPath = engine->FilePath();
    if (len(pdfPath) == 0) {
        return SidecarResult::NotHandled;
    }
    SidecarTarget tgt = ResolveSidecarTarget(pdfPath, allowCreate);
    if (len(tgt.path) == 0) {
        // auto-save with no established sidecar file: nothing to update
        return SidecarResult::Saved;
    }
    if (!EngineHasUnsavedAnnotations(engine)) {
        return SidecarResult::Saved;
    }

    // never blind-overwrite an externally modified file: if the fingerprint
    // drifted since our last sync point (the 2s poller was skipped due to
    // active editing, or the change landed within the poll window), reload
    // first; the debounced save then re-runs against the merged session
    if (tgt.exists) {
        i64 szNow = 0;
        FILETIME ftNow{};
        SidecarSyncState* st = FindSidecarSyncState(tgt.path);
        if (SidecarFileFingerprint(tgt.path, szNow, ftNow) &&
            (!st || st->size != szNow || !FileTimeEq(st->modTime, ftNow))) {
            logf("sidecar: '%s' changed externally, reloading before save\n", tgt.path);
            ShowPlainNotification(win->hwndCanvas, StrL("External changes detected before saving, reloading first"),
                                  5000);
            SidecarReloadFromDisk(tab, e, tgt, st);
            // re-arm the save either way: on success the debounced save
            // writes back the merged session; on failure (file locked) it
            // retries through here until the file becomes writable
            SidecarNotifyChanged(engine);
            // caller sees success; the real write follows via the debounce
            return SidecarResult::Saved;
        }
    }

    Vec<Annotation*> annots;
    EngineMupdfGetAnnotations(engine, annots);
    int nSkippedUnsupported = 0;
    Vec<SidecarAnnot> entries;
    CollectSidecarEntries(e, annots, nSkippedUnsupported, entries);
    Str data;
    if (SidecarMdEnabled()) {
        Str existing;
        if (tgt.exists) {
            existing = file::ReadFile(tgt.path);
        }
        data = BuildSidecarMd(e, pdfPath, entries, existing);
        str::Free(existing);
    } else {
        data = BuildSidecarJson(e, pdfPath, entries);
    }
    if (len(data) == 0) {
        ShowWarningNotification(win->hwndCanvas, StrL("Failed to build sidecar"), 5000);
        fz_context* ctx = e->Ctx();
        for (SidecarAnnot& sa : entries) {
            FreeSidecarAnnotStrings(ctx, sa);
        }
        return SidecarResult::Failed;
    }

    // ensure the central sub-folder exists before writing
    if (tgt.central) {
        Str dir = path::GetDirTemp(tgt.path);
        if (!dir::Exists(dir)) {
            dir::CreateAll(dir);
        }
    }
    if (!WriteSidecarFileAtomic(tgt.path, data)) {
        ShowWarningNotification(
            win->hwndCanvas, fmt(Tr("Failed to save '%s': %s").s, path::GetBaseNameTemp(tgt.path), StrL("cannot write file")),
            5000);
        logf("sidecar: failed to write '%s'\n", tgt.path);
        str::Free(data);
        fz_context* ctx = e->Ctx();
        for (SidecarAnnot& sa : entries) {
            FreeSidecarAnnotStrings(ctx, sa);
        }
        return SidecarResult::Failed;
    }
    str::Free(data);

    // write the payload files (stamp images / attachment contents) into
    // assets/ next to the JSON, then remove asset files that no sidecar
    // in the folder references anymore
    {
        fz_context* ctx = e->Ctx();
        int nAssetFailed = WriteSidecarAssets(tgt.path, entries);
        SidecarRecordSavedSync(engine, tgt, entries);
        for (SidecarAnnot& sa : entries) {
            FreeSidecarAnnotStrings(ctx, sa);
        }
        if (nAssetFailed > 0) {
            // the JSON references the missing files; the annotations stay
            // in the PDF until it is saved, so nothing is lost yet
            ShowWarningNotification(
                win->hwndCanvas, fmt("Saved '%s', but %d asset file(s) failed to write", tgt.path, nAssetFailed), 5000);
        }
        CleanupSidecarAssets(tgt.path);
    }

    e->modifiedAnnotations = false;
    ToolbarUpdateStateForWindow(win, true);
    TempStr baseName = path::GetBaseNameTemp(tgt.path);
    auto loc = tgt.central ? StrL("in central folder") : StrL("next to PDF");
    if (nSkippedUnsupported > 0) {
        // make it visible that some annotations (unsupported types or
        // unreadable payloads) are not in the sidecar and will be lost on
        // reopen (details in the log)
        ShowPlainNotification(win->hwndCanvas, fmt("Saved annotations %s: '%s' (%d skipped)", loc, baseName,
                                                  nSkippedUnsupported),
                              5000);
    } else {
        ShowPlainNotification(win->hwndCanvas,
                              fmt(Tr(tgt.central ? "Saved annotations in central folder: '%s'"
                                                 : "Saved annotations next to PDF: '%s'")
                                      .s,
                                  baseName),
                              5000);
    }
    logf("sidecar: saved %d annotations to '%s' (%s, %d skipped)\n", len(annots), tgt.path,
         tgt.central ? StrL("central") : StrL("sibling"), nSkippedUnsupported);
    return SidecarResult::Saved;
}

// ---------------------------------------------------------------------------
// debounced auto-save

static constexpr UINT_PTR kSidecarTimerId = 0x513C;
static constexpr UINT kSidecarAutoSaveDelayMs = 2000; // 2s debounce

static EngineBase* gSidecarAutoSaveEngine = nullptr; // AddRef'd while pending

static WindowTab* FindTabForEngine(EngineBase* engine) {
    for (MainWindow* w : gWindows) {
        Vec<WindowTab*> tabs = w->Tabs();
        for (WindowTab* t : tabs) {
            DisplayModel* dm = t->AsFixed();
            if (dm && dm->GetEngine() == engine) {
                return t;
            }
        }
    }
    return nullptr;
}

static MainWindow* FindWindowForEngine(EngineBase* engine) {
    WindowTab* tab = FindTabForEngine(engine);
    return tab ? tab->win : nullptr;
}

static void CALLBACK SidecarAutoSaveTimerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time) {
    (void)msg;
    (void)id;
    (void)time;
    KillTimer(hwnd, kSidecarTimerId);
    EngineBase* engine = gSidecarAutoSaveEngine;
    gSidecarAutoSaveEngine = nullptr;
    if (!engine) {
        return;
    }
    WindowTab* tab = FindTabForEngine(engine);
    engine->Release();
    if (!tab) {
        return;
    }
    // this timer only ever fires after MarkNotificationAsModified(), i.e.
    // the user really changed an annotation: creating the sidecar file here
    // is intended - the FIRST annotation made on a document must create
    // the file, otherwise it would only ever appear on manual save or
    // tab close
    SidecarSaveTab(tab, /*allowCreate=*/true);
}

void SidecarNotifyChanged(EngineBase* engine) {
    if (gSidecarImporting) {
        return;
    }
    if (!SidecarSeparateSaveEnabled()) {
        return;
    }
    if (!engine || engine->kind != kindEngineMupdf) {
        return;
    }
    MainWindow* win = FindWindowForEngine(engine);
    if (!win || !win->hwndFrame) {
        return;
    }
    if (gSidecarAutoSaveEngine != engine) {
        if (gSidecarAutoSaveEngine) {
            gSidecarAutoSaveEngine->Release();
        }
        engine->AddRef();
        gSidecarAutoSaveEngine = engine;
    }
    SidecarEnsurePollTimer(win); // belt-and-braces: the poll timer should already run
    SetTimer(win->hwndFrame, kSidecarTimerId, kSidecarAutoSaveDelayMs, SidecarAutoSaveTimerProc);
}
