#ifndef _GalleryCtrl_GalleryCtrl_h_
#define _GalleryCtrl_GalleryCtrl_h_

#include <CtrlLib/CtrlLib.h>
#include <Painter/Painter.h>

using namespace Upp;

// ---------- Status & policies ----------
enum class ThumbStatus {
    Auto,        // generated dummy (checker + tinted block)
    Placeholder, // dashed box + plus
    Missing,     // expected but not found
    Ok,          // image present & valid
    Error        // image present but failed/unsupported
};

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

inline bool HasFlag(DataFlags f, DataFlags m) { return (f & m) != DF_None; }

// ---------- Item ----------
struct GalleryItem : Moveable<GalleryItem> {
    String      name;
    Image       src;           // full-res (optional)
    Image       thumb_normal;  // cached per-zoom/policy (optional)
    Image       thumb_gray;    // cached grayscale per-zoom/policy (optional)
    Color       seed;          // deterministic tint (from name)
    ThumbStatus status = ThumbStatus::Auto;
    bool        selected = false;
    bool        filtered_out = false;
    DataFlags   flags = DF_None;
};

// ---------- Control ----------
class GalleryCtrl : public Ctrl {
public:
    typedef GalleryCtrl CLASSNAME;              // required for THISBACK(...)
    GalleryCtrl();

    // ---- Events ----
    Gate1<const Vector<int>&> WhenSelecting;    // pre-selection veto
    Event<>                   WhenSelection;    // post-selection
    Event<const GalleryItem&> WhenActivate;     // double-click / Enter on item
    Event<int>                WhenZoom;         // zoom step index
    Event<int>                WhenCaret;        // anchor index (not yet surfaced)
    Event<int>                WhenHover;        // item index or -1
    Event<Bar&>               WhenBar;          // extend context menu

    // ---- Items & Images ----
    int   Add(const String& name, const Image& opt_img = Image(), Color tint = Null);
    void  AddDummy(const String& name);

    bool  SetThumbFromFile(int index, const String& filepath); // (stub) returns false if not loaded
    void  SetThumbImage(int index, const Image& img);
    void  ClearThumbImage(int index);

    // ---- Status & Data Flags ----
    void      SetThumbStatus(int index, ThumbStatus s);
    void      SetDataFlags(int index, DataFlags f);
    DataFlags GetDataFlags(int index) const;

    // ---- Selection & Filtering ----
    Vector<int> GetSelection() const;
    void        ClearSelection();

    void  SetFiltered(int index, bool filtered_out);
    void  ClearFilterFlags();

    // ---- Zoom & Aspect ----
    void         SetZoomIndex(int zi);
    int          GetZoomIndex() const { return zoom_i; }

    void         SetAspectPolicy(AspectPolicy p);
    AspectPolicy GetAspectPolicy() const { return aspect_policy; }

    // ---- Visual toggles ----
    void  SetShowSelectionBorders(bool b) { show_selection_border = b; Refresh(); }
    void  SetShowFilterBorders(bool b)    { show_filter_border    = b; Refresh(); }
    void  SetSaturationOn(bool b)         { saturation_on         = b; Refresh(); }
    bool  GetShowSelectionBorders() const { return show_selection_border; }
    bool  GetShowFilterBorders() const    { return show_filter_border; }
    bool  GetSaturationOn() const         { return saturation_on;  }

    // Label backdrop strength (0..255, simulated)
    void  SetLabelBackdropAlpha(int a)    { label_backdrop_alpha = clamp(a, 0, 255); Refresh(); }
    int   GetLabelBackdropAlpha() const   { return label_backdrop_alpha; }

    // Hover effect on/off
    void  SetHoverEnabled(bool b)         { hover_enabled = b; if(!b && hover_index!=-1){hover_index=-1; Refresh(); if(WhenHover) WhenHover(-1);} }
    bool  GetHoverEnabled() const         { return hover_enabled; }

