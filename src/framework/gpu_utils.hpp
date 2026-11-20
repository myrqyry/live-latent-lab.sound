#pragma once
/**
 * @file gpu_utils.hpp
 * @brief GPU acceleration utilities for ONNX Runtime
 * 
 * Provides helpers to configure GPU execution providers:
 * - CUDA (NVIDIA GPUs)
 * - DirectML (Windows - AMD/Intel/NVIDIA)
 * - TensorRT (NVIDIA - optimized)
 * - CoreML (macOS/iOS)
 * 
 * Usage:
 *   Ort::SessionOptions session_options;
 *   stts::gpu::configure_gpu_session(session_options, "cuda", 0);
 */

#include <onnxruntime_cxx_api.h>
#include <string>
#include <iostream>

namespace stts {
namespace gpu {

/**
 * GPU Backend types
 */
enum class Backend {
    CPU,        // Pure CPU inference
    CUDA,       // NVIDIA CUDA
    TENSORRT,   // NVIDIA TensorRT (optimized CUDA)
    DIRECTML,   // Windows DirectML (works with any GPU)
    COREML,     // Apple CoreML
    AUTO        // Auto-detect best available
};

/**
 * GPU Configuration
 */
struct GPUConfig {
    Backend backend = Backend::AUTO;
    int device_id = 0;
    size_t gpu_mem_limit = 0;  // 0 = no limit
    bool enable_cuda_graph = true;
    bool enable_memory_arena = true;
};

/**
 * Check if CUDA is available at runtime
 */
inline bool is_cuda_available() {
#ifdef USE_CUDA
    try {
        // Try to get CUDA device count
        std::vector<std::string> providers = Ort::GetAvailableProviders();
        for (const auto& p : providers) {
            if (p == "CUDAExecutionProvider") {
                return true;
            }
        }
    } catch (...) {
        // CUDA not available
    }
#endif
    return false;
}

/**
 * Check if DirectML is available
 */
inline bool is_directml_available() {
#ifdef USE_DIRECTML
    try {
        std::vector<std::string> providers = Ort::GetAvailableProviders();
        for (const auto& p : providers) {
            if (p == "DmlExecutionProvider") {
                return true;
            }
        }
    } catch (...) {
    }
#endif
#ifdef _WIN32
    // DirectML should be available on Windows 10+
    return true;
#endif
    return false;
}

/**
 * Get available execution providers
 */
inline std::vector<std::string> get_available_providers() {
    return Ort::GetAvailableProviders();
}

/**
 * Print available providers for debugging
 */
inline void log_available_providers() {
    std::cout << "[GPU] Available ONNX Runtime providers: ";
    auto providers = get_available_providers();
    for (size_t i = 0; i < providers.size(); i++) {
        std::cout << providers[i];
        if (i < providers.size() - 1) std::cout << ", ";
    }
    std::cout << std::endl;
}

/**
 * Configure session options for GPU acceleration
 * 
 * @param session_options The session options to configure
 * @param config GPU configuration
 * @return true if GPU was successfully configured
 */
inline bool configure_gpu_session(Ort::SessionOptions& session_options, const GPUConfig& config) {
    bool gpu_enabled = false;
    
    Backend backend = config.backend;
    
    // Auto-detect best backend
    if (backend == Backend::AUTO) {
        if (is_cuda_available()) {
            backend = Backend::CUDA;
        } else if (is_directml_available()) {
            backend = Backend::DIRECTML;
        } else {
            backend = Backend::CPU;
        }
    }
    
    try {
        switch (backend) {
            case Backend::CUDA: {
#ifdef USE_CUDA
                OrtCUDAProviderOptions cuda_options;
                cuda_options.device_id = config.device_id;
                cuda_options.cudnn_conv_algo_search = OrtCudnnConvAlgoSearchExhaustive;
                cuda_options.gpu_mem_limit = config.gpu_mem_limit;
                cuda_options.arena_extend_strategy = 0; // kNextPowerOfTwo
                cuda_options.do_copy_in_default_stream = 1;
                
                session_options.AppendExecutionProvider_CUDA(cuda_options);
                std::cout << "[GPU] CUDA enabled on device " << config.device_id << std::endl;
                gpu_enabled = true;
#else
                std::cerr << "[GPU] CUDA requested but not compiled with CUDA support" << std::endl;
#endif
                break;
            }
            
            case Backend::TENSORRT: {
#ifdef USE_CUDA
                OrtTensorRTProviderOptions trt_options;
                trt_options.device_id = config.device_id;
                trt_options.trt_max_workspace_size = 1ULL << 30; // 1GB
                trt_options.trt_fp16_enable = 1;
                
                session_options.AppendExecutionProvider_TensorRT(trt_options);
                std::cout << "[GPU] TensorRT enabled on device " << config.device_id << std::endl;
                gpu_enabled = true;
#else
                std::cerr << "[GPU] TensorRT requested but not compiled with CUDA support" << std::endl;
#endif
                break;
            }
            
            case Backend::DIRECTML: {
#ifdef USE_DIRECTML
                session_options.AppendExecutionProvider("DML");
                std::cout << "[GPU] DirectML enabled" << std::endl;
                gpu_enabled = true;
#elif defined(_WIN32)
                // Try to append DirectML even without compile flag
                try {
                    session_options.AppendExecutionProvider("DML");
                    std::cout << "[GPU] DirectML enabled" << std::endl;
                    gpu_enabled = true;
                } catch (...) {
                    std::cerr << "[GPU] DirectML not available" << std::endl;
                }
#else
                std::cerr << "[GPU] DirectML only available on Windows" << std::endl;
#endif
                break;
            }
            
            case Backend::COREML: {
#ifdef __APPLE__
                session_options.AppendExecutionProvider("CoreML");
                std::cout << "[GPU] CoreML enabled" << std::endl;
                gpu_enabled = true;
#else
                std::cerr << "[GPU] CoreML only available on macOS/iOS" << std::endl;
#endif
                break;
            }
            
            case Backend::CPU:
            default:
                // CPU is always the fallback
                break;
        }
    } catch (const std::exception& e) {
        std::cerr << "[GPU] Failed to configure GPU: " << e.what() << std::endl;
        std::cerr << "[GPU] Falling back to CPU" << std::endl;
        gpu_enabled = false;
    }
    
    // Consolidate optimizations
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    
    if (config.enable_memory_arena) {
        // Enable memory arena for better performance
        session_options.EnableMemPattern();
    }
    
    return gpu_enabled;
}

/**
 * Simple helper to configure with defaults
 */
inline bool configure_gpu_auto(Ort::SessionOptions& session_options, int num_threads = 4) {
    GPUConfig config;
    config.backend = Backend::AUTO;
    
    // Set CPU thread count as fallback
    session_options.SetIntraOpNumThreads(num_threads);
    
    return configure_gpu_session(session_options, config);
}

/**
 * Parse backend string to enum
 */
inline Backend parse_backend(const std::string& str) {
    if (str == "cuda" || str == "CUDA") return Backend::CUDA;
    if (str == "tensorrt" || str == "TensorRT") return Backend::TENSORRT;
    if (str == "directml" || str == "DirectML" || str == "dml") return Backend::DIRECTML;
    if (str == "coreml" || str == "CoreML") return Backend::COREML;
    if (str == "cpu" || str == "CPU") return Backend::CPU;
    return Backend::AUTO;
}

/**
 * Get backend name string
 */
inline std::string backend_name(Backend backend) {
    switch (backend) {
        case Backend::CUDA: return "CUDA";
        case Backend::TENSORRT: return "TensorRT";
        case Backend::DIRECTML: return "DirectML";
        case Backend::COREML: return "CoreML";
        case Backend::CPU: return "CPU";
        case Backend::AUTO: return "Auto";
        default: return "Unknown";
    }
}

} // namespace gpu
} // namespace stts
