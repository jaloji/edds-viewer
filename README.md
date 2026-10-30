# edds-viewer

View Enfusion engine `.edds` textures (Arma Reforger...) in Windows Explorer and XnView MP.

- **Windows**: thumbnails, preview pane and photo viewers, through a WIC codec.
- **XnView MP**: a format plugin, to browse `.edds` files next to `.dds`, `.png`...
- All formats: the EDDS container (COPY / LZ4 blocks, see [docs/FORMAT.md](docs/FORMAT.md)) is unpacked, then pixels are decoded by Microsoft's [DirectXTex](https://github.com/microsoft/DirectXTex).

Workbench channel swizzles are not stored in the file, so channels are shown as stored.

## Install

Releases come as two downloads; use one or both.

- **Windows Explorer** (`edds-viewer-*-win64.zip`): run `install.bat` as administrator, then `clear-thumbnails.bat`
  if old icons remain. Uninstall with `uninstall.bat`.
- **XnView MP plugin** (`edds-viewer-xnview-*-win64.zip`): close XnView MP and run `install-xnview.bat` as
  administrator. Uninstall with `uninstall-xnview.bat`.

If Microsoft Defender ASR rules block them, allow `C:\Program Files\EddsViewer\EddsWic.dll` or
`C:\Program Files\XnViewMP\Plugins\Xedds.usr` with `Add-MpPreference -AttackSurfaceReductionOnlyExclusions`.

## Build

Needs CMake 3.28+, Git and Visual Studio 2022 (C++). Run `build.bat`: the package is created in `dist\`. DirectXTex is downloaded at a pinned release.

`edds2png` (export to PNG) and `wictest` (decode through the codec) are built too.

## License

MIT. DirectXTex is © Microsoft, MIT license. Not affiliated with Bohemia Interactive.
