#include "GalleryCtrl.h"

using namespace Upp;

// ------------------------- small utils -------------------------
static unsigned Hash32(const String& s) {
    unsigned h = 2166136261u;
    for(int i = 0; i < s.GetCount(); ++i) {
        h ^= (byte)s[i];
        h *= 16777619u;
    }
    return h;
}

static void EraseValue(Vector<int>& v, int x) {
    for(int i = 0; i < v.GetCount(); ++i)
        if(v[i] == x) { v.Remove(i); return; }
}

// ------------------------- ctor -------------------------
GalleryCtrl::GalleryCtrl()
{
    zoom_steps = { 32, 48, 64, 96, 128 };
    zoom_i     = 2; // 64

    AddFrame(sb);
    sb.WhenScroll = THISBACK(OnScroll);

    BackPaint();
    NoWantFocus();

    // stored defaults for forward features
    orientation    = Orientation::Vertical;
    thumb_aspect_w = 16;
    thumb_aspect_h = 9;

    Reflow();
    Refresh();
}

// ------------------------- public API -------------------------
int GalleryCtrl::Add(const String& name, const Image& opt_img, Color tint)
{
    GalleryItem it;
    it.name   = name;
    it.src    = opt_img;
    it.seed   = IsNull(tint) ? AutoColorFromText(name) : tint;
    it.status = IsNull(opt_img) ? ThumbStatus::Auto : ThumbStatus::Ok;
    items.Add(pick(it));

    Reflow();
    Refresh();
    return items.GetCount() - 1;
}

void GalleryCtrl::AddDummy(const String& name)
{
    Add(name, Image(), Null);
}

bool GalleryCtrl::SetThumbFromFile(int, const String&)
{
    // Keep this a stub to avoid decoder dependency here.
    // Load in the app via plugin/png/etc and call SetThumbImage(...) instead.
    return false;
}

void GalleryCtrl::SetThumbImage(int index, const Image& img)
{
    if(!IsValidIndex(index)) return;
    auto& it = items[index];
    it.src = img;
    it.thumb_normal = Null;
    it.thumb_gray   = Null;
    it.status = IsNull(img) ? ThumbStatus::Auto : ThumbStatus::Ok;
    Refresh();
}

void GalleryCtrl::ClearThumbImage(int index)
{
    if(!IsValidIndex(index)) return;
    auto& it = items[index];
    it.src = Image();
    it.thumb_normal = Null;
    it.thumb_gray   = Null;
    it.status = ThumbStatus::Auto;
    Refresh();
}

void GalleryCtrl::Clear()
{
    items.Clear();
    anchor_index = -1;
    Reflow();
    Refresh();
}

void GalleryCtrl::SetThumbStatus(int index, ThumbStatus s)
{
    if(!IsValidIndex(index)) return;
    items[index].status = s;
    Refresh();
}

void GalleryCtrl::SetDataFlags(int index, DataFlags f)
{
    if(!IsValidIndex(index)) return;
    items[index].flags = f;
    Refresh();
}

DataFlags GalleryCtrl::GetDataFlags(int index) const
{
    return IsValidIndex(index) ? items[index].flags : DF_None;
}

Vector<int> GalleryCtrl::GetSelection() const
{
    Vector<int> out;
    for(int i = 0; i < items.GetCount(); ++i)
        if(items[i].selected) out.Add(i);
    return out;
}

void GalleryCtrl::ClearSelection()
{
    for(auto& it : items) it.selected = false;
    anchor_index = -1;
    Refresh();
    if(WhenSelection) WhenSelection();
}

void GalleryCtrl::SetFiltered(int index, bool filtered_out)
{
    if(!IsValidIndex(index)) return;
    items[index].filtered_out = filtered_out;
    Refresh();
}

void GalleryCtrl::ClearFilterFlags()
{
    for(auto& it : items) it.filtered_out = false;
    Refresh();
}

