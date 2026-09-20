#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include <iostream>
#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>

#include "llama.h"

namespace fs = std::filesystem;

struct Message {
    std::string role;
    std::string text;
};

std::mutex g_chat_mutex;
std::vector<Message> g_messages;
std::queue<std::string> g_token_queue;

std::mutex g_worker_mutex;
std::string g_pending_prompt = "";
// Direct, anti-email system prompt
std::string g_system_prompt = "You are AetherOS. Chat casually, directly, and concisely. Never write letters or use bracket placeholders like [Name].";
std::string g_selected_model = "";
std::vector<std::string> g_available_models;
float g_temperature = 0.7f;

std::atomic<bool> g_has_prompt{false};
std::atomic<bool> g_is_generating{false};
std::atomic<bool> g_stop_requested{false};
std::atomic<bool> g_model_ready{false};
std::atomic<bool> g_reload_model{false};
std::atomic<bool> g_app_running{true};
std::atomic<float> g_last_tps{0.0f};

// --- .AOS (Chat) & .AOSB (Bot Preset) File Handlers ---
void save_chat_aos(const std::string& path = "session.aos") {
    std::lock_guard<std::mutex> lock(g_chat_mutex);
    std::ofstream f(path);
    if (!f.is_open()) return;
    f << "AOS_CHAT_V1\n";
    for (const auto& m : g_messages) {
        f << m.role << ":::" << m.text << "\n---MSG_END---\n";
    }
}

void load_chat_aos(const std::string& path = "session.aos") {
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string header;
    std::getline(f, header);
    if (header.find("AOS_CHAT_V1") == std::string::npos) return;

    std::lock_guard<std::mutex> lock(g_chat_mutex);
    g_messages.clear();
    std::string line, full = "";
    while (std::getline(f, line)) {
        if (line == "---MSG_END---") {
            size_t sep = full.find(":::");
            if (sep != std::string::npos) {
                g_messages.push_back({ full.substr(0, sep), full.substr(sep + 3) });
            }
            full = "";
        } else {
            full += (full.empty() ? "" : "\n") + line;
        }
    }
}

void save_bot_aosb(const std::string& path = "default.aosb") {
    std::lock_guard<std::mutex> lock(g_worker_mutex);
    std::ofstream f(path);
    if (!f.is_open()) return;
    f << "AOS_BOT_V1\n";
    f << "TEMP:::" << g_temperature << "\n";
    f << "SYS:::" << g_system_prompt << "\n";
}

void load_bot_aosb(const std::string& path = "default.aosb") {
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string header;
    std::getline(f, header);
    if (header.find("AOS_BOT_V1") == std::string::npos) return;

    std::lock_guard<std::mutex> lock(g_worker_mutex);
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("TEMP:::", 0) == 0) {
            try { g_temperature = std::stof(line.substr(7)); } catch(...) {}
        } else if (line.rfind("SYS:::", 0) == 0) {
            g_system_prompt = line.substr(6);
        }
    }
}

void scan_models() {
    g_available_models.clear();
    std::string model_dir = "model";
    if (fs::exists(model_dir) && fs::is_directory(model_dir)) {
        for (const auto& entry : fs::directory_iterator(model_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".gguf") {
                g_available_models.push_back(entry.path().string());
            }
        }
    }
    if (!g_available_models.empty() && g_selected_model.empty()) {
        g_selected_model = g_available_models[0];
    }
}

