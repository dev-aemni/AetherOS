#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <csignal>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#include <readline/readline.h>
#include <readline/history.h>
#endif

#include "llama.h"
#include "ui.h"

namespace fs = std::filesystem;

struct Message {
    std::string role;
    std::string text;
};

std::vector<Message> g_messages;

std::string g_system_prompt = 
"You are AetherOS, a sharp, friendly, and highly capable AI assistant built by Aemni Acc (@dev-aemni; note: Aemni Acc is an account handle, not a real name). "
"You chat naturally. You understand English, Spanish, Hindi, and other languages. "
"You possess autonomous persistent memory. When the user shares an important personal fact, name, preference, or tech stack (e.g. 'I like JS most', 'My name is X'), record it silently at the very end of your response using: <mem:category>Brief factual description</mem:category>. "
"DO NOT do this for every message—only when the user shares truly meaningful personal details or preferences. "
"When asked to execute shell tasks, output commands in ```bash ... ``` blocks.";

std::string g_model_path = "";
float g_temperature = 0.7f;
int g_max_tokens = 512;
std::string g_response_mode = "undefined";
std::string g_session_file = "";

enum AgentMode { AGENT_CONFIRM, AGENT_AUTO, AGENT_OFF };
AgentMode g_agent_mode = AGENT_CONFIRM;

bool g_use_openrouter = false;
std::string g_openrouter_key = "";
std::string g_openrouter_model = "google/gemini-2.0-flash-exp:free";

std::atomic<bool> g_stop_token{false};
std::atomic<bool> g_is_generating{false};
std::atomic<bool> g_is_thinking{false};

llama_model* g_model = nullptr;
llama_context* g_ctx = nullptr;
const struct llama_vocab* g_vocab = nullptr;

// --- AetherOS Crypt-Pack Engine ---
const uint8_t AOS_MAGIC[5] = { 0x1f, 0x8b, 0x41, 0x4f, 0x53 };
const uint8_t CIPHER_KEY[16] = { 0xa5, 0x5a, 0xf0, 0x0f, 0x3c, 0xc3, 0x96, 0x69, 0x55, 0xaa, 0x12, 0x34, 0x78, 0x9a, 0xbc, 0xef };

std::string aos_pack(const std::string& input) {
    if (input.empty()) return "";
    std::string packed;
    packed.reserve(input.size() + 8);
    for (int i = 0; i < 5; ++i) packed += (char)AOS_MAGIC[i];
    for (size_t i = 0; i < input.size(); ++i) {
        uint8_t b = (uint8_t)input[i];
        uint8_t k = CIPHER_KEY[i % 16] ^ (uint8_t)(i * 37 + 13);
        packed += (char)(b ^ k);
    }
    return packed;
}

std::string aos_unpack(const std::string& data) {
    if (data.size() < 5) return data;
    for (int i = 0; i < 5; ++i) {
        if ((uint8_t)data[i] != AOS_MAGIC[i]) return data;
    }
    std::string unpacked;
    unpacked.reserve(data.size() - 5);
    for (size_t i = 0; i < data.size() - 5; ++i) {
        uint8_t b = (uint8_t)data[i + 5];
        uint8_t k = CIPHER_KEY[i % 16] ^ (uint8_t)(i * 37 + 13);
        unpacked += (char)(b ^ k);
    }
    return unpacked;
}

bool write_packed_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::string packed = aos_pack(content);
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(packed.data(), packed.size());
    f.close();
    return true;
}

std::string read_packed_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return "";
    std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    return aos_unpack(raw);
}

std::string get_exact_datetime() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H-%M-%S", &tm);
    return std::string(buf);
}

fs::path get_app_dir() {
#ifdef _WIN32
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    return fs::path(buffer).parent_path();
#elif defined(__linux__) || defined(__ANDROID__)
    char buffer[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len != -1) {
        buffer[len] = '\0';
        return fs::canonical(fs::path(buffer)).parent_path();
    }
    const char* home = getenv("HOME");
    if (home) {
        fs::path p = fs::path(home) / "programs" / "AetherOS";
        if (fs::exists(p)) return p;
        p = fs::path(home) / "AetherOS";
        if (fs::exists(p)) return p;
    }
    return fs::current_path();
#else
    return fs::current_path();
#endif
}

