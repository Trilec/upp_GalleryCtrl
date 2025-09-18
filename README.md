# GalleryCtrl — Zoomable, virtualized thumbnail grid for U++

A fast, lightweight, **U++ (Ultimate++)** control for browsing large sets of thumbnails (icons, frames, images). Designed to feel like a modern gallery/list view: zoomable tiles, smooth scrolling, selection, and clear visual feedback.

> Built to comfortably handle **10k+ items** with cached thumbnails and virtualized painting.

---

## Highlights

* **Plug-and-play U++ `Ctrl`** — just add and start calling `Add()`
* **Zoom steps** (32 → 128 px by default), mouse-wheel **Ctrl+wheel** to zoom
* **Virtualized drawing** of only visible rows
* **Vertical scrolling** (mouse wheel + scrollbar)
* **Selection UX**: click to select, **Ctrl+click** to multi-select
* **Visual states**: selection border, filter dimming, desaturation toggle
* **Built-in glyphs** when no image exists:

  * `Placeholder` (dashed box + “+”)
  * `Missing` (warning frame + “!”)
* **Deterministic tint colors** from item text (HSL hash)
* Extensible API (set images from RAM or file, filter flags, toggles)


## Roadmap

* Shift-range selection (anchor + range select)
* Rubber-band (drag) selection rectangle with additive/subtractive modes
* Hover highlight and tooltips
* Optional horizontal scrolling / wrap modes
* Grouping, headers, and configurable text layout presets
* JSON-backed configuration for per-tab presets
* Async thumbnail loading hooks

Contributions welcome—please open an issue to discuss bigger changes first.

---

## License

MIT — see `LICENSE`.

---

## Credits

Built on [Ultimate++](https://www.ultimatepp.org) with❤️. Designed for creative workflows (VFX, editorial, writing) where fast review matters.

