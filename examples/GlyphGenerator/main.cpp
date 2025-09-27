/*
--------------------------------------------------------------------------------
 U++ Icon Builder — Modular Primitives (single-file demo)
--------------------------------------------------------------------------------
 - Keeps your existing layout & interactions (tools row, ops row, actions row,
   style panel on the left; live code-export panel on the right) while switching
   the drawing/editing to a tiny per-primitive ops registry. Adds Text & Curve.
 - All geometry is normalized to an inset rectangle (0..1), preserving your
   snapping and layout logic. Code export still emits BufferPainter code.

 Build (U++ 2025.1+):
   uses: Core, CtrlLib, Draw, Painter
--------------------------------------------------------------------------------
*/
#include <CtrlLib/CtrlLib.h>
#include <Painter/Painter.h>
#include <Draw/Draw.h>

#include <iostream>
//#include <iomanip>
//#include <algorithm>

using namespace Upp;

// ===================== Style, Model, Mapping =====================
struct Style : Moveable<Style> {
    Color  fill   = Color(163,201,168);
    Color  stroke = Color(30,53,47);
    int    strokeWidth = 2;
    bool   evenOdd = false;
    String dash;
    bool   enableFill   = true;
    bool   enableStroke = true;
    double opacity      = 1.0;
    bool   outlineEnable  = false;
    Color  outlineColor   = Red();
    int    outlineWidth   = 0;
};

// MODIFIED: Simplified Tool enum for better modularity
enum class Tool  { Cursor, CreateShape };
enum class PType { Rect,   Circle,   Line,   Triangle,   Curve,   Text  };


using std::cout; using std::endl; using std::clamp;
static bool gDebug = true; // flip to false to silence logs quickly
static inline const char* ToolName(Tool t){ return t==Tool::Cursor ? "Cursor" : "CreateShape"; }
static inline const char* PTypeName(PType t){
   switch(t){
        case PType::Rect: return "Rect";
        case PType::Circle: return "Circle";
        case PType::Line: return "Line";
        case PType::Triangle: return "Triangle";
        case PType::Curve: return "Curve";
        case PType::Text: return "Text";
    }
    return "?";
}


struct TextData {
    String text = "Text";
    String face = "";
    double sizeN = 0.18; // relative to inset height
    bool   bold = false, italic = false;
};

struct CurveData {
    bool   cubic   = true;   // false => quadratic
    bool   closed  = false;
    Pointf a0, a1;           // anchors
    Pointf c0, c1;           // controls (c1 ignored for quadratic)
};

struct Shape : Moveable<Shape> {
    PType type = PType::Rect;
    Style style;

    // Rect
    double x=0, y=0, w=0, h=0;
    // Circle
    double cx=0, cy=0, r=0;      // r relative to min(inset w,h)
    // Line / Triangle
    Pointf p1, p2, p3;
    // Payloads
    TextData  text;
    CurveData curve;
};

// mapping helpers (normalized <-> px within given inset)
static inline int  X (const Rect& r, double nx) { return r.left + int(r.Width()  * nx + 0.5); }
static inline int  Y (const Rect& r, double ny) { return r.top  + int(r.Height() * ny + 0.5); }
static inline int  R (const Rect& r, double nr) { return int(min(r.Width(), r.Height()) * nr + 0.5); }
static inline double NX(const Rect& r, int px)  { return (px - r.left) / double(max(1, r.Width()));  }
static inline double NY(const Rect& r, int py)  { return (py - r.top)  / double(max(1, r.Height())); }
static inline int  Snap1D(int v, int origin, int step) { return origin + ((v - origin + step / 2) / step) * step; }

// small helpers for hits
static inline bool IsNearSegment(Point p, Point a, Point b, int tol) {
    if(a == b) return abs(p.x-a.x) <= tol && abs(p.y-a.y) <= tol;
    double vx=b.x-a.x, vy=b.y-a.y, wx=p.x-a.x, wy=p.y-a.y;
    double vv=vx*vx+vy*vy; if(vv <= 1e-9) return false;
    double t=(wx*vx+wy*vy)/vv; if(t<0) t=0; else if(t>1) t=1;
    double qx=a.x+t*vx, qy=a.y+t*vy, dx=p.x-qx, dy=p.y-qy;
    return dx*dx+dy*dy <= double(tol*tol);
}
static inline bool IsPointInTriangle(Point p, Point a, Point b, Point c) {
    auto s=[&](Point p1,Point p2,Point p3){ return (p1.x-p3.x)*(p2.y-p3.y)-(p2.x-p3.x)*(p1.y-p3.y); };
    bool b1=s(p,a,b)<0, b2=s(p,b,c)<0, b3=s(p,c,a)<0; return (b1==b2)&&(b2==b3);
}



