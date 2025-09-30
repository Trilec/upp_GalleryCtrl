/*
================================================================================
 U++ Icon Builder — Modular Primitives (single-file demo)
================================================================================
 Overview
   • Left panel: tools, ops, style, actions. Right panel: live code-export.
   • Drawing/editing is routed through a per-primitive ops registry.
   • Geometry is normalized to an inset clip rectangle (0..1).
   • Code export emits BufferPainter code.

 Build (U++ 2025.1+):
   uses: Core, CtrlLib, Draw, Painter

 Notes on safety & style:
   • No ownership with raw pointers; only stack/stateful members (RAII).
   • All GUI actions remain on the main thread (Ctrl / callbacks).
   • Opacity and dash input are sanitized. Begin/End are balanced per pass.
   • Selection overlays are drawn unclipped for clarity.
   • JSON I/O uses Value/ValueMap; resilient to missing keys.

 Maintenance:
   • Primitive ops are grouped and registered in Facets().
   • Helpers for pixel↔normalized conversions defined once per scope.
   • Keep Handle sizes small and constant (HANDLE_PX).
================================================================================
*/

#include <CtrlLib/CtrlLib.h>
#include <Draw/Draw.h>
#include <Painter/Painter.h>

using namespace Upp;

// ========================= Types: Style / Model / Enums =======================

enum class LineStyle { Solid, LongDash, ShortDash, Dotted };

struct Style : Moveable<Style> {
    Color  fill        = Color(163, 201, 168);
    Color  stroke      = Color(30, 53, 47);
    int    strokeWidth = 2;
    bool   evenOdd     = false;
    String dash;               // freeform "a,b,a,b,..."
    bool   enableFill  = true;
    bool   enableStroke= true;

    double fillOpacity   = 1.0; // 0..1
    double strokeOpacity = 1.0; // 0..1
    double opacity       = 1.0; // global 0..1 multiplier

    // Outline (separate pass)
    bool   outlineEnable   = false;
    bool   outlineOutside  = true; // draw under main passes
    Color  outlineColor    = Red();
    int    outlineWidth    = 0;    // extra outer width
    LineStyle outlineStyle = LineStyle::Solid;
    int    outlineOffsetX  = 0;    // px offset (X) for faux-shadow
    int    outlineOffsetY  = 0;    // px offset (Y)
    String outlineDash;            // freeform dash
    double outlineOpacity  = 1.0;  // 0..1

    LineStyle strokeStyle  = LineStyle::Solid;
};

static inline int ToI(LineStyle s) {
    switch(s) {
        case LineStyle::LongDash:  return 1;
        case LineStyle::ShortDash: return 2;
        case LineStyle::Dotted:    return 3;
        default:                   return 0; // Solid
    }
}
static inline LineStyle FromI(int i) {
    switch(i) {
        case 1:  return LineStyle::LongDash;
        case 2:  return LineStyle::ShortDash;
        case 3:  return LineStyle::Dotted;
        default: return LineStyle::Solid;
    }
}

enum class Tool  { Cursor, CreateShape };
enum class PType { Rect, Circle, Line, Triangle, Curve, Text };

static inline const char* ToolName(Tool t)  { return t == Tool::Cursor ? "Cursor" : "CreateShape"; }
static inline const char* PTypeName(PType t){
    switch(t){
        case PType::Rect:     return "Rect";
        case PType::Circle:   return "Circle";
        case PType::Line:     return "Line";
        case PType::Triangle: return "Triangle";
        case PType::Curve:    return "Curve";
        case PType::Text:     return "Text";
    }
    return "?";
}

// ================================ Payloads ===================================

struct TextData {
    String text = "Text";
    String face = "";
    double sizeN = 0.18; // normalized to inset height
    bool   bold = false, italic = false;
};

struct CurveData {
    bool   cubic  = true;   // false => quadratic
    bool   closed = false;
    Pointf a0, a1;          // anchors
    Pointf c0, c1;          // controls (c1 ignored for quadratic)
};

struct Shape : Moveable<Shape> {
    PType  type  = PType::Rect;
    Style  style;

    // Rect
    double x=0, y=0, w=0, h=0;
    // Circle
    double cx=0, cy=0, r=0;               // r relative to min(inset w,h)
    // Line / Triangle
    Pointf p1, p2, p3;
    // Payloads
    TextData  text;
    CurveData curve;
};

// ===================== Normalized <-> Pixel helpers (global) ==================

static inline int     X(const Rect& r, double nx) { return r.left + int(r.Width()  * nx + 0.5); }
static inline int     Y(const Rect& r, double ny) { return r.top  + int(r.Height() * ny + 0.5); }
static inline int     R(const Rect& r, double nr) { return int(min(r.Width(), r.Height()) * nr + 0.5); }
static inline double  NX(const Rect& r, int px)   { return (px - r.left)  / double(max(1, r.Width())); }
static inline double  NY(const Rect& r, int py)   { return (py - r.top)   / double(max(1, r.Height())); }
static inline int     Snap1D(int v, int origin, int step) { return origin + ((v - origin + step / 2) / step) * step; }

// Inclusive clamp to [0..1] (uses Upp::clamp internally—robust & readable)
static inline double  Clamp01(double v) { return Upp::clamp(v, 0.0, 1.0); }

// Fast text formatter for code emission (thread-local buffer is OK here)
static inline const char* Decimal4(double v) {
    static thread_local String s;
    s = Format("%.4f", v);
    return ~s;
}

// ============================ Hit-testing helpers =============================

static inline bool IsNearSegment(Point p, Point a, Point b, int tol) {
    if(a == b) return abs(p.x - a.x) <= tol && abs(p.y - a.y) <= tol;
    double vx = b.x - a.x, vy = b.y - a.y, wx = p.x - a.x, wy = p.y - a.y;
    double vv = vx * vx + vy * vy;
    if(vv <= 1e-9) return false;
    double t = (wx * vx + wy * vy) / vv;
    t = Upp::clamp(t, 0.0, 1.0);
    double qx = a.x + t * vx, qy = a.y + t * vy, dx = p.x - qx, dy = p.y - qy;
    return dx * dx + dy * dy <= double(tol * tol);
}
static inline bool IsPointInTriangle(Point p, Point a, Point b, Point c) {
    auto s = [&](Point p1, Point p2, Point p3) {
        return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
    };
    bool b1 = s(p, a, b) < 0, b2 = s(p, b, c) < 0, b3 = s(p, c, a) < 0;
    return (b1 == b2) && (b2 == b3);
}

// =============================== Dash helpers ================================

static inline String DashFrom(LineStyle ls, const String& custom) {
    if(!custom.IsEmpty()) return custom;
    switch(ls) {
        case LineStyle::LongDash:  return "12,4";
        case LineStyle::ShortDash: return "8,4";
        case LineStyle::Dotted:    return "2,4";
        default:                   return String();
    }
}

// Value helpers for permissive JSON import forms
static inline bool IsPairArrayValueMap(const Value& v) {
    return IsValueArray(v) && !ValueArray(v).IsEmpty() &&
           IsValueMap(ValueArray(v)[0]) &&
           ValueMap(ValueArray(v)[0]).Find("key") >= 0;
}
static ValueMap PairArrayToMap(const ValueArray& a) {
    ValueMap m;
    for(const Value& e : a) {
        if(!IsValueMap(e)) continue;
        ValueMap kv = e;
        const Value key = kv["key"];
        const Value val = kv["value"];
        m.Add(key, val); // mutation via Add (operator[] returns const Value)
    }
    return m;
}

// ========================= Painter pass building blocks =======================
// (BufferPainter API: Begin/End/Opacity/EvenOdd/Dash/Fill/Stroke/Clip/etc.)
// Verified against headers: Character/Opacity/Dash exist (virtual ops).  :contentReference[oaicite:0]{index=0}  :contentReference[oaicite:1]{index=1}

template <class BuildPath>
static inline void Pass_Outline(BufferPainter& p, BuildPath build, const ::Style& st) {
    if(!st.outlineEnable || st.outlineWidth <= 0) return;

    p.Begin();
    if(st.outlineOffsetX || st.outlineOffsetY)
        p.Translate(st.outlineOffsetX, st.outlineOffsetY);

    build();

    const double o = Clamp01(st.outlineOpacity) * Clamp01(st.opacity);
    if(o < 1.0) p.Opacity(o);

    const String d = DashFrom(st.outlineStyle, st.outlineDash);
    if(!d.IsEmpty()) p.Dash(d, 0.0);

    const int W = (st.enableStroke ? st.strokeWidth : 0) + max(1, 2 * st.outlineWidth);
    p.Stroke(W, st.outlineColor);
    p.End();
}

template <class BuildPath>
static inline void Pass_Fill(BufferPainter& p, BuildPath build, const Style& st) {
    if(!st.enableFill) return;
    p.Begin();
    build();
    if(st.evenOdd) p.EvenOdd(true);
    const double o = Clamp01(st.fillOpacity) * Clamp01(st.opacity);
    if(o < 1.0) p.Opacity(o);
    p.Fill(st.fill);
    p.End();
}

template <class BuildPath>
static inline void Pass_Stroke(BufferPainter& p, BuildPath build, const Style& st) {
    if(!st.enableStroke) return;
    p.Begin();
    build();
    const double o = Clamp01(st.strokeOpacity) * Clamp01(st.opacity);
    if(o < 1.0) p.Opacity(o);
    const String d = DashFrom(st.strokeStyle, st.dash);
    if(!d.IsEmpty()) p.Dash(d, 0.0);
    p.Stroke(st.strokeWidth, st.stroke);
    p.End();
}

// ---------------------------- Code emission helpers ---------------------------

static inline void EmitDashCode(String& out, LineStyle ls, const String& custom) {
    const String d = DashFrom(ls, custom);
    if(!d.IsEmpty())
        out << Format("    p.Dash(String(\"%s\"), 0.0);\n", ~d);
}
static inline void EmitOpacityCode(String& out, double o) {
    o = Clamp01(o);
    if(o < 1.0)
        out << Format("    p.Opacity(%.3f);\n", o);
}

// Safe style application for Painter: sanitize dash/opacity.
static inline void ApplyStyle(BufferPainter& p, const Style& st) {
    if(st.opacity < 1.0)
        p.Opacity(Upp::clamp(st.opacity, 0.0, 1.0));

    if(!st.dash.IsEmpty()) {
        Vector<double> seg;
        const char* s = ~st.dash;
        while(*s) {
            while(*s == ' ' || *s == '\t' || *s == ',') s++;
            char* end = nullptr;
            double v = strtod(s, &end);
            if(end == s) break;
            if(v > 0) seg.Add(v); // ignore non-positive
            s = end;
        }
        if(seg.GetCount() >= 2) {
            String norm;
            for(int i = 0; i < seg.GetCount(); ++i) {
                if(i) norm << ',';
                norm << AsString(seg[i]);
            }
            p.Dash(norm, 0.0);
        }
    }
}

// =========================== Ops Registry Contracts ===========================

struct PrimitiveOps {
    void (*EmitPainter)(BufferPainter&, const Rect&, const Shape&);
    bool (*HitBody)(const Rect&, const Shape&, Point);
    int  (*HitVertex)(const Rect&, const Shape&, Point);
    void (*DrawOverlay)(Draw&, const Rect&, const Shape&);
    void (*BeginCreate)(Shape&, const Rect&, Point);
    void (*DragCreate)(Shape&, const Rect&, Point, Point, bool, int);
    void (*BeginEdit)(Shape&, const Rect&, Point, int, double&, double&);
    void (*DragEdit)(Shape&, const Rect&, Point, bool, int, bool, int, double&, double&);
    void (*EmitCode)(String&, const Shape&);
};

struct ToolSpec : Moveable<ToolSpec> {
    PType       type;
    const char* label;
    const char* tip;
};

const PrimitiveOps&    GetOps(PType);
const Vector<ToolSpec>&GetToolSpecs();

static constexpr int MIN_EMIT_PX = 6;
static constexpr int HANDLE_PX   = 9;

// ============================================================================
// RECT
// ============================================================================

static void Rect_EmitPainter(BufferPainter& p, const Rect& inset, const Shape& s) {
    Rect r = RectC(X(inset, s.x), Y(inset, s.y),
                   X(inset, s.x + s.w) - X(inset, s.x),
                   Y(inset, s.y + s.h) - Y(inset, s.y));
    r.Normalize();
    if(r.Width() < MIN_EMIT_PX || r.Height() < MIN_EMIT_PX) return;

    const Style& st = s.style;
    auto Path = [&] {
        p.Move(Pointf(r.left, r.top));
        p.Line(Pointf(r.right, r.top));
        p.Line(Pointf(r.right, r.bottom));
        p.Line(Pointf(r.left, r.bottom));
        p.Close();
    };

    if(st.outlineEnable && st.outlineOutside) Pass_Outline(p, [&]{ p.Begin(); Path(); p.End(); }, st);
    Pass_Fill  (p, [&]{ p.Begin(); Path(); p.End(); }, st);
    Pass_Stroke(p, [&]{ p.Begin(); Path(); p.End(); }, st);
    if(st.outlineEnable && !st.outlineOutside) Pass_Outline(p, [&]{ p.Begin(); Path(); p.End(); }, st);
}

