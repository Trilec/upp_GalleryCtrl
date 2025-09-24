/*
-------------------------------------------------------------------------------
 U++ Procedural Icon Builder – Drawing Demo (with future Gallery control)
-------------------------------------------------------------------------------
 Purpose
   • Self‑contained demo showing interactive drawing with BufferPainter.
   • Users can place/edit Rectangle / Circle / Line / Triangle primitives,
     tweak styles (fill/stroke/dash/opacity/outline), and export painter code.
   • Right pane emits a ready‑to‑paste function that draws the current design.

 Build (TheIDE, U++ 2025.1+)
   Package type: GUI application
   uses:
     Core,
     CtrlLib,
     Draw,
     Painter
   (No image plugins required unless you add raster I/O.)

 Design choices (keep in mind when extending)
   • All geometry inside Canvas is normalized to the inset rectangle (0..1).
   • Rendering goes through an abstract Emitter, implemented by PainterEmitter.
     This keeps path construction testable and decoupled from BufferPainter.
   • History uses JSON snapshots (ValueMap/ValueArray) – simple and robust.
   • All GUI work happens on the main thread; callbacks are short and safe.

 Future work (non‑breaking)
   • Plug‑in a GalleryCtrl on the left; it will simply manipulate Canvas.shapes
     and call Canvas.Refresh()/WhenModelChanged(). No threading required.
-------------------------------------------------------------------------------
*/

#include <CtrlLib/CtrlLib.h>
#include <Painter/Painter.h>
#include <Draw/Draw.h>

using namespace Upp;

// ===================== Shared model / helpers =====================

// Editing tools available in the UI
enum class Tool { Cursor, Rect, Circle, Line, Triangle };

// Style of a shape (value type – copied into shapes)
struct Style : Moveable<Style> {
    Color  fill   = Color(163,201,168);
    Color  stroke = Color(30,53,47);
    int    strokeWidth = 2;
    bool   evenOdd = false;     // Fill rule: true = Even‑Odd, false = NonZero

    String dash;                // "": solid  e.g. "6 4", "2 3", "6 3 2 3"

    bool   enableFill    = true;
    bool   enableStroke  = true;
    double opacity       = 1.0; // 0..1, applied to the painter

    // Optional outline pass (a contrasting stroke rendered as a separate pass)
    bool   outlineEnable  = false;
    Color  outlineColor   = Red();
    int    outlineWidth   = 0;    // extra width added to (strokeWidth*2)
    bool   outlineOutside = true; // true => draw outline pass first
};

// Pixel inset where normalized geometry is mapped
struct Inset { int x=50, y=50, w=400, h=300; };

// A normalized shape (all geometry in 0..1 relative to inset)
struct Shape : Moveable<Shape> {
    enum class Type { Rect, Circle, Line, Triangle } type;
    Style  style;

    // normalized geometry (0..1)
    double x=0,y=0,w=0,h=0;     // rect
    double cx=0,cy=0,r=0;       // circle (r relative to min(width,height))
    Pointf p1, p2, p3;          // line / triangle
    int    id=0;                // monotonically increasing identifier
};

// Normalized <-> pixel mapping helpers
static inline int  X (const Rect& r, double nx) { return r.left + int(r.Width() * nx + 0.5); }
static inline int  Y (const Rect& r, double ny) { return r.top  + int(r.Height()* ny + 0.5); }
static inline int  Rr(const Rect& r, double nr) { return int(min(r.Width(), r.Height()) * nr + 0.5); }

static inline double NX(const Rect& r, int px) { return (px - r.left)  / double(max(1, r.Width()));  }
static inline double NY(const Rect& r, int py) { return (py - r.top)   / double(max(1, r.Height())); }

// ===================== Emitters =====================

// Abstract path/styling emitter – allows swapping BufferPainter with tests
struct Emitter {
    virtual void Begin()=0;  virtual void End()=0;
    virtual void Move(Pointf p)=0;  virtual void Line(Pointf p)=0;
    virtual void Quadratic(Pointf p1, Pointf p)=0;  virtual void Cubic(Pointf p1, Pointf p2, Pointf p)=0;
    virtual void Close()=0;
    virtual void Fill(Color c)=0;  virtual void Stroke(double w, Color c)=0;
    virtual void Dash(const String& spec, double phase)=0;
    virtual void Opacity(double o)=0;  virtual void EvenOdd(bool eo)=0;
    virtual void Clip()=0;
    virtual ~Emitter(){}
};

// BufferPainter adapter implementing the Emitter interface
struct PainterEmitter : Emitter {
    BufferPainter& p;
    PainterEmitter(BufferPainter& p):p(p){}
    void Begin() override { p.Begin(); }     void End() override { p.End(); }
    void Move(Pointf a) override { p.Move(a); }    void Line(Pointf a) override { p.Line(a); }
    void Quadratic(Pointf a, Pointf b) override { p.Quadratic(a,b); }
    void Cubic(Pointf a, Pointf b, Pointf c) override { p.Cubic(a,b,c); }
    void Close() override { p.Close(); }
    void Fill(Color c) override { p.Fill(c); }
    void Stroke(double w, Color c) override { p.Stroke(w,c); }
    void Dash(const String& spec, double phase) override { if(!spec.IsEmpty()) p.Dash(spec, phase); }
    void Opacity(double o) override { if(o < 1.0) p.Opacity(o); }
    void EvenOdd(bool eo) override { if(eo) p.EvenOdd(true); }
    void Clip() override { p.Clip(); }
};

// ===================== Canvas =====================

struct Canvas : public Ctrl {
    typedef Canvas CLASSNAME;

    // Model & state
    Vector<Shape> shapes;  // back‑to‑front order (last draws on top)
    Style  defaultStyle;
    Inset  inset;
    Tool   tool = Tool::Cursor;
    bool   clip_to_inset = false;
    bool   snap = true;
    int    grid = 20;      // px grid for snapping

    // drawing & editing state
    bool   drawing = false;
    int    selected = -1;
    Point  start;                 // mouse‑down in pixels during creation

    // editing
    bool   moving = false;        // dragging the whole primitive
    int    drag_vertex = -1;      // which handle/vertex (‑1 = none)
    Point  grab_px;               // pixel position at grab

    // for move – normalized grab offset to preserve relative position while moving
    double grab_nx = 0, grab_ny = 0;

    // host callbacks
    Callback WhenModelChanged;      // fire on any model mutation
    Callback WhenSelectionChanged;  // fire when selection changes

    Canvas() { NoTransparent(); }
    ~Canvas() {
        // Clear callbacks to avoid late invocations during teardown
        WhenModelChanged.Clear();
        WhenSelectionChanged.Clear();
    }

    // Current inset rectangle in pixels
    Rect GetInsetRect() const { return RectC(inset.x, inset.y, inset.w, inset.h); }

    // --------- snapping helpers ---------
    static inline int  Snap1D(int v, int origin, int step) {
        // Rounds to the nearest multiple of 'step' relative to 'origin'
        return origin + ((v - origin + step/2) / step) * step;
    }
    static inline Point SnapPt(Point p, const Rect& ir, int step) {
        return Point(Snap1D(p.x, ir.left, step),
                     Snap1D(p.y, ir.top,  step));
    }

    // ---------- path helpers for painter preview ----------
    static void EmitRectPath(Emitter& e, const Rect& r){
        e.Begin();
        e.Move(Pointf(r.left, r.top));
        e.Line(Pointf(r.right, r.top));
        e.Line(Pointf(r.right, r.bottom));
        e.Line(Pointf(r.left, r.bottom));
        e.Close();
    }

