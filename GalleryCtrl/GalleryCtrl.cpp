#include "GalleryCtrl.h"

namespace Upp {

// ==== utilities ==============================================================
static inline int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Safe, alpha-free color mixer: result = a*(255-t) + b*t  (t in 0..255)
static inline Color Mix(Color a, Color b, int t /*0..255*/)
{
    t = ClampInt(t, 0, 255);
    const int u = 255 - t;
    return Color( (a.GetR()*u + b.GetR()*t) / 255,
                  (a.GetG()*u + b.GetG()*t) / 255,
                  (a.GetB()*u + b.GetB()*t) / 255 );
}

const int* GalleryCtrl::ZoomSteps() {
    static const int z[] = { 32, 48, 64, 96, 128 };
    return z;
}
int GalleryCtrl::ZoomStepCount() { return 5; }

Rect GalleryCtrl::NormalizeRect(Rect r)
{
    if(r.left > r.right)   Swap(r.left, r.right);
    if(r.top  > r.bottom)  Swap(r.top,  r.bottom);
    return r;
}

static inline Color Hsv01(double h01, double s, double v) {
    // clamp to [0,1) to satisfy HSVtoRGB logic
    if(h01 >= 1.0) h01 = std::fmod(h01, 1.0);
    if(h01 < 0.0)  h01 = 0.0;
    if(s   < 0.0)  s   = 0.0; if(s > 1.0) s = 1.0;
    if(v   < 0.0)  v   = 0.0; if(v > 1.0) v = 1.0;
    return HsvColorf(h01, s, v);
}

// simple vector helpers (Vector<int> has no Find)
static inline bool ContainsIdx(const Vector<int>& v, int x) {
    for(int i = 0; i < v.GetCount(); ++i) if(v[i] == x) return true;
    return false;
}

static inline void RemoveOne(Vector<int>& v, int x) {
    for(int i = 0; i < v.GetCount(); ++i) if(v[i] == x) { v.Remove(i); return; }
}

// Simple stroke for Draw
static void StrokeRect(Draw& w, const Rect& r, int pen, Color c)
{
    w.DrawRect(r.left, r.top, r.GetWidth(), pen, c);
    w.DrawRect(r.left, r.bottom - pen, r.GetWidth(), pen, c);
    w.DrawRect(r.left, r.top + pen, pen, r.GetHeight() - 2 * pen, c);
    w.DrawRect(r.right - pen, r.top + pen, pen, r.GetHeight() - 2 * pen, c);
}

// Build a premultiplied-alpha swatch and draw it with DrawImage().
// Alpha ~26 ≈ 10% tint. Works with Draw (no Painter needed).
static Image MakeAlphaOverlay(Size sz, Color c, int alpha /*0..255*/)
{
    alpha = ClampInt(alpha, 0, 255);

    ImageBuffer ib(sz);
    ib.SetKind(IMAGE_ALPHA);            // mark as alpha image

    const int r_p = (c.GetR() * alpha + 127) / 255; // premultiply in-place
    const int g_p = (c.GetG() * alpha + 127) / 255;
    const int b_p = (c.GetB() * alpha + 127) / 255;
    const byte a  = (byte)alpha;

    for (int y = 0; y < sz.cy; ++y) {
        RGBA* row = ib[y];
        for (int x = 0; x < sz.cx; ++x) {
            row[x].r = (byte)r_p;
            row[x].g = (byte)g_p;
            row[x].b = (byte)b_p;
            row[x].a = a;
        }
    }
    return ib; // already premultiplied
}


static Image ToGray(const Image& in)
{
    if(in.IsEmpty()) return Image();
    Size sz = in.GetSize();
    ImageBuffer ib(sz);
    const RGBA* s = in;
    for(int y = 0; y < sz.cy; ++y) {
        RGBA* t = ib[y];
        for(int x = 0; x < sz.cx; ++x) {
            byte g = (byte)(s->r * 0.2126 + s->g * 0.7152 + s->b * 0.0722);
            t->r = t->g = t->b = g;
            t->a = s->a;
            ++s; ++t;
        }
    }
    return ib;
}

// ==== ctor ===================================================================
GalleryCtrl::GalleryCtrl()
{
    AddFrame(sb);
    sb.WhenScroll = [&]{
        scroll_x = sb.GetX();
        scroll_y = sb.GetY();
        Refresh();
    };
    NoWantFocus();
    Reflow();
}

// ==== public API =============================================================
int GalleryCtrl::Add(const String& name, const Image& opt_img, Color)
{
    GalleryItem it;
    it.name = name;
    it.thumb = opt_img;
    it.seed = (int)GetHashValue(name);
    items.Add(pick(it));
    Reflow();
    Refresh();
    return items.GetCount() - 1;
}


void GalleryCtrl::AddDummy(const String& name)
{
    Add(name);
}


bool GalleryCtrl::SetThumbFromFile(int index, const String& filepath)
{
    if(index < 0 || index >= items.GetCount()) return false;
    Image img = StreamRaster::LoadFileAny(filepath);
    if(!img.IsEmpty()) {
        items[index].thumb = img;
        items[index].thumb_gray = Image();
        Refresh();
        return true;
    }
    return false;
}


void GalleryCtrl::SetThumbImage(int index, const Image& img)
{
    if(index < 0 || index >= items.GetCount()) return;
    items[index].thumb = img;
    items[index].thumb_gray = Image();
    Refresh();
}


void GalleryCtrl::ClearThumbImage(int index)
{
    if(index < 0 || index >= items.GetCount()) return;
    items[index].thumb = Image();
    items[index].thumb_gray = Image();
    Refresh();
}


void GalleryCtrl::SetThumbStatus(int index, ThumbStatus s)
{
    if(auto* it = TryItem(index)) { it->status = s; Refresh(); }
}


void GalleryCtrl::SetDataFlags(int index, DataFlags f)
{
    if(auto* it = TryItem(index)) { it->flags = f; Refresh(); }
}

DataFlags GalleryCtrl::GetDataFlags(int index) const
{
    const auto* it = TryItem(index);
    return it ? it->flags : DF_None;
}

Vector<int> GalleryCtrl::GetSelection() const
{
    Vector<int> v;
    for(int i = 0; i < items.GetCount(); ++i)
        if(items[i].selected)
            v.Add(i);
    return v;
}

void GalleryCtrl::ClearSelection()
{
    for(auto& it : items) it.selected = false;
    WhenSelection();
    Refresh();
}

void GalleryCtrl::SetFiltered(int index, bool filtered_out)
{
    if(auto* it = TryItem(index)) { it->filtered_out = filtered_out; Refresh(); }
}

void GalleryCtrl::ClearFilterFlags()
{
    for(auto& it : items) it.filtered_out = false;
    Refresh();
}

void GalleryCtrl::SetZoomIndex(int zi)
{
    zi = ClampInt(zi, 0, ZoomStepCount() - 1);
    if(zoom_i == zi) return;
    zoom_i = zi;
    for(auto& it : items) it.thumb_gray = Image();
    Reflow();
    Refresh();
    WhenZoom(zoom_i);
}

void GalleryCtrl::SetAspectPolicy(AspectPolicy p)
{
    if(aspect == p) return;
    aspect = p;
    Refresh();
}

void GalleryCtrl::SetShowSelectionBorders(bool b) { show_sel_ring = b; Refresh(); }
void GalleryCtrl::SetShowFilterBorders(bool b)    { show_filter_ring = b; Refresh(); }
void GalleryCtrl::SetSaturationOn(bool b)         { saturation_on = b; Refresh(); }
void GalleryCtrl::SetHoverEnabled(bool b)         { hover_enabled = b; if(!b) hover_index = -1; Refresh(); }

void GalleryCtrl::SetLabelBackdropAlpha(int a)
{
    a = ClampInt(a, 0, 255);
    if(label_backdrop_alpha == a) return;
    label_backdrop_alpha = a;
    Refresh();
}

void GalleryCtrl::SetScrollMode(ScrollMode m)
{
    if(scroll_mode == m) return;
    scroll_mode = m;
    Reflow();
    Refresh();
}

void GalleryCtrl::SetTilePadding(int px)
{
    px = ClampInt(px, 0, 64);
    if(pad == px) return;
    pad = px;
    Reflow();
    Refresh();
}

void GalleryCtrl::Clear()
{
    items.Clear();
    hover_index = anchor_index = caret_index = -1;
    scroll_x = scroll_y = 0;
    Reflow();
    Refresh();
}

// ==== layout / hit test ======================================================
void GalleryCtrl::Layout()
{
    Reflow();
}

void GalleryCtrl::Reflow()
{
    Size sz = GetSize();
    const int tile = ZoomSteps()[zoom_i];
    const int tw = tile;
    const int th = tile + label_h;

    cols = max(1, (sz.cx + pad) / (tw + pad));
    rows = items.GetCount() ? ( (items.GetCount() + cols - 1) / cols ) : 0;

    content_w = cols * (tw + pad) + pad;
    content_h = rows * (th + pad) + pad;

    scroll_x = ClampInt(scroll_x, 0, max(0, content_w - sz.cx));
    scroll_y = ClampInt(scroll_y, 0, max(0, content_h - sz.cy));

    // scroll mode framing
    Size page = sz;
    Size total(content_w, content_h);

    switch(scroll_mode) {
    case ScrollMode::VerticalOnly:
        total.cx = page.cx;
        scroll_x = 0;
        break;
    case ScrollMode::HorizontalOnly:
        total.cy = page.cy;
        scroll_y = 0;
        break;
    case ScrollMode::None:
        total = page;
        scroll_x = scroll_y = 0;
        break;
    case ScrollMode::Auto:
    default: break;
    }

    sb.Set(Point(scroll_x, scroll_y), page, total);
}

Rect GalleryCtrl::TileRect(int index) const
{
    if(index < 0 || index >= items.GetCount()) return Rect(0,0,0,0);
    const int tile = ZoomSteps()[zoom_i];
    const int tw = tile;
    const int th = tile + label_h;

    int r = index / cols;
    int c = index % cols;

    int x = pad + c * (tw + pad);
    int y = pad + r * (th + pad);
    return RectC(x, y, tw, th);
}

Rect GalleryCtrl::ImageRect(const Rect& tile) const
{
    Rect r = tile;
    r.bottom -= label_h;
    return r;
}

int GalleryCtrl::IndexFromPoint(Point content_pt) const
{
    for(int i = 0; i < items.GetCount(); ++i) {
        Rect tr = TileRect(i);
        if(tr.Contains(content_pt))
            return i;
    }
    return -1;
}

// ==== selection helpers ======================================================
void GalleryCtrl::CommitSelection(const Vector<int>& indices)
{
    // build sorted copy of indices (manual copy to avoid deleted copy-ctor pitfalls)
    Vector<int> in;
    in.Reserve(indices.GetCount());
    for(int v : indices) in.Add(v);
    Sort(in);

    // pre-veto (WhenSelecting Gate)
    if(WhenSelecting && !WhenSelecting(in))
        return;

    for(int i = 0; i < items.GetCount(); ++i)
        items[i].selected = false;
    for(int v : in)
        if(v >= 0 && v < items.GetCount())
            items[v].selected = true;

    WhenSelection();
    Refresh();
}

Vector<int> GalleryCtrl::IndicesInRect(const Rect& rc) const
{
    Vector<int> out;
    for(int i = 0; i < items.GetCount(); ++i)
        if(TileRect(i).Intersects(rc))
            out.Add(i);
    return out;
}

// ==== events / interaction ===================================================
void GalleryCtrl::MouseWheel(Point, int zdelta, dword keyflags)
{
    if(keyflags & K_CTRL) {
        SetZoomIndex(zoom_i + (zdelta > 0 ? +1 : -1));
        return;
    }

    // Simple per-wheel step
    const int th = ZoomSteps()[zoom_i] + label_h + pad;
    const int step = max(8, th / 3);
    int dir = (zdelta > 0) ? -1 : +1;

    if((keyflags & K_SHIFT) == K_SHIFT) {
        if(scroll_mode != ScrollMode::VerticalOnly && scroll_mode != ScrollMode::None)
            sb.SetX(ClampInt(sb.GetX() + dir * step, 0, max(0, content_w - GetSize().cx)));
    } else {
        if(scroll_mode != ScrollMode::HorizontalOnly && scroll_mode != ScrollMode::None)
            sb.SetY(ClampInt(sb.GetY() + dir * step, 0, max(0, content_h - GetSize().cy)));
    }

    scroll_x = sb.GetX();
    scroll_y = sb.GetY();
    Refresh();
}

bool GalleryCtrl::Key(dword key, int)
{
    // Delegate PageUp/Down, Home/End, Arrow to ScrollBars
    if(sb.Key(key)) {
        scroll_x = sb.GetX();
        scroll_y = sb.GetY();
        Refresh();
        return true;
    }
    return false;
}

void GalleryCtrl::MouseLeave()
{
    if(hover_enabled && hover_index >= 0) {
        hover_index = -1;
        WhenHover(-1);
        Refresh();
    }
}

void GalleryCtrl::MouseMove(Point p, dword flags)
{
    // Adopt external drag (entered control with LMB already down)
    if(!HasCapture() && !mouse_down && GetMouseLeft()) {
        SetCapture();
        mouse_down        = true;
        dragging          = false;

        // Initialize marquee mode from current keys
        const bool ctrl  = (flags & K_CTRL)  != 0;
        const bool shift = (flags & K_SHIFT) != 0;
        const bool alt   = (flags & K_ALT)   != 0;
        const bool ctrl_only = ctrl && !shift && !alt;

        drag_intersect   = (ctrl && alt);
        drag_subtractive = (!drag_intersect && alt);
        drag_xor         = (!drag_intersect && !drag_subtractive && ctrl_marquee_xor && ctrl_only);
        drag_additive    = (!drag_intersect && !drag_subtractive && !drag_xor && (shift || ctrl));

        drag_origin_win  = p;
        drag_rect_win    = Rect(p, p);

        // Baseline
        drag_prev_sel.Clear();
        const auto cur = GetSelection();
        drag_prev_sel.Reserve(cur.GetCount());
        for(int k = 0; k < cur.GetCount(); ++k)
            drag_prev_sel.Add(cur[k]);

        // When adopted mid-drag, any pending click is irrelevant
        pending_click = false;
        pending_index = -1;
        pending_flags = 0;
    }

    // Hover when not dragging
    if(!mouse_down) {
        const int hi = IndexFromPoint(p + Point(scroll_x, scroll_y));
        if(hover_enabled && hi != hover_index) {
            hover_index = hi;
            if(WhenHover) WhenHover(hover_index);
            Refresh();
        }
        return;
    }

    // Rubber-band update (normalize + small hysteresis)
    drag_rect_win = Rect(min(drag_origin_win.x, p.x),
                         min(drag_origin_win.y, p.y),
                         max(drag_origin_win.x, p.x) + 1,
                         max(drag_origin_win.y, p.y) + 1);

    if(!dragging && (abs(p.x - drag_origin_win.x) > 2 || abs(p.y - drag_origin_win.y) > 2)) {
        dragging = true;
        // Once we cross the threshold, a pending click is canceled
        pending_click = false;
    }

    if(dragging) {
        // Allow mid-drag modifier changes
        const bool k_ctrl  = (flags & K_CTRL)  != 0;
        const bool k_shift = (flags & K_SHIFT) != 0;
        const bool k_alt   = (flags & K_ALT)   != 0;
        const bool k_ctrl_only = k_ctrl && !k_shift && !k_alt;

        const bool want_inter = k_ctrl && k_alt;
        const bool want_sub   = (!want_inter && k_alt);
        const bool want_xor   = (!want_inter && !want_sub && ctrl_marquee_xor && k_ctrl_only);
        const bool want_add   = (!want_inter && !want_sub && !want_xor && (k_shift || k_ctrl));

        // Precedence: Intersect > Subtract > XOR > Add > Replace
        const bool inter = drag_intersect   || want_inter;
        const bool sub   = !inter && (drag_subtractive || want_sub);
        const bool xr    = !inter && !sub && (drag_xor         || want_xor);
        const bool add   = !inter && !sub && !xr && (drag_additive    || want_add);

        ApplyMarqueeSelection(add, sub, inter, xr);
        Refresh(); // live feedback
    }
}


void GalleryCtrl::LeftDown(Point p, dword flags)
{
    SetCapture();

    const bool ctrl  = (flags & K_CTRL)  != 0;
    const bool shift = (flags & K_SHIFT) != 0;
    const bool alt   = (flags & K_ALT)   != 0;

    const Point ip = p + Point(scroll_x, scroll_y);
    const int   i  = IndexFromPoint(ip);

    mouse_down = true;
    dragging   = false;

    // Compute marquee mode from modifiers
    const bool ctrl_only = ctrl && !shift && !alt;
    drag_intersect   = (ctrl && alt);                                           // Ctrl+Alt
    drag_subtractive = (!drag_intersect && alt);                                // Alt
    drag_xor         = (!drag_intersect && !drag_subtractive && ctrl_marquee_xor && ctrl_only); // Ctrl-only XOR
    drag_additive    = (!drag_intersect && !drag_subtractive && !drag_xor && (shift || ctrl));  // Shift or Ctrl

    drag_origin_win  = p;
    drag_rect_win    = Rect(p, p);

    // Baseline from current selection (for any marquee mode)
    drag_prev_sel.Clear();
    const auto cur = GetSelection();
    drag_prev_sel.Reserve(cur.GetCount());
    for(int k = 0; k < cur.GetCount(); ++k)
        drag_prev_sel.Add(cur[k]);

    if(i < 0) {
        // Clicked whitespace: immediate clear only if NO modifiers (replace intent)
        pending_click = false;
        pending_index = -1;
        pending_flags = 0;

        if(!ctrl && !shift && !alt && cur.GetCount())
            CommitSelection(Vector<int>()); // immediate feedback
        return;
    }

    // Clicked on a tile: defer the click action until LeftUp
    pending_click = true;
    pending_index = i;
    pending_flags = flags;
}


void GalleryCtrl::LeftUp(Point, dword)
{
    if(!mouse_down)
        return;

    if(dragging) {
        // Finalize marquee using the stored flags (live preview happened in MouseMove)
        ApplyMarqueeSelection(drag_additive, drag_subtractive, drag_intersect, drag_xor);
        dragging = false;
    }
    else if(pending_click) {
        // Execute the click selection now
        const bool ctrl  = (pending_flags & K_CTRL)  != 0;
        const bool shift = (pending_flags & K_SHIFT) != 0;

        Vector<int> next; next.Reserve(16);
        const auto  cur = GetSelection();
        for(int k = 0; k < cur.GetCount(); ++k) next.Add(cur[k]);

        const int i = pending_index;

        if(shift && anchor_index >= 0) {
            int a = anchor_index, b = i; if(a > b) Swap(a, b);
            next.Clear();
            for(int k = a; k <= b; ++k) next.Add(k);
        }
        else if(ctrl) {
            bool found = false;
            for(int k = 0; k < next.GetCount(); ++k)
                if(next[k] == i) { next.Remove(k); found = true; break; }
            if(!found) next.Add(i);
            anchor_index = i;
        }
        else {
            next.Clear();
            next.Add(i);
            anchor_index = i;
        }

        CommitSelection(next);
    }

    // Reset click pending + mouse capture
    pending_click = false;
    pending_index = -1;
    pending_flags = 0;

    mouse_down = false;
    ReleaseCapture();
    Refresh();
}



void GalleryCtrl::LeftDouble(Point p, dword)
{
    int i = IndexFromPoint(p + Point(scroll_x, scroll_y));
    if(const auto* it = TryItem(i))
        WhenActivate(*it);
}

void GalleryCtrl::RightDown(Point p, dword)
{
    if(WhenBar)
        MenuBar::Execute(WhenBar, p);
}

// Marquee combiner
void GalleryCtrl::ApplyMarqueeSelection(bool add, bool sub, bool inter, bool xr)
{
    // Convert marquee rect to CONTENT coords
    Rect selc = NormalizeRect(drag_rect_win);
    selc.Offset(scroll_x, scroll_y);

    // All tiles intersecting the marquee
    Vector<int> hits = IndicesInRect(selc);

    Vector<int> next;
    next.Clear();

    if(inter) {
        // baseline ∩ hits
        for(int i = 0; i < drag_prev_sel.GetCount(); ++i) {
            int id = drag_prev_sel[i];
            if(ContainsIdx(hits, id))
                next.Add(id);
        }
    }
    else if(sub) {
        // baseline − hits
        next.Reserve(drag_prev_sel.GetCount());
        for(int i = 0; i < drag_prev_sel.GetCount(); ++i) {
            int id = drag_prev_sel[i];
            if(!ContainsIdx(hits, id))
                next.Add(id);
        }
    }
    else if(xr) {
        // XOR/toggle: symmetric difference with hits
        // start from baseline (manual copy, no assignment)
        next.Reserve(drag_prev_sel.GetCount());
        for(int i = 0; i < drag_prev_sel.GetCount(); ++i)
            next.Add(drag_prev_sel[i]);

        for(int i = 0; i < hits.GetCount(); ++i) {
            int id = hits[i];
            if(ContainsIdx(next, id)) RemoveOne(next, id);
            else                      next.Add(id);
        }
    }
    else if(add) {
        // baseline ∪ hits
        next.Reserve(drag_prev_sel.GetCount() + hits.GetCount());
        for(int i = 0; i < drag_prev_sel.GetCount(); ++i)
            next.Add(drag_prev_sel[i]);
        for(int i = 0; i < hits.GetCount(); ++i) {
            int id = hits[i];
            if(!ContainsIdx(next, id))
                next.Add(id);
        }
    }
    else {
        // replace = hits (manual copy)
        next.Reserve(hits.GetCount());
        for(int i = 0; i < hits.GetCount(); ++i)
            next.Add(hits[i]);
    }

    CommitSelection(next);
}



// ==== painting ===============================================================
void GalleryCtrl::Paint(Draw& w)
{
    Size sz = GetSize();
    w.DrawRect(sz, SColorFace());

    if(items.IsEmpty())
        return;

    const int tile = ZoomSteps()[zoom_i];
    const int tw = tile;
    const int th = tile + label_h;

    const int y0 = scroll_y;
    const int y1 = scroll_y + sz.cy;

    int first_row = max(0, (y0 - pad) / (th + pad));
    int last_row  = min(rows - 1, (y1 - 1) / (th + pad));

    for(int r = first_row; r <= last_row; ++r) {
        for(int c = 0; c < cols; ++c) {
            int i = r * cols + c;
            if(i >= items.GetCount()) break;

            Rect rt = TileRect(i);
            rt.Offset(-scroll_x, -scroll_y);

            const Rect ri = ImageRect(rt);
            Rect lab = rt; lab.top = ri.bottom;

            // Fill tile face
            w.DrawRect(rt, SColorPaper());

            const auto& it = items[i];

            if(it.status == ThumbStatus::Ok && !it.thumb.IsEmpty()) {
                const Size isz = it.thumb.GetSize();
                Rect tr = ri;
                Size dst = isz;

                if(aspect == AspectPolicy::Fit) {
                    int w0 = tr.GetWidth(), h0 = tr.GetHeight();
                    double sx = (double)w0 / isz.cx, sy = (double)h0 / isz.cy;
                    double s = min(sx, sy);
                    dst.cx = int(isz.cx * s + 0.5);
                    dst.cy = int(isz.cy * s + 0.5);
                }
                else if(aspect == AspectPolicy::Fill) {
                    int w0 = tr.GetWidth(), h0 = tr.GetHeight();
                    double sx = (double)w0 / isz.cx, sy = (double)h0 / isz.cy;
                    double s = max(sx, sy);
                    dst.cx = int(isz.cx * s + 0.5);
                    dst.cy = int(isz.cy * s + 0.5);
                }
                else { // Stretch
                    dst = tr.GetSize();
                }

                int dx = tr.left + (tr.GetWidth()  - dst.cx) / 2;
                int dy = tr.top  + (tr.GetHeight() - dst.cy) / 2;

                const Image& draw_im = (it.filtered_out)
                    ? (it.thumb_gray.IsEmpty() ? (items[i].thumb_gray = ToGray(it.thumb)) : it.thumb_gray)
                    : it.thumb;

                w.DrawImage(dx, dy, dst.cx, dst.cy, draw_im);
            }
            else {
                const int g = min(ri.GetWidth(), ri.GetHeight());
                Rect gr = ri; gr.SetSize(Size(g, g));
                gr.Offset((ri.GetWidth() - g)/2, (ri.GetHeight() - g)/2);
                const Image* gimg = nullptr;
                switch(it.status) {
                case ThumbStatus::Placeholder: gimg = &PlaceholderGlyph(g); break;
                case ThumbStatus::Missing:     gimg = &MissingGlyph(g);     break;
                case ThumbStatus::Error:       gimg = &ErrorGlyph(g);       break;
                case ThumbStatus::Auto:
                default: {
                    Color tint = Hsv01((GetHashValue(it.name) % 360) / 360.0, 0.25, 0.90);
                    w.DrawRect(ri, Mix(SColorFace(), tint, 64));
                    w.DrawRect(ri.Deflated(ri.Width()/6, ri.Height()/6), Mix(tint, SColorPaper(), 48));
                    break;
                }
                }
                if(gimg)
                    w.DrawImage(gr, *gimg);
            }

            // Flag dot (orange)
            if(it.flags != DF_None) {
                Rect d = rt.Deflated(4);
                Rect dot = RectC(d.left, d.top, 6, 6);
                w.DrawRect(dot, Color(245, 158, 11));
                StrokeRect(w, dot.Inflated(1), 1, SColorPaper());
            }

            // Label bar (simulated translucency via Mix)
            if(label_h > 0) {
                Color back = Mix(SColorLtFace(), SColorPaper(), 255 - label_backdrop_alpha);
                w.DrawRect(lab, back);
                String txt = it.name;
                w.DrawText(lab.left + 4, lab.top + (lab.GetHeight() - StdFont().GetCy()) / 2,
                           txt, StdFont(), SColorText());
            }

            // Hover ring
            if(hover_enabled && hover_index == i && !it.selected) {
                Color ring = Mix(SColorHighlight(), SColorFace(), 160);
                StrokeRect(w, rt, 1, ring);
            }

			// Selection tint (~10%) + ring
			if(it.selected) {
			    w.DrawImage(rt.left, rt.top, MakeAlphaOverlay(rt.GetSize(), SColorHighlight(), 26));
			    if(show_sel_ring)
			        StrokeRect(w, rt, 2, SColorHighlight());
			}

            // Filter border (subtle)
            if(show_filter_ring && it.filtered_out) {
                StrokeRect(w, rt, 1, Mix(SColorPaper(), SColorShadow(), 200));
            }
        }
    }

	// Rubber band (outline + ~10% halo)
	if(dragging) {
	    Rect r = NormalizeRect(drag_rect_win);
	    r.Offset(-scroll_x, -scroll_y);
	    StrokeRect(w, r, 1, SColorHighlight());
	    w.DrawImage(r.left, r.top, MakeAlphaOverlay(r.GetSize(), SColorHighlight(), 26));
	}
}

// ==== Procedural thumbs & glyphs =============================================
using Upp::BufferPainter;

// RNG helpers
static inline uint32 s_xs32(uint32& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
static inline double s_frand(uint32& s) { return s_xs32(s) / double(0xFFFFFFFFu); }
static inline int    s_rint(uint32& s, int a, int b) { return a + int(s_frand(s) * double(b - a + 1)); }




Image GalleryCtrl::GenRandomThumb(int edge_px, int aspect_w, int aspect_h, uint32 seed)
{
    if(edge_px <= 0) edge_px = 64;
    if(seed == 0)    seed = (uint32)msecs();
    uint32 rng = seed;

    if(aspect_w <= 0 || aspect_h <= 0) {
        static const int asp[][2] = { {1,1}, {4,3}, {16,9}, {3,2}, {185,100}, {239,100} };
        const int n = (int)(sizeof(asp) / sizeof(asp[0]));
        const int *a = asp[s_rint(rng, 0, n - 1)];
        aspect_w = a[0]; aspect_h = a[1];
    }

    const double R = (double)aspect_w / (double)aspect_h;
    const int W = (R >= 1.0) ? edge_px : int(edge_px * R + 0.5);
    const int H = (R >= 1.0) ? int(edge_px / R + 0.5) : edge_px;

    ImageBuffer ib(W, H);
    BufferPainter p; p.Create(ib, MODE_ANTIALIASED);

    auto huec = [&](double s, double v)->Color { return Hsv01(s_rint(rng,0,359)/360.0, s, v); };
    
    
    

    // background
    const Color bgA = Mix(SColorFace(),  huec(0.15, 0.92), 64);
    const Color bgB = Mix(SColorPaper(), huec(0.12, 0.85), 64);
    p.Clear(bgA);
    p.Begin();
        p.Move(0, 0).Line(W, 0).Line(W, H * 0.35).Line(0, H * 0.65).Close().Fill(bgB);
    p.End();

    // border
    p.Rectangle(0, 0, W, H).Stroke(1, SColorShadow());

    auto draw_one = [&](int which, double scale, double deg, Color fill, Color stroke) {
        const double PI = 3.14159265358979323846;
        const double m  = (double)min(W, H);
        const double S  = m * scale;
        const double s2 = S * 0.5;
        const double cx = W * 0.5, cy = H * 0.5;

        p.Begin();
        p.Translate(cx, cy);
        p.Rotate(deg * PI / 180.0);

        switch(which & 3) {
            case 0: { double L = S * 1.25, t = max<double>(1.5, S * 0.08);
                      p.Move(-L * 0.5, 0).Line(L * 0.5, 0).Stroke(t, stroke); break; }
            case 1: { p.Circle(0, 0, s2).Fill(fill).Stroke(max<double>(1.0, S * 0.06), stroke); break; }
            case 2: { double Rr = S * 0.5;
                      Pointf A( 0, -Rr), B( Rr * 0.8660254038, Rr * 0.5), C(-Rr * 0.8660254038, Rr * 0.5);
                      p.Move(A).Line(B).Line(C).Close().Fill(fill).Stroke(max<double>(1.0, S * 0.06), stroke); break; }
            default:{ p.Rectangle(-s2, -s2, S, S).Fill(fill).Stroke(max<double>(1.0, S * 0.06), stroke); break; }
        }
        p.End();
    };

    int s1 = s_rint(rng, 0, 3), s2;
    do s2 = s_rint(rng, 0, 3); while(s2 == s1);

    draw_one(s1, 0.72, s_rint(rng, 0, 359), huec(0.70, 0.96), huec(0.55, 0.70));
    draw_one(s2, 0.45, s_rint(rng, 0, 359), huec(0.55, 0.98), huec(0.60, 0.75));

    p.Finish();
    return ib;
}

static void s_stroke_rect_p(BufferPainter& p, const Rect& r, int pen, Color c) {
    p.Rectangle(r.left, r.top, r.GetWidth(), r.GetHeight()).Stroke(pen, c);
}



static void DrawMyIcon(BufferPainter& p, const Rect& inset)
{
    // Triangle
    p.Begin();
    p.Move((int)(inset.left + inset.Width() * 0.79), (int)(inset.top + inset.Height() * 0.4338));
    p.Line((int)(inset.left + inset.Width() * -0.03), (int)(inset.top + inset.Height() * 1.2867));
    p.Line((int)(inset.left + inset.Width() * 1.61), (int)(inset.top + inset.Height() * 1.2867));
    p.Close();
    p.Fill(Color(163, 201, 168));

    // Triangle
    p.Begin();
    p.Move((int)(inset.left + inset.Width() * 0.2575), (int)(inset.top + inset.Height() * 0.2869));
    p.Line((int)(inset.left + inset.Width() * -0.325), (int)(inset.top + inset.Height() * 1.2233));
    p.Line((int)(inset.left + inset.Width() * 0.775), (int)(inset.top + inset.Height() * 1.2233));
    p.Close();
    p.Fill(Color(163, 201, 168));

    // Circle
    p.Begin();
    p.Circle((int)(inset.left + inset.Width() * 0.6675), (int)(inset.top + inset.Height() * 0.1938), (int)(min(inset.Width(), inset.Height()) * 0.1329));
    p.Fill(Color(163, 201, 168));

}

// Shared: two mountains + sun
static void s_draw_mountains(BufferPainter& p, const Rect& inset,
                             Color back_mtn, Color front_mtn, Color sun)
{
    // Triangle
    // Circle
    p.Begin();
    p.Circle((int)(inset.left + inset.Width() * 0.6675), (int)(inset.top + inset.Height() * 0.1938), (int)(min(inset.Width(), inset.Height()) * 0.1329));
    p.Fill(front_mtn);

    // Rect
    p.Begin();
    p.Move((int)(inset.left + inset.Width() * 0), (int)(inset.top + inset.Height() * 0.0138));
    p.Line((int)(inset.left + inset.Width() * 1), (int)(inset.top + inset.Height() * 0.0138));
    p.Line((int)(inset.left + inset.Width() * 1), (int)(inset.top + inset.Height() * 0.8667));
    p.Line((int)(inset.left + inset.Width() * 0), (int)(inset.top + inset.Height() * 0.8667));
    p.Close();
    p.Stroke(4, front_mtn);

    // Triangle
    p.Begin();
    p.Move((int)(inset.left + inset.Width() * 0.2825), (int)(inset.top + inset.Height() * 0.3236));
    p.Line((int)(inset.left + inset.Width() * -0.3), (int)(inset.top + inset.Height() * 1.26));
    p.Line((int)(inset.left + inset.Width() * 0.8), (int)(inset.top + inset.Height() * 1.26));
    p.Close();

    p.Fill(front_mtn);

    // Triangle
    p.Begin();
    p.Move((int)(inset.left + inset.Width() * 0.7875), (int)(inset.top + inset.Height() * 0.5633));
    p.Line((int)(inset.left + inset.Width() * 0.0075), (int)(inset.top + inset.Height() * 1.3833));
    p.Line((int)(inset.left + inset.Width() * 1.6475), (int)(inset.top + inset.Height() * 1.3833));
    p.Close();
    p.Fill( front_mtn );
    p.Clip();
}

const Image& GalleryCtrl::Glyph(GlyphType type, int tile)
{
    static VectorMap<int, Image> cache; // key = (type<<16) | size
    tile = ClampInt(tile, 16, 512);
    int key = (int(type) << 16) | (tile & 0xFFFF);
    int fi = cache.Find(key);
    if(fi >= 0)
        return cache[fi];

    ImageBuffer ib(tile, tile);
    BufferPainter p; p.Create(ib, MODE_ANTIALIASED);

    Rect r = RectC(0, 0, tile, tile);
    p.Clear(SColorLtFace());
    s_stroke_rect_p(p, r, 1, SColorShadow());

    int m = max(2, tile / 10);
    Rect inset = r.Deflated(m);

    switch(type) {
    case GLYPH_PLACEHOLDER: {
        s_draw_mountains(p, inset, Color(110,110,110), Color(90,90,90), Color(150,150,150));
        break;
    }
    case GLYPH_MISSING: {
        s_draw_mountains(p, inset, Color(120,120,120), Color(100,100,100), Color(160,160,160));
        // Bold diagonal slash with generous width
        double pen = max<double>(2.0, tile * 0.10);
        p.Begin();
            p.Move(inset.TopLeft()).Line(inset.BottomRight()).Stroke(pen, Color(70, 70, 70));
        p.End();
        break;
    }
    case GLYPH_ERROR:
    case GLYPH_WARNING: {
        Color tri = (type == GLYPH_ERROR) ? Color(245,158,11) : Color(255,193,7);
        double cx = inset.left + inset.Width() * 0.5;
        double top = inset.top + inset.Height() * 0.18;
        double base_ = inset.bottom - inset.Height() * 0.08;
        double half = inset.Width() * 0.36;
        p.Begin();
            p.Move(Pointf(cx, top)).Line(Pointf(cx - half, base_)).Line(Pointf(cx + half, base_)).Close().Fill(tri);
        p.End();
        p.Rectangle(cx - inset.Width() * 0.035, inset.top + inset.Height() * 0.40,
                    inset.Width() * 0.07, inset.Height() * 0.28).Fill(SColorPaper());
        p.Circle(cx, inset.bottom - inset.Height() * 0.14, inset.Width() * 0.045).Fill(SColorPaper());
        break;
    }
    case GLYPH_STATUS_OK:
    case GLYPH_STATUS_WARN:
    case GLYPH_STATUS_ERR: {
        Color c = (type == GLYPH_STATUS_OK)   ? Color(76,175,80)
                 : (type == GLYPH_STATUS_WARN)? Color(255,193,7)
                 :                               Color(244,67,54);
        double rad = min(inset.Width(), inset.Height()) * 0.40;
        Pointf C = Pointf(inset.CenterPoint());
        p.Circle(C.x, C.y, rad).Fill(c);
        p.Circle(C.x, C.y, rad).Stroke(1, SColorLtFace());
        break;
    }
    default: {
        p.Begin();
            p.Move(inset.TopLeft()).Line(inset.BottomRight()).Stroke(2, SColorText());
            p.Move(inset.TopRight()).Line(inset.BottomLeft()).Stroke(2, SColorText());
        p.End();
        break;
    }
    }

    p.Finish();
    cache.Add(key) = ib;
    return cache.Get(key);
}

Image GalleryCtrl::GenThumbWithGlyph(GlyphType type, int edge_px, uint32 seed)
{
    Image bg = GenRandomThumb(edge_px, 0, 0, seed);
    if(!bg) return Glyph(type, edge_px);

    const int gsz = max(16, edge_px / 5);
    const Image& g = Glyph(type, gsz);

    ImageDraw iw(bg.GetSize());
    iw.DrawImage(0, 0, bg);
    Size s = bg.GetSize();
    iw.DrawImage(s.cx - gsz - 4, s.cy - gsz - 4, g);
    return iw; // implicit conversion to Image
}

void GalleryCtrl::FillWithRandom(GalleryCtrl& dst, int count, int thumb_edge_px, uint32 seed_base)
{
    dst.Clear();
    for(int i = 0; i < count; ++i) {
        uint32 seed = seed_base ? (seed_base + i) : 0;
        Image img = GenRandomThumb(thumb_edge_px, 0, 0, seed);
        dst.Add(Format("Item %d", i + 1), img);
        dst.SetThumbStatus(i, ThumbStatus::Ok);
        if((i % 7) == 0) dst.SetDataFlags(i, DF_MetaMissing);
    }
    dst.Refresh();
}

} // namespace Upp
