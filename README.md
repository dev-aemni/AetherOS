# AetherOS (AOS Core)
**Autonomous Edge Neural Interface // 100% Offline Multi-Mode Station**

**Author:** [@dev-aemni](https://github.com/dev-aemni)  
**Repository:** [https://github.com/dev-aemni/AetherOS](https://github.com/dev-aemni/AetherOS)  
**License:** [MIT](LICENSE)

---

## Operating Modes

AetherOS delivers three execution interfaces from the same unified C++ core:

### 1. Interactive & One-Shot CLI (`aos` / `aos.exe`)
- **One-Shot**: `aos "What is a kernel?"` (Replies instantly and exits).
- **Interactive REPL**: `aos` (Full terminal console, memory, auto-save).
- **Interruptible**: Press `Ctrl+C` to halt runaway token generation.
- **Commands**: `/help`, `/clear`, `/sys`, `/save`, `/load`, `/temp`, `/exit`.

### 2. Embedded Mobile Web Mode (`run_web.sh` / `run_web.bat`)
- Pinned `100dvh` mobile interface for Chrome / Brave.
- LocalStorage chat persistence across page refreshes.
- `.aos` (chat sessions) and `.aosb` (bot presets) export and import.
- Real-time SSE streaming with code-block copy buttons.

### 3. Native Cyberpunk GUI (`run_gui.sh` / `run_gui.bat`)
- Pure C++ Dear ImGui + SDL3 window.
- Compact layout with mobile touch scaling.
- Hamburger settings drawer (`☰`).

---

## File Specifications
- **`.aos`**: AetherOS Session archives. Interchangeable across CLI, GUI, and Web.
- **`.aosb`**: AetherOS Bot configuration files (System prompt, temperature, persona).

---

## Quick Reference

| Action | Linux / Android | Windows |
|---|---|---|
| **One-Shot CLI** | `aos "hello"` | `aos.exe "hello"` |
| **Interactive CLI** | `aos` | `run_cli.bat` |
| **Mobile Web** | `./run_web.sh` | `run_web.bat` |
| **Native GUI** | `./run_gui.sh` | `run_gui.bat` |
| **Compile All** | `make` / `clang++` | `build_windows.bat` |
