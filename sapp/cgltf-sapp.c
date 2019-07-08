//------------------------------------------------------------------------------
//  cgltf-sapp.c
//
//  A simple(!) GLTF viewer, cgltf + sokol_app.h + sokol_gfx.h + sokol_fetch.h.
//  Doesn't support all GLTF features.
//
//  https://github.com/jkuhlmann/cgltf
//------------------------------------------------------------------------------
#define HANDMADE_MATH_IMPLEMENTATION
#define HANDMADE_MATH_NO_SSE
#include "HandmadeMath.h"
#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_audio.h"
#include "sokol_fetch.h"
#include "dbgui/dbgui.h"
#include "cgltf-sapp.glsl.h"
#define CGLTF_IMPLEMENTATION
#include "stb/stb_image.h"
#include "cgltf/cgltf.h"
#include <assert.h>

#define SAMPLE_COUNT (4)

#define SCENE_INVALID_INDEX (-1)
#define SCENE_MAX_BUFFERS (16)
#define SCENE_MAX_IMAGES (16)
#define SCENE_MAX_MATERIALS (16)
#define SCENE_MAX_PIPELINES (16)
#define SCENE_MAX_MESHES (16)
#define SCENE_MAX_PRIMITIVES_PER_MESH (16)   // aka submesh
#define SCENE_MAX_NODES (16)

typedef struct {
    sg_buffer_type type;
    int offset;
    int size;
    int gltf_buffer_index;
} buffer_creation_params_t;

typedef struct {
    sg_filter min_filter;
    sg_filter mag_filter;
    sg_wrap wrap_s;
    sg_wrap wrap_t;
    int gltf_image_index;
} image_creation_params_t;

typedef struct {
    sg_layout_desc layout;
    sg_primitive_type prim_type;
    sg_index_type index_type;
    bool alpha;
} pipeline_cache_params_t;

typedef struct {
    // shader uniforms
    metallic_params_t fs_params;
    // indices into scene.images[] array
    int base_color;
    int metallic_roughness;
    int normal;
    int occlusion;
    int emissive;
} metallic_material_t;

typedef struct {
    // shader uniforms
    specular_params_t fs_params;
    // indices into scene.images[] array
    int diffuse;
    int specular_glossiness;
    int normal;
    int occlusion;
    int emissive;
} specular_material_t;

typedef struct {
    bool is_metallic;
    metallic_material_t metallic;
    specular_material_t specular;
} material_t;

// map sokol-gfx buffer bind slots to scene.buffers items
typedef struct {
    int num;
    int buffer[SG_MAX_SHADERSTAGE_BUFFERS];
} vertex_buffer_mapping_t;

// a 'primitive' corresponds to a draw call
typedef struct {
    int pipeline;           // index into scene.pipelines array
    int material;           // index into materials array
    vertex_buffer_mapping_t vertex_buffers; // indices into bufferview array by vbuf bind slot
    int index_buffer;       // index into bufferview array for index buffer, or SCENE_INVALID_INDEX
    int base_element;
    int num_elements;
} primitive_t;

// a mesh is a collection of primitives
typedef struct {
    int num_primitives;
    primitive_t primitives[SCENE_MAX_PRIMITIVES_PER_MESH];
} mesh_t;

// a node associates a transform with a mesh
typedef struct {
    int mesh;           // index into scene.meshes, or SCENE_INVALID_INDEX
    hmm_mat4 transform;
} node_t;

// camera helper struct
typedef struct {
    float rx, ry;
    hmm_mat4 view_proj;
} camera_t;

typedef struct {
    int num_buffers;
    int num_images;
    int num_pipelines;
    int num_materials;
    int num_meshes;
    int num_nodes;

    // sokol-gfx resource and scene structure
    sg_buffer buffers[SCENE_MAX_BUFFERS];
    sg_image images[SCENE_MAX_IMAGES];
    sg_pipeline pipelines[SCENE_MAX_PIPELINES];
    material_t materials[SCENE_MAX_MATERIALS];
    mesh_t meshes[SCENE_MAX_MESHES];
    node_t nodes[SCENE_MAX_NODES];

    // helper data for async resource creation and a 'pipeline cash'
    buffer_creation_params_t buffer_params[SCENE_MAX_BUFFERS];
    image_creation_params_t image_params[SCENE_MAX_IMAGES];
    pipeline_cache_params_t pipeline_cache[SCENE_MAX_PIPELINES];
} scene_t;

static const char* filename = "DamagedHelmet.gltf";

static void gltf_parse(const void* ptr, uint64_t num_bytes);
static void gltf_parse_buffers(const cgltf_data* gltf);
static void gltf_parse_images(const cgltf_data* gltf);
static void gltf_parse_materials(const cgltf_data* gltf);
static void gltf_parse_meshes(const cgltf_data* gltf);
static void gltf_parse_nodes(const cgltf_data* gltf);

