#pragma once
// RVC ONNX schema guard: fail closed until all THREE model IO contracts match
// the subset our native inference pipeline can actually supply. This is NOT
// proof of correct generated audio, latency or sample-rate synchronization.
#include <onnxruntime_cxx_api.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>

namespace stts {
struct RvcModelContract {
    struct TensorSpec {
        const char *name;
        ONNXTensorElementDataType type;
        std::vector<int64_t> shape; // -1 means arbitrary positive dimension
    };

    static bool same_shape(const std::vector<int64_t>& actual,
                           const std::vector<int64_t>& expected) {
        if (actual.size() != expected.size()) return false;
        for (size_t i = 0; i < actual.size(); ++i) {
            if (actual[i] == 0 || actual[i] < -1) return false;
            if (expected[i] > 0 && actual[i] > 0 && actual[i] != expected[i])
                return false;
        }
        return true;
    }

    static bool check_session(Ort::Session& session,
                              const std::vector<TensorSpec>& inputs,
                              const std::vector<TensorSpec>& outputs,
                              std::string& reason, const char *label) {
        Ort::AllocatorWithDefaultOptions alloc;
        try {
            if (session.GetInputCount() != inputs.size() ||
                session.GetOutputCount() != outputs.size()) {
                reason = std::string(label) + ": input/output count unsupported";
                return false;
            }
            for (int direction = 0; direction < 2; ++direction) {
                const auto& expected = direction == 0 ? inputs : outputs;
                for (size_t i = 0; i < expected.size(); ++i) {
                    auto name = direction == 0
                        ? session.GetInputNameAllocated(i, alloc)
                        : session.GetOutputNameAllocated(i, alloc);
                    auto info = direction == 0
                        ? session.GetInputTypeInfo(i)
                        : session.GetOutputTypeInfo(i);
                    if (info.GetONNXType() != ONNX_TYPE_TENSOR) {
                        reason = std::string(label) + ": non-tensor IO";
                        return false;
                    }
                    const auto type_and_shape = info.GetTensorTypeAndShapeInfo();
                    if (std::string(name.get()) != expected[i].name ||
                        type_and_shape.GetElementType() != expected[i].type ||
                        !same_shape(type_and_shape.GetShape(), expected[i].shape)) {
                        reason = std::string(label) + ": unsupported IO schema at " +
                            (direction == 0 ? "input " : "output ") + expected[i].name;
                        return false;
                    }
                }
            }
            return true;
        } catch (const std::exception& e) {
            reason = std::string(label) + ": metadata error: " + e.what();
            return false;
        }
    }

    static bool validate(Ort::Session& voice, Ort::Session& pitch,
                         Ort::Session& content, std::string& reason) {
        // RVC export.py: phone=[1,T,C], pitch=int64 coarse, pitchf=float Hz.
        // C is v1 256 or v2 768, checked against content output below.
        Ort::AllocatorWithDefaultOptions alloc;
        int64_t channels = 0;
        try {
            // TensorTypeAndShapeInfo references its owning TypeInfo. Keep
            // the TypeInfo alive until its shape has been fully read.
            auto phone_type = voice.GetInputTypeInfo(0);
            auto phone_info = phone_type.GetTensorTypeAndShapeInfo();
            auto phone_shape = phone_info.GetShape();
            if (phone_shape.size() == 3 && phone_shape[2] > 0)
                channels = phone_shape[2];
            else if (content.GetOutputCount() == 1) {
                auto embed_type = content.GetOutputTypeInfo(0);
                auto embed_info = embed_type.GetTensorTypeAndShapeInfo();
                auto embed_shape = embed_info.GetShape();
                if (embed_shape.size() == 3 && embed_shape[2] > 0)
                    channels = embed_shape[2];
            }
        } catch (const std::exception& e) {
            reason = std::string("RVC embedding metadata: ") + e.what();
            return false;
        }
        if (channels != 256 && channels != 768) {
            reason = "RVC embedding channel dimension must be statically 256 or 768";
            return false;
        }
        return check_session(voice, {
            {"phone", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1,-1,channels}},
            {"phone_lengths", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, {1}},
            {"pitch", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, {1,-1}},
            {"pitchf", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1,-1}},
            {"ds", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, {1}},
            {"rnd", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1,192,-1}},
        }, {{"audio", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1,1,-1}}},
        reason,"Voice") &&
        check_session(pitch, {
            {"audio", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1,-1}},
        }, {{"pitchf", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1,-1}}},
        reason,"RMVPE") &&
        check_session(content, {
            {"source", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1,-1}},
            {"padding_mask", ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL, {1,-1}},
        }, {{"embed", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1,-1,channels}}},
        reason,"HuBERT");
    }
};
}
