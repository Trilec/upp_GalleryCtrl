#include <CtrlLib/CtrlLib.h>
#include <GalleryCtrl/GalleryCtrl.h>

using namespace Upp;

struct DemoWin : TopWindow {
    GalleryCtrl gal;
    Label       status;

    DemoWin() {
        Title("GalleryCtrl Demo").Sizeable().Zoomable();
        Add(gal.HSizePos().VSizePos(0, 24));      // leave bottom strip for status
        Add(status.HSizePos().BottomPos(0, 24));
        status.SetFrame(InsetFrame());

        // Populate with a bunch of items (mix in some statuses & flags)
        const int N = 400;
        for(int i = 0; i < N; ++i) {
            String name = Format("Item %d", i + 1);
            int idx = gal.Add(name);
            if(i % 37 == 0) gal.SetThumbStatus(idx, ThumbStatus::Placeholder);
            if(i % 53 == 0) gal.SetThumbStatus(idx, ThumbStatus::Missing);
            if(i % 97 == 0) gal.SetThumbStatus(idx, ThumbStatus::Error);
            if(i % 7  == 0) gal.SetDataFlags(idx, DF_MetaMissing);
        }

        // Events
        gal.WhenSelection = [=] { UpdateStatus(); };
        gal.WhenCaret     = [=](int) { UpdateStatus(); };
        gal.WhenHover     = [=](int) { /* no-op */ };
        gal.WhenZoom      = [=](int zi) { status.SetText(Format("Zoom step: %d", zi)); };
        gal.WhenBar       = [=](Bar& b) {
            b.Separator();
            b.Add("Aspect: Fit",     [=]{ gal.SetAspectPolicy(AspectPolicy::Fit);     })
             .Radio(gal.GetAspectPolicy() == AspectPolicy::Fit);
            b.Add("Aspect: Fill",    [=]{ gal.SetAspectPolicy(AspectPolicy::Fill);    })
             .Radio(gal.GetAspectPolicy() == AspectPolicy::Fill);
            b.Add("Aspect: Stretch", [=]{ gal.SetAspectPolicy(AspectPolicy::Stretch); })
             .Radio(gal.GetAspectPolicy() == AspectPolicy::Stretch);
        };

        UpdateStatus();
    }

    void UpdateStatus() {
        Vector<int> sel = gal.GetSelection();
        const char* aname =
            gal.GetAspectPolicy() == AspectPolicy::Fit     ? "Fit" :
            gal.GetAspectPolicy() == AspectPolicy::Fill    ? "Fill" :
                                                             "Stretch";
        status.SetText(Format("Selected: %d    Zoom step: %d    Aspect: %s",
                              sel.GetCount(), gal.GetZoomIndex(), aname));
    }

    bool Key(dword key, int) override {
        // Quick aspect switches
        if(key == K_1) { gal.SetAspectPolicy(AspectPolicy::Fit);     UpdateStatus(); return true; }
        if(key == K_2) { gal.SetAspectPolicy(AspectPolicy::Fill);    UpdateStatus(); return true; }
        if(key == K_3) { gal.SetAspectPolicy(AspectPolicy::Stretch); UpdateStatus(); return true; }

        // Zoom (Ctrl + +/-)
        if(key == (K_CTRL|K_ADD) || key == (K_CTRL|K_PLUS))  { gal.SetZoomIndex(gal.GetZoomIndex() + 1); UpdateStatus(); return true; }
        if(key == (K_CTRL|K_SUBTRACT) || key == (K_CTRL|K_MINUS)) { gal.SetZoomIndex(gal.GetZoomIndex() - 1); UpdateStatus(); return true; }

        // Selection helpers
        if(key == (K_CTRL|K_A)) { // select all
            MenuBar::Execute([&](Bar& b){ b.Add("Select all", [&]{ /* reuse ctrl menu */ }); });
        }
        return TopWindow::Key(key, 0);
    }
};

GUI_APP_MAIN 
{
    SetLanguage(GetSystemLNG());
    DemoWin().Run();
}
