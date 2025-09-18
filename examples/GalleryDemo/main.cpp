#include <CtrlLib/CtrlLib.h>
#include <Painter/Painter.h>
#include <GalleryCtrl/GalleryCtrl.h>

using namespace Upp;

/*------------------------------------------------------------------------------
    One-call random thumbnail generator (Painter-only, guide compliant)
------------------------------------------------------------------------------*/
static Image GenRandomThumb(int edge_px, int aspect_w = 0, int aspect_h = 0, uint32 seed = 0)
{
    auto xs32  = [](uint32& s)->uint32 { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };
    auto frand = [&](uint32& s)->double { return xs32(s) / double(0xFFFFFFFFu); };
    auto rint  = [&](uint32& s, int a, int b)->int { return a + int(frand(s) * double(b - a + 1)); };

    if(seed == 0) seed = (uint32)msecs();
    uint32 rng = seed;

    if(aspect_w <= 0 || aspect_h <= 0) {
        static const int asp[][2] = { {1,1},{4,3},{16,9},{3,2},{185,100},{239,100} };
        const int n = (int)(sizeof(asp) / sizeof(asp[0]));
        const int *a = asp[rint(rng, 0, n - 1)];
        aspect_w = a[0]; aspect_h = a[1];
    }
    const double R = (double)aspect_w / (double)aspect_h;
    const int W = (R >= 1.0) ? edge_px : int(edge_px * R + 0.5);
    const int H = (R >= 1.0) ? int(edge_px / R + 0.5) : edge_px;

    ImageBuffer ib(W, H);
    BufferPainter p; p.Create(ib, MODE_ANTIALIASED);

    auto huec = [&](double s, double v)->Color { return HsvColorf(rint(rng, 0, 359) / 360.0, s, v); };

    const Color bgA = Blend(SColorFace(),  huec(0.15, 0.90), 64);
    const Color bgB = Blend(SColorPaper(), huec(0.10, 0.80), 64);
    p.Clear(bgA);
    p.Begin();
    p.Move(0, 0).Line(W, 0).Line(W, H * 0.35).Line(0, H * 0.65).Close().Fill(bgB);
    p.End();

    p.Rectangle(0, 0, W, H).Stroke(1, Blend(SColorText(), SColorPaper(), 200));

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

    int s1 = rint(rng, 0, 3), s2;
    do s2 = rint(rng, 0, 3); while(s2 == s1);
    draw_one(s1, 0.70, rint(rng, 0, 359), huec(0.75, 0.92), huec(0.65, 0.70));
    draw_one(s2, 0.45, rint(rng, 0, 359), huec(0.55, 0.96), huec(0.65, 0.70));

    p.Finish();
    return ib;
}

/*------------------------------------------------------------------------------
    Demo window (controls band + big gallery + status bar)
------------------------------------------------------------------------------*/
struct DemoWin : TopWindow {
    // Layout
    Splitter   split;
    ParentCtrl controls;
    Label      status;

    // Top controls
    DropList   aspect;        // Fit / Fill / Stretch
    SliderCtrl zoom;          // 0..4 (maps to GalleryCtrl zoom steps)
    Option     chk_hover;     // hover highlight on/off
    Option     chk_color;     // saturation on/off (gray if off)
    Label      l_filter;      // "Filter:"
    EditString filter;        // simple name filter
    DropList   gen_pick;      // Random / Error / Auto / Missing / Placeholder
    Button     btn_add, btn_add10, btn_clear_sel, btn_clear_all;

    // View
    GalleryCtrl gal;

    // Zoom slider limits (match GalleryCtrl ctor: steps = {32,48,64,96,128})
    int zoom_min = 0, zoom_max = 4;