static void gltf_fetch_callback(const sfetch_response_t*);
static void gltf_buffer_fetch_callback(const sfetch_response_t*);
static void gltf_image_fetch_callback(const sfetch_response_t*);

static void create_sg_buffers_for_gltf_buffer(int gltf_buffer_index, const uint8_t* bytes, int num_bytes);
static void create_sg_images_for_gltf_image(int gltf_image_index, const uint8_t* bytes, int num_bytes);
static vertex_buffer_mapping_t create_vertex_buffer_mapping_for_gltf_primitive(const cgltf_data* gltf, const cgltf_primitive* prim);
static int create_sg_pipeline_for_gltf_primitive(const cgltf_data* gltf, const cgltf_primitive* prim, const vertex_buffer_mapping_t* vbuf_map);
static hmm_mat4 build_transform_for_gltf_node(const cgltf_data* gltf, const cgltf_node* node);

static void update_camera(int framebuffer_width, int framebuffer_height);
static vs_params_t vs_params_for_node(int node_index);

static struct {
    bool failed;
    sg_pass_action pass_action;
    sg_pass_action failed_pass_action;
    sg_shader metallic_shader;
    sg_shader specular_shader;
    scene_t scene;
    camera_t camera;
} state;

static void init(void) {
    // setup sokol-gfx
    sg_setup(&(sg_desc){
        .gl_force_gles2 = true,
        .mtl_device = sapp_metal_get_device(),
        .mtl_renderpass_descriptor_cb = sapp_metal_get_renderpass_descriptor,
        .mtl_drawable_cb = sapp_metal_get_drawable,
        .d3d11_device = sapp_d3d11_get_device(),
        .d3d11_device_context = sapp_d3d11_get_device_context(),
        .d3d11_render_target_view_cb = sapp_d3d11_get_render_target_view,
        .d3d11_depth_stencil_view_cb = sapp_d3d11_get_depth_stencil_view
    });
    // setup the optional debugging UI
    __dbgui_setup(SAMPLE_COUNT);

    // setup sokol-fetch with 2 channels and 6 lanes per channel,
    // we'll use one channel for mesh data and the other for textures
    sfetch_setup(&(sfetch_desc_t){
        .max_requests = 32,
        .num_channels = 2,
        .num_lanes = 6
    });

    // normal background color, and a "load failed" background color
    state.pass_action = (sg_pass_action) {
        .colors[0] = { .action=SG_ACTION_CLEAR, .val={0.0f, 0.569f, 0.918f, 1.0f} }
    };
    state.failed_pass_action = (sg_pass_action) {
        .colors[0] = { .action=SG_ACTION_CLEAR, .val={1.0f, 0.0f, 0.0f, 1.0f} }
    };

    // create shaders
    state.metallic_shader = sg_make_shader(cgltf_metallic_shader_desc());
    state.specular_shader = sg_make_shader(cgltf_specular_shader_desc());

    // start loading the base gltf file...
    sfetch_send(&(sfetch_request_t){
        .channel = 0,
        .path = filename,
        .callback = gltf_fetch_callback,
    });
}

static void frame(void) {
    // pump the sokol-fetch message queue
    sfetch_dowork();

    const int fb_width = sapp_width();
    const int fb_height = sapp_height();
    update_camera(fb_width, fb_height);

    // render the scene
    if (state.failed) {
        sg_begin_default_pass(&state.failed_pass_action, fb_width, fb_height);
        __dbgui_draw();
        sg_end_pass();
    }
    else {
        sg_begin_default_pass(&state.pass_action, fb_width, fb_height);
        for (int node_index = 0; node_index < state.scene.num_nodes; node_index++) {
            const node_t* node = &state.scene.nodes[node_index];
            if (node->mesh == SCENE_INVALID_INDEX) {
                continue;
            }
            vs_params_t vs_params = vs_params_for_node(node_index);
            const mesh_t* mesh = &state.scene.meshes[node->mesh];
            for (int prim_index = 0; prim_index < mesh->num_primitives; prim_index++) {
                const primitive_t* prim = &mesh->primitives[prim_index];
                const material_t* mat = &state.scene.materials[prim->material];
                sg_apply_pipeline(state.scene.pipelines[prim->pipeline]);
                sg_bindings bind = { 0 };
                for (int vb_slot = 0; vb_slot < prim->vertex_buffers.num; vb_slot++) {
                    bind.vertex_buffers[vb_slot] = state.scene.buffers[prim->vertex_buffers.buffer[vb_slot]];
                }
                if (prim->index_buffer != SCENE_INVALID_INDEX) {
                    bind.index_buffer = state.scene.buffers[prim->index_buffer];
                }
                bind.fs_images[SLOT_occlusion_texture] = state.scene.images[mat->metallic.occlusion];
                sg_apply_bindings(&bind);
                sg_apply_uniforms(SG_SHADERSTAGE_VS, SLOT_vs_params, &vs_params, sizeof(vs_params));
                if (mat->is_metallic) {
                    sg_apply_uniforms(SG_SHADERSTAGE_FS,
                        SLOT_metallic_params,
                        &mat->metallic.fs_params,
                        sizeof(metallic_params_t));
                }
                else {
                    sg_apply_uniforms(SG_SHADERSTAGE_VS,
                        SLOT_specular_params,
                        &mat->specular.fs_params,
                        sizeof(specular_params_t));
                }
                sg_draw(prim->base_element, prim->num_elements, 1);
            }
        }
        __dbgui_draw();
        sg_end_pass();
    }
    sg_commit();
}

