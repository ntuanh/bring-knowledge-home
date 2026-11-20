# AI Compiler — VLM & Multimodal

Learn how vision-language models execute, rebuild small parts of their compilation pipeline, and optimize them with measured results.

**Initial workload:** image + question → text answer. Start with one image and batch size 1, then extend to multiple images and video. Multimodal means combining input modalities; multi-model experiments additionally cover running different models or pipeline components together.

This branch currently contains a learning plan and reference index. The VLM implementations and benchmarks below are planned work.

## 1. Understand the execution pipeline

A representative projector-based VLM follows this path; other architectures may use resamplers or cross-attention.

```text
Image → preprocessing → vision encoder → projector / token merger ─┐
                                                                 ├→ LLM prefill → KV cache → decode → text
Prompt → chat template → tokenizer → text embeddings ─────────────┘
```

Use the model's [processor and chat template](https://huggingface.co/docs/transformers/chat_templating_multimodal) as the reference for image preparation and text/image alignment.

| Stage | What to inspect | Compiler / runtime questions |
| --- | --- | --- |
| Preprocessing | Resize, normalization, crops/patches, image order. | Which work stays on CPU? How much data crosses to the accelerator? |
| Vision encoder | Patch embeddings, attention, tensor shapes and layouts. | Which shapes can be specialized? Where are fusion and tiling useful? |
| Connector | Projected visual embeddings and visual token count. | Do embedding dimensions and image placeholders match? Are copies avoidable? |
| Prefill | Combined context, attention masks, position IDs, initial KV state. | How do image resolution and prompt length affect memory and latency? |
| Decode | One generation step, growing KV state, sampling. | How are cache updates represented? Which graph variants are reused? |

Keep a typed boundary between components: **shape, dtype, layout, device, and cache state**. Compile supported tensor regions separately and record graph breaks and fallbacks.

## 2. Reference models

| Model | Learning purpose | Documentation / source |
| --- | --- | --- |
| SmolVLM / SmolVLM2 | Start with a small checkpoint for pipeline inspection; choose size after checking available memory. | [Docs](https://huggingface.co/docs/transformers/model_doc/smolvlm) · [Implementation](https://github.com/huggingface/transformers/tree/main/src/transformers/models/smolvlm) |
| LLaVA | Trace a vision tower, multimodal projector, and language model connection. | [Project and source](https://github.com/haotian-liu/LLaVA) |
| Qwen2.5-VL | Study variable image resolution, visual token counts, video inputs, and multimodal position handling. | [Docs](https://huggingface.co/docs/transformers/model_doc/qwen2_5_vl) · [Implementation](https://github.com/huggingface/transformers/tree/main/src/transformers/models/qwen2_5_vl) |

These are reference architectures, not a claim that every backend supports every checkpoint. Pin model and processor revisions together.

## 3. Build and optimize incrementally

1. **Establish a reference.** Run one model through Transformers in evaluation mode with fixed inputs and deterministic decoding. Save processor outputs, tensor shapes, intermediate embeddings, and logits.
2. **Rebuild a small graph.** Extend [MiniTensor](https://github.com/ntuanh/bring-knowledge-home/tree/compiler/compiler_visualizer/part2-ai-compiler) from `matmul/add/relu` toward normalization, softmax, reshape/transpose, and a small attention block. Validate each operation before assembling a toy VLM pipeline.
3. **Compile a real component.** Start with the vision encoder or projector using PyTorch compilation. Inspect captured graphs, generated kernels, unsupported operators, and recompilation. Treat prefill and stateful decode as separate experiments.
4. **Optimize one change at a time.** Try legal fusion patterns, layout changes, tiling, reduced precision, and quantization. Sweep image resolution and context length to expose dynamic-shape behavior. Recheck numerical error and task quality after each change.
5. **Add multimodal and multi-model workloads.** Test multiple images, then video frames. Compare per-component batching and device placement; measure memory contention, transfer costs, and scheduling when components or model variants run concurrently.

Cache image features only when image content, preprocessing, encoder/connector revisions, and precision are compatible. Reuse KV state only for matching context and position semantics; do not share either cache across models without an explicit compatibility check.

## 4. Measure correctness and performance

| Area | Record |
| --- | --- |
| Correctness | Intermediate tensor/logit errors with explicit tolerances; answer quality on a fixed VQA or OCR question set. |
| Latency | Preprocessing, encoding, connector, prefill, and per-token decode time; end-to-end time to first token (TTFT), including any queueing and transfers. |
| Throughput | Generated tokens/s and requests/s at a stated batch size and concurrency. |
| Resources | Peak device memory, KV cache size, visual token count, kernel launches, and host/device copies. |
| Compilation | Capture/build time, cold-start latency, graph breaks, and recompilations across input shapes. |

Report cold and warmed-up runs separately. Keep image resolution/count, prompt length, output limit, dtype, and hardware fixed for before/after comparisons. Synchronize accelerator timing and record device, model revision, dependency versions, and benchmark settings.

## 5. Compiler and serving references

| Layer / project | What to read or rebuild | Documentation / source |
| --- | --- | --- |
| PyTorch / TorchInductor | Graph capture, graph breaks, dynamic shapes, and generated kernels. | [Compiler docs](https://docs.pytorch.org/docs/stable/torch.compiler.html) · [Source](https://github.com/pytorch/pytorch/tree/main/torch/_inductor) |
| MLIR Toy | A teaching compiler: AST → dialect IR → rewrites → LLVM lowering. | [Tutorial](https://mlir.llvm.org/docs/Tutorials/Toy/) · [Source](https://github.com/llvm/llvm-project/tree/main/mlir/examples/toy) |
| Apache TVM | Tensor IR, graph transformations, fusion, scheduling, and code generation. | [Docs](https://tvm.apache.org/docs/) · [Source](https://github.com/apache/tvm) |
| Triton language/compiler | GPU kernel experiments: matmul, fused softmax, and attention. | [Tutorials](https://triton-lang.org/main/getting-started/tutorials/index.html) · [Source](https://github.com/triton-lang/triton) |
| TensorRT-LLM | NVIDIA inference optimization and multimodal execution examples. | [Docs](https://nvidia.github.io/TensorRT-LLM/) · [Multimodal source](https://github.com/NVIDIA/TensorRT-LLM/tree/main/examples/models/core/multimodal) |
| vLLM | Serving reference for multimodal input handling, caching, and request execution. | [Multimodal docs](https://docs.vllm.ai/en/latest/features/multimodal_inputs/) · [Source](https://github.com/vllm-project/vllm/tree/main/vllm/multimodal) |

**Suggested first experiment:** profile a small SmolVLM checkpoint on one image, compile only its vision encoder, and compare outputs, warm latency, peak memory, and compilation cost against eager execution.
