#include "model.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void mat_identity(float m[16]) {
    memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat_mul(float out[16], const float a[16], const float b[16]) {
    float temp[16];
    int col, row, k;
    for (col = 0; col < 4; ++col) {
        for (row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            temp[col * 4 + row] = sum;
        }
    }
    memcpy(out, temp, sizeof(temp));
}

/* Blender bone rest matrices are rigid transforms (no scale), so R^-1 = R^T. */
static void mat_inverse_rigid(float out[16], const float m[16]) {
    mat_identity(out);
    out[0] = m[0];  out[1] = m[4];  out[2] = m[8];
    out[4] = m[1];  out[5] = m[5];  out[6] = m[9];
    out[8] = m[2];  out[9] = m[6];  out[10] = m[10];
    out[12] = -(out[0] * m[12] + out[4] * m[13] + out[8] * m[14]);
    out[13] = -(out[1] * m[12] + out[5] * m[13] + out[9] * m[14]);
    out[14] = -(out[2] * m[12] + out[6] * m[13] + out[10] * m[14]);
}

static void mat_from_trs(float out[16], const float loc[3],
                         const float rot[3], const float scale[3]) {
    float sx = sinf(rot[0]), cx = cosf(rot[0]);
    float sy = sinf(rot[1]), cy = cosf(rot[1]);
    float sz = sinf(rot[2]), cz = cosf(rot[2]);

    /* R = Rz * Ry * Rx, the XYZ Euler order used by the bridge. */
    mat_identity(out);
    out[0] = cz * cy * scale[0];
    out[1] = sz * cy * scale[0];
    out[2] = -sy * scale[0];

    out[4] = (cz * sy * sx - sz * cx) * scale[1];
    out[5] = (sz * sy * sx + cz * cx) * scale[1];
    out[6] = cy * sx * scale[1];

    out[8] = (cz * sy * cx + sz * sx) * scale[2];
    out[9] = (sz * sy * cx - cz * sx) * scale[2];
    out[10] = cy * cx * scale[2];

    out[12] = loc[0];
    out[13] = loc[1];
    out[14] = loc[2];
}

static void transform_point(const float m[16], const float p[3], float out[3]) {
    float x = p[0], y = p[1], z = p[2];
    out[0] = m[0] * x + m[4] * y + m[8] * z + m[12];
    out[1] = m[1] * x + m[5] * y + m[9] * z + m[13];
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
}

static void transform_direction(const float m[16], const float p[3], float out[3]) {
    float x = p[0], y = p[1], z = p[2];
    out[0] = m[0] * x + m[4] * y + m[8] * z;
    out[1] = m[1] * x + m[5] * y + m[9] * z;
    out[2] = m[2] * x + m[6] * y + m[10] * z;
}

static void normalize3(float v[3]) {
    float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 0.00001f) {
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    } else {
        v[0] = 0.0f;
        v[1] = 0.0f;
        v[2] = 1.0f;
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)toupper((unsigned char)c);
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void decode_name(const char *src, char *dst, size_t capacity) {
    size_t out = 0;
    size_t i = 0;
    if (capacity == 0) return;
    while (src && src[i] && out + 1 < capacity) {
        if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            int hi = hex_value(src[i + 1]);
            int lo = hex_value(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                i += 3;
                continue;
            }
        }
        dst[out++] = src[i++];
    }
    dst[out] = '\0';
}

static void encode_name(const char *src, char *dst, size_t capacity) {
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    size_t i = 0;
    if (capacity == 0) return;
    while (src && src[i] && out + 4 < capacity) {
        unsigned char c = (unsigned char)src[i++];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            dst[out++] = (char)c;
        } else {
            dst[out++] = '%';
            dst[out++] = hex[c >> 4];
            dst[out++] = hex[c & 15];
        }
    }
    dst[out] = '\0';
}

static int split_tokens(char *line, char **tokens, int max_tokens) {
    int count = 0;
    char *cursor = line;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t' ||
               *cursor == '\r' || *cursor == '\n') ++cursor;
        if (!*cursor) break;
        if (count >= max_tokens) return max_tokens + 1;
        tokens[count++] = cursor;
        while (*cursor && *cursor != ' ' && *cursor != '\t' &&
               *cursor != '\r' && *cursor != '\n') ++cursor;
        if (*cursor) *cursor++ = '\0';
    }
    return count;
}