void GalleryCtrl::SetZoomIndex(int zi)
{
    zi = clamp(zi, 0, zoom_steps.GetCount() - 1);
    if(zoom_i == zi) return;
    zoom_i = zi;
    for(auto& it : items) { it.thumb_normal = Null; it.thumb_gray = Null; }
    Reflow();
    Refresh();
    if(WhenZoom) WhenZoom(zoom_i);
}

void GalleryCtrl::SetAspectPolicy(AspectPolicy p)
{
    if(aspect_policy == p) return;
    aspect_policy = p;
    for(auto& it : items) { it.thumb_normal = Null; it.thumb_gray = Null; }
    Refresh();
}

// Programmer-friendly sizing facade
void GalleryCtrl::SetThumbSize(int px)
{
    if(zoom_steps.IsEmpty()) return;
    int best = 0, best_diff = INT_MAX;
    for(int i = 0; i < zoom_steps.GetCount(); ++i) {
        int d = abs(zoom_steps[i] - px);
        if(d < best_diff) { best = i; best_diff = d; }
    }
    SetZoomIndex(best);
}

int GalleryCtrl::GetThumbSize() const
{
    return zoom_steps.IsEmpty() ? 0 : zoom_steps[zoom_i];
}

void GalleryCtrl::SetThumbAspect(int w, int h)
{
    // stored only; current renderer uses square tiles
    thumb_aspect_w = max(1, w);
    thumb_aspect_h = max(1, h);
    Refresh();
}

void GalleryCtrl::SetStackSize(int px)
{
    labelH = max(0, px);
    Reflow();
    Refresh();
}

void GalleryCtrl::SetOrientation(Orientation o)
{
    if(orientation == o) return;
    orientation = o;
    // Baseline renderer is vertical grid; horizontal comes in the next step.
    Reflow();
    Refresh();
}

// ------------------------- geometry & helpers -------------------------
Rect GalleryCtrl::IndexRectNoScroll(int i) const
{
    int tile   = zoom_steps[zoom_i];
    int cell_w = pad + tile + pad;
    int cell_h = pad + tile + labelH + pad;
    int cc     = max(1, cols);
    int r = i / cc;
    int c = i % cc;
    int x = pad + c * cell_w;
    int y = pad + r * cell_h;
    return RectC(x, y, tile, tile + labelH);
}

Rect GalleryCtrl::IndexRect(int i) const
{
    Rect r = IndexRectNoScroll(i);
    r.Offset(-scroll_x, -scroll_y);
    return r;
}

GalleryItem* GalleryCtrl::TryItem(int i)
{
    return IsValidIndex(i) ? &items[i] : nullptr;
}

int GalleryCtrl::IndexFromPoint(Point p) const
{
    const int tile   = zoom_steps[zoom_i];
    const int cell_w = pad + tile + pad;
    const int cell_h = pad + tile + labelH + pad;

    const int cx = p.x + scroll_x;
    const int cy = p.y + scroll_y;

    if(cx < pad || cy < pad) return -1;

    const int col = (cx - pad) / cell_w;
    const int row = (cy - pad) / cell_h;

    if(col < 0 || row < 0 || col >= max(1, cols)) return -1;

    const int i = row * max(1, cols) + col;
    return IsValidIndex(i) ? i : -1;
}

void GalleryCtrl::Reflow()
{
    const Size sz = GetSize();
    const int tile   = zoom_steps[zoom_i];
    const int cell_w = pad + tile + pad;
    const int cell_h = pad + tile + labelH + pad;

    cols = max(1, sz.cx / cell_w);
    const int rows = items.IsEmpty() ? 0 : (items.GetCount() + cols - 1) / cols;

    content_w = cols * cell_w + pad;
    content_h = rows * cell_h + pad;

    // ScrollBars reserved for future hookup (currently using scroll_x/y).
    Refresh();
}

