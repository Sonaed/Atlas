#include "engine/layers/layer_compositor.h"

namespace creative::engine {

void LayerCompositor::compose(const LayerStack& stack, TileStore& out) const {
    out.clear();
    stack.composite(out);
}

} // namespace creative::engine