static void cleanup(void) {
    sfetch_shutdown();
    __dbgui_shutdown();
    sg_shutdown();
}

// load-callback for the GLTF base file
static void gltf_fetch_callback(const sfetch_response_t* response) {
    if (response->opened) {
        // allocate and bind buffer to load file into
        sfetch_bind_buffer(response->handle, malloc(response->content_size), response->content_size);
    }
    else if (response->fetched) {
        // file has been loaded, parse as GLTF
        gltf_parse(response->buffer_ptr, response->fetched_size);
    }
    if (response->finished) {
        // don't forget to free the buffer (note: it's valid to call free()
        // with a null pointer
        free(sfetch_unbind_buffer(response->handle));
        if (response->failed) {
            state.failed = true;
        }
    }
}

// load-callback for GLTF buffer files
typedef struct {
    cgltf_size buffer_index;
} gltf_buffer_fetch_userdata_t;

static void gltf_buffer_fetch_callback(const sfetch_response_t* response) {
    if (response->opened) {
        sfetch_bind_buffer(response->handle, malloc(response->content_size), response->content_size);
    }
    else if (response->fetched) {
        const gltf_buffer_fetch_userdata_t* user_data = (const gltf_buffer_fetch_userdata_t*)response->user_data;
        int gltf_buffer_index = (int)user_data->buffer_index;
        create_sg_buffers_for_gltf_buffer(
            gltf_buffer_index,
            (const uint8_t*)response->buffer_ptr,
            (int)response->fetched_size);
    }
    if (response->finished) {
        free(sfetch_unbind_buffer(response->handle));
        if (response->failed) {
            state.failed = true;
        }
    }
}

// load-callback for GLTF image files
typedef struct {
    cgltf_size image_index;
} gltf_image_fetch_userdata_t;

static void gltf_image_fetch_callback(const sfetch_response_t* response) {
    if (response->opened) {
        sfetch_bind_buffer(response->handle, malloc(response->content_size), response->content_size);
    }
    else if (response->fetched) {
        const gltf_image_fetch_userdata_t* user_data = (const gltf_image_fetch_userdata_t*)response->user_data;
        int gltf_image_index = (int)user_data->image_index;
        create_sg_images_for_gltf_image(
            gltf_image_index,
            (const uint8_t*)response->buffer_ptr,
            (int)response->fetched_size);
    }
    if (response->finished) {
        free(sfetch_unbind_buffer(response->handle));
        if (response->failed) {
            state.failed = true;
        }
    }
}

// load GLTF data from memory, build scene and issue resource fetch requests
static void gltf_parse(const void* ptr, uint64_t num_bytes) {
    cgltf_options options = { 0 };
    cgltf_data* data = 0;
    const cgltf_result result = cgltf_parse(&options, ptr, num_bytes, &data);
    if (result == cgltf_result_success) {
        gltf_parse_buffers(data);
        gltf_parse_images(data);
        gltf_parse_materials(data);
        gltf_parse_meshes(data);
        gltf_parse_nodes(data);
        cgltf_free(data);
    }
}

// compute indices from cgltf element pointers
static int gltf_buffer_index(const cgltf_data* gltf, const cgltf_buffer* buf) {
    assert(buf);
    return buf - gltf->buffers;
}

static int gltf_bufferview_index(const cgltf_data* gltf, const cgltf_buffer_view* buf_view) {
    assert(buf_view);
    return buf_view - gltf->buffer_views;
}

static int gltf_image_index(const cgltf_data* gltf, const cgltf_image* img) {
    assert(img);
    return img - gltf->images;
}

static int gltf_texture_index(const cgltf_data* gltf, const cgltf_texture* tex) {
    assert(tex);
    return tex - gltf->textures;
}

static int gltf_material_index(const cgltf_data* gltf, const cgltf_material* mat) {
    assert(mat);
    return mat - gltf->materials;
}

