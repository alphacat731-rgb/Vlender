#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/touch.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "model.h"

#define SCREEN_W 960
#define SCREEN_H 544
#define VIEW_X 44
#define VIEW_Y 52
#define VIEW_W 632
#define VIEW_H 366
#define PATH_MODEL "ux0:data/VLENDER/model.vlmesh"
#define PATH_TEXTURE "ux0:data/VLENDER/texture.bmp"
#define PATH_POSE_IN "ux0:data/VLENDER/input.vlpose"
#define PATH_POSE_OUT "ux0:data/VLENDER/output.vlpose"

typedef struct { float x, y, z; } Vec3;

static VLModel g_model;
static GLuint g_texture = 0;
static int g_pose_mode = 1;
static int g_transform_tool = 2; /* 0 select, 1 move, 2 rotate, 3 scale */
static int g_selected_bone = 0;
static int g_active_axis = 2;
static int g_bone_scroll = 0;
static int g_previous_buttons = 0;
static int g_touch_down = 0;
static int g_drag_camera = 0;
static int g_drag_slider = 0;
static int g_touch_moved = 0;
static int g_touch_x = 0, g_touch_y = 0;
static int g_touch_start_x = 0, g_touch_start_y = 0;
static float g_camera_azimuth = 32.0f;
static float g_camera_elevation = -62.0f;
static float g_camera_distance = 5.0f;
static char g_status[128] = "EXPORT A MODEL FROM BLENDER TO ux0:data/VLENDER/";
static int g_frame = 1;

/* Compact 5x7 font: digits 0-9 followed by A-Z, then punctuation. */
static const unsigned char g_font36[36][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
    {14,17,17,15,1,2,12},
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {28,18,17,17,17,18,28},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31}
};

static const unsigned char *glyph_rows(char c, const unsigned char **special) {
    static const unsigned char space[7] = {0,0,0,0,0,0,0};
    static const unsigned char dash[7] = {0,0,0,31,0,0,0};
    static const unsigned char underscore[7] = {0,0,0,0,0,0,31};
    static const unsigned char dot[7] = {0,0,0,0,0,12,12};
    static const unsigned char colon[7] = {0,12,12,0,12,12,0};
    static const unsigned char slash[7] = {1,2,2,4,8,8,16};
    static const unsigned char plus[7] = {0,4,4,31,4,4,0};
    static const unsigned char equal[7] = {0,0,31,0,31,0,0};
    static const unsigned char less[7] = {2,4,8,16,8,4,2};
    static const unsigned char greater[7] = {8,4,2,1,2,4,8};
    static const unsigned char question[7] = {14,17,1,2,4,0,4};
    static const unsigned char leftbracket[7] = {14,8,8,8,8,8,14};
    static const unsigned char rightbracket[7] = {14,2,2,2,2,2,14};
    static const unsigned char pipe[7] = {4,4,4,4,4,4,4};
    static const unsigned char percent[7] = {17,2,4,8,16,17,0};
    static const unsigned char star[7] = {0,21,14,31,14,21,0};
    static const unsigned char asterisk[7] = {0,4,21,14,21,4,0};
    *special = space;
    if (c >= '0' && c <= '9') return g_font36[c - '0'];
    if (c >= 'A' && c <= 'Z') return g_font36[10 + c - 'A'];
    switch (c) {
        case '-': *special = dash; break;
        case '_': *special = underscore; break;
        case '.': *special = dot; break;
        case ':': *special = colon; break;
        case '/': *special = slash; break;
        case '+': *special = plus; break;
        case '=': *special = equal; break;
        case '<': *special = less; break;
        case '>': *special = greater; break;
        case '?': *special = question; break;
        case '[': *special = leftbracket; break;
        case ']': *special = rightbracket; break;
        case '|': *special = pipe; break;
        case '%': *special = percent; break;
        case '*': *special = star; break;
        case '\'': *special = asterisk; break;
        default: *special = space; break;
    }
    return *special;
}

static void set_color(float r, float g, float b) {
    glColor4f(r, g, b, 1.0f);
}

static void rect2d(float x, float y, float w, float h, float r, float g, float b) {
    set_color(r,g,b);
    glBegin(GL_QUADS);
    glVertex2f(x,y); glVertex2f(x+w,y);
    glVertex2f(x+w,y+h); glVertex2f(x,y+h);
    glEnd();
}

static void line2d(float x1, float y1, float x2, float y2, float r, float g, float b) {
    set_color(r,g,b);
    glBegin(GL_LINES);
    glVertex2f(x1,y1); glVertex2f(x2,y2);
    glEnd();
}

static void text2d(float x, float y, float scale, float r, float g, float b, const char *text) {
    float cursor = x;
    if (!text) return;
    set_color(r,g,b);
    glBegin(GL_QUADS);
    for (; *text; ++text) {
        const unsigned char *rows = NULL;
        const unsigned char *pattern = glyph_rows((char)toupper((unsigned char)*text), &rows);
        int row, col;
        for (row = 0; row < 7; ++row) {
            for (col = 0; col < 5; ++col) {
                if ((pattern[row] & (1 << (4 - col))) != 0) {
                    float px = cursor + (float)col * scale;
                    float py = y + (float)row * scale;
                    glVertex2f(px, py);
                    glVertex2f(px + scale, py);
                    glVertex2f(px + scale, py + scale);
                    glVertex2f(px, py + scale);
                }
            }
        }
        cursor += 6.0f * scale;
    }
    glEnd();
}