std::string read_file_to_string(const std::string& path) {
    return read_packed_file(path);
}

void handle_sigint(int sig) {
    if (g_is_generating) {
        g_stop_token = true;
    } else {
        std::cout << "\n" << C_MUTED << "Exiting AetherOS. Goodbye!" << C_RESET << std::endl;
        exit(0);
    }
}

std::string run_shell_command(const std::string& cmd) {
    std::string output = "";
    FILE* pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) return "[Error: Failed to spawn subshell]";
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe) != NULL) {
        std::cout << C_LINE << "│ " << C_WHITE << buf;
        output += buf;
    }
    int code = pclose(pipe);
    output += "\n[Process Exit Code: " + std::to_string(code) + "]";
    return output;
}

std::vector<std::string> extract_commands(const std::string& text) {
    std::vector<std::string> cmds;
    size_t pos = 0;
    while (true) {
        size_t start = text.find("```bash\n", pos);
        if (start == std::string::npos) start = text.find("```sh\n", pos);
        if (start == std::string::npos) break;

        size_t code_start = text.find("\n", start) + 1;
        size_t end = text.find("```", code_start);
        if (end == std::string::npos) break;

        std::string block = text.substr(code_start, end - code_start);
        while (!block.empty() && (block.back() == '\n' || block.back() == ' ' || block.back() == '\r')) {
            block.pop_back();
        }
        if (!block.empty()) cmds.push_back(block);
        pos = end + 3;
    }
    return cmds;
}

void save_chat_aos(const std::string& filename) {
    if (g_messages.empty()) return;
    fs::path base = get_app_dir() / "data" / "chats";
    fs::create_directories(base);

    std::string fname = filename;
    if (fname.empty()) fname = get_exact_datetime() + ".aos";
    if (fname.rfind(".aos") == std::string::npos) fname += ".aos";

    fs::path target = (fname.find("data/chats/") != std::string::npos) ? (get_app_dir() / fname) : (base / fname);

    std::string serialized = "AOS_CHAT_V1\n";
    for (const auto& m : g_messages) {
        serialized += m.role + ":::" + m.text + "\n---MSG_END---\n";
    }
    write_packed_file(target, serialized);
}