static int gltf_mesh_index(const cgltf_data* gltf, const cgltf_mesh* mesh) {
    assert(mesh);
    return mesh - gltf->meshes;
}

// parse the GLTF buffer definitions and start loading buffer blobs
static void gltf_parse_buffers(const cgltf_data* gltf) {
    if (gltf->buffer_views_count > SCENE_MAX_BUFFERS) {
        state.failed = true;
        return;
    }

    // parse the buffer-view attributes
    state.scene.num_buffers = gltf->buffer_views_count;
    for (int i = 0; i < state.scene.num_buffers; i++) {
        const cgltf_buffer_view* gltf_buf_view = &gltf->buffer_views[i];
        buffer_creation_params_t* p = &state.scene.buffer_params[i];
        p->gltf_buffer_index = gltf_buffer_index(gltf, gltf_buf_view->buffer);
        p->offset = gltf_buf_view->offset;
        p->size = gltf_buf_view->size;
        if (gltf_buf_view->type == cgltf_buffer_view_type_indices) {
            p->type = SG_BUFFERTYPE_INDEXBUFFER;
        }
        else {
            p->type = SG_BUFFERTYPE_VERTEXBUFFER;
        }
        // allocate a sokol-gfx buffer handle
        state.scene.buffers[i] = sg_alloc_buffer();
    }

    // start loading all buffers
    for (cgltf_size i = 0; i < gltf->buffers_count; i++) {
        const cgltf_buffer* gltf_buf = &gltf->buffers[i];
        gltf_buffer_fetch_userdata_t user_data = {
            .buffer_index = i
        };
        sfetch_send(&(sfetch_request_t){
            .channel = 0,
            .path = gltf_buf->uri,
            .callback = gltf_buffer_fetch_callback,
            .user_data_ptr = &user_data,
            .user_data_size = sizeof(user_data)
        });
    }
}

// parse all the image-related stuff in the GLTF data

// https://github.com/KhronosGroup/glTF/tree/master/specification/2.0#samplerminfilter
static sg_filter gltf_to_sg_filter(int gltf_filter) {
    switch (gltf_filter) {
        case 9728: return SG_FILTER_NEAREST;
        case 9729: return SG_FILTER_LINEAR;
        case 9984: return SG_FILTER_NEAREST_MIPMAP_NEAREST;
        case 9985: return SG_FILTER_LINEAR_MIPMAP_NEAREST;
        case 9986: return SG_FILTER_NEAREST_MIPMAP_LINEAR;
        case 9987: return SG_FILTER_LINEAR_MIPMAP_LINEAR;
        default: return SG_FILTER_LINEAR;
    }
}

// https://github.com/KhronosGroup/glTF/tree/master/specification/2.0#samplerwraps
static sg_wrap gltf_to_sg_wrap(int gltf_wrap) {
    switch (gltf_wrap) {
        case 33071: return SG_WRAP_CLAMP_TO_EDGE;
        case 33648: return SG_WRAP_MIRRORED_REPEAT;
        case 10497: return SG_WRAP_REPEAT;
        default: return SG_WRAP_REPEAT;
    }
}

static void gltf_parse_images(const cgltf_data* gltf) {
    if (gltf->textures_count > SCENE_MAX_IMAGES) {
        state.failed = true;
        return;
    }

    // parse the texture and sampler attributes
    state.scene.num_images = gltf->textures_count;
    for (int i = 0; i < state.scene.num_images; i++) {
        const cgltf_texture* gltf_tex = &gltf->textures[i];
        image_creation_params_t* p = &state.scene.image_params[i];
        p->gltf_image_index = gltf_image_index(gltf, gltf_tex->image);
        p->min_filter = gltf_to_sg_filter(gltf_tex->sampler->min_filter);
        p->mag_filter = gltf_to_sg_filter(gltf_tex->sampler->mag_filter);
        p->wrap_s = gltf_to_sg_wrap(gltf_tex->sampler->wrap_s);
        p->wrap_t = gltf_to_sg_wrap(gltf_tex->sampler->wrap_t);
        // allocate a sokol-gfx image handle
        state.scene.images[i] = sg_alloc_image();
    }

    // start loading all images
    for (cgltf_size i = 0; i < gltf->images_count; i++) {
        const cgltf_image* gltf_img = &gltf->images[i];
        gltf_image_fetch_userdata_t user_data = {
            .image_index = i
        };
        sfetch_send(&(sfetch_request_t){
            .channel = 1,
            .path = gltf_img->uri,
            .callback = gltf_image_fetch_callback,
            .user_data_ptr = &user_data,
            .user_data_size = sizeof(user_data)
        });
    }
}