static void centered_text(float center_x, float y, float scale, float r, float g, float b, const char *text) {
    size_t len = text ? strlen(text) : 0;
    float width = (float)len * 6.0f * scale;
    text2d(center_x - width * 0.5f, y, scale, r, g, b, text);
}

static void button2d(float x, float y, float w, float h, const char *label, int active) {
    if (active) rect2d(x,y,w,h,0.15f,0.27f,0.20f);
    else rect2d(x,y,w,h,0.105f,0.125f,0.15f);
    rect2d(x,y,w,1.0f,active ? 0.52f : 0.22f,active ? 0.96f : 0.28f,active ? 0.43f : 0.32f);
    rect2d(x,y+h-1.0f,w,1.0f,0.22f,0.26f,0.31f);
    centered_text(x+w*0.5f,y+h*0.5f-3.5f,1.0f,
                  active ? 0.55f : 0.86f,active ? 1.0f : 0.89f,active ? 0.47f : 0.91f,label);
}

static void begin_2d(void) {
    glViewport(0, 0, SCREEN_W, SCREEN_H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, SCREEN_W, SCREEN_H, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_TEXTURE_2D);
}

static void ensure_data_directory(void) {
    sceIoMkdir("ux0:data/VLENDER", 0777);
}

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int load_bmp_texture(const char *path) {
    FILE *file = fopen(path, "rb");
    unsigned char header[54];
    unsigned char *row = NULL;
    unsigned char *pixels = NULL;
    int width, height, top_down, row_size, y, x;
    uint32_t data_offset;
    int bpp, compression;
    GLuint texture = 0;
    if (!file) return 0;
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        header[0] != 'B' || header[1] != 'M') {
        fclose(file);
        return 0;
    }
    data_offset = le32(header + 10);
    width = (int32_t)le32(header + 18);
    height = (int32_t)le32(header + 22);
    bpp = (int)(header[28] | ((unsigned int)header[29] << 8));
    compression = (int)le32(header + 30);
    if (width <= 0 || height == 0 || width > 2048 || abs(height) > 2048 ||
        bpp != 24 || compression != 0) {
        fclose(file);
        return 0;
    }
    top_down = height < 0;
    if (top_down) height = -height;
    row_size = (width * 3 + 3) & ~3;
    row = (unsigned char *)malloc((size_t)row_size);
    pixels = (unsigned char *)malloc((size_t)width * (size_t)height * 3);
    if (!row || !pixels) {
        free(row); free(pixels); fclose(file);
        return 0;
    }
    for (y = 0; y < height; ++y) {
        int dest_y = top_down ? (height - 1 - y) : y;
        if (fseek(file, (long)(data_offset + (uint32_t)y * (uint32_t)row_size), SEEK_SET) != 0 ||
            fread(row, 1, (size_t)row_size, file) != (size_t)row_size) {
            free(row); free(pixels); fclose(file);
            return 0;
        }
        for (x = 0; x < width; ++x) {
            size_t dst = ((size_t)dest_y * (size_t)width + (size_t)x) * 3;
            size_t src = (size_t)x * 3;
            pixels[dst] = row[src + 2];
            pixels[dst + 1] = row[src + 1];
            pixels[dst + 2] = row[src];
        }
    }
    free(row);
    fclose(file);

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(pixels);
    return (int)texture;
}

static void reload_texture(void) {
    if (g_texture) {
        GLuint old = g_texture;
        glDeleteTextures(1, &old);
        g_texture = 0;
    }
    g_texture = (GLuint)load_bmp_texture(PATH_TEXTURE);
}

static int load_project(void) {
    if (!vl_model_load_project(&g_model, PATH_MODEL)) {
        set_color(1.0f,1.0f,1.0f);
        snprintf(g_status, sizeof(g_status), "%s", "NO PROJECT: EXPORT FROM BLENDER AND COPY model.vlmesh");
        reload_texture();
        return 0;
    }
    g_selected_bone = 0;
    g_bone_scroll = 0;
    g_camera_distance = fmaxf(2.8f, g_model.radius * 3.8f);
    g_pose_mode = 1;
    reload_texture();
    vl_model_load_pose(&g_model, PATH_POSE_IN);
    snprintf(g_status, sizeof(g_status), "LOADED PROJECT - %d TRIANGLE CORNERS / %d BONES",
             g_model.vertex_count, g_model.bone_count);
    return 1;
}

static void save_pose(void) {
    ensure_data_directory();
    if (!g_model.loaded || !vl_model_save_pose(&g_model, PATH_POSE_OUT)) {
        snprintf(g_status, sizeof(g_status), "%s", "SAVE FAILED - LOAD A .VLMESH PROJECT FIRST");
    } else {
        snprintf(g_status, sizeof(g_status), "%s", "SAVED output.vlpose - COPY TO PC AND IMPORT IN BLENDER");
    }
}

