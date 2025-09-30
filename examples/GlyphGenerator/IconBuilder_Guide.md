# U++ Icon Builder — Guide v02

> Goal: a single-file U++ app that edits simple vector primitives (Rect, Circle, Line, Triangle, Text, Curve), with normalized geometry, per-primitive operations, selection & editing, live code-generation, and JSON save/load. The guide describes how to build it cleanly and extensibly.

* * *

## 1) Project shape

- **One file** for now: `main.cpp` (uses `Core`, `CtrlLib`, `Draw`, `Painter`).
    
- **Single main window** `MainWin : TopWindow` embedding a custom `Canvas : Ctrl` and a right-hand code pane.
    
- **No virtual classes** for primitives. Use a **function-pointer registry** (`PrimitiveOps`) indexed by an enum `PType`.
    
- **Value semantics** everywhere (`Moveable<T>`, `Vector<Shape>`). No ownership puzzles.
    

* * *

## 2) Coordinates & snapping (the backbone)

- Work inside a **clip/inset rectangle** centered in the canvas (aspect-controlled).
    
- Store all geometry **normalized** to that inset: 0..1 along X and Y.
    
- Helpers (copy these exactly):
    

```cpp
static inline int    X (const Rect& r, double nx){ return r.left + int(r.Width()*nx + 0.5); }
static inline int    Y (const Rect& r, double ny){ return r.top  + int(r.Height()*ny + 0.5); }
static inline int    R (const Rect& r, double nr){ return int(min(r.Width(), r.Height())*nr + 0.5); }
static inline double NX(const Rect& r, int px)   { return (px - r.left) / double(max(1, r.Width())); }
static inline double NY(const Rect& r, int py)   { return (py - r.top)  / double(max(1, r.Height())); }
static inline int    Snap1D(int v, int o, int s) { return o + ((v - o + s/2) / s) * s; }
```

- **Snap in pixel space**, then convert to normalized. Prevents drift.

* * *

## 3) Data model

```cpp
enum class Tool  { Cursor, CreateShape };      // current mode
enum class PType { Rect, Circle, Line, Triangle, Curve, Text };

enum class LineStyle { Solid, LongDash, ShortDash, Dotted };

struct Style : Moveable<Style> {
    Color  fill   = Color(163,201,168);
    Color  stroke = Color(30,53,47);
    int    strokeWidth = 2;
    bool   enableFill = true, enableStroke = true, evenOdd = false;
    String dash;                               // custom stroke dash (optional)
    double opacity = 1.0, fillOpacity = 1.0, strokeOpacity = 1.0;

    // Outline (extra rim pass)
    bool   outlineEnable = false, outlineOutside = true;
    Color  outlineColor = Red();
    int    outlineWidth = 0;                   // extra thickness around main stroke
    String outlineDash;  double outlineOpacity = 1.0;
    int    outlineOffsetX = 0, outlineOffsetY = 0; // shadow-ish offset
    LineStyle strokeStyle = LineStyle::Solid, outlineStyle = LineStyle::Solid;
};

struct TextData : Moveable<TextData> {
    String text = "Text";
    String face = "";                          // empty => default
    double sizeN = 0.18;                       // normalized height vs inset
    bool   bold = false, italic = false;
    // (alignment, shadow, etc. can be added later)
};

struct CurveData : Moveable<CurveData> {
    bool   cubic = true, closed = false;
    Pointf a0, a1, c0, c1;
};

struct Shape : Moveable<Shape> {
    PType type = PType::Rect;
    Style style;

    // Rect
    double x=0, y=0, w=0, h=0;
    // Circle
    double cx=0, cy=0, r=0;
    // Line/Triangle
    Pointf p1, p2, p3;
    // Payloads
    TextData  text;
    CurveData curve;
};
```

* * *

## 4) Primitive protocol (ops registry)

Each primitive provides a small set of functions:

```cpp
struct PrimitiveOps {
    void (*EmitPainter)(BufferPainter&, const Rect& inset, const Shape& s);
    bool (*HitBody)(const Rect& inset, const Shape& s, Point m);
    int  (*HitVertex)(const Rect& inset, const Shape& s, Point m);
    void (*DrawOverlay)(Draw& w, const Rect& inset, const Shape& s);
    void (*BeginCreate)(Shape& s, const Rect& inset, Point start_px);
    void (*DragCreate)(Shape& s, const Rect& inset, Point start_px, Point cur_px, bool snap, int grid);
    void (*BeginEdit)(Shape& s, const Rect& inset, Point grab_px, int hitVertex, double& grab_nx, double& grab_ny);
    void (*DragEdit)(Shape& s, const Rect& inset, Point cur_px, bool snap, int grid,
                     bool moving, int drag_vertex, double& grab_nx, double& grab_ny);
    void (*EmitCode)(String& out, const Shape& s);
};

struct ToolSpec { PType type; const char* label; const char* tip; };

const PrimitiveOps&     GetOps(PType t);
const Vector<ToolSpec>& GetToolSpecs();
```

**Why this shape:** tiny, non-virtual dispatch; Canvas stays generic; adding a primitive is “add row to registry”.

* * *

## 5) Canvas responsibilities (only)

`Canvas : Ctrl` owns the document, selection, interaction, and rendering:

- **Model:** `Vector<Shape> shapes; int selected = -1;`
    
- **Tooling:** `Tool tool; PType creation_type; bool snap, clip; int grid;`
    
- **View:** aspect, clip scale, background color, `show_grid`
    
- **History:** `Vector<String> hist; int hist_ix;` storing JSON snapshots
    
- **Text template:** applied when creating new Text shapes
    
- **Callbacks:** `WhenSelection`, `WhenShapesChanged`
    

### Event flow

**Creation**

1.  `LeftDown` (tool = CreateShape): snap `start_px` if requested; push a `Shape` with `type = creation_type`.
    
2.  `BeginCreate` → `DragCreate` on mouse move.
    
3.  `LeftUp`: finish, fire `WhenShapesChanged`, `PushHist()`.
    

**Editing/Selection**

1.  `LeftDown` (tool = Cursor): try vertex hits from topmost to back; else body hit; set `selected`.
    
2.  `BeginEdit` (stores normalized grab point); `moving = (drag_vertex < 0)`.
    
3.  `DragEdit` while moving/resizing; update canvas.
    
4.  `LeftUp`: finish, fire `WhenShapesChanged`, `PushHist()`.
    

**Keyboard**

- `Delete`: remove selection.
    
- (Later) add arrow nudges, Ctrl+Z/Y, etc.
    

* * *

## 6) Drawing pipeline

1.  Clear the window and compute `Rect inset = GetInsetRect();`
    
2.  **Grid** (if enabled): draw on `BufferPainter` or directly on `Draw`—cheap lines, 1-px.
    
3.  **Painter layer (opaque)**
    
    - Create `ImageBuffer ib(size); ib.SetKind(IMAGE_OPAQUE);`
        
    - `BufferPainter p(ib, MODE_ANTIALIASED); p.Clear(bg_color);`
        
    - Draw white paper rect for `inset`.
        
    - If `clip`, emit a box path, `Clip()`, and keep it active until shapes are drawn, then `End()` to pop.
        
    - For each shape: `GetOps(s.type).EmitPainter(p, inset, s);`
        
    - Blit with `w.DrawImage(0,0, Image(ib));`
        
4.  **Overlay** (never inside painter)
    
    - If there is a selection: `GetOps(type).DrawOverlay(w, inset, shape);`
        
    - Draw clip frame last (4 thin rects).
        

**Guards:** skip tiny primitives (`MIN_EMIT_PX` width, height, or length). Always keep `Begin/End` balanced.

* * *

## 7) Style application & outline pass

- Keep three high-level passes:

```cpp
template<class Build>
static void Pass_Fill  (BufferPainter& p, Build path, const Style& st){
    if(!st.enableFill) return; p.Begin(); path();
    double o = clamp(st.fillOpacity,0.0,1.0) * clamp(st.opacity,0.0,1.0);
    if(o < 1) p.Opacity(o);
    if(st.evenOdd) p.EvenOdd(true);
    p.Fill(st.fill); p.End();
}
template<class Build>
static void Pass_Stroke(BufferPainter& p, Build path, const Style& st){
    if(!st.enableStroke) return; p.Begin(); path();
    double o = clamp(st.strokeOpacity,0.0,1.0) * clamp(st.opacity,0.0,1.0);
    if(o < 1) p.Opacity(o);
    String d = DashFrom(st.strokeStyle, st.dash); if(!d.IsEmpty()) p.Dash(d, 0.0);
    p.Stroke(st.strokeWidth, st.stroke); p.End();
}
template<class Build>
static void Pass_Outline(BufferPainter& p, Build path, const Style& st){
    if(!st.outlineEnable || st.outlineWidth <= 0) return;
    p.Begin();
    if(st.outlineOffsetX || st.outlineOffsetY) p.Translate(st.outlineOffsetX, st.outlineOffsetY);
    path();
    double o = clamp(st.outlineOpacity,0.0,1.0) * clamp(st.opacity,0.0,1.0);
    if(o < 1) p.Opacity(o);
    String d = DashFrom(st.outlineStyle, st.outlineDash); if(!d.IsEmpty()) p.Dash(d, 0.0);
    int W = (st.enableStroke ? st.strokeWidth : 0) + max(1, 2*st.outlineWidth);
    p.Stroke(W, st.outlineColor); p.End();
}
```

- **Order:** If outline is “outside”, draw outline → fill → stroke; otherwise fill/ stroke then outline.

* * *

## 8) Per-primitive rules (summary)

- **Rect**
    
    - Path: 4 lines + `Close()`.
        
    - Hit: normalized rect contains (with small inflate).
        
    - Handles: 4 corners.
        
- **Circle**
    
    - Path: start at east, two `SvgArc(rr, rr, 0, false, true, …)` semicircles; `Close()`.
        
    - Hit: filled interior **or** near ring (`max(6, stroke/2+4)` tolerance).
        
    - Handles: center (move) + east (radius).
        
- **Line**
    
    - Path: `Move(a); Line(b)`.
        
    - Hit: `IsNearSegment`.
        
    - Handles: endpoints.
        
- **Triangle**
    
    - Path: A→B→C→`Close()`.
        
    - Hit: `IsPointInTriangle` when filled; else near any edge.
        
    - Handles: 3 vertices (draw just outside the corner for easier grabbing).
        
- **Curve (Quadratic / Cubic)**
    
    - Path: `Move(a0)` then `Quadratic(c0,a1)` or `Cubic(c0,c1,a1)`; optional `Close()`.
        
    - Hit: bbox inflate check (fast & adequate for editor).
        
    - Handles: a0,c0\[,c1\],a1; helper lines to controls.
        
- **Text (TOP-aligned)**
    
    - **Semantic:** `s.y` is the **TOP** of the text row; the pen baseline is `top + F.Info().GetAscent()`.
        
    - Emit characters: `p.Character(pen, ch, F)` and advance `pen.x` by `GetTextSize(String(ch,1),F).cx`.
        
    - Hit: `TextPixelRect(...)` inflated; handles at 4 corners.
        
    - Creation: drag vertically to set height (`sizeN`), top remains at initial `y`.
        

* * *

## 9) JSON I/O (centralized)

- Root map holds canvas flags (`snap, clip, grid, aspect_ix, clip_scale_n, bg_r/g/b, text_template`) and a `shapes` array.
    
- Each shape entry:
    
    - `"type"` (int), `"style"` map, plus type-specific fields.
- Loader accepts both **ValueMap** and **array of {key,value} pairs** (robust decoding).
    

Minimal sketch:

```cpp
String Canvas::SaveJson() const;   // Build ValueMap root, ValueArray shapes, StoreAsJson(root,true)
void   Canvas::LoadJson(const String& js); // ParseJSON, normalize to ValueMap, rebuild shapes, fire callbacks
```

* * *

## 10) Code generation (right pane)

- For each shape: call `GetOps(type).EmitCode(out, s)`.
    
- Emit the exact geometry you draw, then conditional styles, then `p.End();`.
    