    DemoWin() {
        Title("GalleryCtrl — Rich Demo").Sizeable().Zoomable();

        Add(split.VSizePos(0, 24));
        Add(status.HSizePos().BottomPos(0, 24));
        status.SetFrame(InsetFrame());

        split.Vert(controls, gal);
        split.SetPos(1800); // small top band; 0..10000

        // ----- controls row layout
        controls.HSizePos().VSizePos();
        int x = 4, y = 4, h = 22, gap = 4;
        auto place = [&](Ctrl& c, int w){ controls.Add(c.LeftPos(x, w).TopPos(y, h)); x += w + gap; };

        // Aspect policy
        aspect.Add("Fit"); aspect.Add("Fill"); aspect.Add("Stretch");
        aspect.SetIndex(0);
        place(aspect, 80);

        // Zoom slider
        zoom.MinMax(zoom_min, zoom_max);
        zoom <<= 2; // default ~64px
        place(zoom, 140);

        // Hover + Color toggles (do not chain .Set)
        chk_hover.SetLabel("Hover");    chk_hover <<= true;  place(chk_hover, 80);
        chk_color.SetLabel("Color On"); chk_color <<= true;  place(chk_color, 100);

        // Filter
        l_filter.SetText("Filter:"); place(l_filter, 46);
        place(filter, 200);

        // Generator pick
        gen_pick.Add("Random");
        gen_pick.Add("Error Glyph");
        gen_pick.Add("Auto Glyph");
        gen_pick.Add("Missing Glyph");
        gen_pick.Add("Placeholder");
        gen_pick.SetIndex(0);
        place(gen_pick, 130);

        // Action buttons
        btn_add.SetLabel("Add");
        btn_add10.SetLabel("Add 10");
        btn_clear_sel.SetLabel("Clear selection");
        btn_clear_all.SetLabel("Clear all");
        place(btn_add,        60);
        place(btn_add10,      70);
        place(btn_clear_sel, 120);
        place(btn_clear_all, 100);

        // ----- Gallery defaults
        gal.SetAspectPolicy(AspectPolicy::Fit);
        gal.SetHoverEnabled(true);
        gal.SetSaturationOn(true);
        gal.SetLabelBackdropAlpha(160);

        // ----- events
        aspect.WhenAction = [&]{
            int i = aspect.GetIndex();
            gal.SetAspectPolicy(i == 0 ? AspectPolicy::Fit : i == 1 ? AspectPolicy::Fill : AspectPolicy::Stretch);
            UpdateStatus();
        };
        zoom.WhenAction = [&]{
            gal.SetZoomIndex((int)~zoom);
            UpdateStatus();
        };
        chk_hover.WhenAction = [&]{ gal.SetHoverEnabled((bool)~chk_hover); };
        chk_color.WhenAction = [&]{ gal.SetSaturationOn((bool)~chk_color); gal.Refresh(); };

        filter.WhenAction = [&]{ ApplyNameFilter(~filter); };

        btn_add.WhenAction       = [&]{ AddRandom(1); };
        btn_add10.WhenAction     = [&]{ AddRandom(10); };
        btn_clear_sel.WhenAction = [&]{ gal.ClearSelection(); };
        btn_clear_all.WhenAction = [&]{ gal.Clear(); UpdateStatus(); };

        // Context menu extension + constexpr sketch copy
        gal.WhenBar = [&](Bar& b){
            b.Separator();
            b.Add("Aspect: Fit",     [&]{ aspect.SetIndex(0); aspect.WhenAction(); })
             .Radio(gal.GetAspectPolicy() == AspectPolicy::Fit);
            b.Add("Aspect: Fill",    [&]{ aspect.SetIndex(1); aspect.WhenAction(); })
             .Radio(gal.GetAspectPolicy() == AspectPolicy::Fill);
            b.Add("Aspect: Stretch", [&]{ aspect.SetIndex(2); aspect.WhenAction(); })
             .Radio(gal.GetAspectPolicy() == AspectPolicy::Stretch);
            b.Separator();
            b.Add("Copy constexpr preset sketch", [&]{
                String code =
                    "static constexpr CT::Layout<1,1,0> PRESET_HORZ_BADGE_WATERMARK {\n"
                    "    CT::Orientation::Horizontal,\n"
                    "    { CT::Region::Before, std::array{\n"
                    "        CT::Line{ CT::LineType::Content, CT::ContentLine{\n"
                    "            CT::LineDiv::D2,\n"
                    "            { CT::SegmentType::Icon,   \"status\",    0,0,false,0 },\n"
                    "            { CT::SegmentType::Spacer, nullptr,       0,0,false,0 },\n"
                    "            { CT::SegmentType::Text,   \"name\",      0,0,true, 0 },\n"
                    "            48, 0 }, 48, 0 }\n"
                    "    } },\n"
                    "    { CT::Region::Overlay, std::array{\n"
                    "        CT::Line{ CT::LineType::Content, CT::ContentLine{\n"
                    "            CT::LineDiv::D3,\n"
                    "            { CT::SegmentType::Spacer, nullptr,       0,0,false,0 },\n"
                    "            { CT::SegmentType::Text,   \"watermark\", 0,0,true, 0 },\n"
                    "            { CT::SegmentType::Spacer, nullptr,       0,0,false,0 },\n"
                    "            96, 0 }, 96, 0 }\n"
                    "    } },\n"
                    "    { CT::Region::After, {} }\n"
                    "};\n";
                WriteClipboardText(code);
                PromptOK("Copied constexpr sketch to clipboard.");
            });
        };

        gal.WhenSelection = [&]{ UpdateStatus(); };
        gal.WhenZoom      = [&](int zi){ zoom <<= zi; UpdateStatus(); };

        // Seed items
        AddRandom(400);
        UpdateStatus();
    }

