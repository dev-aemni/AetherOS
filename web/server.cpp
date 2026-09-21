#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#endif

#include "httplib.h"
#include "llama.h"

namespace fs = std::filesystem;

std::mutex g_llm_mutex;
std::string g_system_prompt = "You are AetherOS, an elite offline neural assistant by @dev-aemni. Provide direct, structured, and helpful answers.";
std::string g_current_model_path = "";
float g_temperature = 0.7f;
std::atomic<bool> g_stop_generation{false};

llama_model* g_model = nullptr;
llama_context* g_ctx = nullptr;
const struct llama_vocab* g_vocab = nullptr;

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

bool load_model(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_llm_mutex);
    if (g_ctx) { llama_free(g_ctx); g_ctx = nullptr; }
    if (g_model) { llama_model_free(g_model); g_model = nullptr; }

    llama_model_params mparams = llama_model_default_params();
    g_model = llama_model_load_from_file(path.c_str(), mparams);
    if (!g_model) return false;

    g_vocab = llama_model_get_vocab(g_model);
    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = 1024;
    cparams.n_threads = 4;
    cparams.n_threads_batch = 4;

    g_ctx = llama_init_from_model(g_model, cparams);
    g_current_model_path = path;
    return (g_ctx != nullptr);
}

std::vector<std::string> get_models() {
    std::vector<std::string> list;
    fs::path mdir = get_app_dir() / "model";
    if (fs::exists(mdir)) {
        for (const auto& e : fs::directory_iterator(mdir)) {
            if (e.is_regular_file() && e.path().extension() == ".gguf") {
                list.push_back(e.path().string());
            }
        }
    }
    return list;
}

