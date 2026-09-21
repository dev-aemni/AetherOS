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
"You have direct terminal execution authority. You inspect projects, run shell commands, create files, compile programs, and fix errors automatically. "
"When you need to run a terminal command or inspect files, output the command inside a standard markdown code block: \n"
"```bash\n"
"<command here>\n"
"```\n"
"The host environment will execute your command and return the terminal output to you. "
"Always inspect command outputs and take the next necessary step. Keep conversational commentary sharp and brief.";

std::string g_model_path = "";
float g_temperature = 0.6f;
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
        fs::path p = fs::path(home) / "llama";
        if (fs::exists(p)) return p;
    }
    return fs::current_path();
#else
    return fs::current_path();
#endif
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

void print_ram_dashboard() {
    double total_ram_gb = 0.0, avail_ram_gb = 0.0, process_rss_mb = 0.0;

#if defined(__linux__) || defined(__ANDROID__)
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.rfind("MemTotal:", 0) == 0) total_ram_gb = std::stol(line.substr(9)) / (1024.0 * 1024.0);
        else if (line.rfind("MemAvailable:", 0) == 0) avail_ram_gb = std::stol(line.substr(13)) / (1024.0 * 1024.0);
    }
    std::ifstream status("/proc/self/status");
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) process_rss_mb = std::stol(line.substr(6)) / 1024.0;
    }
#elif defined(_WIN32)
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    GlobalMemoryStatusEx(&memInfo);
    total_ram_gb = memInfo.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    avail_ram_gb = memInfo.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);

    PROCESS_MEMORY_COUNTERS pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    process_rss_mb = pmc.WorkingSetSize / (1024.0 * 1024.0);
#endif

    double model_gb = 0.0;
    if (fs::exists(g_model_path)) model_gb = fs::file_size(g_model_path) / (1024.0 * 1024.0 * 1024.0);

    std::cout << C_LINE << "\n╭─ " << C_RED << "AetherOS Hardware & Memory Dashboard" << C_LINE << " ──────────────────────\n" << C_RESET;
    std::cout << C_LINE << "│ " << C_WHITE << "Total Device RAM:     " << C_MUTED << std::fixed << std::setprecision(2) << total_ram_gb << " GB\n";
    std::cout << C_LINE << "│ " << C_WHITE << "Available/Free RAM:   " << C_GREEN << avail_ram_gb << " GB\n";
    std::cout << C_LINE << "│ " << C_WHITE << "GGUF File Size:       " << C_RED << model_gb << " GB " << C_MUTED << "(Mmap/Disk)\n";
    std::cout << C_LINE << "│ " << C_WHITE << "AOS Process Resident: " << C_CYAN << process_rss_mb << " MB " << C_MUTED << "(Model + Context Cache)\n";
    std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n\n" << C_RESET;
}

std::vector<std::string> get_model_list() {
    std::vector<std::string> list;
    fs::path mdir = get_app_dir() / "model";
    if (fs::exists(mdir)) {
        for (const auto& e : fs::directory_iterator(mdir)) {
            if (e.is_regular_file() && e.path().extension() == ".gguf") list.push_back(e.path().string());
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

void save_chat_aos(const std::string& filename) {
    if (g_messages.empty()) return;
    fs::path base = get_app_dir() / "chats";
    fs::create_directories(base);

    std::string fname = filename;
    if (fname.empty()) fname = get_exact_datetime() + ".aos";
    if (fname.rfind(".aos") == std::string::npos) fname += ".aos";

    fs::path target = (fname.find("chats/") != std::string::npos) ? (get_app_dir() / fname) : (base / fname);

    std::ofstream f(target);
    if (!f.is_open()) return;
    f << "AOS_CHAT_V1\n";
    for (const auto& m : g_messages) {
        f << m.role << ":::" << m.text << "\n---MSG_END---\n";
    }
}

void load_chat_aos(const std::string& filename) {
    fs::path target = (filename.find("chats/") != std::string::npos) ? (get_app_dir() / filename) : (get_app_dir() / "chats" / filename);
    if (target.extension() != ".aos") target += ".aos";

    std::ifstream f(target);
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

std::string get_latest_chat_file() {
    fs::path chats_dir = get_app_dir() / "chats";
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
            std::cout << "\r" << C_LINE << "│ " << C_RED << frames[idx] << " Cloud Agent (" 
                      << std::fixed << std::setprecision(1) << el << "s)..." << C_RESET << std::flush;
            idx = (idx + 1) % 10;
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
        }
        std::cout << "\r" << C_LINE << "│ " << "\033[K" << std::flush;
    });

    std::string escaped_prompt = "";
    for (char c : user_prompt) {
        if (c == '"') escaped_prompt += "\\\"";
        else if (c == '\n') escaped_prompt += "\\n";
        else escaped_prompt += c;
    }

    std::string payload = "{\"model\":\"" + g_openrouter_model + "\",\"messages\":[{\"role\":\"system\",\"content\":\"" + g_system_prompt + "\"},{\"role\":\"user\",\"content\":\"" + escaped_prompt + "\"}]}";
    std::string cmd = "curl -s -X POST https://openrouter.ai/api/v1/chat/completions "
                      "-H \"Authorization: Bearer " + g_openrouter_key + "\" "
                      "-H \"Content-Type: application/json\" "
                      "-d '" + payload + "'";

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

        size_t p = raw_json.find("\"content\":\"");
        if (p != std::string::npos) {
            size_t end = raw_json.find("\"", p + 11);
            if (end != std::string::npos) full_res = raw_json.substr(p + 11, end - (p + 11));
        } else {
            full_res = raw_json;
        }

        for (char c : full_res) {
            if (c == '\n') std::cout << "\n" << C_LINE << "│ " << C_WHITE;
            else std::cout << C_WHITE << c << C_RESET;
        }
    }

    double think_sec = std::chrono::duration<double>(t_think_end - t_start).count();
    std::cout << "\n" << C_LINE << "╰── " << C_MUTED << "[OpenRouter: " << g_openrouter_model << "] • "
              << "Thought for " << std::fixed << std::setprecision(1) << think_sec << "s "
              << C_LINE << "──────────────────────────────\n\n" << C_RESET;

    g_is_generating = false;
    return full_res;
}