    // Circle approximated with 4 cubic Beziers (classic kappa method)
    static void EmitCirclePath(Emitter& e, Point c0, int radius) {
        const double k = 0.5522847498307936; // 4*(sqrt(2)-1)/3
        double rx = radius, ry = radius;
        Pointf c(c0);
        Pointf p0 = c + Pointf(0, -ry);
        Pointf p1 = c + Pointf( k*rx, -ry);
        Pointf p2 = c + Pointf( rx, -k*ry);
        Pointf p3 = c + Pointf( rx, 0);
        Pointf p4 = c + Pointf( rx, k*ry);
        Pointf p5 = c + Pointf( k*rx, ry);
        Pointf p6 = c + Pointf( 0, ry);
        Pointf p7 = c + Pointf(-k*rx, ry);
        Pointf p8 = c + Pointf(-rx, k*ry);
        Pointf p9 = c + Pointf(-rx, 0);
        Pointf p10= c + Pointf(-rx,-k*ry);
        Pointf p11= c + Pointf(-k*rx,-ry);

        e.Begin();
        e.Move(p0);
        e.Cubic(p1, p2, p3);
        e.Cubic(p4, p5, p6);
        e.Cubic(p7, p8, p9);
        e.Cubic(p10,p11,p0);
        e.Close();
    }

    // Style pass (main vs. outline) applied to the *current* path
    static void StylePass(Emitter& e, const Style& st, bool main_pass, bool is_line){
        if(main_pass){
            if(st.opacity < 1.0)         e.Opacity(st.opacity);
            if(!st.dash.IsEmpty())       e.Dash(st.dash, 0.0);
            if(st.enableFill && !is_line) e.Fill(st.fill);
            if(st.enableStroke)          e.Stroke(st.strokeWidth, st.stroke);
            if(st.evenOdd)               e.EvenOdd(true);
        } else {
            if(st.outlineEnable && st.outlineWidth > 0)
                e.Stroke(st.strokeWidth + 2 * max(0, st.outlineWidth), st.outlineColor);
        }
    }

    // Build path for a shape and apply style passes (optionally under clip)
    static void EmitShape(Emitter& e, const Shape& s, const Rect& inset, bool /*clip_to_inset*/) {
        auto rebuild = [&]{
            switch(s.type){
            case Shape::Type::Rect: {
                Rect r(X(inset, s.x), Y(inset, s.y),
                       X(inset, s.x + s.w), Y(inset, s.y + s.h));
                EmitRectPath(e, r);
                break;
            }
            case Shape::Type::Circle:
                EmitCirclePath(e, Point(X(inset, s.cx), Y(inset, s.cy)), Rr(inset, s.r));
                break;
            case Shape::Type::Line:
                e.Begin();
                e.Move(Pointf(X(inset, s.p1.x), Y(inset, s.p1.y)));
                e.Line(Pointf(X(inset, s.p2.x), Y(inset, s.p2.y)));
                break;
            case Shape::Type::Triangle:
                e.Begin();
                e.Move(Pointf(X(inset, s.p1.x), Y(inset, s.p1.y)));
                e.Line(Pointf(X(inset, s.p2.x), Y(inset, s.p2.y)));
                e.Line(Pointf(X(inset, s.p3.x), Y(inset, s.p3.y)));
                e.Close();
                break;
            }
        };

        const bool is_line = (s.type == Shape::Type::Line);

        if(s.style.outlineEnable && s.style.outlineOutside){
            rebuild(); StylePass(e, s.style, false, is_line); e.End();
        }
        rebuild(); StylePass(e, s.style, true,  is_line); e.End();
        if(s.style.outlineEnable && !s.style.outlineOutside){
            rebuild(); StylePass(e, s.style, false, is_line); e.End();
        }
    }

    // ---------- hit‑testing ----------
    // Returns index of the grabbed handle for the selected shape, or ‑1
    int HitVertex(Point m, int px=6) const {
        if(selected < 0 || selected >= shapes.GetCount()) return -1;
        const Shape& s = shapes[selected];
        Rect ir = GetInsetRect();
        auto P=[&](Pointf q){ return Point(X(ir,q.x), Y(ir,q.y)); };
        if(s.type == Shape::Type::Line) {
            if(std::abs(P(s.p1).x - m.x) <= px && std::abs(P(s.p1).y - m.y) <= px) return 0;
            if(std::abs(P(s.p2).x - m.x) <= px && std::abs(P(s.p2).y - m.y) <= px) return 1;
        }
        if(s.type == Shape::Type::Triangle) {
            if(std::abs(P(s.p1).x - m.x) <= px && std::abs(P(s.p1).y - m.y) <= px) return 0;
            if(std::abs(P(s.p2).x - m.x) <= px && std::abs(P(s.p2).y - m.y) <= px) return 1;
            if(std::abs(P(s.p3).x - m.x) <= px && std::abs(P(s.p3).y - m.y) <= px) return 2;
        }
        if(s.type == Shape::Type::Rect) {
            Rect r(X(ir,s.x),Y(ir,s.y), X(ir,s.x+s.w),Y(ir,s.y+s.h));
            if(std::abs(r.left   - m.x) <= px && std::abs(r.top    - m.y) <= px) return 0;
            if(std::abs(r.right  - m.x) <= px && std::abs(r.top    - m.y) <= px) return 1;
            if(std::abs(r.right  - m.x) <= px && std::abs(r.bottom - m.y) <= px) return 2;
            if(std::abs(r.left   - m.x) <= px && std::abs(r.bottom - m.y) <= px) return 3;
        }
        if(s.type == Shape::Type::Circle) {
            Point c(X(ir,s.cx), Y(ir,s.cy));
            int   r = Rr(ir, s.r);
            Rect  bb = RectC(c.x-r, c.y-r, 2*r, 2*r);
            // east radius handle
            if(std::abs(bb.right - m.x) <= px && std::abs(c.y - m.y) <= px) return 0;
        }
        return -1;
    }

    // Body hit (coarse – bounding geometry)
    bool HitBody(Point m) const {
        if(selected < 0 || selected >= shapes.GetCount()) return false;
        const Shape& s = shapes[selected];
        Rect ir = GetInsetRect();
        switch(s.type){
            case Shape::Type::Rect: {
                Rect r(X(ir,s.x),Y(ir,s.y), X(ir,s.x+s.w),Y(ir,s.y+s.h));
                return r.Contains(m);
            }
            case Shape::Type::Circle: {
                Point c(X(ir,s.cx), Y(ir,s.cy));
                int   r = Rr(ir, s.r);
                return (m.x-c.x)*(m.x-c.x) + (m.y-c.y)*(m.y-c.y) <= r*r;
            }
            case Shape::Type::Line: {
                // Bounding box with padding – good UX and fast
                int x1=X(ir,s.p1.x), y1=Y(ir,s.p1.y), x2=X(ir,s.p2.x), y2=Y(ir,s.p2.y);
                return Rect(min(x1,x2),min(y1,y2), max(x1,x2),max(y1,y2)).Inflated(4).Contains(m);
            }
            case Shape::Type::Triangle: {
                int x1=X(ir,s.p1.x), y1=Y(ir,s.p1.y);
                int x2=X(ir,s.p2.x), y2=Y(ir,s.p2.y);
                int x3=X(ir,s.p3.x), y3=Y(ir,s.p3.y);
                Rect bb(min(min(x1,x2),x3), min(min(y1,y2),y3),
                        max(max(x1,x2),x3), max(max(y1,y2),y3));
                return bb.Contains(m);
            }
        }
        return false;
    }

    // Draw a single handle (dual‑tone square for contrast)
    void DrawHandle(Draw& w, Point pt, Color c = SColorText()) const {
        w.DrawRect(RectC(pt.x - 4, pt.y - 3, 6, 6), c);
        w.DrawRect(RectC(pt.x - 3, pt.y - 2, 4, 4), White());
    }

