#!/usr/bin/env bash
set -e
echo "[build] Compiling IDE..."
g++ -std=c++17 \
    src/Processing.cpp \
    src/IDE.cpp \
    src/main.cpp \
    -o cpp-dev \
    -lglfw -lGLEW -lGL -lGLU -lm -pthread
echo "[build] Done: ./cpp-dev"
