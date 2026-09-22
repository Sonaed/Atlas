#pragma once
// Atlas — stockage pixel tuilé.
//
// Chaque tuile est allouée à la première écriture (canvas creux) et peut être
// évincée vers le scratch disk quand le budget RAM est dépassé. La pagination
// est transparente : read_pixel / write_pixel rechargent la tuile au besoin.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace creative::engine {

class TileStore {
public:
    TileStore(std::size_t width, std::size_t height, std::size_t tile_size = 256);
    ~TileStore();
    TileStore(const TileStore&);
    TileStore& operator=(const TileStore&);
    TileStore(TileStore&&) noexcept;
    TileStore& operator=(TileStore&&) noexcept;

    std::size_t width()      const noexcept { return width_; }
    std::size_t height()     const noexcept { return height_; }
    std::size_t tile_size()  const noexcept { return tile_size_; }
    std::size_t tile_count() const noexcept { return tiles_.size(); }
    std::size_t tile_cols()  const noexcept { return cols_; }
    std::size_t tile_rows()  const noexcept { return rows_; }
    std::size_t revision()   const noexcept { return revision_; }

    // ── Pixels ────────────────────────────────────────────────────────────
    void          write_pixel(std::size_t x, std::size_t y, std::uint32_t rgba);
    std::uint32_t read_pixel(std::size_t x, std::size_t y) const noexcept;
    void          touch_region(std::size_t x, std::size_t y, std::size_t w,
                               std::size_t h, std::uint32_t rgba);
    void          clear() noexcept;

    // ── Accès par segments (chemin chaud du pinceau) ───────────────────────
    // Résout la tuile une seule fois pour une suite de pixels contigus, au
    // lieu de refaire quatre divisions entières à chaque write_pixel. `run`
    // reçoit le nombre de pixels exploitables à partir de (x,y) ; l'appelant
    // avance de `run` puis redemande un segment.
    // span_for_read peut renvoyer nullptr (tuile jamais écrite) tout en
    // renseignant `run`, afin que la boucle appelante puisse sauter la zone.
    std::byte*       span_for_write(std::size_t x, std::size_t y, std::size_t& run);
    const std::byte* span_for_read (std::size_t x, std::size_t y, std::size_t& run) const;
    // À appeler une fois par opération (un dab), jamais par pixel : c'est
    // `revision()` qui déclenche la recomposition côté UI.
    void mark_modified() noexcept { ++revision_; }

    // ── Snapshots ─────────────────────────────────────────────────────────
    // Copie complète (alloue). Conservé pour compatibilité ; préférer
    // snapshot_into / snapshot_region dans les chemins chauds.
    std::vector<std::byte> snapshot_rgba() const;
    // Copie complète dans un tampon fourni par l'appelant : aucune allocation.
    // `out` doit faire au moins width*height*4 octets.
    void snapshot_into(std::byte* out) const;
    // Copie d'une sous-région [x0,x1) × [y0,y1) dans `out`, qui est indexé
    // comme le canvas complet (stride = width*4). Seule la région est touchée.
    void snapshot_region(std::size_t x0, std::size_t y0,
                         std::size_t x1, std::size_t y1,
                         std::byte* out) const;
    void restore_rgba(const std::vector<std::byte>& pixels);

    // ── Accès tuile (upload GPU) ──────────────────────────────────────────
    // Pixels bruts d'une tuile résidente, ou nullptr si la tuile est vide.
    // Force le rechargement depuis le scratch si nécessaire.
    const std::byte* tile_pixels(std::size_t index) const;
    void             tile_origin(std::size_t index, std::size_t& x, std::size_t& y) const;
    // Tuiles modifiées depuis le dernier clear_dirty() — pour un upload GPU
    // incrémental plutôt qu'un ré-upload complet à chaque frame.
    const std::vector<std::size_t>& dirty_tiles() const noexcept { return dirty_; }
    void                            clear_dirty() const;
    void                            mark_all_dirty() const;

    // ── Annulation ────────────────────────────────────────────────────────
    // Un pas d'annulation ne retient que les tuiles réellement modifiées, et
    // non un instantané du document entier : sur un 4096² un coup de pinceau
    // coûte quelques centaines de Kio au lieu de 64 Mio.
    // Une pré-image vide signifie « la tuile n'existait pas encore ».
    using UndoStep = std::vector<std::pair<std::size_t, std::vector<std::byte>>>;
    void     begin_record();
    UndoStep commit_record();
    // Échange contenu courant et pré-image : le pas devient son propre
    // inverse, ce qui donne le rétablissement sans stockage supplémentaire.
    void     apply_step(UndoStep& step);
    static std::size_t step_bytes(const UndoStep& s);

    // ── Mémoire ───────────────────────────────────────────────────────────
    std::size_t bytes_resident() const noexcept { return resident_bytes_; }
    std::size_t tiles_resident() const noexcept { return resident_count_; }
    std::size_t tiles_swapped()  const noexcept { return swapped_count_; }
    // Évince les tuiles les plus froides jusqu'à libérer `bytes` octets.
    // Renvoie le nombre d'octets effectivement libérés.
    std::size_t evict_cold(std::size_t bytes) const;

    bool save_ppm(const std::string& path) const;

private:
    struct Tile {
        std::vector<std::byte> px;          // vide ⇒ non résidente
        std::int64_t           scratch = -1; // ≥0 ⇒ présente sur le scratch
        std::uint64_t          stamp   = 0;  // horodatage LRU
        bool                   dirty   = false;
    };

    std::size_t tile_bytes() const noexcept { return tile_size_ * tile_size_ * 4; }
    // Rend la tuile résidente pour écriture (alloue ou recharge). Jamais null.
    Tile&       tile_for_write(std::size_t index);
    // Rend la tuile résidente pour lecture ; null si elle n'a jamais été écrite.
    const Tile* tile_for_read(std::size_t index) const;
    void        page_in(Tile& t) const;
    void        mark_dirty(std::size_t index) const;
    void        release_all() noexcept;
    void        copy_from(const TileStore& other);

    std::size_t width_ = 0, height_ = 0, tile_size_ = 1, cols_ = 0, rows_ = 0;
    std::size_t revision_ = 0;

    mutable std::vector<Tile>        tiles_;
    mutable std::vector<std::size_t> dirty_;
    mutable bool              recording_ = false;
    mutable std::vector<char> recorded_;   // une entrée par tuile
    mutable UndoStep          record_;

    mutable std::size_t   resident_bytes_ = 0;
    mutable std::size_t   resident_count_ = 0;
    mutable std::size_t   swapped_count_  = 0;
    mutable std::uint64_t clock_          = 0;
};

} // namespace creative::engine