    // ---------- painting ----------
    void Paint(Draw& w) override {
        Size sz = GetSize();
        w.DrawRect(sz, SColorFace());
        if(sz.cx <= 2 || sz.cy <= 2) return;

        if(shapes.IsEmpty() && selected >= 0)
            selected = -1;

        const Rect ir = GetInsetRect();

        // inset background
        w.DrawRect(ir, White());

        // grid (light lines)
        for(int x = ir.left + grid; x < ir.right; x += grid)
            w.DrawLine(x, ir.top, x, ir.bottom, 1, SColorPaper());
        for(int y = ir.top + grid; y < ir.bottom; y += grid)
            w.DrawLine(ir.left, y, ir.right, y, 1, SColorPaper());

        // inset dotted frame
        for(int i = ir.left; i < ir.right; i += 6)
            w.DrawLine(i, ir.top, min(i + 3, ir.right), ir.top, 1, SColorHighlight());
        for(int i = ir.left; i < ir.right; i += 6)
            w.DrawLine(i, ir.bottom, min(i + 3, ir.right), ir.bottom, 1, SColorHighlight());
        for(int i = ir.top; i < ir.bottom; i += 6)
            w.DrawLine(ir.left, i, ir.left, min(i + 3, ir.bottom), 1, SColorHighlight());
        for(int i = ir.top; i < ir.bottom; i += 6)
            w.DrawLine(ir.right, i, ir.right, min(i + 3, ir.bottom), 1, SColorHighlight());

        // ---- painter preview (optional clip applied ONCE) ----
        ImageBuffer ib(sz);
        BufferPainter p(ib);
        p.Clear(RGBAZero());

        // Clip to inset so painter drawing respects the white box
        if(clip_to_inset) {
            p.Begin();
            p.Move(Pointf(ir.left,  ir.top));
            p.Line(Pointf(ir.right, ir.top));
            p.Line(Pointf(ir.right, ir.bottom));
            p.Line(Pointf(ir.left,  ir.bottom));
            p.Close();
            p.Clip();              // consume current path as clip; no End() here
        }

        // draw all shapes under the (optional) clip
        {
            PainterEmitter pe(p);
            for(const Shape& s : shapes)
                EmitShape(pe, s, ir, /*clip_to_inset=*/false);
        }

        if(clip_to_inset)
            p.End();           // pop clip

        w.DrawImage(0, 0, Image(ib));

        // ---- selection overlay (NOT clipped) ----
        if(selected >= 0 && selected < shapes.GetCount()) {
            const Shape& s = shapes[selected];
            const Color sel = SColorMark();

            auto Box = [&](Rect r) {
                w.DrawRect(RectC(r.left, r.top, r.Width(), 1), sel);
                w.DrawRect(RectC(r.left, r.bottom, r.Width(), 1), sel);
                w.DrawRect(RectC(r.left, r.top, 1, r.Height()), sel);
                w.DrawRect(RectC(r.right, r.top, 1, r.Height() + 1), sel);
            };

            switch(s.type) {
            case Shape::Type::Rect: {
                Rect r(X(ir, s.x), Y(ir, s.y),
                       X(ir, s.x + s.w), Y(ir, s.y + s.h));
                Box(r);
                DrawHandle(w, Point(r.left,  r.top));
                DrawHandle(w, Point(r.right, r.top));
                DrawHandle(w, Point(r.right, r.bottom));
                DrawHandle(w, Point(r.left,  r.bottom));
                break;
            }
            case Shape::Type::Circle: {
                int cx = X(ir, s.cx), cy = Y(ir, s.cy), rr = Rr(ir, s.r);
                Box(RectC(cx - rr, cy - rr, 2 * rr, 2 * rr));
                DrawHandle(w, Point(cx, cy));        // center
                DrawHandle(w, Point(cx + rr, cy));   // radius handle (east)
                break;
            }
            case Shape::Type::Line: {
                int x1 = X(ir, s.p1.x), y1 = Y(ir, s.p1.y);
                int x2 = X(ir, s.p2.x), y2 = Y(ir, s.p2.y);
                w.DrawLine(x1, y1, x2, y2, 1, sel);
                DrawHandle(w, Point(x1, y1));
                DrawHandle(w, Point(x2, y2));
                break;
            }
            case Shape::Type::Triangle: {
                int x1 = X(ir, s.p1.x), y1 = Y(ir, s.p1.y);
                int x2 = X(ir, s.p2.x), y2 = Y(ir, s.p2.y);
                int x3 = X(ir, s.p3.x), y3 = Y(ir, s.p3.y);
                w.DrawLine(x1, y1, x2, y2, 1, sel);
                w.DrawLine(x2, y2, x3, y3, 1, sel);
                w.DrawLine(x3, y3, x1, y1, 1, sel);
                DrawHandle(w, Point(x1, y1));
                DrawHandle(w, Point(x2, y2));
                DrawHandle(w, Point(x3, y3));
                break;
            }
            }
        }
    }

    // ---------- mouse ----------
    void LeftDown(Point p, dword) override {
        SetFocus();
        SetCapture();

        Rect ir = GetInsetRect();

        // Cursor mode: select and start editing (move / vertex drag)
        if(tool == Tool::Cursor) {
            // pick top‑most shape containing point (walk back‑to‑front)
            int best = -1;
            for(int i = shapes.GetCount()-1; i >= 0; --i) {
                const Shape& s = shapes[i];
                Rect bb;
                switch(s.type){
                    case Shape::Type::Rect:
                        bb = Rect(X(ir,s.x),Y(ir,s.y), X(ir,s.x+s.w),Y(ir,s.y+s.h));
                        break;
                    case Shape::Type::Circle:
                        bb = RectC(X(ir,s.cx)-Rr(ir,s.r), Y(ir,s.cy)-Rr(ir,s.r), 2*Rr(ir,s.r), 2*Rr(ir,s.r));
                        break;
                    case Shape::Type::Line: {
                        int x1=X(ir,s.p1.x), y1=Y(ir,s.p1.y), x2=X(ir,s.p2.x), y2=Y(ir,s.p2.y);
                        bb = Rect(min(x1,x2),min(y1,y2), max(x1,x2),max(y1,y2)).Inflated(6);
                        break;
                    }
                    case Shape::Type::Triangle: {
                        int x1=X(ir,s.p1.x), y1=Y(ir,s.p1.y);
                        int x2=X(ir,s.p2.x), y2=Y(ir,s.p2.y);
                        int x3=X(ir,s.p3.x), y3=Y(ir,s.p3.y);
                        bb = Rect(min(min(x1,x2),x3), min(min(y1,y2),y3),
                                  max(max(x1,x2),x3), max(max(y1,y2),y3)).Inflated(6);
                        break;
                    }
                }
                if(bb.Contains(p)) { best = i; break; }
            }
            selected = best;
            moving = false;
            drag_vertex = -1;
            if(selected >= 0) {
                // prefer a handle if close to one
                int hv = HitVertex(p);
                if(hv >= 0) { drag_vertex = hv; grab_px = p; }
                else if(HitBody(p)) {
                    // begin move; remember grab offset so the shape follows naturally
                    moving = true; grab_px = p;
                    const Shape& s = shapes[selected];
                    if(s.type == Shape::Type::Rect) { grab_nx = NX(ir, p.x) - s.x;  grab_ny = NY(ir, p.y) - s.y; }
                    else if(s.type == Shape::Type::Circle) { grab_nx = NX(ir,p.x) - s.cx; grab_ny = NY(ir,p.y) - s.cy; }
                    else if(s.type == Shape::Type::Line) { grab_nx = NX(ir,p.x) - s.p1.x; grab_ny = NY(ir,p.y) - s.p1.y; }
                    else { /* triangle */             grab_nx = NX(ir,p.x) - s.p1.x; grab_ny = NY(ir,p.y) - s.p1.y; }
                }
            }
            Refresh();
            if(WhenSelectionChanged) WhenSelectionChanged();
            return;
        }

        // Create a new primitive (non‑cursor tools)
        if(!(tool == Tool::Rect || tool == Tool::Circle || tool == Tool::Line || tool == Tool::Triangle)) return;
        if(!ir.Contains(p)) return;

        start = snap ? SnapPt(p, ir, grid) : p;

        Shape s;
        s.id = shapes.IsEmpty() ? 1 : shapes.Top().id + 1;
        s.style = defaultStyle;

        switch(tool){
        case Tool::Rect:
            s.type = Shape::Type::Rect;
            s.x = NX(ir, p.x);
            s.y = NY(ir, p.y);
            s.w = s.h = 0.0;
            break;
        case Tool::Circle:
            s.type = Shape::Type::Circle;
            s.cx = NX(ir, p.x);
            s.cy = NY(ir, p.y);
            s.r = 0.0;
            break;
        case Tool::Line:
            s.type = Shape::Type::Line;
            s.p1 = Pointf(NX(ir,p.x), NY(ir,p.y));
            s.p2 = s.p1;
            break;
        case Tool::Triangle:
            s.type = Shape::Type::Triangle;
            s.p1 = Pointf(NX(ir,p.x), NY(ir,p.y));
            s.p2 = s.p1; s.p3 = s.p1;
            break;
        default: break;
        }
        shapes.Add(s);
        selected = shapes.GetCount()-1;
        drawing = true;
        Refresh();
        if(WhenModelChanged) WhenModelChanged();   // snapshot on create
    }