- **Circle** code uses the two-arc pattern.
    
- **Text** code is **top-aligned** to match the editor. Build the same `Font` as runtime and iterate characters.
    

Skeleton header in pane:

```cpp
void DrawIcon(Draw& w, const Rect& inset)
{
    BufferPainter p(w, MODE_ANTIALIASED);

    // …shape snippets appended here…
}
```

* * *

## 11) UI & tools

- **Tool buttons** come from `GetToolSpecs()`; action sets `tool = CreateShape` and `creation_type = spec.type`.
    
- **Ops row:** `Snap`, `Clip`, `ClipAspect`, `SampleRes`, `Grid` (checkbox), `Step` (grid size).
    
- **Actions row:** `Duplicate`, `FlipX`, `FlipY`, `LayerUp`, `LayerDn`, `Undo`, `Redo`, `Reset`, `Clear`, `Delete`, `Save`, `Load`, `BG` (checkbox) + `ColorPusher`.
    
    - **Reset** = reset **style of selection** to defaults.
        
    - **Clear** = remove all shapes.
        
- **Style panel:** Fill/Stroke toggles, colors, width, dash (string + preset type), per-channel opacities, Outline group (color/width/dash/type/offset).
    
- **Text row:** `Text`, `Codes` (comma-separated char codes), `Font` (DropList), `B`/`I`.
    

**Ownership pattern:** controls you need to read/write later are members; throwaway labels can be created and attached to the parent panel (the parent owns their lifetime).

* * *

## 12) Layering & transforms

- `LayerUp/Down`: swap neighbors and update `selected`.
    
- `Duplicate`: copy shape, nudge by ~6 px in normalized units.
    
- `FlipX/FlipY`: reflect around clip center (0.5, 0.5) in normalized space.
    

* * *

## 13) Testing & guards

- Skip degenerate primitives (`MIN_EMIT_PX` ≈ 6).
    
- Clamp opacities to `[0,1]`.
    
- Validate dash strings (two positive numbers minimum) before calling `Dash`.
    
- Keep `Begin/End` balanced and pair any `Clip()` with an `End()`.
    

**Manual regression (quick list)**

- Create/drag/move each primitive with Snap on/off.
    
- Check Text creation top-alignment and resizing.
    
- Toggle Fill/Stroke/Outline/EvenOdd and dashes.
    
- Undo/Redo after a batch of operations.
    
- Save/Load round-trip; exported code compiles and looks the same.
    

* * *

## 14) Extending with a new primitive (recipe)

1.  Define its payload in `Shape` if needed.
    
2.  Implement the 9 `PrimitiveOps` functions.
    
3.  Add one `FacetRow` to the registry with ops + tool spec.
    
4.  (Optional) add style controls if it needs new UI.
    

* * *

## 15) Roadmap (short)

- **Icons/Symbols:** add a `Symbol` primitive that draws stored path data (IML/JSON bank).
    
- **Layout polish:** small grid layout helper for rows; keyboard shortcuts; nudge & align tools.
    
- **Text++:** alignment (L/C/R), vertical alignment, optional shadow pass.
    
- **Exporters:** PNG/ICO dialogs using offscreen renders at `SampleRes`.
    

* * *

## 16) Checklist — plan vs current

- Single-file app, TopWindow + Canvas + code pane
    
- Normalized geometry + snap in pixel space
    
- Ops registry (per primitive)
    
- Rect, Circle, Line, Triangle, **Text (TOP-aligned)**, Curve
    
- Selection, move/resize with handles; overlays drawn on top
    
- Style system incl. outline pass, dashes, opacities
    
- Grid toggle, BG toggle + color
    
- JSON save/load (robust), Undo/Redo (hist = JSON)
    
- Code generation mirrors runtime (incl. circle arcs, text)
    
- Icons/Symbol primitive
    
- Alignment tools, keyboard shortcuts for nudge
    
- Layout grid helper / UI polish
    
- PNG/ICO exporters, header export template selector
    
- Text alignment/vertical alignment/shadow options
    

* * *

**That’s the full spec you can hand to someone and they’ll build the exact editor we want, with a clear path for the next features.**