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
#include "font_atlas.h"

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
#define PATH_ANIM_IN "ux0:data/VLENDER/input.vlanim"
#define PATH_ANIM_OUT "ux0:data/VLENDER/output.vlanim"
#define VLENDER_TIMELINE_FRAMES 80

typedef struct { float x, y, z; } Vec3;

static VLModel g_model;
static GLuint g_texture = 0;
static GLuint g_font_texture = 0;
static unsigned char g_font_rle_decoded[12000];
static unsigned char g_font_atlas_pixels[VL_FONT_RAW_SIZE];
static int g_pose_mode = 1;
static int g_show_grid = 1;
static int g_show_skeleton = 1;
static int g_show_texture = 1;
static int g_show_wireframe = 0;
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
static int g_playing = 0;
static int g_play_tick = 0;
static int g_open_menu = 0; /* 0 closed, 1 File, 2 Edit, 3 Render, 4 Window, 5 Help */
static int g_has_transform_clipboard = 0;
static float g_clipboard_location[3];
static float g_clipboard_rotation[3];
static float g_clipboard_scale[3];

#define POSE_HISTORY_CAPACITY 24
typedef struct {
    int bone_index;
    float location[3];
    float rotation[3];
    float scale[3];
} PoseSnapshot;

typedef struct {
    int valid;
    float trs[VLENDER_MAX_BONES][9]; /* rotation XYZ, location XYZ, scale XYZ */
} VLAnimationFrame;
static VLAnimationFrame g_animation[VLENDER_TIMELINE_FRAMES];

static PoseSnapshot g_undo_history[POSE_HISTORY_CAPACITY];
static PoseSnapshot g_redo_history[POSE_HISTORY_CAPACITY];
static int g_undo_count = 0;
static int g_redo_count = 0;

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

static int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t decode_base64(const char *src, unsigned char *dst, size_t capacity) {
    size_t written = 0;
    unsigned int accumulator = 0;
    int bits = 0;
    while (*src && *src != '=') {
        int value = base64_value(*src++);
        if (value < 0) continue;
        accumulator = (accumulator << 6) | (unsigned int)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written >= capacity) return 0;
            dst[written++] = (unsigned char)((accumulator >> bits) & 0xffu);
            accumulator &= bits ? ((1u << bits) - 1u) : 0u;
        }
    }
    return written;
}

static int font_init(void) {
    size_t encoded_size;
    size_t in = 0, out = 0;
    int y;
    unsigned char row[VL_FONT_ATLAS_W];

    encoded_size = decode_base64(g_font_atlas_rle_b64, g_font_rle_decoded,
                                 sizeof(g_font_rle_decoded));
    if (encoded_size < 2 || (encoded_size & 1u) != 0u) return 0;
    while (in + 1 < encoded_size) {
        size_t run = g_font_rle_decoded[in++];
        unsigned char value = g_font_rle_decoded[in++];
        if (run == 0 || out + run > VL_FONT_RAW_SIZE) return 0;
        memset(g_font_atlas_pixels + out, value, run);
        out += run;
    }
    if (out != VL_FONT_RAW_SIZE) return 0;

    /* OpenGL's first texture row is the bottom row; flip the top-down raster. */
    for (y = 0; y < VL_FONT_ATLAS_H / 2; ++y) {
        unsigned char *top = g_font_atlas_pixels + (size_t)y * VL_FONT_ATLAS_W;
        unsigned char *bottom = g_font_atlas_pixels +
            (size_t)(VL_FONT_ATLAS_H - 1 - y) * VL_FONT_ATLAS_W;
        memcpy(row, top, sizeof(row));
        memcpy(top, bottom, sizeof(row));
        memcpy(bottom, row, sizeof(row));
    }

    glGenTextures(1, &g_font_texture);
    glBindTexture(GL_TEXTURE_2D, g_font_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, VL_FONT_ATLAS_W,
                 VL_FONT_ATLAS_H, 0, GL_ALPHA, GL_UNSIGNED_BYTE,
                 g_font_atlas_pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    return g_font_texture != 0;
}

static void font_shutdown(void) {
    if (g_font_texture) {
        GLuint old = g_font_texture;
        glDeleteTextures(1, &old);
        g_font_texture = 0;
    }
}

static float text_width(float scale, const char *text) {
    float width = 0.0f;
    const float font_scale = scale * 0.78f;
    if (!text) return 0.0f;
    while (*text) {
        int code = (unsigned char)*text++;
        if (code < VL_FONT_FIRST || code >= VL_FONT_FIRST + VL_FONT_COUNT - 1) code = '?';
        width += (float)g_font_glyphs[code - VL_FONT_FIRST].advance_x4 * 0.25f * font_scale;
    }
    return width;
}

static void text2d(float x, float y, float scale, float r, float g, float b, const char *text) {
    float cursor = x;
    const float font_scale = scale * 0.78f;
    if (!text || !g_font_texture) return;

    set_color(r, g, b);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_font_texture);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glBegin(GL_QUADS);
    for (; *text; ++text) {
        int code = (unsigned char)*text;
        const VLFontGlyph *glyph;
        int index, cell_x, cell_y, source_y;
        float u0, u1, v0, v1, gx, gy, gw, gh;
        if (code < VL_FONT_FIRST || code >= VL_FONT_FIRST + VL_FONT_COUNT - 1) code = '?';
        index = code - VL_FONT_FIRST;
        glyph = &g_font_glyphs[index];
        if (glyph->width && glyph->height) {
            cell_x = (index % VL_FONT_COLS) * VL_FONT_CELL_W;
            cell_y = (index / VL_FONT_COLS) * VL_FONT_CELL_H;
            source_y = cell_y + glyph->top + 4;
            u0 = (float)(cell_x + 2) / (float)VL_FONT_ATLAS_W;
            u1 = (float)(cell_x + 2 + glyph->width) / (float)VL_FONT_ATLAS_W;
            v0 = 1.0f - (float)source_y / (float)VL_FONT_ATLAS_H;
            v1 = 1.0f - (float)(source_y + glyph->height) / (float)VL_FONT_ATLAS_H;
            gx = cursor + (float)glyph->left * font_scale;
            gy = y + (float)glyph->top * font_scale;
            gw = (float)glyph->width * font_scale;
            gh = (float)glyph->height * font_scale;
            glTexCoord2f(u0, v0); glVertex2f(gx, gy);
            glTexCoord2f(u1, v0); glVertex2f(gx + gw, gy);
            glTexCoord2f(u1, v1); glVertex2f(gx + gw, gy + gh);
            glTexCoord2f(u0, v1); glVertex2f(gx, gy + gh);
        }
        cursor += (float)glyph->advance_x4 * 0.25f * font_scale;
    }
    glEnd();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}

