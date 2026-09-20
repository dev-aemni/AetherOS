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
#include <csignal>

#ifdef _WIN32
#include <windows.h>
#endif

#include "llama.h"

namespace fs = std::filesystem;

// --- Colors (ANSI Cyberpunk Palette) ---
#define C_RESET       "\033[0m"
#define C_BOLD        "\033[1m"
#define C_DIM         "\033[2m"
#define C_LIGHT_BLUE  "\033[38;5;75m"   // Light Blue for AOS
#define C_GREEN       "\033[38;5;48m"   // Green for response text
#define C_CYAN        "\033[38;5;45m"   // Cyan for User
#define C_YELLOW      "\033[38;5;220m"
#define C_RED         "\033[38;5;196m"
#define C_GRAY        "\033[38;5;244m"

// --- Global State ---
struct Message {
    std::string role;
    std::string text;
};

std::vector<Message> g_messages;
std::string g_system_prompt = "You are AetherOS, a sharp offline AI assistant by @dev-aemni. Provide direct, structured, and helpful answers.";
std::string g_model_path = "";
float g_temperature = 0.7f;
std::atomic<bool> g_stop_token{false};
std::atomic<bool> g_is_generating{false};

llama_model* g_model = nullptr;
llama_context* g_ctx = nullptr;
const struct llama_vocab* g_vocab = nullptr;

// --- Signal Handler (Ctrl+C Aborts Generation Cleanly) ---
void handle_sigint(int sig) {
    if (g_is_generating) {
        g_stop_token = true;
    } else {
        std::cout << "\n" << C_GRAY << "[INFO] Exiting AetherOS. Goodbye!" << C_RESET << std::endl;
        exit(0);
    }
}

// --- .AOS Serialization ---
void save_chat_aos(const std::string& path = "autosave.aos") {
    std::ofstream f(path);
    if (!f.is_open()) return;
    f << "AOS_CHAT_V1\n";
    for (const auto& m : g_messages) {
        f << m.role << ":::" << m.text << "\n---MSG_END---\n";
    }
}