static bool Rect_HitBody(const Rect& inset, const Shape& s, Point m) {
    Rect r = RectC(X(inset, s.x), Y(inset, s.y),
                   X(inset, s.x + s.w) - X(inset, s.x),
                   Y(inset, s.y + s.h) - Y(inset, s.y));
    r.Normalize();
    return r.Inflated(4).Contains(m);
}

static int Rect_HitVertex(const Rect& inset, const Shape& s, Point m) {
    Point tl(X(inset, s.x),           Y(inset, s.y));
    Point tr(X(inset, s.x + s.w),     Y(inset, s.y));
    Point br(X(inset, s.x + s.w),     Y(inset, s.y + s.h));
    Point bl(X(inset, s.x),           Y(inset, s.y + s.h));
    auto Near = [&](Point a){ return abs(a.x - m.x) <= HANDLE_PX && abs(a.y - m.y) <= HANDLE_PX; };
    if(Near(tl)) return 0;
    if(Near(tr)) return 1;
    if(Near(br)) return 2;
    if(Near(bl)) return 3;
    return -1;
}

static void Rect_DrawOverlay(Draw& w, const Rect& inset, const Shape& s) {
    Color sel = SColorMark();
    Rect r(Point(X(inset, s.x), Y(inset, s.y)),
           Point(X(inset, s.x + s.w), Y(inset, s.y + s.h)));
    r.Normalize();

    // box
    w.DrawRect(r.left, r.top, r.Width(), 1, sel);
    w.DrawRect(r.left, r.bottom, r.Width() + 1, 1, sel);
    w.DrawRect(r.left, r.top, 1, r.Height(), sel);
    w.DrawRect(r.right, r.top, 1, r.Height(), sel);

    // handles
    auto H = [&](Point p){ w.DrawRect(p.x - 3, p.y - 3, 7, 7, sel); };
    H(r.TopLeft()); H(r.TopRight()); H(r.BottomLeft()); H(r.BottomRight());
}

static void Rect_BeginCreate(Shape& s, const Rect& inset, Point start) {
    s.type = PType::Rect;
    s.x = NX(inset, start.x);
    s.y = NY(inset, start.y);
    s.w = s.h = 0;
}

static void Rect_DragCreate(Shape& s, const Rect& inset, Point, Point cur, bool snap, int grid) {
    if(snap){ cur.x = Snap1D(cur.x, inset.left, grid); cur.y = Snap1D(cur.y, inset.top, grid); }
    s.w = NX(inset, cur.x) - s.x;
    s.h = NY(inset, cur.y) - s.y;
}

static void Rect_BeginEdit(Shape&, const Rect& inset, Point grab, int, double& gx, double& gy) {
    gx = NX(inset, grab.x); gy = NY(inset, grab.y);
}

static void Rect_DragEdit(Shape& s, const Rect& inset, Point cur, bool snap, int grid,
                          bool moving, int hv, double& gx, double& gy) {
    if(snap){ cur.x = Snap1D(cur.x, inset.left, grid); cur.y = Snap1D(cur.y, inset.top, grid); }
    double nx = NX(inset, cur.x), ny = NY(inset, cur.y);

    if(moving) { s.x += nx - gx; s.y += ny - gy; gx = nx; gy = ny; return; }

    switch(hv) {
        case 0: s.w += s.x - nx; s.h += s.y - ny; s.x = nx; s.y = ny; break;             // TL
        case 1: s.w =  nx - s.x; s.h += s.y - ny;                s.y = ny; break;        // TR
        case 2: s.w =  nx - s.x; s.h =  ny - s.y;                            break;      // BR
        case 3: s.h =  ny - s.y; s.w += s.x - nx; s.x = nx;                 break;       // BL
        default: break;
    }
}

static void Rect_EmitCode(String& out, const Shape& s) {
    out << "    // Rect\n    p.Begin();\n";
    out << Format(
        "    p.Move(Pointf(X(inset,%s),Y(inset,%s))); "
        "p.Line(Pointf(X(inset,%s),Y(inset,%s))); "
        "p.Line(Pointf(X(inset,%s),Y(inset,%s))); "
        "p.Line(Pointf(X(inset,%s),Y(inset,%s))); p.Close();\n",
        Decimal4(s.x), Decimal4(s.y),
        Decimal4(s.x + s.w), Decimal4(s.y),
        Decimal4(s.x + s.w), Decimal4(s.y + s.h),
        Decimal4(s.x),       Decimal4(s.y + s.h)
    );
    if(s.style.opacity < 1.0) out << Format("    p.Opacity(%.3f);\n", Upp::clamp(s.style.opacity, 0.0, 1.0));
    if(!s.style.dash.IsEmpty()) out << Format("    p.Dash(String(\"%s\"),0.0);\n", ~s.style.dash);
    if(s.style.evenOdd) out << "    p.EvenOdd(true);\n";
    if(s.style.enableFill)
        out << Format("    p.Fill(Color(%d,%d,%d));\n", s.style.fill.GetR(), s.style.fill.GetG(), s.style.fill.GetB());
    if(s.style.enableStroke)
        out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n", s.style.strokeWidth,
                      s.style.stroke.GetR(), s.style.stroke.GetG(), s.style.stroke.GetB());
    out << "    p.End();\n\n";
}

// ============================================================================
// CIRCLE
// ============================================================================

static inline int Rpx(const Rect& r, double nr) { return int(min(r.Width(), r.Height()) * nr + 0.5); }

static void Circle_EmitPainter(BufferPainter& p, const Rect& inset, const Shape& s) {
    const int cx = X(inset, s.cx);
    const int cy = Y(inset, s.cy);
    const int rr = R(inset, s.r);
    if(rr < 1) return;

    const Style& st = s.style;
    auto Path = [&]{
        p.Move(Pointf(cx + rr, cy));
        p.SvgArc(Pointf(rr, rr), 0, false, true, Pointf(cx - rr, cy));
        p.SvgArc(Pointf(rr, rr), 0, false, true, Pointf(cx + rr, cy));
        p.Close();
    };

    if(st.outlineEnable && st.outlineOutside) Pass_Outline(p, [&]{ p.Begin(); Path(); p.End(); }, st);
    Pass_Fill  (p, [&]{ p.Begin(); Path(); p.End(); }, st);
    Pass_Stroke(p, [&]{ p.Begin(); Path(); p.End(); }, st);
    if(st.outlineEnable && !st.outlineOutside) Pass_Outline(p, [&]{ p.Begin(); Path(); p.End(); }, st);
}

static bool Circle_HitBody(const Rect& inset, const Shape& s, Point m) {
    const int cx = X(inset, s.cx);
    const int cy = Y(inset, s.cy);
    const int r  = R(inset, s.r);
    if(r < 1) return false;

    const int dx = m.x - cx, dy = m.y - cy;
    const double d = sqrt(double(dx * dx + dy * dy));
    const int tol = max(6, s.style.strokeWidth / 2 + 4);

    return s.style.enableFill ? (d <= r || fabs(d - r) <= tol) : (fabs(d - r) <= tol);
}

static int Circle_HitVertex(const Rect& inset, const Shape& s, Point m) {
    const Point c(X(inset, s.cx), Y(inset, s.cy));
    const Point e(c.x + R(inset, s.r), c.y); // east handle
    auto Near = [&](Point a){ return abs(a.x - m.x) <= HANDLE_PX && abs(a.y - m.y) <= HANDLE_PX; };
    if(Near(c)) return 0; // center
    if(Near(e)) return 1; // radius handle
    return -1;
}

static void Circle_DrawOverlay(Draw& w, const Rect& inset, const Shape& s) {
    const int cx = X(inset, s.cx);
    const int cy = Y(inset, s.cy);
    const int rr = R(inset, s.r);
    if(rr < 1) return;

    const Color sel = SColorMark();
    const Rect  bb  = RectC(cx - rr, cy - rr, 2 * rr, 2 * rr);

    // box
    w.DrawRect(RectC(bb.left,   bb.top,    bb.Width(), 1), sel);
    w.DrawRect(RectC(bb.left,   bb.bottom, bb.Width(), 1), sel);
    w.DrawRect(RectC(bb.left,   bb.top,    1,          bb.Height()), sel);
    w.DrawRect(RectC(bb.right,  bb.top,    1,          bb.Height()+1), sel);

    // handles
    w.DrawRect(RectC(cx - 2, cy - 2, 5, 5), sel);          // center
    w.DrawRect(RectC(cx + rr - 2, cy - 2, 5, 5), sel);     // east
}

static void Circle_BeginCreate(Shape& s, const Rect& inset, Point start) {
    s.type = PType::Circle;
    s.cx = NX(inset, start.x);
    s.cy = NY(inset, start.y);
    s.r  = 0.0;
}

static void Circle_DragCreate(Shape& s, const Rect& inset, Point, Point cur, bool snap, int grid) {
    if(snap){ cur.x = Snap1D(cur.x, inset.left, grid); cur.y = Snap1D(cur.y, inset.top, grid); }
    const double nx = NX(inset, cur.x);
    const double ny = NY(inset, cur.y);
    const double dx = nx - s.cx, dy = ny - s.cy;
    s.r = max(0.0, sqrt(dx * dx + dy * dy));
}

static void Circle_BeginEdit(Shape&, const Rect& inset, Point grab, int, double& gx, double& gy) {
    gx = NX(inset, grab.x); gy = NY(inset, grab.y);
}

static void Circle_DragEdit(Shape& s, const Rect& inset, Point cur, bool snap, int grid,
                            bool moving, int hv, double& gx, double& gy) {
    if(snap){ cur.x = Snap1D(cur.x, inset.left, grid); cur.y = Snap1D(cur.y, inset.top, grid); }
    const double nx = NX(inset, cur.x);
    const double ny = NY(inset, cur.y);

    if(moving || hv == 0) { s.cx += nx - gx; s.cy += ny - gy; gx = nx; gy = ny; return; }
    if(hv == 1) {
        const double dx = nx - s.cx, dy = ny - s.cy;
        s.r = max(0.0, sqrt(dx * dx + dy * dy));
    }
}

static void Circle_EmitCode(String& out, const Shape& s) {
    out << "    // Circle\n    p.Begin();\n";
    out << Format("    p.Move(Pointf(X(inset,%s)+R(inset,%s), Y(inset,%s)));\n",
                  Decimal4(s.cx), Decimal4(s.r), Decimal4(s.cy));
    out << Format("    p.SvgArc(Pointf(R(inset,%s),R(inset,%s)), 0, false, true, "
                  "Pointf(X(inset,%s)-R(inset,%s), Y(inset,%s)));\n",
                  Decimal4(s.r), Decimal4(s.r), Decimal4(s.cx), Decimal4(s.r), Decimal4(s.cy));
    out << Format("    p.SvgArc(Pointf(R(inset,%s),R(inset,%s)), 0, false, true, "
                  "Pointf(X(inset,%s)+R(inset,%s), Y(inset,%s)));\n",
                  Decimal4(s.r), Decimal4(s.r), Decimal4(s.cx), Decimal4(s.r), Decimal4(s.cy));
    EmitOpacityCode(out, s.style.opacity);
    EmitDashCode(out, s.style.strokeStyle, s.style.dash);
    if(s.style.enableFill)
        out << Format("    p.Fill(Color(%d,%d,%d));\n", s.style.fill.GetR(), s.style.fill.GetG(), s.style.fill.GetB());
    if(s.style.enableStroke)
        out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n", s.style.strokeWidth,
                      s.style.stroke.GetR(), s.style.stroke.GetG(), s.style.stroke.GetB());
    out << "    p.End();\n\n";
}

// ============================================================================
// LINE
// ============================================================================

static void Line_EmitPainter(BufferPainter& p, const Rect& inset, const Shape& s) {
    const Point a(X(inset, s.p1.x), Y(inset, s.p1.y));
    const Point b(X(inset, s.p2.x), Y(inset, s.p2.y));
    const int dx = b.x - a.x, dy = b.y - a.y;
    if(dx * dx + dy * dy < MIN_EMIT_PX * MIN_EMIT_PX) return;

    const Style& st = s.style;
    auto Path = [&]{ p.Move(Pointf(a)); p.Line(Pointf(b)); };

    if(st.outlineEnable && st.outlineOutside) Pass_Outline(p, [&]{ p.Begin(); Path(); p.End(); }, st);
    Pass_Fill  (p, [&]{ p.Begin(); Path(); p.End(); }, st);    // harmless when Fill disabled
    Pass_Stroke(p, [&]{ p.Begin(); Path(); p.End(); }, st);
    if(st.outlineEnable && !st.outlineOutside) Pass_Outline(p, [&]{ p.Begin(); Path(); p.End(); }, st);
}

static bool Line_HitBody(const Rect& inset, const Shape& s, Point m) {
    return IsNearSegment(m,
                         Point(X(inset, s.p1.x), Y(inset, s.p1.y)),
                         Point(X(inset, s.p2.x), Y(inset, s.p2.y)),
                         6);
}

static int Line_HitVertex(const Rect& inset, const Shape& s, Point m) {
    Point a(X(inset, s.p1.x), Y(inset, s.p1.y));
    Point b(X(inset, s.p2.x), Y(inset, s.p2.y));
    auto Near = [&](Point q){ return abs(q.x - m.x) <= HANDLE_PX && abs(q.y - m.y) <= HANDLE_PX; };
    if(Near(a)) return 0;
    if(Near(b)) return 1;
    return -1;
}

