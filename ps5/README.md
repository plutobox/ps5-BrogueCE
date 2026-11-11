# Brogue CE on the PS5 (PPSA92043)

Native title port. For the pinned repository-local toolchain and GitHub Actions
release controls, see [builds and releases](releases.md).

With the toolchain environment loaded, build in WSL:

    cd ps5 && make -f Makefile.native -j8 && bash package-native.sh --build

Output: `build-ps5/app/dist/PPSA92043{,.zip}`.

- Tiles: Brogue's own `tiles.c` runs unchanged; `ps5main.c` binds (`--wrap`) its SDL window/renderer calls to a software renderer drawing into an off-screen surface, shown with OpenGL.
- Files: `ps5_io.h` (forced include) sends relative paths to `/download0/brogue`; assets are read from `/app0/assets`. Log: `/data/brogue-ps5.log`.
- Controller (`BROGUE_PS5` in core): d-pad/stick move or move the focus in menus; Cross = Return (look/travel, choose); Circle = Esc; Square explore; Triangle pack (next target when aiming); Options = actions menu; L1/R1 stairs; L2 search; R2 rest; L3 rest until better; R3 throw again; Touchpad messages; Create help. Text entry uses the system keyboard.
- Debug build: `-DPS5_SCREENSHOT` in `ps5main.o` (screen to `/data/brogue-screen.bmp`, keys from `/data/brogue-input.txt`).