// parse GLTF materials into our own material definition
static void gltf_parse_materials(const cgltf_data* gltf) {
    if (gltf->materials_count > SCENE_MAX_MATERIALS) {
        state.failed = true;
        return;
    }
    state.scene.num_materials = gltf->materials_count;
    for (int i = 0; i < state.scene.num_materials; i++) {
        const cgltf_material* gltf_mat = &gltf->materials[i];
        material_t* scene_mat = &state.scene.materials[i];
        scene_mat->is_metallic = gltf_mat->has_pbr_metallic_roughness;
        if (scene_mat->is_metallic) {
            const cgltf_pbr_metallic_roughness* src = &gltf_mat->pbr_metallic_roughness;
            metallic_material_t* dst = &scene_mat->metallic;
            for (int d = 0; d < 4; d++) {
                dst->fs_params.base_color_factor.Elements[d] = src->base_color_factor[d];
            }
            for (int d = 0; d < 3; d++) {
                dst->fs_params.emissive_factor.Elements[d] = gltf_mat->emissive_factor[d];
            }
            dst->fs_params.metallic_factor = src->metallic_factor;
            dst->fs_params.roughness_factor = src->roughness_factor;
            dst->base_color = gltf_texture_index(gltf, src->base_color_texture.texture);
            dst->metallic_roughness = gltf_texture_index(gltf, src->metallic_roughness_texture.texture);
            dst->normal = gltf_texture_index(gltf, gltf_mat->normal_texture.texture);
            dst->occlusion = gltf_texture_index(gltf, gltf_mat->occlusion_texture.texture);
            dst->emissive = gltf_texture_index(gltf, gltf_mat->emissive_texture.texture);
        }
        else {
            const cgltf_pbr_specular_glossiness* src = &gltf_mat->pbr_specular_glossiness;
            specular_material_t* dst = &scene_mat->specular;
            for (int d = 0; d < 4; d++) {
                dst->fs_params.diffuse_factor.Elements[d] = src->diffuse_factor[d];
            }
            for (int d = 0; d < 3; d++) {
                dst->fs_params.specular_factor.Elements[d] = src->specular_factor[d];
            }
            for (int d = 0; d < 3; d++) {
                dst->fs_params.emissive_factor.Elements[d] = gltf_mat->emissive_factor[d];
            }
            dst->fs_params.glossiness_factor = src->glossiness_factor;
            dst->diffuse = gltf_texture_index(gltf, src->diffuse_texture.texture);
            dst->specular_glossiness = gltf_texture_index(gltf, src->specular_glossiness_texture.texture);
            dst->normal = gltf_texture_index(gltf, gltf_mat->normal_texture.texture);
            dst->occlusion = gltf_texture_index(gltf, gltf_mat->occlusion_texture.texture);
            dst->emissive = gltf_texture_index(gltf, gltf_mat->emissive_texture.texture);
        }
    }
}

// parse GLTF meshes into our own mesh definition
static void gltf_parse_meshes(const cgltf_data* gltf) {
    if (gltf->meshes_count > SCENE_MAX_MESHES) {
        state.failed = true;
        return;
    }
    state.scene.num_meshes = gltf->meshes_count;
    for (cgltf_size mesh_index = 0; mesh_index < gltf->meshes_count; mesh_index++) {
        const cgltf_mesh* gltf_mesh = &gltf->meshes[mesh_index];
        if (gltf_mesh->primitives_count > SCENE_MAX_PRIMITIVES_PER_MESH) {
            state.failed = true;
            return;
        }
        mesh_t* mesh = &state.scene.meshes[mesh_index];
        mesh->num_primitives = gltf_mesh->primitives_count;
        for (cgltf_size prim_index = 0; prim_index < gltf_mesh->primitives_count; prim_index++) {
            const cgltf_primitive* gltf_prim = &gltf_mesh->primitives[prim_index];
            primitive_t* prim = &mesh->primitives[prim_index];

            // a mapping from sokol-gfx vertex buffer bind slots into the scene.buffers array
            prim->vertex_buffers = create_vertex_buffer_mapping_for_gltf_primitive(gltf, gltf_prim);
            // create or reuse a matching pipeline state object
            prim->pipeline = create_sg_pipeline_for_gltf_primitive(gltf, gltf_prim, &prim->vertex_buffers);
            // the material parameters
            prim->material = gltf_material_index(gltf, gltf_prim->material);
            // index buffer, base element, num elements
            if (gltf_prim->indices) {
                prim->index_buffer = gltf_bufferview_index(gltf, gltf_prim->indices->buffer_view);
                assert(state.scene.buffer_params[prim->index_buffer].type == SG_BUFFERTYPE_INDEXBUFFER);
                assert(gltf_prim->indices->stride != 0);
                prim->base_element = 0;
                prim->num_elements = gltf_prim->indices->count;
            }
            else {
                // hmm... looking up the number of elements to render from
                // a random vertex component accessor looks a bit shady
                prim->index_buffer = SCENE_INVALID_INDEX;
                prim->base_element = 0;
                prim->num_elements = gltf_prim->attributes->data->count;
            }
        }
    }
}