    void MouseMove(Point p, dword) override {
        Rect ir = GetInsetRect();

        // live edit
        if(tool == Tool::Cursor && selected >= 0 && selected < shapes.GetCount()) {
            Shape& s = shapes[selected];

            if(moving) {
                double nx, ny;
                if(snap) { Point sp = SnapPt(p, ir, grid); nx = NX(ir, sp.x); ny = NY(ir, sp.y); }
                else     { nx = NX(ir, p.x);              ny = NY(ir, p.y); }

                if(s.type == Shape::Type::Rect) { s.x = nx - grab_nx; s.y = ny - grab_ny; }
                else if(s.type == Shape::Type::Circle) { s.cx = nx - grab_nx; s.cy = ny - grab_ny; }
                else if(s.type == Shape::Type::Line) {
                    Pointf delta(nx - grab_nx - s.p1.x, ny - grab_ny - s.p1.y);
                    s.p1.x += delta.x; s.p1.y += delta.y; s.p2.x += delta.x; s.p2.y += delta.y;
                }
                else { // triangle: move all points by delta (grab relative to p1)
                    Pointf delta(nx - grab_nx - s.p1.x, ny - grab_ny - s.p1.y);
                    s.p1.x += delta.x; s.p1.y += delta.y;
                    s.p2.x += delta.x; s.p2.y += delta.y;
                    s.p3.x += delta.x; s.p3.y += delta.y;
                }
                Refresh();
                return;
            }

            if(drag_vertex >= 0) {
                // Apply snapping to vertex/handle edits as well
                Point sp = snap ? SnapPt(p, ir, grid) : p;

                if(s.type == Shape::Type::Rect) {
                    // four corners 0..3 TL, TR, BR, BL
                    double nx = NX(ir, sp.x), ny = NY(ir, sp.y);
                    double x2 = s.x + s.w, y2 = s.y + s.h;
                    switch(drag_vertex){
                        case 0: s.x = nx; s.y = ny; s.w = x2 - s.x; s.h = y2 - s.y; break;
                        case 1: s.y = ny; s.w = nx - s.x; s.h = y2 - s.y; break;
                        case 2: s.w = nx - s.x; s.h = ny - s.y; break;
                        case 3: s.x = nx; s.w = x2 - s.x; s.h = ny - s.y; break;
                    }
                } else if(s.type == Shape::Type::Circle) {
                    // drag radius handle on the east – keep a circular radius in normalized space
                    double dx = NX(ir, sp.x) - s.cx;
                    double dy = NY(ir, sp.y) - s.cy;
                    s.r = sqrt(dx*dx + dy*dy);
                } else if(s.type == Shape::Type::Line) {
                    if(drag_vertex == 0) s.p1 = Pointf(NX(ir, sp.x), NY(ir, sp.y));
                    else                 s.p2 = Pointf(NX(ir, sp.x), NY(ir, sp.y));
                } else { // triangle 0..2 = p1..p3
                    if(drag_vertex == 0) s.p1 = Pointf(NX(ir, sp.x), NY(ir, sp.y));
                    if(drag_vertex == 1) s.p2 = Pointf(NX(ir, sp.x), NY(ir, sp.y));
                    if(drag_vertex == 2) s.p3 = Pointf(NX(ir, sp.x), NY(ir, sp.y));
                }
                Refresh();
                return;
            }
        }

        // creating new primitive
        if(!drawing) return;

        Shape& s = shapes[selected];
        Point q = snap ? SnapPt(p, ir, grid) : p;

        switch(s.type){
        case Shape::Type::Rect: {
            double x0 = NX(ir, start.x);
            double y0 = NY(ir, start.y);
            double x1 = NX(ir, q.x);
            double y1 = NY(ir, q.y);
            s.x = min(x0,x1); s.y = min(y0,y1);
            s.w = fabs(x1-x0); s.h = fabs(y1-y0);
            break;
        }
        case Shape::Type::Circle: {
            double dx = NX(ir, q.x) - s.cx;
            double dy = NY(ir, q.y) - s.cy;
            s.r = sqrt(dx*dx + dy*dy);
            break;
        }
        case Shape::Type::Line:
            s.p2 = Pointf(NX(ir, q.x), NY(ir, q.y));
            break;
        case Shape::Type::Triangle: {
            Pointf p1 = s.p1;
            Pointf p2 = Pointf(NX(ir, q.x), NY(ir, q.y));
            Pointf p3 = Pointf(2*p1.x - p2.x, p2.y); // isosceles base on p1‑p3
            s.p2 = p2; s.p3 = p3;
            break;
        }
        default: break;
        }
        Refresh();
    }

    void LeftUp(Point, dword) override {
        ReleaseCapture();
        bool edited = drawing || moving || (drag_vertex >= 0);
        drawing = false; moving = false; drag_vertex = -1;
        Refresh();
        if(edited && WhenModelChanged) WhenModelChanged();
    }

    // ---------- edit ops ----------
    void DeleteSelection() {
        if(selected >= 0 && selected < shapes.GetCount()){
            shapes.Remove(selected);
            selected = min(selected, shapes.GetCount() - 1);
            Refresh();
            // Call synchronously; do NOT PostCallback here (can fire after teardown)
            if(WhenModelChanged)        WhenModelChanged();
            if(WhenSelectionChanged)    WhenSelectionChanged();
        }
    }
    void DuplicateSelection() {
        if(selected >= 0 && selected < shapes.GetCount()){
            Shape c = shapes[selected];
            c.id = shapes.IsEmpty() ? 1 : shapes.Top().id + 1;
            shapes.Insert(selected+1, c);
            selected++;
            Refresh();
            if(WhenModelChanged) WhenModelChanged();
        }
    }
    void LayerUp()   { if(selected >= 0 && selected+1 < shapes.GetCount()) { Swap(shapes[selected], shapes[selected+1]); selected++; Refresh(); if(WhenModelChanged) WhenModelChanged(); } }
    void LayerDown() { if(selected > 0)                                     { Swap(shapes[selected], shapes[selected-1]); selected--; Refresh(); if(WhenModelChanged) WhenModelChanged(); } }

    void FlipSelection(bool horizontal) {
        if(selected < 0 || selected >= shapes.GetCount()) return;
        Shape& s = shapes[selected];

        auto flipx=[&](double nx, double cx){ return cx - (nx - cx); };
        auto flipy=[&](double ny, double cy){ return cy - (ny - cy); };

        if(s.type == Shape::Type::Rect) {
            double cx = s.x + s.w/2.0, cy = s.y + s.h/2.0;
            if(horizontal) s.x = flipx(s.x, cx);
            else           s.y = flipy(s.y, cy);
        }
        else if(s.type == Shape::Type::Circle) {
            // flip about centroid does nothing to a circle
        }
        else if(s.type == Shape::Type::Line) {
            double cx = (s.p1.x + s.p2.x)/2.0, cy = (s.p1.y + s.p2.y)/2.0;
            if(horizontal){ s.p1.x = flipx(s.p1.x,cx); s.p2.x = flipx(s.p2.x,cx); }
            else          { s.p1.y = flipy(s.p1.y,cy); s.p2.y = flipy(s.p2.y,cy); }
        }
        else { // triangle
            double cx = (s.p1.x + s.p2.x + s.p3.x)/3.0;
            double cy = (s.p1.y + s.p2.y + s.p3.y)/3.0;
            if(horizontal){
                s.p1.x = flipx(s.p1.x,cx); s.p2.x = flipx(s.p2.x,cx); s.p3.x = flipx(s.p3.x,cx);
            } else {
                s.p1.y = flipy(s.p1.y,cy); s.p2.y = flipy(s.p2.y,cy); s.p3.y = flipy(s.p3.y,cy);
            }
        }
        Refresh();
        if(WhenModelChanged) WhenModelChanged();
    }
};

