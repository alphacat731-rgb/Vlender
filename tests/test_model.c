#include "model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_PROJECT "/tmp/vlender_model_test.vlmesh"
#define TEST_POSE "/tmp/vlender_model_test.vlpose"

static void write_bone(FILE *f, const char *name, int parent) {
    int i;
    fprintf(f, "B %s %d 0 0 0 0 0 1", name, parent);
    for (i = 0; i < 16; ++i) {
        int is_identity = (i == 0 || i == 5 || i == 10 || i == 15);
        fprintf(f, " %d", is_identity ? 1 : 0);
    }
    fprintf(f, " 0 0 0 0 0 0 1 1 1\n");
}

static void write_triangle(FILE *f, int count, int bad_float) {
    static const float positions[3][3] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}
    };
    int i;
    for (i = 0; i < count; ++i) {
        const float *p = positions[i % 3];
        if (bad_float && i == 0) {
            fprintf(f, "V NaN 0 0 0 0 1 0 0 1 0 1\n");
        } else {
            fprintf(f, "V %.3f %.3f %.3f 0 0 1 %.3f %.3f 1 0 1\n",
                    p[0], p[1], p[2], p[0], p[1]);
        }
    }
}

static int write_project(int bones, int cycle, int vertex_count, int bad_float) {
    FILE *f = fopen(TEST_PROJECT, "wb");
    int i;
    if (!f) return 0;
    fprintf(f, "VLENDER_MESH 1\nT texture.bmp\n");
    for (i = 0; i < bones; ++i) {
        char name[24];
        int parent = -1;
        if (cycle && bones >= 2) parent = (i == 0) ? 1 : 0;
        snprintf(name, sizeof(name), "Bone%d", i);
        write_bone(f, name, parent);
    }
    write_triangle(f, vertex_count, bad_float);
    return fclose(f) == 0;
}

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void) {
    VLModel model;
    vl_model_init(&model);

    CHECK(write_project(1, 0, 3, 0), "write valid test mesh");
    CHECK(vl_model_load_project(&model, TEST_PROJECT), "load valid project");
    CHECK(model.loaded == 1, "valid project is marked loaded");
    CHECK(model.bone_count == 1, "valid bone count");
    CHECK(model.vertex_count == 3, "valid triangle corner count");
    CHECK(model.vertex_count % 3 == 0, "triangle count is divisible by three");
    CHECK(strcmp(model.bones[0].name, "Bone0") == 0, "bone name parsed");

    model.bones[0].basis_rotation[0] = 0.25f;
    CHECK(vl_model_save_pose(&model, TEST_POSE), "save pose");
    model.bones[0].basis_rotation[0] = 0.0f;
    CHECK(vl_model_load_pose(&model, TEST_POSE) == 1, "load pose");
    CHECK(fabsf(model.bones[0].basis_rotation[0] - 0.25f) < 0.0001f,
          "pose round-trip keeps rotation");

    {
        FILE *bad_pose = fopen(TEST_POSE, "wb");
        CHECK(bad_pose != NULL, "open malformed pose");
        fprintf(bad_pose, "VLENDER_POSE 1\\nB Bone0 0 0 NaN 0 0 0 1 1 1\\n");
        CHECK(fclose(bad_pose) == 0, "close malformed pose");
    }
    CHECK(vl_model_load_pose(&model, TEST_POSE) == 0,
          "pose with non-finite transform is rejected");
    CHECK(fabsf(model.bones[0].basis_rotation[0] - 0.25f) < 0.0001f,
          "failed pose load preserves current pose");

    CHECK(write_project(1, 0, 3, 1), "write NaN project");
    CHECK(!vl_model_load_project(&model, TEST_PROJECT), "NaN project is rejected");
    CHECK(model.loaded == 1 && model.vertex_count == 3 && model.bone_count == 1,
          "failed load preserves previous valid model");

    CHECK(write_project(2, 1, 3, 0), "write cyclic hierarchy");
    CHECK(!vl_model_load_project(&model, TEST_PROJECT), "cyclic hierarchy is rejected");
    CHECK(model.loaded == 1 && model.vertex_count == 3,
          "cycle failure preserves previous valid model");

    CHECK(write_project(1, 0, 4, 0), "write incomplete triangle");
    CHECK(!vl_model_load_project(&model, TEST_PROJECT),
          "non-triangle corner count is rejected");
    CHECK(model.loaded == 1 && model.vertex_count == 3,
          "incomplete triangle failure preserves previous model");

    CHECK(!vl_model_load_project(&model, "/tmp/vlender_missing_file.vlmesh"),
          "missing file returns failure");
    CHECK(model.loaded == 1 && model.vertex_count == 3,
          "missing file preserves previous model");

    vl_model_free(&model);
    remove(TEST_PROJECT);
    remove(TEST_POSE);
    puts("All Vlender model tests passed.");
    return 0;
}