// parse GLTF nodes into our own node definition
static void gltf_parse_nodes(const cgltf_data* gltf) {
    if (gltf->nodes_count > SCENE_MAX_NODES) {
        state.failed = true;
        return;
    }
    state.scene.num_nodes = gltf->nodes_count;
    for (cgltf_size node_index = 0; node_index < gltf->nodes_count; node_index++) {
        const cgltf_node* gltf_node = &gltf->nodes[node_index];
        node_t* node = &state.scene.nodes[node_index];
        if (gltf_node->mesh) {
            node->mesh = gltf_mesh_index(gltf, gltf_node->mesh);
        }
        else {
            node->mesh = SCENE_INVALID_INDEX;
        }
        node->transform = build_transform_for_gltf_node(gltf, gltf_node);
    }
}

// create the sokol-gfx buffer objects associated with a GLTF buffer view
static void create_sg_buffers_for_gltf_buffer(int gltf_buffer_index, const uint8_t* bytes, int num_bytes) {
    for (int i = 0; i < state.scene.num_buffers; i++) {
        const buffer_creation_params_t* p = &state.scene.buffer_params[i];
        if (p->gltf_buffer_index == gltf_buffer_index) {
            assert((p->offset + p->size) <= num_bytes);
            sg_init_buffer(state.scene.buffers[i], &(sg_buffer_desc){
                .type = p->type,
                .size = p->size,
                .content = bytes + p->offset
            });
        }
    }
}

// create the sokol-gfx image objects associated with a GLTF image
static void create_sg_images_for_gltf_image(int gltf_image_index, const uint8_t* bytes, int num_bytes) {
    for (int i = 0; i < state.scene.num_images; i++) {
        image_creation_params_t* p = &state.scene.image_params[i];
        if (p->gltf_image_index == gltf_image_index) {
            // assume this is an image which can be decoded by stb_image.h
            int img_width, img_height, num_channels;
            const int desired_channels = 4;
            stbi_uc* pixels = stbi_load_from_memory(
                bytes, num_bytes,
                &img_width, &img_height,
                &num_channels, desired_channels);
            if (pixels) {
                /* ok, time to actually initialize the sokol-gfx texture */
                sg_init_image(state.scene.images[i], &(sg_image_desc){
                    .width = img_width,
                    .height = img_height,
                    .pixel_format = SG_PIXELFORMAT_RGBA8,
                    .min_filter = p->min_filter,
                    .mag_filter = p->mag_filter,
                    .content.subimage[0][0] = {
                        .ptr = pixels,
                        .size = img_width * img_height * 4,
                    }
                });
                stbi_image_free(pixels);
            }
        }
    }
}

static sg_vertex_format gltf_to_vertex_format(cgltf_accessor* acc) {
    switch (acc->component_type) {
        case cgltf_component_type_r_8:
            if (acc->type == cgltf_type_vec4) {
                return acc->normalized ? SG_VERTEXFORMAT_BYTE4N : SG_VERTEXFORMAT_BYTE4;
            }
            break;
        case cgltf_component_type_r_8u:
            if (acc->type == cgltf_type_vec4) {
                return acc->normalized ? SG_VERTEXFORMAT_UBYTE4N : SG_VERTEXFORMAT_UBYTE4;
            }
            break;
        case cgltf_component_type_r_16:
            switch (acc->type) {
                case cgltf_type_vec2: return acc->normalized ? SG_VERTEXFORMAT_SHORT2N : SG_VERTEXFORMAT_SHORT2;
                case cgltf_type_vec4: return acc->normalized ? SG_VERTEXFORMAT_SHORT4N : SG_VERTEXFORMAT_SHORT4;
                default: break;
            }
            break;
        case cgltf_component_type_r_32f:
            switch (acc->type) {
                case cgltf_type_scalar: return SG_VERTEXFORMAT_FLOAT;
                case cgltf_type_vec2: return SG_VERTEXFORMAT_FLOAT2;
                case cgltf_type_vec3: return SG_VERTEXFORMAT_FLOAT3;
                case cgltf_type_vec4: return SG_VERTEXFORMAT_FLOAT4;
                default: break;
            }
            break;
        default: break;
    }
    return SG_VERTEXFORMAT_INVALID;
}

static int gltf_attr_type_to_vs_input_slot(cgltf_attribute_type attr_type) {
    switch (attr_type) {
        case cgltf_attribute_type_position: return ATTR_vs_position;
        case cgltf_attribute_type_normal: return ATTR_vs_normal;
        case cgltf_attribute_type_texcoord: return ATTR_vs_texcoord;
        default: return SCENE_INVALID_INDEX;
    }
}

