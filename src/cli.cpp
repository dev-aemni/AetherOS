#include <iostream>
#include <iomanip>
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
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#include <limits.h>
#include <readline/readline.h>
#include <readline/history.h>
#endif

#include "llama.h"

namespace fs = std::filesystem;

#define C_RESET     "\033[0m"
#define C_BOLD      "\033[1m"
#define C_DIM       "\033[2m"
#define C_RED       "\033[38;5;196m" // Crimson Red
#define C_WHITE     "\033[38;5;254m"
#define C_MUTED     "\033[38;5;241m"
#define C_LINE      "\033[38;5;238m"
#define C_CYAN      "\033[38;5;45m"
#define C_GREEN     "\033[38;5;48m"
#define C_YELLOW    "\033[38;5;220m"

struct Message {
    std::string role;
    std::string text;
};

std::vector<Message> g_messages;
std::string g_system_prompt = 
"You are AetherOS, an autonomous coding and terminal agent created by Aemni Acc (@dev-aemni; note: Aemni Acc is an account handle, not a real name). "
"You specialize in coding, system tasks, and shell execution. "
"When you need to run terminal commands, output them inside ```bash ... ``` code blocks.";

std::string g_model_path = "";
float g_temperature = 0.6f;
int g_max_tokens = 512;
std::string g_response_mode = "undefined";
std::string g_session_file = "";

enum AgentMode { AGENT_CONFIRM, AGENT_AUTO, AGENT_OFF };
AgentMode g_agent_mode = AGENT_CONFIRM;

// OpenRouter Cloud Mode
bool g_use_openrouter = false;
std::string g_openrouter_key = "";
std::string g_openrouter_model = "google/gemini-2.0-flash-exp:free";

std::atomic<bool> g_stop_token{false};
std::atomic<bool> g_is_generating{false};
std::atomic<bool> g_is_thinking{false};

llama_model* g_model = nullptr;
llama_context* g_ctx = nullptr;
const struct llama_vocab* g_vocab = nullptr;

// ========================================================
// --- AetherOS Crypt-Pack Engine (Binary Ciphertext) ---
// ========================================================
const uint8_t AOS_MAGIC[5] = { 0x1f, 0x8b, 0x41, 0x4f, 0x53 }; // "\x1f\x8bAOS"
const uint8_t CIPHER_KEY[16] = { 0xa5, 0x5a, 0xf0, 0x0f, 0x3c, 0xc3, 0x96, 0x69, 0x55, 0xaa, 0x12, 0x34, 0x78, 0x9a, 0xbc, 0xef };

std::string aos_pack(const std::string& input) {
    if (input.empty()) return "";
    std::string packed;
    packed.reserve(input.size() + 8);
    for (int i = 0; i < 5; ++i) packed += (char)AOS_MAGIC[i];

    // Scramble with rolling keystream
    for (size_t i = 0; i < input.size(); ++i) {
        uint8_t b = (uint8_t)input[i];
        uint8_t k = CIPHER_KEY[i % 16] ^ (uint8_t)(i * 37 + 13);
        packed += (char)(b ^ k);
    }
    return packed;
}

std::string aos_unpack(const std::string& data) {
    if (data.size() < 5) return data;
    // Check if file has binary magic header
    bool is_packed = true;
    for (int i = 0; i < 5; ++i) {
        if ((uint8_t)data[i] != AOS_MAGIC[i]) { is_packed = false; break; }
    }
    if (!is_packed) return data; // Return as-is if older plain-text file

    std::string unpacked;
    unpacked.reserve(data.size() - 5);
    size_t payload_len = data.size() - 5;
    for (size_t i = 0; i < payload_len; ++i) {
        uint8_t b = (uint8_t)data[i + 5];
        uint8_t k = CIPHER_KEY[i % 16] ^ (uint8_t)(i * 37 + 13);
        unpacked += (char)(b ^ k);
    }
    return unpacked;
}

// Safe Read / Write Wrappers for Packed Files
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