static void Line_DrawOverlay(Draw& w, const Rect& inset, const Shape& s) {
    const Color sel = SColorMark();
    Point p1(X(inset, s.p1.x), Y(inset, s.p1.y));
    Point p2(X(inset, s.p2.x), Y(inset, s.p2.y));
    w.DrawLine(p1, p2, 1, sel);

    auto H = [&](Point p){ w.DrawRect(RectC(p.x - 3, p.y - 3, 7, 7), sel); };
    H(p1); H(p2);
}

static void Line_BeginCreate(Shape& s, const Rect& inset, Point start) {
    s.type = PType::Line;
    s.p1 = Pointf(NX(inset, start.x), NY(inset, start.y));
    s.p2 = s.p1;
}

static void Line_DragCreate(Shape& s, const Rect& inset, Point, Point cur, bool snap, int grid) {
    if(snap){ cur.x = Snap1D(cur.x, inset.left, grid); cur.y = Snap1D(cur.y, inset.top, grid); }
    s.p2 = Pointf(NX(inset, cur.x), NY(inset, cur.y));
}

static void Line_BeginEdit(Shape&, const Rect& inset, Point grab, int, double& gx, double& gy) {
    gx = NX(inset, grab.x); gy = NY(inset, grab.y);
}

static void Line_DragEdit(Shape& s, const Rect& inset, Point cur, bool snap, int grid,
                          bool moving, int hv, double& gx, double& gy) {
    if(snap){ cur.x = Snap1D(cur.x, inset.left, grid); cur.y = Snap1D(cur.y, inset.top, grid); }
    const double nx = NX(inset, cur.x), ny = NY(inset, cur.y);

    if(moving) { Pointf d(nx - gx, ny - gy); s.p1 += d; s.p2 += d; gx = nx; gy = ny; return; }
    if(hv == 0) s.p1 = Pointf(nx, ny); else s.p2 = Pointf(nx, ny);
}

static void Line_EmitCode(String& out, const Shape& s) {
    out << "    // Line\n    p.Begin();\n";
    out << Format("    p.Move(Pointf(X(inset,%s),Y(inset,%s))); p.Line(Pointf(X(inset,%s),Y(inset,%s)));\n",
                  Decimal4(s.p1.x), Decimal4(s.p1.y), Decimal4(s.p2.x), Decimal4(s.p2.y));
    EmitOpacityCode(out, s.style.opacity);
    EmitDashCode(out, s.style.strokeStyle, s.style.dash);
    if(s.style.enableStroke)
        out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n", s.style.strokeWidth,
                      s.style.stroke.GetR(), s.style.stroke.GetG(), s.style.stroke.GetB());
    if(s.style.enableFill)
        out << Format("    p.Fill(Color(%d,%d,%d));\n", s.style.fill.GetR(), s.style.fill.GetG(), s.style.fill.GetB());
    out << "    p.End();\n\n";
}

// ============================================================================
// TRIANGLE
// ============================================================================

static void Triangle_EmitPainter(BufferPainter& p, const Rect& inset, const Shape& s) {
    const Point P[3] = {
        Point(X(inset, s.p1.x), Y(inset, s.p1.y)),
        Point(X(inset, s.p2.x), Y(inset, s.p2.y)),
        Point(X(inset, s.p3.x), Y(inset, s.p3.y))
    };

    Rect bbox = Rect(P[0], P[0]); bbox |= P[1]; bbox |= P[2];
    if(bbox.Width() < MIN_EMIT_PX || bbox.Height() < MIN_EMIT_PX) return;

    const int TwiceArea = abs(P[0].x * (P[1].y - P[2].y) +
                              P[1].x * (P[2].y - P[0].y) +
                              P[2].x * (P[0].y - P[1].y));
    if(TwiceArea < MIN_EMIT_PX * MIN_EMIT_PX) return;

    const Style& st = s.style;
    auto Path = [&]{
        p.Move(Pointf(P[0])); p.Line(Pointf(P[1])); p.Line(Pointf(P[2])); p.Close();
    };

    if(st.outlineEnable && st.outlineOutside) Pass_Outline(p, [&]{ p.Begin(); Path(); p.End(); }, st);
    Pass_Fill  (p, [&]{ p.Begin(); Path(); p.End(); }, st);
    Pass_Stroke(p, [&]{ p.Begin(); Path(); p.End(); }, st);
    if(st.outlineEnable && !st.outlineOutside) Pass_Outline(p, [&]{ p.Begin(); Path(); p.End(); }, st);
}

static bool Triangle_HitBody(const Rect& inset, const Shape& s, Point m) {
    Point a(X(inset, s.p1.x), Y(inset, s.p1.y));
    Point b(X(inset, s.p2.x), Y(inset, s.p2.y));
    Point c(X(inset, s.p3.x), Y(inset, s.p3.y));
    if(s.style.enableFill) return IsPointInTriangle(m, a, b, c);
    return IsNearSegment(m, a, b, 6) || IsNearSegment(m, b, c, 6) || IsNearSegment(m, c, a, 6);
}

static int Triangle_HitVertex(const Rect& inset, const Shape& s, Point m) {
    static constexpr int OUTSIDE_BIAS_PX = HANDLE_PX / 2;
    const Point pA(X(inset, s.p1.x), Y(inset, s.p1.y));
    const Point pB(X(inset, s.p2.x), Y(inset, s.p2.y));
    const Point pC(X(inset, s.p3.x), Y(inset, s.p3.y));
    const double cx = (pA.x + pB.x + pC.x) / 3.0;
    const double cy = (pA.y + pB.y + pC.y) / 3.0;

    auto HitOut = [&](const Point& v) -> bool {
        double dx = v.x - cx, dy = v.y - cy;
        double len = sqrt(dx * dx + dy * dy);
        int ox = 0, oy = 0;
        if(len > 1e-6) {
            dx /= len; dy /= len;
            ox = int(dx * OUTSIDE_BIAS_PX + (dx >= 0 ? 0.5 : -0.5));
            oy = int(dy * OUTSIDE_BIAS_PX + (dy >= 0 ? 0.5 : -0.5));
        }
        Rect hit = RectC(v.x - HANDLE_PX, v.y - HANDLE_PX, 2 * HANDLE_PX + 1, 2 * HANDLE_PX + 1);
        hit.Offset(ox, oy);
        return hit.Contains(m);
    };

    if(HitOut(pA)) return 0;
    if(HitOut(pB)) return 1;
    if(HitOut(pC)) return 2;
    return -1;
}

static void Triangle_DrawOverlay(Draw& w, const Rect& inset, const Shape& s) {
    const Color sel = SColorMark();
    Point a(X(inset, s.p1.x), Y(inset, s.p1.y));
    Point b(X(inset, s.p2.x), Y(inset, s.p2.y));
    Point c(X(inset, s.p3.x), Y(inset, s.p3.y));
    w.DrawLine(a, b, 1, sel);
    w.DrawLine(b, c, 1, sel);
    w.DrawLine(c, a, 1, sel);

    auto H = [&](Point p){ w.DrawRect(RectC(p.x - 3, p.y - 3, 7, 7), sel); };
    H(a); H(b); H(c);
}

static void Triangle_BeginCreate(Shape& s, const Rect& inset, Point start_px) {
    s.type = PType::Triangle;
    s.p1 = Pointf(NX(inset, start_px.x), NY(inset, start_px.y));
    s.p2 = s.p1; s.p3 = s.p1;
}

static void Triangle_DragCreate(Shape& s, const Rect& inset, Point, Point cur_px, bool snap, int grid) {
    if(snap){ cur_px.x = Snap1D(cur_px.x, inset.left, grid); cur_px.y = Snap1D(cur_px.y, inset.top, grid); }
    const Pointf p1 = s.p1;
    const Pointf p2 = Pointf(NX(inset, cur_px.x), NY(inset, cur_px.y));
    const Pointf p3 = Pointf(2 * p1.x - p2.x, p2.y);
    s.p2 = p2; s.p3 = p3;
}

static void Triangle_BeginEdit(Shape&, const Rect& inset, Point grab, int, double& gx, double& gy) {
    gx = NX(inset, grab.x); gy = NY(inset, grab.y);
}

static void Triangle_DragEdit(Shape& s, const Rect& inset, Point cur, bool snap, int grid,
                              bool moving, int hv, double& gx, double& gy) {
    if(snap){ cur.x = Snap1D(cur.x, inset.left, grid); cur.y = Snap1D(cur.y, inset.top, grid); }
    const double nx = NX(inset, cur.x), ny = NY(inset, cur.y);
    if(moving){ const Pointf d(nx - gx, ny - gy); s.p1 += d; s.p2 += d; s.p3 += d; gx = nx; gy = ny; return; }
    if(hv == 0) s.p1 = Pointf(nx, ny);
    if(hv == 1) s.p2 = Pointf(nx, ny);
    if(hv == 2) s.p3 = Pointf(nx, ny);
}

static void Triangle_EmitCode(String& out, const Shape& s) {
    out << "    // Triangle\n    p.Begin();\n";
    out << Format(
        "    p.Move(Pointf(X(inset,%s),Y(inset,%s))); "
        "p.Line(Pointf(X(inset,%s),Y(inset,%s))); "
        "p.Line(Pointf(X(inset,%s),Y(inset,%s))); p.Close();\n",
        Decimal4(s.p1.x), Decimal4(s.p1.y),
        Decimal4(s.p2.x), Decimal4(s.p2.y),
        Decimal4(s.p3.x), Decimal4(s.p3.y)
    );
    EmitOpacityCode(out, s.style.opacity);
    EmitDashCode(out, s.style.strokeStyle, s.style.dash);
    if(s.style.evenOdd) out << "    p.EvenOdd(true);\n";
    if(s.style.enableFill)
        out << Format("    p.Fill(Color(%d,%d,%d));\n", s.style.fill.GetR(), s.style.fill.GetG(), s.style.fill.GetB());
    if(s.style.enableStroke)
        out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n", s.style.strokeWidth,
                      s.style.stroke.GetR(), s.style.stroke.GetG(), s.style.stroke.GetB());
    out << "    p.End();\n\n";
}

// ============================================================================
// TEXT (top-aligned; no baseline offset)
// ============================================================================

static Font MakeFontPx(const TextData& td, int pxH) {
    Font f;
    if(!IsNull(td.face)) f.FaceName(td.face);
    f.Height(pxH);
    if(td.bold)   f.Bold();
    if(td.italic) f.Italic();
    return f;
}

static inline Rect TextPixelRect(const Rect& inset, const Shape& s) {
    const TextData& td = s.text;
    const int pxh = max(1, int(inset.Height() * td.sizeN + 0.5));
    Font F = MakeFontPx(td, pxh);

    const String txt = td.text.IsEmpty() ? String("Text") : td.text;
    const Size tsz = GetTextSize(txt, F);

    const int x = X(inset, s.x);
    const int y = Y(inset, s.y);                // TOP (not baseline)
    const int h = max(pxh, tsz.cy);             // prefer larger defensively
    return RectC(x, y, max(tsz.cx, 10), h);
}

static void Text_EmitPainter(BufferPainter& p, const Rect& inset, const Shape& s) {
    const TextData& td = s.text;
    if(td.text.IsEmpty()) return;

    const int pxh = max(0, int(inset.Height() * td.sizeN + 0.5));
    if(pxh < MIN_EMIT_PX) return;

    Font F = MakeFontPx(td, pxh);
    const int top = Y(inset, s.y);
    const Style& st = s.style;

    auto Path = [&]{
        Pointf pen(X(inset, s.x), top);    // TOP (no ascent)
        for(int i = 0; i < td.text.GetCount(); ++i){
            const int ch = td.text[i];
            p.Character(pen, ch, F);       // verified in BufferPainter API  :contentReference[oaicite:2]{index=2}
            pen.x += GetTextSize(String(ch,1), F).cx;
        }
    };

    if(st.outlineEnable && st.outlineOutside) Pass_Outline(p, [&]{ p.Begin(); Path(); p.End(); }, st);
    Pass_Fill  (p, [&]{ p.Begin(); Path(); p.End(); }, st);
    Pass_Stroke(p, [&]{ p.Begin(); Path(); p.End(); }, st);
    if(st.outlineEnable && !st.outlineOutside) Pass_Outline(p, [&]{ p.Begin(); Path(); p.End(); }, st);
}

static bool Text_HitBody(const Rect& inset, const Shape& s, Point m) { return TextPixelRect(inset, s).Inflated(4).Contains(m); }

static int Text_HitVertex(const Rect& inset, const Shape& s, Point m) {
    const Rect r = TextPixelRect(inset, s);
    const Point tl = r.TopLeft();
    const Point tr(r.right, r.top);
    const Point br = r.BottomRight();
    const Point bl(r.left, r.bottom);
    auto Near = [&](Point a){ return abs(a.x-m.x)<=HANDLE_PX && abs(a.y-m.y)<=HANDLE_PX; };
    if(Near(tl)) return 0;
    if(Near(tr)) return 1;
    if(Near(br)) return 2;
    if(Near(bl)) return 3;
    return -1;
}

