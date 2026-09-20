#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include "llama.h"

int main(int argc, char** argv) {
    const char* model_path = "model/tinyllama-1.1b-chat-v1.0.Q6_K.gguf";
    std::string prompt = "<|system|>\nYou are a concise AI.</s>\n<|user|>\nWhat is 2 + 2?</s>\n<|assistant|>\n";

    if (argc > 1) {
        prompt = argv[1];
    }

    std::cout << "[INFO] Loading model and initializing runtime..." << std::endl;
    llama_backend_init();

    // 1. Load Model
    llama_model_params model_params = llama_model_default_params();
    llama_model* model = llama_model_load_from_file(model_path, model_params);
    if (!model) {
        std::cerr << "[ERROR] Could not load model." << std::endl;
        llama_backend_free();
        return 1;
    }

    const struct llama_vocab* vocab = llama_model_get_vocab(model);

    // 2. Initialize Context
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = 1024;
    // On 8-core phones, 4 threads usually matches the big performance cores
    ctx_params.n_threads = 4;
    ctx_params.n_threads_batch = 4;

    llama_context* ctx = llama_init_from_model(model, ctx_params);
    if (!ctx) {
        std::cerr << "[ERROR] Could not create context." << std::endl;
        llama_model_free(model);
        llama_backend_free();
        return 1;
    }

    // 3. Initialize Sampler Chain (Temperature + Top-K / Dist)
    llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.7f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(1337));

    // 4. Tokenize Prompt
    std::vector<llama_token> tokens(prompt.length() + 32);
    int n_tokens = llama_tokenize(vocab, prompt.c_str(), prompt.length(), tokens.data(), tokens.size(), true, true);
    if (n_tokens < 0) {
        tokens.resize(-n_tokens);
        n_tokens = llama_tokenize(vocab, prompt.c_str(), prompt.length(), tokens.data(), tokens.size(), true, true);
    }
    tokens.resize(n_tokens);

    std::cout << "[INFO] Prompt tokens: " << n_tokens << std::endl;
    std::cout << "\n--- GENERATION START ---\n" << std::flush;

    // 5. Ingest Prompt (Prefill)
    llama_batch batch = llama_batch_get_one(tokens.data(), tokens.size());
    if (llama_decode(ctx, batch) != 0) {
        std::cerr << "[ERROR] Prompt evaluation failed." << std::endl;
        return 1;
    }

    // 6. Generation Loop
    const int max_new_tokens = 128;
    int generated_count = 0;
    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < max_new_tokens; ++i) {
        // Sample next token
        llama_token new_token = llama_sampler_sample(smpl, ctx, -1);

        // Check if EOS/EOG token
        if (llama_vocab_is_eog(vocab, new_token)) {
            break;
        }

        // Convert token to text piece
        char piece_buf[64];
        int n_chars = llama_token_to_piece(vocab, new_token, piece_buf, sizeof(piece_buf), 0, true);
        if (n_chars > 0) {
            std::cout.write(piece_buf, n_chars);
            std::cout << std::flush;
        }

        generated_count++;

        // Feed generated token back into context
        llama_batch next_batch = llama_batch_get_one(&new_token, 1);
        if (llama_decode(ctx, next_batch) != 0) {
            std::cerr << "\n[ERROR] Generation decode failed." << std::endl;
            break;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;

    std::cout << "\n--- GENERATION END ---\n" << std::endl;
    if (elapsed.count() > 0) {
        std::cout << "[STATS] Generated " << generated_count << " tokens in "
                  << elapsed.count() << "s ("
                  << (generated_count / elapsed.count()) << " T/s)" << std::endl;
    }

    // 7. Cleanup
    llama_sampler_free(smpl);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();

    return 0;
}