void load_chat_aos(const std::string& filename) {
    fs::path target = (filename.find("data/chats/") != std::string::npos) ? (get_app_dir() / filename) : (get_app_dir() / "data" / "chats" / filename);
    if (target.extension() != ".aos") target += ".aos";

    std::string content = read_packed_file(target);
    if (content.empty()) return;

    std::istringstream stream(content);
    std::string header;
    std::getline(stream, header);
    if (header.find("AOS_CHAT_V1") == std::string::npos) return;

    g_messages.clear();
    std::string line, full = "";
    while (std::getline(stream, line)) {
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

std::string get_latest_chat_file() {
    fs::path chats_dir = get_app_dir() / "data" / "chats";
    if (!fs::exists(chats_dir)) return "";
    std::string latest_file = "";
    fs::file_time_type latest_time;
    bool first = true;
    for (const auto& e : fs::directory_iterator(chats_dir)) {
        if (e.is_regular_file() && e.path().extension() == ".aos") {
            if (first || e.last_write_time() > latest_time) {
                latest_time = e.last_write_time();
                latest_file = e.path().filename().string();
                first = false;
            }
        }
    }
    return latest_file;
}

std::vector<std::string> get_model_list() {
    std::vector<std::string> list;
    fs::path mdir = get_app_dir() / "model";
    if (fs::exists(mdir)) {
        for (const auto& e : fs::directory_iterator(mdir)) {
            if (e.is_regular_file() && e.path().extension() == ".gguf") {
                std::string fname = e.path().filename().string();
                if (fname.find("mmproj") == std::string::npos) {
                    list.push_back(e.path().string());
                }
            }
        }
    }
    return list;
}

bool load_model_file(const std::string& path) {
    if (g_ctx) { llama_free(g_ctx); g_ctx = nullptr; }
    if (g_model) { llama_model_free(g_model); g_model = nullptr; }

    llama_model_params mparams = llama_model_default_params();
    g_model = llama_model_load_from_file(path.c_str(), mparams);
    if (!g_model) return false;

    g_vocab = llama_model_get_vocab(g_model);
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 2048;
    cparams.n_threads = 4;
    cparams.n_threads_batch = 4;

    g_ctx = llama_init_from_model(g_model, cparams);
    if (!g_ctx) return false;

    g_model_path = path;
    return true;
}

bool set_system_prompt_smart(const std::string& input_or_path, bool verbose = true) {
    fs::path check_path = input_or_path;
    if (!fs::exists(check_path)) {
        fs::path sample_path = get_app_dir() / "data" / "samples" / input_or_path;
        if (fs::exists(sample_path)) check_path = sample_path;
    }

    if (fs::exists(check_path) && fs::is_regular_file(check_path)) {
        std::string content = read_packed_file(check_path);
        if (!content.empty()) {
            g_system_prompt = content;
            if (verbose) {
                std::cout << C_GREEN << "\n  [Loaded System Prompt from: " << check_path.filename().string() 
                          << " (" << content.length() << " chars)]\n\n" << C_RESET;
            }
            return true;
        }
    }
    g_system_prompt = input_or_path;
    if (verbose) {
        std::cout << C_GREEN << "\n  [System Prompt updated (" << g_system_prompt.length() << " chars)]\n\n" << C_RESET;
    }
    return true;
}

void list_memories() {
    fs::path mdir = get_app_dir() / "data" / "memory";
    std::cout << C_LINE << "\n╭─ " << C_RED << "Active Persistent Memory Files (.mem.dat)" << C_LINE << " ─────────────────\n" << C_RESET;
    bool found = false;
    if (fs::exists(mdir)) {
        for (const auto& e : fs::directory_iterator(mdir)) {
            if (e.is_regular_file() && e.path().filename().string().find(".mem.dat") != std::string::npos) {
                found = true;
                std::cout << C_LINE << "│ " << C_WHITE << e.path().filename().string() 
                          << " " << C_MUTED << "(" << fs::file_size(e.path()) << " bytes)\n";
            }
        }
    }
    if (!found) {
        std::cout << C_LINE << "│ " << C_MUTED << "No custom memories stored. The AI records important facts autonomously!\n";
    }
    std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n\n" << C_RESET;
}

void save_custom_memory(const std::string& name, const std::string& content) {
    fs::path mdir = get_app_dir() / "data" / "memory";
    fs::create_directories(mdir);

    int max_id = 0;
    for (const auto& e : fs::directory_iterator(mdir)) {
        std::string fname = e.path().filename().string();
        size_t dash = fname.find('-');
        if (dash != std::string::npos) {
            try { max_id = std::max(max_id, std::stoi(fname.substr(0, dash))); } catch (...) {}
        }
    }
    char id_buf[16];
    snprintf(id_buf, sizeof(id_buf), "%03d", max_id + 1);
    fs::path target_file = mdir / (std::string(id_buf) + "-" + name + ".mem.dat");

    write_packed_file(target_file, content);
}

std::string get_active_memory_context() {
    std::string mem_block = "";
    fs::path mdir = get_app_dir() / "data" / "memory";
    if (fs::exists(mdir)) {
        for (const auto& e : fs::directory_iterator(mdir)) {
            if (e.is_regular_file() && e.path().filename().string().find(".mem.dat") != std::string::npos) {
                std::string c = read_packed_file(e.path());
                if (!c.empty()) {
                    mem_block += "\n[Known Fact: " + c + "]\n";
                }
            }
        }
    }
    return mem_block;
}

bool extract_and_record_memories(std::string& response_text) {
    bool memory_saved = false;
    size_t start = 0;
    while ((start = response_text.find("<mem:")) != std::string::npos) {
        size_t tag_end = response_text.find('>', start);
        if (tag_end == std::string::npos) break;

        std::string mem_name = response_text.substr(start + 5, tag_end - (start + 5));
        size_t close_tag = response_text.find("</mem:", tag_end);
        if (close_tag == std::string::npos) close_tag = response_text.find("</mem>", tag_end);
        if (close_tag == std::string::npos) break;

        size_t close_end = response_text.find('>', close_tag);
        if (close_end == std::string::npos) break;

        std::string mem_fact = response_text.substr(tag_end + 1, close_tag - (tag_end + 1));
        while (!mem_fact.empty() && (mem_fact.front() == ' ' || mem_fact.front() == '\n')) mem_fact.erase(0, 1);
        while (!mem_fact.empty() && (mem_fact.back() == ' ' || mem_fact.back() == '\n')) mem_fact.pop_back();

        if (!mem_fact.empty()) {
            save_custom_memory(mem_name, mem_fact);
            memory_saved = true;
        }

        response_text.erase(start, (close_end + 1) - start);
    }
    return memory_saved;
}

std::string prune_for_history(const std::string& text) {
    if (text.length() < 140) return text;
    size_t code_pos = text.find("```");
    if (code_pos != std::string::npos && code_pos < 100) {
        return text.substr(0, code_pos) + "\n[code snippet generated earlier]\n";
    }
    return text.substr(0, 120) + "...";
}

std::string generate_response_raw(const std::string& input_text, bool& memory_was_saved) {
    g_is_generating = true;
    g_stop_token = false;
    memory_was_saved = false;

    std::thread spinner_thread;
    ui_start_thinking(g_is_thinking, g_stop_token, spinner_thread);

    auto t_start = std::chrono::steady_clock::now();

    std::string mode_modifier = "";
    if (g_response_mode == "short") mode_modifier = " [Instruction: Be brief, 1-2 sentences.]";
    else if (g_response_mode == "long") mode_modifier = " [Instruction: Provide an in-depth breakdown.]";

    std::string mem_context = get_active_memory_context();
    std::string full_prompt = "<|im_start|>system\n" + g_system_prompt + mem_context + mode_modifier + "<|im_end|>\n";
    
    int start_idx = (g_messages.size() > 4) ? (g_messages.size() - 4) : 0;
    for (size_t i = start_idx; i < g_messages.size(); ++i) {
        if (g_messages[i].role == "User") {
            full_prompt += "<|im_start|>user\n" + g_messages[i].text + "<|im_end|>\n";
        } else {
            full_prompt += "<|im_start|>assistant\n" + prune_for_history(g_messages[i].text) + "<|im_end|>\n";
        }
    }
    full_prompt += "<|im_start|>user\n" + input_text + "<|im_end|>\n<|im_start|>assistant\n";

    int32_t n_vocab = llama_vocab_n_tokens(g_vocab);
    llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_penalties(n_vocab, 64, 1.15f, 0.0f, 0.0f));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(g_temperature));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(1337));

    std::vector<llama_token> tokens(full_prompt.length() + 64);
    int n_tokens = llama_tokenize(g_vocab, full_prompt.c_str(), full_prompt.length(), tokens.data(), tokens.size(), true, true);
    if (n_tokens < 0) {
        tokens.resize(-n_tokens);
        n_tokens = llama_tokenize(g_vocab, full_prompt.c_str(), full_prompt.length(), tokens.data(), tokens.size(), true, true);
    }
    tokens.resize(n_tokens);

    std::string full_response = "";
    llama_batch batch = llama_batch_get_one(tokens.data(), tokens.size());

    int count = 0;
    auto decode_start = std::chrono::steady_clock::now();

    if (llama_decode(g_ctx, batch) == 0) {
        decode_start = std::chrono::steady_clock::now();
        ui_stop_thinking(g_is_thinking, spinner_thread);

        for (int i = 0; i < g_max_tokens; ++i) {
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
                if (piece.find("<mem:") == std::string::npos && piece.find("</mem") == std::string::npos) {
                    ui_assistant_stream_piece(piece);
                }
                full_response += piece;
            }
            count++;

            llama_batch next_batch = llama_batch_get_one(&new_token, 1);
            if (llama_decode(g_ctx, next_batch) != 0) break;
        }
    } else {
        decode_start = std::chrono::steady_clock::now();
        ui_stop_thinking(g_is_thinking, spinner_thread);
        std::cout << C_RED << "│ [Error: Context limit reached. Run /n for a new chat.]\n" << C_RESET;
    }

    auto decode_end = std::chrono::steady_clock::now();
    double think_sec = std::chrono::duration<double>(decode_start - t_start).count();
    double gen_sec = std::chrono::duration<double>(decode_end - decode_start).count();
    double tps = (gen_sec > 0.0) ? (count / gen_sec) : 0.0;

    memory_was_saved = extract_and_record_memories(full_response);
    if (memory_was_saved) {
        std::cout << " " << C_GREEN << "●" << C_RESET;
    }

    ui_assistant_footer(count, tps, gen_sec, think_sec, memory_was_saved);

    llama_sampler_free(smpl);
    g_is_generating = false;
    return full_response;
}