// ===================== WrapPanel (flow layout) =====================
// A simple flow/wrap layout container for buttons. Children are laid out
// left‑>right and wrapped when width is insufficient.
struct WrapPanel : ParentCtrl {
    int row_h = 32;
    int hgap  = 6;
    int vgap  = 6;
    int left_pad = 8, top_pad = 5, right_pad = 8, bottom_pad = 5;
    int minw_each = 96;

    // cache
    mutable int  cached_width = -1;
    mutable int  cached_rows  = 1;
    mutable int  cached_height = 0;
    mutable int  cached_gen = -1;
           int  gen = 0; // bump when structure/params change

    WrapPanel& Row(int h) { row_h = h; gen++; RefreshLayout(); return *this; }
    WrapPanel& HGap(int g){ hgap  = g; gen++; RefreshLayout(); return *this; }
    WrapPanel& VGap(int g){ vgap  = g; gen++; RefreshLayout(); return *this; }
    WrapPanel& Padding(int l,int t,int r,int b){
        left_pad=l; top_pad=t; right_pad=r; bottom_pad=b; gen++; RefreshLayout(); return *this;
    }
    void ChildAdded(Ctrl*) override  { gen++; RefreshLayout(); }
    void ChildRemoved(Ctrl*) override{ gen++; RefreshLayout(); }

    // Single source of truth: compute rows for a given width
    void EnsureMeasured(int container_width) const {
        const int avail_w = max(0, container_width - left_pad - right_pad);
        if(container_width == cached_width && cached_gen == gen)
            return;

        int x = left_pad;
        int rows = 1;
        for(const Ctrl* q = GetFirstChild(); q; q = q->GetNext()) {
            const Size want = q->GetStdSize();
            const int w = max(minw_each, want.cx);
            if(x > left_pad && x + w > left_pad + avail_w) {
                rows++;
                x = left_pad;
            }
            x += w + hgap;
        }
        cached_width  = container_width;
        cached_rows   = max(1, rows);
        cached_height = top_pad + cached_rows * row_h + (cached_rows - 1) * vgap + bottom_pad;
        cached_gen    = gen;
    }

    // A sane minimum: one item wide, one row tall
    Size GetMinSize() const override {
        return Size(left_pad + minw_each + right_pad,
                    top_pad + row_h + bottom_pad);
    }

    // Ask this from parent to stack rows in a responsive way
    int MeasureHeight(int container_width) const {
        EnsureMeasured(container_width);
        return cached_height;
    }

    void Layout() override {
        const Size sz = GetSize();
        const int avail = max(0, sz.cx - left_pad - right_pad);

        int x = left_pad, y = top_pad;
        for(Ctrl* q = GetFirstChild(); q; q = q->GetNext()) {
            Ctrl& c = *q;
            const Size want = c.GetStdSize();
            const int w = max(minw_each, want.cx);
            const int h = max(row_h - 2, want.cy);

            if(x > left_pad && x + w > left_pad + avail) {
                x = left_pad;
                y += row_h + vgap;
            }
            c.LeftPos(x, w).TopPos(y, h);
            x += w + hgap;
        }
    }
};

// ===================== Main window =====================
struct MainWin : TopWindow {
    typedef MainWin CLASSNAME;

    // containers
    Splitter   hsplit;
    ParentCtrl left, right;

    // top rows
    StaticRect rowHead, rowPrims, rowOps,rowActs;
    Label      lblTool;

    // wrap containers for the three rows
    WrapPanel primsWrap;
    WrapPanel opsWrap;
    WrapPanel actWrap;

    // primitive buttons (owned members)
    Button bCur, bCirc, bLine, bTri, bRect;

    // operation buttons (owned members)
    Button bDelete, btnUndo, btnRedo, btnSaveDes, btnLoadDes;  // file/history ops

    // action buttons (owned members)
    Button bDup, bUp, bDown, bFlipH, bFlipV;

    // edit panel + canvas
    StaticRect panel;
    int        panel_pref_h = 0;
    Canvas     canvas;

    // right side: header + code
    StaticRect rightTop;
    Label      lFunc;
    EditString edFunc;
    Button     btnCopy, btnSaveCpp, btnClear;
    DocEdit    code;

    // style controls (on panel)
    Option       optFill, optStroke, optEvenOdd, optOutline, optClip, optSnap ;
    ColorPusher  fillClr, strokeClr, outlineClr;
    EditIntSpin  spWidth, spOutlineW;
    SliderCtrl   sOpacity;
    DropList     dlLine, dlDash;
    EditString   edDashCustom;

    // labels
    Label lbStrokeW, lbLine, lbOpacity, lbOutlineW, lbDash;

    // history (snapshots of JSON)
    Vector<String> hist;
    int            hipos = -1;

    ~MainWin() {
        canvas.WhenModelChanged.Clear();
        canvas.WhenSelectionChanged.Clear();
    }