static void load_pose(void) {
    if (!g_model.loaded || vl_model_load_pose(&g_model, PATH_POSE_IN) <= 0) {
        snprintf(g_status, sizeof(g_status), "%s", "NO MATCHING input.vlpose FOUND");
    } else {
        snprintf(g_status, sizeof(g_status), "%s", "POSE LOADED");
    }
}

static float active_rotation_degrees(void) {
    if (!g_model.loaded || g_model.bone_count == 0) return 0.0f;
    return g_model.bones[g_selected_bone].basis_rotation[g_active_axis] * 57.295779513f;
}

static void set_active_rotation(float degrees) {
    if (!g_model.loaded || g_model.bone_count == 0) return;
    degrees = fmaxf(-180.0f, fminf(180.0f, degrees));
    g_model.bones[g_selected_bone].basis_rotation[g_active_axis] = degrees * 0.01745329252f;
    vl_model_update_skin(&g_model);
}

static float active_transform_value(void) {
    if (!g_model.loaded || g_model.bone_count == 0) return 0.0f;
    if (g_transform_tool == 1) return g_model.bones[g_selected_bone].basis_location[g_active_axis];
    if (g_transform_tool == 2) return active_rotation_degrees();
    if (g_transform_tool == 3) return g_model.bones[g_selected_bone].basis_scale[g_active_axis];
    return 0.0f;
}

static float active_transform_min(void) {
    if (g_transform_tool == 1) return -2.0f;
    if (g_transform_tool == 2) return -180.0f;
    return 0.1f;
}

static float active_transform_max(void) {
    if (g_transform_tool == 1) return 2.0f;
    if (g_transform_tool == 2) return 180.0f;
    return 3.0f;
}

static void set_active_transform(float value) {
    if (!g_model.loaded || g_model.bone_count == 0 || g_transform_tool == 0) return;
    if (g_transform_tool == 1) {
        g_model.bones[g_selected_bone].basis_location[g_active_axis] =
            fmaxf(-2.0f, fminf(2.0f, value));
    } else if (g_transform_tool == 2) {
        set_active_rotation(value);
        return;
    } else if (g_transform_tool == 3) {
        g_model.bones[g_selected_bone].basis_scale[g_active_axis] =
            fmaxf(0.1f, fminf(3.0f, value));
    }
    vl_model_update_skin(&g_model);
}

static void adjust_rotation(float amount) {
    if (!g_model.loaded || g_model.bone_count == 0 || g_transform_tool == 0) return;
    if (g_transform_tool == 1) {
        set_active_transform(active_transform_value() + amount * 0.02f);
    } else if (g_transform_tool == 2) {
        set_active_transform(active_rotation_degrees() + amount);
    } else if (g_transform_tool == 3) {
        set_active_transform(active_transform_value() + amount * 0.02f);
    }
}

static void reset_active_transform(void) {
    int i;
    if (!g_model.loaded || g_model.bone_count == 0 || g_transform_tool == 0) return;
    if (g_transform_tool == 1) {
        g_model.bones[g_selected_bone].basis_location[g_active_axis] = 0.0f;
    } else if (g_transform_tool == 2) {
        for (i = 0; i < 3; ++i) g_model.bones[g_selected_bone].basis_rotation[i] = 0.0f;
    } else if (g_transform_tool == 3) {
        g_model.bones[g_selected_bone].basis_scale[g_active_axis] = 1.0f;
    }
    vl_model_update_skin(&g_model);
    snprintf(g_status, sizeof(g_status), "%s", "ACTIVE TRANSFORM RESET");
}

static void select_bone(int index) {
    if (!g_model.loaded || g_model.bone_count == 0) return;
    if (index < 0) index = 0;
    if (index >= g_model.bone_count) index = g_model.bone_count - 1;
    g_selected_bone = index;
    if (g_selected_bone < g_bone_scroll) g_bone_scroll = g_selected_bone;
    if (g_selected_bone >= g_bone_scroll + 6) g_bone_scroll = g_selected_bone - 5;
}

static Vec3 transform_point4(const float m[16], const float p[3]) {
    Vec3 out;
    out.x = m[0]*p[0] + m[4]*p[1] + m[8]*p[2] + m[12];
    out.y = m[1]*p[0] + m[5]*p[1] + m[9]*p[2] + m[13];
    out.z = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14];
    return out;
}