static void centered_text(float center_x, float y, float scale, float r, float g, float b, const char *text) {
    float width = text_width(scale, text);
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

/* Compact, local Material-style vector symbols. All icon geometry is built in,
   so the Vita never needs to load a font or connect to the internet. */
enum {
    ICON_SELECT = 0, ICON_MOVE, ICON_ROTATE, ICON_SCALE,
    ICON_LAYOUT, ICON_POSE, ICON_OBJECT, ICON_OPEN,
    ICON_LOAD, ICON_SAVE, ICON_MODE, ICON_MINUS,
    ICON_PLUS, ICON_RESET, ICON_FIT, ICON_GRID, ICON_BONES,
    ICON_UNDO, ICON_REDO, ICON_TEXTURE, ICON_WIREFRAME, ICON_COPY, ICON_PASTE,
    ICON_KEYFRAME, ICON_PLAY, ICON_STOP, ICON_PREV, ICON_NEXT
};

static void icon_line(float ox, float oy, float s,
                      float ax, float ay, float bx, float by,
                      float r, float g, float b) {
    line2d(ox + ax*s, oy + ay*s, ox + bx*s, oy + by*s, r, g, b);
}

static void icon_rect(float ox, float oy, float s,
                      float x, float y, float w, float h,
                      float r, float g, float b) {
    icon_line(ox,oy,s,x,y,x+w,y,r,g,b);
    icon_line(ox,oy,s,x+w,y,x+w,y+h,r,g,b);
    icon_line(ox,oy,s,x+w,y+h,x,y+h,r,g,b);
    icon_line(ox,oy,s,x,y+h,x,y,r,g,b);
}

static void icon_circle(float ox, float oy, float s,
                        float cx, float cy, float radius,
                        float r, float g, float b) {
    int i;
    set_color(r,g,b);
    glBegin(GL_LINE_LOOP);
    for (i = 0; i < 20; ++i) {
        float a = (float)i * 6.28318530718f / 20.0f;
        glVertex2f(ox + (cx + cosf(a)*radius)*s,
                   oy + (cy + sinf(a)*radius)*s);
    }
    glEnd();
}

static void draw_icon2d(int icon, float x, float y, float size,
                        float r, float g, float b) {
    float s = size / 24.0f;
    switch (icon) {
        case ICON_SELECT:
            icon_line(x,y,s,5,3,5,20,r,g,b);
            icon_line(x,y,s,5,3,19,14,r,g,b);
            icon_line(x,y,s,19,14,13,15,r,g,b);
            icon_line(x,y,s,13,15,16,21,r,g,b);
            icon_line(x,y,s,16,21,13,22,r,g,b);
            icon_line(x,y,s,13,22,10,16,r,g,b);
            icon_line(x,y,s,10,16,5,20,r,g,b);
            break;
        case ICON_MOVE:
            icon_line(x,y,s,12,4,12,20,r,g,b);
            icon_line(x,y,s,4,12,20,12,r,g,b);
            icon_line(x,y,s,12,4,8,8,r,g,b);
            icon_line(x,y,s,12,4,16,8,r,g,b);
            icon_line(x,y,s,12,20,8,16,r,g,b);
            icon_line(x,y,s,12,20,16,16,r,g,b);
            icon_line(x,y,s,4,12,8,8,r,g,b);
            icon_line(x,y,s,4,12,8,16,r,g,b);
            icon_line(x,y,s,20,12,16,8,r,g,b);
            icon_line(x,y,s,20,12,16,16,r,g,b);
            break;
        case ICON_ROTATE:
            icon_circle(x,y,s,12,12,7.5f,r,g,b);
            icon_line(x,y,s,15,3.8f,20,4.5f,r,g,b);
            icon_line(x,y,s,20,4.5f,19.1f,9.3f,r,g,b);
            break;
        case ICON_SCALE:
            icon_line(x,y,s,6,18,18,6,r,g,b);
            icon_line(x,y,s,6,18,6,12,r,g,b);
            icon_line(x,y,s,6,18,12,18,r,g,b);
            icon_line(x,y,s,18,6,12,6,r,g,b);
            icon_line(x,y,s,18,6,18,12,r,g,b);
            icon_line(x,y,s,5,5,9,5,r,g,b);
            icon_line(x,y,s,5,5,5,9,r,g,b);
            icon_line(x,y,s,19,19,15,19,r,g,b);
            icon_line(x,y,s,19,19,19,15,r,g,b);
            break;
        case ICON_LAYOUT:
            icon_rect(x,y,s,3,3,18,18,r,g,b);
            icon_line(x,y,s,12,3,12,21,r,g,b);
            icon_line(x,y,s,3,12,21,12,r,g,b);
            break;
        case ICON_POSE:
            icon_circle(x,y,s,12,4,2.0f,r,g,b);
            icon_circle(x,y,s,12,10,1.7f,r,g,b);
            icon_circle(x,y,s,6,15,1.5f,r,g,b);
            icon_circle(x,y,s,18,15,1.5f,r,g,b);
            icon_circle(x,y,s,6,21,1.3f,r,g,b);
            icon_circle(x,y,s,18,21,1.3f,r,g,b);
            icon_line(x,y,s,12,6,12,10,r,g,b);
            icon_line(x,y,s,12,10,6,15,r,g,b);
            icon_line(x,y,s,12,10,18,15,r,g,b);
            icon_line(x,y,s,6,15,6,21,r,g,b);
            icon_line(x,y,s,18,15,18,21,r,g,b);
            break;
        case ICON_OBJECT:
            icon_line(x,y,s,12,3,21,8,r,g,b);
            icon_line(x,y,s,21,8,12,13,r,g,b);
            icon_line(x,y,s,12,13,3,8,r,g,b);
            icon_line(x,y,s,3,8,12,3,r,g,b);
            icon_line(x,y,s,3,8,3,18,r,g,b);
            icon_line(x,y,s,3,18,12,22,r,g,b);
            icon_line(x,y,s,12,22,21,18,r,g,b);
            icon_line(x,y,s,21,18,21,8,r,g,b);
            icon_line(x,y,s,12,13,12,22,r,g,b);
            break;
        case ICON_OPEN:
            icon_line(x,y,s,3,8,9,8,r,g,b);
            icon_line(x,y,s,9,8,11,10,r,g,b);
            icon_line(x,y,s,11,10,21,10,r,g,b);
            icon_line(x,y,s,21,10,19,20,r,g,b);
            icon_line(x,y,s,19,20,3,20,r,g,b);
            icon_line(x,y,s,3,20,3,8,r,g,b);
            icon_line(x,y,s,3,8,3,5,r,g,b);
            icon_line(x,y,s,3,5,9,5,r,g,b);
            icon_line(x,y,s,9,5,11,8,r,g,b);
            icon_line(x,y,s,11,8,20,8,r,g,b);
            break;
        case ICON_LOAD:
            icon_line(x,y,s,12,3,12,15,r,g,b);
            icon_line(x,y,s,12,15,7,10,r,g,b);
            icon_line(x,y,s,12,15,17,10,r,g,b);
            icon_line(x,y,s,4,16,4,21,r,g,b);
            icon_line(x,y,s,4,21,20,21,r,g,b);
            icon_line(x,y,s,20,21,20,16,r,g,b);
            break;
        case ICON_SAVE:
            icon_rect(x,y,s,4,3,16,18,r,g,b);
            icon_rect(x,y,s,8,3,8,6,r,g,b);
            icon_rect(x,y,s,7,13,10,8,r,g,b);
            icon_line(x,y,s,16,4,16,8,r,g,b);
            break;
        case ICON_MODE:
            icon_line(x,y,s,4,7,20,7,r,g,b);
            icon_line(x,y,s,4,17,20,17,r,g,b);
            icon_circle(x,y,s,9,7,2.5f,r,g,b);
            icon_circle(x,y,s,15,17,2.5f,r,g,b);
            break;
        case ICON_MINUS:
            icon_line(x,y,s,5,12,19,12,r,g,b);
            break;
        case ICON_PLUS:
            icon_line(x,y,s,5,12,19,12,r,g,b);
            icon_line(x,y,s,12,5,12,19,r,g,b);
            break;
        case ICON_RESET:
            icon_circle(x,y,s,12,12,7.5f,r,g,b);
            icon_line(x,y,s,12,3,18,4,r,g,b);
            icon_line(x,y,s,18,4,17,9,r,g,b);
            break;
        case ICON_FIT:
            icon_line(x,y,s,3,8,3,3,r,g,b);
            icon_line(x,y,s,3,3,8,3,r,g,b);
            icon_line(x,y,s,16,3,21,3,r,g,b);
            icon_line(x,y,s,21,3,21,8,r,g,b);
            icon_line(x,y,s,3,16,3,21,r,g,b);
            icon_line(x,y,s,3,21,8,21,r,g,b);
            icon_line(x,y,s,16,21,21,21,r,g,b);
            icon_line(x,y,s,21,21,21,16,r,g,b);
            icon_line(x,y,s,8,8,16,16,r,g,b);
            icon_line(x,y,s,16,8,8,16,r,g,b);
            break;
        case ICON_GRID:
            icon_rect(x,y,s,3,3,18,18,r,g,b);
            icon_line(x,y,s,9,3,9,21,r,g,b);
            icon_line(x,y,s,15,3,15,21,r,g,b);
            icon_line(x,y,s,3,9,21,9,r,g,b);
            icon_line(x,y,s,3,15,21,15,r,g,b);
            break;
        case ICON_BONES:
            icon_circle(x,y,s,6,4,2.2f,r,g,b);
            icon_circle(x,y,s,18,10,2.2f,r,g,b);
            icon_circle(x,y,s,8,20,2.2f,r,g,b);
            icon_line(x,y,s,7.8f,5.2f,16.2f,8.8f,r,g,b);
            icon_line(x,y,s,16.6f,12.0f,9.4f,18.0f,r,g,b);
            break;
        case ICON_UNDO:
            icon_line(x,y,s,9,7,4,12,r,g,b);
            icon_line(x,y,s,4,12,9,17,r,g,b);
            icon_line(x,y,s,4,12,13,12,r,g,b);
            icon_line(x,y,s,13,12,18,15,r,g,b);
            icon_line(x,y,s,18,15,18,19,r,g,b);
            break;
        case ICON_REDO:
            icon_line(x,y,s,15,7,20,12,r,g,b);
            icon_line(x,y,s,20,12,15,17,r,g,b);
            icon_line(x,y,s,20,12,11,12,r,g,b);
            icon_line(x,y,s,11,12,6,15,r,g,b);
            icon_line(x,y,s,6,15,6,19,r,g,b);
            break;
        case ICON_TEXTURE:
            icon_rect(x,y,s,3,4,18,16,r,g,b);
            icon_circle(x,y,s,8,9,2.2f,r,g,b);
            icon_line(x,y,s,4,18,9,13,r,g,b);
            icon_line(x,y,s,9,13,13,16,r,g,b);
            icon_line(x,y,s,13,16,17,11,r,g,b);
            icon_line(x,y,s,17,11,21,16,r,g,b);
            break;
        case ICON_WIREFRAME:
            icon_line(x,y,s,12,3,21,8,r,g,b);
            icon_line(x,y,s,21,8,12,13,r,g,b);
            icon_line(x,y,s,12,13,3,8,r,g,b);
            icon_line(x,y,s,3,8,12,3,r,g,b);
            icon_line(x,y,s,3,8,3,18,r,g,b);
            icon_line(x,y,s,3,18,12,22,r,g,b);
            icon_line(x,y,s,12,22,21,18,r,g,b);
            icon_line(x,y,s,21,18,21,8,r,g,b);
            icon_line(x,y,s,12,13,12,22,r,g,b);
            icon_line(x,y,s,3,18,12,13,r,g,b);
            icon_line(x,y,s,12,13,21,18,r,g,b);
            break;
        case ICON_COPY:
            icon_rect(x,y,s,8,7,12,14,r,g,b);
            icon_line(x,y,s,5,16,3,16,r,g,b);
            icon_line(x,y,s,3,16,3,3,r,g,b);
            icon_line(x,y,s,3,3,15,3,r,g,b);
            icon_line(x,y,s,15,3,15,5,r,g,b);
            break;
        case ICON_PASTE:
            icon_rect(x,y,s,5,5,14,17,r,g,b);
            icon_rect(x,y,s,9,2,6,5,r,g,b);
            icon_line(x,y,s,8,12,16,12,r,g,b);
            icon_line(x,y,s,8,16,16,16,r,g,b);
            break;
        case ICON_KEYFRAME:
            icon_line(x,y,s,12,3,21,12,r,g,b);
            icon_line(x,y,s,21,12,12,21,r,g,b);
            icon_line(x,y,s,12,21,3,12,r,g,b);
            icon_line(x,y,s,3,12,12,3,r,g,b);
            break;
        case ICON_PLAY:
            icon_line(x,y,s,7,4,19,12,r,g,b);
            icon_line(x,y,s,19,12,7,20,r,g,b);
            icon_line(x,y,s,7,20,7,4,r,g,b);
            break;
        case ICON_STOP:
            icon_rect(x,y,s,5,5,14,14,r,g,b);
            break;
        case ICON_PREV:
            icon_line(x,y,s,6,3,6,21,r,g,b);
            icon_line(x,y,s,19,4,8,12,r,g,b);
            icon_line(x,y,s,8,12,19,20,r,g,b);
            break;
        case ICON_NEXT:
            icon_line(x,y,s,18,3,18,21,r,g,b);
            icon_line(x,y,s,5,4,16,12,r,g,b);
            icon_line(x,y,s,16,12,5,20,r,g,b);
            break;
        default:
            break;
    }
}

static void button_icon2d(float x, float y, float w, float h,
                          int icon, int active) {
    float size = fminf(w,h) - 8.0f;
    button2d(x,y,w,h," ",active);
    draw_icon2d(icon,x + (w-size)*0.5f,y + (h-size)*0.5f,size,
                active ? 0.55f : 0.86f,
                active ? 1.0f : 0.89f,
                active ? 0.47f : 0.91f);
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
        if (g_model.loaded) {
            snprintf(g_status, sizeof(g_status), "%s",
                     "LOAD FAILED - CURRENT PROJECT KEPT");
        } else {
            snprintf(g_status, sizeof(g_status), "%s",
                     "NO PROJECT: EXPORT FROM BLENDER AND COPY model.vlmesh");
        }
        /* A failed transactional load leaves the previous model intact; keep
           its texture too instead of deleting a valid renderer resource. */
        return 0;
    }
    g_selected_bone = 0;
    g_bone_scroll = 0;
    g_undo_count = 0;
    g_redo_count = 0;
    g_has_transform_clipboard = 0;
    memset(g_animation, 0, sizeof(g_animation));
    g_frame = 1;
    g_playing = 0;
    g_play_tick = 0;
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
        snprintf(g_status, sizeof(g_status), "%s", "POSE LOAD FAILED - CURRENT POSE KEPT");
    } else {
        /* Loading a pose changes many bones at once; until bulk pose operations
           have grouped history snapshots, clear per-transform history rather
           than offering a misleading undo into an older state. */
        g_undo_count = 0;
        g_redo_count = 0;
        snprintf(g_status, sizeof(g_status), "%s", "POSE LOADED - HISTORY CLEARED");
    }
}