// ------------------------- aspect helpers & gray -------------------------
Size GalleryCtrl::FitSize(Size src, int tile) const
{
    if(src.cx == 0 || src.cy == 0) return Size(tile, tile);
    double rx = double(tile) / src.cx;
    double ry = double(tile) / src.cy;
    double s  = min(rx, ry);
    return Size(int(src.cx * s + 0.5), int(src.cy * s + 0.5));
}

Size GalleryCtrl::FillSize(Size src, int tile) const
{
    if(src.cx == 0 || src.cy == 0) return Size(tile, tile);
    double rx = double(tile) / src.cx;
    double ry = double(tile) / src.cy;
    double s  = max(rx, ry);
    return Size(int(src.cx * s + 0.5), int(src.cy * s + 0.5));
}

Image GalleryCtrl::MakeGray(const Image& in) const
{
    if(IsNull(in)) return in;
    ImageBuffer ib(in.GetSize());
    for(int y = 0; y < in.GetHeight(); ++y) {
        const RGBA* s = in[y];
        RGBA*       d = ib[y];
        for(int x = 0; x < in.GetWidth(); ++x) {
            int v = (77 * s[x].r + 150 * s[x].g + 29 * s[x].b) >> 8; // luminance
            d[x].r = d[x].g = d[x].b = (byte)v;
            d[x].a = s[x].a;
        }
    }
    return Image(ib);
}

Image GalleryCtrl::MakeFillCropped(const Image& src, int tile, bool gray) const
{
    if(IsNull(src)) return Image();
    Size fsz = FillSize(src.GetSize(), tile);

    Image scaled = Rescale(src, fsz);
    if(gray) scaled = MakeGray(scaled);

    // Center-crop to tile
    int sx = max(0, (scaled.GetWidth()  - tile) / 2);
    int sy = max(0, (scaled.GetHeight() - tile) / 2);
    return Crop(scaled, RectC(sx, sy, tile, tile));
}

// ------------------------- glyphs -------------------------
void GalleryCtrl::StrokeRect(Draw& w, const Rect& r, int t, const Color& c)
{
    Rect rr = r;
    for(int i = 0; i < t; ++i) { w.DrawRect(rr, c); rr.Deflate(1); }
}

static Image MakeGlyphGeneric(int tile, void (*fn)(BufferPainter&, int, bool), bool gray)
{
    ImageBuffer ib(Size(tile, tile));
    BufferPainter p;
    p.Create(ib, MODE_ANTIALIASED);
    p.Clear(GrayColor(240));
    fn(p, tile, gray);
    p.Finish();
    return Image(ib);
}

void GalleryCtrl::DrawPlaceholderGlyph(BufferPainter& p, int tile, bool gray)
{
    Color box  = gray ? GrayColor(150) : SColorDisabled();
    Color plus = gray ? GrayColor(100) : SColorText();

    // dashed border
    for(int k = 0; k < tile; ++k) {
        if(k % 4 < 2) {
            p.Rectangle(k, 0, 1, tile).Fill(box);
            p.Rectangle(0, k, tile, 1).Fill(box);
            p.Rectangle(tile-1, k, 1, tile).Fill(box);
            p.Rectangle(k, tile-1, tile, 1).Fill(box);
        }
    }
    // plus
    int m = tile / 2;
    int w = max(1, tile / 10);
    p.Rectangle(m - w, tile/4, 2*w+1, tile/2).Fill(plus);
    p.Rectangle(tile/4, m - w, tile/2, 2*w+1).Fill(plus);
}

void GalleryCtrl::DrawMissingGlyph(BufferPainter& p, int tile, bool gray)
{
    Color frame = gray ? GrayColor(90) : Color(180, 40, 40);
    Color excl  = gray ? GrayColor(110) : Color(220, 60, 60);
    int t = max(2, tile / 16);
    // frame
    p.Rectangle(0, 0, tile, t).Fill(frame);
    p.Rectangle(0, tile - t, tile, t).Fill(frame);
    p.Rectangle(0, 0, t, tile).Fill(frame);
    p.Rectangle(tile - t, 0, t, tile).Fill(frame);
    // exclamation
    int m = tile / 2;
    int w = max(2, tile / 12);
    p.Rectangle(m - w, tile/4, 2*w, tile/2).Fill(excl);
    p.Rectangle(m - w, tile - tile/6, 2*w, w).Fill(excl);
}