    MainWin() {
        Title("U++ Procedural Icon Builder (WYSIWYG)").Sizeable().Zoomable();

        // ------- heading row -------
        lblTool.SetText("Tool: Cursor");
        rowHead.SetFrame(ThinInsetFrame());
        rowHead.Add(lblTool.HCenterPos(300).VCenterPos(22));

        // clip + snap toggles in header (left side)
        optClip.SetLabel("Clip");
        optClip <<= canvas.clip_to_inset;
        optSnap.SetLabel("Snap");
        optSnap <<= canvas.snap;
        optSnap.WhenAction = [=] {
            canvas.snap = (bool)~optSnap;
            canvas.Refresh();
        };
        rowHead.Add(optClip.LeftPos(6, 70).VCenterPos(22));
        rowHead.Add(optSnap.LeftPos(80, 70).VCenterPos(22));

        // ------- primitives row -------
        rowPrims.SetFrame(ThinInsetFrame());
        rowPrims.Add(primsWrap.SizePos()); // WrapPanel fills the row box
        primsWrap.Row(32).HGap(8).VGap(8).Padding(8, 4, 8, 4);

        // configure + add primitive buttons
        bCur.SetLabel("Cursor");     bCur.WhenAction = THISBACK(OnToolCursor);   primsWrap.Add(bCur);
        bCirc.SetLabel("Circle");    bCirc.WhenAction = THISBACK(OnToolCircle);  primsWrap.Add(bCirc);
        bLine.SetLabel("Line");      bLine.WhenAction = THISBACK(OnToolLine);    primsWrap.Add(bLine);
        bTri.SetLabel("Triangle");   bTri.WhenAction = THISBACK(OnToolTriangle); primsWrap.Add(bTri);
        bRect.SetLabel("Rectangle"); bRect.WhenAction = THISBACK(OnToolRect);    primsWrap.Add(bRect);

        // ------- operations row (file/history ops) -------
        rowOps.SetFrame(ThinInsetFrame());
        rowOps.Add(opsWrap.SizePos());
        opsWrap.Row(32).HGap(8).VGap(8).Padding(8, 4, 8, 4);

        bDelete.SetLabel("Delete");       bDelete.WhenAction = THISBACK(OnDelete);     opsWrap.Add(bDelete);
        btnUndo.SetLabel("Undo");         btnUndo.WhenAction = THISBACK(OnUndo);       opsWrap.Add(btnUndo);
        btnRedo.SetLabel("Redo");         btnRedo.WhenAction = THISBACK(OnRedo);       opsWrap.Add(btnRedo);
        btnSaveDes.SetLabel("Save Design"); btnSaveDes.WhenAction = THISBACK(SaveDesign); opsWrap.Add(btnSaveDes);
        btnLoadDes.SetLabel("Load Design"); btnLoadDes.WhenAction = THISBACK(LoadDesign); opsWrap.Add(btnLoadDes);

        // ------- actions row (shape ops) -------
        rowActs.SetFrame(ThinInsetFrame());
        rowActs.Add(actWrap.SizePos());
        actWrap.Row(32).HGap(8).VGap(8).Padding(8, 4, 8, 4);

        bDup.SetLabel("Duplicate"); bDup.WhenAction = THISBACK(OnDuplicate); actWrap.Add(bDup);
        bUp.SetLabel("Layer-Up");   bUp.WhenAction = THISBACK(OnLayerUp);   actWrap.Add(bUp);
        bDown.SetLabel("Layer-Down"); bDown.WhenAction = THISBACK(OnLayerDown); actWrap.Add(bDown);
        bFlipH.SetLabel("Flip H");  bFlipH.WhenAction = THISBACK(OnFlipH);  actWrap.Add(bFlipH);
        bFlipV.SetLabel("Flip V");  bFlipV.WhenAction = THISBACK(OnFlipV);  actWrap.Add(bFlipV);

        // ------- edit panel -------
        {
            int py = 6, lh = 22, pad = 6;

            // Fill
            optFill.SetLabel("Fill"); optFill <<= true;
            panel.Add(optFill.LeftPos(8, 80).TopPos(py, lh));
            fillClr.SetData(canvas.defaultStyle.fill);
            panel.Add(fillClr.LeftPos(96, 84).TopPos(py, lh));
            py += lh + pad;

            // Stroke + width
            optStroke.SetLabel("Stroke"); optStroke <<= true;
            panel.Add(optStroke.LeftPos(8, 80).TopPos(py, lh));
            strokeClr.SetData(canvas.defaultStyle.stroke);
            panel.Add(strokeClr.LeftPos(96, 84).TopPos(py, lh));
            lbStrokeW.SetText("Width");
            panel.Add(lbStrokeW.LeftPos(188, 56).TopPos(py, lh));
            spWidth.MinMax(1, 64); spWidth <<= 2;
            panel.Add(spWidth.LeftPos(246, 64).TopPos(py, lh));
            py += lh + pad;

            // Line style (preset)
            lbLine.SetText("Line");
            panel.Add(lbLine.LeftPos(8, 80).TopPos(py, lh));
            dlLine.Add("Solid"); dlLine.Add("Dashed"); dlLine.Add("Dotted"); dlLine <<= 0;
            panel.Add(dlLine.LeftPos(96, 84).TopPos(py, lh));
            py += lh + pad;

            // Even-odd + Opacity
            optEvenOdd.SetLabel("Even-odd"); optEvenOdd <<= false;
            panel.Add(optEvenOdd.LeftPos(8, 96).TopPos(py, lh));
            lbOpacity.SetText("Opacity");
            panel.Add(lbOpacity.LeftPos(112, 64).TopPos(py, lh));
            sOpacity.MinMax(0, 100); sOpacity <<= 100;
            panel.Add(sOpacity.LeftPos(178, 100).TopPos(py, lh));
            py += lh + pad;

            // Outline
            optOutline.SetLabel("Outline"); optOutline <<= false;
            panel.Add(optOutline.LeftPos(8, 80).TopPos(py, lh));
            outlineClr.SetData(Red());
            panel.Add(outlineClr.LeftPos(96, 84).TopPos(py, lh));
            lbOutlineW.SetText("Width");
            panel.Add(lbOutlineW.LeftPos(188, 56).TopPos(py, lh));
            spOutlineW.MinMax(0, 48); spOutlineW <<= 0;
            panel.Add(spOutlineW.LeftPos(246, 64).TopPos(py, lh));
            py += lh + pad;

            // Dash preset + custom
            lbDash.SetText("Dash");
            panel.Add(lbDash.LeftPos(8, 80).TopPos(py, lh));
            dlDash.Add("None"); dlDash.Add("5 5"); dlDash.Add("2 4"); dlDash.Add("Custom…"); dlDash <<= 0;
            panel.Add(dlDash.LeftPos(96, 84).TopPos(py, lh));
            edDashCustom.SetFilter(CharFilterAscii);
            edDashCustom.Hide();
            panel.Add(edDashCustom.LeftPos(186, 140).TopPos(py, lh));
            py += lh + pad;

            panel_pref_h = py + 8;
        }

        // ------- left layout (heights are set in Layout()) -------
        left.Add(rowHead.HSizePos());
        left.Add(rowPrims.HSizePos());
        left.Add(rowOps.HSizePos());
        left.Add(rowActs.HSizePos());
        left.Add(panel.HSizePos());
        left.Add(canvas.HSizePos());

        // ------- right header + code -------
        lFunc.SetText("Function:");
        rightTop.Add(lFunc.LeftPos(4, 70).TopPos(4, 24));
        edFunc.SetText("DrawMyIcon");
        rightTop.Add(edFunc.LeftPos(80, 200).TopPos(4, 24));
        btnCopy.SetLabel("Copy");
        btnSaveCpp.SetLabel("Save .cpp");
        btnClear.SetLabel("Clear");
        rightTop.Add(btnCopy   .RightPos(160, 60).TopPos(4, 24));
        rightTop.Add(btnSaveCpp.RightPos( 96, 60).TopPos(4, 24));
        rightTop.Add(btnClear  .RightPos( 32, 60).TopPos(4, 24));
        right.Add(rightTop.HSizePos().TopPos(0, 32));

        code.SetFont(CourierZ(12));
        code.SetReadOnly();
        right.Add(code.HSizePos().VSizePos(36, 0));

        // splitter
        hsplit.Horz(left, right);
        Add(hsplit.SizePos());

        // ------- wiring (THISBACK, no untracked captures) -------
        dlDash.WhenAction = THISBACK(OnDashChanged);
        optFill.WhenAction = THISBACK(OnStyleChanged);
        fillClr.WhenAction = THISBACK(OnStyleChanged);
        optStroke.WhenAction = THISBACK(OnStyleChanged);
        strokeClr.WhenAction = THISBACK(OnStyleChanged);
        spWidth.WhenAction = THISBACK(OnStyleChanged);
        dlLine.WhenAction = THISBACK(OnStyleChanged);
        optEvenOdd.WhenAction = THISBACK(OnStyleChanged);
        sOpacity.WhenAction = THISBACK(OnStyleChanged);
        optOutline.WhenAction = THISBACK(OnStyleChanged);
        outlineClr.WhenAction = THISBACK(OnStyleChanged);
        spOutlineW.WhenAction = THISBACK(OnStyleChanged);
        edDashCustom.WhenAction = THISBACK(OnStyleChanged);
        optClip.WhenAction = THISBACK(OnTogglesChanged);
        optSnap.WhenAction = THISBACK(OnTogglesChanged);

        btnCopy.WhenAction = THISBACK(OnCopyCode);
        btnSaveCpp.WhenAction = THISBACK(OnSaveCpp);
        btnClear.WhenAction = THISBACK(OnClearAll);

        canvas.WhenSelectionChanged = THISBACK(OnSelectionChanged);
        canvas.WhenModelChanged = THISBACK(ModelChanged);

        LoadUIFrom(canvas.defaultStyle);
        UpdateToolHeading();
        RefreshCode();
        PushHistory();
    }

    // ---------- layout ----------
    void Layout() override {
        TopWindow::Layout();

        // Current width of the left pane
        int left_w = left.GetSize().cx;
        if(left_w <= 0) return;

        // Fixed header height
        const int hHead = 28;

        // Each WrapPanel returns the height it needs at this width
        int hPrims = primsWrap.MeasureHeight(left_w);
        int hOps   = opsWrap  .MeasureHeight(left_w);
        int hActs  = actWrap  .MeasureHeight(left_w);

        int y = 0;
        rowHead.TopPos(y, hHead);      y += hHead;
        rowPrims.TopPos(y, hPrims);    y += hPrims;
        rowOps  .TopPos(y, hOps);      y += hOps;
        rowActs .TopPos(y, hActs);     y += hActs;

        // Style panel keeps its preferred height
        panel.TopPos(y, panel_pref_h); y += panel_pref_h + 12;

        // Canvas fills the rest
        canvas.VSizePos(y, 0);
    }

    // ---------- handlers ----------
    void OnToolCursor()   { canvas.tool = ::Tool::Cursor;   UpdateToolHeading(); }
    void OnToolCircle()   { canvas.tool = ::Tool::Circle;   UpdateToolHeading(); }
    void OnToolLine()     { canvas.tool = ::Tool::Line;     UpdateToolHeading(); }
    void OnToolTriangle() { canvas.tool = ::Tool::Triangle; UpdateToolHeading(); }
    void OnToolRect()     { canvas.tool = ::Tool::Rect;     UpdateToolHeading(); }