static PoseSnapshot capture_bone_snapshot(int bone_index) {
    PoseSnapshot snapshot;
    int i;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.bone_index = bone_index;
    if (!g_model.loaded || bone_index < 0 || bone_index >= g_model.bone_count) {
        snapshot.bone_index = -1;
        return snapshot;
    }
    for (i = 0; i < 3; ++i) {
        snapshot.location[i] = g_model.bones[bone_index].basis_location[i];
        snapshot.rotation[i] = g_model.bones[bone_index].basis_rotation[i];
        snapshot.scale[i] = g_model.bones[bone_index].basis_scale[i];
    }
    return snapshot;
}

static PoseSnapshot capture_pose_snapshot(void) {
    return capture_bone_snapshot(g_selected_bone);
}

static int snapshots_equal(const PoseSnapshot *a, const PoseSnapshot *b) {
    int i;
    if (a->bone_index != b->bone_index) return 0;
    for (i = 0; i < 3; ++i) {
        if (fabsf(a->location[i] - b->location[i]) > 0.000001f ||
            fabsf(a->rotation[i] - b->rotation[i]) > 0.000001f ||
            fabsf(a->scale[i] - b->scale[i]) > 0.000001f) return 0;
    }
    return 1;
}

static void history_push(PoseSnapshot *stack, int *count,
                         const PoseSnapshot *snapshot) {
    if (snapshot->bone_index < 0) return;
    if (*count > 0 && snapshots_equal(&stack[*count - 1], snapshot)) return;
    if (*count >= POSE_HISTORY_CAPACITY) {
        memmove(stack, stack + 1,
                sizeof(*stack) * (POSE_HISTORY_CAPACITY - 1));
        *count = POSE_HISTORY_CAPACITY - 1;
    }
    stack[(*count)++] = *snapshot;
}

