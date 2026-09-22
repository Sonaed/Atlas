#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  Atlas v3 — Lanceur GPU / CPU
#  Usage :
#    ./launch.sh          → mode CPU (sûr, fallback)
#    ./launch.sh gpu      → mode GPU (Wayland/EGL)
#    ./launch.sh cpu      → mode CPU explicite
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="$SCRIPT_DIR/build/CreativeSystemAtlasGtk"

if [ ! -f "$BINARY" ]; then
    echo "❌  Binaire introuvable : $BINARY"
    echo "   Lance d'abord : ./build.sh"
    exit 1
fi

MODE="${1:-cpu}"

case "$MODE" in
    gpu|GPU)
        export CREATIVE_ENABLE_GPU=1
        echo "🚀  Atlas v3 — Mode GPU (Wayland/EGL)"
        ;;
    cpu|CPU|*)
        unset CREATIVE_ENABLE_GPU 2>/dev/null || true
        echo "🖥  Atlas v3 — Mode CPU (fallback Cairo)"
        ;;
esac

# Ne pas transmettre l'option de mode à GtkApplication : sinon GTK tente
# d'ouvrir "gpu" ou "cpu" comme un fichier et termine avec une erreur.
if [ "$#" -gt 0 ]; then
    case "$1" in
        gpu|GPU|cpu|CPU) shift ;;
    esac
fi

# Support XPPEN : activer Wayland natif pour les tablettes stylet
export GDK_BACKEND=wayland
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"

exec "$BINARY" "$@"
