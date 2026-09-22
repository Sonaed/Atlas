// Atlas — moteur de brosse.
//
// Trois niveaux, du plus bas au plus haut :
//   dab()     une empreinte circulaire à bord doux, écrite directement dans les
//             tuiles par segments horizontaux (chemin chaud du pinceau et de la
//             gomme dans l'UI).
//   segment() une suite d'empreintes interpolées linéairement entre deux
//             échantillons, pression comprise.
//   stroke()  un trait complet lissé en Catmull-Rom, tamponné avec la brosse
//             courante (IBrush). Utilisé par le système d'outils (BrushTool),
//             pas par l'UI GTK, qui fait son propre lissage Hermite.
//
// L'annulation n'est pas gérée ici : elle vit dans TileStore (begin_record /
// commit_record / apply_step), qui ne retient que les tuiles modifiées.
#include "engine/brush/brush_engine.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace creative::engine {

// Brosse procédurale : disque orienté par l'inclinaison, dont le rayon croît
// avec la pression et légèrement avec la vitesse. Passe par read/write_pixel ;
// elle n'est pas dans le chemin chaud de l'UI.
void ProceduralBrush::stamp(TileStore& store, const BrushSample& s, double size,
                            double opacity, std::uint32_t rgba, BrushMode mode) {
    if (!store.width() || !store.height()) return;
    const auto radius = static_cast<std::size_t>(std::max(1.0,
        size * (.65 + .35 * s.pressure) * (1.0 + std::min(s.speed, 1.0) * .15)));
    const auto cx = static_cast<std::size_t>(std::clamp(s.x, 0.0, double(store.width()  - 1)));
    const auto cy = static_cast<std::size_t>(std::clamp(s.y, 0.0, double(store.height() - 1)));
    const auto minx = cx > radius ? cx - radius : 0;
    const auto miny = cy > radius ? cy - radius : 0;
    const auto maxx = std::min(store.width()  - 1, cx + radius);
    const auto maxy = std::min(store.height() - 1, cy + radius);
    const double angle = std::atan2(s.tilt_y, s.tilt_x);
    const double cs = std::cos(angle), sn = std::sin(angle);

    for (std::size_t y = miny; y <= maxy; ++y)
        for (std::size_t x = minx; x <= maxx; ++x) {
            const double dx = double(x) - s.x, dy = double(y) - s.y;
            const double rx = dx * cs + dy * sn, ry = -dx * sn + dy * cs;
            const double d  = std::hypot(rx, ry) / double(radius);
            if (d > 1) continue;
            const double a = opacity * s.pressure * (1 - d);
            auto c = rgba;
            if (mode == BrushMode::Dodge) c |= 0x00101010u;
            if (mode == BrushMode::Burn)  c &= 0xff303030u;
            if (a > .5) {
                // Composition « over » : une empreinte ne rend jamais la
                // peinture existante plus transparente.
                const auto src_alpha = static_cast<unsigned>(std::clamp(a, 0.0, 1.0) * 255.0);
                const auto dst_alpha = (store.read_pixel(x, y) >> 24) & 0xffu;
                const auto out_alpha = std::min(255u, src_alpha + dst_alpha * (255u - src_alpha) / 255u);
                store.write_pixel(x, y, (c & 0x00ffffffu) | (out_alpha << 24));
            }
        }
}

void BrushRegistry::register_brush(std::shared_ptr<IBrush> b) {
    if (b && !find(b->name())) brushes_.push_back(std::move(b));
}
std::shared_ptr<IBrush> BrushRegistry::find(const std::string& n) const {
    for (auto& b : brushes_) if (b->name() == n) return b;
    return {};
}

