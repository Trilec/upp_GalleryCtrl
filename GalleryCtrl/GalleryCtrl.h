#ifndef _GalleryCtrl_GalleryCtrl_h_
#define _GalleryCtrl_GalleryCtrl_h_

#include <CtrlLib/CtrlLib.h>
#include <Painter/Painter.h>

namespace Upp {

//----------------------------------------------------------------------------
//  Enums / Flags
//----------------------------------------------------------------------------
enum class ThumbStatus { Auto, Placeholder, Missing, Ok, Error };

enum class AspectPolicy {
    Fit,     // keep aspect; letter/pillar box (centered)
    Fill,    // keep aspect; crop to fill (center crop)
    Stretch  // ignore aspect; force to tile
};

enum DataFlags : unsigned {
    DF_None        = 0,
    DF_NameMissing = 1 << 0,
    DF_MetaMissing = 1 << 1,
    DF_TagMissing  = 1 << 2
};

enum class ScrollMode { Auto, VerticalOnly, HorizontalOnly, None };

// Small, square glyphs drawn procedurally & cached.
enum GlyphType {
    GLYPH_PLACEHOLDER = 0,   // mountains + sun, gray
    GLYPH_MISSING,            // placeholder with diagonal slash
    GLYPH_ERROR,              // amber warning triangle + !
    GLYPH_WARNING,            // yellow caution triangle + !
    GLYPH_STATUS_OK,          // green dot
    GLYPH_STATUS_WARN,        // yellow dot
    GLYPH_STATUS_ERR,         // red dot
    GLYPH__COUNT
};

//----------------------------------------------------------------------------
//  Item model (internal)
//----------------------------------------------------------------------------
struct GalleryItem : Moveable<GalleryItem> {
    String      name;
    Image       thumb;          // color
    Image       thumb_gray;     // cached grayscale for filtered state
    int         seed = 0;
    ThumbStatus status = ThumbStatus::Auto;
    bool        selected = false;
    bool        filtered_out = false;
    DataFlags   flags = DF_None;
};

//----------------------------------------------------------------------------
//  Control
//----------------------------------------------------------------------------
class GalleryCtrl : public Ctrl {
public:
    // --- Construction
    GalleryCtrl();

    // --- Items & Images
    int   Add(const String& name, const Image& opt_img = Image(), Color tint = Null);
    void  AddDummy(const String& name);

    bool  SetThumbFromFile(int index, const String& filepath);
    void  SetThumbImage(int index, const Image& img);
    void  ClearThumbImage(int index);

    // --- Status & Data Flags
    void      SetThumbStatus(int index, ThumbStatus s);
    void      SetDataFlags(int index, DataFlags f);
    DataFlags GetDataFlags(int index) const;

    // --- Selection & Filtering
    Vector<int> GetSelection() const;
    void        ClearSelection();

    void  SetFiltered(int index, bool filtered_out);
    void  ClearFilterFlags();

    // --- Zoom & Aspect
    void        SetZoomIndex(int zi); // 0..(N-1)
    int         GetZoomIndex() const { return zoom_i; }

    void        SetAspectPolicy(AspectPolicy p);
    AspectPolicy GetAspectPolicy() const { return aspect; }

    // --- Visual Toggles
    void  SetShowSelectionBorders(bool b);
    void  SetShowFilterBorders(bool b);
    void  SetSaturationOn(bool b);
    bool  GetShowSelectionBorders() const { return show_sel_ring; }
    bool  GetShowFilterBorders() const { return show_filter_ring; }
    bool  GetSaturationOn() const { return saturation_on; }

    void  SetHoverEnabled(bool b);
    bool  GetHoverEnabled() const { return hover_enabled; }

    void  SetLabelBackdropAlpha(int a); // 0..255 simulated
    int   GetLabelBackdropAlpha() const { return label_backdrop_alpha; }

    // --- Layout & scroll
    void        SetScrollMode(ScrollMode m);
    ScrollMode  GetScrollMode() const { return scroll_mode; }
    void        SetTilePadding(int px);   // gap around tiles
    int         GetTilePadding() const { return pad; }

    int         GetCount() const { return items.GetCount(); }
    void        Clear();

    // --- Events
    Gate1<const Vector<int>&> WhenSelecting;      // return false to veto
    Event<>                   WhenSelection;      // after commit
    Event<const GalleryItem&> WhenActivate;       // dbl-click / Enter
    Event<int>                WhenZoom;           // zoom index changed
    Event<int>                WhenCaret;          // anchor index moved
    Event<int>                WhenHover;          // hover index (or -1)
    Event<Bar&>               WhenBar;            // extend context menu