void load_chat_aos(const std::string& path = "autosave.aos") {
    std::ifstream f(path);
    if (!f.is_open()) return;
    std::string header;
    std::getline(f, header);
    if (header.find("AOS_CHAT_V1") == std::string::npos) return;

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

// --- Model Discovery ---
std::string auto_detect_model() {
    if (fs::exists("model")) {
        for (const auto& e : fs::directory_iterator("model")) {
            if (e.is_regular_file() && e.path().extension() == ".gguf") {
                return e.path().string();
            }
        }
    }
    return "";
}

// --- Core Generation with Exact Timing & Interrupt Stats ---
std::string generate_response(const std::string& user_prompt) {
    g_is_generating = true;
    g_stop_token = false;

    std::string full_prompt = "<|system|>\n" + g_system_prompt + "</s>\n<|user|>\n" + user_prompt + "</s>\n<|assistant|>\n";

    llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(g_temperature));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(1337));

    std::vector<llama_token> tokens(full_prompt.length() + 32);
    int n_tokens = llama_tokenize(g_vocab, full_prompt.c_str(), full_prompt.length(), tokens.data(), tokens.size(), true, true);
    if (n_tokens < 0) {
        tokens.resize(-n_tokens);
        n_tokens = llama_tokenize(g_vocab, full_prompt.c_str(), full_prompt.length(), tokens.data(), tokens.size(), true, true);
    }
    tokens.resize(n_tokens);

    std::string full_response = "";
    llama_batch batch = llama_batch_get_one(tokens.data(), tokens.size());

    int count = 0;
    auto start = std::chrono::high_resolution_clock::now();

    if (llama_decode(g_ctx, batch) == 0) {
        for (int i = 0; i < 1024; ++i) {
            if (g_stop_token) {
                std::cout << C_RED << " [Stopped]" << C_RESET;
                break;
            }

            llama_token new_token = llama_sampler_sample(smpl, g_ctx, -1);
            if (llama_vocab_is_eog(g_vocab, new_token)) break;

            char buf[64];
            int n = llama_token_to_piece(g_vocab, new_token, buf, sizeof(buf), 0, true);
            if (n > 0) {
                std::string piece(buf, n);
                std::cout << C_GREEN << piece << C_RESET << std::flush;
                full_response += piece;
            }
            count++;

            llama_batch next_batch = llama_batch_get_one(&new_token, 1);
            if (llama_decode(g_ctx, next_batch) != 0) break;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    double sec = elapsed.count();
    double tps = (sec > 0.0) ? (count / sec) : 0.0;

    // Exact required stats format: [31 tokens | 4.34163 T/s | 5.277s]
    char stats_buf[128];
    snprintf(stats_buf, sizeof(stats_buf), "[%d tokens | %.5f T/s | %.3fs]", count, tps, sec);
    std::cout << "\n" << C_GRAY << stats_buf << C_RESET << std::endl;

    llama_sampler_free(smpl);
    g_is_generating = false;
    return full_response;
}

void print_banner() {
    std::cout << C_GREEN << C_BOLD;
    std::cout << "╔═══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║        A E T H E R - O S  //  N E U R A L   C L I             ║\n";
    std::cout << "║     Offline Intelligence Core  |  Author: @dev-aemni          ║\n";
    std::cout << "║     Repo: https://github.com/dev-aemni/AetherOS              ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════╝\n";
    std::cout << C_RESET;
    std::cout << C_GRAY << "Type your prompt or /help. Ctrl+C halts streaming safely.\n\n" << C_RESET;
}

void print_help() {
    std::cout << C_YELLOW << C_BOLD << "\n--- AetherOS Commands ---\n" << C_RESET;
    std::cout << C_CYAN << "  /help            " << C_RESET << "Show this cheat sheet\n";
    std::cout << C_CYAN << "  /clear           " << C_RESET << "Clear chat history & wipe memory\n";
    std::cout << C_CYAN << "  /sys             " << C_RESET << "View current system prompt\n";
    std::cout << C_CYAN << "  /sys <prompt>    " << C_RESET << "Update system prompt dynamically\n";
    std::cout << C_CYAN << "  /save <file.aos> " << C_RESET << "Export conversation session\n";
    std::cout << C_CYAN << "  /load <file.aos> " << C_RESET << "Restore past conversation session\n";
    std::cout << C_CYAN << "  /temp <value>    " << C_RESET << "Set temperature (0.1 to 1.5)\n";
    std::cout << C_CYAN << "  /exit, /quit     " << C_RESET << "Exit application\n\n";
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif

    std::signal(SIGINT, handle_sigint);

    std::string one_shot_prompt = "";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-m" || arg == "--model") && i + 1 < argc) {
            g_model_path = argv[++i];
        } else if ((arg == "-s" || arg == "--sys") && i + 1 < argc) {
            g_system_prompt = argv[++i];
        } else if ((arg == "-t" || arg == "--temp") && i + 1 < argc) {
            g_temperature = std::stof(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: aos [flags] [prompt]\n"
                      << "Flags:\n"
                      << "  -m, --model <path>  Specify GGUF model path\n"
                      << "  -s, --sys <prompt>  Set system prompt\n"
                      << "  -t, --temp <val>    Set temperature (default: 0.7)\n"
                      << "  -h, --help          Show help\n\n"
                      << "Examples:\n"
                      << "  aos                       Launch full interactive CLI\n"
                      << "  aos \"What is Linux?\"     One-shot query and exit\n";
            return 0;
        } else {
            if (!one_shot_prompt.empty()) one_shot_prompt += " ";
            one_shot_prompt += arg;
        }
    }

    if (g_model_path.empty()) {
        g_model_path = auto_detect_model();
    }

    if (g_model_path.empty()) {
        std::cerr << C_RED << "[ERROR] No model found in model/ directory!" << C_RESET << std::endl;
        return 1;
    }

    llama_log_set([](enum ggml_log_level, const char*, void*) {}, nullptr);
    llama_backend_init();

    llama_model_params mparams = llama_model_default_params();
    g_model = llama_model_load_from_file(g_model_path.c_str(), mparams);
    if (!g_model) {
        std::cerr << C_RED << "[ERROR] Failed to load model: " << g_model_path << C_RESET << std::endl;
        return 1;
    }

    g_vocab = llama_model_get_vocab(g_model);
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 1024;
    cparams.n_threads = 4;
    cparams.n_threads_batch = 4;

    g_ctx = llama_init_from_model(g_model, cparams);
    if (!g_ctx) {
        std::cerr << C_RED << "[ERROR] Failed to initialize context." << C_RESET << std::endl;
        return 1;
    }

    // --- MODE 1: One-Shot CLI ---
    if (!one_shot_prompt.empty()) {
        std::cout << C_BOLD << C_CYAN << "User: " << C_RESET << one_shot_prompt << "\n\n";
        std::cout << C_BOLD << C_LIGHT_BLUE << "AOS: " << C_RESET;
        generate_response(one_shot_prompt);

        llama_free(g_ctx);
        llama_model_free(g_model);
        llama_backend_free();
        return 0;
    }

    // --- MODE 2: Interactive REPL ---
    print_banner();
    load_chat_aos("autosave.aos");

    if (!g_messages.empty()) {
        std::cout << C_DIM << "[Restored " << g_messages.size() << " previous messages from autosave.aos]\n\n" << C_RESET;
    }

    std::string input;
    while (true) {
        std::cout << C_BOLD << C_CYAN << "User ❯ " << C_RESET;
        if (!std::getline(std::cin, input)) break;

        if (input.empty()) continue;

        if (input == "/exit" || input == "/quit") break;
        if (input == "/help") { print_help(); continue; }
        if (input == "/clear") {
            g_messages.clear();
            save_chat_aos("autosave.aos");
            std::cout << C_YELLOW << "[Memory Cleared]" << C_RESET << std::endl;
            continue;
        }
        if (input == "/sys") {
            std::cout << C_YELLOW << "System Prompt: " << C_RESET << g_system_prompt << std::endl;
            continue;
        }
        if (input.rfind("/sys ", 0) == 0) {
            g_system_prompt = input.substr(5);
            std::cout << C_GREEN << "[System prompt updated]" << C_RESET << std::endl;
            continue;
        }
        if (input.rfind("/save ", 0) == 0) {
            std::string path = input.substr(6);
            save_chat_aos(path);
            std::cout << C_GREEN << "[Saved to " << path << "]" << C_RESET << std::endl;
            continue;
        }
        if (input.rfind("/load ", 0) == 0) {
            std::string path = input.substr(6);
            load_chat_aos(path);
            std::cout << C_GREEN << "[Loaded " << g_messages.size() << " messages from " << path << "]" << C_RESET << std::endl;
            continue;
        }
        if (input.rfind("/temp ", 0) == 0) {
            g_temperature = std::stof(input.substr(6));
            std::cout << C_GREEN << "[Temperature set to " << g_temperature << "]" << C_RESET << std::endl;
            continue;
        }

        // Generate response with Light Blue prefix
        std::cout << "\n" << C_BOLD << C_LIGHT_BLUE << "AOS ❯ " << C_RESET;
        std::string reply = generate_response(input);
        std::cout << "\n";

        g_messages.push_back({ "User", input });
        g_messages.push_back({ "AOS", reply });
        save_chat_aos("autosave.aos");
    }

    save_chat_aos("autosave.aos");
    llama_free(g_ctx);
    llama_model_free(g_model);
    llama_backend_free();
    return 0;
}