static void Text_DrawOverlay(Draw& w, const Rect& inset, const Shape& s) {
    const Color sel = SColorMark();
    const Rect  r   = TextPixelRect(inset, s);

    // box
    w.DrawRect(RectC(r.left,  r.top,    r.Width(), 1), sel);
    w.DrawRect(RectC(r.left,  r.bottom, r.Width(), 1), sel);
    w.DrawRect(RectC(r.left,  r.top,    1,         r.Height()), sel);
    w.DrawRect(RectC(r.right, r.top,    1,         r.Height()+1), sel);

    // handles
    auto H = [&](Point p){ w.DrawRect(RectC(p.x - 3, p.y - 3, 7, 7), sel); };
    H(r.TopLeft()); H(Point(r.right, r.top)); H(Point(r.left, r.bottom)); H(r.BottomRight());
}

static void Text_BeginCreate(Shape& s, const Rect& inset, Point start) {
    s.type = PType::Text;
    s.x = NX(inset, start.x);
    s.y = NY(inset, start.y);    // TOP (normalized in drag)
    if(s.text.text.IsEmpty()) s.text.text = "Text";
}

static void Text_DragCreate(Shape& s, const Rect& inset, Point start, Point cur, bool snap, int grid) {
    if(snap) cur.y = Snap1D(cur.y, inset.top, grid);

    const double y0 = NY(inset, start.y);
    const double y1 = NY(inset, cur.y);

    const double top = min(y0, y1);
    const double h   = max(0.02, fabs(y1 - y0));

    s.y          = top;     // TOP
    s.text.sizeN = h;       // normalized height
}

static void Text_BeginEdit(Shape&, const Rect& inset, Point grab, int, double& gx, double& gy) {
    gx = NX(inset, grab.x); gy = NY(inset, grab.y);
}

static void Text_DragEdit(Shape& s, const Rect& inset, Point cur, bool snap, int grid,
                          bool moving, int hv, double& gx, double& gy) {
    if(snap){ cur.x = Snap1D(cur.x, inset.left, grid); cur.y = Snap1D(cur.y, inset.top, grid); }
    const double nx = NX(inset, cur.x), ny = NY(inset, cur.y);

    if(moving){ s.x += nx - gx; s.y += ny - gy; gx = nx; gy = ny; return; }

    double bottom = s.y + s.text.sizeN;
    switch(hv){
        case 0: case 1: // top edge
            s.y = ny;
            if(s.y > bottom) Swap(s.y, bottom);
            s.text.sizeN = max(0.02, bottom - s.y);
            break;
        case 2: case 3: // bottom edge
            bottom = ny;
            if(bottom < s.y) Swap(s.y, bottom);
            s.text.sizeN = max(0.02, bottom - s.y);
            break;
        default: break;
    }
}

static void Text_EmitCode(String& out, const Shape& s) {
    out << "    // Text\n    p.Begin();\n";
    out << "    { Pointf pen(X(inset,"<<Decimal4(s.x)<<"), Y(inset,"<<Decimal4(s.y)<<")); Font F; "
           "F.Height(int(inset.Height()*"<<Decimal4(s.text.sizeN)<<"+0.5)); ";
    if(!s.text.face.IsEmpty()) out << "F.FaceName(\""<<s.text.face<<"\"); ";
    if(s.text.bold)            out << "F.Bold(); ";
    if(s.text.italic)          out << "F.Italic(); ";
    out << "String T=\""<<(s.text.text.IsEmpty() ? String("Text") : s.text.text)<<"\"; "
           "for(int i=0;i<T.GetCount();++i){ int ch=T[i]; p.Character(pen,ch,F); "
           "pen.x += GetTextSize(String(ch,1), F).cx; } }\n";
    EmitOpacityCode(out, s.style.opacity);
    EmitDashCode(out, s.style.strokeStyle, s.style.dash);
    if(s.style.evenOdd) out << "    p.EvenOdd(true);\n";
    if(s.style.enableFill)
        out << Format("    p.Fill(Color(%d,%d,%d));\n", s.style.fill.GetR(), s.style.fill.GetG(), s.style.fill.GetB());
    if(s.style.enableStroke)
        out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n", s.style.strokeWidth,
                      s.style.stroke.GetR(), s.style.stroke.GetG(), s.style.stroke.GetB());
    out << "    p.End();\n\n";
}

// ============================================================================
// CURVE (Quadratic / Cubic; open/closed)
// ============================================================================

static void Curve_EmitPainter(BufferPainter& p, const Rect& inset, const Shape& s) {
    const Style& st = s.style;
    const CurveData& c = s.curve;
    auto P = [&](Pointf q) { return Pointf(X(inset, q.x), Y(inset, q.y)); };

    auto Path = [&]{
        p.Move(P(c.a0));
        if(c.cubic) p.Cubic(P(c.c0), P(c.c1), P(c.a1));
        else        p.Quadratic(P(c.c0), P(c.a1));
        if(c.closed) p.Close();
    };

    if(st.outlineEnable && st.outlineOutside) Pass_Outline(p, [&]{ p.Begin(); Path(); p.End(); }, st);
    if(c.closed) Pass_Fill(p,  [&]{ p.Begin(); Path(); p.End(); }, st);
    Pass_Stroke(p,            [&]{ p.Begin(); Path(); p.End(); }, st);
    if(st.outlineEnable && !st.outlineOutside) Pass_Outline(p, [&]{ p.Begin(); Path(); p.End(); }, st);
}

static bool Curve_HitBody(const Rect& inset, const Shape& s, Point m) {
    auto P = [&](Pointf q) { return Point(X(inset, q.x), Y(inset, q.y)); };
    const CurveData& c = s.curve;
    Point a0 = P(c.a0), a1 = P(c.a1), k0 = P(c.c0), k1 = P(c.c1);
    Rect tight(min(min(a0.x, a1.x), min(k0.x, k1.x)),
               min(min(a0.y, a1.y), min(k0.y, k1.y)),
               max(max(a0.x, a1.x), max(k0.x, k1.x)),
               max(max(a0.y, a1.y), max(k0.y, k1.y)));
    return tight.Inflated(6).Contains(m);
}

static int Curve_HitVertex(const Rect& inset, const Shape& s, Point m) {
    auto P = [&](Pointf q) { return Point(X(inset, q.x), Y(inset, q.y)); };
    const CurveData& c = s.curve;
    Point pts[4] = {P(c.a0), P(c.c0), P(c.c1), P(c.a1)};
    const int n = c.cubic ? 4 : 3;
    for(int i = 0; i < n; ++i)
        if(abs(pts[i].x - m.x) <= HANDLE_PX && abs(pts[i].y - m.y) <= HANDLE_PX)
            return i;
    return -1;
}

static void Curve_DrawOverlay(Draw& w, const Rect& inset, const Shape& s) {
    const Color sel = SColorMark();
    auto P = [&](Pointf q) { return Point(X(inset, q.x), Y(inset, q.y)); };
    const CurveData& c = s.curve;
    Point a0 = P(c.a0), a1 = P(c.a1), k0 = P(c.c0), k1 = P(c.c1);

    w.DrawLine(a0, k0, 1, sel);
    if(c.cubic) w.DrawLine(a1, k1, 1, sel);

    auto H = [&](Point pt) { w.DrawRect(RectC(pt.x - 3, pt.y - 3, 6, 6), sel); };
    H(a0); H(k0); if(c.cubic) H(k1); H(a1);
}

static void Curve_BeginCreate(Shape& s, const Rect& inset, Point start) {
    s.type = PType::Curve;
    Pointf q(NX(inset, start.x), NY(inset, start.y));
    s.curve.a0 = s.curve.a1 = s.curve.c0 = s.curve.c1 = q;
    s.curve.cubic = true; s.curve.closed = false;
}

static void Curve_DragCreate(Shape& s, const Rect& inset, Point, Point cur, bool snap, int grid) {
    if(snap){ cur.x = Snap1D(cur.x, inset.left, grid); cur.y = Snap1D(cur.y, inset.top, grid); }
    s.curve.a1 = Pointf(NX(inset, cur.x), NY(inset, cur.y));
    // seed handles at 1/3 and 2/3 along chord
    s.curve.c0 = Pointf((s.curve.a0.x * 2 + s.curve.a1.x) / 3.0,
                        (s.curve.a0.y * 2 + s.curve.a1.y) / 3.0);
    s.curve.c1 = Pointf((s.curve.a0.x + s.curve.a1.x * 2) / 3.0,
                        (s.curve.a0.y + s.curve.a1.y * 2) / 3.0);
}

static void Curve_BeginEdit(Shape&, const Rect& inset, Point grab, int, double& gx, double& gy) {
    gx = NX(inset, grab.x); gy = NY(inset, grab.y);
}

static void Curve_DragEdit(Shape& s, const Rect& inset, Point cur, bool snap, int grid,
                           bool moving, int hv, double& gx, double& gy) {
    if(snap){ cur.x = Snap1D(cur.x, inset.left, grid); cur.y = Snap1D(cur.y, inset.top, grid); }
    const double nx = NX(inset, cur.x), ny = NY(inset, cur.y);
    if(moving) {
        const Pointf d(nx - gx, ny - gy);
        s.curve.a0 += d; s.curve.a1 += d; s.curve.c0 += d; if(s.curve.cubic) s.curve.c1 += d;
        gx = nx; gy = ny; return;
    }
    Pointf np(nx, ny);
    switch(hv) {
        case 0: s.curve.a0 = np; break;
        case 1: s.curve.c0 = np; break;
        case 2: if(s.curve.cubic) s.curve.c1 = np; break;
        case 3: s.curve.a1 = np; break;
        default: break;
    }
}

static void Curve_EmitCode(String& out, const Shape& s) {
    const CurveData& c = s.curve;
    out << "    // Curve\n    p.Begin();\n";
    out << Format("    p.Move(Pointf(X(inset,%s),Y(inset,%s)));\n", Decimal4(c.a0.x), Decimal4(c.a0.y));
    if(c.cubic)
        out << Format("    p.Cubic(Pointf(X(inset,%s),Y(inset,%s)), "
                      "Pointf(X(inset,%s),Y(inset,%s)), Pointf(X(inset,%s),Y(inset,%s)));\n",
                      Decimal4(c.c0.x), Decimal4(c.c0.y),
                      Decimal4(c.c1.x), Decimal4(c.c1.y),
                      Decimal4(c.a1.x), Decimal4(c.a1.y));
    else
        out << Format("    p.Quadratic(Pointf(X(inset,%s),Y(inset,%s)), "
                      "Pointf(X(inset,%s),Y(inset,%s)));\n",
                      Decimal4(c.c0.x), Decimal4(c.c0.y),
                      Decimal4(c.a1.x), Decimal4(c.a1.y));
    if(c.closed) out << "    p.Close();\n";
    EmitOpacityCode(out, s.style.opacity);
    EmitDashCode(out, s.style.strokeStyle, s.style.dash);
    if(s.style.evenOdd) out << "    p.EvenOdd(true);\n";
    if(c.closed && s.style.enableFill)
        out << Format("    p.Fill(Color(%d,%d,%d));\n", s.style.fill.GetR(), s.style.fill.GetG(), s.style.fill.GetB());
    if(s.style.enableStroke)
        out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n", s.style.strokeWidth,
                      s.style.stroke.GetR(), s.style.stroke.GetG(), s.style.stroke.GetB());
    out << "    p.End();\n\n";
}

// =============================== Registry build ==============================

struct FacetRow : Moveable<FacetRow> {
    PType t;
    PrimitiveOps ops;
    ToolSpec spec;
};

static Vector<FacetRow>& Facets() {
    static Vector<FacetRow> F;
    if(!F.IsEmpty()) return F;

    auto add = [&](PType t, PrimitiveOps ops, const char* label, const char* tip) {
        FacetRow& r = F.Add();
        r.t = t; r.ops = ops;
        r.spec.type = t; r.spec.label = label; r.spec.tip = tip;
    };

    PrimitiveOps R { Rect_EmitPainter,    Rect_HitBody,    Rect_HitVertex,
                     Rect_DrawOverlay,    Rect_BeginCreate,Rect_DragCreate,
                     Rect_BeginEdit,      Rect_DragEdit,   Rect_EmitCode };
    PrimitiveOps C { Circle_EmitPainter,  Circle_HitBody,  Circle_HitVertex,
                     Circle_DrawOverlay,  Circle_BeginCreate, Circle_DragCreate,
                     Circle_BeginEdit,    Circle_DragEdit, Circle_EmitCode };
    PrimitiveOps L { Line_EmitPainter,    Line_HitBody,    Line_HitVertex,
                     Line_DrawOverlay,    Line_BeginCreate, Line_DragCreate,
                     Line_BeginEdit,      Line_DragEdit,   Line_EmitCode };
    PrimitiveOps T { Triangle_EmitPainter,Triangle_HitBody,Triangle_HitVertex,
                     Triangle_DrawOverlay,Triangle_BeginCreate,Triangle_DragCreate,
                     Triangle_BeginEdit,  Triangle_DragEdit,  Triangle_EmitCode };
    PrimitiveOps TX{ Text_EmitPainter,    Text_HitBody,    Text_HitVertex,
                     Text_DrawOverlay,    Text_BeginCreate, Text_DragCreate,
                     Text_BeginEdit,      Text_DragEdit,   Text_EmitCode };
    PrimitiveOps CV{ Curve_EmitPainter,   Curve_HitBody,   Curve_HitVertex,
                     Curve_DrawOverlay,   Curve_BeginCreate, Curve_DragCreate,
                     Curve_BeginEdit,     Curve_DragEdit,  Curve_EmitCode };

    add(PType::Rect,     R,  "Rect",     "Insert rectangle");
    add(PType::Circle,   C,  "Circle",   "Insert circle");
    add(PType::Line,     L,  "Line",     "Insert line");
    add(PType::Triangle, T,  "Triangle", "Insert triangle");
    add(PType::Text,     TX, "Text",     "Insert text");
    add(PType::Curve,    CV, "Curve",    "Insert curve");

    return F;
}