    void OnDelete()    { canvas.DeleteSelection(); RefreshCode(); }
    void OnDuplicate() { canvas.DuplicateSelection(); RefreshCode(); }
    void OnLayerUp()   { canvas.LayerUp();           RefreshCode(); }
    void OnLayerDown() { canvas.LayerDown();         RefreshCode(); }
    void OnFlipH()     { canvas.FlipSelection(true);  RefreshCode(); }
    void OnFlipV()     { canvas.FlipSelection(false); RefreshCode(); }
    void OnUndo()      { Undo(); RefreshCode(); }
    void OnRedo()      { Redo(); RefreshCode(); }

    void OnTogglesChanged() {
        canvas.clip_to_inset = (bool)~optClip;
        canvas.snap          = (bool)~optSnap;
        canvas.Refresh();
        RefreshCode();
    }

    void OnDashChanged() {
        edDashCustom.Show(dlDash.GetIndex() == 3);
        ApplyStyleToSelectionOrDefaults(false);
    }
    void OnStyleChanged() {
        ApplyStyleToSelectionOrDefaults(false);
    }
    void OnCopyCode() { WriteClipboardText(code.Get()); }
    void OnSaveCpp() {
        FileSel fs; fs.Type("C++ file","*.cpp");
        if(fs.ExecuteSaveAs("Save generated .cpp"))
            SaveFile(fs.Get(), code.Get());
    }
    void OnClearAll() {
        canvas.shapes.Clear();
        canvas.selected = -1;
        canvas.Refresh();
        RefreshCode();
        PushHistory();
        OnSelectionChanged();
    }

    void OnSelectionChanged() {
        UpdateToolHeading();
        if(canvas.selected >= 0 && canvas.selected < canvas.shapes.GetCount())
            LoadUIFrom(canvas.shapes[canvas.selected].style);
        else
            LoadUIFrom(canvas.defaultStyle);
        RefreshCode();
    }

    void ModelChanged() {
        RefreshCode();
        PushHistory();
    }

    void UpdateToolHeading() {
        String t = "Tool: ";
        switch(canvas.tool){
            case Tool::Cursor:   t << "Cursor"; break;
            case Tool::Rect:     t << "Rectangle"; break;
            case Tool::Circle:   t << "Circle"; break;
            case Tool::Line:     t << "Line"; break;
            case Tool::Triangle: t << "Triangle"; break;
        }
        if(canvas.tool == Tool::Cursor && canvas.selected >= 0 && canvas.selected < canvas.shapes.GetCount()){
            const Shape& s = canvas.shapes[canvas.selected];
            t << "  |  Editing "
              << (s.type == Shape::Type::Rect ? "Rectangle" :
                  s.type == Shape::Type::Circle ? "Circle" :
                  s.type == Shape::Type::Line ? "Line" : "Triangle");
        }
        lblTool.SetText(t);
    }

    // Read current UI widgets into a Style and apply to selection or defaults
    void ApplyStyleToSelectionOrDefaults(bool push_hist) {
        ::Style st = canvas.defaultStyle;

        st.enableFill   = (bool)~optFill;
        st.fill         = (Color)fillClr.GetData();

        st.enableStroke = (bool)~optStroke;
        st.stroke       = (Color)strokeClr.GetData();
        st.strokeWidth  = (int)~spWidth;

        st.evenOdd      = (bool)~optEvenOdd;
        st.opacity      = (int)~sOpacity / 100.0;

        switch(dlDash.GetIndex()){
            case 0: st.dash.Clear();         break;
            case 1: st.dash = "5 5";         break;
            case 2: st.dash = "2 4";         break;
            case 3: st.dash = ~edDashCustom; break;
        }

        st.outlineEnable  = (bool)~optOutline;
        st.outlineColor   = (Color)outlineClr.GetData();
        st.outlineWidth   = (int)~spOutlineW;
        st.outlineOutside = true;

        ::Style* dst = (canvas.selected >= 0 && canvas.selected < canvas.shapes.GetCount())
                         ? &canvas.shapes[canvas.selected].style
                         : &canvas.defaultStyle;

        *dst = st;
        canvas.Refresh();
        RefreshCode();
        if(push_hist && canvas.WhenModelChanged) canvas.WhenModelChanged();
    }

    // Push a Style into the UI widgets
    void LoadUIFrom(const ::Style& st) {
        optFill    <<= st.enableFill;
        fillClr    .SetData(st.fill);

        optStroke  <<= st.enableStroke;
        strokeClr  .SetData(st.stroke);
        spWidth    <<= st.strokeWidth;

        optEvenOdd <<= st.evenOdd;
        sOpacity   <<= int(st.opacity * 100);

        if(st.dash.IsEmpty())      dlDash <<= 0;
        else if(st.dash == "5 5")  dlDash <<= 1;
        else if(st.dash == "2 4")  dlDash <<= 2;
        else { dlDash <<= 3; edDashCustom.SetText(st.dash); }
        edDashCustom.Show(dlDash.GetIndex() == 3);

        optOutline <<= st.outlineEnable && st.outlineWidth > 0;
        outlineClr .SetData(st.outlineColor);
        spOutlineW <<= max(st.outlineWidth, 0);
    }