void GalleryCtrl::DrawErrorGlyph(BufferPainter& p, int tile, bool gray)
{
    Color frame = gray ? GrayColor(90) : Color(180, 40, 40);
    Color err   = gray ? GrayColor(110) : Color(220, 60, 60);
    int t = max(2, tile / 16);

    // frame
    p.Rectangle(0, 0, tile, t).Fill(frame);
    p.Rectangle(0, tile - t, tile, t).Fill(frame);
    p.Rectangle(0, 0, t, tile).Fill(frame);
    p.Rectangle(tile - t, 0, t, tile).Fill(frame);

    // X
    for (int k = 0; k < tile; ++k) {
        p.Rectangle(k, k, 1, 1).Fill(err);
        p.Rectangle(tile - k - 1, k, 1, 1).Fill(err);
    }
}

Image GalleryCtrl::MakePlaceholderGlyph(int tile, bool gray)
{
    return MakeGlyphGeneric(tile, &GalleryCtrl::DrawPlaceholderGlyph, gray);
}
Image GalleryCtrl::MakeMissingGlyph(int tile, bool gray)
{
    return MakeGlyphGeneric(tile, &GalleryCtrl::DrawMissingGlyph, gray);
}
Image GalleryCtrl::MakeErrorGlyph(int tile, bool gray)
{
    return MakeGlyphGeneric(tile, &GalleryCtrl::DrawErrorGlyph, gray);
}

const Image& GalleryCtrl::PlaceholderGlyph(int tile)
{
    static VectorMap<int, Image> cache, cache_gray;
    int ii = cache.Find(tile);
    if(ii < 0) {
        cache.Add(tile, MakePlaceholderGlyph(tile, false));
        cache_gray.Add(tile, MakePlaceholderGlyph(tile, true));
        ii = cache.Find(tile);
    }
    return cache[ii];
}

const Image& GalleryCtrl::MissingGlyph(int tile)
{
    static VectorMap<int, Image> cache, cache_gray;
    int ii = cache.Find(tile);
    if(ii < 0) {
        cache.Add(tile, MakeMissingGlyph(tile, false));
        cache_gray.Add(tile, MakeMissingGlyph(tile, true));
        ii = cache.Find(tile);
    }
    return cache[ii];
}

const Image& GalleryCtrl::ErrorGlyph(int tile)
{
    static VectorMap<int, Image> cache, cache_gray;
    int ii = cache.Find(tile);
    if(ii < 0) {
        cache.Add(tile, MakeErrorGlyph(tile, false));
        cache_gray.Add(tile, MakeErrorGlyph(tile, true));
        ii = cache.Find(tile);
    }
    return cache[ii];
}

// ------------------------- colors -------------------------
Color GalleryCtrl::AutoColorFromText(const String& s) const
{
    unsigned h = Hash32(s);
    // Map to safe ranges, then normalize to [0..1] for HsvColorf
    int H8 = (h & 0xFF);                   // 0..255
    int S8 = 120 + int((h >> 8)  & 0x3F);  // 120..183
    int V8 = 170 + int((h >> 14) & 0x3F);  // 170..233
    return HsvColorf(H8 / 255.0, S8 / 255.0, V8 / 255.0);
}