static int valid_float_token(const char *token) {
    char *end = NULL;
    float value;
    if (!token || !*token) return 0;
    errno = 0;
    value = strtof(token, &end);
    return end != token && end && *end == 0 &&
           errno != ERANGE && isfinite(value);
}

static int valid_int_token(const char *token) {
    char *end = NULL;
    long value;
    if (!token || !*token) return 0;
    errno = 0;
    value = strtol(token, &end, 10);
    return end != token && end && *end == 0 &&
           errno != ERANGE && value >= -2147483647L - 1L &&
           value <= 2147483647L;
}

static float token_float(char **tokens, int index) {
    return strtof(tokens[index], NULL);
}

static int token_int(char **tokens, int index) {
    return (int)strtol(tokens[index], NULL, 10);
}

void vl_model_init(VLModel *model) {
    if (!model) return;
    memset(model, 0, sizeof(*model));
    snprintf(model->texture_name, sizeof(model->texture_name), "%s", "texture.bmp");
    model->radius = 1.0f;
}

void vl_model_free(VLModel *model) {
    if (!model) return;
    free(model->vertices);
    vl_model_init(model);
}

int vl_model_find_bone(const VLModel *model, const char *name) {
    int i;
    if (!model || !name) return -1;
    for (i = 0; i < model->bone_count; ++i) {
        if (strcmp(model->bones[i].name, name) == 0) return i;
    }
    return -1;
}

static int append_vertex(VLModel *model, const VLVertex *vertex) {
    VLVertex *new_vertices;
    int capacity;
    if (model->vertex_count >= VLENDER_MAX_CORNERS) return 0;
    if (model->vertex_count >= model->vertex_capacity) {
        capacity = model->vertex_capacity ? model->vertex_capacity * 2 : 1024;
        if (capacity > VLENDER_MAX_CORNERS) capacity = VLENDER_MAX_CORNERS;
        new_vertices = (VLVertex *)realloc(model->vertices,
                                            (size_t)capacity * sizeof(VLVertex));
        if (!new_vertices) return 0;
        model->vertices = new_vertices;
        model->vertex_capacity = capacity;
    }
    model->vertices[model->vertex_count++] = *vertex;
    return 1;
}

static int parse_bone(VLModel *model, char **tokens, int count) {
    VLBone *bone;
    int i;
    if (count != 34 || model->bone_count >= VLENDER_MAX_BONES ||
        !tokens[1][0] || !valid_int_token(tokens[2])) return 0;
    for (i = 3; i < count; ++i) {
        if (!valid_float_token(tokens[i])) return 0;
    }
    bone = &model->bones[model->bone_count];
    memset(bone, 0, sizeof(*bone));
    decode_name(tokens[1], bone->name, sizeof(bone->name));
    if (!bone->name[0] || vl_model_find_bone(model, bone->name) >= 0) return 0;
    bone->parent = token_int(tokens, 2);
    for (i = 0; i < 3; ++i) bone->head_local[i] = token_float(tokens, 3 + i);
    for (i = 0; i < 3; ++i) bone->tail_local[i] = token_float(tokens, 6 + i);
    for (i = 0; i < 16; ++i) bone->rest[i] = token_float(tokens, 9 + i);
    for (i = 0; i < 3; ++i) bone->basis_location[i] = token_float(tokens, 25 + i);
    for (i = 0; i < 3; ++i) bone->basis_rotation[i] = token_float(tokens, 28 + i);
    for (i = 0; i < 3; ++i) bone->basis_scale[i] = token_float(tokens, 31 + i);
    if (bone->parent < -1 || bone->parent >= VLENDER_MAX_BONES ||
        bone->parent == model->bone_count) return 0;
    mat_inverse_rigid(bone->inv_rest, bone->rest);
    ++model->bone_count;
    return 1;
}