static void history_record_undo(void) {
    PoseSnapshot current;
    if (!g_model.loaded || g_model.bone_count <= 0) return;
    current = capture_pose_snapshot();
    history_push(g_undo_history, &g_undo_count, &current);
    g_redo_count = 0;
}

static void apply_pose_snapshot(const PoseSnapshot *snapshot) {
    int i;
    if (!snapshot || snapshot->bone_index < 0 ||
        snapshot->bone_index >= g_model.bone_count) return;
    g_selected_bone = snapshot->bone_index;
    if (g_selected_bone < g_bone_scroll) g_bone_scroll = g_selected_bone;
    if (g_selected_bone >= g_bone_scroll + 6) g_bone_scroll = g_selected_bone - 5;
    for (i = 0; i < 3; ++i) {
        g_model.bones[g_selected_bone].basis_location[i] = snapshot->location[i];
        g_model.bones[g_selected_bone].basis_rotation[i] = snapshot->rotation[i];
        g_model.bones[g_selected_bone].basis_scale[i] = snapshot->scale[i];
    }
    vl_model_update_skin(&g_model);
}

static void undo_transform(void) {
    PoseSnapshot previous;
    PoseSnapshot current;
    if (!g_model.loaded || g_model.bone_count <= 0 || g_undo_count <= 0) {
        snprintf(g_status, sizeof(g_status), "%s", "NOTHING TO UNDO");
        return;
    }
    previous = g_undo_history[--g_undo_count];
    /* History entries identify the bone that was edited. Selection may have
       changed since then, so redo must capture that bone, not today's selection. */
    current = capture_bone_snapshot(previous.bone_index);
    history_push(g_redo_history, &g_redo_count, &current);
    apply_pose_snapshot(&previous);
    snprintf(g_status, sizeof(g_status), "%s", "UNDO TRANSFORM");
}

static void redo_transform(void) {
    PoseSnapshot next;
    PoseSnapshot current;
    if (!g_model.loaded || g_model.bone_count <= 0 || g_redo_count <= 0) {
        snprintf(g_status, sizeof(g_status), "%s", "NOTHING TO REDO");
        return;
    }
    next = g_redo_history[--g_redo_count];
    current = capture_bone_snapshot(next.bone_index);
    history_push(g_undo_history, &g_undo_count, &current);
    apply_pose_snapshot(&next);
    snprintf(g_status, sizeof(g_status), "%s", "REDO TRANSFORM");
}