const PrimitiveOps& GetOps(PType t) {
    const Vector<FacetRow>& v = Facets();
    for(const FacetRow& r : v) if(r.t == t) return r.ops;
    return v[0].ops; // fallback
}
const Vector<ToolSpec>& GetToolSpecs() {
    static Vector<ToolSpec> S;
    if(!S.IsEmpty()) return S;
    for(const FacetRow& r : Facets()) S.Add(r.spec);
    return S;
}

// ========================== Canvas (model / view / IO) =======================

struct Canvas : Ctrl {
    // ---------- Model ----------
    Vector<Shape> shapes;
    int    selected = -1;

    // Interaction / tool
    Tool   tool = Tool::Cursor;
    PType  creation_type = PType::Rect;
    bool   snap = true;
    bool   clip = true;
    int    grid = 8;

    // Clip/aspect
    struct Aspect { int w, h; const char* label; };
    inline static constexpr Aspect ASPECTS[] = {
        Aspect{1,1,"1:1"}, Aspect{4,3,"4:3"}, Aspect{3,2,"3:2"}, Aspect{16,9,"16:9"},
        Aspect{21,9,"21:9"}, Aspect{9,16,"9:16"}, Aspect{2,3,"2:3"}, Aspect{3,4,"3:4"}
    };
    inline static constexpr int ASPECT_COUNT = (int)(sizeof(ASPECTS)/sizeof(ASPECTS[0]));
    int    aspect_ix    = 0;      // default 1:1
    double clip_scale_n = 0.80;   // clip rect vs canvas
    Color  bg_color     = SColorFace();
    int    sample_width = 128;

    // Drag state
    bool   creating = false;
    bool   editing  = false;
    bool   moving   = false;
    int    drag_vertex = -1;
    Point  start_px;
    double grab_nx = 0.0, grab_ny = 0.0;

    // Text template for new text primitives
    String text_template = "Text";

    // Callbacks
    Callback WhenSelection;
    Callback WhenShapesChanged;

    // ---------- Helpers ----------
    Rect GetInsetRect() const {
        const Size sz = GetSize();
        const int baseW = int(sz.cx * clip_scale_n + 0.5);
        const int baseH = int(sz.cy * clip_scale_n + 0.5);

        const Aspect a = ASPECTS[Upp::clamp(aspect_ix, 0, ASPECT_COUNT - 1)];
        const double ar = double(a.w) / double(a.h);

        int w = baseW;
        int h = int(w / ar + 0.5);
        if(h > baseH) { h = baseH; w = int(h * ar + 0.5); }

        const int L = (sz.cx - w) / 2;
        const int T = (sz.cy - h) / 2;
        return RectC(L, T, w, h);
    }

    // grid visibility (visual only)
    bool show_grid = true;

    // ---------- History ----------
    Vector<String> hist;
    int hist_ix = -1;

    void PushHist() {
        String s = SaveJson();
        if(hist_ix + 1 < hist.GetCount())
            hist.Trim(hist_ix + 1);
        hist.Add(s);
        hist_ix = hist.GetCount() - 1;
    }
    bool Undo() { if(hist_ix <= 0) return false; hist_ix--; LoadJson(hist[hist_ix]); return true; }
    bool Redo() { if(hist_ix + 1 >= hist.GetCount()) return false; hist_ix++; LoadJson(hist[hist_ix]); return true; }

    // ---------- Pixel helpers (Canvas scope) ----------
    static inline int     X(const Rect& r, double nx) { return r.left + int(r.Width() * nx + 0.5); }
    static inline int     Y(const Rect& r, double ny) { return r.top  + int(r.Height()* ny + 0.5); }
    static inline int     R(const Rect& r, double nr) { return int(min(r.Width(), r.Height()) * nr + 0.5); }
    static inline double  NX(const Rect& r, int px)   { return (px - r.left) / double(max(1, r.Width())); }
    static inline double  NY(const Rect& r, int py)   { return (py - r.top)  / double(max(1, r.Height())); }
    static inline int     Snap1D(int v, int origin, int step) { return origin + ((v - origin + step / 2) / step) * step; }

    // ---------- Defaults ----------
    Canvas() {
        snap = true; clip = true; grid = 8;
        aspect_ix = 0; clip_scale_n = 0.80;
        bg_color = SColorFace(); sample_width = 128;
        text_template = "Text";
    }

    // ============================ Painting ===================================
    void Paint(Draw& w) override {
        const Size sz = GetSize();
        w.DrawRect(sz, bg_color);

        const Rect ir = GetInsetRect();

        // Offscreen layer (opaque) — cheap double-buffering
        ImageBuffer ib(sz);
        ib.SetKind(IMAGE_OPAQUE);
        BufferPainter p(ib, MODE_ANTIALIASED);
        p.Clear(bg_color);

        // Inset design area
        p.DrawRect(ir, White());

        // Grid under shapes (not clipped)
        if(show_grid && grid > 0) {
            const Color gc = Color(200, 230, 230);
            for(int x = ir.left + grid; x < ir.right; x += grid)
                p.DrawRect(RectC(x, ir.top, 1, ir.Height()), gc);
            for(int y = ir.top + grid; y < ir.bottom; y += grid)
                p.DrawRect(RectC(ir.left, y, ir.Width(), 1), gc);
        }

        // Optional clip
        if(clip) {
            p.Begin();
            p.Move(Pointf(ir.left, ir.top));
            p.Line(Pointf(ir.right, ir.top));
            p.Line(Pointf(ir.right, ir.bottom));
            p.Line(Pointf(ir.left, ir.bottom));
            p.Close();
            p.Clip();     // keep clip active
        }

        // Shapes
        for(const Shape& s : shapes)
            GetOps(s.type).EmitPainter(p, ir, s);

        if(clip) p.End(); // pop clip

        // Blit
        w.DrawImage(0, 0, Image(ib));

        // Overlay selected (never clipped)
        if(selected >= 0 && selected < shapes.GetCount())
            GetOps(shapes[selected].type).DrawOverlay(w, ir, shapes[selected]);

        // Clip frame (always visible)
        const Color frame = SColorMark();
        w.DrawRect(RectC(ir.left,  ir.top,    ir.Width(), 1), frame);
        w.DrawRect(RectC(ir.left,  ir.bottom, ir.Width(), 1), frame);
        w.DrawRect(RectC(ir.left,  ir.top,    1,           ir.Height()), frame);
        w.DrawRect(RectC(ir.right, ir.top,    1,           ir.Height()+1), frame);
    }

    // ============================ Mouse ======================================
    void LeftDown(Point p, dword) override {
        SetFocus();
        SetCapture();

        const Rect ir = GetInsetRect();

        creating = editing = moving = false;
        drag_vertex = -1;

        if(tool == Tool::Cursor) {
            // 1) vertex first, top-most
            int vshape = -1, vindex = -1;
            for(int i = shapes.GetCount() - 1; i >= 0; --i) {
                int hv = GetOps(shapes[i].type).HitVertex(ir, shapes[i], p);
                if(hv >= 0) { vshape = i; vindex = hv; break; }
            }
            if(vshape >= 0) {
                if(selected != vshape) { selected = vshape; if(WhenSelection) WhenSelection(); }
                Shape& s = shapes[selected];
                drag_vertex = vindex;
                GetOps(s.type).BeginEdit(s, ir, p, drag_vertex, grab_nx, grab_ny);
                editing = true; moving = false; Refresh(); return;
            }

            // 2) body hit
            int pick = -1;
            for(int i = shapes.GetCount() - 1; i >= 0; --i)
                if(GetOps(shapes[i].type).HitBody(ir, shapes[i], p)) { pick = i; break; }

            if(selected != pick) { selected = pick; if(WhenSelection) WhenSelection(); }
            if(selected >= 0) {
                Shape& s = shapes[selected];
                drag_vertex = -1;
                GetOps(s.type).BeginEdit(s, ir, p, drag_vertex, grab_nx, grab_ny);
                editing = true; moving = true;
            }
            Refresh(); return;
        }

        // Creation
        if(tool == Tool::CreateShape && ir.Contains(p)) {
            start_px = snap ? Point(Snap1D(p.x, ir.left, grid), Snap1D(p.y, ir.top, grid)) : p;

            Shape s; s.type = creation_type;
            if(s.type == PType::Text)
                s.text.text = text_template.IsEmpty() ? "Text" : text_template;

            shapes.Add(s);
            selected = shapes.GetCount() - 1;

            GetOps(shapes[selected].type).BeginCreate(shapes[selected], ir, start_px);
            creating = true;
            if(WhenSelection) WhenSelection();
            Refresh();
        }
    }

    void MouseMove(Point p, dword) override {
        if(!HasCapture()) return;
        const Rect ir = GetInsetRect();

        if(creating && selected >= 0) {
            GetOps(shapes[selected].type).DragCreate(shapes[selected], ir, start_px, p, snap, grid);
            Refresh();
        }
        else if(editing && selected >= 0) {
            if(!GetMouseLeft()) { // lost button? finalize
                ReleaseCapture();
                creating = editing = moving = false;
                drag_vertex = -1;
                if(WhenShapesChanged) WhenShapesChanged();
                Refresh(); return;
            }
            GetOps(shapes[selected].type).DragEdit(shapes[selected], ir, p, snap, grid,
                                                   moving, drag_vertex, grab_nx, grab_ny);
            Refresh();
        }
    }

    void LeftUp(Point, dword) override {
        if(!HasCapture()) return;
        ReleaseCapture();
        bool changed = creating || editing;
        creating = editing = moving = false;
        drag_vertex = -1;
        if(changed && WhenShapesChanged) WhenShapesChanged();
        PushHist();
        Refresh();
    }

    bool Key(dword key, int) override {
        if(key == K_DELETE){ DeleteSelected(); return true; }
        return false;
    }

    // ============================ Actions ====================================

    void ClearAll() {
        shapes.Clear(); selected = -1;
        if(WhenShapesChanged) WhenShapesChanged();
        PushHist(); Refresh();
    }

    void DeleteSelected() {
        if(selected >= 0 && selected < shapes.GetCount()) {
            shapes.Remove(selected);
            selected = -1;
            if(WhenSelection)     WhenSelection();
            if(WhenShapesChanged) WhenShapesChanged();
            PushHist(); Refresh();
        }
    }

    void ResetStyleSelectedOrDefaults() {
        if(selected >= 0 && selected < shapes.GetCount())
            shapes[selected].style = ::Style(); // default-constructed
        if(WhenSelection)     WhenSelection();
        if(WhenShapesChanged) WhenShapesChanged();
        PushHist(); Refresh();
    }

    void LayerUp() {
        if(selected < 0 || selected >= shapes.GetCount() - 1) return;
        Swap(shapes[selected], shapes[selected + 1]);
        selected++;
        if(WhenShapesChanged) WhenShapesChanged();
        PushHist(); Refresh();
    }

    void LayerDown() {
        if(selected <= 0 || selected >= shapes.GetCount()) return;
        Swap(shapes[selected], shapes[selected - 1]);
        selected--;
        if(WhenShapesChanged) WhenShapesChanged();
        PushHist(); Refresh();
    }

    void DuplicateSelected() {
        if(selected < 0 || selected >= shapes.GetCount()) return;
        Shape s = shapes[selected];
        const Rect ir = GetInsetRect();
        const double dx = 6.0 / max(1, ir.Width());
        const double dy = 6.0 / max(1, ir.Height());
        switch(s.type) {
            case PType::Rect:     s.x += dx; s.y += dy; break;
            case PType::Circle:   s.cx += dx; s.cy += dy; break;
            case PType::Line:     s.p1 += Pointf(dx,dy); s.p2 += Pointf(dx,dy); break;
            case PType::Triangle: s.p1 += Pointf(dx,dy); s.p2 += Pointf(dx,dy); s.p3 += Pointf(dx,dy); break;
            case PType::Text:     s.x += dx; s.y += dy; break;
            case PType::Curve:    s.curve.a0 += Pointf(dx,dy); s.curve.a1 += Pointf(dx,dy);
                                  s.curve.c0 += Pointf(dx,dy); s.curve.c1 += Pointf(dx,dy); break;
        }
        shapes.Insert(++selected, s);
        if(WhenSelection) WhenSelection();
        if(WhenShapesChanged) WhenShapesChanged();
        PushHist(); Refresh();
    }

    void FlipXSelected() {
        if(selected < 0 || selected >= shapes.GetCount()) return;
        const double cx = 0.5; // normalized center
        Shape& s = shapes[selected];
        switch(s.type) {
            case PType::Rect:     s.x = 2 * cx - (s.x + s.w); break;
            case PType::Circle:   s.cx = 2 * cx - s.cx; break;
            case PType::Line:     s.p1.x = 2 * cx - s.p1.x; s.p2.x = 2 * cx - s.p2.x; break;
            case PType::Triangle: s.p1.x = 2 * cx - s.p1.x; s.p2.x = 2 * cx - s.p2.x; s.p3.x = 2 * cx - s.p3.x; break;
            case PType::Text:     s.x = 2 * cx - s.x; break;
            case PType::Curve:    s.curve.a0.x = 2 * cx - s.curve.a0.x;
                                  s.curve.a1.x = 2 * cx - s.curve.a1.x;
                                  s.curve.c0.x = 2 * cx - s.curve.c0.x;
                                  s.curve.c1.x = 2 * cx - s.curve.c1.x; break;
        }
        if(WhenShapesChanged) WhenShapesChanged();
        PushHist(); Refresh();
    }

