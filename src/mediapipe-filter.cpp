#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/threading.h>
#include <util/platform.h>
#include <mutex>
#include <atomic>
#include <thread>
#include "framework/vision_engine.hpp"
#include "engines/mediapipe_onnx_engine.hpp"

struct mediapipe_filter_data {
    obs_source_t *context;
    
    std::unique_ptr<stts::VisionEngine> engine;
    std::string model_path;
    
    std::mutex result_mutex;
    stts::VisionResult latest_result;
    
    // Threading
    std::atomic<bool> processing{false};
    std::thread worker_thread;
    std::atomic<bool> stop_thread{false};
    
    // Input/Output handling
    gs_stagesurf_t *stage_surface = nullptr;
    gs_texrender_t *texrender = nullptr;
    uint8_t *tex_data = nullptr;
    uint32_t tex_width = 0;
    uint32_t tex_height = 0;
    
    // Deferred resize (allocated in tick, not render)
    std::atomic<bool> needs_resize{false};
    uint32_t pending_width = 0;
    uint32_t pending_height = 0;
};

static const char *mediapipe_get_name(void *type_data)
{
    UNUSED_PARAMETER(type_data);
    return "Mediapipe Face Mesh (ONNX)";
}

static void *mediapipe_create(obs_data_t *settings, obs_source_t *context)
{
    mediapipe_filter_data *data = new mediapipe_filter_data;
    data->context = context;
    
    data->engine = std::make_unique<stts::MediapipeOnnxEngine>();
    data->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    
    obs_source_update(context, settings);
    return data;
}

static void mediapipe_destroy(void *data_ptr)
{
    mediapipe_filter_data *data = (mediapipe_filter_data *)data_ptr;
    
    data->stop_thread = true;
    if (data->worker_thread.joinable()) {
        data->worker_thread.join();
    }
    
    obs_enter_graphics();
    if (data->stage_surface) {
        gs_stagesurface_destroy(data->stage_surface);
    }
    if (data->texrender) {
        gs_texrender_destroy(data->texrender);
    }
    obs_leave_graphics();
    
    if (data->tex_data) {
        bfree(data->tex_data);
    }
    
    delete data;
}

static void mediapipe_update(void *data_ptr, obs_data_t *settings)
{
    mediapipe_filter_data *data = (mediapipe_filter_data *)data_ptr;
    
    std::string new_path = obs_data_get_string(settings, "model_path");
    if (new_path != data->model_path) {
        data->model_path = new_path;
        if (!data->model_path.empty()) {
            data->engine->initialize(data->model_path);
        }
    }
}