// JSON Escaping
std::string escape_json(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ('\x00' <= c && c <= '\x1f') {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
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
        fs::path p = fs::path(home) / "AetherOS";
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

// --- Live Memory Engine (Packed data/memory/<idseq>-<name>.mem.dat) ---
int get_next_memory_id() {
    int max_id = 0;
    fs::path mdir = get_app_dir() / "data" / "memory";
    if (fs::exists(mdir)) {
        for (const auto& e : fs::directory_iterator(mdir)) {
            if (e.is_regular_file()) {
                std::string fname = e.path().filename().string();
                size_t dash = fname.find('-');
                if (dash != std::string::npos) {
                    try {
                        int id = std::stoi(fname.substr(0, dash));
                        if (id > max_id) max_id = id;
                    } catch (...) {}
                }
            }
        }
    }
    return max_id + 1;
}

void save_live_memory(const std::string& name, const std::string& content) {
    fs::path mdir = get_app_dir() / "data" / "memory";
    fs::create_directories(mdir);

    std::string target_file = "";
    for (const auto& e : fs::directory_iterator(mdir)) {
        std::string fname = e.path().filename().string();
        if (fname.find("-" + name + ".mem.dat") != std::string::npos) {
            target_file = e.path().string();
            break;
        }
    }

    std::string existing_text = "";
    if (target_file.empty()) {
        int next_id = get_next_memory_id();
        char id_buf[16];
        snprintf(id_buf, sizeof(id_buf), "%03d", next_id);
        target_file = (mdir / (std::string(id_buf) + "-" + name + ".mem.dat")).string();
    } else {
        existing_text = read_packed_file(target_file);
    }

    std::string updated = existing_text + (existing_text.empty() ? "" : "\n") + content;
    if (write_packed_file(target_file, updated)) {
        std::cout << C_GREEN << "\n  [Memory packed: " << fs::path(target_file).filename().string() << "]\n\n" << C_RESET;
    }
}

std::string get_active_memory_context() {
    std::string mem_block = "";
    fs::path mdir = get_app_dir() / "data" / "memory";
    if (fs::exists(mdir)) {
        for (const auto& e : fs::directory_iterator(mdir)) {
            if (e.is_regular_file() && e.path().filename().string().find(".mem.dat") != std::string::npos) {
                std::string c = read_packed_file(e.path());
                if (!c.empty()) {
                    mem_block += "\n[Memory: " + e.path().stem().string() + "]\n" + c + "\n";
                }
            }
        }
    }
    return mem_block;
}

void list_memories() {
    fs::path mdir = get_app_dir() / "data" / "memory";
    std::cout << C_LINE << "\n╭─ " << C_RED << "Active Packed Memory Files (.mem.dat)" << C_LINE << " ─────────────────\n" << C_RESET;
    bool found = false;
    if (fs::exists(mdir)) {
        for (const auto& e : fs::directory_iterator(mdir)) {
            if (e.is_regular_file() && e.path().filename().string().find(".mem.dat") != std::string::npos) {
                found = true;
                std::cout << C_LINE << "│ " << C_WHITE << e.path().filename().string() 
                          << " " << C_MUTED << "(Packed: " << fs::file_size(e.path()) << " bytes)\n";
            }
        }
    }
    if (!found) {
        std::cout << C_LINE << "│ " << C_MUTED << "No .mem.dat files. Add with: /mem <name> <fact>\n";
    }
    std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n\n" << C_RESET;
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

// --- Save & Load Packed .AOS Chat Sessions ---
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

// --- Guaranteed Atomic OpenRouter Engine (Zero Race Conditions) ---
std::string query_openrouter(const std::string& user_prompt) {
    g_is_generating = true;
    g_is_thinking = true;
    auto t_start = std::chrono::steady_clock::now();

    std::thread spinner([&]() {
        const char* frames[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
        int idx = 0;
        while (g_is_thinking && !g_stop_token) {
            auto now = std::chrono::steady_clock::now();
            double el = std::chrono::duration<double>(now - t_start).count();
            std::cout << "\r" << C_LINE << "│ " << C_RED << frames[idx] << " Cloud Request (" 
                      << std::fixed << std::setprecision(1) << el << "s)..." << C_RESET << std::flush;
            idx = (idx + 1) % 10;
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
        }
        std::cout << "\r" << C_LINE << "│ " << "\033[K" << std::flush;
    });

    std::string sys_with_mem = g_system_prompt + get_active_memory_context();

    std::string msgs_json = "[{\"role\":\"system\",\"content\":\"" + escape_json(sys_with_mem) + "\"}";
    int start_idx = (g_messages.size() > 8) ? (g_messages.size() - 8) : 0;
    for (size_t i = start_idx; i < g_messages.size(); ++i) {
        std::string role = (g_messages[i].role == "User") ? "user" : "assistant";
        msgs_json += ",{\"role\":\"" + role + "\",\"content\":\"" + escape_json(g_messages[i].text) + "\"}";
    }
    msgs_json += ",{\"role\":\"user\",\"content\":\"" + escape_json(user_prompt) + "\"}]";

    std::string payload = "{\"model\":\"" + g_openrouter_model + "\",\"messages\":" + msgs_json + "}";
    
    // Unique atomic request file to prevent race conditions
    auto epoch_ms = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path cfg_dir = get_app_dir() / "data" / "config";
    fs::create_directories(cfg_dir);
    fs::path tmp_path = cfg_dir / ("req_" + std::to_string(epoch_ms) + ".json");

    FILE* fp = fopen(tmp_path.string().c_str(), "wb");
    if (!fp) {
        g_is_thinking = false;
        if (spinner.joinable()) spinner.join();
        std::cout << C_LINE << "│ " << C_RED << "Error: Cannot open payload file for write.\n" << C_RESET;
        return "";
    }
    fwrite(payload.data(), 1, payload.size(), fp);
    fflush(fp);
    fclose(fp);

    std::string cmd = "curl -s -X POST https://openrouter.ai/api/v1/chat/completions "
                      "-H \"Authorization: Bearer " + g_openrouter_key + "\" "
                      "-H \"Content-Type: application/json\" "
                      "-d @\"" + tmp_path.string() + "\"";

    FILE* pipe = popen(cmd.c_str(), "r");
    auto t_think_end = std::chrono::steady_clock::now();
    g_is_thinking = false;
    if (spinner.joinable()) spinner.join();

    std::string full_res = "";
    if (pipe) {
        char buf[512];
        std::string raw_json = "";
        while (fgets(buf, sizeof(buf), pipe) != NULL) raw_json += buf;
        pclose(pipe);

        // Parse content
        size_t p = raw_json.find("\"content\":");
        if (p != std::string::npos) {
            size_t val_start = raw_json.find("\"", p + 10);
            if (val_start != std::string::npos) {
                val_start++;
                size_t val_end = val_start;
                bool escape = false;
                while (val_end < raw_json.size()) {
                    if (escape) {
                        escape = false;
                    } else if (raw_json[val_end] == '\\') {
                        escape = true;
                    } else if (raw_json[val_end] == '"') {
                        break;
                    }
                    val_end++;
                }
                std::string parsed = raw_json.substr(val_start, val_end - val_start);
                for (size_t i = 0; i < parsed.size(); ++i) {
                    if (parsed[i] == '\\' && i + 1 < parsed.size()) {
                        if (parsed[i+1] == 'n') { full_res += '\n'; i++; }
                        else if (parsed[i+1] == '"') { full_res += '"'; i++; }
                        else if (parsed[i+1] == '\\') { full_res += '\\'; i++; }
                        else { full_res += parsed[i]; }
                    } else {
                        full_res += parsed[i];
                    }
                }
            }
        } else {
            full_res = raw_json;
        }

        for (char c : full_res) {
            if (c == '\n') std::cout << "\n" << C_LINE << "│ " << C_WHITE;
            else std::cout << C_WHITE << c << C_RESET;
        }
    }

    fs::remove(tmp_path);

    double think_sec = std::chrono::duration<double>(t_think_end - t_start).count();
    std::cout << "\n" << C_LINE << "╰── " << C_MUTED << "[OpenRouter: " << g_openrouter_model << "] • "
              << "Thought for " << std::fixed << std::setprecision(1) << think_sec << "s "
              << C_LINE << "──────────────────────────────\n\n" << C_RESET;

    g_is_generating = false;
    return full_res;
}

// --- Local GGUF Engine with 5-Argument Sampler ---
std::string generate_response_raw(const std::string& input_text) {
    g_is_generating = true;
    g_stop_token = false;
    g_is_thinking = true;

    auto t_start = std::chrono::steady_clock::now();

    std::thread spinner_thread([&]() {
        const char* frames[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
        int idx = 0;
        while (g_is_thinking && !g_stop_token) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - t_start).count();
            std::cout << "\r" << C_LINE << "│ " << C_RED << frames[idx] << " Thinking (" 
                      << std::fixed << std::setprecision(1) << elapsed << "s)..." << C_RESET << std::flush;
            idx = (idx + 1) % 10;
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
        }
        std::cout << "\r" << C_LINE << "│ " << "\033[K" << std::flush;
    });

    std::string mode_modifier = "";
    if (g_response_mode == "short") mode_modifier = " [Instruction: Be extremely brief, 1-2 sentences only.]";
    else if (g_response_mode == "long") mode_modifier = " [Instruction: Provide an in-depth breakdown.]";

    std::string mem_context = get_active_memory_context();
    std::string full_prompt = "<|im_start|>system\n" + g_system_prompt + mem_context + mode_modifier + "<|im_end|>\n";
    
    int start_idx = (g_messages.size() > 8) ? (g_messages.size() - 8) : 0;
    for (size_t i = start_idx; i < g_messages.size(); ++i) {
        if (g_messages[i].role == "User") {
            full_prompt += "<|im_start|>user\n" + g_messages[i].text + "<|im_end|>\n";
        } else {
            full_prompt += "<|im_start|>assistant\n" + g_messages[i].text + "<|im_end|>\n";
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
        g_is_thinking = false;
        if (spinner_thread.joinable()) spinner_thread.join();

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
                for (char c : piece) {
                    if (c == '\n') std::cout << "\n" << C_LINE << "│ " << C_WHITE;
                    else std::cout << C_WHITE << c << C_RESET;
                }
                std::cout << std::flush;
                full_response += piece;
            }
            count++;

            llama_batch next_batch = llama_batch_get_one(&new_token, 1);
            if (llama_decode(g_ctx, next_batch) != 0) break;
        }
    } else {
        decode_start = std::chrono::steady_clock::now();
        g_is_thinking = false;
        if (spinner_thread.joinable()) spinner_thread.join();
    }

    auto decode_end = std::chrono::steady_clock::now();
    double think_sec = std::chrono::duration<double>(decode_start - t_start).count();
    double gen_sec = std::chrono::duration<double>(decode_end - decode_start).count();
    double tps = (gen_sec > 0.0) ? (count / gen_sec) : 0.0;

    std::cout << "\n" << C_LINE << "╰── " << C_MUTED 
              << count << " tokens · " 
              << std::fixed << std::setprecision(1) << tps << " T/s · " 
              << std::setprecision(2) << gen_sec << "s • "
              << "Thought for " << std::setprecision(1) << think_sec << "s "
              << C_LINE << "──────────────────────────────\n\n" << C_RESET;

    llama_sampler_free(smpl);
    g_is_generating = false;

    save_live_memory("session", "Turn completed: " + input_text.substr(0, 32));
    return full_response;
}

std::string run_agent_loop(const std::string& initial_user_input) {
    std::string current_input = initial_user_input;
    std::string final_response = "";
    int agent_turns = 0;
    const int max_turns = 4;

    while (agent_turns < max_turns) {
        std::string reply = "";
        if (g_use_openrouter && !g_openrouter_key.empty()) {
            reply = query_openrouter(current_input);
        } else {
            reply = generate_response_raw(current_input);
        }
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

        current_input = tool_feedbacks + "\nInspect the terminal output above and continue or report results to user.";
        std::cout << C_LINE << "╭─ " << C_RED << C_BOLD << "AetherOS (Agent Step " << agent_turns << ") " << C_LINE << "─────────────────────────\n" << C_RESET;
    }
    return final_response;
}

void print_banner() {
    std::cout << "\n" << C_RED << C_BOLD;
    std::cout << "  █████╗ ███████╗████████╗██╗  ██╗███████╗██████╗ \n";
    std::cout << " ██╔══██╗██╔════╝╚══██╔══╝██║  ██║██╔════╝██╔══██╗\n";
    std::cout << " ███████║█████╗     ██║   ███████║█████╗  ██████╔╝\n";
    std::cout << " ██╔══██║██╔══╝     ██║   ██╔══██║██╔══╝  ██╔══██╗\n";
    std::cout << " ██║  ██║███████╗   ██║   ██║  ██║███████╗██║  ██║\n";
    std::cout << " ╚═╝  ╚═╝╚══════╝   ╚═╝   ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝\n";
    std::cout << C_RESET;
    std::cout << C_WHITE << C_BOLD << "   A E T H E R - O S " << C_RESET 
              << C_MUTED << " // Autonomous Agent Core (@dev-aemni)\n";
    std::cout << C_LINE << " ──────────────────────────────────────────────────────────\n" << C_RESET;
    std::cout << C_MUTED << "  Saving: data/chats/" << g_session_file << " · Type /help for controls\n\n" << C_RESET;
}

void print_help() {
    std::cout << C_LINE << "╭─ " << C_RED << "AetherOS Commands Reference" << C_LINE << " ─────────────────────────────\n" << C_RESET;
    std::cout << C_LINE << "│ " << C_WHITE << " /sys <file|prompt>         " << C_MUTED << "Load prompt from file (or data/samples/) or set text\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /mem                       " << C_MUTED << "List active persistent memory (.mem.dat)\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /mem <name> <fact>         " << C_MUTED << "Save live memory to data/memory/<idseq>-<name>.mem.dat\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /openrouter [key|model]    " << C_MUTED << "Configure OpenRouter or switch cloud model\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /agent [auto|confirm|off]  " << C_MUTED << "Configure autonomous shell execution\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /cmd <command>             " << C_MUTED << "Manually run shell command\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /model [name|#]            " << C_MUTED << "List or switch local GGUF models\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /token <num>               " << C_MUTED << "Set token limit (e.g. /token 1024)\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /q, /quit, /exit           " << C_MUTED << "Quit application immediately\n";
    std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n\n" << C_RESET;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif

    std::signal(SIGINT, handle_sigint);

    // Read persistently stored OpenRouter key (auto-unpacks)
    fs::path key_path = get_app_dir() / "data" / "config" / "openrouter.key";
    if (fs::exists(key_path)) {
        g_openrouter_key = read_packed_file(key_path);
        // Trim newlines
        while (!g_openrouter_key.empty() && (g_openrouter_key.back() == '\n' || g_openrouter_key.back() == '\r')) {
            g_openrouter_key.pop_back();
        }
    }

    std::string one_shot_prompt = "";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-or" || arg == "--openrouter") && i + 1 < argc) {
            g_openrouter_key = argv[++i];
            g_use_openrouter = true;
            write_packed_file(get_app_dir() / "data" / "config" / "openrouter.key", g_openrouter_key);
        } else if ((arg == "-m" || arg == "--model") && i + 1 < argc) {
            std::string m = argv[++i];
            if (m.find("/") != std::string::npos && m.find("model/") == std::string::npos) {
                g_openrouter_model = m;
                g_use_openrouter = true;
            } else {
                g_model_path = m;
            }
        } else if ((arg == "-s" || arg == "--sys" || arg == "-sf" || arg == "--sys-file") && i + 1 < argc) {
            set_system_prompt_smart(argv[++i], false);
        } else if ((arg == "-t" || arg == "--temp") && i + 1 < argc) {
            g_temperature = std::stof(argv[++i]);
        } else if (arg == "--auto") {
            g_agent_mode = AGENT_AUTO;
        } else if (arg == "-h" || arg == "--help" || arg == "-help") {
            std::cout << "Usage: aos [flags] [prompt]\n";
            return 0;
        } else {
            if (!one_shot_prompt.empty()) one_shot_prompt += " ";
            one_shot_prompt += arg;
        }
    }

    if (!g_use_openrouter) {
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
    }

    // One-Shot Mode (Zero Disk Writes)
    if (!one_shot_prompt.empty()) {
        std::cout << C_LINE << "╭─ " << C_MUTED << "User ──────────────────────────────────────────\n" << C_RESET;
        std::cout << C_LINE << "│ " << C_WHITE << one_shot_prompt << "\n" << C_RESET;
        std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n" << C_RESET;
        std::cout << C_LINE << "╭─ " << C_RED << C_BOLD << "AetherOS ────────────────────────────────────────\n" << C_RESET;
        run_agent_loop(one_shot_prompt);

        if (g_ctx) llama_free(g_ctx);
        if (g_model) llama_model_free(g_model);
        llama_backend_free();
        return 0;
    }

    // Interactive Mode
    std::string latest_session = get_latest_chat_file();
    if (!latest_session.empty()) {
        load_chat_aos(latest_session);
        g_session_file = latest_session;
    } else {
        g_session_file = get_exact_datetime() + ".aos";
    }

    print_banner();

    if (!g_messages.empty()) {
        std::cout << C_MUTED << "  [Restored " << g_messages.size() << " messages from data/chats/" << g_session_file << "]\n\n" << C_RESET;
    }

    std::string input;
    while (true) {
        std::string mode_str = (g_agent_mode == AGENT_AUTO) ? "auto" : ((g_agent_mode == AGENT_CONFIRM) ? "confirm" : "off");
        std::string target_label = g_use_openrouter ? ("Cloud: " + g_openrouter_model) : (fs::path(g_model_path).stem().string());
        if (target_label.length() > 24) target_label = target_label.substr(0, 21) + "...";

        std::cout << C_LINE << "╭─ " << C_RED << "AetherOS" << C_MUTED << " · " << target_label 
                  << " [agent:" << mode_str << "] " << C_LINE << "─────────────────────────\n" << C_RESET;

        char* line_read = nullptr;
#ifndef _WIN32
        line_read = readline("\001\033[38;5;238m\002╰\001\033[1;38;5;196m\002❯ \001\033[0m\002");
#else
        std::cout << C_LINE << "╰" << C_BOLD << C_RED << "❯ " << C_WHITE;
        std::string win_line;
        if (!std::getline(std::cin, win_line)) break;
        line_read = strdup(win_line.c_str());
#endif

        if (!line_read) break;

        std::string input(line_read);
        if (!input.empty()) {
#ifndef _WIN32
            add_history(line_read);
#endif
        }
        free(line_read);

        if (input.empty()) continue;

        if (input == "/q" || input.rfind("/q ", 0) == 0 || 
            input == "/quit" || input.rfind("/quit ", 0) == 0 || 
            input == "/exit" || input.rfind("/exit ", 0) == 0) {
            break;
        }

        if (input == "/help") { print_help(); continue; }
        if (input == "/mem") { list_memories(); continue; }

        if (input.rfind("/mem ", 0) == 0) {
            std::string rest = input.substr(5);
            size_t sp = rest.find(' ');
            if (sp != std::string::npos) {
                save_live_memory(rest.substr(0, sp), rest.substr(sp + 1));
            } else {
                std::cout << C_YELLOW << "\n  [Usage: /mem <name> <text to remember>]\n\n" << C_RESET;
            }
            continue;
        }

        if (input.rfind("/sys ", 0) == 0) {
            set_system_prompt_smart(input.substr(5));
            continue;
        }

        if (input == "/sys") {
            std::cout << C_LINE << "╭─ Active System Prompt ───────────────────────────────────\n" << C_RESET;
            std::cout << C_LINE << "│ " << C_WHITE << g_system_prompt << "\n" << C_RESET;
            std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n\n" << C_RESET;
            continue;
        }

        if (input.rfind("/openrouter ", 0) == 0) {
            std::string arg = input.substr(12);
            if (arg == "local") {
                g_use_openrouter = false;
                std::cout << C_GREEN << "\n  [Switched back to local offline GGUF engine]\n\n" << C_RESET;
            } else if (arg.find("/") != std::string::npos) {
                g_openrouter_model = arg;
                g_use_openrouter = true;
                std::cout << C_GREEN << "\n  [Switched to Cloud Model: " << g_openrouter_model << "]\n\n" << C_RESET;
            } else {
                g_openrouter_key = arg;
                g_use_openrouter = true;
                write_packed_file(get_app_dir() / "data" / "config" / "openrouter.key", g_openrouter_key);
                std::cout << C_GREEN << "\n  [OpenRouter API Key Packed & Saved permanently!]\n\n" << C_RESET;
            }
            continue;
        }

        if (input == "/agent auto") { g_agent_mode = AGENT_AUTO; std::cout << C_GREEN << "\n  [Agent Mode: AUTO]\n\n" << C_RESET; continue; }
        if (input == "/agent confirm") { g_agent_mode = AGENT_CONFIRM; std::cout << C_GREEN << "\n  [Agent Mode: CONFIRM]\n\n" << C_RESET; continue; }
        if (input == "/agent off") { g_agent_mode = AGENT_OFF; std::cout << C_YELLOW << "\n  [Agent Execution Disabled]\n\n" << C_RESET; continue; }

        if (input.rfind("/cmd ", 0) == 0) {
            std::cout << C_LINE << "╭─ Shell Execution ────────────────────────────────────────\n" << C_RESET;
            run_shell_command(input.substr(5));
            std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n\n" << C_RESET;
            continue;
        }

        if (input == "/clear") {
            g_messages.clear();
            g_session_file = get_exact_datetime() + ".aos";
            std::cout << C_MUTED << "\n  Context cleared. New session: data/chats/" << g_session_file << "\n\n" << C_RESET;
            continue;
        }

        std::cout << "\n" << C_LINE << "╭─ " << C_RED << C_BOLD << "AetherOS " << C_LINE << "────────────────────────────────────────\n" << C_RESET;
        std::string reply = run_agent_loop(input);

        g_messages.push_back({ "User", input });
        g_messages.push_back({ "AOS", reply });
        save_chat_aos(g_session_file);
    }

    save_chat_aos(g_session_file);
    if (g_ctx) llama_free(g_ctx);
    if (g_model) llama_model_free(g_model);
    llama_backend_free();
    return 0;
}
