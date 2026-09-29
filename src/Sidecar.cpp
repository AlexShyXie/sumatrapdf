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
//                                      // caret ink redact
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
//       "icon": "Comment",            // text annotation icon
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
#include "base/File.h"
#include "base/GuessFileType.h"
#include "base/Win.h"

#include <math.h>
#include <time.h>

extern "C" {
#include <mupdf/pdf.h>
}

#include "gui/UIModels.h"
#include "Settings.h"
#include "Annotation.h"
#include "DocController.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "EngineMupdf.h"
#include "DisplayModel.h"
#include "Translations.h"
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

// "D:\books\report.pdf" -> "D:\books\report.json"
static SidecarTarget ResolveSiblingTarget(Str pdfPath) {
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
    b.Append(StrL(".json"));
    res.path = b.TakeStr();
    res.central = false;
    res.exists = file::Exists(res.path);
    return res;
}

// "D:\books\sub\report.pdf" + "D:\notes" -> "D:\notes\sub\report.json"
static SidecarTarget ResolveCentralTarget(Str pdfPath) {
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
    b.Append(StrL(".json"));
    res.path = b.TakeStr();
    res.central = true;
    res.exists = file::Exists(res.path);
    return res;
}

// order of preference is the v5 load/save order: existing sibling wins,
// then existing central, otherwise (when creating is allowed) sibling
static SidecarTarget ResolveSidecarTarget(Str pdfPath, bool allowCreate) {
    SidecarTarget sib = ResolveSiblingTarget(pdfPath);
    SidecarTarget cen = ResolveCentralTarget(pdfPath);
    if (sib.exists) {
        return sib;
    }
    if (cen.exists) {
        return cen;
    }
    if (allowCreate && len(sib.path) > 0) {
        return sib;
    }
    return SidecarTarget{};
}

