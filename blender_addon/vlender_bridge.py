bl_info = {
    "name": "Vlender Pose Bridge",
    "author": "Vlender contributors",
    "version": (0, 1, 0),
    "blender": (3, 6, 0),
    "location": "File > Import/Export, View3D > Sidebar > Vlender",
    "description": "Exchange local armature pose transforms with the Vlender PS Vita editor",
    "category": "Import-Export",
}

import os
from urllib.parse import quote, unquote

import bpy
from bpy.props import BoolProperty, StringProperty
from bpy_extras.io_utils import ExportHelper, ImportHelper


POSE_HEADER = "VLENDER_POSE 1"


def find_armature(context):
    active = context.view_layer.objects.active
    candidates = []
    if active:
        candidates.append(active)
    candidates.extend(obj for obj in context.selected_objects if obj not in candidates)

    for obj in candidates:
        if obj.type == "ARMATURE":
            return obj
        if obj.type == "MESH":
            armature = obj.find_armature()
            if armature:
                return armature
            if obj.parent and obj.parent.type == "ARMATURE":
                return obj.parent
    for obj in context.scene.objects:
        if obj.type == "ARMATURE" and obj.select_get():
            return obj
    return None


def encode_bone_name(name):
    # Keep the file easy to parse from C while preserving arbitrary bone names.
    return quote(name, safe="._-")


def decode_bone_name(name):
    return unquote(name)


def parse_pose_file(filepath):
    pose = {}
    with open(filepath, "r", encoding="utf-8") as stream:
        header = stream.readline().strip()
        if header != POSE_HEADER:
            raise ValueError("Unsupported file header: expected " + POSE_HEADER)

        for line_number, raw_line in enumerate(stream, start=2):
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) != 11 or parts[0] != "B":
                raise ValueError("Invalid bone record on line %d" % line_number)
            name = decode_bone_name(parts[1])
            try:
                values = [float(value) for value in parts[2:]]
            except ValueError as exc:
                raise ValueError("Non-numeric transform on line %d" % line_number) from exc
            if len(values) != 9:
                raise ValueError("Expected 9 transform values on line %d" % line_number)
            pose[name] = {
                "rotation": values[0:3],
                "location": values[3:6],
                "scale": values[6:9],
            }
    if not pose:
        raise ValueError("The pose file contains no bone records")
    return pose


def get_first_material_image(armature):
    # Prioritize meshes skinned to this armature; fall back to any selected mesh.
    meshes = []
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH":
            continue
        linked_armature = obj.find_armature()
        if linked_armature == armature or obj.parent == armature:
            meshes.append(obj)

    if not meshes:
        meshes = [obj for obj in bpy.context.selected_objects if obj.type == "MESH"]

    for mesh in meshes:
        for slot in mesh.material_slots:
            material = slot.material
            if not material or not material.use_nodes or not material.node_tree:
                continue
            for node in material.node_tree.nodes:
                if node.type == "TEX_IMAGE" and node.image:
                    image = node.image
                    if image.source == "FILE" and image.size[0] > 0 and image.size[1] > 0:
                        return image
    return None


def save_texture_preview(armature, filepath):
    image = get_first_material_image(armature)
    if image is None:
        return False

    old_path = image.filepath_raw
    old_format = image.file_format
    try:
        image.filepath_raw = filepath
        image.file_format = "PNG"
        image.save()
    finally:
        image.filepath_raw = old_path
        image.file_format = old_format
    return os.path.isfile(filepath) and os.path.getsize(filepath) > 0