// ------------------------- rendering -------------------------
void GalleryCtrl::EnsureThumbs(GalleryItem& it)
{
    if(!IsNull(it.thumb_normal)) return;

    const int tile = zoom_steps[zoom_i];
    if(IsNull(it.src)) {
        it.thumb_normal = MakePlaceholderGlyph(tile, false);
        it.thumb_gray   = MakePlaceholderGlyph(tile, true);
        return;
    }

    switch(aspect_policy) {
    case AspectPolicy::Fit:
        it.thumb_normal = Rescale(it.src, FitSize(it.src.GetSize(), tile));
        break;
    case AspectPolicy::Fill:
        it.thumb_normal = MakeFillCropped(it.src, tile, false);
        break;
    case AspectPolicy::Stretch:
        it.thumb_normal = Rescale(it.src, Size(tile, tile));
        break;
    }

    it.thumb_gray = MakeGray(it.thumb_normal);
}

void GalleryCtrl::Layout()
{
    Reflow();    // recompute cols/rows/content when size changes
}

void GalleryCtrl::Paint(Draw& w)
{
    const Size sz = GetSize();
    w.DrawRect(sz, SColorFace());

    if(items.IsEmpty()) return;

    const int tile   = zoom_steps[zoom_i];
    const int cell_w = pad + tile + pad;
    const int cell_h = pad + tile + labelH + pad;
    const int cc     = max(1, cols);
    const int rows   = (items.GetCount() + cc - 1) / cc;

    const int y0 = max(0, (scroll_y) / cell_h);
    const int y1 = min(max(0, rows - 1), (scroll_y + sz.cy) / cell_h);

    // visible tiles
    for(int r = y0; r <= y1; ++r) {
        for(int c = 0; c < cc; ++c) {
            const int i = r * cc + c;
            if(i >= items.GetCount()) break;

            Rect rc     = IndexRect(i);
            Rect img_rc = RectC(rc.left, rc.top, tile, tile);

            // hover tint
            if(hover_enabled && i == hover_index)
                w.DrawRect(img_rc, Blend(SColorFace(), SColorHighlight(), 32));

            // image
            auto& it = items[i];
            EnsureThumbs(it);

            Image to_draw;
            if(it.status == ThumbStatus::Missing)
                to_draw = MissingGlyph(tile);
            else if(it.status == ThumbStatus::Error)
                to_draw = ErrorGlyph(tile);
            else if(IsNull(it.src) && it.status == ThumbStatus::Auto)
                to_draw = PlaceholderGlyph(tile);
            else
                to_draw = it.filtered_out && saturation_on ? it.thumb_gray : it.thumb_normal;

            Size isz = to_draw.GetSize();
            Point ip = img_rc.CenterPos(isz);
            w.DrawImage(RectC(ip.x, ip.y, isz.cx, isz.cy), to_draw);

            // label backdrop (simulated alpha)
            Rect lab = RectC(rc.left, rc.top + tile, tile, labelH);
            Color lb = Blend(SColorLtFace(), SColorPaper(), 255 - label_backdrop_alpha);
            w.DrawRect(lab, lb);

            // label text
            w.DrawText(lab.left + 4, lab.top + (lab.GetHeight() - Draw::GetStdFontCy())/2,
                       it.name, StdFont(), SColorText());

            // data-flag dot
            if(it.flags != DF_None) {
                int d = max(4, tile / 10);
                Rect dot = RectC(lab.right - d - 4, lab.top + (lab.GetHeight() - d)/2, d, d);
                w.DrawRect(dot, Color(255, 140, 0));
            }

            // selection ring
            if(show_selection_border && it.selected) {
                StrokeRect(w, img_rc, max(2, tile/18), SColorHighlight());
            }

            // filtered border (subtle)
            if(show_filter_border && it.filtered_out) {
                StrokeRect(w, img_rc, 1, SColorPaper());
            }
        }
    }

    // marquee
	if(dragging && !drag_rect_win.IsEmpty()) {
	    Rect r = drag_rect_win;
	    r.Offset(-scroll_x, -scroll_y);
	    // Draw has no alpha → simulate translucency against face color.
	    Color fill = Blend(SColorFace(), SColorHighlight(), 64); // subtle tint
	    w.DrawRect(r, fill);
	    StrokeRect(w, r, 1, SColorHighlight());                  // 1px ring
	}

}