std::string run_agent_loop(const std::string& initial_user_input) {
    std::string current_input = initial_user_input;
    std::string final_response = "";
    int agent_turns = 0;
    const int max_turns = 4;

    while (agent_turns < max_turns) {
        bool mem_saved = false;
        std::string reply = generate_response_raw(current_input, mem_saved);
        final_response += reply + "\n";

        if (g_agent_mode == AGENT_OFF) break;

        std::vector<std::string> cmds = extract_commands(reply);
        if (cmds.empty()) break;

        agent_turns++;

        std::string tool_feedbacks = "";
        for (const auto& cmd : cmds) {
            std::cout << C_LINE << "╭─ " << C_YELLOW << "Agent Action: Shell Execution" << C_LINE << " ──────────────────────\n" << C_RESET;
            std::cout << C_LINE << "│ " << C_CYAN << "$ " << cmd << "\n" << C_RESET;
            std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n" << C_RESET;

            bool allow = (g_agent_mode == AGENT_AUTO);
            if (!allow) {
                std::cout << C_BOLD << C_RED << "Execute command? [Y/n/auto]: " << C_RESET;
                std::string choice;
                std::getline(std::cin, choice);
                if (choice == "auto") {
                    g_agent_mode = AGENT_AUTO;
                    allow = true;
                } else if (choice.empty() || choice == "y" || choice == "Y") {
                    allow = true;
                }
            }

            if (allow) {
                std::cout << C_LINE << "╭─ Executing... ───────────────────────────────────────────\n" << C_RESET;
                std::string result = run_shell_command(cmd);
                std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n\n" << C_RESET;
                tool_feedbacks += "[Terminal Execution Output for `" + cmd + "`]:\n" + result + "\n";
            } else {
                std::cout << C_MUTED << "[Execution skipped by user]\n\n" << C_RESET;
                tool_feedbacks += "[Command `" + cmd + "` execution rejected by user]\n";
            }
        }

        current_input = tool_feedbacks + "\nInspect output above and continue.";
        std::cout << C_LINE << "╭─ " << C_RED << C_BOLD << "AetherOS (Agent Step " << agent_turns << ") " << C_LINE << "─────────────────────────\n" << C_RESET;
    }
    return final_response;
}