    // --- Glyph accessors (compat with spec)
    static const Image& PlaceholderGlyph(int tile = 64) { return Glyph(GLYPH_PLACEHOLDER, tile); }
    static const Image& MissingGlyph(int tile = 64)     { return Glyph(GLYPH_MISSING, tile); }
    static const Image& ErrorGlyph(int tile = 64)       { return Glyph(GLYPH_ERROR, tile); }

    // --- Procedural demo tools (kept in lib while developing)
    static Image GenRandomThumb(int edge_px, int aspect_w = 0, int aspect_h = 0, uint32 seed = 0);
    static const Image& Glyph(GlyphType type, int tile);
    static Image GenThumbWithGlyph(GlyphType type, int edge_px, uint32 seed = 0);
    static void  FillWithRandom(GalleryCtrl& dst, int count, int thumb_edge_px, uint32 seed_base = 0);

private:
    // ---- Ctrl overrides ----
    void   Paint(Draw& w) override;
    void   Layout() override;                // recompute grid on resize
    void   LeftDown(Point p, dword flags) override;
    void   LeftDouble(Point p, dword flags) override;
    void   LeftUp(Point p, dword flags) override;
    void   RightDown(Point p, dword flags) override;
    void   MouseMove(Point p, dword flags) override;
    void   MouseLeave() override;
    bool   Key(dword key, int) override;
    void   MouseWheel(Point p, int zdelta, dword keyflags) override;

    // ---- Layout / paint helpers ----
    void   Reflow();
    Rect   TileRect(int index) const;          // tile rect in CONTENT coords
    Rect   ImageRect(const Rect& tile) const;  // image box within tile
    int    IndexFromPoint(Point content_pt) const; // -1 if gap or outside

    // ---- Selection helpers ----
    GalleryItem*       TryItem(int i)       { return (i >= 0 && i < items.GetCount()) ? &items[i] : nullptr; }
    const GalleryItem* TryItem(int i) const { return (i >= 0 && i < items.GetCount()) ? &items[i] : nullptr; }
    void   CommitSelection(const Vector<int>& indices);
    Vector<int> IndicesInRect(const Rect& rc) const;  // tiles intersecting rect (CONTENT coords)
    static Rect NormalizeRect(Rect r);
    
    void ApplyMarqueeSelection(bool add, bool sub, bool inter, bool xr);
	void SetCtrlMarqueeXor(bool on) { ctrl_marquee_xor = on; }
	bool GetCtrlMarqueeXor() const  { return ctrl_marquee_xor; }


private:
    // ---- Data ----
    Vector<GalleryItem> items;

    ScrollBars sb;

    // geometry
    int   pad = 8;                  // pixel gap between tiles
    int   label_h = 18;             // bottom label bar height
    int   cols = 1;
    int   rows = 0;
    int   content_w = 0;
    int   content_h = 0;

    // view state
    int   zoom_i = 2;               // index into zoom_steps
    AspectPolicy aspect = AspectPolicy::Fit;
    ScrollMode   scroll_mode = ScrollMode::Auto;

    // flags
    bool  show_sel_ring    = true;
    bool  show_filter_ring = true;
    bool  hover_enabled    = true;
    bool  saturation_on    = true;

    int   label_backdrop_alpha = 170; // 0..255 simulated alpha

    // interaction
    int   hover_index  = -1;
    int   anchor_index = -1;
    int   caret_index  = -1;

    int   scroll_x = 0;
    int   scroll_y = 0;

    bool  mouse_down        = false;
    bool  dragging          = false;
    
	// Ctrl+Alt = Intersect (highest precedence)
	// else Alt = Subtract
	// else (Shift or Ctrl) = Add
	bool  ctrl_marquee_xor = true;
	bool  drag_additive = false;
	bool  drag_subtractive = false;
	bool  drag_intersect = false;
	bool  drag_xor = false;
	bool  pending_click  = false;
	int   pending_index  = -1;
	dword pending_flags  = 0;

    Point drag_origin_win;
    Rect  drag_rect_win;
    Vector<int> drag_prev_sel;

    // zoom step lookup (longest side)
    static const int* ZoomSteps();
    static int        ZoomStepCount();
};

} // namespace Upp

#endif
