# Vlender

**Vlender** is an experimental PlayStation Vita homebrew project for making small, practical pose edits to Blender armatures from a handheld.

> Current stage: **alpha 0.1 bridge prototype**. This is not a full Blender replacement and does not yet load or render arbitrary `.blend` scenes. The Vita app edits pose data exported by the bundled Blender add-on. Textures can be previewed separately; UV-mapped 3D rendering is a later milestone.

## What works in this milestone

- Native Vita UI built with VitaSDK and libvita2d.
- Front touchscreen controls, plus D-pad / face-button shortcuts.
- Bone list and Euler rotation controls, with small angle adjustments and reset.
- Import/export of the simple `.vlpose` interchange format.
- Optional PNG/JPEG texture preview at `ux0:data/VLENDER/preview.png` or `preview.jpg`.
- Blender 3.x/4.x/5.x add-on source for exporting/importing pose transforms and exporting a material-image preview.
- GitHub Actions builds an installable `.vpk` artifact.

## Build

The CI uses the official VitaSDK container image. Locally, install VitaSDK and the `libvita2d` package, then run:

```sh
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The VPK is written to the build directory.

## Use the Blender bridge

1. In Blender, install `blender_addon/vlender_bridge.py` from **Edit → Preferences → Add-ons → Install from Disk** (the exact label can vary by Blender version), then enable it.
2. Select an armature in Pose Mode, or select a mesh parented to an armature.
3. Export a `.vlpose` file using **File → Export → Vlender Pose (.vlpose)**. The add-on also writes `preview.png` when it can find an image texture on a material.
4. Use VitaShell to create `ux0:data/VLENDER/`; copy the exported pose to `ux0:data/VLENDER/input.vlpose` and the preview image to `ux0:data/VLENDER/preview.png`.
5. On the Vita, choose **LOAD** to import the pose. Edit bone rotations, then choose **SAVE** to write `ux0:data/VLENDER/output.vlpose`.
6. Copy `output.vlpose` back to the PC and use **File → Import → Vlender Pose (.vlpose)** to apply the transforms to the same armature. Save a new `.blend` file on the PC.

The file bridge deliberately does **not** rewrite the original `.blend` file. Keep a backup and apply the returned pose in Blender.

## Controls

- Touch a bone row to select it.
- Tap X/Y/Z to choose the Euler axis; use the slider or +/- controls for precision.
- D-pad up/down selects bones; left/right adjusts the chosen axis.
- Cross cycles the active axis; Circle resets the selected bone's rotation.
- Triangle saves to `output.vlpose`; Square loads `input.vlpose`; Start exits.
- The schematic stick-figure is a **generic preview**, not the actual geometry from your Blender file.

## Format

A `.vlpose` file is UTF-8 text:

```text
VLENDER_POSE 1
B bone_name_encoded rx ry rz lx ly lz sx sy sz
```

Bone names use percent-encoding so spaces and non-ASCII names survive. Rotations are Euler radians in XYZ order; location and scale are included for round-tripping. Unknown bone names are ignored by Blender on import, which lets one file survive harmless rig edits.

## Roadmap

1. Better armature metadata and real hierarchy-aware skeletal preview.
2. A package format carrying mesh data, UVs, materials and textures.
3. Hardware-accelerated 3D viewport and texture-mapped mesh display.
4. Broader material support and direct-file compatibility investigation.

## License

MIT. See [LICENSE](LICENSE).