// Full Formatted CLI Help Menu
void print_full_cli_help() {
    std::cout << C_RED << C_BOLD << "\n╔═══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║            A E T H E R - O S   v7.2   M A N U A L                 ║\n";
    std::cout << "║        Autonomous Neural CLI Core  |  Author: @dev-aemni          ║\n";
    std::cout << "╚═══════════════════════════════════════════════════════════════════╝\n" << C_RESET;

    std::cout << C_WHITE << C_BOLD << "\nCOMMAND-LINE USAGE:\n" << C_RESET;
    std::cout << C_AMBER << "  aos                     " << C_MUTED << "Resume latest session from data/chats/\n";
    std::cout << C_AMBER << "  aos n                   " << C_MUTED << "Start a FRESH, brand-new chat session (no history)\n";
    std::cout << C_AMBER << "  aos \"your prompt\"       " << C_MUTED << "One-shot query: outputs response and exits\n";

    std::cout << C_WHITE << C_BOLD << "\nCOMMAND-LINE FLAGS:\n" << C_RESET;
    std::cout << C_CYAN  << "  n, -n, --new            " << C_MUTED << "Start fresh session (ignores autosave resume)\n";
    std::cout << C_CYAN  << "  -s,  --sys <text|path>  " << C_MUTED << "Set system prompt (or load from file .txt/.md)\n";
    std::cout << C_CYAN  << "  -sf, --sys-file <path>  " << C_MUTED << "Explicitly load system prompt from file\n";
    std::cout << C_CYAN  << "  -m,  --model <path>     " << C_MUTED << "Specify custom GGUF model path\n";
    std::cout << C_CYAN  << "  -t,  --temp <val>       " << C_MUTED << "Set sampling temperature (default: 0.7)\n";
    std::cout << C_CYAN  << "  --auto                  " << C_MUTED << "Autonomous shell execution mode without prompts\n";
    std::cout << C_CYAN  << "  -h,  --help, -help      " << C_MUTED << "Show this manual\n";

    std::cout << C_WHITE << C_BOLD << "\nINTERACTIVE CHAT COMMANDS (Inside session):\n" << C_RESET;
    std::cout << C_GREEN << "  /n, /new                " << C_MUTED << "Reset to a brand-new session immediately\n";
    std::cout << C_GREEN << "  /help                   " << C_MUTED << "Show in-chat cheat sheet\n";
    std::cout << C_GREEN << "  /mem                    " << C_MUTED << "List active memory files\n";
    std::cout << C_GREEN << "  /mem <name> <fact>      " << C_MUTED << "Manually save persistent memory fact\n";
    std::cout << C_GREEN << "  /sys <file|prompt>      " << C_MUTED << "Update system prompt dynamically\n";
    std::cout << C_GREEN << "  /cmd <command>          " << C_MUTED << "Execute shell command inside session\n";
    std::cout << C_GREEN << "  /model [name|#]         " << C_MUTED << "List or switch active GGUF models\n";
    std::cout << C_GREEN << "  /token <limit>          " << C_MUTED << "Set token limit (e.g. /token 1024)\n";
    std::cout << C_GREEN << "  /q, exit, quit          " << C_MUTED << "Quit application cleanly\n\n";
}