    void AddRandom(int n) {
        const int start = gal.GetCount();
        for(int k = 0; k < n; ++k) {
            const int i = start + k;
            const int idx = gal.Add(Format("Item %d", i + 1));

            // occasional statuses for variety
            if(i % 37 == 0)      gal.SetThumbStatus(idx, ThumbStatus::Placeholder);
            else if(i % 53 == 0) gal.SetThumbStatus(idx, ThumbStatus::Missing);
            else if(i % 97 == 0) gal.SetThumbStatus(idx, ThumbStatus::Error);
            else                 gal.SetThumbStatus(idx, ThumbStatus::Ok);

            if((i % 7) == 0) gal.SetDataFlags(idx, DF_MetaMissing);

            const int choice = gen_pick.GetIndex();
            if(choice == 0) {
                Image im = GenRandomThumb(144, 0, 0, 1234u + idx * 23u);
                gal.SetThumbImage(idx, im);
            } else if(choice == 1) {
                gal.SetThumbStatus(idx, ThumbStatus::Error);
            } else if(choice == 2) {
                gal.SetThumbStatus(idx, ThumbStatus::Auto);
            } else if(choice == 3) {
                gal.SetThumbStatus(idx, ThumbStatus::Missing);
            } else if(choice == 4) {
                gal.SetThumbStatus(idx, ThumbStatus::Placeholder);
            }
        }
        ApplyNameFilter(~filter);
        gal.Refresh();
    }

    void ApplyNameFilter(const String& q) {
        const String needle = ToLower(q);
        const int n = gal.GetCount();
        for(int i = 0; i < n; ++i) {
            const String nm = ToLower(Format("Item %d", i + 1));
            const bool match = needle.IsEmpty() || nm.Find(needle) >= 0;
            gal.SetFiltered(i, !match); // gray non-matching (demo behavior)
        }
        gal.Refresh();
    }

    void UpdateStatus() {
        const int n   = gal.GetCount();
        const int sel = gal.GetSelection().GetCount();
        const char* aname =
            gal.GetAspectPolicy() == AspectPolicy::Fit     ? "Fit" :
            gal.GetAspectPolicy() == AspectPolicy::Fill    ? "Fill" :
                                                             "Stretch";
        status.SetText(Format("Items: %d    Selected: %d    Zoom step: %d    Aspect: %s",
                              n, sel, gal.GetZoomIndex(), aname));
    }
};

GUI_APP_MAIN
{
    SetLanguage(GetSystemLNG());
    DemoWin().Run();
}