static int project_view_point(Vec3 point, float *screen_x, float *screen_y) {
    float x = point.x - (g_model.loaded ? g_model.center[0] : 0.0f);
    float y = point.y - (g_model.loaded ? g_model.center[1] : 0.0f);
    float z = point.z - (g_model.loaded ? g_model.center[2] : 0.0f);
    float azimuth = g_camera_azimuth * 0.01745329252f;
    float elevation = g_camera_elevation * 0.01745329252f;
    float ca = cosf(azimuth), sa = sinf(azimuth);
    float ce = cosf(elevation), se = sinf(elevation);
    float x1 = ca * x - sa * y;
    float y1 = sa * x + ca * y;
    float y2 = ce * y1 - se * z;
    float z2 = se * y1 + ce * z - g_camera_distance;
    float focal = (float)VIEW_H * 0.5f / tanf(22.5f * 0.01745329252f);
    float depth = -z2;
    if (depth < 0.05f) return 0;
    *screen_x = (float)VIEW_X + (float)VIEW_W * 0.5f + focal * x1 / depth;
    *screen_y = (float)VIEW_Y + (float)VIEW_H * 0.5f - focal * y2 / depth;
    return *screen_x >= (float)(VIEW_X - 24) &&
           *screen_x <= (float)(VIEW_X + VIEW_W + 24) &&
           *screen_y >= (float)(VIEW_Y - 24) &&
           *screen_y <= (float)(VIEW_Y + VIEW_H + 24);
}

static float touch_to_segment_distance(float px, float py,
                                       float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    float length_squared = dx * dx + dy * dy;
    float t = 0.0f;
    float qx, qy, ex, ey;
    if (length_squared > 0.001f) t = ((px - ax) * dx + (py - ay) * dy) / length_squared;
    t = fmaxf(0.0f, fminf(1.0f, t));
    qx = ax + t * dx;
    qy = ay + t * dy;
    ex = px - qx;
    ey = py - qy;
    return sqrtf(ex * ex + ey * ey);
}

static int pick_bone_at(int x, int y) {
    int i, found = -1;
    float best_distance = 16.0f;
    if (!g_model.loaded || !g_pose_mode || g_model.bone_count <= 0) return 0;
    for (i = 0; i < g_model.bone_count; ++i) {
        const VLBone *bone = &g_model.bones[i];
        Vec3 head = transform_point4(bone->pose, bone->head_local);
        Vec3 tail = transform_point4(bone->pose, bone->tail_local);
        float hx, hy, tx, ty, distance;
        if (!project_view_point(head, &hx, &hy) || !project_view_point(tail, &tx, &ty)) continue;
        distance = touch_to_segment_distance((float)x, (float)y, hx, hy, tx, ty);
        if (distance < best_distance) {
            best_distance = distance;
            found = i;
        }
    }
    if (found >= 0) {
        select_bone(found);
        snprintf(g_status, sizeof(g_status), "%s", "BONE SELECTED IN 3D VIEWPORT");
        return 1;
    }
    return 0;
}

static void grid_line(float x1, float y1, float z1, float x2, float y2, float z2,
                      float r, float g, float b) {
    set_color(r,g,b);
    glVertex3f(x1,y1,z1);
    glVertex3f(x2,y2,z2);
}

static void draw_grid(void) {
    int i;
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_LINES);
    for (i = -12; i <= 12; ++i) {
        float strength = (i == 0) ? 0.42f : 0.17f;
        grid_line((float)i,-12.0f,0.0f,(float)i,12.0f,0.0f,strength,strength*1.03f,strength*1.08f);
        grid_line(-12.0f,(float)i,0.0f,12.0f,(float)i,0.0f,strength,strength*1.03f,strength*1.08f);
    }
    glEnd();
    glBegin(GL_LINES);
    grid_line(-12.0f,0.0f,0.006f,12.0f,0.0f,0.006f,0.72f,0.18f,0.20f);
    grid_line(0.0f,-12.0f,0.008f,0.0f,12.0f,0.008f,0.20f,0.60f,0.30f);
    glEnd();
}

static void cube_face(const float p[12], float r, float g, float b) {
    set_color(r,g,b);
    glBegin(GL_QUADS);
    glVertex3f(p[0],p[1],p[2]);
    glVertex3f(p[3],p[4],p[5]);
    glVertex3f(p[6],p[7],p[8]);
    glVertex3f(p[9],p[10],p[11]);
    glEnd();
}

static void draw_demo_cube(void) {
    static const float faces[6][12] = {
        {-0.7f,-0.7f,-0.7f, 0.7f,-0.7f,-0.7f, 0.7f,0.7f,-0.7f, -0.7f,0.7f,-0.7f},
        {-0.7f,-0.7f,0.7f, -0.7f,0.7f,0.7f, 0.7f,0.7f,0.7f, 0.7f,-0.7f,0.7f},
        {-0.7f,-0.7f,-0.7f, -0.7f,-0.7f,0.7f, 0.7f,-0.7f,0.7f, 0.7f,-0.7f,-0.7f},
        {-0.7f,0.7f,-0.7f, 0.7f,0.7f,-0.7f, 0.7f,0.7f,0.7f, -0.7f,0.7f,0.7f},
        {-0.7f,-0.7f,-0.7f, -0.7f,0.7f,-0.7f, -0.7f,0.7f,0.7f, -0.7f,-0.7f,0.7f},
        {0.7f,-0.7f,-0.7f, 0.7f,-0.7f,0.7f, 0.7f,0.7f,0.7f, 0.7f,0.7f,-0.7f}
    };
    cube_face(faces[0],0.25f,0.30f,0.35f);
    cube_face(faces[1],0.47f,0.61f,0.52f);
    cube_face(faces[2],0.28f,0.37f,0.39f);
    cube_face(faces[3],0.38f,0.48f,0.49f);
    cube_face(faces[4],0.22f,0.29f,0.32f);
    cube_face(faces[5],0.34f,0.45f,0.43f);
}