// ------------------------- input -------------------------
void GalleryCtrl::LeftDown(Point p, dword flags)
{
    SetCapture();

    int i = IndexFromPoint(p);
    bool ctrl  = flags & K_CTRL;
    bool shift = flags & K_SHIFT;

    if(i < 0) {
        // Start marquee on empty space
        mouse_down      = true;
        dragging        = false;
        drag_additive   = ctrl;
        drag_origin_win = p;
        drag_rect_win   = Rect(p, p);
        drag_prev_sel   = GetSelection();
        return;
    }

    auto* it = TryItem(i);
    if(!it) return;

    Vector<int> next = GetSelection();

    if(shift && anchor_index >= 0) {
        int a = anchor_index, b = i;
        if(a > b) Swap(a, b);
        next.Clear();
        for(int k = a; k <= b; ++k) next.Add(k);
    }
    else if(ctrl) {
        if(it->selected) EraseValue(next, i);
        else             next.Add(i);
        anchor_index = i;
    }
    else {
        next.Clear();
        next.Add(i);
        anchor_index = i;
    }

    CommitSelection(next);
}

void GalleryCtrl::LeftDouble(Point p, dword)
{
    int i = IndexFromPoint(p);
    if(i >= 0 && WhenActivate)
        WhenActivate(items[i]);
}

void GalleryCtrl::LeftUp(Point, dword)
{
    ReleaseCapture();
    if(dragging) {
        ApplyMarqueeSelection();
        dragging = false;
        Refresh();
    }
    mouse_down = false;
}

void GalleryCtrl::RightDown(Point p, dword)
{
    ShowContextMenu(p);
}

void GalleryCtrl::MouseMove(Point p, dword flags)
{
    if(mouse_down) {
        // marquee drag
        dragging = true;
        drag_rect_win = Rect(min(p.x, drag_origin_win.x),
                             min(p.y, drag_origin_win.y),
                             max(p.x, drag_origin_win.x),
                             max(p.y, drag_origin_win.y));
        Refresh();
    }
    else {
        int ni = IndexFromPoint(p);
        if(hover_enabled && ni != hover_index) {
            hover_index = ni; // may be -1
            Refresh();
            if(WhenHover) WhenHover(hover_index);
        }
    }
}

bool GalleryCtrl::Key(dword key, int)
{
    const int tile   = zoom_steps[zoom_i];
    const int step_y = tile + labelH + pad*2;
    const int page_y = max(1, GetSize().cy - step_y);

    if(key == K_PAGEUP)   { scroll_y = max(0, scroll_y - page_y); Refresh(); return true; }
    if(key == K_PAGEDOWN) { scroll_y = min(max(0, content_h - GetSize().cy), scroll_y + page_y); Refresh(); return true; }
    if(key == K_HOME)     { scroll_y = 0; Refresh(); return true; }
    if(key == K_END)      { scroll_y = max(0, content_h - GetSize().cy); Refresh(); return true; }

    return false;
}

void GalleryCtrl::MouseWheel(Point, int zdelta, dword keyflags)
{
    if(keyflags & K_CTRL) {
        SetZoomIndex(zoom_i + (zdelta > 0 ? +1 : -1));
        return;
    }
    int page = max(8, GetSize().cy / 8);
    scroll_y -= sgn(zdelta) * page;
    scroll_y = clamp(scroll_y, 0, max(0, content_h - GetSize().cy));
    Refresh();
}

void GalleryCtrl::OnScroll()
{
    // Reserved for future ScrollBars hookup.
    Refresh();
}

// ------------------------- selection helpers -------------------------
void GalleryCtrl::SelectRange(int a, int b, bool additive)
{
    if(items.IsEmpty()) return;
    a = clamp(a, 0, items.GetCount()-1);
    b = clamp(b, 0, items.GetCount()-1);
    if(a > b) Swap(a, b);

    if(!additive)
        for(auto& it : items) it.selected = false;

    for(int i = a; i <= b; ++i) items[i].selected = true;
    Refresh();
    if(WhenSelection) WhenSelection();
}

