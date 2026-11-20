// Actual ONNX Runtime negatives + deterministic tensor-shape contract checks.
// No simulated inference success: successful RVC audio needs real model trio.
#include "engines/rvc_model_contract.hpp"
#include "engines/rvc_onnx_engine.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    using stts::RvcModelContract;
    assert(RvcModelContract::same_shape({1,-1,768},{1,-1,768}));
    assert(RvcModelContract::same_shape({1,130,256},{1,-1,256}));
    assert(!RvcModelContract::same_shape({1,768,130},{1,-1,768}));
    assert(!RvcModelContract::same_shape({1,0,768},{1,-1,768}));
    assert(!RvcModelContract::same_shape({1,-2,768},{1,-1,768}));
    assert(!RvcModelContract::same_shape({1,-1},{1,-1,768}));

    stts::RvcOnnxEngine engine;
    const auto original=std::vector<float>(1600,0.125f);
    assert(!engine.initialize("|||2|cpu|40000"));
    assert(!engine.is_ready());
    assert(engine.process(original,48000)==original);
    assert(!engine.initialize("/dev/null|/dev/null|/dev/null|2|cpu|0"));
    assert(!engine.is_ready());
    // With a user-chosen sample rate, the actual ORT graph loader must
    // refuse corrupt model bytes instead of reporting conversion readiness.
    assert(!engine.initialize("/dev/null|/dev/null|/dev/null|2|cpu|40000"));
    assert(!engine.is_ready());
    assert(engine.process(original,48000)==original);
    if (argc == 2) {
        // A real, well-formed ONNX graph with the WRONG tensor contract.
        // No inference output is invented or accepted as converted speech.
        const std::string path = argv[1];
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "rvc_schema_negative");
        Ort::SessionOptions options;
        Ort::Session wrong(env, path.c_str(), options);
        std::string reason;
        assert(!RvcModelContract::validate(wrong,wrong,wrong,reason));
        assert(!reason.empty());
        assert(!engine.initialize(path+"|"+path+"|"+path+"|2|cpu|40000"));
        assert(!engine.is_ready());
        assert(engine.process(original,48000)==original);
        std::printf("RVC_WELLFORMED_WRONG_GRAPH_REJECTED=1 reason=%s\n",
            reason.c_str());
    }
    std::puts("RVC_CONTRACT_NEGATIVES_PASS=1");
}