static int parse_vertex(VLModel *model, char **tokens, int count) {
    VLVertex vertex;
    int influence_count;
    int i;
    float weight_sum = 0.0f;
    if (count < 10) return 0;
    for (i = 1; i <= 8; ++i) {
        if (!valid_float_token(tokens[i])) return 0;
    }
    if (!valid_int_token(tokens[9])) return 0;
    memset(&vertex, 0, sizeof(vertex));
    for (i = 0; i < 3; ++i) vertex.position[i] = token_float(tokens, 1 + i);
    for (i = 0; i < 3; ++i) vertex.normal[i] = token_float(tokens, 4 + i);
    vertex.uv[0] = token_float(tokens, 7);
    vertex.uv[1] = token_float(tokens, 8);
    influence_count = token_int(tokens, 9);
    if (influence_count < 0 || influence_count > 4 ||
        count != 10 + influence_count * 2) return 0;
    for (i = 0; i < influence_count; ++i) {
        int bone_id;
        float weight;
        if (!valid_int_token(tokens[10 + i * 2]) ||
            !valid_float_token(tokens[11 + i * 2])) return 0;
        bone_id = token_int(tokens, 10 + i * 2);
        weight = token_float(tokens, 11 + i * 2);
        /* The exporter normalizes each vertex's top-four weights before
           writing. Reject out-of-range weights so hostile/corrupt files cannot
           overflow the sum and silently collapse a vertex's skinning. */
        if (weight < 0.0f || weight > 1.0001f) return 0;
        if (weight > 0.0f && (bone_id < 0 || bone_id >= model->bone_count)) return 0;
        if (weight > 0.0f) {
            vertex.bone[i] = bone_id;
            vertex.weight[i] = weight;
            weight_sum += weight;
        } else {
            vertex.bone[i] = 0;
            vertex.weight[i] = 0.0f;
        }
    }
    if (weight_sum > 0.00001f) {
        for (i = 0; i < 4; ++i) vertex.weight[i] /= weight_sum;
    } else if (model->bone_count > 0) {
        vertex.bone[0] = 0;
        vertex.weight[0] = 1.0f;
    }
    normalize3(vertex.normal);
    memcpy(vertex.skinned_position, vertex.position, sizeof(vertex.position));
    memcpy(vertex.skinned_normal, vertex.normal, sizeof(vertex.normal));
    return append_vertex(model, &vertex);
}

static int validate_bone_visit(const VLModel *model, int index,
                               unsigned char *state) {
    int parent;
    if (state[index] == 1) return 0;
    if (state[index] == 2) return 1;
    state[index] = 1;
    parent = model->bones[index].parent;
    if (parent < -1 || parent >= model->bone_count || parent == index) return 0;
    if (parent >= 0 && !validate_bone_visit(model, parent, state)) return 0;
    state[index] = 2;
    return 1;
}

static int validate_bone_hierarchy(const VLModel *model) {
    unsigned char state[VLENDER_MAX_BONES];
    int i;
    memset(state, 0, sizeof(state));
    for (i = 0; i < model->bone_count; ++i) {
        if (!validate_bone_visit(model, i, state)) return 0;
    }
    return 1;
}

/* Transactional load: a malformed/missing project never destroys the current model. */
int vl_model_load_project(VLModel *model, const char *path) {
    FILE *file;
    char line[1024];
    int header_seen = 0;
    VLModel candidate;
    if (!model || !path) return 0;
    file = fopen(path, "rb");
    if (!file) return 0;

    vl_model_init(&candidate);
    while (fgets(line, sizeof(line), file)) {
        char *tokens[40];
        int count;
        char *trim = line;
        size_t line_length = strlen(line);
        if (line_length == sizeof(line) - 1 && line[line_length - 1] != '\n') {
            int ch;
            while ((ch = fgetc(file)) != '\n' && ch != EOF) { }
            fclose(file);
            vl_model_free(&candidate);
            return 0;
        }
        while (*trim == ' ' || *trim == '\t') ++trim;
        if (*trim == '\0' || *trim == '\n' || *trim == '#') continue;
        count = split_tokens(trim, tokens, 40);
        if (count == 0) continue;
        if (strcmp(tokens[0], "VLENDER_MESH") == 0) {
            if (header_seen || count != 2 || strcmp(tokens[1], "1") != 0) {
                fclose(file);
                vl_model_free(&candidate);
                return 0;
            }
            header_seen = 1;
        } else if (strcmp(tokens[0], "T") == 0) {
            if (!header_seen || count != 2 || !tokens[1][0]) {
                fclose(file);
                vl_model_free(&candidate);
                return 0;
            }
            snprintf(candidate.texture_name, sizeof(candidate.texture_name),
                     "%s", tokens[1]);
        } else if (strcmp(tokens[0], "B") == 0) {
            if (!header_seen || !parse_bone(&candidate, tokens, count)) {
                fclose(file);
                vl_model_free(&candidate);
                return 0;
            }
        } else if (strcmp(tokens[0], "V") == 0) {
            if (!header_seen || !parse_vertex(&candidate, tokens, count)) {
                fclose(file);
                vl_model_free(&candidate);
                return 0;
            }
        }
    }

    if (ferror(file)) {
        fclose(file);
        vl_model_free(&candidate);
        return 0;
    }
    fclose(file);

    if (!header_seen || candidate.vertex_count < 3 ||
        candidate.vertex_count % 3 != 0 ||
        !validate_bone_hierarchy(&candidate)) {
        vl_model_free(&candidate);
        return 0;
    }

    candidate.loaded = 1;
    vl_model_update_skin(&candidate);
    vl_model_free(model);
    *model = candidate;
    return 1;
}