static void draw_model_mesh(void) {
    int i;
    if (!g_model.loaded || g_model.vertex_count < 3) {
        draw_demo_cube();
        return;
    }

    if (g_texture) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_texture);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    } else {
        glDisable(GL_TEXTURE_2D);
    }
    glBegin(GL_TRIANGLES);
    for (i = 0; i < g_model.vertex_count; ++i) {
        const VLVertex *v = &g_model.vertices[i];
        float nx = v->skinned_normal[0], ny = v->skinned_normal[1], nz = v->skinned_normal[2];
        float diffuse = fmaxf(0.0f, nx * -0.34f + ny * -0.42f + nz * 0.84f);
        float shade = 0.27f + diffuse * 0.73f;
        shade = fminf(1.0f, fmaxf(0.2f, shade));
        if (!g_texture) set_color(0.36f * shade,0.72f * shade,0.46f * shade);
        else set_color(shade,shade,shade);
        glTexCoord2f(v->uv[0],v->uv[1]);
        glVertex3f(v->skinned_position[0],v->skinned_position[1],v->skinned_position[2]);
    }
    glEnd();
    if (g_texture) {
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
    }
}

static void draw_skeleton(void) {
    int i;
    if (!g_model.loaded || !g_pose_mode || g_model.bone_count == 0) return;
    glDisable(GL_TEXTURE_2D);
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    for (i = 0; i < g_model.bone_count; ++i) {
        const VLBone *bone = &g_model.bones[i];
        Vec3 head = transform_point4(bone->pose,bone->head_local);
        Vec3 tail = transform_point4(bone->pose,bone->tail_local);
        if (i == g_selected_bone) set_color(1.0f,0.47f,0.12f);
        else set_color(0.35f,0.77f,0.94f);
        glVertex3f(head.x,head.y,head.z);
        glVertex3f(tail.x,tail.y,tail.z);
    }
    glEnd();
}

static void render_3d_view(void) {
    float center_x = g_model.loaded ? g_model.center[0] : 0.0f;
    float center_y = g_model.loaded ? g_model.center[1] : 0.0f;
    float center_z = g_model.loaded ? g_model.center[2] : 0.0f;
    glViewport(VIEW_X, SCREEN_H - VIEW_Y - VIEW_H, VIEW_W, VIEW_H);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearColor(0.075f,0.083f,0.096f,1.0f);
    glEnable(GL_SCISSOR_TEST);
    glScissor(VIEW_X, SCREEN_H - VIEW_Y - VIEW_H, VIEW_W, VIEW_H);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(45.0f,(float)VIEW_W/(float)VIEW_H,0.05f,300.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f,0.0f,-g_camera_distance);
    glRotatef(g_camera_elevation,1.0f,0.0f,0.0f);
    glRotatef(g_camera_azimuth,0.0f,0.0f,1.0f);
    glTranslatef(-center_x,-center_y,-center_z);

    draw_grid();
    draw_model_mesh();
    draw_skeleton();
    glDisable(GL_DEPTH_TEST);
}

static void draw_ui_panels(void) {
    begin_2d();
    rect2d(0,0,SCREEN_W,SCREEN_H,0.075f,0.083f,0.095f);
    rect2d(0,0,SCREEN_W,46,0.10f,0.11f,0.13f);
    rect2d(0,46,SCREEN_W,1,0.23f,0.26f,0.30f);
    rect2d(VIEW_X,VIEW_Y,VIEW_W,VIEW_H,0.13f,0.14f,0.16f);
    rect2d(0,VIEW_Y,40,VIEW_H,0.09f,0.10f,0.12f);
    rect2d(680,VIEW_Y,280,166,0.095f,0.105f,0.12f);
    rect2d(680,222,280,196,0.095f,0.105f,0.12f);
    rect2d(0,420,SCREEN_W,75,0.09f,0.10f,0.12f);
    rect2d(0,495,SCREEN_W,49,0.07f,0.078f,0.09f);
    rect2d(680,VIEW_Y,280,1,0.24f,0.27f,0.31f);
    rect2d(680,222,280,1,0.24f,0.27f,0.31f);
    rect2d(0,420,SCREEN_W,1,0.24f,0.27f,0.31f);
    rect2d(VIEW_X,VIEW_Y,VIEW_W,1,0.25f,0.28f,0.31f);
    rect2d(VIEW_X,VIEW_Y+VIEW_H-1,VIEW_W,1,0.25f,0.28f,0.31f);
    rect2d(VIEW_X,VIEW_Y,1,VIEW_H,0.25f,0.28f,0.31f);
    rect2d(VIEW_X+VIEW_W-1,VIEW_Y,1,VIEW_H,0.25f,0.28f,0.31f);
}