    void FlipYSelected() {
        if(selected < 0 || selected >= shapes.GetCount()) return;
        const double cy = 0.5;
        Shape& s = shapes[selected];
        switch(s.type) {
            case PType::Rect:     s.y = 2 * cy - (s.y + s.h); break;
            case PType::Circle:   s.cy = 2 * cy - s.cy; break;
            case PType::Line:     s.p1.y = 2 * cy - s.p1.y; s.p2.y = 2 * cy - s.p2.y; break;
            case PType::Triangle: s.p1.y = 2 * cy - s.p1.y; s.p2.y = 2 * cy - s.p2.y; s.p3.y = 2 * cy - s.p3.y; break;
            case PType::Text:     s.y = 2 * cy - s.y; break;
            case PType::Curve:    s.curve.a0.y = 2 * cy - s.curve.a0.y;
                                  s.curve.a1.y = 2 * cy - s.curve.a1.y;
                                  s.curve.c0.y = 2 * cy - s.curve.c0.y;
                                  s.curve.c1.y = 2 * cy - s.curve.c1.y; break;
        }
        if(WhenShapesChanged) WhenShapesChanged();
        PushHist(); Refresh();
    }

    void ResetDefaults() {
        shapes.Clear(); selected = -1;
        snap = true; clip = true; grid = 8;
        aspect_ix = 0; clip_scale_n = 0.80;
        text_template = "Text";
        if(WhenShapesChanged) WhenShapesChanged();
        PushHist(); Refresh();
    }

    // ============================ JSON I/O ===================================

    // Safe getters (ValueMap has no Get; operator[] yields const Value)
    static inline Value  VM(ValueMap m, const char* k)           { return m[k]; }
    static inline int    VI(ValueMap m, const char* k, int def)  { Value v = m[k]; return IsNumber(v) ? (int)v : def; }
    static inline double VD(ValueMap m, const char* k, double d) { Value v = m[k]; return IsNumber(v) ? (double)v : d; }
    static inline bool   VB(ValueMap m, const char* k, bool def) { Value v = m[k]; return IsNumber(v) ? ((int)v)!=0 : def; }
    static inline String VS(ValueMap m, const char* k, const String& def) { Value v = m[k]; return IsString(v) ? (String)v : def; }

    ValueMap StyleToVM(const ::Style& s) const {
        ValueMap m;
        m("fill_r", s.fill.GetR())("fill_g", s.fill.GetG())("fill_b", s.fill.GetB())
         ("stroke_r", s.stroke.GetR())("stroke_g", s.stroke.GetG())("stroke_b", s.stroke.GetB())
         ("stroke_w", s.strokeWidth)("evenOdd", s.evenOdd)("dash", s.dash)
         ("enableFill", s.enableFill)("enableStroke", s.enableStroke)
         ("opacity", s.opacity)("fillOpacity", s.fillOpacity)("strokeOpacity", s.strokeOpacity)
         ("outlineEnable", s.outlineEnable)("outlineOutside", s.outlineOutside)
         ("outline_r", s.outlineColor.GetR())("outline_g", s.outlineColor.GetG())("outline_b", s.outlineColor.GetB())
         ("outline_w", s.outlineWidth)("outlineOpacity", s.outlineOpacity)
         ("strokeStyle", ToI(s.strokeStyle))("outlineStyle", ToI(s.outlineStyle))
         ("outlineDash", s.outlineDash)("outlineOffsetX", s.outlineOffsetX)("outlineOffsetY", s.outlineOffsetY);
        return m;
    }

    void StyleFrom(::Style& s, ValueMap m) {
        s.fill        = Color(VI(m,"fill_r",163), VI(m,"fill_g",201), VI(m,"fill_b",168));
        s.stroke      = Color(VI(m,"stroke_r",30), VI(m,"stroke_g",53), VI(m,"stroke_b",47));
        s.strokeWidth = VI(m,"stroke_w",2);
        s.evenOdd     = VB(m,"evenOdd",false);
        s.dash        = VS(m,"dash",String());
        s.enableFill  = VB(m,"enableFill",true);
        s.enableStroke= VB(m,"enableStroke",true);

        s.opacity       = VD(m,"opacity",1.0);
        s.fillOpacity   = VD(m,"fillOpacity",1.0);
        s.strokeOpacity = VD(m,"strokeOpacity",1.0);

        s.outlineEnable   = VB(m,"outlineEnable",false);
        s.outlineOutside  = VB(m,"outlineOutside",true);
        s.outlineColor    = Color(VI(m,"outline_r",255), VI(m,"outline_g",0), VI(m,"outline_b",0));
        s.outlineWidth    = VI(m,"outline_w",0);
        s.outlineOpacity  = VD(m,"outlineOpacity",1.0);
        s.strokeStyle     = FromI(VI(m,"strokeStyle",0));
        s.outlineStyle    = FromI(VI(m,"outlineStyle",0));
        s.outlineDash     = VS(m,"outlineDash",String());
        s.outlineOffsetX  = VI(m,"outlineOffsetX",0);
        s.outlineOffsetY  = VI(m,"outlineOffsetY",0);
    }

    String SaveJson() const {
        ValueMap root;
        root("snap", snap)("clip", clip)("grid", grid)("aspect_ix", aspect_ix)
            ("clip_scale_n", clip_scale_n)
            ("bg_r", bg_color.GetR())("bg_g", bg_color.GetG())("bg_b", bg_color.GetB())
            ("text_template", text_template);

        ValueArray a;
        for(const Shape& s : shapes) {
            ValueMap sh;
            sh("type", (int)s.type)("style", StyleToVM(s.style));
            switch(s.type) {
                case PType::Rect:     sh("x", s.x)("y", s.y)("w", s.w)("h", s.h); break;
                case PType::Circle:   sh("cx", s.cx)("cy", s.cy)("r", s.r); break;
                case PType::Line:     sh("p1x", s.p1.x)("p1y", s.p1.y)("p2x", s.p2.x)("p2y", s.p2.y); break;
                case PType::Triangle: sh("p1x", s.p1.x)("p1y", s.p1.y)("p2x", s.p2.x)("p2y", s.p2.y)
                                           ("p3x", s.p3.x)("p3y", s.p3.y); break;
                case PType::Text:     sh("x", s.x)("y", s.y)("txt", s.text.text)("face", s.text.face)
                                           ("sizeN", s.text.sizeN)("bold", s.text.bold)("italic", s.text.italic); break;
                case PType::Curve:    sh("cubic", s.curve.cubic)("closed", s.curve.closed)
                                           ("a0x", s.curve.a0.x)("a0y", s.curve.a0.y)
                                           ("a1x", s.curve.a1.x)("a1y", s.curve.a1.y)
                                           ("c0x", s.curve.c0.x)("c0y", s.curve.c0.y)
                                           ("c1x", s.curve.c1.x)("c1y", s.curve.c1.y); break;
            }
            a.Add(sh);
        }
        root("shapes", a);
        return StoreAsJson(root, true); // pretty
    }

    void LoadJson(const String& js) {
        Value v = ParseJSON(~js);      // JSON APIs in U++ Core  :contentReference[oaicite:3]{index=3}  :contentReference[oaicite:4]{index=4}
        if(IsNull(v)) { Refresh(); return; }

        ValueMap root;
        if(IsValueMap(v)) root = v;
        else if(IsPairArrayValueMap(v)) root = PairArrayToMap(ValueArray(v));
        else { Refresh(); return; }

        snap         = VB(root, "snap", true);
        clip         = VB(root, "clip", true);
        grid         = VI(root, "grid", 8);
        aspect_ix    = VI(root, "aspect_ix", 0);
        clip_scale_n = VD(root, "clip_scale_n", 0.80);
        bg_color     = Color(VI(root,"bg_r", SColorFace().GetR()),
                             VI(root,"bg_g", SColorFace().GetG()),
                             VI(root,"bg_b", SColorFace().GetB()));
        text_template = VS(root, "text_template", "Text");

        shapes.Clear();
        ValueArray a = ValueArray(root["shapes"]);
        for(int i = 0; i < a.GetCount(); ++i) {
            if(!IsValueMap(a[i])) continue;
            ValueMap sh = a[i];

            Shape s;
            s.type = (PType)VI(sh, "type", (int)PType::Rect);

            Value stv = sh["style"];
            if(IsValueMap(stv)) StyleFrom(s.style, ValueMap(stv));

            switch(s.type) {
                case PType::Rect:
                    s.x = VD(sh, "x", 0); s.y = VD(sh, "y", 0);
                    s.w = VD(sh, "w", 0); s.h = VD(sh, "h", 0);
                    break;
                case PType::Circle:
                    s.cx = VD(sh, "cx", 0); s.cy = VD(sh, "cy", 0); s.r = VD(sh, "r", 0);
                    break;
                case PType::Line:
                    s.p1 = Pointf(VD(sh,"p1x",0), VD(sh,"p1y",0));
                    s.p2 = Pointf(VD(sh,"p2x",0), VD(sh,"p2y",0));
                    break;
                case PType::Triangle:
                    s.p1 = Pointf(VD(sh,"p1x",0), VD(sh,"p1y",0));
                    s.p2 = Pointf(VD(sh,"p2x",0), VD(sh,"p2y",0));
                    s.p3 = Pointf(VD(sh,"p3x",0), VD(sh,"p3y",0));
                    break;
                case PType::Text:
                    s.x = VD(sh,"x",0); s.y = VD(sh,"y",0);
                    s.text.text   = VS(sh,"txt","Text");
                    s.text.face   = VS(sh,"face",String());
                    s.text.sizeN  = VD(sh,"sizeN",0.18);
                    s.text.bold   = VB(sh,"bold",false);
                    s.text.italic = VB(sh,"italic",false);
                    break;
                case PType::Curve:
                    s.curve.cubic  = VB(sh,"cubic",true);
                    s.curve.closed = VB(sh,"closed",false);
                    s.curve.a0 = Pointf(VD(sh,"a0x",0), VD(sh,"a0y",0));
                    s.curve.a1 = Pointf(VD(sh,"a1x",0), VD(sh,"a1y",0));
                    s.curve.c0 = Pointf(VD(sh,"c0x",0), VD(sh,"c0y",0));
                    s.curve.c1 = Pointf(VD(sh,"c1x",0), VD(sh,"c1y",0));
                    break;
            }
            shapes.Add(s);
        }
        selected = shapes.IsEmpty() ? -1 : shapes.GetCount() - 1;
        if(WhenSelection)     WhenSelection();
        if(WhenShapesChanged) WhenShapesChanged();
        Refresh();
    }
};

// ===================== Main Window (UI, wiring, code panel) ==================

struct MainWin : TopWindow {
    typedef MainWin CLASSNAME;

    // Layout
    Splitter   split;
    ParentCtrl left, right;
    StaticRect rowTools, rowOps, rowActions, rowStyle, rowCanvas;
    ParentCtrl toolbox;
    TimeCallback codes_debounce; // (reserved for future use; harmless)

    // Tools
    Button bCursor;

    // Ops row
    Option cbSnap, cbClip, cbBgEnable, cbShowGrid;
    Label  lblGrid, lblAspect, lblSample, lblSel;
    EditInt edGrid;
    DropList dlAspect, dlSampleRes;

    // Actions row
    Button bDup, bUndo, bRedo, bFlipX, bFlipY, bReset, bDelete, bLayerUp, bLayerDown, bLoad,
           bSave, bBg, bClear;
    ColorPusher cBg;

    // Style row widgets
    Option cbFill, cbStroke, cbEvenOdd, cbOutline;
    ColorPusher cFill, cStroke, cOutline;
    Label lblStrokeW, lblOutW, lblOpacity, lblDash, lblFillOp, lblStrokeOp, lblOutOp;
    EditIntSpin spinStrokeW, spinOutlineW;
    EditIntSpin esOutlineOffX, esOutlineOffY;
    EditString  edDash, edOutlineDash;
    DropList    dlStrokeType, dlOutlineType;

    // Per-channel opacity sliders
    SliderCtrl sFillOpacity, sStrokeOpacity, sOutlineOpacity;

    // Text subrow
    Label lblText, lblFont;
    EditString edText, edCodes;
    DropList  dlFont;
    Option    cbBold, cbItalic;

    // Labels (outline/stroke/text)
    Label lblOlType, lblOlOp, lblOlDash, lblOff, lblCodes;
    Label lblStType, lblStOp, lblStDash;

    // Code panel
    StaticRect  codeHdr;
    ParentCtrl  codeHdrBox;
    Label       codeTitle;
    Button      bCopy;
    DocEdit     code;

    // Canvas
    Canvas canvas;

    // ------------------- helpers -------------------
    void UpdateCode() {
        String out;
        out << "void DrawIcon(Draw& w, const Rect& inset)\n{\n";
        out << "    BufferPainter p(w, MODE_ANTIALIASED);\n\n";
        for(const Shape& s : canvas.shapes)
            GetOps(s.type).EmitCode(out, s);
        out << "}\n";
        code <<= out;
    }