static sg_primitive_type gltf_to_prim_type(cgltf_primitive_type prim_type) {
    switch (prim_type) {
        case cgltf_primitive_type_points: return SG_PRIMITIVETYPE_POINTS;
        case cgltf_primitive_type_lines: return SG_PRIMITIVETYPE_LINES;
        case cgltf_primitive_type_line_strip: return SG_PRIMITIVETYPE_LINE_STRIP;
        case cgltf_primitive_type_triangles: return SG_PRIMITIVETYPE_TRIANGLES;
        case cgltf_primitive_type_triangle_strip: return SG_PRIMITIVETYPE_TRIANGLE_STRIP;
        default: return _SG_PRIMITIVETYPE_DEFAULT;
    }
}

static sg_primitive_type gltf_to_index_type(const cgltf_primitive* prim) {
    if (prim->indices) {
        if (prim->indices->component_type == cgltf_component_type_r_16u) {
            return SG_INDEXTYPE_UINT16;
        }
        else {
            return SG_INDEXTYPE_UINT32;
        }
    }
    else {
        return SG_INDEXTYPE_NONE;
    }
}

// creates a vertex buffer bind slot mapping for a specific GLTF primitive
static vertex_buffer_mapping_t create_vertex_buffer_mapping_for_gltf_primitive(const cgltf_data* gltf, const cgltf_primitive* prim) {
    vertex_buffer_mapping_t map = { 0 };
    for (int i = 0; i < SG_MAX_SHADERSTAGE_BUFFERS; i++) {
        map.buffer[i] = SCENE_INVALID_INDEX;
    }
    for (cgltf_size attr_index = 0; attr_index < prim->attributes_count; attr_index++) {
        const cgltf_attribute* attr = &prim->attributes[attr_index];
        const cgltf_accessor* acc = attr->data;
        int buffer_view_index  = gltf_bufferview_index(gltf, acc->buffer_view);
        int i = 0;
        for (; i < map.num; i++) {
            if (map.buffer[i] == buffer_view_index) {
                break;
            }
        }
        if ((i == map.num) && (map.num < SG_MAX_SHADERSTAGE_BUFFERS)) {
            map.buffer[map.num++] = buffer_view_index;
        }
        assert(map.num <= SG_MAX_SHADERSTAGE_BUFFERS);
    }
    return map;
}

static sg_layout_desc create_sg_layout_for_gltf_primitive(const cgltf_data* gltf, const cgltf_primitive* prim, const vertex_buffer_mapping_t* vbuf_map) {
    assert(prim->attributes_count <= SG_MAX_VERTEX_ATTRIBUTES);
    sg_layout_desc layout = { 0 };
    for (cgltf_size attr_index = 0; attr_index < prim->attributes_count; attr_index++) {
        const cgltf_attribute* attr = &prim->attributes[attr_index];
        int attr_slot = gltf_attr_type_to_vs_input_slot(attr->type);
        if (attr_slot != SCENE_INVALID_INDEX) {
            layout.attrs[attr_slot].format = gltf_to_vertex_format(attr->data);
        }
        int buffer_view_index = gltf_bufferview_index(gltf, attr->data->buffer_view);
        for (int vb_slot = 0; vb_slot < vbuf_map->num; vb_slot++) {
            if (vbuf_map->buffer[vb_slot] == buffer_view_index) {
                layout.attrs[attr_slot].buffer_index = vb_slot;
            }
        }
    }
    return layout;
}

// helper to compare to pipeline-cache items
static bool pipelines_equal(const pipeline_cache_params_t* p0, const pipeline_cache_params_t* p1) {
    if (p0->prim_type != p1->prim_type) {
        return false;
    }
    if (p0->alpha != p1->alpha) {
        return false;
    }
    if (p0->index_type != p1->index_type) {
        return false;
    }
    for (int i = 0; i < SG_MAX_VERTEX_ATTRIBUTES; i++) {
        const sg_vertex_attr_desc* a0 = &p0->layout.attrs[i];
        const sg_vertex_attr_desc* a1 = &p1->layout.attrs[i];
        if ((a0->buffer_index != a1->buffer_index) ||
            (a0->offset != a1->offset) ||
            (a0->format != a1->format))
        {
            return false;
        }
    }
    return true;
}