bool GalleryCtrl::CommitSelection(const Vector<int>& next)
{
    Vector<int> before = GetSelection();
    Vector<int> after; after <<= next; // deep copy

    Sort(before);
    Sort(after);

    Vector<int> unique;
    for(int i = 0; i < after.GetCount(); ++i) {
        int v = after[i];
        if(IsValidIndex(v) && (unique.IsEmpty() || unique.Top() != v))
            unique.Add(v);
    }

    bool changed = (before.GetCount() != unique.GetCount());
    if(!changed)
        for(int i = 0; i < before.GetCount(); ++i)
            if(before[i] != unique[i]) { changed = true; break; }
    if(!changed) return false;

    for(auto& it : items) it.selected = false;
    for(int v : unique) items[v].selected = true;

    Refresh();
    if(WhenSelection) WhenSelection();
    return true;
}

void GalleryCtrl::ApplyMarqueeSelection()
{
    if(drag_rect_win.IsEmpty())
        return;

    Rect selrc = drag_rect_win.Offseted(scroll_x, scroll_y); // to content space
    const int tile   = zoom_steps[zoom_i];
    const int cell_w = pad + tile + pad;
    const int cell_h = pad + tile + labelH + pad;

    Vector<int> merge;
    if(drag_additive)  merge <<= drag_prev_sel;

    int first_r = max(0, (selrc.top  - pad) / cell_h);
    int last_r  = (selrc.bottom - pad) / cell_h;
    int first_c = max(0, (selrc.left - pad) / cell_w);
    int last_c  = (selrc.right - pad) / cell_w;

    for(int r = first_r; r <= last_r; ++r) {
        for(int c = first_c; c <= last_c; ++c) {
            int i = r * max(1, cols) + c;
            if(!IsValidIndex(i)) continue;
            Rect rc = IndexRectNoScroll(i);
            if(rc.Intersects(selrc))
                merge.Add(i);
        }
    }
    CommitSelection(merge);
}

void GalleryCtrl::DoSelectAll()
{
    Vector<int> all;
    all.SetCount(items.GetCount());
    for(int i = 0; i < items.GetCount(); ++i) all[i] = i;
    CommitSelection(all);
}

void GalleryCtrl::DoInvertSelection()
{
    Vector<int> inv;
    for(int i = 0; i < items.GetCount(); ++i)
        if(!items[i].selected) inv.Add(i);
    CommitSelection(inv);
}

void GalleryCtrl::DoClearSelection()
{
    ClearSelection();
}

void GalleryCtrl::DoRemoveSelected()
{
    Vector<int> sel = GetSelection();
    Sort(sel);
    for(int i = sel.GetCount()-1; i >= 0; --i)
        items.Remove(sel[i]);
    anchor_index = -1;
    Reflow();
    Refresh();
    if(WhenSelection) WhenSelection();
}

void GalleryCtrl::DoRemoveAll()
{
    items.Clear();
    anchor_index = -1;
    Reflow();
    Refresh();
    if(WhenSelection) WhenSelection();
}

// ------------------------- context menu -------------------------
void GalleryCtrl::ShowContextMenu(Point p)
{
    auto build = [=](Bar& bar) {
        bar.Add(t_("Select all"),       THISBACK(DoSelectAll));
        bar.Add(t_("Invert selection"), THISBACK(DoInvertSelection));
        bar.Add(t_("Clear selection"),  THISBACK(DoClearSelection));
        bar.Separator(); // correct API

        bool hasSel = !GetSelection().IsEmpty();
        bar.Add(t_("Remove selected"),  THISBACK(DoRemoveSelected)).Enable(hasSel);
        bar.Add(t_("Remove all"),       THISBACK(DoRemoveAll)).Enable(!items.IsEmpty());

        if(WhenBar) WhenBar(bar);
    };

    MenuBar::Execute(build, p); // static overload (Event<Bar&>, Point)
}
