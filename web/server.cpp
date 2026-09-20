#include <iostream>
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "httplib.h"
#include "llama.h"

namespace fs = std::filesystem;

std::mutex g_llm_mutex;
std::string g_system_prompt = "You are AetherOS, an elite offline neural assistant. Provide sharp, structured, and helpful responses.";
std::string g_current_model_path = "";
float g_temperature = 0.7f;
std::atomic<bool> g_stop_generation{false};

llama_model* g_model = nullptr;
llama_context* g_ctx = nullptr;
const struct llama_vocab* g_vocab = nullptr;

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
    if (fs::exists("model")) {
        for (const auto& e : fs::directory_iterator("model")) {
            if (e.is_regular_file() && e.path().extension() == ".gguf") {
                list.push_back(e.path().string());
            }
        }
    }
    return list;
}

// Embedded Web UI with LocalStorage, Markdown, Code Copy, and .aos / .aosb support
const char* HTML_UI = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover">
<title>AetherOS // Neural Interface</title>
<style>
  :root {
    --bg: #0b0e14;
    --panel: #151921;
    --border: #232a36;
    --accent: #00ff88;
    --user-msg: #0284c7;
    --text: #e2e8f0;
    --code-bg: #07090d;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, monospace; }

  html, body {
    height: 100%;
    height: 100dvh;
    background: var(--bg);
    color: var(--text);
    overflow: hidden;
  }

  #app-root {
    display: flex;
    flex-direction: column;
    height: 100%;
    height: 100dvh;
    width: 100vw;
    position: fixed;
    top: 0; left: 0;
  }

  header {
    flex: 0 0 54px;
    background: var(--panel);
    border-bottom: 1px solid var(--border);
    padding: 0 16px;
    display: flex;
    justify-content: space-between;
    align-items: center;
    z-index: 10;
  }
  .hdr-left { display: flex; align-items: center; gap: 12px; }
  .menu-btn { background: transparent; border: none; color: var(--accent); font-size: 1.4rem; cursor: pointer; }
  header h1 { font-size: 1.05rem; color: var(--accent); letter-spacing: 1px; }
  #status { font-size: 0.8rem; color: #94a3b8; }

  #chat {
    flex: 1 1 auto;
    min-height: 0;
    overflow-y: auto;
    padding: 16px;
    display: flex;
    flex-direction: column;
    gap: 14px;
    -webkit-overflow-scrolling: touch;
  }

  .msg { max-width: 90%; padding: 12px 16px; border-radius: 8px; line-height: 1.5; font-size: 0.95rem; word-wrap: break-word; }
  .user { align-self: flex-end; background: var(--user-msg); color: #fff; }
  .bot { align-self: flex-start; background: var(--panel); border: 1px solid var(--border); color: var(--text); }
  .bot b { color: var(--accent); }

  /* Code Blocks with Copy Button */
  pre {
    background: var(--code-bg);
    border: 1px solid var(--border);
    border-radius: 6px;
    padding: 10px;
    margin: 8px 0;
    position: relative;
    overflow-x: auto;
  }
  code { font-family: "Fira Code", monospace; font-size: 0.85rem; color: #38bdf8; }
  .copy-btn {
    position: absolute;
    top: 6px;
    right: 6px;
    background: #1e293b;
    color: #94a3b8;
    border: 1px solid var(--border);
    border-radius: 4px;
    padding: 3px 8px;
    font-size: 0.75rem;
    cursor: pointer;
  }

  footer {
    flex: 0 0 auto;
    background: var(--panel);
    border-top: 1px solid var(--border);
    padding: 10px 12px;
    padding-bottom: max(10px, env(safe-area-inset-bottom));
    display: flex;
    gap: 8px;
    align-items: center;
    z-index: 10;
  }

  input {
    flex: 1;
    background: var(--bg);
    border: 1px solid var(--border);
    color: #fff;
    padding: 12px;
    border-radius: 6px;
    font-size: 16px;
    outline: none;
  }
  input:focus { border-color: var(--accent); }

  button.btn {
    padding: 0 16px;
    height: 44px;
    border: none;
    border-radius: 6px;
    font-weight: bold;
    cursor: pointer;
  }
  #send { background: #059669; color: #fff; }
  #stop { background: #dc2626; color: #fff; }

  /* Hamburger Drawer */
  #drawer {
    position: fixed;
    top: 0; left: -100%;
    width: 85%;
    max-width: 340px;
    height: 100%;
    background: #10141a;
    border-right: 1px solid var(--border);
    z-index: 100;
    transition: left 0.25s ease;
    padding: 20px;
    display: flex;
    flex-direction: column;
    gap: 14px;
    overflow-y: auto;
  }
  #drawer.open { left: 0; }
  #overlay { position: fixed; inset: 0; background: rgba(0,0,0,0.6); display: none; z-index: 90; }
  #overlay.open { display: block; }
  .drawer-title { color: var(--accent); font-size: 1.1rem; border-bottom: 1px solid var(--border); padding-bottom: 8px; font-weight: bold; }
  .drawer-label { font-size: 0.85rem; color: #94a3b8; margin-bottom: 4px; }
  textarea {
    width: 100%;
    height: 90px;
    background: var(--bg);
    border: 1px solid var(--border);
    color: #fff;
    padding: 8px;
    border-radius: 6px;
    font-size: 14px;
    resize: none;
  }
  select {
    width: 100%;
    padding: 10px;
    background: var(--bg);
    border: 1px solid var(--border);
    color: #fff;
    border-radius: 6px;
  }
  .btn-group { display: flex; gap: 8px; }
  .btn-group button { flex: 1; height: 38px; border: none; border-radius: 6px; font-weight: bold; cursor: pointer; font-size: 0.8rem; }
</style>
</head>
<body>

<div id="app-root">
  <header>
    <div class="hdr-left">
      <button class="menu-btn" onclick="toggleDrawer()">☰</button>
      <h1>AETHER-OS</h1>
    </div>
    <span id="status">Ready</span>
  </header>

  <div id="chat"></div>

  <footer>
    <input type="text" id="prompt" placeholder="Ask AetherOS..." autocomplete="off" />
    <button class="btn" id="send" onclick="sendPrompt()">SEND</button>
    <button class="btn" id="stop" onclick="stopGen()">STOP</button>
  </footer>
</div>

<!-- Drawer Menu -->
<div id="overlay" onclick="toggleDrawer()"></div>
<div id="drawer">
  <div class="drawer-title">AETHER-OS CONTROL</div>

  <div>
    <div class="drawer-label">Active Model</div>
    <select id="model_select" onchange="changeModel(this.value)"></select>
  </div>

  <div>
    <div class="drawer-label">System Prompt (.aosb)</div>
    <textarea id="sys_prompt"></textarea>
  </div>

  <div class="btn-group">
    <button style="background: #0284c7; color: #fff;" onclick="saveConfig()">SAVE CONFIG</button>
    <button style="background: #334155; color: #fff;" onclick="exportBotConfig()">EXPORT .AOSB</button>
  </div>
  <div class="btn-group">
    <button style="background: #1e293b; color: #38bdf8;" onclick="triggerImport('aosb')">IMPORT .AOSB</button>
  </div>

  <hr style="border-color: var(--border);" />

  <div class="drawer-label">Session Management (.aos)</div>
  <div class="btn-group">
    <button style="background: #059669; color: #fff;" onclick="exportChatSession()">EXPORT .AOS</button>
    <button style="background: #1e293b; color: #34d399;" onclick="triggerImport('aos')">IMPORT .AOS</button>
  </div>

  <button class="btn" style="background: #991b1b; color: #fff; height: 38px;" onclick="clearChat()">CLEAR CHAT</button>
</div>

<!-- Hidden File Pickers for .aos and .aosb -->
<input type="file" id="file_import_aos" accept=".aos" style="display:none" onchange="importChatFile(event)" />
<input type="file" id="file_import_aosb" accept=".aosb" style="display:none" onchange="importBotFile(event)" />

<script>
  let chatHistory = [];
  const STORAGE_KEY = 'AETHEROS_CHAT_V1';
  const CONFIG_KEY = 'AETHEROS_CONFIG_V1';

  // Format basic Markdown into HTML
  function renderMarkdown(txt) {
    let html = txt
      .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
      .replace(/```([\s\S]*?)```/g, (match, p1) => {
        return `<pre><button class="copy-btn" onclick="copyCode(this)">Copy</button><code>${p1.trim()}</code></pre>`;
      })
      .replace(/\*\*(.*?)\*\*/g, '<strong>$1</strong>')
      .replace(/\*(.*?)\*/g, '<em>$1</em>')
      .replace(/^# (.*$)/gim, '<h3 style="color:var(--accent);margin:4px 0;">$1</h3>')
      .replace(/\n/g, '<br/>');
    return html;
  }

  function copyCode(btn) {
    const code = btn.nextElementSibling.innerText;
    navigator.clipboard.writeText(code);
    btn.innerText = 'Copied!';
    setTimeout(() => btn.innerText = 'Copy', 1500);
  }

  function renderChat() {
    const container = document.getElementById('chat');
    container.innerHTML = '';
    if (chatHistory.length === 0) {
      container.innerHTML = '<div class="msg bot"><b>[AOS]</b> AetherOS Ready. Memory active.</div>';
      return;
    }
    chatHistory.forEach(m => {
      const d = document.createElement('div');
      d.className = `msg ${m.role === 'User' ? 'user' : 'bot'}`;
      d.innerHTML = m.role === 'User' ? m.text : `<b>[AOS]</b> ` + renderMarkdown(m.text);
      container.appendChild(d);
    });
    container.scrollTop = container.scrollHeight;
  }

  function saveStorage() {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(chatHistory));
  }

  function loadStorage() {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw) {
      try { chatHistory = JSON.parse(raw); } catch(e) { chatHistory = []; }
    }
    renderChat();

    const savedSys = localStorage.getItem(CONFIG_KEY);
    if (savedSys) {
      document.getElementById('sys_prompt').value = savedSys;
      saveConfig(false);
    }
  }

  function toggleDrawer() {
    document.getElementById('drawer').classList.toggle('open');
    document.getElementById('overlay').classList.toggle('open');
  }

  function saveConfig(notify = true) {
    const val = document.getElementById('sys_prompt').value;
    localStorage.setItem(CONFIG_KEY, val);
    fetch('/api/config', {
      method: 'POST',
      body: JSON.stringify({ system_prompt: val })
    });
    if (notify) toggleDrawer();
  }

  function clearChat() {
    chatHistory = [];
    saveStorage();
    renderChat();
    toggleDrawer();
  }

  // --- .AOS EXPORT & IMPORT ---
  function exportChatSession() {
    const data = {
      format: "aos_chat_v1",
      timestamp: Date.now(),
      messages: chatHistory
    };
    downloadFile(JSON.stringify(data, null, 2), `session_${Date.now()}.aos`);
  }

  function triggerImport(type) {
    if (type === 'aos') document.getElementById('file_import_aos').click();
    if (type === 'aosb') document.getElementById('file_import_aosb').click();
  }

  function importChatFile(e) {
    const file = e.target.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = (evt) => {
      try {
        const parsed = JSON.parse(evt.target.result);
        if (parsed.messages && Array.isArray(parsed.messages)) {
          chatHistory = parsed.messages;
          saveStorage();
          renderChat();
          toggleDrawer();
        }
      } catch(err) { alert('Invalid .aos file!'); }
    };
    reader.readAsText(file);
  }

  // --- .AOSB BOT CONFIG EXPORT & IMPORT ---
  function exportBotConfig() {
    const cfg = {
      format: "aos_bot_v1",
      name: "AetherOS Preset",
      system_prompt: document.getElementById('sys_prompt').value
    };
    downloadFile(JSON.stringify(cfg, null, 2), `preset_${Date.now()}.aosb`);
  }

  function importBotFile(e) {
    const file = e.target.files[0];
    if (!file) return;
    const reader = new FileReader();
    reader.onload = (evt) => {
      try {
        const parsed = JSON.parse(evt.target.result);
        if (parsed.system_prompt) {
          document.getElementById('sys_prompt').value = parsed.system_prompt;
          saveConfig(true);
        }
      } catch(err) { alert('Invalid .aosb file!'); }
    };
    reader.readAsText(file);
  }

  function downloadFile(content, filename) {
    const blob = new Blob([content], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
  }

  // Populate models dropdown
  fetch('/api/models').then(r => r.json()).then(models => {
    const sel = document.getElementById('model_select');
    sel.innerHTML = '';
    models.forEach(m => {
      const opt = document.createElement('option');
      opt.value = m;
      opt.innerText = m.replace('model/', '');
      sel.appendChild(opt);
    });
  });

  function changeModel(m) {
    fetch('/api/set_model', { method: 'POST', body: m });
  }

  // --- Streaming Generation ---
  let activeBotMessage = null;
  function sendPrompt() {
    const input = document.getElementById('prompt');
    const text = input.value.trim();
    if (!text) return;
    input.value = '';

    chatHistory.push({ role: 'User', text: text });
    chatHistory.push({ role: 'AOS', text: '' });
    saveStorage();
    renderChat();

    document.getElementById('status').innerText = 'Generating...';

    const evt = new EventSource(`/api/chat?prompt=${encodeURIComponent(text)}`);
    evt.onmessage = (e) => {
      chatHistory[chatHistory.length - 1].text += e.data;
      renderChat();
    };
    evt.onerror = () => {
      evt.close();
      saveStorage();
      document.getElementById('status').innerText = 'Ready';
    };
  }

  function stopGen() {
    fetch('/api/stop', { method: 'POST' });
  }

  window.addEventListener('DOMContentLoaded', loadStorage);
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
    std::cout << "  AETHER-OS PERSISTENT SERVER ACTIVE\n";
    std::cout << "  Open: http://localhost:8080\n";
    std::cout << "============================================\n";

    svr.listen("0.0.0.0", 8080);
    return 0;
}