static void encode_bone_name(const char *source, char *dest, size_t capacity) {
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    size_t i;
    if (!capacity) return;
    for (i = 0; source && source[i] && out + 4 < capacity; ++i) {
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
    dest[out] = '\0';
}

static int decode_bone_name(const char *source, char *dest, size_t capacity) {
    size_t out = 0, i = 0;
    if (!source || !capacity) return 0;
    while (source[i] && out + 1 < capacity) {
        if (source[i] == '%' && source[i+1] && source[i+2]) {
            char hi = source[i+1], lo = source[i+2];
            int a = (hi >= '0' && hi <= '9') ? hi-'0' :
                    (hi >= 'A' && hi <= 'F') ? hi-'A'+10 :
                    (hi >= 'a' && hi <= 'f') ? hi-'a'+10 : -1;
            int b = (lo >= '0' && lo <= '9') ? lo-'0' :
                    (lo >= 'A' && lo <= 'F') ? lo-'A'+10 :
                    (lo >= 'a' && lo <= 'f') ? lo-'a'+10 : -1;
            if (a >= 0 && b >= 0) {
                dest[out++] = (char)((a << 4) | b);
                i += 3;
                continue;
            }
        }
        dest[out++] = source[i++];
    }
    dest[out] = '\0';
    return 1;
}

static void capture_animation_frame(int frame) {
    int bone, axis;
    VLAnimationFrame *key;
    if (!g_model.loaded || frame < 1 || frame > VLENDER_TIMELINE_FRAMES) return;
    key = &g_animation[frame - 1];
    memset(key, 0, sizeof(*key));
    for (bone = 0; bone < g_model.bone_count; ++bone) {
        VLBone *b = &g_model.bones[bone];
        for (axis = 0; axis < 3; ++axis) {
            key->trs[bone][axis] = b->basis_rotation[axis];
            key->trs[bone][3 + axis] = b->basis_location[axis];
            key->trs[bone][6 + axis] = b->basis_scale[axis];
        }
    }
    key->valid = 1;
}

static float interpolate_angle(float a, float b, float t) {
    float delta = b - a;
    while (delta > 3.14159265359f) delta -= 6.28318530718f;
    while (delta < -3.14159265359f) delta += 6.28318530718f;
    return a + delta * t;
}

static int apply_animation_frame(int frame, int announce) {
    int left = -1, right = -1, i, bone, axis;
    float t = 0.0f;
    if (!g_model.loaded || frame < 1 || frame > VLENDER_TIMELINE_FRAMES) return 0;
    for (i = 0; i < VLENDER_TIMELINE_FRAMES; ++i) {
        if (!g_animation[i].valid) continue;
        if (i <= frame - 1) left = i;
        if (i >= frame - 1) { right = i; break; }
    }
    if (left < 0 && right < 0) {
        if (announce) snprintf(g_status,sizeof(g_status),"%s","NO KEYFRAMES - INSERT A KEY");
        return 0;
    }
    if (left < 0) left = right;
    if (right < 0) right = left;
    if (right != left) t = (float)((frame - 1) - left) / (float)(right - left);

    for (bone = 0; bone < g_model.bone_count; ++bone) {
        VLBone *b = &g_model.bones[bone];
        for (axis = 0; axis < 9; ++axis) {
            float a = g_animation[left].trs[bone][axis];
            float z = g_animation[right].trs[bone][axis];
            float value;
            if (axis < 3) value = interpolate_angle(a, z, t);
            else value = a + (z - a) * t;
            if (axis < 3) b->basis_rotation[axis] = value;
            else if (axis < 6) b->basis_location[axis - 3] = value;
            else b->basis_scale[axis - 6] = value;
        }
    }
    vl_model_update_skin(&g_model);
    g_undo_count = 0;
    g_redo_count = 0;
    if (announce) {
        snprintf(g_status,sizeof(g_status),"TIMELINE FRAME %02d",frame);
    }
    return 1;
}

static void goto_timeline_frame(int frame) {
    if (frame < 1) frame = 1;
    if (frame > VLENDER_TIMELINE_FRAMES) frame = VLENDER_TIMELINE_FRAMES;
    g_playing = 0;
    g_play_tick = 0;
    g_frame = frame;
    (void)apply_animation_frame(g_frame, 1);
}

static void insert_timeline_keyframe(void) {
    if (!g_model.loaded || !g_pose_mode) {
        snprintf(g_status,sizeof(g_status),"%s","ENTER POSE MODE TO INSERT A KEYFRAME");
        return;
    }
    g_playing = 0;
    g_play_tick = 0;
    capture_animation_frame(g_frame);
    snprintf(g_status,sizeof(g_status),"INSERTED KEYFRAME AT %02d",g_frame);
}

static void delete_timeline_keyframe(void) {
    g_playing = 0;
    g_play_tick = 0;
    if (g_frame < 1 || g_frame > VLENDER_TIMELINE_FRAMES ||
        !g_animation[g_frame - 1].valid) {
        snprintf(g_status,sizeof(g_status),"%s","NO KEYFRAME AT CURRENT FRAME");
        return;
    }
    memset(&g_animation[g_frame - 1],0,sizeof(g_animation[g_frame - 1]));
    (void)apply_animation_frame(g_frame, 0);
    snprintf(g_status,sizeof(g_status),"DELETED KEYFRAME AT %02d",g_frame);
}

static void toggle_timeline_playback(void) {
    if (!g_model.loaded) {
        snprintf(g_status,sizeof(g_status),"%s","LOAD A PROJECT BEFORE PLAYBACK");
        return;
    }
    {
        int i, found = 0;
        for (i = 0; i < VLENDER_TIMELINE_FRAMES; ++i)
            if (g_animation[i].valid) { found = 1; break; }
        if (!found) {
            snprintf(g_status,sizeof(g_status),"%s","INSERT AT LEAST ONE KEYFRAME");
            return;
        }
    }
    g_playing = !g_playing;
    g_play_tick = 0;
    g_undo_count = 0;
    g_redo_count = 0;
    snprintf(g_status,sizeof(g_status),"TIMELINE %s",g_playing ? "PLAYING" : "PAUSED");
}

static void save_animation(void) {
    FILE *file;
    int frame, bone, axis, has_keyframe = 0;
    char encoded[VLENDER_NAME_MAX * 3];
    ensure_data_directory();
    if (!g_model.loaded) {
        snprintf(g_status,sizeof(g_status),"%s","SAVE ANIMATION FAILED - NO PROJECT");
        return;
    }
    for (frame = 0; frame < VLENDER_TIMELINE_FRAMES; ++frame) {
        if (g_animation[frame].valid) { has_keyframe = 1; break; }
    }
    if (!has_keyframe) {
        snprintf(g_status,sizeof(g_status),"%s","NO KEYFRAMES TO SAVE");
        return;
    }
    file = fopen(PATH_ANIM_OUT, "wb");
    if (!file) {
        snprintf(g_status,sizeof(g_status),"%s","ANIMATION SAVE FAILED");
        return;
    }
    fprintf(file,"VLENDER_ANIM 1\n");
    for (frame = 0; frame < VLENDER_TIMELINE_FRAMES; ++frame) {
        if (!g_animation[frame].valid) continue;
        fprintf(file,"F %d\n",frame + 1);
        for (bone = 0; bone < g_model.bone_count; ++bone) {
            encode_bone_name(g_model.bones[bone].name,encoded,sizeof(encoded));
            fprintf(file,"B %s",encoded);
            for (axis = 0; axis < 9; ++axis)
                fprintf(file," %.7g",g_animation[frame].trs[bone][axis]);
            fputc('\n',file);
        }
    }
    {
        int write_failed = ferror(file);
        if (fclose(file) != 0) write_failed = 1;
        if (write_failed) {
            snprintf(g_status,sizeof(g_status),"%s","ANIMATION SAVE FAILED");
            return;
        }
    }
    snprintf(g_status,sizeof(g_status),"%s","SAVED output.vlanim - IMPORT IN BLENDER");
}

static int parse_animation_file(void) {
    FILE *file;
    char line[1024];
    VLAnimationFrame *candidate;
    int header_seen = 0, current_frame = -1, matched = 0, has_frame = 0;
    file = fopen(PATH_ANIM_IN,"rb");
    if (!file) return 0;
    candidate = (VLAnimationFrame *)calloc(VLENDER_TIMELINE_FRAMES,sizeof(*candidate));
    if (!candidate) { fclose(file); return 0; }

    while (fgets(line,sizeof(line),file)) {
        char *trim = line;
        size_t length = strlen(line);
        char type[8];
        char extra;
        if (length == sizeof(line)-1 && line[length-1] != '\n') {
            int ch;
            while ((ch=fgetc(file)) != '\n' && ch != EOF) { }
            goto fail;
        }
        while (*trim == ' ' || *trim == '\t') ++trim;
        if (*trim == '\0' || *trim == '\n' || *trim == '#') continue;
        if (sscanf(trim,"%7s",type) != 1) continue;
        if (strcmp(type,"VLENDER_ANIM") == 0) {
            int version = 0;
            if (header_seen || sscanf(trim,"VLENDER_ANIM %d %c",&version,&extra) != 1 || version != 1) goto fail;
            header_seen = 1;
        } else if (strcmp(type,"F") == 0) {
            int frame;
            if (!header_seen || sscanf(trim,"F %d %c",&frame,&extra) != 1 ||
                frame < 1 || frame > VLENDER_TIMELINE_FRAMES) goto fail;
            current_frame = frame - 1;
            has_frame = 1;
            {
                int bone, axis;
                for (bone = 0; bone < g_model.bone_count; ++bone) {
                    VLBone *source_bone = &g_model.bones[bone];
                    for (axis = 0; axis < 3; ++axis) {
                        candidate[current_frame].trs[bone][axis] = source_bone->basis_rotation[axis];
                        candidate[current_frame].trs[bone][3 + axis] = source_bone->basis_location[axis];
                        candidate[current_frame].trs[bone][6 + axis] = source_bone->basis_scale[axis];
                    }
                }
            }
        } else if (strcmp(type,"B") == 0) {
            char encoded[VLENDER_NAME_MAX * 3];
            float values[9];
            char name[VLENDER_NAME_MAX];
            int bone, n;
            if (!header_seen || current_frame < 0 ||
                sscanf(trim,"B %239s %f %f %f %f %f %f %f %f %f %c",
                       encoded,&values[0],&values[1],&values[2],
                       &values[3],&values[4],&values[5],
                       &values[6],&values[7],&values[8],&extra) != 10) goto fail;
            for (n = 0; n < 9; ++n) if (!isfinite(values[n])) goto fail;
            decode_bone_name(encoded,name,sizeof(name));
            bone = vl_model_find_bone(&g_model,name);
            if (bone >= 0) {
                memcpy(candidate[current_frame].trs[bone],values,sizeof(values));
                candidate[current_frame].valid = 1;
                ++matched;
            }
        } else {
            goto fail;
        }
    }
    {
        int close_failed = ferror(file);
        if (fclose(file) != 0) close_failed = 1;
        file = NULL;
        if (close_failed) goto fail;
    }
    if (!header_seen || !has_frame || matched == 0) goto fail;
    memcpy(g_animation,candidate,sizeof(g_animation));
    free(candidate);
    g_playing = 0;
    g_play_tick = 0;
    goto_timeline_frame(1);
    snprintf(g_status,sizeof(g_status),"LOADED ANIMATION - %d BONE RECORDS",matched);
    return 1;

fail:
    if (file) fclose(file);
    free(candidate);
    return 0;
}

static void copy_selected_transform(void) {
    int i;
    if (!g_model.loaded || !g_pose_mode || g_selected_bone < 0 ||
        g_selected_bone >= g_model.bone_count) {
        snprintf(g_status, sizeof(g_status), "%s", "COPY FAILED - ENTER POSE MODE");
        return;
    }
    for (i = 0; i < 3; ++i) {
        g_clipboard_location[i] = g_model.bones[g_selected_bone].basis_location[i];
        g_clipboard_rotation[i] = g_model.bones[g_selected_bone].basis_rotation[i];
        g_clipboard_scale[i] = g_model.bones[g_selected_bone].basis_scale[i];
    }
    g_has_transform_clipboard = 1;
    snprintf(g_status, sizeof(g_status), "COPIED TRANSFORM: %.40s",
             g_model.bones[g_selected_bone].name);
}

static void paste_selected_transform(void) {
    int i;
    if (!g_model.loaded || !g_pose_mode || g_selected_bone < 0 ||
        g_selected_bone >= g_model.bone_count || !g_has_transform_clipboard) {
        snprintf(g_status, sizeof(g_status), "%s", "PASTE FAILED - POSE MODE / COPY FIRST");
        return;
    }
    history_record_undo();
    for (i = 0; i < 3; ++i) {
        g_model.bones[g_selected_bone].basis_location[i] = g_clipboard_location[i];
        g_model.bones[g_selected_bone].basis_rotation[i] = g_clipboard_rotation[i];
        g_model.bones[g_selected_bone].basis_scale[i] = g_clipboard_scale[i];
    }
    vl_model_update_skin(&g_model);
    snprintf(g_status, sizeof(g_status), "PASTED TRANSFORM: %.40s",
             g_model.bones[g_selected_bone].name);
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
    if (!g_model.loaded || g_model.bone_count == 0 || g_transform_tool == 0 || !g_pose_mode) return;
    if (g_playing) {
        g_playing = 0;
        g_play_tick = 0;
        snprintf(g_status,sizeof(g_status),"%s","PLAYBACK PAUSED FOR EDITING");
    }
    if (!g_drag_slider) history_record_undo();
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
    if (!g_model.loaded || g_model.bone_count == 0 || g_transform_tool == 0 || !g_pose_mode) return;
    if (g_playing) {
        g_playing = 0;
        g_play_tick = 0;
    }
    history_record_undo();
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

    if (g_show_texture && g_texture) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, g_texture);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    } else {
        glDisable(GL_TEXTURE_2D);
    }

    /* Solid or textured surface. The same lighting factor keeps solid mode
       useful for checking shape, without relying on a material texture. */
    glBegin(GL_TRIANGLES);
    for (i = 0; i < g_model.vertex_count; ++i) {
        const VLVertex *v = &g_model.vertices[i];
        float nx = v->skinned_normal[0], ny = v->skinned_normal[1], nz = v->skinned_normal[2];
        float diffuse = fmaxf(0.0f, nx * -0.34f + ny * -0.42f + nz * 0.84f);
        float shade = 0.27f + diffuse * 0.73f;
        shade = fminf(1.0f, fmaxf(0.2f, shade));
        if (!g_show_texture || !g_texture) set_color(0.44f * shade,0.49f * shade,0.56f * shade);
        else set_color(shade,shade,shade);
        glTexCoord2f(v->uv[0],v->uv[1]);
        glVertex3f(v->skinned_position[0],v->skinned_position[1],v->skinned_position[2]);
    }
    glEnd();

    if (g_show_wireframe) {
        /* Draw each triangle edge a hair above the skin surface to avoid
           depth flicker. Uses the already-skinned vertices, so the overlay
           follows bone edits in real time. */
        glDisable(GL_TEXTURE_2D);
        glDepthFunc(GL_LEQUAL);
        glLineWidth(1.0f);
        glBegin(GL_LINES);
        for (i = 0; i + 2 < g_model.vertex_count; i += 3) {
            const VLVertex *a = &g_model.vertices[i];
            const VLVertex *b = &g_model.vertices[i + 1];
            const VLVertex *c = &g_model.vertices[i + 2];
            const VLVertex *ends[6] = {a,b,b,c,c,a};
            int j;
            set_color(0.055f,0.075f,0.095f);
            for (j = 0; j < 6; ++j) {
                const VLVertex *v = ends[j];
                float offset = 0.0015f;
                glVertex3f(v->skinned_position[0] + v->skinned_normal[0] * offset,
                           v->skinned_position[1] + v->skinned_normal[1] * offset,
                           v->skinned_position[2] + v->skinned_normal[2] * offset);
            }
        }
        glEnd();
        glDepthFunc(GL_LESS);
    }

    if (g_texture) {
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
    }
}