// --- Inference Engine with Precise Thinking Time Metric ---
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
    if (g_response_mode == "short") mode_modifier = " [Instruction: Be extremely brief.]";
    else if (g_response_mode == "long") mode_modifier = " [Instruction: Provide an in-depth, thorough breakdown.]";

    std::string full_prompt = "<|system|>\n" + g_system_prompt + mode_modifier + "</s>\n<|user|>\n" + input_text + "</s>\n<|assistant|>\n";

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

    // Exact requested format: 9 tokens · 3.7 T/s · 2.44s • Thought for X.Xs
    std::cout << "\n" << C_LINE << "╰── " << C_MUTED 
              << count << " tokens · " 
              << std::fixed << std::setprecision(1) << tps << " T/s · " 
              << std::setprecision(2) << gen_sec << "s • "
              << "Thought for " << std::setprecision(1) << think_sec << "s "
              << C_LINE << "──────────────────────────────\n\n" << C_RESET;

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
              << C_MUTED << " // Autonomous Coding Agent (by Aemni Acc · @dev-aemni)\n";
    std::cout << C_LINE << " ──────────────────────────────────────────────────────────\n" << C_RESET;
    std::cout << C_MUTED << "  Saving: chats/" << g_session_file << " · Type /help for controls\n\n" << C_RESET;
}