class EXPORT_SCENE_OT_vlender_pose(bpy.types.Operator, ExportHelper):
    """Export pose transforms for editing in Vlender on the PS Vita"""
    bl_idname = "export_scene.vlender_pose"
    bl_label = "Export Vlender Pose"
    bl_options = {"PRESET"}

    filename_ext = ".vlpose"
    filter_glob: StringProperty(default="*.vlpose", options={"HIDDEN"})
    export_texture_preview: BoolProperty(
        name="Export texture preview",
        description="Write the first image texture found on a mesh using this armature to preview.png",
        default=True,
    )

    def execute(self, context):
        armature = find_armature(context)
        if not armature:
            self.report({"ERROR"}, "Select an armature, or a mesh linked to an armature.")
            return {"CANCELLED"}

        try:
            with open(self.filepath, "w", encoding="utf-8", newline="\n") as stream:
                stream.write(POSE_HEADER + "\n")
                stream.write("# B name_percent_encoded rx ry rz lx ly lz sx sy sz\n")
                for pose_bone in armature.pose.bones:
                    # matrix_basis gives the local pose transform relative to the rest bone/parent.
                    basis = pose_bone.matrix_basis.copy()
                    rotation = basis.to_euler("XYZ")
                    location = pose_bone.location
                    scale = pose_bone.scale
                    values = (
                        rotation.x, rotation.y, rotation.z,
                        location.x, location.y, location.z,
                        scale.x, scale.y, scale.z,
                    )
                    encoded = encode_bone_name(pose_bone.name)
                    stream.write("B %s %s\n" % (
                        encoded,
                        " ".join("%.7f" % value for value in values),
                    ))
        except (OSError, RuntimeError, ValueError) as exc:
            self.report({"ERROR"}, "Could not export pose: %s" % exc)
            return {"CANCELLED"}

        preview_path = os.path.join(os.path.dirname(self.filepath), "preview.png")
        if self.export_texture_preview:
            try:
                if save_texture_preview(armature, preview_path):
                    self.report({"INFO"}, "Pose and preview.png exported.")
                else:
                    self.report({"INFO"}, "Pose exported; no usable image texture was found.")
            except (OSError, RuntimeError, ValueError) as exc:
                self.report({"WARNING"}, "Pose exported, but texture preview failed: %s" % exc)
        else:
            self.report({"INFO"}, "Vlender pose exported.")
        return {"FINISHED"}


class IMPORT_SCENE_OT_vlender_pose(bpy.types.Operator, ImportHelper):
    """Apply a Vlender pose file to the matching armature bones"""
    bl_idname = "import_scene.vlender_pose"
    bl_label = "Import Vlender Pose"

    filename_ext = ".vlpose"
    filter_glob: StringProperty(default="*.vlpose", options={"HIDDEN"})

    def execute(self, context):
        armature = find_armature(context)
        if not armature:
            self.report({"ERROR"}, "Select an armature, or a mesh linked to an armature.")
            return {"CANCELLED"}

        try:
            pose = parse_pose_file(self.filepath)
        except (OSError, ValueError) as exc:
            self.report({"ERROR"}, "Could not read pose file: %s" % exc)
            return {"CANCELLED"}

        applied = 0
        missing = 0
        for name, transform in pose.items():
            pose_bone = armature.pose.bones.get(name)
            if pose_bone is None:
                missing += 1
                continue
            pose_bone.rotation_mode = "XYZ"
            pose_bone.rotation_euler = transform["rotation"]
            pose_bone.location = transform["location"]
            pose_bone.scale = transform["scale"]
            applied += 1

        context.view_layer.update()
        self.report(
            {"INFO"},
            "Applied %d bone(s); %d unmatched bone name(s) skipped." % (applied, missing),
        )
        return {"FINISHED"}


class VIEW3D_PT_vlender_bridge(bpy.types.Panel):
    bl_label = "Vlender"
    bl_idname = "VIEW3D_PT_vlender_bridge"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Vlender"

    def draw(self, context):
        layout = self.layout
        layout.label(text="PS Vita pose bridge", icon="ARMATURE_DATA")
        layout.operator("export_scene.vlender_pose", icon="EXPORT")
        layout.operator("import_scene.vlender_pose", icon="IMPORT")
        layout.label(text="Exports transforms, not mesh geometry.")
        layout.label(text="Texture export is a separate image preview.")


def menu_export(self, context):
    self.layout.operator(
        EXPORT_SCENE_OT_vlender_pose.bl_idname,
        text="Vlender Pose (.vlpose)",
    )


def menu_import(self, context):
    self.layout.operator(
        IMPORT_SCENE_OT_vlender_pose.bl_idname,
        text="Vlender Pose (.vlpose)",
    )


CLASSES = (
    EXPORT_SCENE_OT_vlender_pose,
    IMPORT_SCENE_OT_vlender_pose,
    VIEW3D_PT_vlender_bridge,
)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_export.append(menu_export)
    bpy.types.TOPBAR_MT_file_import.append(menu_import)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_export)
    bpy.types.TOPBAR_MT_file_import.remove(menu_import)
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
