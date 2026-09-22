#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  Atlas v3 — Script de build
#  Usage :
#    ./build.sh           → build incrémental
#    ./build.sh clean     → supprime build/ et recompile tout
#    ./build.sh release   → build Release (optimisé)
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_TYPE="${2:-Release}"
JOBS=$(nproc)

if [ "${1:-}" = "clean" ]; then
    echo "🗑  Nettoyage du répertoire build/"
    rm -rf build
fi

if [ ! -d build ]; then
    mkdir build
fi

echo "⚙  Configuration CMake (${BUILD_TYPE}) …"
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    2>&1 | grep -v "^--" | head -20

echo ""
echo "🔨  Compilation avec ${JOBS} threads …"
cmake --build build --target CreativeSystemAtlasGtk -j"$JOBS" 2>&1

if [ $? -eq 0 ]; then
    echo ""
    echo "✅  Build réussi !"
    echo "   Binaire : build/CreativeSystemAtlasGtk"
    echo ""
    echo "   Lancement CPU :  ./launch.sh"
    echo "   Lancement GPU :  ./launch.sh gpu"
else
    echo ""
    echo "❌  Erreurs de compilation. Voir la sortie ci-dessus."
    exit 1
fi