static void draw_skeleton(void) {
    int i;
    if (!g_model.loaded || !g_pose_mode || !g_show_skeleton || g_model.bone_count == 0) return;
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

    if (g_show_grid) draw_grid();
    draw_model_mesh();
    if (g_show_skeleton) draw_skeleton();
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
    rect2d(680,VIEW_Y,280,25,0.12f,0.135f,0.155f);
    rect2d(680,222,280,25,0.12f,0.135f,0.155f);
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
        button_icon2d(694,382,48,24,ICON_MINUS,0);
        button_icon2d(748,382,48,24,ICON_PLUS,0);
        button_icon2d(802,382,66,24,ICON_RESET,0);
        button_icon2d(876,382,58,24,ICON_POSE,g_pose_mode);
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
    button_icon2d(694,382,48,24,ICON_MINUS,0);
    button_icon2d(748,382,48,24,ICON_PLUS,0);
    button_icon2d(802,382,66,24,ICON_RESET,0);
    button_icon2d(876,382,58,24,ICON_POSE,g_pose_mode);
}

static void draw_toolbar(void) {
    button_icon2d(4,62,32,32,ICON_SELECT,g_transform_tool==0);
    button_icon2d(4,101,32,32,ICON_MOVE,g_transform_tool==1);
    button_icon2d(4,140,32,32,ICON_ROTATE,g_transform_tool==2);
    button_icon2d(4,179,32,32,ICON_SCALE,g_transform_tool==3);
    text2d(7,218,0.75f,0.44f,0.50f,0.56f,"TOOLS");
    button_icon2d(4,231,32,28,ICON_GRID,g_show_grid);
    text2d(7,260,0.72f,0.44f,0.50f,0.56f,"GRID");
    button_icon2d(4,280,32,28,ICON_BONES,g_show_skeleton);
    text2d(5,309,0.72f,0.44f,0.50f,0.56f,"BONES");
    button_icon2d(4,326,32,28,ICON_TEXTURE,g_show_texture);
    text2d(4,355,0.66f,0.44f,0.50f,0.56f,"TEX");
    button_icon2d(4,372,32,28,ICON_WIREFRAME,g_show_wireframe);
    text2d(1,401,0.58f,0.44f,0.50f,0.56f,"WIREFR");
}

static void draw_timeline(void) {
    int i;
    char label[40];
    text2d(16,431,1.12f,0.89f,0.91f,0.93f,"TIMELINE");
    text2d(16,449,0.82f,0.49f,0.55f,0.61f,"FRAME");
    snprintf(label,sizeof(label),"%02d / %02d",g_frame,VLENDER_TIMELINE_FRAMES);
    text2d(56,449,0.87f,0.55f,0.96f,0.49f,label);

    button_icon2d(91,442,18,22,ICON_PREV,0);
    button_icon2d(111,442,18,22,ICON_NEXT,0);
    button_icon2d(131,442,18,22,ICON_KEYFRAME,
                  g_animation[g_frame - 1].valid);
    button_icon2d(151,442,18,22,g_playing ? ICON_STOP : ICON_PLAY,g_playing);

    line2d(178,436,178,487,0.23f,0.27f,0.31f);
    for (i = 0; i < VLENDER_TIMELINE_FRAMES; ++i) {
        float x = 188.0f + (float)i * 9.0f;
        float height = (i % 10 == 0) ? 15.0f : (i % 5 == 0 ? 9.0f : 5.0f);
        if (g_animation[i].valid) {
            set_color(0.40f,0.91f,0.42f);
            glPointSize(4.0f);
            glBegin(GL_POINTS);
            glVertex2f(x,441.0f);
            glEnd();
        }
        line2d(x,455.0f-height*0.5f,x,455.0f+height*0.5f,
               g_animation[i].valid ? 0.40f : 0.33f,
               g_animation[i].valid ? 0.91f : 0.37f,
               g_animation[i].valid ? 0.42f : 0.42f);
    }
    line2d(188.0f + (float)(g_frame - 1) * 9.0f,437.0f,
           188.0f + (float)(g_frame - 1) * 9.0f,480.0f,
           g_playing ? 0.35f : 0.96f,g_playing ? 0.95f : 0.42f,
           g_playing ? 0.48f : 0.15f);
    text2d(850,480,0.68f,0.45f,0.52f,0.58f,
           g_playing ? "PLAYING" : "POSE KEYS");
}