// dab : rgba == 0 signifie « gomme » (l'alpha existant est réduit, la couleur
// conservée). Sinon composition « over » avec la couleur et l'alpha donnés.
// `hardness` règle la part du rayon peinte à pleine opacité avant le fondu.
void BrushEngine::dab(std::size_t x, std::size_t y, std::size_t radius,
                      std::uint32_t rgba, double hardness) {
    if (!store_) return;
    const std::size_t W = store_->width(), H = store_->height();
    if (!W || !H || x >= W || y >= H) return;

    const std::size_t min_x = x > radius ? x - radius : 0;
    const std::size_t min_y = y > radius ? y - radius : 0;
    const std::size_t max_x = std::min(W - 1, x + radius);
    const std::size_t max_y = std::min(H - 1, y + radius);

    const unsigned src_full = (rgba >> 24) & 0xffu;
    const bool     erase    = (rgba == 0u);
    if (!erase && !src_full) return;

    const double R        = static_cast<double>(std::max<std::size_t>(1, radius));
    const double edge     = std::clamp(hardness, 0.0, 1.0) * 0.95;
    const double inv_soft = 1.0 / std::max(0.001, 1.0 - edge);

    const std::byte cr = std::byte( rgba        & 0xffu);
    const std::byte cg = std::byte((rgba >>  8) & 0xffu);
    const std::byte cb = std::byte((rgba >> 16) & 0xffu);

    for (std::size_t py = min_y; py <= max_y; ++py) {
        const double dy  = static_cast<double>(py) - static_cast<double>(y);
        const double dy2 = dy * dy;
        const double rem = R * R - dy2;
        if (rem < 0.0) continue;

        // Demi-largeur du disque sur cette ligne : on n'itère que sur les
        // pixels réellement dans le cercle au lieu de balayer tout le carré
        // englobant et d'en rejeter 21 %.
        const double        hw  = std::sqrt(rem);
        const std::int64_t  cxi = static_cast<std::int64_t>(x);
        std::int64_t lo = static_cast<std::int64_t>(std::ceil (cxi - hw));
        std::int64_t hi = static_cast<std::int64_t>(std::floor(cxi + hw));
        lo = std::max<std::int64_t>(lo, static_cast<std::int64_t>(min_x));
        hi = std::min<std::int64_t>(hi, static_cast<std::int64_t>(max_x));
        if (lo > hi) continue;

        std::size_t px = static_cast<std::size_t>(lo);
        const std::size_t end = static_cast<std::size_t>(hi);

        while (px <= end) {
            std::size_t run = 0;
            // Une seule résolution de tuile pour tout le segment : les quatre
            // divisions entières de write_pixel disparaissent du chemin chaud.
            std::byte* p = store_->span_for_write(px, py, run);
            if (!p || !run) break;
            run = std::min(run, end - px + 1);

            for (std::size_t i = 0; i < run; ++i) {
                const double dx = static_cast<double>(px + i) - static_cast<double>(x);
                const double d  = std::sqrt(dx * dx + dy2) / R;
                if (d > 1.0) continue;

                const double cov = d <= edge ? 1.0 : (1.0 - d) * inv_soft;
                const unsigned sa =
                    static_cast<unsigned>(src_full * std::clamp(cov, 0.0, 1.0));
                if (!sa) continue;

                std::byte* q = p + i * 4;
                const unsigned da = std::to_integer<unsigned>(q[3]);
                if (erase) {
                    // Gomme : l'alpha existant tend vers 0, la couleur est gardée.
                    q[3] = std::byte(da * (255u - sa) / 255u);
                } else {
                    // Porter-Duff « over » : une nouvelle empreinte ne rend
                    // jamais la peinture existante plus transparente.
                    q[0] = cr; q[1] = cg; q[2] = cb;
                    q[3] = std::byte(std::min(255u, sa + da * (255u - sa) / 255u));
                }
            }
            px += run;
        }
    }
    store_->mark_modified();  // une révision par dab, pas par pixel
    ++dabs_;
}

