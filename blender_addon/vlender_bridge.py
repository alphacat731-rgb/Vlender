bl_info = {
    "name": "Vlender Project Bridge",
    "author": "Vlender contributors",
    "version": (0, 2, 0),
    "blender": (3, 6, 0),
    "location": "File > Export, View3D > Sidebar > Vlender",
    "description": "Export a textured, skinned mesh and armature for the Vlender PS Vita 3D editor",
    "category": "Import-Export",
}

import os
import math
import struct
from array import array
from urllib.parse import quote, unquote

import bpy
from bpy.props import BoolProperty, StringProperty
from bpy_extras.io_utils import ExportHelper, ImportHelper


POSE_HEADER = "VLENDER_POSE 1"
MESH_HEADER = "VLENDER_MESH 1"
MAX_TEXTURE_EDGE = 512
MAX_BONES = 256
MAX_TRIANGLE_CORNERS = 45000


def find_armature(context):
    candidates = []
    active = context.view_layer.objects.active
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


def find_mesh_for_armature(context, armature):
    active = context.view_layer.objects.active
    candidates = []
    if active and active.type == "MESH":
        candidates.append(active)
    candidates.extend(obj for obj in context.selected_objects
                      if obj.type == "MESH" and obj not in candidates)
    candidates.extend(obj for obj in context.scene.objects
                      if obj.type == "MESH" and obj not in candidates)
    for obj in candidates:
        if obj.find_armature() == armature or obj.parent == armature:
            return obj
    return None


def encode_name(name):
    return quote(name, safe="._-")


def decode_name(name):
    return unquote(name)


def parse_pose_file(filepath):
    pose = {}
    with open(filepath, "r", encoding="utf-8") as stream:
        if stream.readline().strip() != POSE_HEADER:
            raise ValueError("Expected file header " + POSE_HEADER)
        for line_number, raw_line in enumerate(stream, start=2):
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) != 11 or parts[0] != "B":
                raise ValueError("Invalid bone record on line %d" % line_number)
            try:
                values = [float(value) for value in parts[2:]]
            except ValueError as exc:
                raise ValueError("Invalid transform on line %d" % line_number) from exc
            pose[decode_name(parts[1])] = {
                "rotation": values[0:3],
                "location": values[3:6],
                "scale": values[6:9],
            }
    if not pose:
        raise ValueError("The pose file contains no bone records")
    return pose