static int popup_menu_x(void) {
    switch (g_open_menu) {
        case 1: return 150;
        case 2: return 185;
        case 3: return 224;
        case 4: return 267;
        case 5: return 318;
        default: return 0;
    }
}

static int popup_menu_width(void) {
    return g_open_menu == 5 ? 165 : 185;
}

static int popup_menu_rows(void) {
    switch (g_open_menu) {
        case 1: return 6;
        case 2: return 8;
        case 3: return 5;
        case 4: return 3;
        case 5: return 5;
        default: return 0;
    }
}

static const char *popup_menu_label(int row) {
    static const char *file_items[] = {
        "Reload Project", "Load Pose", "Save Pose",
        "Load Animation", "Save Animation", "Close Menu"
    };
    static const char *edit_items[] = {
        "Undo Transform", "Redo Transform", "Copy Bone TRS",
        "Paste Bone TRS", "Reset Bone Transform", "Insert Keyframe",
        "Delete Keyframe", "Play / Pause Timeline"
    };
    static const char *render_items[] = {
        "Toggle Texture", "Toggle Wireframe", "Toggle Skeleton",
        "Toggle Grid", "Reset View"
    };
    static const char *window_items[] = {
        "Toggle Pose/Object", "Reset View", "Close Menu"
    };
    static const char *help_items[] = {
        "D-pad: select bone", "Left/Right: adjust",
        "Cross: change axis", "Circle: reset tool",
        "Tap outside to close"
    };
    if (row < 0) return "";
    switch (g_open_menu) {
        case 1: return row < 6 ? file_items[row] : "";
        case 2: return row < 8 ? edit_items[row] : "";
        case 3: return row < 5 ? render_items[row] : "";
        case 4: return row < 3 ? window_items[row] : "";
        case 5: return row < 5 ? help_items[row] : "";
        default: return "";
    }
}

static void draw_popup_menu(void) {
    int i, x, width, height;
    if (g_open_menu == 0) return;
    x = popup_menu_x();
    width = popup_menu_width();
    height = popup_menu_rows() * 23 + 8;
    rect2d((float)x,42.0f,(float)width,(float)height,0.055f,0.062f,0.073f);
    rect2d((float)x,42.0f,(float)width,1.0f,0.33f,0.39f,0.44f);
    rect2d((float)x,42.0f,1.0f,(float)height,0.28f,0.33f,0.38f);
    rect2d((float)(x+width-1),42.0f,1.0f,(float)height,0.28f,0.33f,0.38f);
    rect2d((float)x,42.0f+(float)height-1.0f,(float)width,1.0f,0.28f,0.33f,0.38f);
    for (i = 0; i < popup_menu_rows(); ++i) {
        float y = 51.0f + (float)i * 23.0f;
        text2d((float)x+10.0f,y,0.92f,
               (g_open_menu == 5 ? 0.55f : 0.86f),
               (g_open_menu == 5 ? 0.62f : 0.88f),
               (g_open_menu == 5 ? 0.69f : 0.90f),
               popup_menu_label(i));
        if (i + 1 < popup_menu_rows() && g_open_menu != 5)
            line2d((float)x+7.0f,y+17.0f,(float)(x+width-7),y+17.0f,
                   0.13f,0.15f,0.18f);
    }
}

static void execute_popup_item(int row) {
    switch (g_open_menu) {
        case 1:
            if (row == 0) load_project();
            else if (row == 1) load_pose();
            else if (row == 2) save_pose();
            else if (row == 3) {
                if (!g_model.loaded || !parse_animation_file())
                    snprintf(g_status,sizeof(g_status),"%s","ANIMATION LOAD FAILED - CHECK input.vlanim");
            } else if (row == 4) save_animation();
            break;
        case 2:
            if (row == 0) undo_transform();
            else if (row == 1) redo_transform();
            else if (row == 2) copy_selected_transform();
            else if (row == 3) paste_selected_transform();
            else if (row == 4) reset_active_transform();
            else if (row == 5) insert_timeline_keyframe();
            else if (row == 6) delete_timeline_keyframe();
            else if (row == 7) toggle_timeline_playback();
            break;
        case 3:
            if (row == 0) {
                g_show_texture = !g_show_texture;
                snprintf(g_status,sizeof(g_status),"TEXTURE DISPLAY %s",g_show_texture ? "ON" : "OFF");
            } else if (row == 1) {
                g_show_wireframe = !g_show_wireframe;
                snprintf(g_status,sizeof(g_status),"WIREFRAME OVERLAY %s",g_show_wireframe ? "ON" : "OFF");
            } else if (row == 2) {
                g_show_skeleton = !g_show_skeleton;
                snprintf(g_status,sizeof(g_status),"SKELETON OVERLAY %s",g_show_skeleton ? "ON" : "OFF");
            } else if (row == 3) {
                g_show_grid = !g_show_grid;
                snprintf(g_status,sizeof(g_status),"VIEWPORT GRID %s",g_show_grid ? "ON" : "OFF");
            } else if (row == 4) {
                g_camera_azimuth = 32.0f;
                g_camera_elevation = -62.0f;
                g_camera_distance = g_model.loaded ? fmaxf(2.8f,g_model.radius*3.8f) : 4.2f;
                snprintf(g_status,sizeof(g_status),"%s","VIEWPORT CAMERA RESET");
            }
            break;
        case 4:
            if (row == 0) {
                g_pose_mode = !g_pose_mode;
                snprintf(g_status,sizeof(g_status),"MODE: %s",g_pose_mode ? "POSE" : "OBJECT");
            } else if (row == 1) {
                g_camera_azimuth = 32.0f;
                g_camera_elevation = -62.0f;
                g_camera_distance = g_model.loaded ? fmaxf(2.8f,g_model.radius*3.8f) : 4.2f;
                snprintf(g_status,sizeof(g_status),"%s","VIEWPORT CAMERA RESET");
            }
            break;
        case 5:
            snprintf(g_status,sizeof(g_status),"%s",
                     "HELP: D-PAD SELECTS BONES; CROSS CHANGES AXIS; CIRCLE RESETS TOOL");
            break;
        default:
            break;
    }
    g_open_menu = 0;
}