// --- LLM Worker Thread ---
void llm_worker_func() {
    llama_log_set([](enum ggml_log_level, const char*, void*) {}, nullptr);
    llama_backend_init();

    llama_model* model = nullptr;
    llama_context* ctx = nullptr;
    const struct llama_vocab* vocab = nullptr;

    auto load_model = [&](const std::string& path) -> bool {
        if (ctx) { llama_free(ctx); ctx = nullptr; }
        if (model) { llama_model_free(model); model = nullptr; }
        g_model_ready = false;

        llama_model_params mparams = llama_model_default_params();
        model = llama_model_load_from_file(path.c_str(), mparams);
        if (!model) return false;

        vocab = llama_model_get_vocab(model);
        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = 1024;
        cparams.n_threads = 4;
        cparams.n_threads_batch = 4;

        ctx = llama_init_from_model(model, cparams);
        if (!ctx) return false;

        g_model_ready = true;
        return true;
    };

    if (!g_selected_model.empty()) {
        load_model(g_selected_model);
    }

    while (g_app_running) {
        if (g_reload_model) {
            load_model(g_selected_model);
            g_reload_model = false;
        }

        if (!g_has_prompt || !g_model_ready) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        std::string full_prompt;
        float temp = 0.7f;
        {
            std::lock_guard<std::mutex> lock(g_worker_mutex);
            full_prompt = "<|system|>\n" + g_system_prompt + "</s>\n<|user|>\n" + g_pending_prompt + "</s>\n<|assistant|>\n";
            temp = g_temperature;
            g_has_prompt = false;
        }

        g_is_generating = true;
        g_stop_requested = false;

        llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(temp));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(1337));

        std::vector<llama_token> tokens(full_prompt.length() + 32);
        int n_tokens = llama_tokenize(vocab, full_prompt.c_str(), full_prompt.length(), tokens.data(), tokens.size(), true, true);
        if (n_tokens < 0) {
            tokens.resize(-n_tokens);
            n_tokens = llama_tokenize(vocab, full_prompt.c_str(), full_prompt.length(), tokens.data(), tokens.size(), true, true);
        }
        tokens.resize(n_tokens);

        llama_batch batch = llama_batch_get_one(tokens.data(), tokens.size());
        if (llama_decode(ctx, batch) == 0) {
            int gen_count = 0;
            auto start_time = std::chrono::high_resolution_clock::now();

            for (int i = 0; i < 512 && g_app_running; ++i) {
                if (g_stop_requested) {
                    std::lock_guard<std::mutex> lock(g_chat_mutex);
                    g_token_queue.push(" [Stopped]");
                    break;
                }

                llama_token new_token = llama_sampler_sample(smpl, ctx, -1);
                if (llama_vocab_is_eog(vocab, new_token)) break;

                char buf[64];
                int n_chars = llama_token_to_piece(vocab, new_token, buf, sizeof(buf), 0, true);
                if (n_chars > 0) {
                    std::lock_guard<std::mutex> lock(g_chat_mutex);
                    g_token_queue.push(std::string(buf, n_chars));
                }
                gen_count++;

                llama_batch next_batch = llama_batch_get_one(&new_token, 1);
                if (llama_decode(ctx, next_batch) != 0) break;
            }

            auto end_time = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed = end_time - start_time;
            if (elapsed.count() > 0) g_last_tps = gen_count / elapsed.count();
        }

        llama_sampler_free(smpl);
        g_is_generating = false;

        save_chat_aos("autosave.aos");
    }

    if (ctx) llama_free(ctx);
    if (model) llama_model_free(model);
    llama_backend_free();
}

// --- Compact Dark Theme ---
void SetupCompactAOSTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    
    // Sleek, dense spacing
    style.WindowPadding    = ImVec2(6.0f, 6.0f);
    style.FramePadding     = ImVec2(6.0f, 4.0f);
    style.ItemSpacing      = ImVec2(5.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 3.0f);
    style.ScrollbarSize    = 10.0f;
    style.WindowRounding   = 4.0f;
    style.ChildRounding    = 4.0f;
    style.FrameRounding    = 4.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]       = ImVec4(0.08f, 0.09f, 0.11f, 1.0f);
    colors[ImGuiCol_ChildBg]        = ImVec4(0.10f, 0.12f, 0.15f, 1.0f);
    colors[ImGuiCol_Button]         = ImVec4(0.12f, 0.55f, 0.35f, 1.0f);
    colors[ImGuiCol_ButtonHovered]  = ImVec4(0.15f, 0.65f, 0.42f, 1.0f);
    colors[ImGuiCol_FrameBg]        = ImVec4(0.13f, 0.15f, 0.19f, 1.0f);
    colors[ImGuiCol_Border]         = ImVec4(0.18f, 0.22f, 0.28f, 1.0f);
    colors[ImGuiCol_Text]           = ImVec4(0.92f, 0.94f, 0.96f, 1.0f);
}