// ---------------------------------------------------------------------------
// type mapping

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
    // free text extras
    int textSize = -1;
    float textColor[3] = {0, 0, 0};
    bool hasTextCol = false;
    int quadding = -1; // 0 left, 1 center, 2 right
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
static void FreeSidecarAnnotStrings(SidecarAnnot& sa) {
    str::Free(sa.contents);
    str::Free(sa.author);
    str::Free(sa.subject);
    str::Free(sa.name);
    str::Free(sa.icon);
    str::Free(sa.text);
    sa.contents = {};
    sa.author = {};
    sa.subject = {};
    sa.name = {};
    sa.icon = {};
    sa.text = {};
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
            return true;
        default:
            // Link / Popup / Widget / Stamp (image payload) / FileAttachment
            // (payload) / media types: not supported
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

    PdfColor c = GetColor(a);
    if (c != (PdfColor)kColorUnset) {
        PdfColorToF(c, sa.color);
        sa.hasColor = true;
    }
    PdfColor ic = InteriorColor(a);
    if (ic != (PdfColor)kColorUnset) {
        PdfColorToF(ic, sa.interiorCol);
        sa.hasInterior = true;
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
    if (sa.type == AnnotationType::Text && len(sa.icon) > 0) {
        b.Append(StrL(",\n      \"icon\": "));
        AppendEscapedJson(b, sa.icon);
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

static constexpr int kSidecarVersion = 1;

static Str BuildSidecarJson(EngineMupdf* e, const Vec<Annotation*>& annots, Str pdfPath) {
    Vec<SidecarAnnot> entries;
    StextCache cache;
    cache.e = e;
    int nWritten = 0;
    for (Annotation* a : annots) {
        if (!a || !AnnotationIsLive(a)) {
            continue;
        }
        if (!SidecarTypeSupported(a->type)) {
            continue;
        }
        SidecarAnnot sa;
        CollectSidecarAnnot(a, sa, cache);
        if (sa.pageNo < 1 || sa.bounds.IsEmpty()) {
            FreeSidecarAnnotStrings(sa);
            continue;
        }
        VecAppend(entries, std::move(sa));
        nWritten++;
    }
    StextCacheReset(cache);

    str::Builder b;
    b.Append(fmt("{\n  \"version\": %d,\n  \"generator\": \"SumatraPDF-sidecar/1\",\n", kSidecarVersion));
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
    logf("sidecar: exported %d annotations\n", nWritten);
    // the collect side heap-dups its strings: release them after serializing
    for (SidecarAnnot& sa : entries) {
        FreeSidecarAnnotStrings(sa);
    }
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
                    FreeSidecarAnnotStrings(sa);
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
// import (creates PDF annotations in the freshly loaded engine)

// returns true if the page already contains this annotation: exact /NM
// match first, then the type+rect heuristic (prevents duplicates when the
// PDF embeds annotations that also exist in the sidecar)
static bool PageHasAnnot(fz_context* ctx, pdf_page* page, const SidecarAnnot& a) {
    pdf_annot* pa = pdf_first_annot(ctx, page);
    if (len(a.name) > 0) {
        while (pa) {
            const char* nm = pdf_annot_name(ctx, pa);
            if (nm && str::Eq(Str(nm), a.name)) {
                return true;
            }
            pa = pdf_next_annot(ctx, pa);
        }
        pa = pdf_first_annot(ctx, page);
    }
    while (pa) {
        auto pt = pdf_annot_type(ctx, pa);
        if ((AnnotationType)pt == a.type) {
            fz_rect r = pdf_annot_rect(ctx, pa);
            RectF rb = ToRectF(r);
            if (fabsf(rb.x - a.bounds.x) < 1.0f && fabsf(rb.y - a.bounds.y) < 1.0f &&
                fabsf((rb.x + rb.dx) - (a.bounds.x + a.bounds.dx)) < 1.0f &&
                fabsf((rb.y + rb.dy) - (a.bounds.y + a.bounds.dy)) < 1.0f) {
                return true;
            }
        }
        pa = pdf_next_annot(ctx, pa);
    }
    return false;
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
        if (!a.bounds.IsEmpty()) {
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
        if (a.type == AnnotationType::Text) {
            if (len(a.icon) > 0) {
                pdf_set_annot_icon_name(ctx, pa, CStrTemp(a.icon));
            }
            if (a.isOpen) {
                pdf_set_annot_is_open(ctx, pa, 1);
            }
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
    if (!ParseSidecarJson(ctx, z, entries)) {
        logf("sidecar: failed to parse '%s'\n", use.path);
    }
    str::Free(data); // entry strings were dup'ed out of the JSON DOM
    if (len(entries) == 0) {
        logf("sidecar: no supported annotations in '%s'\n", use.path);
        return;
    }
    // suppress the auto-save arming that MarkNotificationAsModified would
    // do during import (import may also run off the UI thread)
    gSidecarImporting = true;
    int n = ImportSidecarEntries(e, entries);
    gSidecarImporting = false;
    for (SidecarAnnot& sa : entries) {
        FreeSidecarAnnotStrings(sa);
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

    Vec<Annotation*> annots;
    EngineMupdfGetAnnotations(engine, annots);
    Str json = BuildSidecarJson(e, annots, pdfPath);
    if (len(json) == 0) {
        ShowWarningNotification(win->hwndCanvas, StrL("Failed to build JSON sidecar"), 5000);
        return SidecarResult::Failed;
    }

    // ensure the central sub-folder exists before writing
    if (tgt.central) {
        Str dir = path::GetDirTemp(tgt.path);
        if (!dir::Exists(dir)) {
            dir::CreateAll(dir);
        }
    }
    if (!file::WriteFile(tgt.path, json)) {
        ShowWarningNotification(win->hwndCanvas,
                                fmt(Tr("Failed to save '%s': %s").s, tgt.path, StrL("cannot write file")), 5000);
        logf("sidecar: failed to write '%s'\n", tgt.path);
        str::Free(json);
        return SidecarResult::Failed;
    }
    str::Free(json);

    e->modifiedAnnotations = false;
    ToolbarUpdateStateForWindow(win, true);
    ShowPlainNotification(win->hwndCanvas, fmt(Tr("Saved annotations to '%s'").s, tgt.path), 5000);
    logf("sidecar: saved %d annotations to '%s' (%s)\n", len(annots), tgt.path,
         tgt.central ? StrL("central") : StrL("sibling"));
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
    // don't create sidecar files out of nowhere; only update existing ones
    SidecarSaveTab(tab, /*allowCreate=*/false);
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
    SetTimer(win->hwndFrame, kSidecarTimerId, kSidecarAutoSaveDelayMs, SidecarAutoSaveTimerProc);
}
