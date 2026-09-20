#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <sstream>
#include <chrono>

#include "llama.h"

// --- Global Thread State ---
std::mutex g_queue_mutex;
std::queue<std::string> g_token_queue;

std::mutex g_prompt_mutex;
std::string g_pending_prompt = "";
std::atomic<bool> g_has_prompt{false};
std::atomic<bool> g_is_generating{false};
std::atomic<bool> g_stop_requested{false};
std::atomic<bool> g_model_ready{false};
std::atomic<bool> g_app_running{true};
std::atomic<float> g_last_tps{0.0f};

// --- LLM Background Worker ---
void llm_worker_func() {
    llama_log_set([](enum ggml_log_level, const char*, void*) {}, nullptr);
    llama_backend_init();

    const char* model_path = "model/tinyllama-1.1b-chat-v1.0.Q6_K.gguf";
    llama_model_params mparams = llama_model_default_params();
    llama_model* model = llama_model_load_from_file(model_path, mparams);

    if (!model) {
        std::cerr << "[ERROR] Could not load model." << std::endl;
        return;
    }

    const struct llama_vocab* vocab = llama_model_get_vocab(model);

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 1024;
    cparams.n_threads = 4;
    cparams.n_threads_batch = 4;

    llama_context* ctx = llama_init_from_model(model, cparams);
    if (!ctx) {
        llama_model_free(model);
        return;
    }

    g_model_ready = true;

    while (g_app_running) {
        if (!g_has_prompt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        std::string current_prompt;
        {
            std::lock_guard<std::mutex> lock(g_prompt_mutex);
            current_prompt = "<|system|>\nYou are AOS, a helpful AI assistant. Answer clearly.</s>\n<|user|>\n"
                           + g_pending_prompt + "</s>\n<|assistant|>\n";
            g_has_prompt = false;
        }

        g_is_generating = true;
        g_stop_requested = false;

        llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.7f));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(1337));

        // Tokenize
        std::vector<llama_token> tokens(current_prompt.length() + 32);
        int n_tokens = llama_tokenize(vocab, current_prompt.c_str(), current_prompt.length(), tokens.data(), tokens.size(), true, true);
        if (n_tokens < 0) {
            tokens.resize(-n_tokens);
            n_tokens = llama_tokenize(vocab, current_prompt.c_str(), current_prompt.length(), tokens.data(), tokens.size(), true, true);
        }
        tokens.resize(n_tokens);

        // Ingest Prompt
        llama_batch batch = llama_batch_get_one(tokens.data(), tokens.size());
        if (llama_decode(ctx, batch) == 0) {
            int gen_count = 0;
            auto start_time = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < 512 && g_app_running; ++i) {
                // Check STOP signal
                if (g_stop_requested) {
                    std::lock_guard<std::mutex> lock(g_queue_mutex);
                    g_token_queue.push(" [Stopped]");
                    break;
                }

                llama_token new_token = llama_sampler_sample(smpl, ctx, -1);
                if (llama_vocab_is_eog(vocab, new_token)) {
                    break;
                }

                char piece_buf[64];
                int n_chars = llama_token_to_piece(vocab, new_token, piece_buf, sizeof(piece_buf), 0, true);
                if (n_chars > 0) {
                    std::lock_guard<std::mutex> lock(g_queue_mutex);
                    g_token_queue.push(std::string(piece_buf, n_chars));
                }

                gen_count++;

                llama_batch next_batch = llama_batch_get_one(&new_token, 1);
                if (llama_decode(ctx, next_batch) != 0) {
                    break;
                }
            }

            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end_time - start_time;
            if (elapsed.count() > 0) {
                g_last_tps = gen_count / elapsed.count();
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            g_token_queue.push("\n\n");
        }

        llama_sampler_free(smpl);
        g_is_generating = false;
    }

    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
}

// --- Styled Text Structure ---
struct RenderLine {
    std::string text;
    SDL_Color color;
    bool is_code;
};

