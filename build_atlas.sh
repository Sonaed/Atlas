#!/usr/bin/env bash
set -e
cd /home/deanos/Documents/CreativeSystemAtlas_alpha0.1

echo "=== Atlas v3 — Build ==="
if [ ! -d build ]; then
    mkdir build
fi
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -5
cmake --build . --target CreativeSystemAtlasGtk -j$(nproc) 2>&1
echo "=== Build terminé : build/CreativeSystemAtlasGtk ==="