// AetherOS Web Interface (Single-Solid Amber Accent + Shimmer Thinking State)
const char* HTML_UI = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover">
<title>AetherOS // Neural Console</title>
<style>
  :root {
    --bg: #09090b;
    --surface: #121215;
    --border: #24242a;
    --amber: #D97706; /* Signature AetherOS Amber */
    --amber-hover: #b45309;
    --text: #f4f4f5;
    --muted: #71717a;
    --code-bg: #070709;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Inter", "Segoe UI", Roboto, sans-serif; }

  html, body {
    height: 100%;
    height: 100dvh;
    background: var(--bg);
    color: var(--text);
    overflow: hidden;
  }

  #layout {
    display: flex;
    flex-direction: column;
    height: 100%;
    height: 100dvh;
    width: 100vw;
    position: fixed;
    top: 0; left: 0;
  }

  /* Minimal Top Bar */
  header {
    flex: 0 0 46px;
    background: var(--surface);
    border-bottom: 1px solid var(--border);
    padding: 0 16px;
    display: flex;
    justify-content: space-between;
    align-items: center;
    z-index: 20;
  }
  .brand { display: flex; align-items: center; gap: 10px; }
  .menu-btn { background: transparent; border: none; color: var(--amber); font-size: 1.25rem; cursor: pointer; }
  .brand h1 { font-size: 0.95rem; font-weight: 700; color: var(--text); letter-spacing: 0.5px; }
  .brand span { color: var(--amber); }
  #status-pill { font-size: 0.75rem; color: var(--muted); }

  /* Feed Area (Responds on top) */
  #chat-feed {
    flex: 1 1 auto;
    min-height: 0;
    overflow-y: auto;
    padding: 16px 16px 90px 16px;
    display: flex;
    flex-direction: column;
    gap: 16px;
    -webkit-overflow-scrolling: touch;
  }

  .entry { display: flex; flex-direction: column; gap: 4px; }
  .tag { font-size: 0.75rem; font-weight: 700; letter-spacing: 0.5px; }
  .tag.user { color: var(--muted); align-self: flex-end; }
  .tag.bot { color: var(--amber); }

  .bubble {
    max-width: 90%;
    padding: 12px 14px;
    border-radius: 6px;
    font-size: 0.925rem;
    line-height: 1.6;
    word-break: break-word;
  }
  .bubble.user {
    align-self: flex-end;
    background: #18181c;
    border: 1px solid var(--border);
    color: var(--text);
  }
  .bubble.bot {
    align-self: flex-start;
    background: transparent;
    padding-left: 0;
  }

  /* Animated Thinking State */
  .thinking-indicator {
    display: inline-flex;
    align-items: center;
    gap: 8px;
    background: #171410;
    border: 1px solid #3d2b19;
    color: var(--amber);
    padding: 6px 12px;
    border-radius: 6px;
    font-size: 0.8rem;
    font-family: monospace;
    animation: pulse 1.5s infinite ease-in-out;
  }
  @keyframes pulse { 0%, 100% { opacity: 0.7; } 50% { opacity: 1; } }

  /* Code Blocks with Language & Copy */
  .code-block {
    background: var(--code-bg);
    border: 1px solid var(--border);
    border-radius: 6px;
    margin: 8px 0;
    overflow: hidden;
  }
  .code-top {
    background: #111115;
    border-bottom: 1px solid var(--border);
    padding: 4px 10px;
    display: flex;
    justify-content: space-between;
    font-size: 0.7rem;
    color: var(--muted);
  }
  pre { padding: 10px; overflow-x: auto; margin: 0; font-family: monospace; font-size: 0.85rem; color: #fbbf24; }
  .copy-btn { background: #18181c; border: 1px solid var(--border); color: #ccc; border-radius: 4px; padding: 2px 6px; cursor: pointer; font-size: 0.65rem; }

  /* Anchored Bottom Input Dock */
  .dock {
    position: fixed;
    bottom: 0; left: 0;
    width: 100%;
    padding: 10px 14px max(12px, env(safe-area-inset-bottom)) 14px;
    background: linear-gradient(to top, var(--bg) 85%, transparent);
    z-index: 30;
  }
  .dock-box {
    max-width: 780px;
    margin: 0 auto;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 6px 10px;
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .dock-box:focus-within { border-color: var(--amber); }
  input {
    flex: 1;
    background: transparent;
    border: none;
    color: #fff;
    font-size: 16px;
    outline: none;
  }
  .action-btn {
    width: 34px; height: 34px;
    border-radius: 6px;
    border: none;
    cursor: pointer;
    font-weight: bold;
    display: flex; align-items: center; justify-content: center;
  }
  .send { background: var(--amber); color: #000; }
  .stop { background: #dc2626; color: #fff; }

  /* Drawer Menu */
  #drawer {
    position: fixed;
    top: 0; left: -100%;
    width: 80%; max-width: 320px;
    height: 100%;
    background: var(--surface);
    border-right: 1px solid var(--border);
    z-index: 100;
    transition: left 0.2s ease;
    padding: 20px;
    display: flex;
    flex-direction: column;
    gap: 14px;
  }
  #drawer.open { left: 0; }
  #overlay { position: fixed; inset: 0; background: rgba(0,0,0,0.6); display: none; z-index: 90; }
  #overlay.open { display: block; }
  select, textarea {
    width: 100%; background: var(--bg); border: 1px solid var(--border); color: #fff;
    padding: 8px; border-radius: 6px; font-size: 0.85rem; outline: none;
  }
  .p-btn { height: 34px; background: #18181c; border: 1px solid var(--border); color: #fff; border-radius: 6px; cursor: pointer; }
</style>
</head>
<body>

<div id="layout">
  <header>
    <div class="brand">
      <button class="menu-btn" onclick="toggleDrawer()">☰</button>
      <h1>AETHER-OS <span>//</span> CORE</h1>
    </div>
    <span id="status-pill">Ready</span>
  </header>

  <div id="chat-feed"></div>

  <div class="dock">
    <div class="dock-box">
      <input type="text" id="prompt-in" placeholder="Message AetherOS..." autocomplete="off" />
      <button class="action-btn send" id="send-btn" onclick="sendPrompt()">▲</button>
      <button class="action-btn stop" id="stop-btn" style="display:none;" onclick="stopGen()">■</button>
    </div>
  </div>
</div>

<!-- Settings Drawer -->
<div id="overlay" onclick="toggleDrawer()"></div>
<div id="drawer">
  <div style="font-size:0.85rem; font-weight:700; color:var(--amber);">AETHER-OS DIRECTIVES</div>
  <div>
    <div style="font-size:0.75rem; color:var(--muted); margin-bottom:4px;">Active Model</div>
    <select id="model_select" onchange="changeModel(this.value)"></select>
  </div>
  <div>
    <div style="font-size:0.75rem; color:var(--muted); margin-bottom:4px;">System Directives (.aosb)</div>
    <textarea id="sys_prompt" rows="3"></textarea>
  </div>
  <button class="p-btn" onclick="saveConfig()">Save Directives</button>
  <button class="p-btn" style="background:#2a1515; color:#ef4444; margin-top:auto;" onclick="clearChat()">Reset Memory</button>
</div>

<script>
  let chatHistory = [];
  const STORAGE_KEY = 'AETHEROS_WEB_CHAT';
  const CONFIG_KEY = 'AETHEROS_WEB_SYS';
  let thinkingTimer = null;

  function parseMarkdown(t) {
    return t.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
      .replace(/```([a-zA-Z]*)\n?([\s\S]*?)```/g, (m, lang, code) => {
        const l = lang ? lang.toUpperCase() : "CODE";
        return `<div class="code-block"><div class="code-top"><span>${l}</span><button class="copy-btn" onclick="copyCode(this)">Copy</button></div><pre><code>${code.trim()}</code></pre></div>`;
      })
      .replace(/\*\*(.*?)\*\*/g, '<strong>$1</strong>')
      .replace(/\*(.*?)\*/g, '<em>$1</em>')
      .replace(/\n/g, '<br/>');
  }

  function copyCode(btn) {
    const code = btn.closest('.code-block').querySelector('code').innerText;
    navigator.clipboard.writeText(code);
    btn.innerText = 'Copied!';
    setTimeout(() => btn.innerText = 'Copy', 1500);
  }

  function renderFeed() {
    const feed = document.getElementById('chat-feed');
    feed.innerHTML = '';
    chatHistory.forEach(m => {
      const isUser = m.role === 'User';
      feed.innerHTML += `
        <div class="entry">
          <span class="tag ${isUser ? 'user' : 'bot'}">${isUser ? 'YOU' : 'AETHER-OS'}</span>
          <div class="bubble ${isUser ? 'user' : 'bot'}">${isUser ? m.text : parseMarkdown(m.text)}</div>
        </div>`;
    });
    feed.scrollTop = feed.scrollHeight;
  }

  function toggleDrawer() {
    document.getElementById('drawer').classList.toggle('open');
    document.getElementById('overlay').classList.toggle('open');
  }

  function saveConfig() {
    const val = document.getElementById('sys_prompt').value;
    localStorage.setItem(CONFIG_KEY, val);
    fetch('/api/config', { method: 'POST', body: JSON.stringify({ system_prompt: val }) });
    toggleDrawer();
  }

  function clearChat() {
    chatHistory = [];
    localStorage.removeItem(STORAGE_KEY);
    renderFeed();
    toggleDrawer();
  }

  fetch('/api/models').then(r => r.json()).then(models => {
    const sel = document.getElementById('model_select');
    sel.innerHTML = '';
    models.forEach(m => {
      const opt = document.createElement('option');
      opt.value = m;
      opt.innerText = m.split('/').pop();
      sel.appendChild(opt);
    });
  });

  function changeModel(m) {
    fetch('/api/set_model', { method: 'POST', body: m });
  }

  function sendPrompt() {
    const input = document.getElementById('prompt-in');
    const text = input.value.trim();
    if (!text) return;
    input.value = '';

    chatHistory.push({ role: 'User', text: text });
    renderFeed();

    // Live Animated Thinking Buffer
    const feed = document.getElementById('chat-feed');
    const thinkId = 'think-' + Date.now();
    let startTime = Date.now();
    feed.innerHTML += `
      <div class="entry" id="${thinkId}">
        <span class="tag bot">AETHER-OS</span>
        <div><span class="thinking-indicator">⠋ Thinking (<span class="sec">0.0</span>s)...</span></div>
      </div>`;
    feed.scrollTop = feed.scrollHeight;

    thinkingTimer = setInterval(() => {
      const el = document.getElementById(thinkId);
      if (el) {
        const sec = ((Date.now() - startTime) / 1000).toFixed(1);
        el.querySelector('.sec').innerText = sec;
      }
    }, 100);

    setGenerating(true);

    let firstToken = true;
    const evt = new EventSource(`/api/chat?prompt=${encodeURIComponent(text)}`);

    evt.onmessage = (e) => {
      if (firstToken) {
        firstToken = false;
        clearInterval(thinkingTimer);
        const thinkEl = document.getElementById(thinkId);
        if (thinkEl) thinkEl.remove();

        chatHistory.push({ role: 'AOS', text: '' });
      }
      chatHistory[chatHistory.length - 1].text += e.data;
      renderFeed();
    };

    evt.onerror = () => {
      evt.close();
      clearInterval(thinkingTimer);
      const thinkEl = document.getElementById(thinkId);
      if (thinkEl && firstToken) thinkEl.remove();

      localStorage.setItem(STORAGE_KEY, JSON.stringify(chatHistory));
      setGenerating(false);
    };
  }

  function stopGen() {
    fetch('/api/stop', { method: 'POST' });
  }

  function setGenerating(isGen) {
    document.getElementById('send-btn').style.display = isGen ? 'none' : 'flex';
    document.getElementById('stop-btn').style.display = isGen ? 'flex' : 'none';
    document.getElementById('status-pill').innerText = isGen ? 'Generating...' : 'Ready';
    document.getElementById('status-pill').style.color = isGen ? '#D97706' : '#71717a';
  }

  document.getElementById('prompt-in').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') sendPrompt();
  });

  window.addEventListener('DOMContentLoaded', () => {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) {
      try { chatHistory = JSON.parse(raw); } catch(e) {}
    }
    renderFeed();
    const sys = localStorage.getItem(CONFIG_KEY);
    if (sys) document.getElementById('sys_prompt').value = sys;
  });
</script>
</body>
</html>
)rawliteral";

int main() {
    llama_log_set([](enum ggml_log_level, const char*, void*) {}, nullptr);
    llama_backend_init();

    auto models = get_models();
    if (!models.empty()) {
        std::cout << "[INFO] Loading model: " << models[0] << std::endl;
        load_model(models[0]);
    }

    httplib::Server svr;

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(HTML_UI, "text/html");
    });

    svr.Get("/api/models", [](const httplib::Request&, httplib::Response& res) {
        auto list = get_models();
        std::string json = "[";
        for (size_t i = 0; i < list.size(); ++i) {
            json += "\"" + list[i] + "\"" + (i + 1 < list.size() ? "," : "");
        }
        json += "]";
        res.set_content(json, "application/json");
    });

    svr.Post("/api/set_model", [](const httplib::Request& req, httplib::Response& res) {
        if (load_model(req.body)) {
            res.set_content("OK", "text/plain");
        } else {
            res.status = 500;
        }
    });

    svr.Post("/api/config", [](const httplib::Request& req, httplib::Response& res) {
        size_t pos = req.body.find("\"system_prompt\":\"");
        if (pos != std::string::npos) {
            size_t end = req.body.find("\"", pos + 17);
            if (end != std::string::npos) {
                g_system_prompt = req.body.substr(pos + 17, end - (pos + 17));
            }
        }
        res.set_content("OK", "text/plain");
    });

    svr.Post("/api/stop", [](const httplib::Request&, httplib::Response& res) {
        g_stop_generation = true;
        res.set_content("Stopped", "text/plain");
    });

    svr.Get("/api/chat", [](const httplib::Request& req, httplib::Response& res) {
        if (!req.has_param("prompt")) return;
        std::string user_prompt = req.get_param_value("prompt");

        res.set_chunked_content_provider("text/event-stream", [user_prompt](size_t, httplib::DataSink& sink) {
            std::lock_guard<std::mutex> lock(g_llm_mutex);
            g_stop_generation = false;

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

            llama_batch batch = llama_batch_get_one(tokens.data(), tokens.size());
            if (llama_decode(g_ctx, batch) == 0) {
                for (int i = 0; i < 512; ++i) {
                    if (g_stop_generation) break;

                    llama_token new_token = llama_sampler_sample(smpl, g_ctx, -1);
                    if (llama_vocab_is_eog(g_vocab, new_token)) break;

                    char buf[64];
                    int n = llama_token_to_piece(g_vocab, new_token, buf, sizeof(buf), 0, true);
                    if (n > 0) {
                        std::string piece(buf, n);
                        std::string sse_chunk = "data: " + piece + "\n\n";
                        sink.write(sse_chunk.data(), sse_chunk.size());
                    }

                    llama_batch next_batch = llama_batch_get_one(&new_token, 1);
                    if (llama_decode(g_ctx, next_batch) != 0) break;
                }
            }

            llama_sampler_free(smpl);
            sink.done();
            return true;
        });
    });

    std::cout << "\n============================================\n";
    std::cout << "  AETHER-OS SERVER ACTIVE\n";
    std::cout << "  URL: http://localhost:8080\n";
    std::cout << "============================================\n";

    svr.listen("0.0.0.0", 8080);
    return 0;
}
