#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/touch.h>

#include <vita2d.h>

#define SCREEN_W 960
#define SCREEN_H 544
#define MAX_BONES 128
#define NAME_CAP 96
#define MAX_LINE 320
#define BONE_ROW_Y 147
#define BONE_ROW_H 29

#define C_BG       RGBA8(14, 17, 22, 255)
#define C_PANEL    RGBA8(25, 30, 38, 255)
#define C_PANEL_2  RGBA8(33, 40, 50, 255)
#define C_BORDER   RGBA8(54, 64, 77, 255)
#define C_TEXT     RGBA8(235, 240, 245, 255)
#define C_MUTED    RGBA8(144, 157, 171, 255)
#define C_LIME     RGBA8(132, 255, 112, 255)
#define C_LIME_D   RGBA8(48, 93, 44, 255)
#define C_ORANGE   RGBA8(255, 165, 74, 255)
#define C_BLUE     RGBA8(106, 184, 255, 255)
#define C_RED      RGBA8(255, 109, 109, 255)

typedef struct {
    char name[NAME_CAP];
    float rot[3];
    float loc[3];
    float scale[3];
} Bone;

typedef struct {
    const char *label;
    const char *key;
    int side; /* 0 = any, 1 = left, 2 = right */
    float x1, y1, x2, y2;
} Segment;

typedef struct {
    float x1, y1, x2, y2;
    int bone_index;
    const char *label;
} DrawSegment;

static Bone g_bones[MAX_BONES];
static int g_bone_count = 0;
static int g_selected = 0;
static int g_axis = 2;
static int g_page = 0; /* 0 rig, 1 textures, 2 project */
static int g_list_offset = 0;
static int g_previous_buttons = 0;
static int g_touch_was_down = 0;
static vita2d_pgf *g_font = NULL;
static vita2d_texture *g_texture = NULL;
static char g_status[112] = "Demo rig ready - load a .vlpose from Project";
static DrawSegment g_drawn_segments[24];
static int g_drawn_segment_count = 0;

static const Segment g_segments[] = {
    { "Head",       "head",     0, 455, 137, 455, 165 },
    { "Neck",       "neck",     0, 455, 164, 455, 184 },
    { "Spine",      "spine",    0, 455, 182, 455, 235 },
    { "Hips",       "hip",      0, 455, 232, 455, 258 },
    { "Upper arm L","upperarm", 1, 455, 187, 415, 207 },
    { "Forearm L",  "forearm",  1, 415, 207, 381, 229 },
    { "Hand L",     "hand",     1, 381, 229, 360, 233 },
    { "Upper arm R","upperarm", 2, 455, 187, 495, 207 },
    { "Forearm R",  "forearm",  2, 495, 207, 529, 229 },
    { "Hand R",     "hand",     2, 529, 229, 550, 233 },
    { "Thigh L",    "thigh",    1, 455, 255, 432, 298 },
    { "Shin L",     "shin",     1, 432, 298, 414, 343 },
    { "Foot L",     "foot",     1, 414, 343, 392, 346 },
    { "Thigh R",    "thigh",    2, 455, 255, 478, 298 },
    { "Shin R",     "shin",     2, 478, 298, 496, 343 },
    { "Foot R",     "foot",     2, 496, 343, 518, 346 }
};

static const char *axis_name(int axis) {
    static const char *names[] = { "X", "Y", "Z" };
    return names[(axis >= 0 && axis <= 2) ? axis : 2];
}