// Safe style application for Painter: skips bad dash and clamps opacity.
static inline void ApplyStyle(BufferPainter& p, const Style& st) {
    // 1) Opacity
    if(st.opacity < 1.0) {
        double a = st.opacity;
        if(a < 0) a = 0;
        if(a > 1) a = 1;
        p.Opacity(a);
    }

    // 2) Dash — only apply if we have at least two strictly positive numbers.
    if(!st.dash.IsEmpty()) {
        Vector<double> seg;
        const char* s = ~st.dash;
        while(*s) {
            while(*s == ' ' || *s == '\t' || *s == ',') s++;
            char* end = nullptr;
            double v = strtod(s, &end);
            if(end == s) break;
            if(v > 0) seg.Add(v);      // ignore zeros / negatives
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

// ===================== Ops Registry Types =====================
struct PrimitiveOps {
    void (*EmitPainter)(BufferPainter&, const Rect&, const Shape&);
    bool (*HitBody)(const Rect&, const Shape&, Point);
    int  (*HitVertex)(const Rect&, const Shape&, Point, int px);
    void (*DrawOverlay)(Draw&, const Rect&, const Shape&);
    void (*BeginCreate)(Shape&, const Rect&, Point start_px);
    void (*DragCreate)(Shape&, const Rect&, Point start_px, Point cur_px, bool snap, int grid);
    void (*BeginEdit)(Shape&, const Rect&, Point grab_px, int hitVertex, double& grab_nx, double& grab_ny);
    void (*DragEdit)(Shape&, const Rect&, Point cur_px, bool snap, int grid, bool moving, int drag_vertex,
                     double& grab_nx, double& grab_ny);
    void (*EmitCode)(String& out, const Shape&);
};

struct ToolSpec : Moveable<ToolSpec> {
    PType  type;
    const char* label;
    const char* tip;
};

const PrimitiveOps&   GetOps(PType);
const Vector<ToolSpec>& GetToolSpecs();


// Minimal pixel threshold/size for starting/creating shapes.
static constexpr int MIN_EMIT_PX = 1;


// ============ Rect ============
// Rect_EmitPainter — skip tiny rects; always normalize
static void Rect_EmitPainter(BufferPainter& p, const Rect& inset, const Shape& s){
    Rect r = RectC(
        X(inset, s.x),
        Y(inset, s.y),
        X(inset, s.x + s.w) - X(inset, s.x),
        Y(inset, s.y + s.h) - Y(inset, s.y)
    );
    r.Normalize();
    if(r.Width()  < MIN_EMIT_PX || r.Height() < MIN_EMIT_PX) return;     // <— guard

    const Style& st = s.style;
    p.Begin();
        p.Move(Pointf(r.left,  r.top));
        p.Line(Pointf(r.right, r.top));
        p.Line(Pointf(r.right, r.bottom));
        p.Line(Pointf(r.left,  r.bottom));
        p.Close();
        if(st.opacity < 1.0)        p.Opacity(st.opacity);
        if(!st.dash.IsEmpty())      p.Dash(st.dash, 0.0);
        if(st.enableFill)           p.Fill(st.fill);
        if(st.enableStroke)         p.Stroke(st.strokeWidth, st.stroke);
    p.End();
}

// Rect_HitBody — normalized so hit-testing works regardless of drag direction
static bool Rect_HitBody(const Rect& inset, const Shape& s, Point m){
    Rect r = RectC(
        X(inset, s.x),
        Y(inset, s.y),
        X(inset, s.x + s.w) - X(inset, s.x),
        Y(inset, s.y + s.h) - Y(inset, s.y)
    );
    r.Normalize();
    return r.Inflated(4).Contains(m);
}

static int Rect_HitVertex(const Rect& inset, const Shape& s, Point m, int px){
    Point tl(X(inset,s.x),Y(inset,s.y));
    Point tr(X(inset,s.x+s.w),Y(inset,s.y));
    Point br(X(inset,s.x+s.w),Y(inset,s.y+s.h));
    Point bl(X(inset,s.x),Y(inset,s.y+s.h));
    auto NearPoint=[&](Point a){return abs(a.x-m.x)<=px && abs(a.y-m.y)<=px;};
    if(NearPoint(tl)) return 0; if(NearPoint(tr)) return 1; if(NearPoint(br)) return 2; if(NearPoint(bl)) return 3; return -1;
}

static void Rect_DrawOverlay(Draw& w, const Rect& inset, const Shape& s){
    Color sel=SColorMark();
    Point p1(X(inset, s.x), Y(inset, s.y));
    Point p2(X(inset, s.x + s.w), Y(inset, s.y + s.h));
    Rect r(p1, p2);
    r.Normalize(); // Ensures correct coordinates even if width/height is negative

    // Draw selection box
    w.DrawRect(r.left, r.top, r.Width(), 1, sel);
    w.DrawRect(r.left, r.bottom, r.Width() + 1, 1, sel);
    w.DrawRect(r.left, r.top, 1, r.Height(), sel);
    w.DrawRect(r.right, r.top, 1, r.Height(), sel);

    // Draw vertex handles for resizing
    int hsz = 3;
    auto DrawHandle = [&](Point p) {
        w.DrawRect(p.x - hsz, p.y - hsz, 2 * hsz + 1, 2 * hsz + 1, sel);
    };
    DrawHandle(r.TopLeft());
    DrawHandle(r.TopRight());
    DrawHandle(r.BottomLeft());
    DrawHandle(r.BottomRight());
}
static void Rect_BeginCreate(Shape& s, const Rect& inset, Point start){ s.type=PType::Rect; s.x=NX(inset,start.x); s.y=NY(inset,start.y); s.w=s.h=0; }
static void Rect_DragCreate(Shape& s, const Rect& inset, Point start, Point cur, bool snap, int grid){
    if(snap){ cur.x=Snap1D(cur.x,inset.left,grid); cur.y=Snap1D(cur.y,inset.top,grid); }
    s.w = NX(inset,cur.x) - s.x; s.h = NY(inset,cur.y) - s.y;
}
static void Rect_BeginEdit(Shape&, const Rect& inset, Point grab, int, double& gx, double& gy){ gx=NX(inset,grab.x); gy=NY(inset,grab.y); }
static void Rect_DragEdit(Shape& s, const Rect& inset, Point cur, bool snap, int grid, bool moving, int hv, double& gx, double& gy){
    if(snap){ cur.x=Snap1D(cur.x,inset.left,grid); cur.y=Snap1D(cur.y,inset.top,grid); }
    double nx=NX(inset,cur.x), ny=NY(inset,cur.y);
    if(moving){
        s.x += nx - gx;
        s.y += ny - gy;
        gx = nx; // Update the grab point for the next relative move
        gy = ny;
        return;
    }
    // Resizing logic remains correct but depends on a stable s.x/s.y
    switch(hv){
        case 0: s.w += s.x-nx; s.h += s.y-ny; s.x=nx; s.y=ny; break;
        case 1: s.w = nx - s.x; s.h += s.y-ny; s.y=ny; break;
        case 2: s.w = nx - s.x; s.h = ny - s.y; break;
        case 3: s.h = ny - s.y; s.w += s.x-nx; s.x=nx; break;
        default: break;
    }
}
static void Rect_EmitCode(String& out, const Shape& s){
    out << "    // Rect\n    p.Begin();\n";
    out << Format("    p.Move(Pointf(X(inset,%g),Y(inset,%g))); p.Line(Pointf(X(inset,%g),Y(inset,%g))); "
                  "p.Line(Pointf(X(inset,%g),Y(inset,%g))); p.Line(Pointf(X(inset,%g),Y(inset,%g))); p.Close();\n",
                  s.x,s.y, s.x+s.w,s.y, s.x+s.w,s.y+s.h, s.x,s.y+s.h);
    if(s.style.opacity<1.0) out << Format("    p.Opacity(%g);\n", s.style.opacity);
    if(!s.style.dash.IsEmpty()) out << Format("    p.Dash(String(\"%s\"),0.0);\n", ~s.style.dash);
    if(s.style.evenOdd) out << "    p.EvenOdd(true);\n";
    if(s.style.enableFill)   out << Format("    p.Fill(Color(%d,%d,%d));\n", s.style.fill.GetR(),s.style.fill.GetG(),s.style.fill.GetB());
    if(s.style.enableStroke) out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n", s.style.strokeWidth,s.style.stroke.GetR(),s.style.stroke.GetG(),s.style.stroke.GetB());
    out << "    p.End();\n\n";
}


// ===== Circle (final) =======================================================

static inline int Rpx(const Rect& r, double nr)     { return int(min(r.Width(), r.Height()) * nr + 0.5); }
static inline double NrFromPx(const Rect& r, int px) { return px / double(max(1, min(r.Width(), r.Height()))); }

// Painter: true circle via two semicircular arcs.
// Guards: skip when radius < 1px; keeps Begin/End balanced.
static void Circle_EmitPainter(BufferPainter& p, const Rect& inset, const Shape& s) {
    const int cx = X(inset, s.cx);
    const int cy = Y(inset, s.cy);
    const int rr = R(inset, s.r);
    if(rr < 1) return;

    const Style& st = s.style;
    p.Begin();
        p.Move(Pointf(cx + rr, cy));                         // start at +X
        p.SvgArc(Pointf(rr, rr), 0, false, true, Pointf(cx - rr, cy)); // half 1
        p.SvgArc(Pointf(rr, rr), 0, false, true, Pointf(cx + rr, cy)); // half 2
        if(st.opacity < 1.0)   p.Opacity(st.opacity);
        if(!st.dash.IsEmpty()) p.Dash(st.dash, 0.0);
        if(st.enableFill)      p.Fill(st.fill);
        if(st.enableStroke)    p.Stroke(st.strokeWidth, st.stroke);
    p.End();
}


static bool Circle_HitBody(const Rect& inset, const Shape& s, Point m)
{
    const int cx = X(inset, s.cx);
    const int cy = Y(inset, s.cy);
    const int r  = R(inset, s.r);
    if(r < 1) return false;

    const int dx = m.x - cx, dy = m.y - cy;
    const double d = sqrt(double(dx*dx + dy*dy));
    const int tol = max(6, s.style.strokeWidth/2 + 4);

    return s.style.enableFill ? (d <= r || fabs(d - r) <= tol)
                              : (fabs(d - r) <= tol);
}

static int Circle_HitVertex(const Rect& inset, const Shape& s, Point m, int px)
{
    const Point c(X(inset, s.cx), Y(inset, s.cy));
    const Point e(c.x + R(inset, s.r), c.y); // east handle

    auto nearp = [&](Point a){ return abs(a.x - m.x) <= px && abs(a.y - m.y) <= px; };
    if(nearp(c)) return 0;   // center
    if(nearp(e)) return 1;   // radius handle
    return -1;
}

static void Circle_DrawOverlay(Draw& w, const Rect& inset, const Shape& s)
{
    const int cx = X(inset, s.cx);
    const int cy = Y(inset, s.cy);
    const int rr = R(inset, s.r);
    if(rr < 1) return;

    const Color sel = SColorMark();
    const Rect  bb  = RectC(cx - rr, cy - rr, 2*rr, 2*rr);

    // Box outline (four thin rects) – cheap & safe
    w.DrawRect(RectC(bb.left,  bb.top,    bb.Width(), 1), sel);
    w.DrawRect(RectC(bb.left,  bb.bottom, bb.Width(), 1), sel);
    w.DrawRect(RectC(bb.left,  bb.top,    1,           bb.Height()), sel);
    w.DrawRect(RectC(bb.right, bb.top,    1,           bb.Height()+1), sel);

    // Handles (center + east)
    w.DrawRect(RectC(cx - 2,     cy - 2, 5, 5), sel);
    w.DrawRect(RectC(cx + rr - 2, cy - 2, 5, 5), sel);
}


static void Circle_BeginCreate(Shape& s, const Rect& inset, Point start)
{
    s.type = PType::Circle;
    s.cx   = NX(inset, start.x);
    s.cy   = NY(inset, start.y);
    s.r    = 0.0;
}

static void Circle_DragCreate(Shape& s, const Rect& inset, Point /*start*/, Point cur,
                              bool snap, int grid)
{
    if(snap) { cur.x = Snap1D(cur.x, inset.left, grid); cur.y = Snap1D(cur.y, inset.top, grid); }
    const double nx = ::clamp(NX(inset, cur.x), 0.0, 1.0);
    const double ny = ::clamp(NY(inset, cur.y), 0.0, 1.0);
    const double dx = nx - s.cx, dy = ny - s.cy;
    s.r = max(0.0, sqrt(dx*dx + dy*dy));
}

static void Circle_BeginEdit(Shape&, const Rect& inset, Point grab, int /*hv*/,
                             double& gx, double& gy)
{
    gx = NX(inset, grab.x);
    gy = NY(inset, grab.y);
}

static void Circle_DragEdit(Shape& s, const Rect& inset, Point cur, bool snap, int grid,
                            bool moving, int hv, double& gx, double& gy)
{
    if(snap) { cur.x = Snap1D(cur.x, inset.left, grid); cur.y = Snap1D(cur.y, inset.top, grid); }
    const double nx = ::clamp(NX(inset, cur.x), 0.0, 1.0);
    const double ny = ::clamp(NY(inset, cur.y), 0.0, 1.0);

    if(moving || hv == 0) {
        s.cx = ::clamp(s.cx + (nx - gx), 0.0, 1.0);
        s.cy = ::clamp(s.cy + (ny - gy), 0.0, 1.0);
        gx = nx; gy = ny;
        return;
    }
    if(hv == 1) {
        const double dx = nx - s.cx, dy = ny - s.cy;
        s.r = max(0.0, sqrt(dx*dx + dy*dy));
    }
}

static void Circle_EmitCode(String& out, const Shape& s) {
    out << "    // Circle\n";
    out << "    p.Begin();\n";
    out << Format("    p.Move(Pointf(X(inset,%g)+R(inset,%g), Y(inset,%g)));\n",
                  s.cx, s.r, s.cy);
    out << Format("    p.SvgArc(Pointf(R(inset,%g),R(inset,%g)), 0, false, true,  "
                  "Pointf(X(inset,%g)-R(inset,%g), Y(inset,%g)));\n",
                  s.r, s.r, s.cx, s.r, s.cy);
    out << Format("    p.SvgArc(Pointf(R(inset,%g),R(inset,%g)), 0, false, true,  "
                  "Pointf(X(inset,%g)+R(inset,%g), Y(inset,%g)));\n",
                  s.r, s.r, s.cx, s.r, s.cy);
    if(s.style.opacity < 1.0)
        out << Format("    p.Opacity(%g);\n", s.style.opacity);
    if(!s.style.dash.IsEmpty())
        out << Format("    p.Dash(String(\"%s\"), 0.0);\n", ~s.style.dash);
    if(s.style.enableFill)
        out << Format("    p.Fill(Color(%d,%d,%d));\n",
                      s.style.fill.GetR(), s.style.fill.GetG(), s.style.fill.GetB());
    if(s.style.enableStroke)
        out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n",
                      s.style.strokeWidth,
                      s.style.stroke.GetR(), s.style.stroke.GetG(), s.style.stroke.GetB());
    out << "    p.End();\n\n";
}



// ============ Line_EmitPainter (guard by length in pixels) ============

static void Line_EmitPainter(BufferPainter& p, const Rect& inset, const Shape& s){
    const Point a(X(inset, s.p1.x), Y(inset, s.p1.y));
    const Point b(X(inset, s.p2.x), Y(inset, s.p2.y));
    const int dx = b.x - a.x, dy = b.y - a.y;
    if(dx*dx + dy*dy < MIN_EMIT_PX * MIN_EMIT_PX) return; // <— guard

    const Style& st = s.style;
    p.Begin();
        p.Move(Pointf(a));
        p.Line(Pointf(b));
        if(st.opacity < 1.0)        p.Opacity(st.opacity);
        if(!st.dash.IsEmpty())      p.Dash(st.dash, 0.0);
        if(st.enableStroke)         p.Stroke(st.strokeWidth, st.stroke);
        if(st.enableFill)           p.Fill(st.fill);  // (rare for lines, but supported)
    p.End();
}


static bool Line_HitBody(const Rect& inset, const Shape& s, Point m){
    return IsNearSegment(m, Point(X(inset,s.p1.x),Y(inset,s.p1.y)), Point(X(inset,s.p2.x),Y(inset,s.p2.y)), 6);
}
static int Line_HitVertex(const Rect& inset, const Shape& s, Point m, int px){
    Point a(X(inset,s.p1.x),Y(inset,s.p1.y)), b(X(inset,s.p2.x),Y(inset,s.p2.y));
    auto NearPoint=[&](Point q){return abs(q.x-m.x)<=px && abs(q.y-m.y)<=px;};
    if(NearPoint(a)) return 0; if(NearPoint(b)) return 1; return -1;
}
static void Line_DrawOverlay(Draw& w, const Rect& inset, const Shape& s){
    Color sel=SColorMark();
    Point p1 = Point(X(inset,s.p1.x),Y(inset,s.p1.y));
    Point p2 = Point(X(inset,s.p2.x),Y(inset,s.p2.y));
    w.DrawLine(p1, p2, 1, sel);
    
    // Draw vertex handles
    int hsz = 3;
    auto DrawHandle = [&](Point p) {
        w.DrawRect(p.x - hsz, p.y - hsz, 2 * hsz + 1, 2 * hsz + 1, sel);
    };
    DrawHandle(p1);
    DrawHandle(p2);
}
static void Line_BeginCreate(Shape& s, const Rect& inset, Point start){ s.type=PType::Line; s.p1=Pointf(NX(inset,start.x),NY(inset,start.y)); s.p2=s.p1; }
static void Line_DragCreate(Shape& s, const Rect& inset, Point start, Point cur, bool snap, int grid){
    if(snap){ cur.x=Snap1D(cur.x,inset.left,grid); cur.y=Snap1D(cur.y,inset.top,grid); }
    s.p2=Pointf(NX(inset,cur.x),NY(inset,cur.y));
}
static void Line_BeginEdit(Shape&, const Rect& inset, Point grab, int, double& gx,double& gy){ gx=NX(inset,grab.x); gy=NY(inset,grab.y); }
static void Line_DragEdit(Shape& s, const Rect& inset, Point cur, bool snap,int grid,bool moving,int hv,double& gx,double& gy){
    if(snap){ cur.x=Snap1D(cur.x,inset.left,grid); cur.y=Snap1D(cur.y,inset.top,grid); }
    double nx=NX(inset,cur.x), ny=NY(inset,cur.y);
    if(moving){
        Pointf d(nx-gx, ny-gy);
        s.p1 += d;
        s.p2 += d;
        gx = nx; // Update grab point
        gy = ny;
        return;
    }
    if(hv==0) s.p1=Pointf(nx,ny); else if(hv==1) s.p2=Pointf(nx,ny);
}
static void Line_EmitCode(String& out, const Shape& s){
    out << "    // Line\n    p.Begin();\n";
    out << Format("    p.Move(Pointf(X(inset,%g),Y(inset,%g))); p.Line(Pointf(X(inset,%g),Y(inset,%g)));\n", s.p1.x,s.p1.y,s.p2.x,s.p2.y);
    if(s.style.opacity<1.0) out << Format("    p.Opacity(%g);\n", s.style.opacity);
    if(!s.style.dash.IsEmpty()) out << Format("    p.Dash(String(\"%s\"),0.0);\n", ~s.style.dash);
    if(s.style.enableStroke) out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n", s.style.strokeWidth,s.style.stroke.GetR(),s.style.stroke.GetG(),s.style.stroke.GetB());
    out << "    p.End();\n\n";
}

// ============ Triangle ============
// Triangle_EmitPainter — guard tiny bbox OR near-collinear vertices
static void Triangle_EmitPainter(BufferPainter& p, const Rect& inset, const Shape& s){
    const Point P[3] = {
        Point(X(inset, s.p1.x), Y(inset, s.p1.y)),
        Point(X(inset, s.p2.x), Y(inset, s.p2.y)),
        Point(X(inset, s.p3.x), Y(inset, s.p3.y))
    };

    Rect bbox = Rect(P[0], P[0]);
    bbox |= P[1]; bbox |= P[2];
    if(bbox.Width()  < MIN_EMIT_PX || 
       bbox.Height() < MIN_EMIT_PX) return;              // tiny

    int TwiceArea = abs(P[0].x*(P[1].y - P[2].y) + P[1].x*(P[2].y - P[0].y) + P[2].x*(P[0].y - P[1].y));
    if(TwiceArea < MIN_EMIT_PX * MIN_EMIT_PX) return; // near-line

    const Style& st = s.style;
    p.Begin();
        p.Move(Pointf(P[0]));
        p.Line(Pointf(P[1]));
        p.Line(Pointf(P[2]));
        p.Close();
        if(st.opacity < 1.0)        p.Opacity(st.opacity);
        if(!st.dash.IsEmpty())      p.Dash(st.dash, 0.0);
        if(st.enableFill)           p.Fill(st.fill);
        if(st.enableStroke)         p.Stroke(st.strokeWidth, st.stroke);
    p.End();
}

static bool Triangle_HitBody(const Rect& inset, const Shape& s, Point m){
    Point a(X(inset,s.p1.x),Y(inset,s.p1.y)), b(X(inset,s.p2.x),Y(inset,s.p2.y)), c(X(inset,s.p3.x),Y(inset,s.p3.y));
    if(s.style.enableFill) return IsPointInTriangle(m,a,b,c);
    return IsNearSegment(m,a,b,6) || IsNearSegment(m,b,c,6) || IsNearSegment(m,c,a,6);
}
// Triangle_HitVertex — full correct version
static int Triangle_HitVertex(const Rect& inset, const Shape& s, Point m, int px){
    Point P[3] = {
        Point(X(inset,s.p1.x), Y(inset,s.p1.y)),
        Point(X(inset,s.p2.x), Y(inset,s.p2.y)),
        Point(X(inset,s.p3.x), Y(inset,s.p3.y))
    };
    for(int i = 0; i < 3; ++i)
        if(abs(P[i].x - m.x) <= px && abs(P[i].y - m.y) <= px)
            return i;
    return -1;
}
static void Triangle_DrawOverlay(Draw& w, const Rect& inset, const Shape& s){
    Color sel=SColorMark();
    Point a(X(inset,s.p1.x),Y(inset,s.p1.y)), b(X(inset,s.p2.x),Y(inset,s.p2.y)), c(X(inset,s.p3.x),Y(inset,s.p3.y));
    w.DrawLine(a,b,1,sel); w.DrawLine(b,c,1,sel); w.DrawLine(c,a,1,sel);
    
    // Draw vertex handles
    int hsz = 3;
    auto DrawHandle = [&](Point p) {
        w.DrawRect(p.x - hsz, p.y - hsz, 2 * hsz + 1, 2 * hsz + 1, sel);
    };
    DrawHandle(a);
    DrawHandle(b);
    DrawHandle(c);
}
static void Triangle_BeginCreate(Shape& s, const Rect& inset, Point start){ s.type=PType::Triangle; s.p1=Pointf(NX(inset,start.x),NY(inset,start.y)); s.p2=s.p3=s.p1; }
static void Triangle_DragCreate(Shape& s, const Rect& inset, Point start, Point cur, bool snap, int grid){
    if(snap){ cur.x=Snap1D(cur.x,inset.left,grid); cur.y=Snap1D(cur.y,inset.top,grid); }
    Pointf q(NX(inset,cur.x),NY(inset,cur.y)); s.p2=Pointf(q.x, s.p1.y); s.p3=q;
}
static void Triangle_BeginEdit(Shape&, const Rect& inset, Point grab, int, double& gx,double& gy){ gx=NX(inset,grab.x); gy=NY(inset,grab.y); }
static void Triangle_DragEdit(Shape& s, const Rect& inset, Point cur, bool snap,int grid,bool moving,int hv,double& gx,double& gy){
    if(snap){ cur.x=Snap1D(cur.x,inset.left,grid); cur.y=Snap1D(cur.y,inset.top,grid); }
    double nx=NX(inset,cur.x), ny=NY(inset,cur.y);
    if(moving){
        Pointf d(nx-gx,ny-gy);
        s.p1+=d; s.p2+=d; s.p3+=d;
        gx = nx; // Update grab point
        gy = ny;
        return;
    }
    if(hv==0) s.p1=Pointf(nx,ny); else if(hv==1) s.p2=Pointf(nx,ny); else if(hv==2) s.p3=Pointf(nx,ny);
}
static void Triangle_EmitCode(String& out, const Shape& s){
    out << "    // Triangle\n    p.Begin();\n";
    out << Format("    p.Move(Pointf(X(inset,%g),Y(inset,%g))); p.Line(Pointf(X(inset,%g),Y(inset,%g))); p.Line(Pointf(X(inset,%g),Y(inset,%g))); p.Close();\n",
                  s.p1.x,s.p1.y, s.p2.x,s.p2.y, s.p3.x,s.p3.y);
    if(s.style.opacity<1.0) out << Format("    p.Opacity(%g);\n", s.style.opacity);
    if(!s.style.dash.IsEmpty()) out << Format("    p.Dash(String(\"%s\"),0.0);\n", ~s.style.dash);
    if(s.style.evenOdd) out << "    p.EvenOdd(true);\n";
    if(s.style.enableFill)   out << Format("    p.Fill(Color(%d,%d,%d));\n", s.style.fill.GetR(),s.style.fill.GetG(),s.style.fill.GetB());
    if(s.style.enableStroke) out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n", s.style.strokeWidth,s.style.stroke.GetR(),s.style.stroke.GetG(),s.style.stroke.GetB());
    out << "    p.End();\n\n";
}

// ============ Text ============
static Font MakeFontPx(const TextData& td, int pxH){ Font f; if(!IsNull(td.face)) f.FaceName(td.face); f.Height(pxH); if(td.bold) f.Bold(); if(td.italic) f.Italic(); return f; }

// Text_EmitPainter — guard empty/tiny; uses TOP-LEFT anchoring
static void Text_EmitPainter(BufferPainter& p, const Rect& inset, const Shape& s){
    const TextData& td = s.text;
    if(IsNull(td.text) || td.text.IsEmpty()) return;

    int hpx = max(0, int(inset.Height() * td.sizeN + 0.5));
    if(hpx < MIN_EMIT_PX) return;                          // <— guard

    Font F;
    if(!IsNull(td.face)) F.FaceName(td.face);
    F.Height(hpx);
    if(td.bold)   F.Bold();
    if(td.italic) F.Italic();

    const Style& st = s.style;

    Pointf pen(X(inset, s.x), Y(inset, s.y));              // TOP-LEFT
    p.Begin();
        for(int i = 0; i < td.text.GetCount(); ++i)
            p.Character(pen, td.text[i], F);
        if(st.opacity < 1.0)   p.Opacity(st.opacity);
        if(st.enableFill)   p.Fill(st.fill);
        if(st.enableStroke) p.Stroke(st.strokeWidth, st.stroke);
    p.End();
}


// Exact text pixel-rect for overlay/hit — TOP-LEFT anchoring (matches Text_EmitPainter)
static inline Rect TextPixelRect(const Rect& inset, const Shape& s){
    const TextData& td = s.text;
    const int hpx = max(1, int(inset.Height() * td.sizeN + 0.5));

    Font F;
    if(!IsNull(td.face)) F.FaceName(td.face);
    F.Height(hpx);
    if(td.bold)   F.Bold();
    if(td.italic) F.Italic();

    const Size tsz = GetTextSize(td.text, F);
    const int x = X(inset, s.x);
    const int y = Y(inset, s.y);                 // TOP-LEFT (not baseline)
    return RectC(x, y, max(tsz.cx, 10), hpx);
}

static bool Text_HitBody(const Rect& inset, const Shape& s, Point m){
    return TextPixelRect(inset, s).Inflated(4).Contains(m);
}

static int Text_HitVertex(const Rect& inset, const Shape& s, Point m, int px){
    const Rect r = TextPixelRect(inset, s);
    const Point tl = r.TopLeft();
    const Point tr = Point(r.right, r.top);
    const Point br = r.BottomRight();
    const Point bl = Point(r.left, r.bottom);
    auto nearp = [&](Point a){ return abs(a.x - m.x) <= px && abs(a.y - m.y) <= px; };
    if(nearp(tl)) return 0;
    if(nearp(tr)) return 1;
    if(nearp(br)) return 2;
    if(nearp(bl)) return 3;
    return -1;
}

static void Text_DrawOverlay(Draw& w, const Rect& inset, const Shape& s){
    const Color sel = SColorMark();
    const Rect  r   = TextPixelRect(inset, s);

    // Outline
    w.DrawRect(RectC(r.left,  r.top,    r.Width(), 1), sel);
    w.DrawRect(RectC(r.left,  r.bottom, r.Width(), 1), sel);
    w.DrawRect(RectC(r.left,  r.top,    1,         r.Height()), sel);
    w.DrawRect(RectC(r.right, r.top,    1,         r.Height()+1), sel);

    // Corner handles
    const int hs = 3;
    auto Handle = [&](Point p){ w.DrawRect(p.x - hs, p.y - hs, 2*hs + 1, 2*hs + 1, sel); };
    Handle(r.TopLeft());
    Handle(Point(r.right, r.top));
    Handle(Point(r.left,  r.bottom));
    Handle(r.BottomRight());
}

static void Text_BeginCreate(Shape& s, const Rect& inset, Point start){ s.type=PType::Text; s.x=NX(inset,start.x); s.y=NY(inset,start.y); }
static void Text_DragCreate(Shape& s, const Rect& inset, Point start, Point cur, bool snap, int grid){
    if(snap){ cur.y=Snap1D(cur.y,inset.top,grid); }
    s.text.sizeN = max(0.02, fabs(NY(inset,cur.y) - NY(inset,start.y)));
}
static void Text_BeginEdit(Shape&, const Rect& inset, Point grab, int, double& gx,double& gy){ gx=NX(inset,grab.x); gy=NY(inset,grab.y); }
static void Text_DragEdit(Shape& s, const Rect& inset, Point cur, bool snap,int grid,bool moving,int hv,double& gx,double& gy){
    if(snap){ cur.x=Snap1D(cur.x,inset.left,grid); cur.y=Snap1D(cur.y,inset.top,grid); }
    double nx=NX(inset,cur.x), ny=NY(inset,cur.y);
    if(moving){
        s.x += nx-gx;
        s.y += ny-gy;
        gx = nx; // Update grab point
        gy = ny;
        return;
    }
    // Resizing logic (any vertex drag changes size for simplicity)
    s.text.sizeN = max(0.02, fabs(ny - s.y));
}
static void Text_EmitCode(String& out, const Shape& s){
    out << "    // Text\n    p.Begin();\n";
    out << "    { Pointf pen(X(inset,"<<s.x<<"),Y(inset,"<<s.y<<")); Font F; F.Height(int(inset.Height()*"<<s.text.sizeN<<"+0.5)); ";
    if(!s.text.face.IsEmpty()) out << "F.FaceName(\""<<s.text.face<<"\"); ";
    if(s.text.bold) out << "F.Bold(); ";
    if(s.text.italic) out << "F.Italic(); ";
    out << "String T=\""<<s.text.text<<"\"; for(int i=0;i<T.GetCount();++i) p.Character(pen,T[i],F); }\n";
    if(s.style.opacity<1.0) out << Format("    p.Opacity(%g);\n", s.style.opacity);
    if(!s.style.dash.IsEmpty()) out << Format("    p.Dash(String(\"%s\"),0.0);\n", ~s.style.dash);
    if(s.style.evenOdd) out << "    p.EvenOdd(true);\n";
    if(s.style.enableFill)   out << Format("    p.Fill(Color(%d,%d,%d));\n", s.style.fill.GetR(),s.style.fill.GetG(),s.style.fill.GetB());
    if(s.style.enableStroke) out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n", s.style.strokeWidth,s.style.stroke.GetR(),s.style.stroke.GetG(),s.style.stroke.GetB());
    out << "    p.End();\n\n";
}

// ============ Curve ============
static void Curve_EmitPainter(BufferPainter& p, const Rect& inset, const Shape& s){
    const Style& st=s.style; const CurveData& c=s.curve;
    auto P=[&](Pointf q){return Pointf(X(inset,q.x),Y(inset,q.y));};
    p.Begin();
        p.Move(P(c.a0));
        if(c.cubic) p.Cubic(P(c.c0), P(c.c1), P(c.a1)); else p.Quadratic(P(c.c0), P(c.a1));
        if(c.closed) p.Close();
        if(st.opacity<1.0) p.Opacity(st.opacity);
        if(!st.dash.IsEmpty()) p.Dash(st.dash,0.0);
        if(st.evenOdd) p.EvenOdd(true);
        if(c.closed && st.enableFill) p.Fill(st.fill);
        if(st.enableStroke) p.Stroke(st.strokeWidth, st.stroke);
    p.End();
}
static bool Curve_HitBody(const Rect& inset, const Shape& s, Point m){
    auto P=[&](Pointf q){return Point(X(inset,q.x),Y(inset,q.y));};
    const CurveData& c=s.curve; Point a0=P(c.a0), a1=P(c.a1), c0=P(c.c0), c1=P(c.c1);
    Rect tight(min(min(a0.x,a1.x),min(c0.x,c1.x)), min(min(a0.y,a1.y),min(c0.y,c1.y)),
               max(max(a0.x,a1.x),max(c0.x,c1.x)), max(max(a0.y,a1.y),max(c0.y,c1.y)));
    return tight.Inflated(6).Contains(m);
}
static int  Curve_HitVertex(const Rect& inset, const Shape& s, Point m, int px){
    auto P=[&](Pointf q){return Point(X(inset,q.x),Y(inset,q.y));};
    const CurveData& c=s.curve; Point pts[4]={ P(c.a0), P(c.c0), P(c.c1), P(c.a1) };
    int n = c.cubic ? 4 : 3;
    for(int i=0;i<n;++i) if(abs(pts[i].x-m.x)<=px && abs(pts[i].y-m.y)<=px) return i;
    return -1;
}
static void Curve_DrawOverlay(Draw& w, const Rect& inset, const Shape& s){
    Color sel=SColorMark(); auto P=[&](Pointf q){return Point(X(inset,q.x),Y(inset,q.y));};
    const CurveData& c=s.curve; Point a0=P(c.a0), a1=P(c.a1), k0=P(c.c0), k1=P(c.c1);
    w.DrawLine(a0,k0,1,sel); if(c.cubic) w.DrawLine(a1,k1,1,sel);
    auto H=[&](Point pt){ w.DrawRect(RectC(pt.x-3,pt.y-3,6,6), sel); };
    H(a0); H(k0); if(c.cubic) H(k1); H(a1);
}
static void Curve_BeginCreate(Shape& s, const Rect& inset, Point start){
    s.type=PType::Curve; Pointf q(NX(inset,start.x),NY(inset,start.y));
    s.curve.a0=s.curve.a1=s.curve.c0=s.curve.c1=q;
}
static void Curve_DragCreate(Shape& s, const Rect& inset, Point start, Point cur, bool snap, int grid){
    if(snap){ cur.x=Snap1D(cur.x,inset.left,grid); cur.y=Snap1D(cur.y,inset.top,grid); }
    s.curve.a1=Pointf(NX(inset,cur.x),NY(inset,cur.y));
    s.curve.c0=Pointf((s.curve.a0.x*2+s.curve.a1.x)/3.0,(s.curve.a0.y*2+s.curve.a1.y)/3.0);
    s.curve.c1=Pointf((s.curve.a0.x+s.curve.a1.x*2)/3.0,(s.curve.a0.y+s.curve.a1.y*2)/3.0);
}
static void Curve_BeginEdit(Shape&, const Rect& inset, Point grab, int, double& gx,double& gy){ gx=NX(inset,grab.x); gy=NY(inset,grab.y); }
static void Curve_DragEdit(Shape& s, const Rect& inset, Point cur, bool snap,int grid,bool moving,int hv,double& gx,double& gy){
    if(snap){ cur.x=Snap1D(cur.x,inset.left,grid); cur.y=Snap1D(cur.y,inset.top,grid); }
    double nx=NX(inset,cur.x), ny=NY(inset,cur.y);
    if(moving){
        Pointf d(nx-gx,ny-gy);
        s.curve.a0+=d; s.curve.a1+=d; s.curve.c0+=d; if(s.curve.cubic) s.curve.c1+=d;
        gx = nx; // Update grab point
        gy = ny;
        return;
    }
    Pointf np(nx,ny);
    switch(hv){ case 0: s.curve.a0=np; break; case 1: s.curve.c0=np; break; case 2: if(s.curve.cubic) s.curve.c1=np; break; case 3: s.curve.a1=np; break; default: break; }
}
static void Curve_EmitCode(String& out, const Shape& s){
    const CurveData& c=s.curve; out<<"    // Curve\n    p.Begin();\n";
    out<<Format("    p.Move(Pointf(X(inset,%g),Y(inset,%g)));\n", c.a0.x,c.a0.y);
    if(c.cubic)
        out<<Format("    p.Cubic(Pointf(X(inset,%g),Y(inset,%g)), Pointf(X(inset,%g),Y(inset,%g)), Pointf(X(inset,%g),Y(inset,%g)));\n",
                    c.c0.x,c.c0.y, c.c1.x,c.c1.y, c.a1.x,c.a1.y);
    else
        out<<Format("    p.Quadratic(Pointf(X(inset,%g),Y(inset,%g)), Pointf(X(inset,%g),Y(inset,%g)));\n",
                    c.c0.x,c.c0.y, c.a1.x,c.a1.y);
    if(c.closed) out<<"    p.Close();\n";
    if(s.style.opacity<1.0) out<<Format("    p.Opacity(%g);\n", s.style.opacity);
    if(!s.style.dash.IsEmpty()) out<<Format("    p.Dash(String(\"%s\"),0.0);\n", ~s.style.dash);
    if(s.style.evenOdd) out<<"    p.EvenOdd(true);\n";
    if(c.closed && s.style.enableFill) out<<Format("    p.Fill(Color(%d,%d,%d));\n", s.style.fill.GetR(),s.style.fill.GetG(),s.style.fill.GetB());
    if(s.style.enableStroke)          out<<Format("    p.Stroke(%d, Color(%d,%d,%d));\n", s.style.strokeWidth,s.style.stroke.GetR(),s.style.stroke.GetG(),s.style.stroke.GetB());
    out<<"    p.End();\n\n";
}

// ===================== Registry Build =====================
struct FacetRow : Moveable<FacetRow> {
    PType t;
    PrimitiveOps ops;
    ToolSpec spec;
};

static Vector<FacetRow>& Facets(){
    static Vector<FacetRow> F;
    if(!F.IsEmpty()) return F;

    auto add=[&](PType t, PrimitiveOps ops, const char* label, const char* tip){
        FacetRow& r=F.Add(); r.t=t; r.ops=ops; r.spec.type=t; r.spec.label=label; r.spec.tip=tip;
    };

    PrimitiveOps R{ Rect_EmitPainter, Rect_HitBody, Rect_HitVertex, Rect_DrawOverlay,
                    Rect_BeginCreate, Rect_DragCreate, Rect_BeginEdit, Rect_DragEdit, Rect_EmitCode };
    PrimitiveOps C{ Circle_EmitPainter, Circle_HitBody, Circle_HitVertex, Circle_DrawOverlay,
                    Circle_BeginCreate, Circle_DragCreate, Circle_BeginEdit, Circle_DragEdit, Circle_EmitCode };
    PrimitiveOps L{ Line_EmitPainter, Line_HitBody, Line_HitVertex, Line_DrawOverlay,
                    Line_BeginCreate, Line_DragCreate, Line_BeginEdit, Line_DragEdit, Line_EmitCode };
    PrimitiveOps T{ Triangle_EmitPainter, Triangle_HitBody, Triangle_HitVertex, Triangle_DrawOverlay,
                    Triangle_BeginCreate, Triangle_DragCreate, Triangle_BeginEdit, Triangle_DragEdit, Triangle_EmitCode };
    PrimitiveOps TX{ Text_EmitPainter, Text_HitBody, Text_HitVertex, Text_DrawOverlay,
                     Text_BeginCreate, Text_DragCreate, Text_BeginEdit, Text_DragEdit, Text_EmitCode };
    PrimitiveOps CV{ Curve_EmitPainter, Curve_HitBody, Curve_HitVertex, Curve_DrawOverlay,
                     Curve_BeginCreate, Curve_DragCreate, Curve_BeginEdit, Curve_DragEdit, Curve_EmitCode };

    add(PType::Rect,     R,  "Rect",     "Insert rectangle");
    add(PType::Circle,   C,  "Circle",   "Insert circle");
    add(PType::Line,     L,  "Line",     "Insert line");
    add(PType::Triangle, T,  "Triangle", "Insert triangle");
    add(PType::Text,     TX, "Text",     "Insert text");
    add(PType::Curve,    CV, "Curve",    "Insert curve");

    // ---- DEBUG DUMP: verify wiring of ops table at startup ----
    if(gDebug){
        Cout() << "[Facets] registry built:\n";
        for(const auto& r : F){
            Cout() << "  " << PTypeName(r.t)
                   << " Emit="        << (const void*)r.ops.EmitPainter
                   << " HitBody="     << (const void*)r.ops.HitBody
                   << " HitVertex="   << (const void*)r.ops.HitVertex
                   << " DrawOverlay=" << (const void*)r.ops.DrawOverlay
                   << " BeginCreate=" << (const void*)r.ops.BeginCreate
                   << " DragCreate="  << (const void*)r.ops.DragCreate
                   << " BeginEdit="   << (const void*)r.ops.BeginEdit
                   << " DragEdit="    << (const void*)r.ops.DragEdit
                   << " EmitCode="    << (const void*)r.ops.EmitCode
                   << '\n';
        }
    }
    // -----------------------------------------------------------

    return F;
}

const PrimitiveOps& GetOps(PType t){
    const Vector<FacetRow>& v=Facets();
    for(const FacetRow& r : v) if(r.t==t) return r.ops;
    return v[0].ops;
}
const Vector<ToolSpec>& GetToolSpecs(){
    static Vector<ToolSpec> S; if(!S.IsEmpty()) return S;
    for(const FacetRow& r : Facets()) S.Add(r.spec);
    return S;
}

// ===================== Canvas (AA render, thin drag) =====================
struct Canvas : Ctrl {
    // model
    Vector<Shape> shapes;
    int           selected = -1;

    // interaction
    Tool   tool = Tool::Cursor;
    // ADDED: PType to create, decoupling Canvas from specific tools
    PType  creation_type = PType::Rect;
    bool   snap = true;
    bool   clip = true;
    int    grid = 8;

    // state
    bool   creating = false;
    bool   editing  = false;
    bool   moving   = false;

    int    drag_vertex = -1;
    Point  start_px;
    double grab_nx = 0.0, grab_ny = 0.0;

    // callbacks
    Callback WhenSelection;
    Callback WhenShapesChanged;

    Rect GetInsetRect() const {
        const Size sz = GetSize();
        const int iw = (sz.cx * 70) / 100;
        const int ih = (sz.cy * 70) / 100;
        const int l  = (sz.cx - iw) / 2;
        const int t  = (sz.cy - ih) / 2 + 40;
        return RectC(l, t, iw, ih);
    }


// Canvas::Paint — fix black frame by using an alpha image buffer
// Canvas::Paint — alpha layer, ops-dispatch logging, overlay & border
// Canvas::Paint — render whole inset into an OPAQUE buffer and blit once
// Canvas::Paint — render inset into an alpha ImageBuffer and blit once
//suppresses painting the selected circle via BufferPainter while it’s being created/edite
// short-circuit everything except the static background during a live circle interaction
//multi test ,BYPASS_PAINTER_WHEN_ANY_CIRCLE ,OPAQUE_PAINTER_TEST,SKIP_OVERLAY_TEST
//found overlay was issue : fix to set painter layer opaque  guarantees pixels defined each frame; nothing transparent for compositor to “guess,”.

void Paint(Draw& w) override
{
    const Size sz = GetSize();
    w.DrawRect(sz, SColorFace());

    const Rect ir = GetInsetRect();

    // Inset background (what the painter layer sits on)
    w.DrawRect(ir, White());

    // Grid
    if(grid > 0) {
        for(int x = ir.left + grid; x < ir.right; x += grid)
            w.DrawRect(RectC(x, ir.top, 1, ir.Height()), Color(230,230,230));
        for(int y = ir.top + grid; y < ir.bottom; y += grid)
            w.DrawRect(RectC(ir.left, y, ir.Width(), 1), Color(230,230,230));
    }

    // ----- Painter layer: OPAQUE, no alpha -----
    {
        ImageBuffer ib(ir.GetSize());
        ib.SetKind(IMAGE_OPAQUE);          // <— key change: opaque, not alpha

        BufferPainter p(ib, MODE_ANTIALIASED);
        p.Clear(White());                  // <— paint a solid background

        // Optional clip to 0..inset size for safety
        if(clip) {
            p.Begin();
            p.Move(Pointf(0, 0));
            p.Line(Pointf(ir.Width(), 0));
            p.Line(Pointf(ir.Width(), ir.Height()));
            p.Line(Pointf(0, ir.Height()));
            p.Close();
            p.Clip();
        }

        const Rect inset0 = RectC(0, 0, ir.Width(), ir.Height());
        for(const Shape& s : shapes)
            GetOps(s.type).EmitPainter(p, inset0, s);

        if(clip) p.End();

        // Blit opaque painter layer into the inset
        w.DrawImage(ir.left, ir.top, Image(ib));
    }

    // Selection overlay last (drawn directly on 'w')
    if(selected >= 0 && selected < shapes.GetCount())
        GetOps(shapes[selected].type).DrawOverlay(w, ir, shapes[selected]);

    // Inset border
    w.DrawRect(RectC(ir.left,  ir.top,    ir.Width(), 1), SColorDisabled());
    w.DrawRect(RectC(ir.left,  ir.bottom, ir.Width(), 1), SColorDisabled());
    w.DrawRect(RectC(ir.left,  ir.top,    1,           ir.Height()), SColorDisabled());
    w.DrawRect(RectC(ir.right, ir.top,    1,           ir.Height()), SColorDisabled());
}



void LeftDown(Point p, dword) override {
    SetFocus(); SetCapture();
    const Rect ir = GetInsetRect();

    creating = editing = moving = false;
    drag_vertex = -1;

    if(gDebug){
        cout << "[LeftDown] tool=" << ToolName(tool)
             << " p=(" << p.x << "," << p.y << ")"
             << " ir=" << ir.ToString().ToStd()
             << " shapes=" << shapes.GetCount() << endl;
    }

    if(tool == Tool::Cursor) {
        int pick = -1;
        for(int i = shapes.GetCount()-1; i >= 0; --i)
            if(GetOps(shapes[i].type).HitBody(ir, shapes[i], p)) { pick = i; break; }

        if(selected != pick) {
            selected = pick;
            if(WhenSelection) WhenSelection();
        }

        if(selected >= 0) {
            Shape& s = shapes[selected];
            drag_vertex = GetOps(s.type).HitVertex(ir, s, p, 6);
            GetOps(s.type).BeginEdit(s, ir, p, drag_vertex, grab_nx, grab_ny);
            editing = true;
            moving  = (drag_vertex < 0);

            if(gDebug){
                cout << "  CursorSelect idx=" << selected
                     << " type=" << PTypeName(shapes[selected].type)
                     << " hv=" << drag_vertex
                     << " moving=" << int(moving)
                     << " grabN=(" << grab_nx << "," << grab_ny << ")" << endl;
            }
        }
        Refresh();
        return;
    }

    // CreateShape
    if(tool == Tool::CreateShape && ir.Contains(p)) {
        start_px = snap ? Point(Snap1D(p.x, ir.left, grid), Snap1D(p.y, ir.top, grid)) : p;

        Shape s;
        s.type = creation_type;
        shapes.Add(s);
        selected = shapes.GetCount()-1;

        GetOps(shapes[selected].type).BeginCreate(shapes[selected], ir, start_px);
        creating = true;
        if(WhenSelection) WhenSelection();

        if(gDebug){
            cout << "  BeginCreate idx=" << selected
                 << " type=" << PTypeName(creation_type)
                 << " start=(" << start_px.x << "," << start_px.y << ")" << endl;
        }
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
            GetOps(shapes[selected].type).DragEdit(shapes[selected], ir, p, snap, grid, moving, drag_vertex, grab_nx, grab_ny);
            Refresh();
        }
    }


// Canvas::LeftUp — instrumented
void LeftUp(Point, dword) override {
    if(HasCapture()) {
        ReleaseCapture();
        bool was = creating || editing;
        creating = editing = moving = false;
        drag_vertex = -1;
        if(WhenShapesChanged && was) WhenShapesChanged();
        if(gDebug) cout << "[LeftUp] end creating/editing; Refresh()\n";
        Refresh();
    }
}

    void LostCapture()  {
        creating = editing = moving = false;
        drag_vertex = -1;
        Refresh();
    }
    
    bool Key(dword key, int) override {
        if(key == K_DELETE && selected >= 0 && selected < shapes.GetCount()) {
            shapes.Remove(selected);
            selected = -1;
            if(WhenSelection) WhenSelection();
            if(WhenShapesChanged) WhenShapesChanged();
            Refresh();
            return true;
        }
        return false;
    }
    
    void ClearAll() {
        shapes.Clear();
        selected = -1;
        if(WhenShapesChanged) WhenShapesChanged();
        Refresh();
    }

    void DeleteSelected() {
        if(selected >= 0 && selected < shapes.GetCount()) {
            shapes.Remove(selected);
            selected = -1;
            if(WhenShapesChanged) WhenShapesChanged();
            Refresh();
        }
    }

};


// ===================== Main Window (UI, wiring) =====================
struct MainWin : TopWindow {
	typedef MainWin CLASSNAME;
    // ... (keep layout containers and controls)
    Splitter   split;
    ParentCtrl left, right;
    StaticRect rowTools, rowOps, rowActions, rowStyle, rowCanvas;
    ParentCtrl toolbox;
    Button     bCursor;
    Option  cbSnap, cbClip;
    EditInt edGrid;
    Label   lblGrid;
    Button bClear, bDelete;
    Option       cbFill, cbStroke, cbEvenOdd, cbOutline;
    ColorPusher  cFill, cStroke, cOutline;
    EditInt      spinStrokeW, spinOutlineW;
    EditDouble   edOpacity;
    EditString   edDash;
    Label        lblStrokeW, lblOpacity, lblDash, lblOutW;
    Canvas canvas;
    StaticRect codeHdr;
    ParentCtrl codeHdrBox;
    Label      codeTitle;
    Button     bCopy;
    DocEdit    code;

    // ... (keep helper methods: UpdateCode, PushStyleToUI, etc.)
    void UpdateCode(){
        String out;
        out << "void DrawIcon(Draw& w, const Rect& inset)\n{\n";
        out << "    // painter setup elided in export snippet\n\n";
        for(const Shape& s : canvas.shapes)
            GetOps(s.type).EmitCode(out, s);
        out << "}\n";
        code <<= out;
    }

    void PushStyleToUI(){
        if(canvas.selected < 0 || canvas.selected >= canvas.shapes.GetCount())
            return;
        const ::Style& st = canvas.shapes[canvas.selected].style;
        cbFill     = st.enableFill;
        cbStroke   = st.enableStroke;
        cbEvenOdd  = st.evenOdd;
        cbOutline  = st.outlineEnable;

        cFill     <<= st.fill;
        cStroke   <<= st.stroke;
        cOutline  <<= st.outlineColor;

        spinStrokeW   <<= st.strokeWidth;
        spinOutlineW  <<= st.outlineWidth;
        edOpacity     <<= st.opacity;
        edDash        <<= st.dash;
    }

    void PullStyleFromUI(){
        if(canvas.selected < 0 || canvas.selected >= canvas.shapes.GetCount())
            return;
        ::Style& st = canvas.shapes[canvas.selected].style;

        st.enableFill    = (bool)~cbFill;
        st.enableStroke  = (bool)~cbStroke;
        st.evenOdd       = (bool)~cbEvenOdd;
        st.outlineEnable = (bool)~cbOutline;

        st.fill          = (Color)~cFill;
        st.stroke        = (Color)~cStroke;
        st.outlineColor  = (Color)~cOutline;

        st.strokeWidth   = (int)~spinStrokeW;
        st.outlineWidth  = (int)~spinOutlineW;
        st.opacity       = (double)~edOpacity;
        st.dash          = ~edDash;

        canvas.Refresh();
        UpdateCode();
    }

    void OnSnap()  { canvas.snap = (bool)~cbSnap; canvas.Refresh(); }
    void OnClip()  { canvas.clip = (bool)~cbClip; canvas.Refresh(); }
    void OnGrid()  { int g = (int)~edGrid; if(g < 2) g = 2; if(g > 64) g = 64; canvas.grid = g; canvas.Refresh(); }

    void OnSelectionChanged() { PushStyleToUI(); }
    void OnShapesChanged()    { UpdateCode(); }
    void OnCopyCode()         { WriteClipboardText(~code); PromptOK("Code copied to clipboard."); }

    // MODIFIED: BuildToolButtons now sets creation type, removing logic from Canvas
    void BuildToolButtons(){
        int x = 6;
        toolbox.Add(bCursor.LeftPos(x, 80).VSizePos(6, 6));
        bCursor.SetLabel("Cursor");
        bCursor << [=]{ canvas.tool = Tool::Cursor; };
        x += 86;

        for(const ToolSpec& sp : GetToolSpecs()){
            Button& b = *new Button;
            b.SetLabel(sp.label);
            b.Tip(sp.tip);
            // Set the generic creation tool and the specific shape type to create
            b.WhenAction = [=]{
                canvas.tool = Tool::CreateShape;
                canvas.creation_type = sp.type;
            };
            toolbox.Add(b.LeftPos(x, 90).VSizePos(6, 6));
            x += 96;
        }
    }

    MainWin() {
        Title("U++ Icon Builder — Modular Primitives").Sizeable().Zoomable();

        Add(split.SizePos());
        split.Horz(left, right);
        split.SetPos(6000); // ~60%

        // Left column layout
        left.Add(rowTools.TopPos(0, 40).HSizePos());
        left.Add(rowOps.TopPos(40, 28).HSizePos());
        left.Add(rowActions.TopPos(68, 32).HSizePos());
        left.Add(rowStyle.TopPos(100, 140).HSizePos());
        left.Add(rowCanvas.VSizePos(240, 0).HSizePos());

        // Tools row
        rowTools.SetFrame(ThinInsetFrame());
        rowTools.Add(toolbox.SizePos());
        BuildToolButtons();

        // Ops row: snap/clip/grid quick toggles
        rowOps.SetFrame(ThinInsetFrame());
        cbSnap.SetLabel("Snap");
        cbClip.SetLabel("Clip");
        lblGrid.SetText("Grid");

        rowOps.Add(cbSnap.LeftPos(6, 70).VCenterPos());
        rowOps.Add(cbClip.LeftPos(82, 70).VCenterPos());
        rowOps.Add(lblGrid.LeftPos(158, 40).VCenterPos());

        edGrid.MinMax(2, 64);
        edGrid <<= 8;
        rowOps.Add(edGrid.LeftPos(204, 60).VCenterPos());

        cbSnap.WhenAction = THISBACK(OnSnap);
        cbClip.WhenAction = THISBACK(OnClip);
        edGrid.WhenAction = THISBACK(OnGrid);

        // Actions
        rowActions.SetFrame(ThinInsetFrame());
        rowActions.Add(bClear.SetLabel("Clear").LeftPos(6, 80).VCenterPos());
        rowActions.Add(bDelete.SetLabel("Delete").LeftPos(92, 80).VCenterPos());
        bClear  << [=]{ canvas.ClearAll(); UpdateCode(); };
        bDelete << [=]{ canvas.DeleteSelected(); UpdateCode(); };

        // Style panel (global to selected shape)
        rowStyle.SetFrame(ThinInsetFrame());
        int y = 6, h = 24, pad = 4, x = 6, w = 110;

        rowStyle.Add(cbFill.SetLabel("Fill").LeftPos(x, w).TopPos(y, h)); x += w + 6;
        rowStyle.Add(cFill.LeftPos(x, 100).TopPos(y, h));                 x += 110;
        rowStyle.Add(cbStroke.SetLabel("Stroke").LeftPos(x, w).TopPos(y, h));
        x = 6; y += h + pad;

        rowStyle.Add(cStroke.LeftPos(x, 100).TopPos(y, h));               x += 110;
        lblStrokeW.SetText("Stroke W");
        rowStyle.Add(lblStrokeW.LeftPos(x, 70).TopPos(y, h));             x += 76;
        spinStrokeW.MinMax(0, 128);
        spinStrokeW <<= 2;
        rowStyle.Add(spinStrokeW.LeftPos(x, 60).TopPos(y, h));
        x = 6; y += h + pad;

        rowStyle.Add(cbEvenOdd.SetLabel("EvenOdd").LeftPos(x, 90).TopPos(y, h)); x += 96;
        lblOpacity.SetText("Opacity");
        rowStyle.Add(lblOpacity.LeftPos(x, 68).TopPos(y, h));                x += 74;
        edOpacity.MinMax(0.0, 1.0);
        edOpacity <<= 1.0;
        rowStyle.Add(edOpacity.LeftPos(x, 80).TopPos(y, h));
        x = 6; y += h + pad;

        lblDash.SetText("Dash");
        rowStyle.Add(lblDash.LeftPos(x, 40).TopPos(y, h));                   x += 46;
        rowStyle.Add(edDash.LeftPos(x, 190).TopPos(y, h));                   x += 200;
        rowStyle.Add(cbOutline.SetLabel("Outline").LeftPos(x, 80).TopPos(y, h)); x += 86;
        rowStyle.Add(cOutline.LeftPos(x, 100).TopPos(y, h));                 x += 110;
        lblOutW.SetText("OutW");
        rowStyle.Add(lblOutW.LeftPos(x, 46).TopPos(y, h));                   x += 52;
        spinOutlineW.MinMax(0, 128);
        spinOutlineW <<= 0;
        rowStyle.Add(spinOutlineW.LeftPos(x, 60).TopPos(y, h));

        // style change wiring
        cbFill.WhenAction       = THISBACK(PullStyleFromUI);
        cbStroke.WhenAction     = THISBACK(PullStyleFromUI);
        cbEvenOdd.WhenAction    = THISBACK(PullStyleFromUI);
        cbOutline.WhenAction    = THISBACK(PullStyleFromUI);
        cFill.WhenAction        = THISBACK(PullStyleFromUI);
        cStroke.WhenAction      = THISBACK(PullStyleFromUI);
        cOutline.WhenAction     = THISBACK(PullStyleFromUI);
        spinStrokeW.WhenAction  = THISBACK(PullStyleFromUI);
        spinOutlineW.WhenAction = THISBACK(PullStyleFromUI);
        edOpacity.WhenAction    = THISBACK(PullStyleFromUI);
        edDash.WhenAction       = THISBACK(PullStyleFromUI);

        // Canvas
        rowCanvas.SetFrame(ThinInsetFrame());
        rowCanvas.Add(canvas.SizePos());
        canvas.WhenSelection     = THISBACK(OnSelectionChanged);
        canvas.WhenShapesChanged = THISBACK(OnShapesChanged);

        // Right column (code)
        right.Add(codeHdr.TopPos(0, 32).HSizePos());
        right.Add(code.VSizePos(32, 0).HSizePos());

        codeHdr.SetFrame(ThinInsetFrame());
        codeHdr.Add(codeHdrBox.SizePos());
        codeHdrBox.Add(codeTitle.LeftPos(6, 300).VCenterPos());
        codeHdrBox.Add(bCopy.RightPos(6, 80).VCenterPos());
        codeTitle.SetText("Generated BufferPainter code");
        bCopy.SetLabel("Copy");
        bCopy.WhenAction = THISBACK(OnCopyCode);

        UpdateCode();
    }
};


// ===================== main =====================

GUI_APP_MAIN
{
    MainWin().Run();
}