// --- Parse Markdown & Roles ---
std::vector<RenderLine> parse_markdown(const std::string& raw) {
    std::vector<RenderLine> result;
    std::istringstream stream(raw);
    std::string line;
    bool in_code_block = false;

    while (std::getline(stream, line)) {
        // Code Block delimiter
        if (line.rfind("```", 0) == 0) {
            in_code_block = !in_code_block;
            continue;
        }

        // Word wrap long lines at 72 chars
        std::vector<std::string> sublines;
        while (line.length() > 72) {
            sublines.push_back(line.substr(0, 72));
            line = line.substr(72);
        }
        sublines.push_back(line);

        for (auto& sl : sublines) {
            RenderLine rl;
            rl.is_code = in_code_block;

            if (in_code_block) {
                rl.text = "  " + sl;
                rl.color = { 0, 220, 255, 255 }; // Cyan Code
            } else if (sl.rfind("User:", 0) == 0) {
                rl.text = sl;
                rl.color = { 80, 200, 255, 255 }; // Bright Blue/Cyan User
            } else if (sl.rfind("Bot:", 0) == 0) {
                rl.text = sl;
                rl.color = { 0, 255, 128, 255 }; // Green Bot
            } else if (sl.rfind("#", 0) == 0) {
                rl.text = sl;
                rl.color = { 255, 215, 0, 255 }; // Gold Headers
            } else if (sl.rfind("- ", 0) == 0 || sl.rfind("* ", 0) == 0) {
                rl.text = "  * " + sl.substr(2);
                rl.color = { 240, 240, 240, 255 }; // Bullet points
            } else {
                rl.text = sl;
                rl.color = { 200, 205, 210, 255 }; // Standard body text
            }
            result.push_back(rl);
        }
    }
    return result;
}

