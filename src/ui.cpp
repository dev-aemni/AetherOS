#include "ui.h"
#include <iostream>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#endif

void ui_init() {
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

void ui_banner(const std::string& model, float temp, const std::string& session_file) {
    std::cout << "\n" << C_RED << C_BOLD;
    std::cout << "  █████╗ ███████╗████████╗██╗  ██╗███████╗██████╗ \n";
    std::cout << " ██╔══██╗██╔════╝╚══██╔══╝██║  ██║██╔════╝██╔══██╗\n";
    std::cout << " ███████║█████╗     ██║   ███████║█████╗  ██████╔╝\n";
    std::cout << " ██╔══██║██╔══╝     ██║   ██╔══██║██╔══╝  ██╔══██╗\n";
    std::cout << " ██║  ██║███████╗   ██║   ██║  ██║███████╗██║  ██║\n";
    std::cout << " ╚═╝  ╚═╝╚══════╝   ╚═╝   ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝\n";
    std::cout << C_RESET;
    std::cout << C_WHITE << C_BOLD << "   A E T H E R - O S " << C_RESET 
              << C_MUTED << " // v7.2 Neural Console (by Aemni Acc · @dev-aemni)\n";
    std::cout << C_LINE << " ──────────────────────────────────────────────────────────\n" << C_RESET;
    std::cout << C_MUTED << "  Session: data/chats/" << session_file << " | Type /help\n\n" << C_RESET;
}

void ui_help() {
    std::cout << C_LINE << "╭─ " << C_RED << "AetherOS v7.2 Interactive Commands" << C_LINE << " ─────────────────────────\n" << C_RESET;
    std::cout << C_LINE << "│ " << C_WHITE << " /n, /new                   " << C_MUTED << "Start a fresh new conversation\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /sys <file|prompt>         " << C_MUTED << "Load prompt from file or set raw text\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /mem                       " << C_MUTED << "List custom memories (.mem.dat)\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /mem <name> <fact>         " << C_MUTED << "Save fact to data/memory/<id>-<name>.mem.dat\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /openrouter [key|model]    " << C_MUTED << "Configure OpenRouter or switch cloud model\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /agent [auto|confirm|off]  " << C_MUTED << "Configure autonomous shell execution\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /cmd <command>             " << C_MUTED << "Manually run shell command\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /model [name|#]            " << C_MUTED << "List or switch local GGUF models\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /token <num>               " << C_MUTED << "Set token limit (e.g. /token 1024)\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /clear                     " << C_MUTED << "Wipe conversation history\n";
    std::cout << C_LINE << "│ " << C_WHITE << " /q, exit, quit             " << C_MUTED << "Quit application immediately\n";
    std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n\n" << C_RESET;
}

void ui_stats(const std::string& model, const std::string& mode, int max_tokens, float temp, int msg_count, const std::string& sys) {
    std::cout << C_LINE << "╭─ " << C_RED << "Runtime Dashboard" << C_LINE << " ─────────────────────────────────────\n" << C_RESET;
    std::cout << C_LINE << "│ " << C_WHITE << "Model:         " << C_MUTED << model << "\n";
    std::cout << C_LINE << "│ " << C_WHITE << "Response Mode: " << C_RED << mode << C_MUTED << " (" << max_tokens << " max tokens)\n";
    std::cout << C_LINE << "│ " << C_WHITE << "Temperature:   " << C_MUTED << temp << "\n";
    std::cout << C_LINE << "│ " << C_WHITE << "Active Turns:  " << C_MUTED << msg_count << " messages\n";
    std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n\n" << C_RESET;
}

void ui_user_card(const std::string& prompt) {
    std::cout << C_LINE << "╭─ " << C_MUTED << "User ──────────────────────────────────────────\n" << C_RESET;
    std::cout << C_LINE << "│ " << C_WHITE << prompt << "\n" << C_RESET;
    std::cout << C_LINE << "╰──────────────────────────────────────────────────────────\n" << C_RESET;
}

void ui_assistant_header() {
    std::cout << "\n" << C_LINE << "╭─ " << C_RED << C_BOLD << "AetherOS " 
              << C_YELLOW << "✦ " << C_LINE << "───────────────────────────────────────\n" << C_RESET;
}

void ui_assistant_stream_piece(const std::string& piece) {
    for (char c : piece) {
        if (c == '\n') {
            std::cout << "\n" << C_LINE << "│ " << C_WHITE;
        } else {
            std::cout << C_WHITE << c << C_RESET;
        }
    }
    std::cout << std::flush;
}

void ui_assistant_footer(int tokens, double tps, double gen_sec, double think_sec, bool memory_saved) {
    std::string mem_badge = memory_saved ? (std::string(C_GREEN) + "● memory saved " + C_RESET + C_LINE + "│ ") : "";

    std::cout << "\n" << C_LINE << "╰── " << C_MUTED 
              << tokens << " tokens │ " 
              << std::fixed << std::setprecision(1) << tps << " T/s │ " 
              << std::setprecision(2) << gen_sec << "s │ "
              << C_RED << "⚡ " << std::setprecision(1) << think_sec << "s think │ "
              << mem_badge
              << C_LINE << "──────────────────────\n\n" << C_RESET;
}

void ui_start_thinking(std::atomic<bool>& is_thinking, std::atomic<bool>& stop_token, std::thread& spinner_thread) {
    is_thinking = true;
    auto t_start = std::chrono::steady_clock::now();

    spinner_thread = std::thread([t_start, &is_thinking, &stop_token]() {
        const char* frames[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
        int idx = 0;
        while (is_thinking && !stop_token) {
            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - t_start).count();
            std::cout << "\r" << C_LINE << "│ " << C_RED << frames[idx] << " " 
                      << C_YELLOW << "✦ " << C_RED << "Synthesizing (" 
                      << std::fixed << std::setprecision(1) << elapsed << "s)..." << C_RESET << std::flush;
            idx = (idx + 1) % 10;
            std::this_thread::sleep_for(std::chrono::milliseconds(75));
        }
        std::cout << "\r" << C_LINE << "│ " << "\033[K" << std::flush;
    });
}

void ui_stop_thinking(std::atomic<bool>& is_thinking, std::thread& spinner_thread) {
    is_thinking = false;
    if (spinner_thread.joinable()) {
        spinner_thread.join();
    }
}

std::string ui_prompt_dock(const std::string& model, const std::string& mode, int max_tokens) {
    std::cout << C_LINE << "╭─ " << C_RED << "AetherOS" << C_MUTED << " · " << model 
              << " [" << mode << " · " << max_tokens << "t] " << C_LINE << "─────────────────────────\n" << C_RESET;
    return "\001\033[38;5;238m\002╰\001\033[1;38;5;196m\002❯ \001\033[0m\002";
}
