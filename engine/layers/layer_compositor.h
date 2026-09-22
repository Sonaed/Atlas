#pragma once
// Atlas — composition d'une pile de calques, côté CPU.
//
// Séparation stricte des responsabilités :
//   * LayerCompositor compose (CPU) : pile de calques → un TileStore.
//   * NativeRenderer présente (GPU) : TileStore → écran.
// L'ancienne version faisait les deux à la fois : elle composait sur CPU,
// envoyait le résultat en texture, dessinait dans un framebuffer sans
// attachement (donc incomplet, dessin ignoré) puis appelait eglSwapBuffers
// depuis le moteur. Le moteur ne dépend plus ni d'EGL ni de GL.
#include "engine/layers/layer_stack.h"
#include "engine/tiles/tile_store.h"

namespace creative::engine {

class LayerCompositor {
public:
    // Efface `out` puis y compose la pile, du bas vers le haut.
    // `out` doit avoir les dimensions du document.
    void compose(const LayerStack& stack, TileStore& out) const;
};

} // namespace creative::engine