static void update_bone_pose(VLModel *model, int index, unsigned char *state) {
    VLBone *bone = &model->bones[index];
    float basis[16];
    if (state[index] == 2) return;
    if (state[index] == 1) {
        /* Defensive cycle fallback. Valid projects reject cycles at load time;
           never rewrite a bone's parent silently during pose evaluation. */
        mat_from_trs(basis, bone->basis_location, bone->basis_rotation, bone->basis_scale);
        mat_mul(bone->pose, bone->rest, basis);
        mat_mul(bone->skin, bone->pose, bone->inv_rest);
        state[index] = 2;
        return;
    }
    state[index] = 1;

    mat_from_trs(basis, bone->basis_location, bone->basis_rotation, bone->basis_scale);
    if (bone->parent >= 0 && bone->parent < model->bone_count && bone->parent != index) {
        float relative[16];
        float parent_relative[16];
        update_bone_pose(model, bone->parent, state);
        mat_mul(relative, model->bones[bone->parent].inv_rest, bone->rest);
        mat_mul(parent_relative, model->bones[bone->parent].pose, relative);
        mat_mul(bone->pose, parent_relative, basis);
    } else {
        mat_mul(bone->pose, bone->rest, basis);
    }
    mat_mul(bone->skin, bone->pose, bone->inv_rest);
    state[index] = 2;
}

void vl_model_update_skin(VLModel *model) {
    unsigned char state[VLENDER_MAX_BONES];
    int i, j;
    if (!model) return;
    memset(state, 0, sizeof(state));
    for (i = 0; i < model->bone_count; ++i) update_bone_pose(model, i, state);

    if (model->vertex_count == 0) return;
    model->bounds_min[0] = model->bounds_min[1] = model->bounds_min[2] = 1.0e30f;
    model->bounds_max[0] = model->bounds_max[1] = model->bounds_max[2] = -1.0e30f;

    for (i = 0; i < model->vertex_count; ++i) {
        VLVertex *vertex = &model->vertices[i];
        float p[3] = { 0.0f, 0.0f, 0.0f };
        float n[3] = { 0.0f, 0.0f, 0.0f };
        float total = 0.0f;
        if (model->bone_count > 0) {
            for (j = 0; j < 4; ++j) {
                int bone_id = vertex->bone[j];
                float transformed[3];
                float transformed_normal[3];
                float weight = vertex->weight[j];
                if (weight <= 0.0f || bone_id < 0 || bone_id >= model->bone_count) continue;
                transform_point(model->bones[bone_id].skin, vertex->position, transformed);
                transform_direction(model->bones[bone_id].skin, vertex->normal, transformed_normal);
                p[0] += transformed[0] * weight;
                p[1] += transformed[1] * weight;
                p[2] += transformed[2] * weight;
                n[0] += transformed_normal[0] * weight;
                n[1] += transformed_normal[1] * weight;
                n[2] += transformed_normal[2] * weight;
                total += weight;
            }
        }
        if (total <= 0.00001f) {
            memcpy(p, vertex->position, sizeof(p));
            memcpy(n, vertex->normal, sizeof(n));
        } else if (fabsf(total - 1.0f) > 0.0001f) {
            p[0] /= total; p[1] /= total; p[2] /= total;
            n[0] /= total; n[1] /= total; n[2] /= total;
        }
        normalize3(n);
        memcpy(vertex->skinned_position, p, sizeof(p));
        memcpy(vertex->skinned_normal, n, sizeof(n));
        for (j = 0; j < 3; ++j) {
            model->bounds_min[j] = fminf(model->bounds_min[j], p[j]);
            model->bounds_max[j] = fmaxf(model->bounds_max[j], p[j]);
        }
    }

    for (i = 0; i < 3; ++i) model->center[i] = (model->bounds_min[i] + model->bounds_max[i]) * 0.5f;
    {
        float dx = model->bounds_max[0] - model->bounds_min[0];
        float dy = model->bounds_max[1] - model->bounds_min[1];
        float dz = model->bounds_max[2] - model->bounds_min[2];
        model->radius = fmaxf(0.25f, sqrtf(dx * dx + dy * dy + dz * dz) * 0.5f);
    }
}

