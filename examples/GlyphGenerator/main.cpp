#include <CtrlLib/CtrlLib.h>
#include <Painter/Painter.h>
#include <Draw/Draw.h>

using namespace Upp;

// ──────────────────────────────────────────────────────────────────────────────
// Model
// ──────────────────────────────────────────────────────────────────────────────
enum class Tool { Cursor, Rect, Circle, Line, Triangle };

struct Style : Moveable<Style> {
    Color  fill   = Color(163,201,168);
    Color  stroke = Color(30,53,47);
    int    strokeWidth = 2;
    String dash;            // e.g. "5 3" (empty => solid)
    bool   enableFill  = true;
    bool   enableStroke = true;
    double opacity = 1.0;   // 0..1
    bool   evenOdd = false; // (not used by Circle)
    Color  outlineColor = Red();
    int    outlineOffset = 0; // 0 disables outline
};

struct Inset { int x=50, y=50, w=400, h=300; };

struct Shape : Moveable<Shape> {
    enum class Type { Rect, Circle, Line, Triangle } type;
    Style  style;

    // normalized geometry (0..1)
    double x=0,y=0,w=0,h=0;     // Rect
    double cx=0,cy=0,r=0;       // Circle (r relative to min(w,h))
    Pointf p1, p2, p3;          // Line/Triangle
    int    id=0;
};

static inline int X(const Rect& r, double nx) { return r.left + int(r.Width()*nx + 0.5); }
static inline int Y(const Rect& r, double ny) { return r.top  + int(r.Height()*ny + 0.5); }
static inline int R(const Rect& r, double nr) { return int(min(r.Width(), r.Height())*nr + 0.5); }

// ──────────────────────────────────────────────────────────────────────────────
// “Emitter” helpers (for BufferPainter + generator)
// ──────────────────────────────────────────────────────────────────────────────
struct Emitter {
    virtual void Begin() = 0;
    virtual void End() = 0;
    virtual void Move(Pointf p) = 0;
    virtual void Line(Pointf p) = 0;
    virtual void Close() = 0;
    virtual void Fill(Color c) = 0;
    virtual void Stroke(double w, Color c) = 0;
    virtual void Dash(const String& spec, double phase) = 0;
    virtual void Opacity(double o) = 0;
    virtual void Clip() = 0;
    virtual ~Emitter() {}
};

struct PainterEmitter : Emitter {
    BufferPainter& p;
    PainterEmitter(BufferPainter& p) : p(p) {}
    void Begin() override { p.Begin(); }
    void End() override   { p.End(); }
    void Move(Pointf a) override { p.Move(a); }
    void Line(Pointf a) override { p.Line(a); }
    void Close() override { p.Close(); }
    void Fill(Color c) override { p.Fill(c); }
    void Stroke(double w, Color c) override { p.Stroke(w, c); }
    void Dash(const String& spec, double phase) override { if(!spec.IsEmpty()) p.Dash(spec, phase); }
    void Opacity(double o) override { if(o < 1.0) p.Opacity(o); }
    void Clip() override { p.Clip(); }
};

struct CodeEmitter : Emitter {
    String& out;
    CodeEmitter(String& s) : out(s) {}
    void Put(const String& s){ out << s; }
    void Begin() override { Put("    p.Begin();\n"); }
    void End() override   { Put("    p.End();\n"); }
    void Move(Pointf a) override { Put(Format("    p.Move(Pointf(X(%g), Y(%g)));\n", a.x, a.y)); }
    void Line(Pointf a) override { Put(Format("    p.Line(Pointf(X(%g), Y(%g)));\n", a.x, a.y)); }
    void Close() override { Put("    p.Close();\n"); }
    void Fill(Color c) override { Put(Format("    p.Fill(Color(%d,%d,%d));\n", c.GetR(), c.GetG(), c.GetB())); }
    void Stroke(double w, Color c) override { Put(Format("    p.Stroke(%g, Color(%d,%d,%d));\n", w, c.GetR(), c.GetG(), c.GetB())); }
    void Dash(const String& spec, double phase) override { if(!spec.IsEmpty()) Put(Format("    p.Dash(String(\"%s\"), %g);\n", spec.Begin(), phase)); }
    void Opacity(double o) override { if(o < 1.0) Put(Format("    p.Opacity(%g);\n", o)); }
    void Clip() override { Put("    p.Clip();\n"); }
};

// Path helpers for non-circle shapes
static void EmitRectPath(Emitter& e, const Rect& r){
    e.Begin();
    e.Move(Pointf(r.left, r.top));
    e.Line(Pointf(r.right, r.top));
    e.Line(Pointf(r.right, r.bottom));
    e.Line(Pointf(r.left, r.bottom));
    e.Close();
}

