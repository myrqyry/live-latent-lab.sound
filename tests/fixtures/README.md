# Negative ONNX contract fixture

`onnxruntime_non_rvc_sigmoid.onnx` is a 103-byte valid ONNX Sigmoid graph
(originally `onnxruntime/datasets/sigmoid.onnx` from the Microsoft ONNX
Runtime Python distribution, MIT-licensed).

SHA-256: `5340aba67a7e3475162ad794378af55f1718f55f9a5d74b4af60ecc7f7a624b6`

It is intentionally **not an RVC model**. The RVC safety suite loads
it using real ONNX Runtime and verifies that shape/name/type mismatch
prevents voice conversion from being advertised as ready.

No generated speech, synthetic inference success, or mock model is used.
The ONNX Runtime project and its MIT license are available at:
https://github.com/microsoft/onnxruntime
