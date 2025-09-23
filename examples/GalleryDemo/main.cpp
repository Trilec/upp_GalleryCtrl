#include <CtrlLib/CtrlLib.h>
#include <Painter/Painter.h>
#include <GalleryCtrl/GalleryCtrl.h>

using namespace Upp;


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
                Image im = gal.GenRandomThumb(144, 0, 0, 1234u + idx * 23u);
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