static void mediapipe_video_render(void *data_ptr, gs_effect_t *effect)
{
    UNUSED_PARAMETER(effect);
    mediapipe_filter_data *data = (mediapipe_filter_data *)data_ptr;
    
    // 1. Pass through video to screen
    obs_source_skip_video_filter(data->context);
    
    // 2. Capture frame for inference (throttled)
    if (!data->processing && !data->model_path.empty()) {
        obs_source_t *target = obs_filter_get_target(data->context);
        if (target) {
            uint32_t width = obs_source_get_base_width(target);
            uint32_t height = obs_source_get_base_height(target);
            
            if (width > 0 && height > 0) {
                bool size_changed = (data->tex_width != width || data->tex_height != height);
                
                // Ensure stage surface exists (GPU objects must be created on graphics thread)
                if (!data->stage_surface || size_changed) {
                    if (data->stage_surface) gs_stagesurface_destroy(data->stage_surface);
                    data->stage_surface = gs_stagesurface_create(width, height, GS_BGRA);
                    data->tex_width = width;
                    data->tex_height = height;
                }
                
                // Defer CPU allocation to tick to avoid allocation in render callback
                if (size_changed) {
                    data->pending_width = width;
                    data->pending_height = height;
                    data->needs_resize.store(true, std::memory_order_release);
                    return;
                }
                
                // Render target to our texture
                gs_texrender_reset(data->texrender);
                if (gs_texrender_begin(data->texrender, width, height)) {
                    struct vec4 clear_color;
                    vec4_zero(&clear_color);
                    gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
                    
                    obs_source_video_render(target);
                    
                    gs_texrender_end(data->texrender);
                    
                    // Copy to stage
                    gs_texture_t *tex = gs_texrender_get_texture(data->texrender);
                    if (tex && data->tex_data) {
                        gs_stage_texture(data->stage_surface, tex);
                        
                        uint8_t *ptr;
                        uint32_t linesize;
                        if (gs_stagesurface_map(data->stage_surface, &ptr, &linesize)) {
                            for(uint32_t y=0; y<height; y++) {
                                memcpy(data->tex_data + (y * width * 4), ptr + (y * linesize), width * 4);
                            }
                            
                            gs_stagesurface_unmap(data->stage_surface);
                            
                            // Start worker
                            data->processing = true;
                            if (data->worker_thread.joinable()) data->worker_thread.join();
                            
                            data->worker_thread = std::thread([data, width, height]() {
                                stts::ImageFrame frame;
                                frame.data = data->tex_data;
                                frame.width = width;
                                frame.height = height;
                                frame.channels = 4;
                                frame.row_bytes = width * 4;
                                
                                stts::VisionResult res = data->engine->process(frame);
                                
                                {
                                    std::lock_guard<std::mutex> lock(data->result_mutex);
                                    data->latest_result = res;
                                }
                                data->processing = false;
                            });
                        }
                    }
                }
            }
        }
    }
    
    // 3. Draw Overlay
    {
        std::lock_guard<std::mutex> lock(data->result_mutex);
        if (data->latest_result.has_detection) {
            gs_matrix_push();
            gs_matrix_identity();
            
            gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
            vec4 color;
            vec4_set(&color, 0.0f, 1.0f, 0.0f, 1.0f);
            gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &color);
            
            gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
            
            // obs_source_t *target = obs_filter_get_target(data->context);
            // float w = (float)obs_source_get_base_width(target);
            // float h = (float)obs_source_get_base_height(target);
            
            gs_technique_begin(tech);
            gs_technique_begin_pass(tech, 0);
            
            gs_load_vertexbuffer(nullptr);
            // gs_begin(GS_POINTS);
            
            // for (const auto& pt : data->latest_result.landmarks) {
            //     gs_vertex3f(pt.first * w, pt.second * h, 0.0f);
            // }
            
            // gs_end();
            gs_technique_end_pass(tech);
            gs_technique_end(tech);
            
            gs_matrix_pop();
        }
    }
}

static void mediapipe_tick(void *data_ptr, float seconds)
{
    UNUSED_PARAMETER(seconds);
    mediapipe_filter_data *data = (mediapipe_filter_data *)data_ptr;
    
    if (data->needs_resize.load(std::memory_order_acquire)) {
        data->needs_resize.store(false, std::memory_order_release);
        
        if (data->tex_data) {
            bfree(data->tex_data);
        }
        data->tex_data = (uint8_t*)bmalloc(data->pending_width * data->pending_height * 4);
        data->tex_width = data->pending_width;
        data->tex_height = data->pending_height;
    }
}

static obs_properties_t *mediapipe_properties(void *data_ptr)
{
    UNUSED_PARAMETER(data_ptr);
    obs_properties_t *props = obs_properties_create();
    
    obs_properties_add_path(props, "model_path", "Mediapipe Model (.onnx)", 
                           OBS_PATH_FILE, "*.onnx", nullptr);
    obs_property_set_long_description(obs_properties_get(props, "model_path"), 
                                      "Path to the Mediapipe Face/Hand Mesh ONNX model.");
                           
    return props;
}

struct obs_source_info mediapipe_filter = {
    .id = "mediapipe_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = mediapipe_get_name,
    .create = mediapipe_create,
    .destroy = mediapipe_destroy,
    .get_properties = mediapipe_properties,
    .update = mediapipe_update,
    .video_tick = mediapipe_tick,
    .video_render = mediapipe_video_render
};