static void draw_ui_overlay(void) {
    char stats[80];
    begin_2d();
    text2d(13,13,1.8f,0.53f,1.0f,0.45f,"VLENDER");
    text2d(91,18,0.9f,0.53f,0.59f,0.64f,"3D STUDIO");
    text2d(157,17,0.82f,0.82f,0.85f,0.88f,"File");
    text2d(192,17,0.82f,0.82f,0.85f,0.88f,"Edit");
    text2d(228,17,0.82f,0.82f,0.85f,0.88f,"Render");
    text2d(273,17,0.82f,0.82f,0.85f,0.88f,"Window");
    text2d(324,17,0.82f,0.82f,0.85f,0.88f,"Help");
    button_icon2d(405,9,34,28,ICON_LAYOUT,1);
    button_icon2d(445,9,34,28,ICON_POSE,g_pose_mode);
    button_icon2d(485,9,34,28,ICON_OBJECT,!g_pose_mode);
    button_icon2d(532,9,34,28,ICON_UNDO,g_undo_count>0);
    button_icon2d(572,9,34,28,ICON_REDO,g_redo_count>0);
    button_icon2d(612,9,34,28,ICON_COPY,g_model.loaded);
    button_icon2d(652,9,34,28,ICON_PASTE,g_has_transform_clipboard && g_model.loaded && g_pose_mode);
    button_icon2d(748,9,34,28,ICON_FIT,0);
    button_icon2d(788,9,34,28,ICON_OPEN,0);
    button_icon2d(828,9,34,28,ICON_LOAD,0);
    button_icon2d(868,9,34,28,ICON_SAVE,0);
    button_icon2d(908,9,38,28,ICON_MODE,g_pose_mode);
    draw_toolbar();
    draw_outliner();
    draw_properties();
    draw_timeline();
    text2d(12,510,0.95f,0.75f,0.80f,0.84f,g_status);
    text2d(12,528,0.9f,0.42f,0.49f,0.55f,"D-PAD SELECT / LEFT-RIGHT ADJUST / CROSS AXIS / SELECT VIEW / START EXIT");
    text2d(692,528,0.9f,0.42f,0.49f,0.55f,"TOUCH: ORBIT / SELECT / ADJUST");
    text2d(56,61,0.95f,0.80f,0.84f,0.87f,g_model.loaded ? "MESH WORKSPACE" : "NO PROJECT LOADED");
    if (g_model.loaded) {
        snprintf(stats,sizeof(stats),"VERTS %d  TRIS %d  BONES %d",
                 g_model.vertex_count,g_model.vertex_count/3,g_model.bone_count);
        text2d(350,61,0.72f,0.49f,0.75f,0.78f,stats);
    } else {
        centered_text(VIEW_X + VIEW_W*0.5f, VIEW_Y + VIEW_H*0.5f - 5.0f,
                      1.4f,0.88f,0.91f,0.92f,"3D VIEWPORT READY");
    }
    line2d(621,75,651,75,0.90f,0.30f,0.30f);
    line2d(621,75,621,103,0.30f,0.90f,0.44f);
    line2d(621,75,603,93,0.30f,0.62f,1.0f);
    text2d(654,72,0.9f,0.98f,0.42f,0.42f,"X");
    text2d(616,107,0.9f,0.39f,0.95f,0.51f,"Y");
    text2d(595,95,0.9f,0.40f,0.70f,1.0f,"Z");
    draw_popup_menu();
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
    if (g_open_menu != 0 && y >= 45) {
        int px = popup_menu_x();
        int pw = popup_menu_width();
        int rows = popup_menu_rows();
        int bottom = 42 + rows * 23 + 8;
        if (x >= px && x < px + pw && y >= 42 && y < bottom) {
            int row = (y - 51) / 23;
            if (row >= 0 && row < rows) execute_popup_item(row);
            else g_open_menu = 0;
        } else {
            g_open_menu = 0;
        }
        return;
    }
    if (y < 45) {
        if (x >= 150 && x < 186) { g_open_menu = g_open_menu == 1 ? 0 : 1; return; }
        if (x >= 186 && x < 224) { g_open_menu = g_open_menu == 2 ? 0 : 2; return; }
        if (x >= 224 && x < 270) { g_open_menu = g_open_menu == 3 ? 0 : 3; return; }
        if (x >= 270 && x < 320) { g_open_menu = g_open_menu == 4 ? 0 : 4; return; }
        if (x >= 320 && x < 360) { g_open_menu = g_open_menu == 5 ? 0 : 5; return; }
        if (x >= 748 && x < 784) {
            g_camera_azimuth = 32.0f;
            g_camera_elevation = -62.0f;
            g_camera_distance = g_model.loaded ? fmaxf(2.8f, g_model.radius * 3.8f) : 4.2f;
            snprintf(g_status,sizeof(g_status),"%s","VIEWPORT CAMERA RESET");
        }
        else if (x >= 788 && x < 824) load_project();
        else if (x >= 828 && x < 864) load_pose();
        else if (x >= 868 && x < 904) save_pose();
        else if (x >= 532 && x < 566) undo_transform();
        else if (x >= 572 && x < 606) redo_transform();
        else if (x >= 612 && x < 646) copy_selected_transform();
        else if (x >= 652 && x < 686) paste_selected_transform();
        else if (x >= 908 && x < 948) g_pose_mode = !g_pose_mode;
        else if (x >= 445 && x < 479) g_pose_mode = 1;
        else if (x >= 485 && x < 519) g_pose_mode = 0;
        else if (x >= 405 && x < 439) snprintf(g_status,sizeof(g_status),"%s","LAYOUT WORKSPACE");
        return;
    }
    if (x >= 4 && x <= 38) {
        if (y >= 62 && y <= 94) g_transform_tool = 0;
        else if (y >= 101 && y <= 133) g_transform_tool = 1;
        else if (y >= 140 && y <= 172) g_transform_tool = 2;
        else if (y >= 179 && y <= 211) g_transform_tool = 3;
        else if (y >= 231 && y <= 259) {
            g_show_grid = !g_show_grid;
            snprintf(g_status,sizeof(g_status),"VIEWPORT GRID %s",g_show_grid ? "ON" : "OFF");
        }
        else if (y >= 280 && y <= 308) {
            g_show_skeleton = !g_show_skeleton;
            snprintf(g_status,sizeof(g_status),"SKELETON OVERLAY %s",g_show_skeleton ? "ON" : "OFF");
        }
        else if (y >= 326 && y <= 354) {
            g_show_texture = !g_show_texture;
            snprintf(g_status,sizeof(g_status),"TEXTURE DISPLAY %s",g_show_texture ? "ON" : "OFF");
        }
        else if (y >= 372 && y <= 400) {
            g_show_wireframe = !g_show_wireframe;
            snprintf(g_status,sizeof(g_status),"WIREFRAME OVERLAY %s",g_show_wireframe ? "ON" : "OFF");
        }
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
        history_record_undo();
        g_drag_slider = 1;
        set_slider_from_x(x);
        return;
    }
    if (x >= 694 && x < 744 && y >= 382 && y < 408) { adjust_rotation(-5.0f); return; }
    if (x >= 748 && x < 798 && y >= 382 && y < 408) { adjust_rotation(5.0f); return; }
    if (x >= 802 && x < 870 && y >= 382 && y < 408) { reset_active_transform(); return; }
    if (x >= 876 && x < 940 && y >= 382 && y < 408) { g_pose_mode = !g_pose_mode; return; }
    if (y >= 436 && y <= 486) {
        if (x >= 91 && x < 109) { goto_timeline_frame(g_frame - 1); return; }
        if (x >= 111 && x < 129) { goto_timeline_frame(g_frame + 1); return; }
        if (x >= 131 && x < 149) { insert_timeline_keyframe(); return; }
        if (x >= 151 && x < 169) { toggle_timeline_playback(); return; }
        if (x >= 188 && x <= 908) {
            int frame = 1 + (int)((x - 188 + 4) / 9);
            goto_timeline_frame(frame);
            return;
        }
    }
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
    if (pressed & SCE_CTRL_SELECT) {
        g_camera_azimuth = 32.0f;
        g_camera_elevation = -62.0f;
        g_camera_distance = g_model.loaded ? fmaxf(2.8f, g_model.radius * 3.8f) : 4.2f;
        snprintf(g_status,sizeof(g_status),"%s","VIEWPORT CAMERA RESET");
    }
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
    if (g_playing) {
        if (++g_play_tick >= 6) {
            g_play_tick = 0;
            g_frame = g_frame >= VLENDER_TIMELINE_FRAMES ? 1 : g_frame + 1;
            (void)apply_animation_frame(g_frame, 0);
        }
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
    if (!font_init()) {
        snprintf(g_status,sizeof(g_status),"%s","FONT ATLAS INIT FAILED");
    }
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
    font_shutdown();
    vl_model_free(&g_model);
    sceKernelExitProcess(0);
    return 0;
}