int main(int argc, char* argv[]) {
    ui_init();
    std::signal(SIGINT, handle_sigint);

    std::string one_shot_prompt = "";
    bool force_new_chat = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "n" || arg == "-n" || arg == "--new") {
            force_new_chat = true;
        } else if (arg == "-m" || arg == "--model") {
            if (i + 1 < argc) g_model_path = argv[++i];
        } else if (arg == "-s" || arg == "--sys" || arg == "-sf" || arg == "--sys-file") {
            if (i + 1 < argc) set_system_prompt_smart(argv[++i], false);
        } else if (arg == "-t" || arg == "--temp") {
            if (i + 1 < argc) g_temperature = std::stof(argv[++i]);
        } else if (arg == "--auto") {
            g_agent_mode = AGENT_AUTO;
        } else if (arg == "-h" || arg == "--help" || arg == "-help") {
            print_full_cli_help();
            return 0;
        } else {
            if (!one_shot_prompt.empty()) one_shot_prompt += " ";
            one_shot_prompt += arg;
        }
    }

    auto model_list = get_model_list();
    if (g_model_path.empty() && !model_list.empty()) g_model_path = model_list[0];

    if (g_model_path.empty()) {
        std::cerr << C_RED << "[ERROR] No model found in model/ directory!" << C_RESET << std::endl;
        return 1;
    }

    llama_log_set([](enum ggml_log_level, const char*, void*) {}, nullptr);
    llama_backend_init();

    if (!load_model_file(g_model_path)) {
        std::cerr << C_RED << "[ERROR] Failed to load model: " << g_model_path << C_RESET << std::endl;
        return 1;
    }

    // One-Shot Mode
    if (!one_shot_prompt.empty()) {
        ui_user_card(one_shot_prompt);
        ui_assistant_header();
        run_agent_loop(one_shot_prompt);

        if (g_ctx) llama_free(g_ctx);
        if (g_model) llama_model_free(g_model);
        llama_backend_free();
        return 0;
    }

    // Interactive Mode Setup
    if (force_new_chat) {
        // Start completely fresh session
        g_messages.clear();
        g_session_file = get_exact_datetime() + ".aos";
    } else {
        std::string latest_session = get_latest_chat_file();
        if (!latest_session.empty()) {
            load_chat_aos(latest_session);
            g_session_file = latest_session;
        } else {
            g_session_file = get_exact_datetime() + ".aos";
        }
    }

    std::string m_tag = fs::path(g_model_path).stem().string();
    ui_banner(m_tag, g_temperature, g_session_file);

    if (force_new_chat) {
        std::cout << C_GREEN << "  [Started Fresh Chat Session: data/chats/" << g_session_file << "]\n\n" << C_RESET;
    } else if (!g_messages.empty()) {
        std::cout << C_MUTED << "  [Restored " << g_messages.size() << " messages from data/chats/" << g_session_file << "]\n\n" << C_RESET;
    }

    std::string input;
    while (true) {
        std::string mode_str = (g_agent_mode == AGENT_AUTO) ? "auto" : ((g_agent_mode == AGENT_CONFIRM) ? "confirm" : "off");
        std::string prompt_prefix = ui_prompt_dock(m_tag.substr(0, 16), mode_str, g_max_tokens);

        char* line_read = nullptr;
#ifndef _WIN32
        line_read = readline(prompt_prefix.c_str());
#else
        std::cout << C_LINE << "╰" << C_BOLD << C_RED << "❯ " << C_WHITE;
        std::string win_line;
        if (!std::getline(std::cin, win_line)) break;
        line_read = strdup(win_line.c_str());
#endif

        if (!line_read) break;

        std::string raw_input(line_read);
        if (!raw_input.empty()) {
#ifndef _WIN32
            add_history(line_read);
#endif
        }
        free(line_read);

        while (!raw_input.empty() && (raw_input.front() == ' ' || raw_input.front() == '\t')) raw_input.erase(0, 1);
        while (!raw_input.empty() && (raw_input.back() == ' ' || raw_input.back() == '\t')) raw_input.pop_back();

        if (raw_input.empty()) continue;

        if (raw_input == "/q" || raw_input.rfind("/q ", 0) == 0 ||
            raw_input == "/quit" || raw_input.rfind("/quit ", 0) == 0 ||
            raw_input == "/exit" || raw_input.rfind("/exit ", 0) == 0 ||
            raw_input == "exit" || raw_input == "quit" || raw_input == "q") {
            break;
        }

        if (raw_input == "/help") { ui_help(); continue; }

        // Start New Chat In-Session
        if (raw_input == "/n" || raw_input == "/new") {
            g_messages.clear();
            g_session_file = get_exact_datetime() + ".aos";
            std::cout << C_GREEN << "\n  [New chat session active: data/chats/" << g_session_file << "]\n\n" << C_RESET;
            continue;
        }

        if (raw_input == "/mem") {
            list_memories();
            continue;
        }
        if (raw_input.rfind("/mem ", 0) == 0) {
            std::string rest = raw_input.substr(5);
            size_t sp = rest.find(' ');
            if (sp != std::string::npos) {
                save_custom_memory(rest.substr(0, sp), rest.substr(sp + 1));
                std::cout << C_GREEN << "\n  [Memory saved: " << rest.substr(0, sp) << ".mem.dat]\n\n" << C_RESET;
            } else {
                std::cout << C_YELLOW << "\n  [Usage: /mem <name> <fact to remember>]\n\n" << C_RESET;
            }
            continue;
        }

        if (raw_input.rfind("/sys ", 0) == 0) {
            set_system_prompt_smart(raw_input.substr(5));
            continue;
        }

        if (raw_input == "/sys") {
            std::cout << C_LINE << "╭─ Active System Prompt ───────────────────────────────────\n" << C_RESET;
            std::cout << C_LINE << "│ " << C_WHITE << g_system_prompt << "\n" << C_RESET;
            std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n\n" << C_RESET;
            continue;
        }

        if (raw_input == "/clear") {
            g_messages.clear();
            g_session_file = get_exact_datetime() + ".aos";
            std::cout << C_MUTED << "\n  Context cleared. New session: data/chats/" << g_session_file << "\n\n" << C_RESET;
            continue;
        }

        if (raw_input.rfind("/cmd ", 0) == 0) {
            std::cout << C_LINE << "╭─ Shell Execution ────────────────────────────────────────\n" << C_RESET;
            run_shell_command(raw_input.substr(5));
            std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n\n" << C_RESET;
            continue;
        }

        ui_assistant_header();
        std::string reply = run_agent_loop(raw_input);

        g_messages.push_back({ "User", raw_input });
        g_messages.push_back({ "AOS", reply });
        save_chat_aos(g_session_file);
    }

    save_chat_aos(g_session_file);
    if (g_ctx) llama_free(g_ctx);
    if (g_model) llama_model_free(g_model);
    llama_backend_free();
    return 0;
}
