#!/bin/bash
# Build script for Miyoo platform

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build_miyoo"

echo "=== Building Butterscotch for Miyoo ==="

# Clean build directory
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with Miyoo toolchain
cmake -DPLATFORM=miyoo \
      -DCMAKE_TOOLCHAIN_FILE="$SCRIPT_DIR/cmake/miyoo.cmake" \
      -DNO_AUDIO=ON \
      -DCMAKE_BUILD_TYPE=Release \
      "$SCRIPT_DIR"

# Build
echo "=== Building ==="
make -j$(nproc)

# Create IPK package
echo "=== Creating IPK package ==="
make ipk

echo "=== Build complete ==="
echo "Binary: $BUILD_DIR/butterscotch"
echo "IPK:    $BUILD_DIR/butterscotch_1.0.0_all.ipk"
echo ""
echo "File info:"
file "$BUILD_DIR/butterscotch"
file "$BUILD_DIR/butterscotch_1.0.0_all.ipk"