    // ---- Built-in glyph accessors ----
    static const Image& PlaceholderGlyph(int tile = 32);
    static const Image& MissingGlyph(int tile = 32);
    static const Image& ErrorGlyph(int tile = 32);

private:
    // ---- Layout / geometry helpers ----
    void  Reflow();
    Rect  IndexRectNoScroll(int i) const;
    Rect  IndexRect(int i) const;

    // ---- Point to index + guards ----
    inline bool IsValidIndex(int i) const { return i >= 0 && i < items.GetCount(); }
    GalleryItem* TryItem(int i);
    int   IndexFromPoint(Point p) const; // -1 if not an item

    // ---- Scrolling ----
    void  OnScroll();

    // ---- Selection helpers ----
    void  SelectRange(int a, int b, bool additive);
    bool  CommitSelection(const Vector<int>& next);
    void  ApplyMarqueeSelection();
    void  DoSelectAll();
    void  DoInvertSelection();
    void  DoClearSelection();
    void  DoRemoveSelected();
    void  DoRemoveAll();

    // ---- Context menu ----
    void  ShowContextMenu(Point p);

    // ---- Caching / rendering helpers ----
    void   EnsureThumbs(GalleryItem& it);
    Color  AutoColorFromText(const String& s) const;

    // Aspect helpers (definitions in .cpp)
    Size   FitSize(Size src, int tile) const;
    Size   FillSize(Size src, int tile) const;
    Image  MakeFillCropped(const Image& src, int tile, bool gray) const;
    Image  MakeGray(const Image& in) const;

    // Glyph painters/builders
    static void   StrokeRect(Draw& w, const Rect& r, int t, const Color& c);
    static void   DrawPlaceholderGlyph(BufferPainter& p, int tile, bool gray);
    static void   DrawMissingGlyph(BufferPainter& p, int tile, bool gray);
    static void   DrawErrorGlyph(BufferPainter& p, int tile, bool gray);
    static Image  MakePlaceholderGlyph(int tile, bool gray);
    static Image  MakeMissingGlyph(int tile, bool gray);
    static Image  MakeErrorGlyph(int tile, bool gray);

private:
    // ---- Data ----
    Vector<GalleryItem> items;

    // ---- Scrollbars & content metrics ----
    ScrollBars sb;
    int        scroll_x   = 0;
    int        scroll_y   = 0;
    int        content_w  = 0;
    int        content_h  = 0;

    // ---- Grid metrics ----
    int        cols       = 1;
    int        pad        = 6;
    int        labelH     = 20;

    // ---- Zoom ----
    Vector<int>  zoom_steps;      // e.g. {32, 48, 64, 96, 128}; filled in ctor
    int          zoom_i     = 0;  // current zoom index (into zoom_steps)

    // ---- Policy / toggles ----
    AspectPolicy aspect_policy       = AspectPolicy::Fit;
    bool         show_selection_border = true;
    bool         show_filter_border    = true;
    bool         saturation_on         = true;
    int          label_backdrop_alpha  = 180;
    bool         hover_enabled         = true;

    // ---- Input / hover / selection state ----
    bool        mouse_down     = false;
    bool        dragging       = false;
    bool        drag_additive  = false;
    Point       drag_origin_win;
    Rect        drag_rect_win;
    Vector<int> drag_prev_sel;           // snapshot for additive marquee

    int         hover_index    = -1;
    int         anchor_index   = -1;

private:
    // ---- Ctrl overrides ----
    void   Paint(Draw& w) override;
    void   LeftDown(Point p, dword flags) override;
    void   LeftDouble(Point p, dword flags) override;
    void   LeftUp(Point p, dword flags) override;
    void   RightDown(Point p, dword flags) override;
    void   MouseMove(Point p, dword flags) override;
    void   MouseLeave() override { if(hover_index != -1){ hover_index = -1; Refresh(); if(WhenHover) WhenHover(-1);} }
    bool   Key(dword key, int) override;
    void   MouseWheel(Point p, int zdelta, dword keyflags) override;
};

#endif