int vl_model_load_pose(VLModel *model, const char *path) {
    FILE *file;
    char line[512];
    int header_seen = 0, applied = 0, failed = 0;
    float old_trs[VLENDER_MAX_BONES][9];
    int i, axis;
    if (!model || !path || !model->loaded) return 0;
    file = fopen(path, "rb");
    if (!file) return 0;

    for (i = 0; i < model->bone_count; ++i) {
        for (axis = 0; axis < 3; ++axis) {
            old_trs[i][axis] = model->bones[i].basis_rotation[axis];
            old_trs[i][3 + axis] = model->bones[i].basis_location[axis];
            old_trs[i][6 + axis] = model->bones[i].basis_scale[axis];
        }
    }

    while (fgets(line, sizeof(line), file)) {
        char *tokens[16];
        int count;
        char *trim = line;
        size_t length = strlen(line);
        if (length == sizeof(line) - 1 && line[length - 1] != '\n') {
            int ch;
            while ((ch = fgetc(file)) != '\n' && ch != EOF) { }
            failed = 1;
            break;
        }
        while (*trim == ' ' || *trim == '\t') ++trim;
        if (*trim == '\0' || *trim == '\n' || *trim == '#') continue;
        count = split_tokens(trim, tokens, 16);
        if (!count) continue;

        if (strcmp(tokens[0], "VLENDER_POSE") == 0) {
            if (header_seen || count != 2 || strcmp(tokens[1], "1") != 0) {
                failed = 1;
                break;
            }
            header_seen = 1;
            continue;
        }

        if (strcmp(tokens[0], "B") == 0) {
            char name[VLENDER_NAME_MAX];
            int bone_id;
            if (!header_seen || count != 11) {
                failed = 1;
                break;
            }
            for (i = 2; i < 11; ++i) {
                if (!valid_float_token(tokens[i])) {
                    failed = 1;
                    break;
                }
            }
            if (failed) break;
            decode_name(tokens[1], name, sizeof(name));
            bone_id = vl_model_find_bone(model, name);
            if (bone_id >= 0) {
                VLBone *bone = &model->bones[bone_id];
                for (axis = 0; axis < 3; ++axis)
                    bone->basis_rotation[axis] = token_float(tokens, 2 + axis);
                for (axis = 0; axis < 3; ++axis)
                    bone->basis_location[axis] = token_float(tokens, 5 + axis);
                for (axis = 0; axis < 3; ++axis)
                    bone->basis_scale[axis] = token_float(tokens, 8 + axis);
                ++applied;
            }
        }
    }
    if (ferror(file)) failed = 1;
    fclose(file);

    if (failed || !header_seen || applied == 0) {
        for (i = 0; i < model->bone_count; ++i) {
            for (axis = 0; axis < 3; ++axis) {
                model->bones[i].basis_rotation[axis] = old_trs[i][axis];
                model->bones[i].basis_location[axis] = old_trs[i][3 + axis];
                model->bones[i].basis_scale[axis] = old_trs[i][6 + axis];
            }
        }
        vl_model_update_skin(model);
        return 0;
    }

    vl_model_update_skin(model);
    return applied;
}

int vl_model_save_pose(const VLModel *model, const char *path) {
    FILE *file;
    int i;
    if (!model || !path || model->bone_count == 0) return 0;
    file = fopen(path, "wb");
    if (!file) return 0;
    if (fprintf(file, "VLENDER_POSE 1\n# B name_percent_encoded rx ry rz lx ly lz sx sy sz\n") < 0) {
        fclose(file);
        return 0;
    }
    for (i = 0; i < model->bone_count; ++i) {
        const VLBone *bone = &model->bones[i];
        char encoded[VLENDER_NAME_MAX * 3 + 1];
        encode_name(bone->name, encoded, sizeof(encoded));
        if (fprintf(file, "B %s %.7f %.7f %.7f %.7f %.7f %.7f %.7f %.7f %.7f\n",
                    encoded,
                    (double)bone->basis_rotation[0], (double)bone->basis_rotation[1],
                    (double)bone->basis_rotation[2], (double)bone->basis_location[0],
                    (double)bone->basis_location[1], (double)bone->basis_location[2],
                    (double)bone->basis_scale[0], (double)bone->basis_scale[1],
                    (double)bone->basis_scale[2]) < 0) {
            fclose(file);
            return 0;
        }
    }
    return fclose(file) == 0;
}