    void PushStyleToUI() {
        if(canvas.selected < 0 || canvas.selected >= canvas.shapes.GetCount()) return;
        const ::Style& st = canvas.shapes[canvas.selected].style;

        cbFill       = st.enableFill;
        cbStroke     = st.enableStroke;
        cbEvenOdd    = st.evenOdd;
        cbOutline    = st.outlineEnable;

        cFill        <<= st.fill;
        cStroke      <<= st.stroke;
        cOutline     <<= st.outlineColor;

        spinStrokeW  <<= st.strokeWidth;
        spinOutlineW <<= st.outlineWidth;

        // sliders are 0..100 ints
        sFillOpacity   <<= Upp::clamp(int(st.fillOpacity   * 100 + 0.5), 0, 100);
        sStrokeOpacity <<= Upp::clamp(int(st.strokeOpacity * 100 + 0.5), 0, 100);
        sOutlineOpacity<<= Upp::clamp(int(st.outlineOpacity* 100 + 0.5), 0, 100);

        // dash preset + strings
        dlStrokeType.SetIndex(ToI(st.strokeStyle));
        dlOutlineType.SetIndex(ToI(st.outlineStyle));

        edDash        <<= st.dash;
        edOutlineDash <<= st.outlineDash;

        esOutlineOffX <<= st.outlineOffsetX;
        esOutlineOffY <<= st.outlineOffsetY;

        // If Text selected: mirror text & font selection
        if(canvas.shapes[canvas.selected].type == PType::Text) {
            const TextData& td = canvas.shapes[canvas.selected].text;
            edText <<= td.text;
            edCodes <<= (td.text.IsEmpty() ? String() : AsString((int)(byte)td.text[0]));
            int face_ix = 0;
            for(int i = 0; i < dlFont.GetCount(); ++i) {
                if((String)dlFont.GetValue(i) == td.face) { face_ix = i; break; }
            }
            dlFont.SetIndex(face_ix);
            cbBold   = td.bold;
            cbItalic = td.italic;
            MirrorTextUIFromSelection();
        }
    }

    void PullStyleFromUI() {
        if(canvas.selected < 0 || canvas.selected >= canvas.shapes.GetCount()) return;
        ::Style& st = canvas.shapes[canvas.selected].style;

        st.enableFill   = (bool)~cbFill;
        st.enableStroke = (bool)~cbStroke;
        st.evenOdd      = (bool)~cbEvenOdd;
        st.outlineEnable= (bool)~cbOutline;

        st.fill        = (Color)~cFill;
        st.stroke      = (Color)~cStroke;
        st.outlineColor= (Color)~cOutline;

        st.strokeWidth = (int)~spinStrokeW;
        st.outlineWidth= (int)~spinOutlineW;

        st.fillOpacity   = Upp::clamp(((int)~sFillOpacity)   / 100.0, 0.0, 1.0);
        st.strokeOpacity = Upp::clamp(((int)~sStrokeOpacity) / 100.0, 0.0, 1.0);
        st.outlineOpacity= Upp::clamp(((int)~sOutlineOpacity)/ 100.0, 0.0, 1.0);

        st.strokeStyle = FromI(dlStrokeType.GetIndex());
        st.outlineStyle= FromI(dlOutlineType.GetIndex());

        st.dash        = ~edDash;
        st.outlineDash = ~edOutlineDash;

        st.outlineOffsetX = (int)~esOutlineOffX;
        st.outlineOffsetY = (int)~esOutlineOffY;

        canvas.Refresh();
        UpdateCode();
    }

    void OnSnap() { canvas.snap = (bool)~cbSnap; canvas.Refresh(); }
    void OnClip() { canvas.clip = (bool)~cbClip; canvas.Refresh(); }
    void OnGrid() {
        int g = (int)~edGrid;
        g = Upp::clamp(g, 2, 64);
        canvas.grid = g;
        canvas.Refresh();
    }

    void OnSelectionChanged() { PushStyleToUI(); }
    void OnShapesChanged()    { UpdateCode(); }
    void OnCopyCode()         { WriteClipboardText(~code); PromptOK("Code copied to clipboard."); }

    // --- simple horizontal cursor for row layout
    int colStart = 3, colPad = 6, rowPad = 3, colX = 3;
    int ColPos(int width, bool reset = false, bool noPad = false) {
        if(reset) colX = colStart;
        const int cur = colX; colX += width + (noPad ? 0 : colPad); return cur;
    }

    // Tool buttons
    void BuildToolButtons() {
        int x = 6;
        toolbox.Add(bCursor.LeftPos(x, 80).VSizePos(6, 6));
        bCursor.SetLabel("Cursor");
        bCursor << [=] { canvas.tool = Tool::Cursor; };
        x += 86;

        for(const ToolSpec& sp : GetToolSpecs()) {
            Button& b = *new Button;
            b.SetLabel(sp.label);
            b.Tip(sp.tip);
            b.WhenAction = [=] { canvas.tool = Tool::CreateShape; canvas.creation_type = sp.type; };
            toolbox.Add(b.LeftPos(x, 90).VSizePos(6, 6));
            x += 96;
        }
    }

    // System fonts → DropList (Font enumeration in Draw.h)  :contentReference[oaicite:5]{index=5}
    void LoadSystemFonts() {
        dlFont.Clear();
        int n = Font::GetFaceCount();
        for(int i = 0; i < n; ++i)
            dlFont.Add(Font::GetFaceName(i), Font::GetFaceName(i));
        if(dlFont.GetCount() == 0)
            dlFont.Add("Default", "");
        dlFont.SetIndex(0);
    }

    void MirrorTextUIFromSelection() {
        if(canvas.selected < 0 || canvas.selected >= canvas.shapes.GetCount()) return;
        if(canvas.shapes[canvas.selected].type != PType::Text) return;
        const TextData& td = canvas.shapes[canvas.selected].text;

        edText  <<= td.text;

        String codes;
        for(int i = 0; i < td.text.GetCount(); ++i) {
            if(i) codes << ',';
            codes << AsString((int)(byte)td.text[i]);
        }
        edCodes <<= codes;

        int face_ix = 0;
        for(int i = 0; i < dlFont.GetCount(); ++i)
            if((String)dlFont.GetValue(i) == td.face) { face_ix = i; break; }
        dlFont.SetIndex(face_ix);
        cbBold = td.bold; cbItalic = td.italic;
    }

    void DoSave() {
        FileSel fs; fs.Type("JSON", "*.json");
        if(!fs.ExecuteSaveAs("Save icon JSON")) return;
        SaveFile(~fs, canvas.SaveJson());
    }
    void DoLoad() {
        FileSel fs; fs.Type("JSON", "*.json");
        if(!fs.ExecuteOpen("Load icon JSON")) return;
        canvas.LoadJson(LoadFile(~fs));
        UpdateCode();
    }
    
