#!/bin/bash

echo "========================================"
echo "Building Skeleton Vulkan (macOS)"
echo "========================================"
echo ""

# Detect CMake path
if [ -x "/usr/local/bin/cmake" ]; then
    CMAKE_PATH="/usr/local/bin/cmake"
elif [ -x "/opt/homebrew/bin/cmake" ]; then
    CMAKE_PATH="/opt/homebrew/bin/cmake"
elif command -v cmake &> /dev/null; then
    CMAKE_PATH="cmake"
else
    echo "[ERROR] CMake not found. Please install CMake."
    echo "  brew install cmake"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build_mac"
SOURCE_DIR="$SCRIPT_DIR"

echo "CMake: $CMAKE_PATH"
echo "Build directory: $BUILD_DIR"
echo ""

# Configure
echo "[1/2] Configuring CMake..."
echo ""

$CMAKE_PATH -B "$BUILD_DIR" -S "$SOURCE_DIR" \
    -DCMAKE_BUILD_TYPE=Release

if [ $? -ne 0 ]; then
    echo ""
    echo "[ERROR] CMake configuration failed!"
    exit 1
fi

echo ""
echo "[2/2] Building..."
echo ""

$CMAKE_PATH --build "$BUILD_DIR" --config Release

if [ $? -ne 0 ]; then
    echo ""
    echo "[ERROR] Build failed!"
    exit 1
fi

echo ""
echo "========================================"
echo "BUILD SUCCESSFUL"
echo "========================================"
echo ""
echo "Output: build_mac/Release/SkeletonVulkan.plugin"
echo ""
echo "Install by copying to:"
echo "  /Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/"
echo ""