int main(int argc, char* argv[]) {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");

    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;

    scan_models();
    load_bot_aosb("default.aosb");
    load_chat_aos("autosave.aos");
    if (g_messages.empty()) {
        g_messages.push_back({ "AOS", "AetherOS Core online. Ready for direct input." });
    }

    int win_w = 800;
    int win_h = 600;
    SDL_Window* window = SDL_CreateWindow("AetherOS // Compact", win_w, win_h, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, "software");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = 1.15f; // Crisp, compact text scaling

    SetupCompactAOSTheme();

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    std::thread worker(llm_worker_func);

    char input_buf[1024] = "";
    char sys_prompt_buf[1024];
    strncpy(sys_prompt_buf, g_system_prompt.c_str(), sizeof(sys_prompt_buf));

    bool show_settings = false;
    bool scroll_to_bottom = true;
    bool running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) running = false;
        }

        // Consume tokens
        {
            std::lock_guard<std::mutex> lock(g_chat_mutex);
            while (!g_token_queue.empty()) {
                if (!g_messages.empty() && g_messages.back().role == "AOS") {
                    g_messages.back().text += g_token_queue.front();
                }
                g_token_queue.pop();
                scroll_to_bottom = true;
            }
        }

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        int cur_w, cur_h;
        SDL_GetWindowSize(window, &cur_w, &cur_h);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2((float)cur_w, (float)cur_h));
        ImGui::Begin("AetherOS_Canvas", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        // --- Compact Top Bar ---
        if (ImGui::Button(" = ")) {
            show_settings = !show_settings;
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.6f, 1.0f), "AETHER-OS");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.6f, 0.7f, 1.0f), "| %s", g_model_ready ? "READY" : "LOADING");

        if (g_last_tps > 0.0f) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "(%.2f T/s)", g_last_tps.load());
        }

        // --- Chat Area (Expands to fill most of screen) ---
        float footer_height = 44.0f;
        ImGui::BeginChild("ChatRegion", ImVec2(0, -footer_height), true);
        for (const auto& msg : g_messages) {
            if (msg.role == "User") {
                ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "[You]");
            } else {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.5f, 1.0f), "[AOS]");
            }
            ImGui::SameLine();
            ImGui::TextWrapped("%s", msg.text.c_str());
            ImGui::Spacing();
        }
        if (scroll_to_bottom) {
            ImGui::SetScrollHereY(1.0f);
            scroll_to_bottom = false;
        }
        ImGui::EndChild();

        // --- Slim Compact Footer ---
        ImGui::PushItemWidth(-140.0f);
        bool enter = ImGui::InputTextWithHint("##PromptInput", "Ask AOS...", input_buf, IM_ARRAYSIZE(input_buf), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopItemWidth();

        ImGui::SameLine();
        if ((ImGui::Button("SEND", ImVec2(62, 0)) || enter) && strlen(input_buf) > 0) {
            if (g_model_ready && !g_is_generating) {
                {
                    std::lock_guard<std::mutex> lock(g_chat_mutex);
                    g_messages.push_back({ "User", input_buf });
                    g_messages.push_back({ "AOS", "" });
                }
                {
                    std::lock_guard<std::mutex> lock(g_worker_mutex);
                    g_pending_prompt = input_buf;
                    g_has_prompt = true;
                }
                input_buf[0] = '\0';
                scroll_to_bottom = true;
            }
        }

        ImGui::SameLine();
        if (g_is_generating) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.15f, 0.15f, 1.0f));
            if (ImGui::Button("STOP", ImVec2(62, 0))) g_stop_requested = true;
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.25f, 0.25f, 1.0f));
            ImGui::Button("STOP", ImVec2(62, 0));
            ImGui::PopStyleColor();
        }

        // --- Settings Drawer Modal ---
        if (show_settings) {
            ImGui::SetNextWindowSize(ImVec2(cur_w * 0.90f, cur_h * 0.85f), ImGuiCond_Always);
            ImGui::SetNextWindowPos(ImVec2(cur_w * 0.05f, cur_h * 0.07f), ImGuiCond_Always);
            if (ImGui::Begin("Settings & Files##Modal", &show_settings, ImGuiWindowFlags_NoCollapse)) {
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.6f, 1.0f), "CONFIG & PRESETS");
                ImGui::Separator();

                ImGui::Text("Active Model:");
                if (ImGui::BeginCombo("##ModelCombo", g_selected_model.c_str())) {
                    for (const auto& m : g_available_models) {
                        bool is_selected = (g_selected_model == m);
                        if (ImGui::Selectable(m.c_str(), is_selected)) {
                            g_selected_model = m;
                            g_reload_model = true;
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::Spacing();
                ImGui::Text("System Prompt (.aosb):");
                ImGui::InputTextMultiline("##SysPrompt", sys_prompt_buf, sizeof(sys_prompt_buf), ImVec2(-1, 80));
                
                if (ImGui::Button("Apply Prompt", ImVec2(110, 28))) {
                    std::lock_guard<std::mutex> lock(g_worker_mutex);
                    g_system_prompt = sys_prompt_buf;
                }
                ImGui::SameLine();
                if (ImGui::Button("Export .aosb", ImVec2(110, 28))) {
                    save_bot_aosb("default.aosb");
                }
                ImGui::SameLine();
                if (ImGui::Button("Import .aosb", ImVec2(110, 28))) {
                    load_bot_aosb("default.aosb");
                    strncpy(sys_prompt_buf, g_system_prompt.c_str(), sizeof(sys_prompt_buf));
                }

                ImGui::Spacing();
                ImGui::SliderFloat("Temperature", &g_temperature, 0.1f, 1.5f, "%.2f");

                ImGui::Separator();
                ImGui::Text("Session Management (.aos):");
                if (ImGui::Button("Export Chat (.aos)", ImVec2(140, 28))) {
                    save_chat_aos("saved_session.aos");
                }
                ImGui::SameLine();
                if (ImGui::Button("Import Chat (.aos)", ImVec2(140, 28))) {
                    load_chat_aos("saved_session.aos");
                }

                ImGui::Spacing();
                if (ImGui::Button("Clear Chat History", ImVec2(150, 28))) {
                    std::lock_guard<std::mutex> lock(g_chat_mutex);
                    g_messages.clear();
                    save_chat_aos("autosave.aos");
                }
                ImGui::SameLine();
                if (ImGui::Button("Close Menu", ImVec2(100, 28))) {
                    show_settings = false;
                }

                ImGui::End();
            }
        }

        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 20, 22, 26, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

        SDL_Delay(16);
    }

    save_chat_aos("autosave.aos");
    save_bot_aosb("default.aosb");

    g_app_running = false;
    g_stop_requested = true;
    if (worker.joinable()) worker.join();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
