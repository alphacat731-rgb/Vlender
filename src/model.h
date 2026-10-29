#ifndef VLENDER_MODEL_H
#define VLENDER_MODEL_H

#define VLENDER_MAX_BONES 128
#define VLENDER_MAX_CORNERS 30000
#define VLENDER_NAME_MAX 64

typedef struct {
    float position[3];
    float normal[3];
    float uv[2];
    int bone[4];
    float weight[4];
    float skinned_position[3];
    float skinned_normal[3];
} VLVertex;

typedef struct {
    char name[VLENDER_NAME_MAX];
    int parent;
    float head_local[3];
    float tail_local[3];
    float rest[16];
    float inv_rest[16];
    float basis_location[3];
    float basis_rotation[3];
    float basis_scale[3];
    float pose[16];
    float skin[16];
} VLBone;

typedef struct {
    VLVertex *vertices;
    int vertex_count;
    int vertex_capacity;
    VLBone bones[VLENDER_MAX_BONES];
    int bone_count;
    char texture_name[128];
    int loaded;
    float bounds_min[3];
    float bounds_max[3];
    float center[3];
    float radius;
} VLModel;

void vl_model_init(VLModel *model);
void vl_model_free(VLModel *model);
int vl_model_load_project(VLModel *model, const char *path);
int vl_model_load_pose(VLModel *model, const char *path);
int vl_model_save_pose(const VLModel *model, const char *path);
void vl_model_update_skin(VLModel *model);
int vl_model_find_bone(const VLModel *model, const char *name);

#endif