def write_bmp24(image, filepath, max_edge=MAX_TEXTURE_EDGE):
    """Write a guaranteed 24-bit BMP with Blender's bottom-left pixel origin preserved."""
    width, height = int(image.size[0]), int(image.size[1])
    if width < 1 or height < 1:
        raise ValueError("Texture has no pixel data")
    if max(width, height) > 4096:
        raise ValueError("Texture exceeds 4096 px. Resize it in Blender before exporting to Vita.")
    factor = min(1.0, float(max_edge) / float(max(width, height)))
    out_width = max(1, int(round(width * factor)))
    out_height = max(1, int(round(height * factor)))

    # Blender's Image.pixels is a bottom-left-origin RGBA float buffer. Use a
    # compact float array instead of a Python list, then sample down to 512 px.
    rgba = array("f", [0.0]) * (width * height * 4)
    image.pixels.foreach_get(rgba)
    row_stride = (out_width * 3 + 3) & ~3
    image_size = row_stride * out_height
    file_size = 54 + image_size
    header = bytearray()
    header.extend(b"BM")
    header.extend(struct.pack("<IHHI", file_size, 0, 0, 54))
    header.extend(struct.pack("<IiiHHIIiiII",
                              40, out_width, out_height, 1, 24, 0, image_size,
                              2835, 2835, 0, 0))

    def srgb(channel):
        channel = max(0.0, min(1.0, float(channel)))
        if channel <= 0.0031308:
            result = channel * 12.92
        else:
            result = 1.055 * (channel ** (1.0 / 2.4)) - 0.055
        return max(0, min(255, int(round(result * 255.0))))

    with open(filepath, "wb") as stream:
        stream.write(header)
        # BMP stores the bottom row first; Blender's pixel buffer also starts
        # at the bottom row, so preserving ascending y avoids a vertical flip.
        for out_y in range(out_height):
            src_y = min(height - 1, (out_y * height) // out_height)
            row = bytearray()
            for out_x in range(out_width):
                src_x = min(width - 1, (out_x * width) // out_width)
                index = (src_y * width + src_x) * 4
                r = srgb(rgba[index])
                g = srgb(rgba[index + 1])
                b = srgb(rgba[index + 2])
                row.extend((b, g, r))
            row.extend(b"\0" * (row_stride - len(row)))
            stream.write(row)

def find_material_image(mesh_obj):
    for slot in mesh_obj.material_slots:
        material = slot.material
        if not material or not material.use_nodes or not material.node_tree:
            continue
        # Prefer the image feeding a Principled BSDF's Base Color.
        principled_nodes = [node for node in material.node_tree.nodes
                            if node.type == "BSDF_PRINCIPLED"]
        preferred = []
        for principled in principled_nodes:
            socket = principled.inputs.get("Base Color")
            if socket and socket.is_linked:
                for link in socket.links:
                    if link.from_node.type == "TEX_IMAGE" and link.from_node.image:
                        preferred.append(link.from_node.image)
        if preferred:
            return preferred[0]
        for node in material.node_tree.nodes:
            if node.type == "TEX_IMAGE" and node.image:
                return node.image
    return None


def normalized_weights(mesh_obj, vertex, bone_index, fallback_index):
    candidates = []
    for membership in vertex.groups:
        if membership.group >= len(mesh_obj.vertex_groups):
            continue
        name = mesh_obj.vertex_groups[membership.group].name
        index = bone_index.get(name)
        weight = float(membership.weight)
        if index is not None and weight > 0.00001:
            candidates.append((index, weight))
    candidates.sort(key=lambda pair: pair[1], reverse=True)
    candidates = candidates[:4]
    total = sum(weight for _, weight in candidates)
    if total <= 0.00001:
        if fallback_index is None:
            return []
        return [(fallback_index, 1.0)]
    return [(index, weight / total) for index, weight in candidates]


def matrix_column_major(matrix):
    return [float(matrix[row][col]) for col in range(4) for row in range(4)]


def write_vlmesh(filepath, armature, mesh_obj, export_texture):
    if len(armature.data.bones) > MAX_BONES:
        raise ValueError("Vlender alpha currently supports at most %d bones" % MAX_BONES)

    mesh = mesh_obj.data
    mesh.calc_loop_triangles()
    corner_count = len(mesh.loop_triangles) * 3
    if corner_count < 3:
        raise ValueError("Mesh has no triangles")
    if corner_count > MAX_TRIANGLE_CORNERS:
        raise ValueError(
            "Mesh has %d triangle corners; the Vita alpha limit is %d. Decimate the mesh first."
            % (corner_count, MAX_TRIANGLE_CORNERS)
        )

    armature_data_bones = list(armature.data.bones)
    bone_index = {bone.name: index for index, bone in enumerate(armature_data_bones)}
    fallback_index = 0 if armature_data_bones else None
    pose_by_name = {bone.name: bone for bone in armature.pose.bones}
    object_to_armature = armature.matrix_world.inverted() @ mesh_obj.matrix_world
    normal_matrix = object_to_armature.to_3x3().inverted().transposed()
    uv_layer = mesh.uv_layers.active

    parent_indices = []
    for bone in armature_data_bones:
        parent_indices.append(bone_index.get(bone.parent.name, -1) if bone.parent else -1)

    base_dir = os.path.dirname(os.path.abspath(filepath))
    texture_path = os.path.join(base_dir, "texture.bmp")
    texture_written = False
    if export_texture:
        image = find_material_image(mesh_obj)
        if image:
            try:
                write_bmp24(image, texture_path)
                texture_written = os.path.isfile(texture_path) and os.path.getsize(texture_path) > 54
            except (OSError, RuntimeError, ValueError) as exc:
                raise ValueError("Could not export the base-colour image: %s" % exc) from exc

    corner_count_written = 0
    with open(filepath, "w", encoding="utf-8", newline="\n") as stream:
        stream.write(MESH_HEADER + "\n")
        stream.write("# B name parent head_local_xyz tail_local_xyz rest_matrix_col_major16 basis_loc_xyz basis_euler_xyz basis_scale_xyz\n")
        stream.write("# V position_xyz normal_xyz uv_xy influence_count then (bone_index weight) pairs\n")
        stream.write("T texture.bmp\n" if texture_written else "# No exported texture; install a texture.bmp beside this file if desired.\n")

        for index, bone in enumerate(armature_data_bones):
            rest = bone.matrix_local.copy()
            inverse_rest = rest.inverted()
            head_local = inverse_rest @ bone.head_local
            tail_local = inverse_rest @ bone.tail_local
            pose_bone = pose_by_name.get(bone.name)
            if pose_bone:
                loc, quat, scale = pose_bone.matrix_basis.decompose()
                rotation = quat.to_euler("XYZ")
            else:
                loc = (0.0, 0.0, 0.0)
                rotation = (0.0, 0.0, 0.0)
                scale = (1.0, 1.0, 1.0)
            values = [
                parent_indices[index],
                head_local.x, head_local.y, head_local.z,
                tail_local.x, tail_local.y, tail_local.z,
                *matrix_column_major(rest),
                loc.x, loc.y, loc.z,
                rotation.x, rotation.y, rotation.z,
                scale.x, scale.y, scale.z,
            ]
            stream.write("B %s %d %s\n" % (
                encode_name(bone.name), parent_indices[index],
                " ".join("%.7f" % float(value) for value in values[1:])
            ))

        for triangle in mesh.loop_triangles:
            for vertex_index, loop_index in zip(triangle.vertices, triangle.loops):
                vertex = mesh.vertices[vertex_index]
                position = object_to_armature @ vertex.co
                normal = normal_matrix @ vertex.normal
                if normal.length > 0.000001:
                    normal.normalize()
                else:
                    normal = (0.0, 0.0, 1.0)
                uv = uv_layer.data[loop_index].uv if uv_layer else (0.0, 0.0)
                influences = normalized_weights(mesh_obj, vertex, bone_index, fallback_index)
                values = [
                    position.x, position.y, position.z,
                    normal.x, normal.y, normal.z,
                    float(uv[0]), float(uv[1]),
                    float(len(influences)),
                ]
                values.extend(value for pair in influences for value in pair)
                stream.write("V %s\n" % " ".join("%.7f" % float(value) for value in values))
                corner_count_written += 1

    return corner_count_written, texture_written


class EXPORT_SCENE_OT_vlender_project(bpy.types.Operator, ExportHelper):
    """Export one armature-skinned mesh, its UVs, texture preview, and skeleton for Vlender"""
    bl_idname = "export_scene.vlender_project"
    bl_label = "Export Vlender 3D Project"
    bl_options = {"PRESET"}

    filename_ext = ".vlmesh"
    filter_glob: StringProperty(default="*.vlmesh", options={"HIDDEN"})
    export_texture: BoolProperty(
        name="Export base-colour texture",
        description="Write texture.bmp beside the project when an image texture can be found",
        default=True,
    )

    def execute(self, context):
        armature = find_armature(context)
        if armature is None:
            self.report({"ERROR"}, "Select an armature or a mesh linked to an armature.")
            return {"CANCELLED"}
        mesh_obj = find_mesh_for_armature(context, armature)
        if mesh_obj is None:
            self.report({"ERROR"}, "No mesh linked to the selected armature was found.")
            return {"CANCELLED"}
        try:
            corners, texture_written = write_vlmesh(
                self.filepath, armature, mesh_obj, self.export_texture
            )
        except (OSError, RuntimeError, ValueError, OverflowError) as exc:
            self.report({"ERROR"}, str(exc))
            return {"CANCELLED"}
        message = "Exported %d triangle corners and %d bones" % (
            corners, len(armature.data.bones)
        )
        message += "; texture.bmp included" if texture_written else "; no image texture included"
        self.report({"INFO"}, message)
        return {"FINISHED"}


class EXPORT_SCENE_OT_vlender_pose(bpy.types.Operator, ExportHelper):
    """Export pose transforms to a Vlender .vlpose file"""
    bl_idname = "export_scene.vlender_pose"
    bl_label = "Export Vlender Pose"
    bl_options = {"PRESET"}

    filename_ext = ".vlpose"
    filter_glob: StringProperty(default="*.vlpose", options={"HIDDEN"})

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
                    basis = pose_bone.matrix_basis.copy()
                    location, quaternion, scale = basis.decompose()
                    rotation = quaternion.to_euler("XYZ")
                    values = (
                        rotation.x, rotation.y, rotation.z,
                        location.x, location.y, location.z,
                        scale.x, scale.y, scale.z,
                    )
                    stream.write("B %s %s\n" % (
                        encode_name(pose_bone.name),
                        " ".join("%.7f" % value for value in values),
                    ))
        except (OSError, RuntimeError, ValueError) as exc:
            self.report({"ERROR"}, "Could not export pose: %s" % exc)
            return {"CANCELLED"}
        self.report({"INFO"}, "Vlender pose exported.")
        return {"FINISHED"}


class IMPORT_SCENE_OT_vlender_pose(bpy.types.Operator, ImportHelper):
    """Apply a Vlender pose to matching bones in the selected armature"""
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
        self.report({"INFO"}, "Applied %d bones; skipped %d unmatched names." % (applied, missing))
        return {"FINISHED"}


def parse_animation_file(filepath):
    """Read Vlender's 80-frame animation interchange format."""
    frames = {}
    current_frame = None
    header_seen = False
    with open(filepath, "r", encoding="utf-8") as stream:
        for line_number, raw_line in enumerate(stream, start=1):
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if not header_seen:
                if parts != ["VLENDER_ANIM", "1"]:
                    raise ValueError("Expected header VLENDER_ANIM 1")
                header_seen = True
                continue
            if parts[0] == "F":
                if len(parts) != 2:
                    raise ValueError("Invalid frame record on line %d" % line_number)
                try:
                    current_frame = int(parts[1])
                except ValueError as exc:
                    raise ValueError("Invalid frame number on line %d" % line_number) from exc
                if not 1 <= current_frame <= 80:
                    raise ValueError("Frame number must be between 1 and 80")
                frames.setdefault(current_frame, {})
                continue
            if parts[0] == "B":
                if current_frame is None or len(parts) != 11:
                    raise ValueError("Invalid bone record on line %d" % line_number)
                try:
                    values = [float(value) for value in parts[2:]]
                except ValueError as exc:
                    raise ValueError("Invalid transform on line %d" % line_number) from exc
                if not all(math.isfinite(value) for value in values):
                    raise ValueError("Non-finite transform on line %d" % line_number)
                name = decode_name(parts[1])
                frames[current_frame][name] = {
                    "rotation": values[0:3],
                    "location": values[3:6],
                    "scale": values[6:9],
                }
            else:
                raise ValueError("Unknown record on line %d" % line_number)
    if not header_seen or not any(frames.values()):
        raise ValueError("Animation file contains no matching bone records")
    return frames


class EXPORT_SCENE_OT_vlender_animation(bpy.types.Operator, ExportHelper):
    """Sample the current armature action and export up to 80 frames for Vlender"""
    bl_idname = "export_scene.vlender_animation"
    bl_label = "Export Vlender Animation"
    bl_options = {"PRESET"}

    filename_ext = ".vlanim"
    filter_glob: StringProperty(default="*.vlanim", options={"HIDDEN"})

    def execute(self, context):
        armature = find_armature(context)
        if not armature or not armature.pose:
            self.report({"ERROR"}, "Select an armature with an animation action.")
            return {"CANCELLED"}
        if not armature.animation_data or not armature.animation_data.action:
            self.report({"ERROR"}, "The selected armature has no active action to sample.")
            return {"CANCELLED"}

        scene = context.scene
        start_frame = scene.frame_start
        end_frame = min(scene.frame_end, start_frame + 79)
        if end_frame < start_frame:
            self.report({"ERROR"}, "The scene frame range is empty.")
            return {"CANCELLED"}

        original_frame = scene.frame_current
        try:
            with open(self.filepath, "w", encoding="utf-8", newline="\n") as stream:
                stream.write("VLENDER_ANIM 1\n")
                for source_frame in range(start_frame, end_frame + 1):
                    scene.frame_set(source_frame)
                    context.view_layer.update()
                    target_frame = source_frame - start_frame + 1
                    stream.write("F %d\n" % target_frame)
                    for pose_bone in armature.pose.bones:
                        location, quaternion, scale = pose_bone.matrix_basis.decompose()
                        rotation = quaternion.to_euler("XYZ")
                        values = (
                            rotation.x, rotation.y, rotation.z,
                            location.x, location.y, location.z,
                            scale.x, scale.y, scale.z,
                        )
                        stream.write("B %s %s\n" % (
                            encode_name(pose_bone.name),
                            " ".join("%.7g" % value for value in values),
                        ))
        except (OSError, RuntimeError, ValueError, OverflowError) as exc:
            self.report({"ERROR"}, "Could not export animation: %s" % exc)
            return {"CANCELLED"}
        finally:
            scene.frame_set(original_frame)
            context.view_layer.update()

        self.report({"INFO"}, "Exported %d animation frames to Vlender." % (end_frame - start_frame + 1))
        return {"FINISHED"}


class IMPORT_SCENE_OT_vlender_animation(bpy.types.Operator, ImportHelper):
    """Import a Vlender .vlanim file as a new action on the selected armature"""
    bl_idname = "import_scene.vlender_animation"
    bl_label = "Import Vlender Animation"

    filename_ext = ".vlanim"
    filter_glob: StringProperty(default="*.vlanim", options={"HIDDEN"})

    def execute(self, context):
        armature = find_armature(context)
        if not armature or not armature.pose:
            self.report({"ERROR"}, "Select the target armature.")
            return {"CANCELLED"}
        try:
            frames = parse_animation_file(self.filepath)
        except (OSError, ValueError) as exc:
            self.report({"ERROR"}, "Could not read animation: %s" % exc)
            return {"CANCELLED"}

        action = bpy.data.actions.new("Vlender Animation")
        armature.animation_data_create()
        armature.animation_data.action = action

        applied_frames = 0
        applied_bones = 0
        missing_names = set()
        for frame_number in sorted(frames):
            context.scene.frame_set(frame_number)
            for name, transform in frames[frame_number].items():
                pose_bone = armature.pose.bones.get(name)
                if pose_bone is None:
                    missing_names.add(name)
                    continue
                pose_bone.rotation_mode = "XYZ"
                pose_bone.rotation_euler = transform["rotation"]
                pose_bone.location = transform["location"]
                pose_bone.scale = transform["scale"]
                pose_bone.keyframe_insert(data_path="location", frame=frame_number, group=name)
                pose_bone.keyframe_insert(data_path="rotation_euler", frame=frame_number, group=name)
                pose_bone.keyframe_insert(data_path="scale", frame=frame_number, group=name)
                applied_bones += 1
            applied_frames += 1

        context.scene.frame_start = min(frames)
        context.scene.frame_end = max(frames)
        context.scene.frame_set(min(frames))
        context.view_layer.update()
        self.report(
            {"INFO"},
            "Imported %d frames; inserted %d bone-frame samples; skipped %d unknown bone names."
            % (applied_frames, applied_bones, len(missing_names)),
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
        layout.label(text="PS Vita 3D editor bridge", icon="ARMATURE_DATA")
        layout.operator("export_scene.vlender_project", icon="EXPORT")
        layout.operator("export_scene.vlender_pose", icon="EXPORT")
        layout.operator("import_scene.vlender_pose", icon="IMPORT")
        layout.separator()
        layout.label(text="One mesh + armature per project")
        layout.label(text="Texture maps are exported as BMP")
        layout.label(text="The original .blend stays untouched")


def menu_export_project(self, context):
    self.layout.operator(
        EXPORT_SCENE_OT_vlender_project.bl_idname,
        text="Vlender 3D Project (.vlmesh)",
    )


def menu_export_pose(self, context):
    self.layout.operator(
        EXPORT_SCENE_OT_vlender_pose.bl_idname,
        text="Vlender Pose (.vlpose)",
    )


def menu_import_pose(self, context):
    self.layout.operator(
        IMPORT_SCENE_OT_vlender_pose.bl_idname,
        text="Vlender Pose (.vlpose)",
    )


CLASSES = (
    EXPORT_SCENE_OT_vlender_project,
    EXPORT_SCENE_OT_vlender_pose,
    IMPORT_SCENE_OT_vlender_pose,
    VIEW3D_PT_vlender_bridge,
)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.TOPBAR_MT_file_export.append(menu_export_project)
    bpy.types.TOPBAR_MT_file_export.append(menu_export_pose)
    bpy.types.TOPBAR_MT_file_import.append(menu_import_pose)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_export_project)
    bpy.types.TOPBAR_MT_file_export.remove(menu_export_pose)
    bpy.types.TOPBAR_MT_file_import.remove(menu_import_pose)
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
