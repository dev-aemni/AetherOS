#pragma once
#include <string>
#include <atomic>
#include <thread>
#include <chrono>

#define C_RESET       "\033[0m"
#define C_BOLD        "\033[1m"
#define C_DIM         "\033[2m"
#define C_RED         "\033[38;5;196m"
#define C_AMBER       "\033[38;5;208m"
#define C_WHITE       "\033[38;5;254m"
#define C_MUTED       "\033[38;5;241m"
#define C_LINE        "\033[38;5;238m"
#define C_CYAN        "\033[38;5;45m"
#define C_GREEN       "\033[38;5;48m"  // Memory Glow Green
#define C_YELLOW      "\033[38;5;220m"

void ui_init();
void ui_banner(const std::string& model, float temp, const std::string& session_file);
void ui_help();
void ui_stats(const std::string& model, const std::string& mode, int max_tokens, float temp, int msg_count, const std::string& sys);
void ui_user_card(const std::string& prompt);
void ui_assistant_header();
void ui_assistant_stream_piece(const std::string& piece);
void ui_assistant_footer(int tokens, double tps, double gen_sec, double think_sec, bool memory_saved);
void ui_start_thinking(std::atomic<bool>& is_thinking, std::atomic<bool>& stop_token, std::thread& spinner_thread);
void ui_stop_thinking(std::atomic<bool>& is_thinking, std::thread& spinner_thread);
std::string ui_prompt_dock(const std::string& model, const std::string& mode, int max_tokens);