void print_help() {
    std::cout << C_LINE << "╭─ " << C_RED << "AetherOS Agent Commands" << C_LINE << " ────────────────────────────────\n" << C_RESET;
    std::cout << C_LINE << "│ " << C_WHITE << " /agent auto           " << C_MUTED << "Run shell commands autonomously without prompt\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /agent confirm        " << C_MUTED << "Ask [Y/n] confirmation before running commands\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /agent off            " << C_MUTED << "Disable shell tool execution\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /cmd <command>        " << C_MUTED << "Manually run a shell command\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /ram                  " << C_MUTED << "Hardware RAM & GGUF memory metrics\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /openrouter [key|#]   " << C_MUTED << "Switch to OpenRouter cloud models\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /response <mode>      " << C_MUTED << "Mode: undefined | short | medium | long\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /model [name|#]       " << C_MUTED << "List or switch local GGUF models\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /q, /quit, /exit      " << C_MUTED << "Quit application\n";
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

    std::string one_shot_prompt = "";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-m" || arg == "--model") && i + 1 < argc) {
            g_model_path = argv[++i];
        } else if ((arg == "-s" || arg == "--sys") && i + 1 < argc) {
            g_system_prompt = argv[++i];
        } else if ((arg == "-t" || arg == "--temp") && i + 1 < argc) {
            g_temperature = std::stof(argv[++i]);
        } else if (arg == "--auto") {
            g_agent_mode = AGENT_AUTO;
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: aos [flags] [prompt]\n";
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

    // --- ONE-SHOT MODE ---
    if (!one_shot_prompt.empty()) {
        std::cout << C_LINE << "╭─ " << C_MUTED << "User ──────────────────────────────────────────\n" << C_RESET;
        std::cout << C_LINE << "│ " << C_WHITE << one_shot_prompt << "\n" << C_RESET;
        std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n" << C_RESET;
        std::cout << C_LINE << "╭─ " << C_RED << C_BOLD << "AetherOS ────────────────────────────────────────\n" << C_RESET;
        run_agent_loop(one_shot_prompt);

        llama_free(g_ctx);
        llama_model_free(g_model);
        llama_backend_free();
        return 0;
    }

    // --- INTERACTIVE REPL MODE ---
    std::string latest_session = get_latest_chat_file();
    if (!latest_session.empty()) {
        load_chat_aos(latest_session);
        g_session_file = latest_session;
    } else {
        g_session_file = get_exact_datetime() + ".aos";
    }

    print_banner();

    if (!g_messages.empty()) {
        std::cout << C_MUTED << "  [Restored " << g_messages.size() << " messages from chats/" << g_session_file << "]\n\n" << C_RESET;
    }

    std::string input;
    while (true) {
        std::string mode_str = (g_agent_mode == AGENT_AUTO) ? "auto" : ((g_agent_mode == AGENT_CONFIRM) ? "confirm" : "off");
        std::string target_label = g_use_openrouter ? ("Cloud: " + g_openrouter_model) : (fs::path(g_model_path).stem().string());
        if (target_label.length() > 24) target_label = target_label.substr(0, 21) + "...";

        std::cout << C_LINE << "╭─ " << C_RED << "AetherOS" << C_MUTED << " · " << target_label 
                  << " [agent:" << mode_str << "] " << C_LINE << "─────────────────────────\n" << C_RESET;
        std::cout << C_LINE << "╰" << C_BOLD << C_RED << "❯ " << C_WHITE;
        if (!std::getline(std::cin, input)) break;
        std::cout << C_RESET;

        if (input.empty()) continue;

        if (input == "/q" || input == "/quit" || input == "/exit") break;
        if (input == "/help") { print_help(); continue; }
        if (input == "/ram") { print_ram_dashboard(); continue; }

        if (input == "/agent auto") {
            g_agent_mode = AGENT_AUTO;
            std::cout << C_GREEN << "\n  [Autonomous Agent Mode: AUTO]\n\n" << C_RESET;
            continue;
        }
        if (input == "/agent confirm") {
            g_agent_mode = AGENT_CONFIRM;
            std::cout << C_GREEN << "\n  [Autonomous Agent Mode: CONFIRM]\n\n" << C_RESET;
            continue;
        }
        if (input == "/agent off") {
            g_agent_mode = AGENT_OFF;
            std::cout << C_YELLOW << "\n  [Agent Tool Execution Disabled]\n\n" << C_RESET;
            continue;
        }

        if (input.rfind("/cmd ", 0) == 0) {
            std::cout << C_LINE << "╭─ Manual Shell Command ───────────────────────────────────\n" << C_RESET;
            run_shell_command(input.substr(5));
            std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n\n" << C_RESET;
            continue;
        }

        if (input.rfind("/response ", 0) == 0) {
            std::string mode = input.substr(10);
            if (mode == "undefined" || mode == "default") { g_response_mode = "undefined"; g_max_tokens = 512; }
            else if (mode == "short") { g_response_mode = "short"; g_max_tokens = 128; }
            else if (mode == "medium") { g_response_mode = "medium"; g_max_tokens = 512; }
            else if (mode == "long") { g_response_mode = "long"; g_max_tokens = 1024; }
            std::cout << C_GREEN << "\n  [Response mode set to: " << g_response_mode << "]\n\n" << C_RESET;
            continue;
        }

        if (input == "/clear") {
            g_messages.clear();
            g_session_file = get_exact_datetime() + ".aos";
            std::cout << C_MUTED << "\n  Context cleared. New session: chats/" << g_session_file << "\n\n" << C_RESET;
            continue;
        }

        if (input == "/openrouter local") {
            g_use_openrouter = false;
            std::cout << C_GREEN << "\n  [Switched to local offline GGUF engine]\n\n" << C_RESET;
            continue;
        }
        if (input.rfind("/openrouter select ", 0) == 0) {
            std::string sel = input.substr(19);
            if (sel == "1") g_openrouter_model = "google/gemini-2.0-flash-exp:free";
            else if (sel == "2") g_openrouter_model = "meta-llama/llama-3.3-70b-instruct:free";
            else if (sel == "3") g_openrouter_model = "deepseek/deepseek-chat";
            else if (sel == "4") g_openrouter_model = "anthropic/claude-3.5-sonnet";
            else g_openrouter_model = sel;
            g_use_openrouter = true;
            std::cout << C_GREEN << "\n  [OpenRouter Active: " << g_openrouter_model << "]\n\n" << C_RESET;
            continue;
        }
        if (input.rfind("/openrouter ", 0) == 0) {
            g_openrouter_key = input.substr(12);
            g_use_openrouter = true;
            std::cout << C_GREEN << "\n  [OpenRouter API Key Saved! Agent active with " << g_openrouter_model << "]\n\n" << C_RESET;
            continue;
        }

        // --- Execute Agent Pipeline with DeepSeek/Claude Style Stats ---
        std::cout << "\n" << C_LINE << "╭─ " << C_RED << C_BOLD << "AetherOS " << C_LINE << "────────────────────────────────────────\n" << C_RESET;
        std::string reply = run_agent_loop(input);

        g_messages.push_back({ "User", input });
        g_messages.push_back({ "AOS", reply });
        save_chat_aos(g_session_file);
    }

    save_chat_aos(g_session_file);
    llama_free(g_ctx);
    llama_model_free(g_model);
    llama_backend_free();
    return 0;
}
