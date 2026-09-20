# AetherOS by @dev-aemni (https://github.com/dev-aemni/AetherOS)

# AetherOS // User Guide

## 1. Launching Modes

### Mode A: Web Browser Mode (Best for Phones & Everyday Use)
- **Why use it**: Zero window-scaling hassles, full mobile auto-correct keyboard, 120Hz smooth scrolling.
- **How to launch**:
  - Android: `./run_web.sh`
  - Windows: Double-click `run_web.bat`
- Open your browser to `http://localhost:8080`.

### Mode B: Native GUI Mode (Cyberpunk Desktop Interface)
- **Why use it**: Self-contained graphical window powered by SDL3 and Dear ImGui.
- **How to launch**:
  - Android: Ensure Termux:X11 is running, then run `./run_gui.sh`.
  - Windows: Double-click `run_gui.bat`.

---

## 2. Using the Hamburger Menu [ ☰ ]
Tap the **`[ = ]`** button in the top left of the interface to open the control drawer:
- **Active Model Dropdown**: Hot-swap between any `.gguf` files located in the `model/` directory.
- **System Prompt**: Customize the bot's behavior, tone, or roleplay rules.
- **Temperature Slider**: Adjust creativity (0.2 = deterministic/coding; 0.8 = conversational).

---

## 3. Custom File Formats

### `.aos` (AetherOS Session)
- Contains complete conversation transcripts, timestamps, and message roles.
- **Export**: Click `Export Chat (.aos)` to save your current chat.
- **Import**: Click `Import Chat (.aos)` to restore any past session.
- The app automatically writes to `autosave.aos` after every response so progress is never lost.

### `.aosb` (AetherOS Bot Preset)
- Contains bot configurations: system prompts, temperature, and persona instructions.
- Save distinct personas (e.g., `coder.aosb`, `assistant.aosb`, `tutor.aosb`) and switch between them instantly.

---

## 4. Emergency Generation Stop
If the model begins generating repetitive text or rambling, tap the red **`[STOP]`** button. Generation halts instantly within one token cycle without freezing the interface.
