# AetherOS by @dev-aemni (https://github.com/dev-aemni/AetherOS)

# AetherOS // Technical Architecture

## 1. System Design
AetherOS is decoupled into two independent execution layers:

```text
+-------------------------------------------------------------+
|                     PRESENTATION LAYER                      |
|   Dear ImGui (SDL3 Renderer)   OR   HTML5/CSS/JS (SSE Web)  |
+-------------------------------------------------------------+
                              ▲
                              │ Thread-Safe Token Queue (FIFO)
                              ▼
+-------------------------------------------------------------+
|                     INFERENCE LAYER                         |
|   Dedicated Worker Thread  <--->  libllama.so / llama.dll   |
+-------------------------------------------------------------+
                              │ mmap (Zero-Copy)
                              ▼
+-------------------------------------------------------------+
|                      GGUF MODEL BINARY                      |
+-------------------------------------------------------------+
cat << 'EOF' > ~/llama/docs/MODEL_GUIDE.md
# AetherOS // Model Guide

All models must be in **GGUF** format and placed inside the `model/` folder.

---

## 1. Understanding Quantization Formats
- **Q4_K_M**: Standard 4-bit balance. Medium memory footprint, good speed, strong retention.
- **Q5_K_M**: 5-bit high fidelity. Best balance for small models (1B–3B).
- **Q6_K**: Near-uncompressed accuracy. Excellent if RAM allows.

---

## 2. Hardware Tiers

### Tier 1: Low-RAM Mobile Devices (3 GB – 4 GB RAM)
*Recommended for phones like the Samsung Galaxy A50.*

| Model | Quantization | File Size | Peak RAM | Est. Speed |
|---|---|---|---|---|
| **Qwen 2.5 1.5B Instruct** | Q5_K_M | ~1.2 GB | ~1.5 GB | 5.5 T/s |
| **Llama 3.2 1B Instruct** | Q6_K | ~950 MB | ~1.2 GB | 6.5 T/s |
| **SmolLM2 1.7B Instruct** | Q4_K_M | ~1.0 GB | ~1.4 GB | 5.0 T/s |

### Tier 2: Mid-Tier Laptops & High-RAM Phones (8 GB – 16 GB RAM)
*Recommended for Windows PCs or modern 8GB+ smartphones.*

| Model | Quantization | File Size | Peak RAM | Est. Speed |
|---|---|---|---|---|
| **Llama 3.2 3B Instruct** | Q4_K_M | ~2.0 GB | ~2.6 GB | 15–30 T/s (PC) |
| **Qwen 2.5 7B Instruct** | Q4_K_M | ~4.4 GB | ~5.2 GB | 8–18 T/s (PC) |
| **Mistral 7B Instruct v0.3** | Q4_K_M | ~4.3 GB | ~5.1 GB | 8–18 T/s (PC) |

---

## 3. Where to Download Models
Visit [HuggingFace](https://huggingface.co/models?search=gguf) and search for GGUF releases from trusted community quantizers like `bartowski` or `Qwen`. Download `.gguf` files straight into your `model/` folder.
