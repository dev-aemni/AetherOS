#!/bin/bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

echo "=========================================="
echo "  Compiling AetherOS CLI (src/cli.cpp)... "
echo "=========================================="

clang++ -std=c++17 -O3 src/cli.cpp \
  -Isrc/llama-runtime/include \
  -Lsrc/llama-runtime/lib \
  -lllama -lggml -lggml-base -lggml-cpu \
  -lreadline \
  -Wl,-rpath,'$ORIGIN/src/llama-runtime/lib' \
  -Wl,-rpath,"$DIR/src/llama-runtime/lib" \
  -lpthread \
  -o aos

if [ $? -eq 0 ]; then
  chmod +x "$DIR/aos"
  ln -sf "$DIR/aos" "$PREFIX/bin/aos"
  echo ""
  echo "[SUCCESS] AetherOS compiled successfully!"
  echo "[+] Global command updated: Type 'aos' from anywhere."
else
  echo ""
  echo "[ERROR] Compilation failed!"
  exit 1
fi