	// Constructor
	MainWin()
	{
		Title("U++ Icon Builder — v0.8").Sizeable().Zoomable();

		Add(split.SizePos());
		split.Horz(left, right);
		split.SetPos(6000);

		// Left column layout
		left.Add(rowTools.TopPos(0, 40).HSizePos());
		left.Add(rowOps.TopPos(40, 28).HSizePos());
		left.Add(rowActions.TopPos(68, 32).HSizePos());
		left.Add(rowStyle.TopPos(100, 168).HSizePos());
		left.Add(rowCanvas.VSizePos(268, 0).HSizePos());

		// Tools
		rowTools.SetFrame(ThinInsetFrame());
		rowTools.Add(toolbox.SizePos());
		BuildToolButtons();

		// Ops row
		rowOps.SetFrame(ThinInsetFrame());
		cbSnap <<= canvas.snap;
		cbClip <<= canvas.clip;
		edGrid <<= canvas.grid;
		for(int i = 0; i < Canvas::ASPECT_COUNT; ++i)
			dlAspect.Add(i, Canvas::ASPECTS[i].label);
		dlAspect.SetIndex(canvas.aspect_ix);

		dlSampleRes.Add(32, "32");
		dlSampleRes.Add(64, "64");
		dlSampleRes.Add(128, "128");
		dlSampleRes.Add(512, "512");
		dlSampleRes.SetData(canvas.sample_width);

		int y = 3, h = 24;
		//int  xbtn = 6;;
		colStart = 3;
		colPad = 6;
		rowPad = 3;
		
		colX = colStart;

		// ---- RowOps layout
		cbSnap.SetLabel("Snap");
		rowOps.Add(cbSnap.LeftPos(ColPos(60, true), 60).VCenterPos());
		cbClip.SetLabel("Clip");
		rowOps.Add(cbClip.LeftPos(ColPos(60), 60).VCenterPos());

	   // BG toggle  + color pusher (member)
		cbBgEnable.SetLabel("Bg Color");
		cbBgEnable <<= true;
		rowOps.Add(cbBgEnable.LeftPos(ColPos(60), 50).VCenterPos());

		cBg <<= Color(250, 250, 250);
		rowOps.Add(cBg.LeftPos(ColPos(50), 40).VCenterPos());

		lblAspect.SetText("Aspect Ratio");
		rowOps.Add(lblAspect.LeftPos(ColPos(70), 80).VCenterPos());
		rowOps.Add(dlAspect.LeftPos(ColPos(60), 60).VCenterPos());

		lblSample.SetText("SampleRes");
		rowOps.Add(lblSample.LeftPos(ColPos(60), 70).VCenterPos());
		rowOps.Add(dlSampleRes.LeftPos(ColPos(60), 60).VCenterPos());

		cbShowGrid.SetLabel("Grid");
		cbShowGrid <<= true;
		rowOps.Add(cbShowGrid.LeftPos(ColPos(40), 40).VCenterPos());

		lblGrid.SetText("Step");
		rowOps.Add(lblGrid.LeftPos(ColPos(40), 40).VCenterPos());
		edGrid.MinMax(2, 64);
		rowOps.Add(edGrid.LeftPos(ColPos(60), 60).VCenterPos());
	    


		// ---- Actions row
		rowActions.SetFrame(ThinInsetFrame());

		bDup.SetLabel("Duplicate");
		rowActions.Add(bDup.LeftPos( ColPos(70,true,true), 70).VCenterPos());

		bFlipX.SetLabel("FlipX");
		rowActions.Add(bFlipX.LeftPos( ColPos(70), 70).VCenterPos());

		bFlipY.SetLabel("FlipY");
		rowActions.Add(bFlipY.LeftPos( ColPos(70), 70).VCenterPos());

		bLayerUp.SetLabel("LayerUp");
		rowActions.Add(bLayerUp.LeftPos( ColPos(70), 70).VCenterPos());

		bLayerDown.SetLabel("LayerDn");
		rowActions.Add(bLayerDown.LeftPos( ColPos(70), 70).VCenterPos());


		bUndo.SetLabel("Undo");
		rowActions.Add(bUndo.LeftPos( ColPos(70), 70).VCenterPos());

		bRedo.SetLabel("Redo");
		rowActions.Add(bRedo.LeftPos( ColPos(70), 70).VCenterPos());

		bReset.SetLabel("Reset");
		rowActions.Add(bReset.LeftPos( ColPos(70), 70).VCenterPos());

		rowActions.Add(bDelete.SetLabel("Delete").LeftPos( ColPos(70), 70).VCenterPos());

		// ---- Style panel
		rowStyle.SetFrame(ThinInsetFrame());

		// Row 1: Fill
		y = 3;
		colX = colStart;
		cbFill.SetLabel("Fill");
		rowStyle.Add(cbFill.LeftPos(ColPos(60, true), 60).TopPos(y, h));
		rowStyle.Add(cFill.LeftPos(ColPos(40), 40).TopPos(y, h));

		lblFillOp.SetText("Opacity");
		rowStyle.Add(lblFillOp.LeftPos(ColPos(40), 40).TopPos(y, h));
		sFillOpacity.MinMax(0, 100);
		sFillOpacity <<= 100;
		rowStyle.Add(sFillOpacity.LeftPos(ColPos(175), 120).TopPos(y, h));
	  
        cbEvenOdd.SetLabel("EvenOdd");
		rowStyle.Add(cbEvenOdd.LeftPos(ColPos(80), 80).TopPos(y, h));

		y += h + rowPad;

		// Row 2: Stroke
		colX = colStart;
		cbStroke.SetLabel("Stroke");
		rowStyle.Add(cbStroke.LeftPos(ColPos(60, true), 60).TopPos(y, h));
		rowStyle.Add(cStroke.LeftPos(ColPos(40), 40).TopPos(y, h));
		
		
		lblStOp.SetText("Opacity");
		rowStyle.Add(lblStOp.LeftPos(ColPos(40), 40).TopPos(y, h));
		sStrokeOpacity.MinMax(0, 100);
		sStrokeOpacity <<= 100;
		rowStyle.Add(sStrokeOpacity.LeftPos(ColPos(120), 120).TopPos(y, h));

		lblStrokeW.SetText("Width");
		rowStyle.Add(lblStrokeW.LeftPos(ColPos(50), 50).TopPos(y, h));
		spinStrokeW.MinMax(0, 128);
		spinStrokeW <<= 2;
		rowStyle.Add(spinStrokeW.LeftPos(ColPos(60), 60).TopPos(y, h));

		lblStType.SetText("Type");
		rowStyle.Add(lblStType.LeftPos(ColPos(40), 40).TopPos(y, h));
		dlStrokeType.Clear();
		dlStrokeType.Add("Solid");
		dlStrokeType.Add("LongDash");
		dlStrokeType.Add("ShortDash");
		dlStrokeType.Add("Dotted");
		dlStrokeType <<= 0;
		rowStyle.Add(dlStrokeType.LeftPos(ColPos(80), 80).TopPos(y, h));



		lblStDash.SetText("Dash");
		rowStyle.Add(lblStDash.LeftPos(ColPos(40), 40).TopPos(y, h));
		rowStyle.Add(edDash.LeftPos(ColPos(60), 60).TopPos(y, h)); // MEMBER

		y += h + rowPad;

		// Row 3: Outline
		colX = colStart;
		
		cbOutline.SetLabel("Outline");
		rowStyle.Add(cbOutline.LeftPos(ColPos(60,true), 60).TopPos(y, h));
		rowStyle.Add(cOutline.LeftPos(ColPos(40), 40).TopPos(y, h));
		
		lblOlOp.SetText("Opacity");
		rowStyle.Add(lblOlOp.LeftPos(ColPos(40), 40).TopPos(y, h));
		sOutlineOpacity.MinMax(0, 100);
		sOutlineOpacity <<= 100;
		rowStyle.Add(sOutlineOpacity.LeftPos(ColPos(120), 120).TopPos(y, h));

		lblOutW.SetText("Width");
		rowStyle.Add(lblOutW.LeftPos(ColPos(50), 50).TopPos(y, h));
		spinOutlineW.MinMax(0, 128);
		spinOutlineW <<= 0;
		rowStyle.Add(spinOutlineW.LeftPos(ColPos(60), 60).TopPos(y, h));

		lblOlType.SetText("Type");
		rowStyle.Add(lblOlType.LeftPos(ColPos(40), 40).TopPos(y, h));
		dlOutlineType.Clear();
		dlOutlineType.Add("Solid");
		dlOutlineType.Add("LongDash");
		dlOutlineType.Add("ShortDash");
		dlOutlineType.Add("Dotted");
		dlOutlineType <<= 0;
		rowStyle.Add(dlOutlineType.LeftPos(ColPos(80), 80).TopPos(y, h));


		lblOlDash.SetText("Dash");
		rowStyle.Add(lblOlDash.LeftPos(ColPos(40), 40).TopPos(y, h));
		rowStyle.Add(edOutlineDash.LeftPos(ColPos(60), 60).TopPos(y, h)); 

		lblOff.SetText("OffX/Y");
		rowStyle.Add(lblOff.LeftPos(ColPos(50), 50).TopPos(y, h));
		rowStyle.Add(esOutlineOffX.LeftPos(ColPos(50), 50).TopPos(y, h));
		rowStyle.Add(esOutlineOffY.LeftPos(ColPos(50), 50).TopPos(y, h)); 

		y += h + rowPad;

		// Row 4: Text
		colX = colStart;
		lblText.SetText("Text");
		rowStyle.Add(lblText.LeftPos(ColPos(60, true), 60).TopPos(y, h));
		rowStyle.Add(edText.LeftPos(ColPos(100), 100).TopPos(y, h));

		lblCodes.SetText("Codes");
		rowStyle.Add(lblCodes.LeftPos(ColPos(60), 60).TopPos(y, h));
		rowStyle.Add(edCodes.LeftPos(ColPos(100), 100).TopPos(y, h));

		lblFont.SetText("Font");
		rowStyle.Add(lblFont.LeftPos(ColPos(40), 40).TopPos(y, h));
		rowStyle.Add(dlFont.LeftPos(ColPos(200), 200).TopPos(y, h)); 
		cbBold.SetLabel("Bold");
		cbItalic.SetLabel("Italic");
		rowStyle.Add(cbBold.LeftPos(ColPos(40), 40).TopPos(y, h));
		rowStyle.Add(cbItalic.LeftPos(ColPos(40), 40).TopPos(y, h));

		// Fonts
		LoadSystemFonts();

		// Canvas + code panel
		rowCanvas.SetFrame(ThinInsetFrame());
		rowCanvas.Add(canvas.SizePos());
		canvas.WhenSelection = THISBACK(OnSelectionChanged);
		canvas.WhenShapesChanged = THISBACK(OnShapesChanged);

		right.Add(codeHdr.TopPos(0, 32).HSizePos());
		right.Add(code.VSizePos(32, 0).HSizePos());
		codeHdr.SetFrame(ThinInsetFrame());
		codeHdr.Add(codeHdrBox.SizePos());
	
	    codeTitle.SetText("Generated BufferPainter code");
        codeHdrBox.Add(codeTitle.LeftPos(6, 300).VCenterPos());
	    
	    int xbtn = 6;
		bCopy.SetLabel("Copy");
		codeHdrBox.Add(bCopy.RightPos(xbtn, 60).VCenterPos());
        
        xbtn += colPad+60;
		bSave.SetLabel("Save");
		codeHdrBox.Add(bSave.RightPos(xbtn, 60).VCenterPos());
		
		xbtn += colPad+60;
		bLoad.SetLabel("Load");
		codeHdrBox.Add(bLoad.RightPos(xbtn, 60).VCenterPos());
		
		xbtn += colPad+60;
		codeHdrBox.Add(bClear.SetLabel("Clear").RightPos(xbtn, 60).VCenterPos());



		// Actions wiring (members)
		bDup << [=] {
			canvas.DuplicateSelected();
			UpdateCode();
		};
		bFlipX << [=] {
			canvas.FlipXSelected();
			UpdateCode();
		};
		bFlipY << [=] {
			canvas.FlipYSelected();
			UpdateCode();
		};
		bLayerUp << [=] {
			canvas.LayerUp();
			UpdateCode();
		};
		bLayerDown << [=] {
			canvas.LayerDown();
			UpdateCode();
		};
		bReset << [=] {
			canvas.ResetStyleSelectedOrDefaults();
			UpdateCode();
		};
		bClear << [=] {
			canvas.ClearAll();
			UpdateCode();
		};
		bDelete << [=] {
			canvas.DeleteSelected();
			UpdateCode();
		};
		bSave << [=] { DoSave(); };
		bLoad << [=] { DoLoad(); };
		bUndo << [=] {
			if(canvas.Undo())
				UpdateCode();
		};
		bRedo << [=] {
			if(canvas.Redo())
				UpdateCode();
		};

		// BG wiring (members; safe captures)
		cbBgEnable.WhenAction = [this] {
			canvas.bg_color = (bool)~cbBgEnable ? (Color)~cBg : White();
			canvas.Refresh();
		};
		cBg.WhenAction = [this] {
			if((bool)~cbBgEnable) {
				canvas.bg_color = (Color)~cBg;
				canvas.Refresh();
			}
		};

		// Ops wiring
		cbSnap.WhenAction = THISBACK(OnSnap);
		cbClip.WhenAction = THISBACK(OnClip);
		edGrid.WhenAction = THISBACK(OnGrid);
		cbShowGrid.WhenAction = [this] {
			canvas.show_grid = (bool)~cbShowGrid;
			canvas.Refresh();
		};

		dlAspect.WhenAction = [=] {
			canvas.aspect_ix = Upp::clamp(dlAspect.GetIndex(), 0, Canvas::ASPECT_COUNT - 1);
			canvas.Refresh();
		};
		dlSampleRes.WhenAction = [=] {
			canvas.sample_width = (int)dlSampleRes.GetData();
			canvas.Refresh();
		};

		// Style wiring
		cbFill.WhenAction = THISBACK(PullStyleFromUI);
		cbStroke.WhenAction = THISBACK(PullStyleFromUI);
		cbEvenOdd.WhenAction = THISBACK(PullStyleFromUI);
		cbOutline.WhenAction = THISBACK(PullStyleFromUI);
		cFill.WhenAction = THISBACK(PullStyleFromUI);
		cStroke.WhenAction = THISBACK(PullStyleFromUI);
		cOutline.WhenAction = THISBACK(PullStyleFromUI);
		spinStrokeW.WhenAction = THISBACK(PullStyleFromUI);
		spinOutlineW.WhenAction = THISBACK(PullStyleFromUI);
		sFillOpacity.WhenAction = THISBACK(PullStyleFromUI);
		sStrokeOpacity.WhenAction = THISBACK(PullStyleFromUI);
		sOutlineOpacity.WhenAction = THISBACK(PullStyleFromUI);
		edDash.WhenAction = THISBACK(PullStyleFromUI);
		edOutlineDash.WhenAction = THISBACK(PullStyleFromUI);
		esOutlineOffX.WhenAction = THISBACK(PullStyleFromUI);
		esOutlineOffY.WhenAction = THISBACK(PullStyleFromUI);

		// Dash presets
		dlStrokeType.WhenAction = [this] {
			if(canvas.selected < 0 || canvas.selected >= canvas.shapes.GetCount())
				return;
			::Style& st = canvas.shapes[canvas.selected].style;
			st.strokeStyle = FromI(dlStrokeType.GetIndex());
			String d = DashFrom(st.strokeStyle, String());
			if(!d.IsEmpty())
				edDash <<= d;
			PullStyleFromUI();
		};
		dlOutlineType.WhenAction = [this] {
			if(canvas.selected < 0 || canvas.selected >= canvas.shapes.GetCount())
				return;
			::Style& st = canvas.shapes[canvas.selected].style;
			st.outlineStyle = FromI(dlOutlineType.GetIndex());
			String d = DashFrom(st.outlineStyle, String());
			if(!d.IsEmpty())
				edOutlineDash <<= d;
			PullStyleFromUI();
		};

		// Text wiring (with debounce for Codes)
		auto text_to_codes = [](const String& s) -> String {
			String out;
			for(int i = 0; i < s.GetCount(); ++i) {
				if(i)
					out << ',';
				out << AsString((int)(byte)s[i]);
			}
			return out;
		};
		auto codes_to_text = [](const String& codes) -> String {
			String out;
			CParser p(~codes);
			while(!p.IsEof()) {
				if(p.IsNumber()) {
					int v = p.ReadNumber();
					if(v >= 0 && v <= 0x10FFFF) {
						if(v <= 0xFF)
							out.Cat((byte)v);
						else {
							WString ws;
							ws.Cat((wchar)v);
							out.Cat(ToUtf8(ws));
						}
					}
				}
				if(p.Char(',')) {
				}
				else if(!p.IsEof())
					p.SkipTerm();
			}
			return out;
		};

		edText.WhenAction = [this, text_to_codes] {
			String t = ~edText;
			if(t.IsEmpty())
				t = "Text";
			canvas.text_template = t;
			if(canvas.selected >= 0 && canvas.selected < canvas.shapes.GetCount() &&
			   canvas.shapes[canvas.selected].type == PType::Text) {
				canvas.shapes[canvas.selected].text.text = t;
				edCodes <<= text_to_codes(t);
				canvas.Refresh();
				UpdateCode();
			}
		};
        //echo in text codes
		edCodes.WhenAction = [this] {
			// small helper kept inline to avoid captures
			auto codes_to_text = [](const String& codes) -> String {
				String out;
				CParser p(~codes);
				while(!p.IsEof()) {
					if(p.IsNumber()) {
						int v = p.ReadNumber();
						if(v >= 0 && v <= 0x10FFFF) {
							if(v <= 0xFF)
								out.Cat((byte)v);
							else {
								WString ws;
								ws.Cat((wchar)v);
								out.Cat(ToUtf8(ws));
							}
						}
					}
					if(p.Char(',')) {
					}
					else if(!p.IsEof())
						p.SkipTerm();
				}
				return out;
			};

			String t = codes_to_text(~edCodes);
			if(t.IsEmpty())
				t = "Text";
			canvas.text_template = t;

			if(canvas.selected >= 0 && canvas.selected < canvas.shapes.GetCount() &&
			   canvas.shapes[canvas.selected].type == PType::Text) {
				canvas.shapes[canvas.selected].text.text = t;
				edText <<= t;
				// throttle code generation a bit: only refresh drawing here
				canvas.Refresh();
				UpdateCode();
			}
		};

		dlFont.WhenAction = [this] {
			String face = (String)dlFont.GetValue();
			if(canvas.selected >= 0 && canvas.selected < canvas.shapes.GetCount() &&
			   canvas.shapes[canvas.selected].type == PType::Text) {
				canvas.shapes[canvas.selected].text.face = face;
				canvas.Refresh();
				UpdateCode();
			}
		};
		cbBold.WhenAction = [this] {
			if(canvas.selected >= 0 && canvas.selected < canvas.shapes.GetCount() &&
			   canvas.shapes[canvas.selected].type == PType::Text) {
				canvas.shapes[canvas.selected].text.bold = (bool)~cbBold;
				canvas.Refresh();
				UpdateCode();
			}
		};
		cbItalic.WhenAction = [this] {
			if(canvas.selected >= 0 && canvas.selected < canvas.shapes.GetCount() &&
			   canvas.shapes[canvas.selected].type == PType::Text) {
				canvas.shapes[canvas.selected].text.italic = (bool)~cbItalic;
				canvas.Refresh();
				UpdateCode();
			}
		};

		bCopy.WhenAction = THISBACK(OnCopyCode);

		// seed history with empty doc
		canvas.PushHist();
		UpdateCode();
	}
};

// ===================== WinMain =====================
GUI_APP_MAIN
{
	SetDefaultCharset(CHARSET_UTF8);
	MainWin().Run();
}


