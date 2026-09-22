#!/usr/bin/env bash
cd /home/deanos/Documents/CreativeSystemAtlas_alpha0.1
cmake --build build --target CreativeSystemAtlasGtk -j$(nproc) 2>&1
echo "___BUILD_EXIT:$?___"
