# Locus Icon Assets

Design direction: a data/workspace stack contained inside a simple hexagonal
control boundary. The large mark keeps the perimeter nodes; the small header
and tray exports use simplified geometry for legibility at 16-32 px.

Generated files:

- `source/locus-icon.svg` - vector master for review and future edits.
- `github/locus-github-front-1024.png` - large front-page/readme icon.
- `windows/locus.ico` - Windows app icon bundle for Explorer and taskbar.
- `macos/Locus.icns` - macOS app icon bundle for Finder and Dock.
- `macos/Locus.iconset/` - source PNG iconset used for macOS packaging.
- `tray/` - 16/20/24/32 px tray PNGs and ICO bundles for idle, active,
  indexing, and error states.
- `about/` - 256 and 512 px PNGs for the About window.
- `header/` - 16/20/24/32 px PNGs for the top-left window header.
- `png/` - general-purpose app PNG exports from 16 to 1024 px.

Regenerate with:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File assets\icons\generate_locus_icons.ps1
```
