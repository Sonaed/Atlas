#pragma once
// Atlas — modèle de calques générique (raster, groupe, ajustement).
//
// Note : l'interface GTK d'Atlas (ui/gtk/main.cpp) ne passe pas par ce modèle ;
// elle gère ses calques directement comme des TileStore, avec sa propre
// composition optimisée (bg_cache + dirty-rect). Ce modèle sert aux outils,
// aux plugins et au chemin LayerStack de NativeCanvasSurface.
#include "engine/fusion/fusion_creator_engine.h"
#include "engine/tiles/tile_store.h"
#include <memory>
#include <string>
#include <vector>

namespace creative::engine {

class ILayer {
public:
    virtual ~ILayer() = default;
    virtual std::string name() const = 0;
    virtual void set_name(std::string) = 0;
    // Compose ce calque par-dessus `out`.
    virtual void render(TileStore& out) const = 0;
    // Masque du calque, ou nullptr si le type de calque n'en a pas.
    virtual TileStore* mask() { return nullptr; }
    bool visible = true;
    bool locked = false;
    float opacity = 1.0f;
    std::string blend_mode = "normal";
};

class RasterLayer final : public ILayer {
public:
    // Le masque n'est plus rempli à la construction : l'ancien
    // touch_region(0,0,w,h,blanc) allouait toutes ses tuiles d'emblée, soit
    // 64 Mio par calque sur un 4096², pour un masque que personne n'utilisait.
    // Il est matérialisé (opaque) à la première demande via mask().
    RasterLayer(std::string n, std::size_t w, std::size_t h)
        : name_(std::move(n)), store_(w, h), mask_(w, h) {}

    std::string name() const override { return name_; }
    void set_name(std::string n) override { name_ = std::move(n); }

    TileStore* mask() override {
        if (!has_mask_) {
            mask_.touch_region(0, 0, mask_.width(), mask_.height(), 0xffffffffu);
            has_mask_ = true;
        }
        return &mask_;
    }
    bool has_mask() const { return has_mask_; }

    void render(TileStore& out) const override {
        if (!visible) return;
        const auto p = store_.snapshot_rgba();
        std::vector<std::byte> m;
        if (has_mask_) m = mask_.snapshot_rgba();   // sinon : masque implicite opaque
        const std::size_t W = store_.width(), H = store_.height();
        for (std::size_t y = 0; y < H; ++y)
            for (std::size_t x = 0; x < W; ++x) {
                const auto i = (y * W + x) * 4;
                unsigned a = std::to_integer<unsigned char>(p[i + 3]);
                if (has_mask_) a = a * std::to_integer<unsigned char>(m[i + 3]) / 255;
                if (!a) continue;
                out.write_pixel(x, y,
                      std::uint32_t(std::to_integer<unsigned char>(p[i]))
                    | std::uint32_t(std::to_integer<unsigned char>(p[i + 1])) << 8
                    | std::uint32_t(std::to_integer<unsigned char>(p[i + 2])) << 16
                    | std::uint32_t(a) << 24);
            }
    }

    TileStore& pixels() { return store_; }
    TileStore& mask_pixels() { return *mask(); }

private:
    std::string name_;
    TileStore store_, mask_;
    bool has_mask_ = false;
};

class GroupLayer final : public ILayer {
public:
    explicit GroupLayer(std::string n) : name_(std::move(n)) {}
    std::string name() const override { return name_; }
    void set_name(std::string n) override { name_ = std::move(n); }
    void render(TileStore& out) const override {
        if (visible) for (auto& l : children_) l->render(out);
    }
    void add(std::shared_ptr<ILayer> l) { if (l) children_.push_back(std::move(l)); }
    const auto& children() const { return children_; }
private:
    std::string name_;
    std::vector<std::shared_ptr<ILayer>> children_;
};

// Calque d'ajustement : réservé, n'a pas encore d'effet au rendu.
class AdjustmentLayer final : public ILayer {
public:
    explicit AdjustmentLayer(std::string n) : name_(std::move(n)) {}
    std::string name() const override { return name_; }
    void set_name(std::string n) override { name_ = std::move(n); }
    void render(TileStore&) const override {}
private:
    std::string name_;
};

class LayerStack {
public:
    LayerStack(std::size_t w, std::size_t h) : width_(w), height_(h) {}
    std::size_t width()  const { return width_; }
    std::size_t height() const { return height_; }

    void add(std::shared_ptr<ILayer> l) { if (l) layers_.push_back(std::move(l)); }
    bool remove(std::size_t i) {
        if (i >= layers_.size()) return false;
        layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    bool move(std::size_t from, std::size_t to) {
        if (from >= layers_.size() || to >= layers_.size()) return false;
        auto l = std::move(layers_[from]);
        layers_.erase(layers_.begin() + static_cast<std::ptrdiff_t>(from));
        layers_.insert(layers_.begin() + static_cast<std::ptrdiff_t>(to), std::move(l));
        return true;
    }
    // Compose tous les calques, du premier (fond) au dernier, sur `out`.
    void composite(TileStore& out) const { for (auto& l : layers_) l->render(out); }
    std::size_t size() const { return layers_.size(); }
    const auto& layers() const { return layers_; }

private:
    std::size_t width_, height_;
    std::vector<std::shared_ptr<ILayer>> layers_;
};

} // namespace creative::engine