static void draw_outliner(void) {
    int i;
    char label[84];
    text2d(694,62,1.25f,0.90f,0.92f,0.94f,"OUTLINER");
    text2d(694,77,1.0f,0.46f,0.52f,0.58f,"SCENE COLLECTION");
    if (!g_model.loaded) {
        text2d(704,100,1.1f,0.54f,0.59f,0.64f,"NO PROJECT");
        text2d(704,120,1.0f,0.54f,0.59f,0.64f,"USE OPEN");
        return;
    }
    for (i = 0; i < 6; ++i) {
        int index = g_bone_scroll + i;
        int y = 94 + i * 19;
        if (index >= g_model.bone_count) break;
        if (index == g_selected_bone) rect2d(690,(float)y-3,260,18,0.17f,0.27f,0.20f);
        snprintf(label,sizeof(label),"%s %.27s",index == g_selected_bone ? ">" : " ",g_model.bones[index].name);
        text2d(704,(float)y,1.0f,index == g_selected_bone ? 0.56f : 0.79f,
               index == g_selected_bone ? 1.0f : 0.82f,index == g_selected_bone ? 0.46f : 0.86f,label);
    }
}

static void draw_properties(void) {
    char text[72];
    int axis;
    float low, high, factor;
    const char *value_label;
    const char *units;
    text2d(694,233,1.25f,0.90f,0.92f,0.94f,"TRANSFORM");
    if (!g_model.loaded || g_model.bone_count == 0) {
        text2d(694,257,1.0f,0.53f,0.59f,0.64f,"LOAD A VLMESH");
        return;
    }
    snprintf(text,sizeof(text),"BONE: %.25s",g_model.bones[g_selected_bone].name);
    text2d(694,251,1.0f,0.56f,0.94f,0.48f,text);
    button2d(694,263,54,24,"X",g_active_axis==0);
    button2d(755,263,54,24,"Y",g_active_axis==1);
    button2d(816,263,54,24,"Z",g_active_axis==2);
    if (g_transform_tool == 0) {
        text2d(696,300,1.0f,0.55f,0.96f,0.48f,"SELECT TOOL");
        text2d(696,321,0.9f,0.52f,0.57f,0.62f,"TAP OUTLINER OR BONE");
        button2d(694,382,48,24,"-5",0);
        button2d(748,382,48,24,"+5",0);
        button2d(802,382,66,24,"RESET",0);
        button2d(876,382,58,24,"POSE",g_pose_mode);
        return;
    }
    value_label = g_transform_tool == 1 ? "LOC" : (g_transform_tool == 2 ? "ROT" : "SCALE");
    units = g_transform_tool == 1 ? "M" : (g_transform_tool == 2 ? "DEG" : "X");
    for (axis = 0; axis < 3; ++axis) {
        const char *axis_label = axis == 0 ? "X" : (axis == 1 ? "Y" : "Z");
        float value = g_transform_tool == 1
            ? g_model.bones[g_selected_bone].basis_location[axis]
            : (g_transform_tool == 2
                ? g_model.bones[g_selected_bone].basis_rotation[axis] * 57.295779513f
                : g_model.bones[g_selected_bone].basis_scale[axis]);
        snprintf(text,sizeof(text),"%s %s %7.2f %s",axis_label,value_label,value,units);
        text2d(696,295.0f + (float)axis * 19.0f,0.82f,
               axis==g_active_axis ? 0.56f : 0.75f,axis==g_active_axis ? 1.0f : 0.80f,
               axis==g_active_axis ? 0.48f : 0.84f,text);
    }
    low = active_transform_min();
    high = active_transform_max();
    factor = (active_transform_value() - low) / (high - low);
    factor = fmaxf(0.0f, fminf(1.0f, factor));
    rect2d(696,355,238,3,0.20f,0.24f,0.28f);
    rect2d(696,355,238 * factor,3,0.50f,0.94f,0.43f);
    rect2d(696 + 238 * factor - 3,349,6,15,0.85f,1.0f,0.79f);
    if (g_transform_tool == 1) {
        text2d(696,365,0.9f,0.52f,0.57f,0.62f,"-2.0");
        text2d(902,365,0.9f,0.52f,0.57f,0.62f,"+2.0");
    } else if (g_transform_tool == 2) {
        text2d(696,365,0.9f,0.52f,0.57f,0.62f,"-180");
        text2d(904,365,0.9f,0.52f,0.57f,0.62f,"180");
    } else {
        text2d(696,365,0.9f,0.52f,0.57f,0.62f,"0.1");
        text2d(904,365,0.9f,0.52f,0.57f,0.62f,"3.0");
    }
    button2d(694,382,48,24,"-5",0);
    button2d(748,382,48,24,"+5",0);
    button2d(802,382,66,24,"RESET",0);
    button2d(876,382,58,24,"POSE",g_pose_mode);
}

static void draw_toolbar(void) {
    button2d(4,62,32,32,"SEL",g_transform_tool==0);
    button2d(4,101,32,32,"MOV",g_transform_tool==1);
    button2d(4,140,32,32,"ROT",g_transform_tool==2);
    button2d(4,179,32,32,"SCL",g_transform_tool==3);
    text2d(8,238,0.9f,0.46f,0.52f,0.58f,"3D");
    text2d(8,252,0.9f,0.46f,0.52f,0.58f,"VIEW");
}

