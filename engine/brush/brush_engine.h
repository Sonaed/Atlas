#pragma once
// Atlas — moteur de brosse. Voir brush_engine.cpp pour la répartition des rôles
// entre dab(), segment() et stroke().
#include "engine/tiles/tile_store.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace creative::engine {

enum class BrushMode { Normal, Blur, Smear, Clone, Dodge, Burn };

// Un échantillon de trait, en coordonnées document (pixels).
// pressure : 0..1. tilt_x/tilt_y : inclinaison normalisée -1..1.
// speed : distance parcourue depuis l'échantillon précédent.
struct BrushSample { double x = 0, y = 0, pressure = 1, tilt_x = 0, tilt_y = 0, speed = 0; };

// Une brosse sait imprimer une empreinte à partir d'un échantillon.
class IBrush {
public:
    virtual ~IBrush() = default;
    virtual std::string name() const = 0;
    virtual void stamp(TileStore&, const BrushSample&, double size, double opacity,
                       std::uint32_t rgba, BrushMode) = 0;
};

class ProceduralBrush final : public IBrush {
public:
    std::string name() const override { return "Procedural"; }
    void stamp(TileStore&, const BrushSample&, double, double, std::uint32_t, BrushMode) override;
};

// Catalogue de brosses par nom (un nom ne peut être enregistré qu'une fois).
class BrushRegistry {
public:
    void register_brush(std::shared_ptr<IBrush> brush);
    std::shared_ptr<IBrush> find(const std::string& name) const;
    std::size_t size() const noexcept { return brushes_.size(); }
private:
    std::vector<std::shared_ptr<IBrush>> brushes_;
};

class BrushEngine {
public:
    explicit BrushEngine(TileStore& store) : store_(&store) {}
    // Change le calque cible (appelé à chaque changement de calque actif).
    void set_store(TileStore& store) noexcept { store_ = &store; }

    // Empreinte unique en (x,y). rgba == 0 : gomme. Chemin chaud de l'UI.
    void dab(std::size_t x, std::size_t y, std::size_t radius,
             std::uint32_t rgba = 0xff000000u, double hardness = 1.0);

    // Empreintes interpolées linéairement de `from` à `to`, espacées de
    // spacing × rayon. Les deux extrémités sont incluses.
    void segment(const BrushSample& from, const BrushSample& to,
                 double radius, double opacity, std::uint32_t rgba,
                 double hardness = 1.0, double spacing = 0.25);

    // Trait complet lissé en Catmull-Rom, tamponné avec la brosse courante.
    void stroke(const std::vector<BrushSample>& samples, std::uint32_t rgba = 0xff000000u);
    std::vector<BrushSample> interpolate_catmull_rom(const std::vector<BrushSample>& samples,
                                                     double spacing = 3.0) const;

    void set_mode(BrushMode mode) noexcept { mode_ = mode; }
    void set_brush(std::shared_ptr<IBrush> brush) { brush_ = std::move(brush); }
    std::size_t dab_count() const noexcept { return dabs_; }

    // Pas d'undo/redo ici : l'annulation est gérée par TileStore, qui ne
    // retient que les tuiles modifiées. L'ancienne pile de BrushEngine
    // stockait un instantané complet du document par trait, sans limite.
    // Pas de ThreadPool non plus : il démarrait un thread au lancement
    // d'Atlas pour un travail qu'on attendait aussitôt de façon synchrone.

private:
    TileStore* store_;
    std::size_t dabs_ = 0;
    BrushMode mode_ = BrushMode::Normal;
    std::shared_ptr<IBrush> brush_ = std::make_shared<ProceduralBrush>();
};

} // namespace creative::engine