    // Regenerate painter code for the right pane (pure string builder)
    void RefreshCode() {
        String out;
        const String func = Nvl(edFunc.GetText(), "DrawMyIcon");

        out << "static void " << func << "(BufferPainter& p, const Rect& inset)\n{\n"
            << "    auto X =[&](double nx){ return inset.left + int(inset.Width()  * nx + 0.5); };\n"
            << "    auto Y =[&](double ny){ return inset.top  + int(inset.Height() * ny + 0.5); };\n"
            << "    auto Rr=[&](double nr){ return int(min(inset.Width(), inset.Height()) * nr + 0.5); };\n\n";

        auto emit_rect = [&](double x,double y,double w,double h){
            out << "    // Rectangle\n"
                << "    p.Begin();\n"
                << Format("    p.Move(Pointf(X(%g), Y(%g)));\n", x, y)
                << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", x+w, y)
                << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", x+w, y+h)
                << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", x,   y+h)
                << "    p.Close();\n";
        };
        auto emit_circle = [&](double cx,double cy,double r){
            out << "    // Circle\n"
                << "    p.Begin();\n"
                << Format("    p.Circle(X(%g), Y(%g), Rr(%g));\n", cx, cy, r);
        };
        auto emit_line = [&](double x1,double y1,double x2,double y2){
            out << "    // Line\n"
                << "    p.Begin();\n"
                << Format("    p.Move(Pointf(X(%g), Y(%g)));\n", x1, y1)
                << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", x2, y2);
        };
        auto emit_tri = [&](Pointf a, Pointf b, Pointf c){
            out << "    // Triangle\n"
                << "    p.Begin();\n"
                << Format("    p.Move(Pointf(X(%g), Y(%g)));\n", a.x, a.y)
                << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", b.x, b.y)
                << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", c.x, c.y)
                << "    p.Close();\n";
        };
        auto emit_style = [&](const ::Style& st, bool main_pass, bool is_line){
            if(main_pass){
                if(st.opacity < 1.0)
                    out << Format("    p.Opacity(%g);\n", st.opacity);
                if(!st.dash.IsEmpty())
                    out << Format("    p.Dash(String(\"%s\"), 0.0);\n", st.dash.Begin());
                if(st.enableFill && !is_line)
                    out << Format("    p.Fill(Color(%d,%d,%d));\n", st.fill.GetR(), st.fill.GetG(), st.fill.GetB());
                if(st.enableStroke)
                    out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n",
                                  st.strokeWidth, st.stroke.GetR(), st.stroke.GetG(), st.stroke.GetB());
                if(st.evenOdd)
                    out << "    p.EvenOdd(true);\n";
            } else {
                if(st.outlineEnable && st.outlineWidth > 0)
                    out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n",
                                  st.strokeWidth + 2 * max(0, st.outlineWidth),
                                  st.outlineColor.GetR(), st.outlineColor.GetG(), st.outlineColor.GetB());
            }
            out << "    p.End();\n\n";
        };

        for(const Shape& s : canvas.shapes) {
            const bool is_line = (s.type == Shape::Type::Line);

            if(s.style.outlineEnable && s.style.outlineWidth > 0 && s.style.outlineOutside){
                switch(s.type){
                    case Shape::Type::Rect:     emit_rect(s.x,s.y,s.w,s.h);   break;
                    case Shape::Type::Circle:   emit_circle(s.cx,s.cy,s.r);    break;
                    case Shape::Type::Line:     emit_line(s.p1.x,s.p1.y,s.p2.x,s.p2.y); break;
                    case Shape::Type::Triangle: emit_tri(s.p1,s.p2,s.p3);      break;
                }
                emit_style(s.style, false, is_line);
            }

            switch(s.type){
                case Shape::Type::Rect:     emit_rect(s.x,s.y,s.w,s.h);   break;
                case Shape::Type::Circle:   emit_circle(s.cx,s.cy,s.r);    break;
                case Shape::Type::Line:     emit_line(s.p1.x,s.p1.y,s.p2.x,s.p2.y); break;
                case Shape::Type::Triangle: emit_tri(s.p1,s.p2,s.p3);      break;
            }
            emit_style(s.style, true, is_line);

            if(s.style.outlineEnable && s.style.outlineWidth > 0 && !s.style.outlineOutside){
                switch(s.type){
                    case Shape::Type::Rect:     emit_rect(s.x,s.y,s.w,s.h);   break;
                    case Shape::Type::Circle:   emit_circle(s.cx,s.cy,s.r);    break;
                    case Shape::Type::Line:     emit_line(s.p1.x,s.p1.y,s.p2.x,s.p2.y); break;
                    case Shape::Type::Triangle: emit_tri(s.p1,s.p2,s.p3);      break;
                }
                emit_style(s.style, false, is_line);
            }
        }

        out << "}\n";
        code.Set(out);
    }

    // ---------- history & file ops ----------
    void PushHistory() {
        String snap = MakeJson();
        if(hipos+1 < hist.GetCount())
            hist.Remove(hipos+1, hist.GetCount() - (hipos+1));
        hist.Add(snap);
        hipos = hist.GetCount()-1;
    }
    void Undo() { if(hipos > 0)            { hipos--; LoadJson(hist[hipos]); } }
    void Redo() { if(hipos+1 < hist.GetCount()) { hipos++; LoadJson(hist[hipos]); } }

    void SaveDesign() {
        FileSel fs; fs.Type("Design JSON","*.json");
        if(fs.ExecuteSaveAs("Save design")) SaveFile(fs.Get(), MakeJson());
    }
    void LoadDesign() {
        FileSel fs; fs.Type("Design JSON","*.json");
        if(!fs.ExecuteOpen("Load design")) return;
        String s = LoadFile(fs.Get());
        if(!LoadJson(s))
            PromptOK("Invalid or incompatible design file.");
        else
            PushHistory();
    }

    // ---------- JSON (round‑trippable; stable for history) ----------
    static String ColorToString(Color c) { return Format("#%02X%02X%02X", c.GetR(), c.GetG(), c.GetB()); }
    static Color  StringToColor(const String& s) {
        if(s.GetCount()==7 && s[0]=='#') {
            int n = (int)strtoul(~s + 1, nullptr, 16);
            return Color((n>>16)&255, (n>>8)&255, n&255);
        }
        return Black();
    }

    String MakeJson() {
        ValueMap root;
        root("function") = edFunc.GetText();

        ValueMap ins;
        ins("x") = canvas.inset.x; ins("y") = canvas.inset.y;
        ins("w") = canvas.inset.w; ins("h") = canvas.inset.h;
        root("inset") = ins;

        ValueArray arr;
        for(const Shape& s : canvas.shapes){
            ValueMap m;
            m("type") = (s.type == Shape::Type::Rect ? "rect" :
                         s.type == Shape::Type::Circle ? "circle" :
                         s.type == Shape::Type::Line ? "line" : "triangle");

            ValueMap st;
            st("fill")           = ColorToString(s.style.fill);
            st("stroke")         = ColorToString(s.style.stroke);
            st("strokeWidth")    = s.style.strokeWidth;
            st("dash")           = s.style.dash;
            st("enableFill")     = s.style.enableFill;
            st("enableStroke")   = s.style.enableStroke;
            st("opacity")        = s.style.opacity;
            st("evenOdd")        = s.style.evenOdd;
            st("outlineEnable")  = s.style.outlineEnable;
            st("outlineColor")   = ColorToString(s.style.outlineColor);
            st("outlineWidth")   = s.style.outlineWidth;
            st("outlineOutside") = s.style.outlineOutside;
            m("style") = st;

            ValueMap g;
            g("x")=s.x; g("y")=s.y; g("w")=s.w; g("h")=s.h;
            g("cx")=s.cx; g("cy")=s.cy; g("r")=s.r;
            ValueMap p1, p2, p3;
            p1("x")=s.p1.x; p1("y")=s.p1.y;
            p2("x")=s.p2.x; p2("y")=s.p2.y;
            p3("x")=s.p3.x; p3("y")=s.p3.y;
            g("p1")=p1; g("p2")=p2; g("p3")=p3;
            m("geom") = g;

            arr.Add(m);
        }
        root("shapes") = arr;

        return AsJSON(root, true);
    }

    bool LoadJson(const String& s) {
        Value v = ParseJSON(s);
        if(IsError(v)) return false;
        const ValueMap& root = v;
        if(root.Find("inset") < 0 || root.Find("shapes") < 0) return false;

        if(root.Find("function") >= 0)
            edFunc.SetText(root["function"].ToString());

        const ValueMap& ins = root["inset"];
        canvas.inset.x = (int)ins["x"]; canvas.inset.y = (int)ins["y"];
        canvas.inset.w = (int)ins["w"]; canvas.inset.h = (int)ins["h"];

        canvas.shapes.Clear();
        const ValueArray& arr = root["shapes"];
        for(const Value& it : arr){
            const ValueMap& m = it;
            Shape s;
            String t = m["type"].ToString();
            s.type = t=="rect" ? Shape::Type::Rect :
                    t=="circle" ? Shape::Type::Circle :
                    t=="line" ? Shape::Type::Line : Shape::Type::Triangle;

            const ValueMap& st = m["style"];
            s.style.fill            = StringToColor(st["fill"].ToString());
            s.style.stroke          = StringToColor(st["stroke"].ToString());
            s.style.strokeWidth     = (int)st["strokeWidth"];
            s.style.dash            = st["dash"].ToString();
            s.style.enableFill      = (bool)st["enableFill"];
            s.style.enableStroke    = (bool)st["enableStroke"];
            s.style.opacity         = st["opacity"].To<double>();
            s.style.evenOdd         = (bool)st["evenOdd"];
            s.style.outlineEnable   = (bool)st["outlineEnable"];
            s.style.outlineColor    = StringToColor(st["outlineColor"].ToString());
            s.style.outlineWidth    = (int)st["outlineWidth"];
            s.style.outlineOutside  = (bool)st["outlineOutside"];

            const ValueMap& g = m["geom"];
            s.x = g["x"].To<double>(); s.y = g["y"].To<double>();
            s.w = g["w"].To<double>(); s.h = g["h"].To<double>();
            s.cx= g["cx"].To<double>(); s.cy= g["cy"].To<double>();
            s.r = g["r"].To<double>();

            auto rdpt=[&](const Value& vp)->Pointf{
                const ValueMap& pm = vp;
                return Pointf(pm["x"].To<double>(), pm["y"].To<double>());
            };
            if(g.Find("p1")>=0) s.p1 = rdpt(g["p1"]);
            if(g.Find("p2")>=0) s.p2 = rdpt(g["p2"]);
            if(g.Find("p3")>=0) s.p3 = rdpt(g["p3"]);

            s.id = canvas.shapes.IsEmpty() ? 1 : canvas.shapes.Top().id + 1;
            canvas.shapes.Add(s);
        }
        canvas.Refresh();
        RefreshCode();
        UpdateToolHeading();
        return true;
    }
};

// ===================== Entry point =====================
GUI_APP_MAIN {
    SetLanguage(GetSystemLNG());
    MainWin win;
    win.Run();                // window and all children die at the end of this scope
}