static void draw_timeline(void) {
    int i;
    char label[40];
    text2d(16,431,1.15f,0.89f,0.91f,0.93f,"TIMELINE");
    text2d(16,449,0.9f,0.49f,0.55f,0.61f,"FRAME");
    snprintf(label,sizeof(label),"%04d",g_frame);
    text2d(58,449,1.05f,0.55f,0.96f,0.49f,label);
    line2d(150,436,150,487,0.23f,0.27f,0.31f);
    for (i = 0; i <= 80; ++i) {
        float x = 168.0f + (float)i * 9.0f;
        if (x > 942.0f) break;
        float height = (i % 10 == 0) ? 15.0f : (i % 5 == 0 ? 9.0f : 5.0f);
        line2d(x,455.0f-height*0.5f,x,455.0f+height*0.5f,
               i == (g_frame % 80) ? 0.45f : 0.33f,
               i == (g_frame % 80) ? 0.95f : 0.37f,
               i == (g_frame % 80) ? 0.38f : 0.42f);
    }
    line2d(168.0f + (float)(g_frame % 80) * 9.0f,437.0f,
           168.0f + (float)(g_frame % 80) * 9.0f,480.0f,0.96f,0.42f,0.15f);
}

static void draw_ui_overlay(void) {
    begin_2d();
    text2d(13,13,1.8f,0.53f,1.0f,0.45f,"VLENDER");
    text2d(126,18,1.0f,0.53f,0.59f,0.64f,"3D STUDIO");
    button2d(250,9,80,28,"LAYOUT",1);
    button2d(336,9,64,28,"POSE",g_pose_mode);
    button2d(406,9,70,28,"OBJECT",!g_pose_mode);
    button2d(691,9,56,28,"OPEN",0);
    button2d(752,9,56,28,"LOAD",0);
    button2d(813,9,56,28,"SAVE",0);
    button2d(874,9,72,28,"MODE",g_pose_mode);
    draw_toolbar();
    draw_outliner();
    draw_properties();
    draw_timeline();
    text2d(12,510,0.95f,0.75f,0.80f,0.84f,g_status);
    text2d(12,528,0.9f,0.42f,0.49f,0.55f,"D-PAD SELECT / LEFT-RIGHT ROTATE / CROSS AXIS / START EXIT");
    text2d(692,528,0.9f,0.42f,0.49f,0.55f,"TOUCH: ORBIT / SELECT / ADJUST");
    text2d(56,61,0.95f,0.80f,0.84f,0.87f,g_model.loaded ? "MESH WORKSPACE" : "NO PROJECT LOADED");
    if (!g_model.loaded) text2d(210,252,1.4f,0.88f,0.91f,0.92f,"3D VIEWPORT READY");
    /* Small XYZ viewport gizmo */
    line2d(621,75,651,75,0.90f,0.30f,0.30f);
    line2d(621,75,621,103,0.30f,0.90f,0.44f);
    line2d(621,75,603,93,0.30f,0.62f,1.0f);
    text2d(654,72,0.9f,0.98f,0.42f,0.42f,"X");
    text2d(616,107,0.9f,0.39f,0.95f,0.51f,"Y");
    text2d(595,95,0.9f,0.40f,0.70f,1.0f,"Z");
}