// --- Main App ---
int main(int argc, char* argv[]) {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return 1;
    }

    int win_w = 800;
    int win_h = 600;
    SDL_Window* window = SDL_CreateWindow("AOS // AI Terminal", win_w, win_h, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "software");

    SDL_StartTextInput(window);
    std::thread worker(llm_worker_func);

    std::string raw_conversation = "AOS CORE v0.2 READY.\n\n";
    std::string current_input = "";
    int scroll_offset = 0;
    bool running = true;
    SDL_Event event;

    // Button Coordinates
    SDL_FRect stop_btn_rect = { 690.0f, 525.0f, 85.0f, 32.0f };
    SDL_FRect send_btn_rect = { 595.0f, 525.0f, 85.0f, 32.0f };

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_TEXT_INPUT) {
                if (!g_is_generating) {
                    current_input += event.text.text;
                }
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                SDL_Keycode key = event.key.key;
                if (key == SDLK_ESCAPE) {
                    running = false;
                } else if (key == SDLK_BACKSPACE && !current_input.empty()) {
                    current_input.pop_back();
                } else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && !current_input.empty()) {
                    if (g_model_ready && !g_is_generating) {
                        raw_conversation += "User: " + current_input + "\n\nBot: ";
                        {
                            std::lock_guard<std::mutex> lock(g_prompt_mutex);
                            g_pending_prompt = current_input;
                            g_has_prompt = true;
                        }
                        current_input.clear();
                    }
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                float mx = event.button.x;
                float my = event.button.y;

                // Check [STOP] Click / Touch
                if (mx >= stop_btn_rect.x && mx <= stop_btn_rect.x + stop_btn_rect.w &&
                    my >= stop_btn_rect.y && my <= stop_btn_rect.y + stop_btn_rect.h) {
                    if (g_is_generating) {
                        g_stop_requested = true;
                    }
                }

                // Check [SEND] Click / Touch
                if (mx >= send_btn_rect.x && mx <= send_btn_rect.x + send_btn_rect.w &&
                    my >= send_btn_rect.y && my <= send_btn_rect.y + send_btn_rect.h) {
                    if (g_model_ready && !g_is_generating && !current_input.empty()) {
                        raw_conversation += "User: " + current_input + "\n\nBot: ";
                        {
                            std::lock_guard<std::mutex> lock(g_prompt_mutex);
                            g_pending_prompt = current_input;
                            g_has_prompt = true;
                        }
                        current_input.clear();
                    }
                }
            }
        }

        // Consume tokens from LLM thread
        {
            std::lock_guard<std::mutex> lock(g_queue_mutex);
            while (!g_token_queue.empty()) {
                raw_conversation += g_token_queue.front();
                g_token_queue.pop();
            }
        }

        // --- RENDER ---
        SDL_SetRenderDrawColor(renderer, 15, 17, 20, 255);
        SDL_RenderClear(renderer);

        // 1. Top Header Bar
        SDL_SetRenderDrawColor(renderer, 0, 255, 128, 255);
        std::string status_bar = "[AOS // AI Core] | Status: ";
        if (!g_model_ready) status_bar += "LOADING...";
        else if (g_is_generating) status_bar += "GENERATING...";
        else status_bar += "READY";

        if (g_last_tps > 0.0f) {
            char buf[32];
            snprintf(buf, sizeof(buf), " (%.2f T/s)", g_last_tps.load());
            status_bar += buf;
        }
        SDL_RenderDebugText(renderer, 20.0f, 12.0f, status_bar.c_str());

        SDL_SetRenderDrawColor(renderer, 35, 42, 50, 255);
        SDL_RenderLine(renderer, 20.0f, 28.0f, 780.0f, 28.0f);

        // 2. Render Formatted Markdown Chat
        std::vector<RenderLine> lines = parse_markdown(raw_conversation);
        int max_visible_lines = 28;
        int start_idx = 0;
        if ((int)lines.size() > max_visible_lines) {
            start_idx = lines.size() - max_visible_lines;
        }

        float text_y = 38.0f;
        for (int i = start_idx; i < (int)lines.size(); ++i) {
            const auto& l = lines[i];

            // Render dark background for code blocks
            if (l.is_code) {
                SDL_SetRenderDrawColor(renderer, 25, 30, 38, 255);
                SDL_FRect code_bg = { 15.0f, text_y - 2.0f, 770.0f, 16.0f };
                SDL_RenderFillRect(renderer, &code_bg);
            }

            SDL_SetRenderDrawColor(renderer, l.color.r, l.color.g, l.color.b, l.color.a);
            SDL_RenderDebugText(renderer, 20.0f, text_y, l.text.c_str());
            text_y += 16.0f;
        }

        // 3. Input Divider
        SDL_SetRenderDrawColor(renderer, 35, 42, 50, 255);
        SDL_RenderLine(renderer, 20.0f, 510.0f, 780.0f, 510.0f);

        // 4. Input Field Box
        SDL_SetRenderDrawColor(renderer, 22, 26, 32, 255);
        SDL_FRect input_box = { 20.0f, 525.0f, 565.0f, 32.0f };
        SDL_RenderFillRect(renderer, &input_box);

        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        std::string prompt_text = "> " + current_input;
        if (!g_is_generating && (SDL_GetTicks() / 500) % 2 == 0) {
            prompt_text += "_";
        }
        SDL_RenderDebugText(renderer, 28.0f, 533.0f, prompt_text.c_str());

        // 5. [SEND] Button
        SDL_SetRenderDrawColor(renderer, 30, 140, 70, 255);
        SDL_RenderFillRect(renderer, &send_btn_rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDebugText(renderer, 620.0f, 533.0f, "SEND");

        // 6. [STOP] Button
        if (g_is_generating) {
            SDL_SetRenderDrawColor(renderer, 200, 40, 40, 255); // Bright Red Active
        } else {
            SDL_SetRenderDrawColor(renderer, 70, 30, 30, 255); // Dim Red Inactive
        }
        SDL_RenderFillRect(renderer, &stop_btn_rect);
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderDebugText(renderer, 715.0f, 533.0f, "STOP");

        // Bottom Hint
        SDL_SetRenderDrawColor(renderer, 100, 110, 120, 255);
        SDL_RenderDebugText(renderer, 20.0f, 570.0f, "Tap [STOP] to abort | Tap [SEND] or press Enter to prompt");

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    g_app_running = false;
    g_stop_requested = true;
    if (worker.joinable()) {
        worker.join();
    }

    SDL_StopTextInput(window);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