// Create a unique sokol-gfx pipeline object for GLTF primitive (aka submesh),
// maintains a cache of shared, unique pipeline objects. Returns an index
// into state.scene.pipelines
static int create_sg_pipeline_for_gltf_primitive(const cgltf_data* gltf, const cgltf_primitive* prim, const vertex_buffer_mapping_t* vbuf_map) {
    pipeline_cache_params_t pip_params = {
        .layout = create_sg_layout_for_gltf_primitive(gltf, prim, vbuf_map),
        .prim_type = gltf_to_prim_type(prim->type),
        .index_type = gltf_to_index_type(prim),
        .alpha = prim->material->alpha_mode != cgltf_alpha_mode_opaque
    };
    int i = 0;
    for (; i < state.scene.num_pipelines; i++) {
        if (pipelines_equal(&state.scene.pipeline_cache[i], &pip_params)) {
            // an indentical pipeline already exists, reuse this
            assert(state.scene.pipelines[i].id != SG_INVALID_ID);
            return i;
        }
    }
    if ((i == state.scene.num_pipelines) && (state.scene.num_pipelines < SCENE_MAX_PIPELINES)) {
        state.scene.pipeline_cache[i] = pip_params;
        const bool is_metallic = prim->material->has_pbr_metallic_roughness;
        state.scene.pipelines[i] = sg_make_pipeline(&(sg_pipeline_desc){
            .layout = pip_params.layout,
            .shader = is_metallic ? state.metallic_shader : state.specular_shader,
            .primitive_type = pip_params.prim_type,
            .index_type = pip_params.index_type,
            .depth_stencil = {
                .depth_write_enabled = !pip_params.alpha,
                .depth_compare_func = SG_COMPAREFUNC_LESS_EQUAL,
            },
            .blend = {
                .enabled = pip_params.alpha,
                .src_factor_rgb = pip_params.alpha ? SG_BLENDFACTOR_SRC_ALPHA : 0,
                .dst_factor_rgb = pip_params.alpha ? SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : 0,
                .color_write_mask = pip_params.alpha ? SG_COLORMASK_RGB : 0,
            },
            .rasterizer = {
                .cull_mode = SG_CULLMODE_BACK,
                .face_winding = SG_FACEWINDING_CCW,
                .sample_count = SAMPLE_COUNT,
            }
        });
        state.scene.num_pipelines++;
    }
    assert(state.scene.num_pipelines <= SCENE_MAX_PIPELINES);
    return i;
}

static hmm_mat4 build_transform_for_gltf_node(const cgltf_data* gltf, const cgltf_node* node) {
    hmm_mat4 parent_tform = HMM_Mat4d(1);
    if (node->parent) {
        parent_tform = build_transform_for_gltf_node(gltf, node->parent);
    }
    hmm_mat4 tform = HMM_Mat4d(1);
    if (node->has_matrix) {
        // needs testing, not sure if the element order is correct
        tform = *(hmm_mat4*)node->matrix;
    }
    else {
        hmm_mat4 translate = HMM_Mat4d(1);
        hmm_mat4 rotate = HMM_Mat4d(1);
        hmm_mat4 scale = HMM_Mat4d(1);
        if (node->has_translation) {
            translate = HMM_Translate(HMM_Vec3(node->translation[0], node->translation[1], node->translation[2]));
        }
        if (node->has_rotation) {
            rotate = HMM_QuaternionToMat4(HMM_Quaternion(node->rotation[0], node->rotation[1], node->rotation[2], node->rotation[3]));
        }
        if (node->has_scale) {
            scale = HMM_Scale(HMM_Vec3(node->scale[0], node->scale[1], node->scale[2]));
        }
        // NOTE: not sure if the multiplication order is correct
        tform = HMM_MultiplyMat4(HMM_MultiplyMat4(HMM_MultiplyMat4(scale, rotate), translate), parent_tform);
    }
    return tform;
}

static void update_camera(int framebuffer_width, int framebuffer_height) {
    const float w = (float) framebuffer_width;
    const float h = (float) framebuffer_height;
    const float dist = 3.0f;
    float eye_x = dist * sin(state.camera.ry);
    float eye_y = dist * cos(state.camera.ry);
    hmm_mat4 proj = HMM_Perspective(60.0f, w/h, 0.01f, 100.0f);
    hmm_mat4 view = HMM_LookAt(HMM_Vec3(eye_x, 1.5f, eye_y), HMM_Vec3(0.0f, 0.0f, 0.0f), HMM_Vec3(0.0f, 1.0f, 0.0f));
    state.camera.view_proj = HMM_MultiplyMat4(proj, view);
    state.camera.rx += 0.01f;
    state.camera.ry += 0.02f;
}

static vs_params_t vs_params_for_node(int node_index) {
    vs_params_t vs_params = {
        .mvp = HMM_MultiplyMat4(state.camera.view_proj, state.scene.nodes[node_index].transform)
    };
    return vs_params;
}

sapp_desc sokol_main(int argc, char* argv[]) {
    return (sapp_desc){
        .init_cb = init,
        .frame_cb = frame,
        .cleanup_cb = cleanup,
        .event_cb = __dbgui_event,
        .width = 800,
        .height = 600,
        .sample_count = SAMPLE_COUNT,
        .gl_force_gles2 = true,
        .window_title = "GLTF Viewer",
    };
}


