#!/bin/bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export DISPLAY=:0
export LD_LIBRARY_PATH="$DIR/src/llama-runtime/lib:$LD_LIBRARY_PATH"
cd "$DIR"
./aos_imgui