static float clampf(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static void set_status(const char *message) {
    snprintf(g_status, sizeof(g_status), "%s", message ? message : "");
}

static void draw_text(int x, int y, unsigned int color, float scale, const char *message) {
    if (g_font && message) {
        vita2d_pgf_draw_text(g_font, x, y, color, scale, message);
    }
}

static void draw_centered_text(int x, int y, unsigned int color, float scale, const char *message) {
    int width = 0;
    if (g_font && message) {
        width = vita2d_pgf_text_width(g_font, scale, message);
    }
    draw_text(x - width / 2, y, color, scale, message);
}

static void draw_panel(float x, float y, float w, float h, unsigned int color) {
    vita2d_draw_rectangle(x, y, w, h, color);
    vita2d_draw_line(x, y, x + w, y, C_BORDER);
    vita2d_draw_line(x, y + h, x + w, y + h, C_BORDER);
    vita2d_draw_line(x, y, x, y + h, C_BORDER);
    vita2d_draw_line(x + w, y, x + w, y + h, C_BORDER);
}

static void draw_button(float x, float y, float w, float h, const char *label, int selected) {
    unsigned int fill = selected ? C_LIME_D : C_PANEL_2;
    unsigned int ink = selected ? C_LIME : C_TEXT;
    vita2d_draw_rectangle(x, y, w, h, fill);
    vita2d_draw_rectangle(x, y, w, 1, selected ? C_LIME : C_BORDER);
    vita2d_draw_rectangle(x, y + h - 1, w, 1, C_BORDER);
    draw_centered_text((int)(x + w / 2), (int)(y + h / 2 + 7), ink, 0.82f, label);
}

static void add_bone(const char *name) {
    if (g_bone_count >= MAX_BONES || !name) return;
    Bone *bone = &g_bones[g_bone_count++];
    memset(bone, 0, sizeof(*bone));
    snprintf(bone->name, sizeof(bone->name), "%s", name);
    bone->scale[0] = bone->scale[1] = bone->scale[2] = 1.0f;
}

static void set_demo_rig(void) {
    g_bone_count = 0;
    add_bone("root");
    add_bone("hips");
    add_bone("spine");
    add_bone("chest");
    add_bone("neck");
    add_bone("head");
    add_bone("shoulder.L");
    add_bone("upperarm.L");
    add_bone("forearm.L");
    add_bone("hand.L");
    add_bone("shoulder.R");
    add_bone("upperarm.R");
    add_bone("forearm.R");
    add_bone("hand.R");
    add_bone("thigh.L");
    add_bone("shin.L");
    add_bone("foot.L");
    add_bone("thigh.R");
    add_bone("shin.R");
    add_bone("foot.R");
    g_selected = 0;
    g_list_offset = 0;
}

static void lowercase_copy(char *dest, size_t capacity, const char *source) {
    size_t i = 0;
    if (!capacity) return;
    for (; source && source[i] && i + 1 < capacity; ++i) {
        dest[i] = (char)tolower((unsigned char)source[i]);
    }
    dest[i] = '\0';
}

static int contains(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static int bone_side_matches(const char *name, int wanted_side) {
    if (wanted_side == 0) return 1;
    int left = contains(name, "left") || contains(name, ".l") ||
               contains(name, "_l") || contains(name, "-l") ||
               contains(name, "l_") || contains(name, "l.");
    int right = contains(name, "right") || contains(name, ".r") ||
                contains(name, "_r") || contains(name, "-r") ||
                contains(name, "r_") || contains(name, "r.");
    if (wanted_side == 1) return left && !right;
    return right && !left;
}

static int find_bone_for_segment(const Segment *segment) {
    char normalized[NAME_CAP];
    for (int i = 0; i < g_bone_count; ++i) {
        lowercase_copy(normalized, sizeof(normalized), g_bones[i].name);
        if (!contains(normalized, segment->key)) continue;
        if (!bone_side_matches(normalized, segment->side)) continue;
        return i;
    }
    /* A few common rig aliases. */
    if (strcmp(segment->key, "hip") == 0) {
        for (int i = 0; i < g_bone_count; ++i) {
            lowercase_copy(normalized, sizeof(normalized), g_bones[i].name);
            if (contains(normalized, "pelvis") || contains(normalized, "hips")) return i;
        }
    }
    if (strcmp(segment->key, "upperarm") == 0) {
        for (int i = 0; i < g_bone_count; ++i) {
            lowercase_copy(normalized, sizeof(normalized), g_bones[i].name);
            if ((contains(normalized, "arm") || contains(normalized, "shoulder")) &&
                !contains(normalized, "forearm") && !contains(normalized, "hand") &&
                bone_side_matches(normalized, segment->side)) return i;
        }
    }
    return -1;
}

static void get_segment_points(int segment_index, float *x1, float *y1, float *x2, float *y2, int *bone_index) {
    const Segment *segment = &g_segments[segment_index];
    *x1 = segment->x1;
    *y1 = segment->y1;
    *x2 = segment->x2;
    *y2 = segment->y2;
    *bone_index = find_bone_for_segment(segment);
    if (*bone_index >= 0) {
        float angle = g_bones[*bone_index].rot[2];
        float dx = segment->x2 - segment->x1;
        float dy = segment->y2 - segment->y1;
        *x2 = segment->x1 + dx * cosf(angle) - dy * sinf(angle);
        *y2 = segment->y1 + dx * sinf(angle) + dy * cosf(angle);
    }
}

static void draw_schematic(void) {
    draw_panel(270, 72, 424, 390, C_PANEL);
    draw_text(288, 99, C_TEXT, 0.88f, "RIG PREVIEW");
    draw_text(288, 118, C_MUTED, 0.58f, "SCHEMATIC ONLY - NOT YOUR MODEL MESH");
    vita2d_draw_line(288, 127, 675, 127, C_BORDER);

    /* Subtle viewport guides. */
    for (int x = 315; x < 670; x += 35) vita2d_draw_line((float)x, 143, (float)x, 425, RGBA8(36, 43, 52, 255));
    for (int y = 155; y < 440; y += 35) vita2d_draw_line(300, (float)y, 676, (float)y, RGBA8(36, 43, 52, 255));

    g_drawn_segment_count = 0;
    for (int i = 0; i < (int)(sizeof(g_segments) / sizeof(g_segments[0])); ++i) {
        float x1, y1, x2, y2;
        int bone_index;
        get_segment_points(i, &x1, &y1, &x2, &y2, &bone_index);
        unsigned int color = C_MUTED;
        if (bone_index == g_selected) color = C_LIME;
        else if (bone_index >= 0) color = C_BLUE;

        vita2d_draw_line(x1, y1, x2, y2, RGBA8(8, 10, 13, 255));
        vita2d_draw_line(x1 + 1, y1, x2 + 1, y2, color);
        vita2d_draw_fill_circle(x1, y1, bone_index == g_selected ? 5.5f : 3.2f, color);
        vita2d_draw_fill_circle(x2, y2, bone_index == g_selected ? 5.5f : 3.2f, color);

        if (g_drawn_segment_count < (int)(sizeof(g_drawn_segments) / sizeof(g_drawn_segments[0]))) {
            DrawSegment *saved = &g_drawn_segments[g_drawn_segment_count++];
            saved->x1 = x1; saved->y1 = y1; saved->x2 = x2; saved->y2 = y2;
            saved->bone_index = bone_index;
            saved->label = g_segments[i].label;
        }
    }

    /* Head outline and a centre-line hint. */
    vita2d_draw_line(444, 139, 466, 139, C_TEXT);
    vita2d_draw_line(444, 139, 438, 149, C_TEXT);
    vita2d_draw_line(438, 149, 444, 162, C_TEXT);
    vita2d_draw_line(444, 162, 466, 162, C_TEXT);
    vita2d_draw_line(466, 162, 472, 149, C_TEXT);
    vita2d_draw_line(472, 149, 466, 139, C_TEXT);
    vita2d_draw_line(455, 174, 455, 225, RGBA8(77, 87, 100, 255));

    char count_text[48];
    snprintf(count_text, sizeof(count_text), "%d bones loaded", g_bone_count);
    draw_text(288, 447, C_MUTED, 0.7f, count_text);
    draw_text(475, 447, C_MUTED, 0.7f, "Z rotation bends schematic segments");
}

static void draw_bone_list(void) {
    draw_panel(18, 72, 238, 390, C_PANEL);
    draw_text(34, 99, C_TEXT, 0.9f, "ARMATURE");
    draw_text(34, 118, C_MUTED, 0.58f, "TAP A BONE TO SELECT");
    vita2d_draw_line(30, 127, 244, 127, C_BORDER);

    int visible_rows = 10;
    if (g_selected < g_list_offset) g_list_offset = g_selected;
    if (g_selected >= g_list_offset + visible_rows) g_list_offset = g_selected - visible_rows + 1;
    if (g_list_offset < 0) g_list_offset = 0;
    int max_offset = g_bone_count > visible_rows ? g_bone_count - visible_rows : 0;
    if (g_list_offset > max_offset) g_list_offset = max_offset;

    for (int row = 0; row < visible_rows; ++row) {
        int index = g_list_offset + row;
        if (index >= g_bone_count) break;
        int y = BONE_ROW_Y + row * BONE_ROW_H;
        int selected = index == g_selected;
        if (selected) {
            vita2d_draw_rectangle(28, (float)(y - 19), 218, 26, C_LIME_D);
            vita2d_draw_rectangle(28, (float)(y - 19), 3, 26, C_LIME);
        }
        char label[54];
        snprintf(label, sizeof(label), "%02d  %.30s", index + 1, g_bones[index].name);
        draw_text(38, y, selected ? C_LIME : C_TEXT, 0.75f, label);
    }
    char footer[48];
    snprintf(footer, sizeof(footer), "%d / %d", g_bone_count ? g_selected + 1 : 0, g_bone_count);
    draw_text(34, 447, C_MUTED, 0.68f, footer);
}

static void draw_inspector(void) {
    draw_panel(708, 72, 234, 390, C_PANEL);
    draw_text(725, 99, C_TEXT, 0.9f, "TRANSFORM");
    vita2d_draw_line(721, 109, 929, 109, C_BORDER);

    if (g_bone_count <= 0) {
        draw_text(725, 145, C_MUTED, 0.8f, "No bones loaded");
        return;
    }

    draw_text(725, 133, C_MUTED, 0.62f, "SELECTED BONE");
    draw_text(725, 157, C_LIME, 0.78f, g_bones[g_selected].name);
    draw_text(725, 184, C_MUTED, 0.65f, "EULER ROTATION (DEGREES)");

    for (int axis = 0; axis < 3; ++axis) {
        char line[48];
        snprintf(line, sizeof(line), "%s  %7.2f deg", axis_name(axis),
                 g_bones[g_selected].rot[axis] * 57.2957795f);
        draw_text(727, 211 + axis * 25, axis == g_axis ? C_LIME : C_TEXT, 0.83f, line);
    }

    draw_text(725, 300, C_MUTED, 0.64f, "ACTIVE AXIS");
    draw_button(725, 310, 58, 30, "X", g_axis == 0);
    draw_button(789, 310, 58, 30, "Y", g_axis == 1);
    draw_button(853, 310, 58, 30, "Z", g_axis == 2);

    /* Touch slider maps the active Euler angle from -180 to +180 degrees. */
    vita2d_draw_line(725, 361, 911, 361, C_BORDER);
    vita2d_draw_line(818, 353, 818, 369, C_MUTED);
    float degrees = g_bones[g_selected].rot[g_axis] * 57.2957795f;
    float marker_x = 725.0f + ((clampf(degrees, -180.0f, 180.0f) + 180.0f) / 360.0f) * 186.0f;
    vita2d_draw_line(725, 361, marker_x, 361, C_LIME);
    vita2d_draw_fill_circle(marker_x, 361, 6.0f, C_LIME);
    draw_text(725, 386, C_MUTED, 0.62f, "-180");
    draw_centered_text(818, 386, C_MUTED, 0.62f, "0");
    draw_text(886, 386, C_MUTED, 0.62f, "+180");

    draw_button(725, 401, 56, 34, "-5", 0);
    draw_button(787, 401, 56, 34, "+5", 0);
    draw_button(849, 401, 62, 34, "RESET", 0);
    draw_text(725, 451, C_MUTED, 0.58f, "D-PAD +/-: 5 DEGREE STEPS");
}

static void load_texture_preview(void) {
    if (g_texture) {
        vita2d_free_texture(g_texture);
        g_texture = NULL;
    }
    g_texture = vita2d_load_PNG_file("ux0:data/VLENDER/preview.png");
    if (!g_texture) g_texture = vita2d_load_JPEG_file("ux0:data/VLENDER/preview.jpg");
}

static void draw_texture_page(void) {
    draw_panel(18, 72, 924, 390, C_PANEL);
    draw_text(38, 102, C_TEXT, 1.0f, "TEXTURE PREVIEW");
    draw_text(38, 126, C_MUTED, 0.7f, "PNG/JPEG image preview exported by the Blender bridge");
    vita2d_draw_line(36, 139, 923, 139, C_BORDER);

    if (g_texture) {
        float width = (float)vita2d_texture_get_width(g_texture);
        float height = (float)vita2d_texture_get_height(g_texture);
        float scale_x = 470.0f / width;
        float scale_y = 270.0f / height;
        float scale = scale_x < scale_y ? scale_x : scale_y;
        if (scale > 1.0f) scale = 1.0f;
        float draw_w = width * scale;
        float draw_h = height * scale;
        float x = 58.0f + (470.0f - draw_w) / 2.0f;
        float y = 156.0f + (270.0f - draw_h) / 2.0f;
        vita2d_draw_rectangle(52, 150, 482, 282, C_PANEL_2);
        vita2d_draw_texture_scale(g_texture, x, y, scale, scale);
        draw_text(560, 180, C_LIME, 0.9f, "PREVIEW LOADED");
        char dims[64];
        snprintf(dims, sizeof(dims), "%.0f x %.0f px", width, height);
        draw_text(560, 208, C_TEXT, 0.8f, dims);
        draw_text(560, 251, C_MUTED, 0.72f, "Current alpha scope:");
        draw_text(560, 274, C_TEXT, 0.72f, "image preview only");
        draw_text(560, 298, C_MUTED, 0.68f, "UV-mapped 3D materials");
        draw_text(560, 319, C_MUTED, 0.68f, "are on the roadmap.");
    } else {
        draw_panel(52, 158, 482, 270, C_PANEL_2);
        draw_centered_text(293, 260, C_MUTED, 1.0f, "NO PREVIEW IMAGE FOUND");
        draw_centered_text(293, 291, C_TEXT, 0.76f, "Copy preview.png or preview.jpg to:");
        draw_centered_text(293, 315, C_LIME, 0.76f, "ux0:data/VLENDER/");
        draw_text(560, 193, C_TEXT, 0.82f, "How to get one");
        draw_text(560, 224, C_MUTED, 0.72f, "Export a pose from Blender");
        draw_text(560, 247, C_MUTED, 0.72f, "with the Vlender add-on.");
        draw_text(560, 296, C_MUTED, 0.72f, "The add-on looks for the");
        draw_text(560, 319, C_MUTED, 0.72f, "first image-texture node.");
    }
    draw_text(38, 449, C_MUTED, 0.66f, "This alpha does not yet apply UVs to a 3D mesh.");
}

static void draw_project_page(void) {
    draw_panel(18, 72, 924, 390, C_PANEL);
    draw_text(38, 103, C_TEXT, 1.0f, "POSE FILE");
    draw_text(38, 128, C_MUTED, 0.7f, "The Vita edits a portable pose file, not the original .blend project.");
    vita2d_draw_line(36, 140, 923, 140, C_BORDER);

    draw_button(46, 166, 202, 50, "LOAD input.vlpose", 0);
    draw_button(264, 166, 202, 50, "SAVE output.vlpose", 0);
    draw_button(482, 166, 202, 50, "RESET SELECTED", 0);
    draw_button(700, 166, 202, 50, "RESET DEMO RIG", 0);

    draw_text(46, 255, C_LIME, 0.9f, "INPUT");
    draw_text(46, 280, C_TEXT, 0.8f, "ux0:data/VLENDER/input.vlpose");
    draw_text(46, 326, C_LIME, 0.9f, "OUTPUT");
    draw_text(46, 351, C_TEXT, 0.8f, "ux0:data/VLENDER/output.vlpose");

    draw_text(46, 397, C_MUTED, 0.7f, "Triangle: save  |  Square: load  |  Circle: reset rotation");
    draw_text(46, 423, C_MUTED, 0.7f, "Transfer files with VitaShell, then apply output.vlpose in Blender.");
}

static void draw_tabs(void) {
    vita2d_draw_rectangle(0, 478, SCREEN_W, 66, RGBA8(19, 23, 29, 255));
    vita2d_draw_line(0, 478, SCREEN_W, 478, C_BORDER);
    draw_button(18, 490, 180, 38, "RIG", g_page == 0);
    draw_button(210, 490, 180, 38, "TEXTURES", g_page == 1);
    draw_button(402, 490, 180, 38, "PROJECT / I-O", g_page == 2);
    draw_text(608, 503, C_MUTED, 0.62f, "START: exit");
    draw_text(608, 526, C_MUTED, 0.62f, "CROSS: axis  |  CIRCLE: reset");
    draw_text(22, 23, C_LIME, 1.12f, "VLENDER");
    draw_text(142, 22, C_MUTED, 0.62f, "VITA RIG EDITOR  /  ALPHA 0.1");
    vita2d_draw_line(18, 39, 942, 39, C_BORDER);
    draw_text(22, 61, C_TEXT, 0.58f, g_status);
}

static void draw_app(void) {
    vita2d_set_clear_color(C_BG);
    vita2d_start_drawing();
    vita2d_clear_screen();
    draw_tabs();

    if (g_page == 0) {
        draw_bone_list();
        draw_schematic();
        draw_inspector();
    } else if (g_page == 1) {
        draw_texture_page();
    } else {
        draw_project_page();
    }

    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static void encode_name(const char *source, char *dest, size_t capacity) {
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (size_t i = 0; source && source[i] && out + 4 < capacity; ++i) {
        unsigned char c = (unsigned char)source[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            dest[out++] = (char)c;
        } else {
            dest[out++] = '%';
            dest[out++] = hex[c >> 4];
            dest[out++] = hex[c & 15];
        }
    }
    if (capacity) dest[out] = '\0';
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void decode_name(const char *source, char *dest, size_t capacity) {
    size_t out = 0;
    for (size_t i = 0; source && source[i] && out + 1 < capacity; ++i) {
        if (source[i] == '%' && source[i + 1] && source[i + 2]) {
            int hi = hex_value(source[i + 1]);
            int lo = hex_value(source[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dest[out++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        dest[out++] = source[i];
    }
    if (capacity) dest[out] = '\0';
}

static void ensure_data_directory(void) {
    sceIoMkdir("ux0:data/VLENDER", 0777);
}

static void save_pose(void) {
    ensure_data_directory();
    SceUID fd = sceIoOpen("ux0:data/VLENDER/output.vlpose",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        set_status("Save failed. Create ux0:data/VLENDER with VitaShell.");
        return;
    }

    static char buffer[65536];
    size_t used = 0;
    int count = snprintf(buffer, sizeof(buffer),
                         "VLENDER_POSE 1\n# B name_percent_encoded rx ry rz lx ly lz sx sy sz\n");
    if (count < 0 || (size_t)count >= sizeof(buffer)) {
        sceIoClose(fd);
        set_status("Could not format pose file.");
        return;
    }
    used = (size_t)count;

    for (int i = 0; i < g_bone_count; ++i) {
        char encoded[NAME_CAP * 3 + 1];
        encode_name(g_bones[i].name, encoded, sizeof(encoded));
        count = snprintf(buffer + used, sizeof(buffer) - used,
                         "B %s %.7f %.7f %.7f %.7f %.7f %.7f %.7f %.7f %.7f\n",
                         encoded,
                         g_bones[i].rot[0], g_bones[i].rot[1], g_bones[i].rot[2],
                         g_bones[i].loc[0], g_bones[i].loc[1], g_bones[i].loc[2],
                         g_bones[i].scale[0], g_bones[i].scale[1], g_bones[i].scale[2]);
        if (count < 0 || (size_t)count >= sizeof(buffer) - used) {
            sceIoClose(fd);
            set_status("Pose is too large to save.");
            return;
        }
        used += (size_t)count;
    }

    int written = sceIoWrite(fd, buffer, used);
    sceIoClose(fd);
    if (written == (int)used) set_status("Saved output.vlpose - copy it back to your PC.");
    else set_status("Save failed while writing the pose file.");
}

static int load_pose(void) {
    SceUID fd = sceIoOpen("ux0:data/VLENDER/input.vlpose", SCE_O_RDONLY, 0);
    if (fd < 0) {
        set_status("Missing input.vlpose. Copy an exported file into ux0:data/VLENDER/.");
        return 0;
    }

    static char buffer[65536];
    int bytes = sceIoRead(fd, buffer, sizeof(buffer) - 1);
    sceIoClose(fd);
    if (bytes <= 0) {
        set_status("Input pose is empty or unreadable.");
        return 0;
    }
    buffer[bytes] = '\0';
    if (strncmp(buffer, "VLENDER_POSE 1", 14) != 0) {
        set_status("Unsupported pose format. Expected VLENDER_POSE 1.");
        return 0;
    }

    Bone loaded[MAX_BONES];
    int count = 0;
    char *saveptr = NULL;
    for (char *line = strtok_r(buffer, "\n\r", &saveptr);
         line && count < MAX_BONES;
         line = strtok_r(NULL, "\n\r", &saveptr)) {
        if (line[0] != 'B' || line[1] != ' ') continue;
        char encoded[NAME_CAP * 3 + 1];
        float rx, ry, rz, lx, ly, lz, sx, sy, sz;
        int fields = sscanf(line, "B %288s %f %f %f %f %f %f %f %f %f",
                            encoded, &rx, &ry, &rz, &lx, &ly, &lz, &sx, &sy, &sz);
        if (fields != 10) continue;

        Bone *bone = &loaded[count];
        memset(bone, 0, sizeof(*bone));
        decode_name(encoded, bone->name, sizeof(bone->name));
        bone->rot[0] = rx; bone->rot[1] = ry; bone->rot[2] = rz;
        bone->loc[0] = lx; bone->loc[1] = ly; bone->loc[2] = lz;
        bone->scale[0] = sx; bone->scale[1] = sy; bone->scale[2] = sz;
        ++count;
    }

    if (count == 0) {
        set_status("No valid bones found in input.vlpose.");
        return 0;
    }
    memcpy(g_bones, loaded, (size_t)count * sizeof(Bone));
    g_bone_count = count;
    g_selected = 0;
    g_axis = 2;
    g_list_offset = 0;
    set_status(count >= MAX_BONES ? "Loaded pose data (128-bone limit). Preview is schematic." : "Loaded pose data. Preview is schematic; Blender keeps the original rig.");
    return 1;
}

static void adjust_rotation(float degrees) {
    if (g_bone_count <= 0) return;
    g_bones[g_selected].rot[g_axis] += degrees * 0.01745329252f;
    /* Keep values bounded and readable in the UI. */
    if (g_bones[g_selected].rot[g_axis] > 6.283185307f) g_bones[g_selected].rot[g_axis] -= 6.283185307f;
    if (g_bones[g_selected].rot[g_axis] < -6.283185307f) g_bones[g_selected].rot[g_axis] += 6.283185307f;
}

static void reset_selected_rotation(void) {
    if (g_bone_count <= 0) return;
    g_bones[g_selected].rot[0] = 0.0f;
    g_bones[g_selected].rot[1] = 0.0f;
    g_bones[g_selected].rot[2] = 0.0f;
    set_status("Selected bone rotation reset.");
}

static float point_segment_distance(float px, float py, const DrawSegment *segment) {
    float dx = segment->x2 - segment->x1;
    float dy = segment->y2 - segment->y1;
    float length2 = dx * dx + dy * dy;
    float t = 0.0f;
    if (length2 > 0.001f) {
        t = ((px - segment->x1) * dx + (py - segment->y1) * dy) / length2;
    }
    t = clampf(t, 0.0f, 1.0f);
    float cx = segment->x1 + t * dx;
    float cy = segment->y1 + t * dy;
    float ex = px - cx;
    float ey = py - cy;
    return sqrtf(ex * ex + ey * ey);
}

static void select_segment_at(float x, float y) {
    float closest = 1000.0f;
    int found = -1;
    for (int i = 0; i < g_drawn_segment_count; ++i) {
        if (g_drawn_segments[i].bone_index < 0) continue;
        float distance = point_segment_distance(x, y, &g_drawn_segments[i]);
        if (distance < closest) {
            closest = distance;
            found = g_drawn_segments[i].bone_index;
        }
    }
    if (found >= 0 && closest <= 23.0f) {
        g_selected = found;
        set_status("Bone selected from the schematic preview.");
    }
}

static void handle_touch(int x, int y) {
    if (y >= 478) {
        if (x >= 18 && x < 198) g_page = 0;
        else if (x >= 210 && x < 390) {
            g_page = 1;
            load_texture_preview();
        } else if (x >= 402 && x < 582) g_page = 2;
        return;
    }

    if (g_page == 0) {
        if (x >= 28 && x < 246 && y >= 128 && y < 420) {
            int row = (y - 128) / BONE_ROW_H;
            int index = g_list_offset + row;
            if (index >= 0 && index < g_bone_count) {
                g_selected = index;
                set_status("Bone selected.");
            }
            return;
        }

        if (x >= 270 && x < 694 && y >= 130 && y < 465) {
            select_segment_at((float)x, (float)y);
            return;
        }

        if (x >= 725 && x <= 911 && y >= 309 && y <= 342) {
            if (x < 785) g_axis = 0;
            else if (x < 849) g_axis = 1;
            else g_axis = 2;
            return;
        }
        if (x >= 715 && x <= 922 && y >= 346 && y <= 377) {
            if (g_bone_count > 0) {
                float degree = -180.0f + ((float)(x - 725) / 186.0f) * 360.0f;
                degree = clampf(degree, -180.0f, 180.0f);
                g_bones[g_selected].rot[g_axis] = degree * 0.01745329252f;
                set_status("Rotation adjusted with touchscreen slider.");
            }
            return;
        }
        if (x >= 725 && x < 782 && y >= 400 && y < 438) adjust_rotation(-5.0f);
        else if (x >= 787 && x < 844 && y >= 400 && y < 438) adjust_rotation(5.0f);
        else if (x >= 849 && x < 912 && y >= 400 && y < 438) reset_selected_rotation();
    } else if (g_page == 2) {
        if (y >= 166 && y <= 216) {
            if (x >= 46 && x <= 248) load_pose();
            else if (x >= 264 && x <= 466) save_pose();
            else if (x >= 482 && x <= 684) reset_selected_rotation();
            else if (x >= 700 && x <= 902) {
                set_demo_rig();
                set_status("Demo rig restored.");
            }
        }
    }
}

static int update_input(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned int pressed = pad.buttons & ~(unsigned int)g_previous_buttons;
    g_previous_buttons = (int)pad.buttons;

    if (pressed & SCE_CTRL_START) return 0;
    if (g_page == 0 && g_bone_count > 0) {
        if (pressed & SCE_CTRL_UP) {
            if (g_selected > 0) --g_selected;
            set_status("Bone selection moved up.");
        }
        if (pressed & SCE_CTRL_DOWN) {
            if (g_selected + 1 < g_bone_count) ++g_selected;
            set_status("Bone selection moved down.");
        }
        if (pressed & SCE_CTRL_LEFT) adjust_rotation(-5.0f);
        if (pressed & SCE_CTRL_RIGHT) adjust_rotation(5.0f);
        if (pressed & SCE_CTRL_CROSS) {
            g_axis = (g_axis + 1) % 3;
            set_status("Active rotation axis changed.");
        }
        if (pressed & SCE_CTRL_CIRCLE) reset_selected_rotation();
        if (pressed & SCE_CTRL_TRIANGLE) save_pose();
        if (pressed & SCE_CTRL_SQUARE) load_pose();
        if (pressed & SCE_CTRL_LTRIGGER) adjust_rotation(-1.0f);
        if (pressed & SCE_CTRL_RTRIGGER) adjust_rotation(1.0f);
    } else {
        if (pressed & SCE_CTRL_TRIANGLE) save_pose();
        if (pressed & SCE_CTRL_SQUARE) load_pose();
        if (pressed & SCE_CTRL_CIRCLE) reset_selected_rotation();
    }

    SceTouchData touch;
    memset(&touch, 0, sizeof(touch));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    int touch_down = touch.reportNum > 0;
    if (touch_down && !g_touch_was_down) {
        int x = (int)touch.reports[0].x / 2;
        int y = (int)touch.reports[0].y / 2;
        handle_touch(x, y);
    }
    g_touch_was_down = touch_down;
    return 1;
}

int main(void) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    ensure_data_directory();

    if (vita2d_init() < 0) {
        sceKernelExitProcess(1);
        return 1;
    }
    vita2d_set_vblank_wait(1);
    g_font = vita2d_load_default_pgf();
    set_demo_rig();
    load_texture_preview();

    int running = 1;
    while (running) {
        running = update_input();
        draw_app();
    }

    if (g_texture) vita2d_free_texture(g_texture);
    if (g_font) vita2d_free_pgf(g_font);
    vita2d_fini();
    sceKernelExitProcess(0);
    return 0;
}