static void StylePass(Emitter& e, const Style& st, bool outline_pass, bool is_line){
    if(!outline_pass) {
        if(st.opacity < 1.0) e.Opacity(st.opacity);
        if(!st.dash.IsEmpty()) e.Dash(st.dash, 0.0);
    }
    if(outline_pass) {
        int ow = st.strokeWidth + 2*max(0, st.outlineOffset);
        e.Stroke(ow, st.outlineColor);
    } else {
        if(st.enableFill && !is_line) e.Fill(st.fill);
        if(st.enableStroke)          e.Stroke(st.strokeWidth, st.stroke);
    }
}

// Emit a shape (BufferPainter-backed) — circles use p.Circle now
static void EmitShape(Emitter& e, const Shape& s, const Rect& inset, bool clip_to_inset, BufferPainter* bp = nullptr) {
    if(clip_to_inset){
        EmitRectPath(e, inset);
        e.Clip();
    }

    switch(s.type){
    case Shape::Type::Rect: {
        Rect r(X(inset, s.x), Y(inset, s.y),
               X(inset, s.x + s.w), Y(inset, s.y + s.h));
        EmitRectPath(e, r);
        break;
    }
    case Shape::Type::Circle: {
        // Circles are emitted directly to BufferPainter (Emitter has no Circle())
        // So if we have a BufferPainter, use p.Circle; otherwise build a path rectangular proxy.
        if(bp) {
            bp->Begin();
            bp->Circle(X(inset, s.cx), Y(inset, s.cy), R(inset, s.r));
            // style
            if(s.style.opacity < 1.0) bp->Opacity(s.style.opacity);
            if(!s.style.dash.IsEmpty()) bp->Dash(s.style.dash, 0.0);
            if(s.style.enableFill)  bp->Fill(s.style.fill);
            if(s.style.enableStroke)bp->Stroke(s.style.strokeWidth, s.style.stroke);
            bp->End();

            if(s.style.outlineOffset > 0) {
                bp->Begin();
                bp->Circle(X(inset, s.cx), Y(inset, s.cy), R(inset, s.r));
                bp->Stroke(s.style.strokeWidth + 2*max(0, s.style.outlineOffset), s.style.outlineColor);
                bp->End();
            }
            return;
        } else {
            // CodeEmitter path is handled later in generator; here just fall through with a rectangle box so we keep API uniform.
            Rect r(X(inset, s.cx)-R(inset,s.r), Y(inset,s.cy)-R(inset,s.r), 2*R(inset,s.r), 2*R(inset,s.r));
            EmitRectPath(e, r);
        }
        break;
    }
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

    if(s.style.outlineOffset > 0){
        StylePass(e, s.style, true, s.type == Shape::Type::Line);
        e.End();
        // path again for normal style
        switch(s.type){
        case Shape::Type::Rect: {
            Rect r(X(inset, s.x), Y(inset, s.y),
                   X(inset, s.x + s.w), Y(inset, s.y + s.h));
            EmitRectPath(e, r);
            break;
        }
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
        default: break; // circle was early-returned when bp!=nullptr
        }
    }
    StylePass(e, s.style, false, s.type == Shape::Type::Line);
    e.End();
}

// ──────────────────────────────────────────────────────────────────────────────
struct Canvas : public Ctrl {
    typedef Canvas CLASSNAME;

    Vector<Shape> shapes;
    Style  defaultStyle;
    Inset  inset;
    Tool   tool = Tool::Cursor;
    bool   clip_to_inset = false;
    bool   snap = true;
    int    grid = 20;

    // editing
    bool   drawing = false;
    int    selected = -1;
    Point  start;

    enum DragMode { DRAG_NONE, DRAG_MOVE, DRAG_P1, DRAG_P2, DRAG_P3, DRAG_RADIUS, DRAG_RECT_BR } drag = DRAG_NONE;

    Callback WhenModelChanged;

    Canvas() {
       NoTransparent(); 
       WantFocus();        // ask U++ to give us focus on click/tab
      SetFrame(NullFrame()); 
    }
    
   
    Rect GetInsetRect() const { return RectC(inset.x, inset.y, inset.w, inset.h); }

	// --- selection ops ---
	bool HasSelection() const { return selected >= 0 && selected < shapes.GetCount(); }
	
	void DeleteSelection() {
	    if(!HasSelection()) return;
	    shapes.Remove(selected);
	    if(selected >= shapes.GetCount()) selected = shapes.GetCount() - 1;
	    Refresh();
	    if(WhenModelChanged) WhenModelChanged();
	}
	
	void LayerUp() {
	    if(!HasSelection() || selected >= shapes.GetCount() - 1) return;
	    Swap(shapes[selected], shapes[selected + 1]);
	    selected++;
	    Refresh();
	    if(WhenModelChanged) WhenModelChanged();
	}
	
	void LayerDown() {
	    if(!HasSelection() || selected <= 0) return;
	    Swap(shapes[selected], shapes[selected - 1]);
	    selected--;
	    Refresh();
	    if(WhenModelChanged) WhenModelChanged();
	}
	
	void DuplicateSelection() {
	    if(!HasSelection()) return;
	    Shape s = shapes[selected];
	    s.id = shapes.IsEmpty() ? 1 : shapes.Top().id + 1;
	    shapes.Insert(selected + 1, s);
	    selected++;
	    Refresh();
	    if(WhenModelChanged) WhenModelChanged();
	}
	

	// --- keyboard shortcuts ---
	bool Key(dword key, int) override {
	    switch(key) {
	    case K_DELETE:     DeleteSelection();       return true;
	    case K_CTRL_UP:    LayerUp();               return true;
	    case K_CTRL_DOWN:  LayerDown();             return true;
	    case K_CTRL_D:     DuplicateSelection();    return true;
	    default:           return false;
	    }
	}

    void Paint(Draw& w) override {
        Size sz = GetSize();
        w.DrawRect(sz, SColorFace());

        Rect ir = GetInsetRect();
        w.DrawRect(ir, White());

        // grid
        for(int x = ir.left + grid; x < ir.right; x += grid)
            w.DrawLine(x, ir.top, x, ir.bottom, 1, SColorPaper());
        for(int y = ir.top + grid; y < ir.bottom; y += grid)
            w.DrawLine(ir.left, y, ir.right, y, 1, SColorPaper());

        // inset dotted frame
        for(int i = ir.left; i < ir.right; i += 6)
            w.DrawLine(i, ir.top, min(i+3, ir.right), ir.top, 1, SColorHighlight());
        for(int i = ir.left; i < ir.right; i += 6)
            w.DrawLine(i, ir.bottom, min(i+3, ir.right), ir.bottom, 1, SColorHighlight());
        for(int i = ir.top; i < ir.bottom; i += 6)
            w.DrawLine(ir.left, i, ir.left, min(i+3, ir.bottom), 1, SColorHighlight());
        for(int i = ir.top; i < ir.bottom; i += 6)
            w.DrawLine(ir.right, i, ir.right, min(i+3, ir.bottom), 1, SColorHighlight());

        // preview with BufferPainter
        ImageBuffer ib(sz);
        BufferPainter p(ib);
        p.Clear(RGBAZero());

        for(const Shape& s : shapes) {
            PainterEmitter pe(p);
            EmitShape(pe, s, ir, clip_to_inset, &p); // pass &p so circle uses p.Circle
        }
        w.DrawImage(0, 0, Image(ib));

        // selection box + edit handles
        if(selected >= 0 && selected < shapes.GetCount()){
            const Shape& s = shapes[selected];
            Color sel = SColorMark();
            auto Box = [&](Rect r) {
                w.DrawRect(RectC(r.left, r.top, r.Width(), 1), sel);
                w.DrawRect(RectC(r.left, r.bottom, r.Width(), 1), sel);
                w.DrawRect(RectC(r.left, r.top, 1, r.Height()), sel);
                w.DrawRect(RectC(r.right, r.top, 1, r.Height()+1), sel);
            };
            auto Handle = [&](Point p){
                w.DrawRect(RectC(p.x-3, p.y-3, 7, 7), sel);
            };

            switch(s.type){
            case Shape::Type::Rect: {
                Rect r(X(ir, s.x), Y(ir, s.y), X(ir, s.x+s.w), Y(ir, s.y+s.h));
                Box(r);
                Handle(Point(r.right, r.bottom)); // simple BR handle for resize
                break;
            }
            case Shape::Type::Circle: {
                int cx = X(ir, s.cx), cy = Y(ir, s.cy), rr = R(ir, s.r);
                Box(RectC(cx-rr, cy-rr, 2*rr, 2*rr));
                Handle(Point(cx, cy));
                Handle(Point(cx+rr, cy)); // radius handle on the right
                break;
            }
            case Shape::Type::Line: {
                Point a(X(ir,s.p1.x), Y(ir,s.p1.y));
                Point b(X(ir,s.p2.x), Y(ir,s.p2.y));
                w.DrawLine(a, b, 1, sel);
                Handle(a); Handle(b);
                break;
            }
            case Shape::Type::Triangle: {
                Point a(X(ir,s.p1.x), Y(ir,s.p1.y));
                Point b(X(ir,s.p2.x), Y(ir,s.p2.y));
                Point c(X(ir,s.p3.x), Y(ir,s.p3.y));
                w.DrawLine(a, b, 1, sel);
                w.DrawLine(b, c, 1, sel);
                w.DrawLine(c, a, 1, sel);
                Handle(a); Handle(b); Handle(c);
                break;
            }}
        }
    }

    // hit helpers
    static bool Near(Point a, Point b, int tol=6) {
        int dx = a.x - b.x, dy = a.y - b.y;
        return dx*dx + dy*dy <= tol*tol;
    }
    static bool Inside(Rect r, Point p) { return r.Contains(p); }

    void LeftDown(Point p, dword) override {
        SetFocus();         // make sure we get Key() next
        SetCapture();
        
        start = p;
        Rect ir = GetInsetRect();

        if(tool == Tool::Cursor) {
            // select topmost shape under cursor; also detect handle
            selected = -1; drag = DRAG_NONE;
            for(int i = shapes.GetCount()-1; i >= 0; --i) {
                Shape& s = shapes[i];
                switch(s.type){
                case Shape::Type::Rect: {
                    Rect r(X(ir, s.x), Y(ir, s.y), X(ir, s.x+s.w), Y(ir, s.y+s.h));
                    if(Near(p, Point(r.right, r.bottom))) { selected=i; drag=DRAG_RECT_BR; break; }
                    if(Inside(r, p)) { selected=i; drag=DRAG_MOVE; }
                } break;
                case Shape::Type::Circle: {
                    int cx = X(ir, s.cx), cy = Y(ir, s.cy), rr = R(ir, s.r);
                    Rect bb = RectC(cx-rr, cy-rr, 2*rr, 2*rr);
                    if(Near(p, Point(cx+rr, cy))) { selected=i; drag=DRAG_RADIUS; break; }
                    if(Near(p, Point(cx, cy)) || Inside(bb, p)) { selected=i; drag=DRAG_MOVE; }
                } break;
                case Shape::Type::Line: {
                    Point a(X(ir,s.p1.x), Y(ir,s.p1.y));
                    Point b(X(ir,s.p2.x), Y(ir,s.p2.y));
                    if(Near(p,a)) { selected=i; drag=DRAG_P1; break; }
                    if(Near(p,b)) { selected=i; drag=DRAG_P2; break; }
                    // else approximate hit by bounding-box move
                    if(Inside(Rect(min(a.x,b.x), min(a.y,b.y), max(a.x,b.x), max(a.y,b.y)).Inflated(3), p)) { selected=i; drag=DRAG_MOVE; }
                } break;
                case Shape::Type::Triangle: {
                    Point a(X(ir,s.p1.x), Y(ir,s.p1.y));
                    Point b(X(ir,s.p2.x), Y(ir,s.p2.y));
                    Point c(X(ir,s.p3.x), Y(ir,s.p3.y));
                    if(Near(p,a)) { selected=i; drag=DRAG_P1; break; }
                    if(Near(p,b)) { selected=i; drag=DRAG_P2; break; }
                    if(Near(p,c)) { selected=i; drag=DRAG_P3; break; }
                    Rect bb = Rect(min(a.x,b.x,c.x), min(a.y,b.y,c.y), max(a.x,b.x,c.x), max(a.y,b.y,c.y));
                    if(Inside(bb, p)) { selected=i; drag=DRAG_MOVE; }
                } break;
                }
                if(selected>=0) break;
            }
            drawing = (selected>=0);
            Refresh();
            return;
        }

        // drawing new primitives
        if(!(tool == Tool::Rect || tool == Tool::Circle || tool == Tool::Line || tool == Tool::Triangle)) return;
        if(!ir.Contains(p)) return;

        Shape s;
        s.id = shapes.IsEmpty() ? 1 : shapes.Top().id + 1;
        s.style = defaultStyle;

        switch(tool){
        case Tool::Rect:
            s.type = Shape::Type::Rect;
            s.x = double(p.x - ir.left)/ir.Width();
            s.y = double(p.y - ir.top)/ir.Height();
            s.w = s.h = 0.0;
            break;
        case Tool::Circle:
            s.type = Shape::Type::Circle;
            s.cx = double(p.x - ir.left)/ir.Width();
            s.cy = double(p.y - ir.top)/ir.Height();
            s.r = 0.0;
            break;
        case Tool::Line:
            s.type = Shape::Type::Line;
            s.p1 = Pointf(double(p.x - ir.left)/ir.Width(), double(p.y - ir.top)/ir.Height());
            s.p2 = s.p1;
            break;
        case Tool::Triangle:
            s.type = Shape::Type::Triangle;
            s.p1 = Pointf(double(p.x - ir.left)/ir.Width(), double(p.y - ir.top)/ir.Height());
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
        Point q = p;
        if(snap) {
            q.x = ir.left + ((q.x - ir.left + grid/2) / grid) * grid;
            q.y = ir.top  + ((q.y - ir.top  + grid/2) / grid) * grid;
        }

        if(tool == Tool::Cursor) {
            if(!drawing || selected<0 || selected>=shapes.GetCount()) return;
            Shape& s = shapes[selected];

            // delta in normalized coords
            double dx = double(q.x - start.x) / ir.Width();
            double dy = double(q.y - start.y) / ir.Height();

            switch(s.type){
            case Shape::Type::Rect:
                if(drag==DRAG_MOVE){ s.x += dx; s.y += dy; }
                else if(drag==DRAG_RECT_BR){ s.w += dx; s.h += dy; }
                break;
            case Shape::Type::Circle:
                if(drag==DRAG_MOVE){ s.cx += dx; s.cy += dy; }
                else if(drag==DRAG_RADIUS){
                    double nx = double(q.x - ir.left)/ir.Width();
                    double rr = hypot(nx - s.cx, 0.0); // horizontal radius handle
                    s.r = max(0.0, rr);
                }
                break;
            case Shape::Type::Line:
                if(drag==DRAG_MOVE){ s.p1.x += dx; s.p1.y += dy; s.p2.x += dx; s.p2.y += dy; }
                else if(drag==DRAG_P1){ s.p1.x += dx; s.p1.y += dy; }
                else if(drag==DRAG_P2){ s.p2.x += dx; s.p2.y += dy; }
                break;
            case Shape::Type::Triangle:
                if(drag==DRAG_MOVE){ s.p1.x += dx; s.p1.y += dy; s.p2.x += dx; s.p2.y += dy; s.p3.x += dx; s.p3.y += dy; }
                else if(drag==DRAG_P1){ s.p1.x += dx; s.p1.y += dy; }
                else if(drag==DRAG_P2){ s.p2.x += dx; s.p2.y += dy; }
                else if(drag==DRAG_P3){ s.p3.x += dx; s.p3.y += dy; }
                break;
            }
            start = q;
            Refresh();
            return;
        }

        if(!drawing || selected<0) return;
        Shape& s = shapes[selected];
        switch(s.type){
        case Shape::Type::Rect: {
            double x0 = double(start.x - ir.left)/ir.Width();
            double y0 = double(start.y - ir.top)/ir.Height();
            double x1 = double(q.x - ir.left)/ir.Width();
            double y1 = double(q.y - ir.top)/ir.Height();
            s.x = min(x0,x1); s.y = min(y0,y1);
            s.w = fabs(x1-x0); s.h = fabs(y1-y0);
            break;
        }
        case Shape::Type::Circle: {
            double dx = double(q.x - ir.left)/ir.Width()  - s.cx;
            double dy = double(q.y - ir.top)/ir.Height()   - s.cy;
            s.r = sqrt(dx*dx + dy*dy);
            break;
        }
        case Shape::Type::Line:
            s.p2 = Pointf(double(q.x - ir.left)/ir.Width(), double(q.y - ir.top)/ir.Height());
            break;
        case Shape::Type::Triangle: {
            Pointf p1 = s.p1;
            Pointf p2 = Pointf(double(q.x - ir.left)/ir.Width(), double(q.y - ir.top)/ir.Height());
            Pointf p3 = Pointf(2*p1.x - p2.x, p2.y);
            s.p2 = p2; s.p3 = p3;
            break;
        }
        default: break;
        }
        Refresh();
    }

    void LeftUp(Point, dword) override {
        ReleaseCapture();
        drawing = false;
        drag = DRAG_NONE;
        Refresh();
        if(WhenModelChanged) WhenModelChanged();   // snapshot on mouse-up
    }


};

// ──────────────────────────────────────────────────────────────────────────────
// App window (buttons instead of ToolBar; two rows; splitter-safe)
// ──────────────────────────────────────────────────────────────────────────────
struct MainWin : TopWindow {
    typedef MainWin CLASSNAME;

    // layout
    Splitter   hsplit;
    ParentCtrl left, right;

    // top heading
    Label heading;

    // left panels
    StaticRect primitivesRow;
    StaticRect actionsRow;
    StaticRect varsPanel;
    Canvas     canvas;

    // primitive buttons
    Button btnCursor, btnRect, btnCircle, btnLine, btnTriangle;

    // actions buttons
    Button btnUndo, btnRedo, btnClear, btnSaveDes, btnLoadDes;

    // variables panel
    Option       optFill, optStroke, optClip, optSnap;
    ColorPusher  fillClr, strokeClr, outlineClr;
    EditString   edDash;
    EditIntSpin  spWidth, spOutline, spGrid;
    SliderCtrl   sOpacity;
    Label        lWidth, lDash, lOpacity, lOutline, lOffset, lGrid;

    // right: code header + editor
    StaticRect rightTop;
    Label      lFunc;
    EditString edFunc;
    Button     btnCopy, btnSaveCpp;
    DocEdit    code;

    // history
    Vector<String> hist;
    int hipos = -1;

    MainWin() {
        Title("U++ Procedural Icon Builder (WYSIWYG)").Sizeable().Zoomable();

        // Heading
        heading.SetText("U++ procedural icon builder 1.0");
        heading.SetAlign(ALIGN_CENTER);
        heading.SetFont(StdFont().Bold().Height(14));
        Add(heading.HSizePos().TopPos(0, 24));

        // ——— Left: primitives row (always single line; splitter-safe)
        btnCursor.SetLabel("Cursor");
        btnRect.SetLabel("Rect");
        btnCircle.SetLabel("Circle");
        btnLine.SetLabel("Line");
        btnTriangle.SetLabel("Triangle");

        // place in a single row
        int x=4, w=70, h=26, gap=4;
        for(Button* b : { &btnCursor, &btnRect, &btnCircle, &btnLine, &btnTriangle }) {
            primitivesRow.Add(b->LeftPos(x, w).TopPos(2, h));
            x += w + gap;
        }
        left.Add(primitivesRow.HSizePos().TopPos(24, 30));

        // ——— Left: actions row (second line)
        btnUndo.SetLabel("Undo");
        btnRedo.SetLabel("Redo");
        btnClear.SetLabel("Clear");
        btnSaveDes.SetLabel("Save des");
        btnLoadDes.SetLabel("Load des");

        x=4;
        for(Button* b : { &btnUndo, &btnRedo, &btnClear, &btnSaveDes, &btnLoadDes }) {
            actionsRow.Add(b->LeftPos(x, w).TopPos(2, h));
            x += w + gap;
        }
        left.Add(actionsRow.HSizePos().TopPos(24+30, 30));

        // ——— Variables panel
        int y = 0, lh = 22, pad = 4;
        lWidth.SetText("Width");
        lDash.SetText("Dash");
        lOpacity.SetText("Opacity");
        lOutline.SetText("Outline");
        lOffset.SetText("Offset");
        lGrid.SetText("Grid px");

        // tooltips / hints
        edDash.Tip("Dash pattern: e.g. \"5 3\" (on-off lengths in px). Empty = solid.");

        optFill.SetLabel("Fill");        optFill <<= true;
        fillClr.SetData(canvas.defaultStyle.fill);
        varsPanel.Add(optFill.LeftPos(4,80).TopPos(y,lh));
        varsPanel.Add(fillClr.LeftPos(90,80).TopPos(y,lh));
        y += lh + pad;

        optStroke.SetLabel("Stroke");    optStroke <<= true;
        strokeClr.SetData(canvas.defaultStyle.stroke);
        varsPanel.Add(optStroke.LeftPos(4,80).TopPos(y,lh));
        varsPanel.Add(strokeClr.LeftPos(90,80).TopPos(y,lh));
        y += lh + pad;

        varsPanel.Add(lWidth.LeftPos(4,80).TopPos(y,lh));
        spWidth.MinMax(1,64);            spWidth <<= 2;
        varsPanel.Add(spWidth.LeftPos(90,80).TopPos(y,lh));
        y += lh + pad;

        varsPanel.Add(lDash.LeftPos(4,80).TopPos(y,lh));
        edDash.SetText("");
        varsPanel.Add(edDash.LeftPos(90,160).TopPos(y,lh));
        y += lh + pad;

        varsPanel.Add(lOpacity.LeftPos(4,60).TopPos(y,lh));
        sOpacity.MinMax(0,100);          sOpacity <<= 100;
        varsPanel.Add(sOpacity.LeftPos(68,120).TopPos(y,lh));
        y += lh + pad;

        varsPanel.Add(lOutline.LeftPos(4,60).TopPos(y,lh));
        outlineClr.SetData(Red());
        varsPanel.Add(outlineClr.LeftPos(68,80).TopPos(y,lh));
        varsPanel.Add(lOffset.LeftPos(152,60).TopPos(y,lh));
        spOutline.MinMax(0,48);          spOutline <<= 0;
        varsPanel.Add(spOutline.LeftPos(214,60).TopPos(y,lh));
        y += lh + pad;

        optClip.SetLabel("Clip to inset"); optClip <<= false;
        varsPanel.Add(optClip.LeftPos(4,120).TopPos(y,lh));
        optSnap.SetLabel("Snap");          optSnap <<= true;
        varsPanel.Add(optSnap.LeftPos(130,70).TopPos(y,lh));
        varsPanel.Add(lGrid.LeftPos(200,60).TopPos(y,lh));
        spGrid.MinMax(4,200);              spGrid <<= 20;
        varsPanel.Add(spGrid.LeftPos(262,60).TopPos(y,lh));
        y += lh + pad;

        left.Add(varsPanel.HSizePos().TopPos(24+30+30, y + 2));

        // canvas below panels
        left.Add(canvas.HSizePos().VSizePos(24+30+30 + (y + 2), 0));

        // ——— Right: code header + code editor
        lFunc.SetText("Function:");
        rightTop.Add(lFunc.LeftPos(4,70).TopPos(4,24));
        edFunc.SetText("DrawMyIcon");
        rightTop.Add(edFunc.LeftPos(80,200).TopPos(4,24));
        btnCopy.SetLabel("Copy");
        btnSaveCpp.SetLabel("Save .cpp");
        rightTop.Add(btnCopy.RightPos(96+64,64).TopPos(4,24));
        rightTop.Add(btnSaveCpp.RightPos(32,96).TopPos(4,24));
        right.Add(rightTop.HSizePos().TopPos(0, 32));

        code.SetFont(CourierZ(12));
        code.SetReadOnly();
        right.Add(code.HSizePos().VSizePos(36, 0));

        // splitter and heading stacking
        Add(hsplit.HSizePos().VSizePos(24, 0));
        hsplit.Horz(left, right);

        // actions
        auto Sync = [&]{
            canvas.defaultStyle.enableFill    = (bool)~optFill;
            canvas.defaultStyle.enableStroke  = (bool)~optStroke;
            canvas.defaultStyle.fill          = (Color)fillClr.GetData();
            canvas.defaultStyle.stroke        = (Color)strokeClr.GetData();
            canvas.defaultStyle.strokeWidth   = (int)~spWidth;
            canvas.defaultStyle.dash          = ~edDash;
            canvas.defaultStyle.opacity       = (int)~sOpacity / 100.0;
            canvas.defaultStyle.outlineColor  = (Color)outlineClr.GetData();
            canvas.defaultStyle.outlineOffset = (int)~spOutline;
            canvas.clip_to_inset = (bool)~optClip;
            canvas.snap          = (bool)~optSnap;
            canvas.grid          = (int)~spGrid;
            canvas.Refresh();
            RefreshCode();
        };
        for(Ctrl* c : { (Ctrl*)&optFill, (Ctrl*)&fillClr, (Ctrl*)&optStroke, (Ctrl*)&strokeClr, (Ctrl*)&spWidth,
                        (Ctrl*)&edDash, (Ctrl*)&sOpacity, (Ctrl*)&outlineClr, (Ctrl*)&spOutline,
                        (Ctrl*)&optClip, (Ctrl*)&optSnap, (Ctrl*)&spGrid })
            c->WhenAction = Sync;

        // tool switching (highlight selected tool by bold font)
        auto SetTool = [&](Tool t){
            canvas.tool = t;
            auto NF = StdFont();
            auto BF = StdFont().Bold();
            btnCursor.SetFont(t==Tool::Cursor ? BF : NF);
            btnRect.SetFont(t==Tool::Rect ? BF : NF);
            btnCircle.SetFont(t==Tool::Circle ? BF : NF);
            btnLine.SetFont(t==Tool::Line ? BF : NF);
            btnTriangle.SetFont(t==Tool::Triangle ? BF : NF);
        };
        btnCursor  << [=]{ SetTool(Tool::Cursor);  };
        btnRect    << [=]{ SetTool(Tool::Rect);    };
        btnCircle  << [=]{ SetTool(Tool::Circle);  };
        btnLine    << [=]{ SetTool(Tool::Line);    };
        btnTriangle<< [=]{ SetTool(Tool::Triangle);};

        // row 2 actions
        btnUndo    << [=]{ Undo(); };
        btnRedo    << [=]{ Redo(); };
        btnClear   << [=]{ canvas.shapes.Clear(); canvas.Refresh(); RefreshCode(); if(canvas.WhenModelChanged) canvas.WhenModelChanged(); };
        btnSaveDes << [=]{ SaveDesign(); };
        btnLoadDes << [=]{ LoadDesign(); };

        // header buttons
        btnCopy    << [=]{ WriteClipboardText(code.Get()); };
        btnSaveCpp << [=]{
            FileSel fs; fs.Type("C++ file","*.cpp");
            if(fs.ExecuteSaveAs("Save generated .cpp")) SaveFile(fs.Get(), code.Get());
        };

        // track model changes -> history snapshot
        canvas.WhenModelChanged = THISBACK(OnCanvasChanged);


        // initial populate
        SetTool(Tool::Cursor);
        Sync();
        PushHistory();
    }
    
void OnCanvasChanged()
{
    PushHistory();
    RefreshCode();
}

    // code generation (with comments and Circle primitive)
    void RefreshCode() {
        String out;
        out << "static void " << Nvl(edFunc.GetText(), "DrawMyIcon")
            << "(BufferPainter& p, const Rect& inset)\n{\n"
            << "    // helpers\n"
            << "    auto X=[&](double nx){ return inset.left + int(inset.Width()*nx + 0.5); };\n"
            << "    auto Y=[&](double ny){ return inset.top  + int(inset.Height()*ny + 0.5); };\n"
            << "    auto Rr=[&](double nr){ return int(min(inset.Width(), inset.Height())*nr + 0.5); };\n\n";

        for(const Shape& s : canvas.shapes) {
            switch(s.type){
            case Shape::Type::Rect:
                out << "    // RECTANGLE\n";
                out << "    p.Begin();\n";
                out << Format("    p.Move(Pointf(X(%g), Y(%g)));\n", s.x, s.y);
                out << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", s.x+s.w, s.y);
                out << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", s.x+s.w, s.y+s.h);
                out << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", s.x, s.y+s.h);
                out << "    p.Close();\n";
                break;

            case Shape::Type::Circle:
                out << "    // CIRCLE\n";
                out << "    p.Begin();\n";
                out << Format("    p.Circle(X(%g), Y(%g), Rr(%g));\n", s.cx, s.cy, s.r);
                break;

            case Shape::Type::Line:
                out << "    // LINE\n";
                out << "    p.Begin();\n";
                out << Format("    p.Move(Pointf(X(%g), Y(%g)));\n", s.p1.x, s.p1.y);
                out << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", s.p2.x, s.p2.y);
                break;

            case Shape::Type::Triangle:
                out << "    // TRIANGLE\n";
                out << "    p.Begin();\n";
                out << Format("    p.Move(Pointf(X(%g), Y(%g)));\n", s.p1.x, s.p1.y);
                out << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", s.p2.x, s.p2.y);
                out << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", s.p3.x, s.p3.y);
                out << "    p.Close();\n";
                break;
            }

            if(s.style.outlineOffset > 0){
                out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n",
                              s.style.strokeWidth + 2*max(0, s.style.outlineOffset),
                              s.style.outlineColor.GetR(), s.style.outlineColor.GetG(), s.style.outlineColor.GetB());
                out << "    p.End();\n";

                // redraw path for primary styles
                if(s.type == Shape::Type::Rect){
                    out << "    p.Begin();\n";
                    out << Format("    p.Move(Pointf(X(%g), Y(%g)));\n", s.x, s.y);
                    out << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", s.x+s.w, s.y);
                    out << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", s.x+s.w, s.y+s.h);
                    out << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", s.x, s.y+s.h);
                    out << "    p.Close();\n";
                }
                else if(s.type == Shape::Type::Circle){
                    out << "    p.Begin();\n";
                    out << Format("    p.Circle(X(%g), Y(%g), Rr(%g));\n", s.cx, s.cy, s.r);
                }
                else if(s.type == Shape::Type::Line){
                    out << "    p.Begin();\n";
                    out << Format("    p.Move(Pointf(X(%g), Y(%g)));\n", s.p1.x, s.p1.y);
                    out << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", s.p2.x, s.p2.y);
                }
                else if(s.type == Shape::Type::Triangle){
                    out << "    p.Begin();\n";
                    out << Format("    p.Move(Pointf(X(%g), Y(%g)));\n", s.p1.x, s.p1.y);
                    out << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", s.p2.x, s.p2.y);
                    out << Format("    p.Line(Pointf(X(%g), Y(%g)));\n", s.p3.x, s.p3.y);
                    out << "    p.Close();\n";
                }
            }
            if(!s.style.dash.IsEmpty())
                out << Format("    p.Dash(String(\"%s\"), 0.0);\n", s.style.dash.Begin());
            if(s.style.opacity < 1.0)
                out << Format("    p.Opacity(%g);\n", s.style.opacity);
            if(s.style.enableFill && s.type != Shape::Type::Line)
                out << Format("    p.Fill(Color(%d,%d,%d));\n", s.style.fill.GetR(), s.style.fill.GetG(), s.style.fill.GetB());
            if(s.style.enableStroke)
                out << Format("    p.Stroke(%d, Color(%d,%d,%d));\n", s.style.strokeWidth, s.style.stroke.GetR(), s.style.stroke.GetG(), s.style.stroke.GetB());
            out << "    p.End();\n\n";
        }
        out << "}\n";
        code.Set(out);
    }

    // history / json
    void PushHistory() {
        String snap = MakeJson();
        if(hipos+1 < hist.GetCount())
            hist.Remove(hipos+1, hist.GetCount() - (hipos+1));
        hist.Add(snap);
        hipos = hist.GetCount()-1;
    }
    void Undo() { if(hipos > 0) { hipos--; LoadJson(hist[hipos]); } }
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
            st("fill")          = ColorToString(s.style.fill);
            st("stroke")        = ColorToString(s.style.stroke);
            st("strokeWidth")   = s.style.strokeWidth;
            st("dash")          = s.style.dash;
            st("enableFill")    = s.style.enableFill;
            st("enableStroke")  = s.style.enableStroke;
            st("opacity")       = s.style.opacity;
            st("outlineColor")  = ColorToString(s.style.outlineColor);
            st("outlineOffset") = s.style.outlineOffset;
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
            s.style.fill          = StringToColor(st["fill"].ToString());
            s.style.stroke        = StringToColor(st["stroke"].ToString());
            s.style.strokeWidth   = (int)st["strokeWidth"];
            s.style.dash          = st["dash"].ToString();
            s.style.enableFill    = (bool)st["enableFill"];
            s.style.enableStroke  = (bool)st["enableStroke"];
            s.style.opacity       = st["opacity"].To<double>();
            s.style.outlineColor  = StringToColor(st["outlineColor"].ToString());
            s.style.outlineOffset = (int)st["outlineOffset"];

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
        return true;
    }
};

GUI_APP_MAIN
{
    SetLanguage(GetSystemLNG());
    MainWin().Run();
}
