# Vlender

**Vlender** is a PlayStation Vita homebrew 3D posing workspace inspired by Blender's dark editor layout. It uses **vitaGL** for hardware-accelerated 3D, a real perspective viewport, UV-mapped meshes, an armature outliner, transform controls, a small timeline, and front-touchscreen interaction.

It is **not a port of Blender itself**. The Vita app reads Vlender's compact `.vlmesh` project format exported from Blender by the included add-on. Poses can be brought back into the original ```.blend``` project without rewriting that file on the Vita.

## What this version can do

- Render a real 3D viewport using VitaGL/OpenGL and depth testing.
- Display a textured, UV-mapped, armature-skinned mesh. The bridge exports one mesh, its active UV map, one base-colour image, and up to four bone weights per vertex.
- Show a skeleton overlay and selectable bones in a right-side outliner.
- Select bones in the outliner or by tapping the visible skeleton in the 3D viewport. The Select/Move/Rotate/Scale tools edit the corresponding bone transform using the touch slider, D-pad, and shoulder-button fine steps.
- Orbit the camera by dragging in the viewport or using the left stick; the right stick adjusts zoom.
- Load and save ```.vlpose``` files.
- Generate an app icon and LiveArea art, with GitHub Actions inputs to choose custom PNG files and the displayed app name.

### Current constraints

- The app **does not read arbitrary ```.blend``` files directly**. Export a ```.vlmesh``` project from Blender first.
- This alpha exports one mesh per project, with the mesh's active UV map and one base-colour image texture. Complex Blender shader graphs, multiple materials, modifiers, shape keys, cloth/physics, and animation playback are not evaluated by the Vita exporter.
- The texture bridge writes a 24-bit BMP preview with a maximum edge of 512 pixels to fit the Vita memory/GPU budget.
- The current file format is capped at 128 bones and 10,000 triangles.
- The viewport editor is Blender-inspired, not a pixel-perfect port of Blender's desktop UI. The timeline is visual scaffolding, not a keyframe animation editor yet.
- vitaGL homebrew requires the compatible console-side shader compiler module (`libshacccg.suprx`) to be available on the Vita. The build does not bundle proprietary system files.

## Build locally

Use the official VitaSDK toolchain and install vitaGL and its dependencies. In the official `vitasdk/vitasdk:2026.08` container this workflow uses:

```sh
vdpm install vitaGL
python3 tools/generate_branding.py
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
```

The package should be `build/Vlender.vpk`. GitHub Actions runs the same toolchain flow and uploads a downloadable `Vlender-VPK` artifact.

## Export a 3D project from Blender

1. Install `blender_addon/vlender_bridge.py` from **Edit → Preferences → Add-ons → Install from Disk** (menus vary slightly by Blender release), then enable it.
2. Select the mesh linked to the armature you want to pose, or select the armature and its mesh.
3. Choose **File → Export → Vlender 3D Project (.vlmesh)**. The add-on writes a `.vlmesh` project file and, when it finds a suitable image texture, a sibling `texture.bmp`.
4. With VitaShell, create `ux0:data/VLENDER/` and copy the exported project to `ux0:data/VLENDER/model.vlmesh`. Copy the companion texture to `ux0:data/VLENDER/texture.bmp` if one was exported.
5. Start Vlender. The initial viewport shows a demo cube when no project is loaded. Tap **OPEN** to reload the model from `ux0:data/VLENDER/model.vlmesh`, or copy the project before launching.
6. Tap a bone in the **Outliner** or directly in the 3D viewport to select it. Tap **SEL**, **MOV**, **ROT**, or **SCL** on the left toolbar to choose the transform tool. Use the X/Y/Z buttons and property slider to adjust the active axis. Drag in the 3D viewport to orbit the camera. D-pad up/down selects bones; left/right adjusts the active tool; Cross changes axis; Circle resets the active transform; L/R make finer adjustments.
7. Tap **SAVE**, or press Triangle, to write `ux0:data/VLENDER/output.vlpose`.
8. Copy `output.vlpose` to the PC and use **File → Import → Vlender Pose (.vlpose)** in Blender. Review the pose and save a new `.blend` file.

The `.vlmesh` is a **single-mesh interchange format**, not a Blender file. Keep your original `.blend` and a backup. If the selected rig uses unusual constraints or non-standard bone setups, review the imported pose in Blender before saving.

## Customize the Vita app icon and LiveArea banner

The workflow's **Run workflow** form can customize the LiveArea title, icon, and background/banner.

1. Add your artwork PNGs to this repository first. Use exactly **128 × 128 px** for the icon and **840 × 500 px** for the LiveArea banner. Example: `branding/my_icon.png` and `branding/my_banner.png`.
2. Open **Actions → Build Vlender VPK → Run workflow**.
3. Set **App name**, **Icon path**, and **Banner path** to your chosen values. Leave either image path blank to use the generated Vlender artwork.
4. Download the new `Vlender-VPK` artifact after the run succeeds.

The action accepts repository-relative paths, rather than uploading an image directly through the form. The app's in-workspace wordmark remains `VLENDER`; these inputs customize the Vita bubble icon, LiveArea banner, and displayed app name.

## `.vlmesh` bridge format

The UTF-8 file begins with `VLENDER_MESH 1`, followed by an optional `T texture.bmp` record, one `B` record per bone, and one `V` record per triangle corner. The exporter duplicates triangle corners so UV seams are retained. Bone records carry the parent index, bone-local head/tail points, rest transform, and local pose TRS. Vertex records carry armature-space position/normal, UV coordinates, and up to four normalized bone-index/weight pairs.

Pose exchange uses:

```text
VLENDER_POSE 1
B bone_name_percent_encoded rx ry rz lx ly lz sx sy sz
```

Rotation values are XYZ Euler radians. Names use percent-encoding. Unknown bone names are skipped when importing a pose back into Blender.

## Repository layout

- `src/main.c` - VitaGL workspace, UI, touch/controller input, BMP texture loading.
- `src/model.c`, `src/model.h` - `.vlmesh`/`.vlpose` readers, transforms, and skinning.
- `blender_addon/vlender_bridge.py` - Blender export/import add-on.
- `tools/generate_branding.py` - dependency-free icon and LiveArea-art generator.
- `tools/validate_png.py` - checks custom artwork dimensions in CI.
- `.github/workflows/build.yml` - VPK build and branding customization inputs.

## License

MIT for the code in this repository. vitaGL and the VitaSDK components remain under their own licenses.