static void draw_frame(void) {
    glViewport(0,0,SCREEN_W,SCREEN_H);
    glClearColor(0.075f,0.083f,0.095f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    draw_ui_panels();
    render_3d_view();
    draw_ui_overlay();
    vglSwapBuffers(GL_FALSE);
}

static void set_slider_from_x(int x) {
    float factor = (float)(x - 696) / 238.0f;
    float low = active_transform_min();
    float high = active_transform_max();
    factor = fmaxf(0.0f, fminf(1.0f, factor));
    set_active_transform(low + factor * (high - low));
}

static void on_touch_press(int x, int y) {
    if (y < 45) {
        if (x >= 691 && x < 750) load_project();
        else if (x >= 752 && x < 810) load_pose();
        else if (x >= 813 && x < 870) save_pose();
        else if (x >= 874 && x < 948) g_pose_mode = !g_pose_mode;
        else if (x >= 336 && x < 400) g_pose_mode = 1;
        else if (x >= 406 && x < 476) g_pose_mode = 0;
        return;
    }
    if (x >= 4 && x <= 38) {
        if (y >= 62 && y <= 94) g_transform_tool = 0;
        else if (y >= 101 && y <= 133) g_transform_tool = 1;
        else if (y >= 140 && y <= 172) g_transform_tool = 2;
        else if (y >= 179 && y <= 211) g_transform_tool = 3;
        return;
    }
    if (x >= 690 && x <= 952 && y >= 88 && y < 210) {
        int row = (y - 91) / 19;
        select_bone(g_bone_scroll + row);
        return;
    }
    if (x >= 694 && x <= 870 && y >= 263 && y < 287) {
        g_active_axis = x < 752 ? 0 : (x < 813 ? 1 : 2);
        return;
    }
    if (x >= 690 && x <= 940 && y >= 348 && y <= 359) {
        g_drag_slider = 1;
        set_slider_from_x(x);
        return;
    }
    if (x >= 694 && x < 744 && y >= 382 && y < 408) { adjust_rotation(-5.0f); return; }
    if (x >= 748 && x < 798 && y >= 382 && y < 408) { adjust_rotation(5.0f); return; }
    if (x >= 802 && x < 870 && y >= 382 && y < 408) { reset_active_transform(); return; }
    if (x >= 876 && x < 940 && y >= 382 && y < 408) { g_pose_mode = !g_pose_mode; return; }
    if (x >= 44 && x <= 676 && y >= 52 && y <= 418) {
        g_drag_camera = 1;
    }
    g_touch_x = x;
    g_touch_y = y;
}

static int update_input(void) {
    SceCtrlData pad;
    SceTouchData touch;
    unsigned int pressed;
    int touch_is_down;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    pressed = pad.buttons & ~(unsigned int)g_previous_buttons;
    g_previous_buttons = (int)pad.buttons;

    if (pressed & SCE_CTRL_START) return 0;
    if (g_model.loaded && g_model.bone_count > 0 && g_pose_mode) {
        if (pressed & SCE_CTRL_UP) select_bone(g_selected_bone - 1);
        if (pressed & SCE_CTRL_DOWN) select_bone(g_selected_bone + 1);
        if (pressed & SCE_CTRL_LEFT) adjust_rotation(-5.0f);
        if (pressed & SCE_CTRL_RIGHT) adjust_rotation(5.0f);
        if (pressed & SCE_CTRL_CROSS) g_active_axis = (g_active_axis + 1) % 3;
        if (pressed & SCE_CTRL_CIRCLE) reset_active_transform();
        if (pressed & SCE_CTRL_SQUARE) load_pose();
        if (pressed & SCE_CTRL_TRIANGLE) save_pose();
        if (pressed & SCE_CTRL_LTRIGGER) adjust_rotation(-1.0f);
        if (pressed & SCE_CTRL_RTRIGGER) adjust_rotation(1.0f);
    } else {
        if (pressed & SCE_CTRL_TRIANGLE) save_pose();
        if (pressed & SCE_CTRL_SQUARE) load_pose();
    }

    /* Analog sticks orbit and zoom the actual 3D camera. */
    if (abs((int)pad.lx - 128) > 16) g_camera_azimuth += ((float)pad.lx - 128.0f) * 0.0025f;
    if (abs((int)pad.ly - 128) > 16) g_camera_elevation = fmaxf(-85.0f, fminf(-12.0f, g_camera_elevation + ((float)pad.ly - 128.0f) * 0.0020f));
    if (abs((int)pad.rx - 128) > 24) g_camera_distance = fmaxf(1.0f, fminf(40.0f, g_camera_distance + ((float)pad.rx - 128.0f) * 0.005f));

    memset(&touch, 0, sizeof(touch));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    touch_is_down = touch.reportNum > 0;
    if (touch_is_down) {
        int x = (int)touch.report[0].x / 2;
        int y = (int)touch.report[0].y / 2;
        if (!g_touch_down) {
            on_touch_press(x,y);
            g_touch_start_x = x;
            g_touch_start_y = y;
            g_touch_moved = 0;
        } else {
            if (abs(x - g_touch_start_x) > 6 || abs(y - g_touch_start_y) > 6) g_touch_moved = 1;
            if (g_drag_camera) {
                int dx = x - g_touch_x;
                int dy = y - g_touch_y;
                g_camera_azimuth += (float)dx * 0.45f;
                g_camera_elevation = fmaxf(-85.0f, fminf(-12.0f, g_camera_elevation + (float)dy * 0.35f));
            }
            if (g_drag_slider) set_slider_from_x(x);
        }
        g_touch_x = x;
        g_touch_y = y;
    } else {
        if (g_drag_camera && !g_touch_moved &&
            g_touch_start_x >= VIEW_X && g_touch_start_x <= VIEW_X + VIEW_W &&
            g_touch_start_y >= VIEW_Y && g_touch_start_y <= VIEW_Y + VIEW_H) {
            (void)pick_bone_at(g_touch_x, g_touch_y);
        }
        g_drag_camera = 0;
        g_drag_slider = 0;
    }
    g_touch_down = touch_is_down;
    return 1;
}

int main(void) {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    ensure_data_directory();
    vl_model_init(&g_model);

    vglInit(0x800000);
    vglWaitVblankStart(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);

    if (!load_project()) {
        g_model.loaded = 0;
        g_camera_distance = 4.2f;
    }

    for (;;) {
        if (!update_input()) break;
        draw_frame();
        sceDisplayWaitVblankStart();
    }

    if (g_texture) glDeleteTextures(1,&g_texture);
    vl_model_free(&g_model);
    sceKernelExitProcess(0);
    return 0;
}