void BrushEngine::segment(const BrushSample& from, const BrushSample& to,
                          double radius, double opacity, std::uint32_t rgba,
                          double hardness, double spacing) {
    if (!store_ || !store_->width() || !store_->height()) return;
    const double dx = to.x - from.x, dy = to.y - from.y;
    const double distance = std::hypot(dx, dy);
    const int count = std::max(1, static_cast<int>(std::ceil(
        distance / std::max(0.25, spacing * std::max(1.0, radius)))));
    const unsigned r = (rgba >> 0) & 0xffu, g = (rgba >> 8) & 0xffu;
    const unsigned b = (rgba >> 16) & 0xffu;
    // Inclure les deux extrémités : cela évite une rupture entre le dab
    // initial et le premier segment, notamment quand la pression varie.
    for (int i = 0; i <= count; ++i) {
        const double t = static_cast<double>(i) / count;
        BrushSample s;
        s.x = from.x + dx * t; s.y = from.y + dy * t;
        s.pressure = std::clamp(from.pressure + (to.pressure - from.pressure) * t, 0.0, 1.0);
        const double a = std::clamp(opacity * s.pressure, 0.0, 1.0);
        const auto alpha = static_cast<unsigned>(a * 255.0 + 0.5);
        const auto rr = static_cast<std::size_t>(std::max(1.0, radius * (.65 + .35 * s.pressure)));
        dab(static_cast<std::size_t>(std::clamp(s.x, 0.0, double(store_->width()-1))),
            static_cast<std::size_t>(std::clamp(s.y, 0.0, double(store_->height()-1))),
            rr, r | (g << 8) | (b << 16) | (alpha << 24), hardness);
    }
}

// Lissage Catmull-Rom uniforme d'une suite d'échantillons, rééchantillonnée
// tous les `spacing` pixels environ. Pression et inclinaison sont interpolées
// linéairement ; la vitesse est la distance au point de contrôle précédent.
std::vector<BrushSample> BrushEngine::interpolate_catmull_rom(
        const std::vector<BrushSample>& in, double spacing) const {
    if (in.size() < 2) return in;
    std::vector<BrushSample> out;
    out.push_back(in.front());
    for (std::size_t i = 0; i + 1 < in.size(); ++i) {
        const auto& p0 = i ? in[i - 1] : in[i];
        const auto& p1 = in[i];
        const auto& p2 = in[i + 1];
        const auto& p3 = i + 2 < in.size() ? in[i + 2] : p2;
        const double dist = std::hypot(p2.x - p1.x, p2.y - p1.y);
        const int n = std::max(1, int(std::ceil(dist / std::max(1., spacing))));
        for (int j = 1; j <= n; ++j) {
            const double t = double(j) / n, t2 = t * t, t3 = t2 * t;
            BrushSample s;
            s.x = .5 * ((2*p1.x) + (-p0.x + p2.x)*t + (2*p0.x - 5*p1.x + 4*p2.x - p3.x)*t2
                      + (-p0.x + 3*p1.x - 3*p2.x + p3.x)*t3);
            s.y = .5 * ((2*p1.y) + (-p0.y + p2.y)*t + (2*p0.y - 5*p1.y + 4*p2.y - p3.y)*t2
                      + (-p0.y + 3*p1.y - 3*p2.y + p3.y)*t3);
            s.pressure = p1.pressure + (p2.pressure - p1.pressure) * t;
            s.tilt_x   = p1.tilt_x   + (p2.tilt_x   - p1.tilt_x)   * t;
            s.tilt_y   = p1.tilt_y   + (p2.tilt_y   - p1.tilt_y)   * t;
            s.speed    = std::hypot(s.x - p1.x, s.y - p1.y);
            out.push_back(s);
        }
    }
    return out;
}

// Trait complet : lissage puis tamponnage avec la brosse courante.
// Exécuté directement sur le thread appelant. L'ancienne version postait le
// travail sur un pool d'un seul thread puis attendait aussitôt sa fin — un
// aller-retour de thread sans aucun parallélisme. Elle empilait aussi un
// instantané complet du document par trait, sans limite ; l'annulation est
// désormais du ressort de TileStore.
void BrushEngine::stroke(const std::vector<BrushSample>& samples, std::uint32_t rgba) {
    if (!store_ || samples.empty() || !brush_) return;
    const auto path = interpolate_catmull_rom(samples, 3.0);
    BrushSample prev = path.front();
    for (const auto& raw : path) {
        BrushSample s = raw;
        s.speed = std::hypot(s.x - prev.x, s.y - prev.y);
        brush_->stamp(*store_, s, 8.0, s.pressure, rgba, mode_);
        prev = s;
        ++dabs_;
    }
    store_->mark_modified();
}

} // namespace creative::engine
