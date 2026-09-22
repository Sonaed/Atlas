#include "engine/tiles/tile_store.h"
#include "engine/memory/memory_manager.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace creative::engine {

// ── Construction / copie ─────────────────────────────────────────────────────

TileStore::TileStore(std::size_t width, std::size_t height, std::size_t tile_size)
    : width_(width), height_(height),
      tile_size_(std::max<std::size_t>(1, tile_size)) {
    cols_ = (width_  + tile_size_ - 1) / tile_size_;
    rows_ = (height_ + tile_size_ - 1) / tile_size_;
    tiles_.resize(cols_ * rows_);
}

TileStore::~TileStore() { release_all(); }

void TileStore::release_all() noexcept {
    auto& mm = MemoryManager::instance();
    for (auto& t : tiles_) {
        if (!t.px.empty()) mm.on_free(t.px.size());
        if (t.scratch >= 0) mm.free_block(t.scratch, tile_bytes());
        t.px.clear();
        t.px.shrink_to_fit();
        t.scratch = -1;
    }
    resident_bytes_ = resident_count_ = swapped_count_ = 0;
    dirty_.clear();
}

void TileStore::copy_from(const TileStore& o) {
    width_ = o.width_; height_ = o.height_; tile_size_ = o.tile_size_;
    cols_ = o.cols_; rows_ = o.rows_; revision_ = o.revision_;
    tiles_.assign(o.tiles_.size(), Tile{});
    resident_bytes_ = resident_count_ = swapped_count_ = 0;
    clock_ = o.clock_;
    dirty_.clear();
    // La copie matérialise en RAM : les tuiles sur scratch sont rechargées.
    auto& mm = MemoryManager::instance();
    for (std::size_t i = 0; i < o.tiles_.size(); ++i) {
        const Tile* src = o.tile_for_read(i);
        if (!src || src->px.empty()) continue;
        tiles_[i].px    = src->px;
        tiles_[i].stamp = src->stamp;
        tiles_[i].dirty = true;
        mm.on_alloc(tiles_[i].px.size());
        resident_bytes_ += tiles_[i].px.size();
        ++resident_count_;
        dirty_.push_back(i);
    }
}

TileStore::TileStore(const TileStore& o) { copy_from(o); }

TileStore& TileStore::operator=(const TileStore& o) {
    if (this != &o) { release_all(); copy_from(o); }
    return *this;
}

TileStore::TileStore(TileStore&& o) noexcept
    : width_(o.width_), height_(o.height_), tile_size_(o.tile_size_),
      cols_(o.cols_), rows_(o.rows_), revision_(o.revision_),
      tiles_(std::move(o.tiles_)), dirty_(std::move(o.dirty_)),
      resident_bytes_(o.resident_bytes_), resident_count_(o.resident_count_),
      swapped_count_(o.swapped_count_), clock_(o.clock_) {
    o.tiles_.clear();
    o.resident_bytes_ = o.resident_count_ = o.swapped_count_ = 0;
}

TileStore& TileStore::operator=(TileStore&& o) noexcept {
    if (this != &o) {
        release_all();
        width_ = o.width_; height_ = o.height_; tile_size_ = o.tile_size_;
        cols_ = o.cols_; rows_ = o.rows_; revision_ = o.revision_;
        tiles_ = std::move(o.tiles_); dirty_ = std::move(o.dirty_);
        resident_bytes_ = o.resident_bytes_; resident_count_ = o.resident_count_;
        swapped_count_ = o.swapped_count_; clock_ = o.clock_;
        o.tiles_.clear();
        o.resident_bytes_ = o.resident_count_ = o.swapped_count_ = 0;
    }
    return *this;
}

// ── Pagination ───────────────────────────────────────────────────────────────

void TileStore::page_in(Tile& t) const {
    if (!t.px.empty() || t.scratch < 0) return;
    auto& mm = MemoryManager::instance();
    t.px.resize(tile_bytes());
    if (!mm.read_block(t.scratch, t.px.data(), t.px.size())) {
        // Lecture impossible : on repart d'une tuile vide plutôt que de
        // propager des pixels corrompus.
        std::fill(t.px.begin(), t.px.end(), std::byte{0});
    }
    mm.free_block(t.scratch, tile_bytes());
    t.scratch = -1;
    mm.on_alloc(t.px.size());
    resident_bytes_ += t.px.size();
    ++resident_count_;
    if (swapped_count_) --swapped_count_;
}

std::size_t TileStore::evict_cold(std::size_t bytes) const {
    if (!bytes) return 0;
    auto& mm = MemoryManager::instance();

    // Candidats : tuiles résidentes, triées par ancienneté d'accès.
    std::vector<std::size_t> cand;
    cand.reserve(resident_count_);
    for (std::size_t i = 0; i < tiles_.size(); ++i)
        if (!tiles_[i].px.empty()) cand.push_back(i);
    std::sort(cand.begin(), cand.end(), [this](std::size_t a, std::size_t b) {
        return tiles_[a].stamp < tiles_[b].stamp;
    });

    std::size_t freed = 0;
    for (std::size_t i : cand) {
        if (freed >= bytes) break;
        Tile& t = tiles_[i];
        const std::int64_t off = mm.write_block(t.px.data(), t.px.size());
        if (off < 0) break; // scratch indisponible : on garde tout en RAM
        const std::size_t n = t.px.size();
        t.scratch = off;
        t.px.clear();
        t.px.shrink_to_fit();
        mm.on_free(n);
        resident_bytes_ -= n;
        --resident_count_;
        ++swapped_count_;
        freed += n;
    }
    return freed;
}

void TileStore::begin_record() {
    recording_ = true;
    record_.clear();
    recorded_.assign(tiles_.size(), 0);
}

TileStore::UndoStep TileStore::commit_record() {
    recording_ = false;
    recorded_.clear();
    recorded_.shrink_to_fit();
    return std::move(record_);
}

std::size_t TileStore::step_bytes(const UndoStep& s) {
    std::size_t n = 0;
    for (const auto& e : s) n += e.second.size();
    return n;
}

void TileStore::apply_step(UndoStep& step) {
    auto& mm = MemoryManager::instance();
    for (auto& entry : step) {
        const std::size_t idx = entry.first;
        if (idx >= tiles_.size()) continue;
        Tile& t = tiles_[idx];
        if (t.px.empty() && t.scratch >= 0) page_in(t);

        // Le contenu courant devient la pré-image du pas inverse.
        std::vector<std::byte> cur = std::move(t.px);
        t.px.clear();
        if (!cur.empty()) {
            mm.on_free(cur.size());
            resident_bytes_ -= cur.size();
            if (resident_count_) --resident_count_;
        }
        t.px = std::move(entry.second);
        if (!t.px.empty()) {
            mm.on_alloc(t.px.size());
            resident_bytes_ += t.px.size();
            ++resident_count_;
        }
        entry.second = std::move(cur);
        t.stamp = ++clock_;
        mark_dirty(idx);
    }
    ++revision_;
}

TileStore::Tile& TileStore::tile_for_write(std::size_t index) {
    Tile& t = tiles_[index];

    // Capturer la pré-image avant toute modification — et avant l'allocation,
    // pour qu'une tuile encore absente soit enregistrée comme telle.
    if (recording_ && index < recorded_.size() && !recorded_[index]) {
        recorded_[index] = 1;
        if (t.px.empty() && t.scratch < 0) {
            record_.emplace_back(index, std::vector<std::byte>{});
        } else {
            if (t.px.empty()) page_in(t);
            record_.emplace_back(index, t.px);
        }
    }

    t.stamp = ++clock_;
    if (!t.px.empty()) return t;
    if (t.scratch >= 0) { page_in(t); return t; }

    // Nouvelle tuile : vérifier le budget avant d'allouer.
    auto& mm = MemoryManager::instance();
    const std::size_t need = tile_bytes();
    if (mm.resident_bytes() + need > mm.budget_bytes())
        evict_cold(mm.resident_bytes() + need - mm.budget_bytes());

    t.px.assign(need, std::byte{0});
    mm.on_alloc(need);
    resident_bytes_ += need;
    ++resident_count_;
    return t;
}

const TileStore::Tile* TileStore::tile_for_read(std::size_t index) const {
    Tile& t = tiles_[index];
    if (t.px.empty() && t.scratch < 0) return nullptr; // jamais écrite
    t.stamp = ++clock_;
    if (t.px.empty()) page_in(t);
    return &t;
}

void TileStore::mark_dirty(std::size_t index) const {
    Tile& t = tiles_[index];
    if (t.dirty) return;
    t.dirty = true;
    dirty_.push_back(index);
}

void TileStore::clear_dirty() const {
    for (std::size_t i : dirty_) tiles_[i].dirty = false;
    dirty_.clear();
}

void TileStore::mark_all_dirty() const {
    dirty_.clear();
    for (std::size_t i = 0; i < tiles_.size(); ++i) {
        tiles_[i].dirty = true;
        dirty_.push_back(i);
    }
}

// ── Pixels ───────────────────────────────────────────────────────────────────

void TileStore::write_pixel(std::size_t x, std::size_t y, std::uint32_t rgba) {
    if (x >= width_ || y >= height_) return;
    const std::size_t index = (y / tile_size_) * cols_ + x / tile_size_;
    Tile& t = tile_for_write(index);
    const std::size_t local = ((y % tile_size_) * tile_size_ + x % tile_size_) * 4;
    t.px[local + 0] = std::byte( rgba        & 0xff);
    t.px[local + 1] = std::byte((rgba >>  8) & 0xff);
    t.px[local + 2] = std::byte((rgba >> 16) & 0xff);
    t.px[local + 3] = std::byte((rgba >> 24) & 0xff);
    mark_dirty(index);
    ++revision_;
}

std::uint32_t TileStore::read_pixel(std::size_t x, std::size_t y) const noexcept {
    if (x >= width_ || y >= height_) return 0u;
    const std::size_t index = (y / tile_size_) * cols_ + x / tile_size_;
    const Tile* t = tile_for_read(index);
    if (!t || t->px.empty()) return 0u;
    const std::size_t local = ((y % tile_size_) * tile_size_ + x % tile_size_) * 4;
    return  std::uint32_t(std::to_integer<unsigned char>(t->px[local + 0]))
         | (std::uint32_t(std::to_integer<unsigned char>(t->px[local + 1])) <<  8)
         | (std::uint32_t(std::to_integer<unsigned char>(t->px[local + 2])) << 16)
         | (std::uint32_t(std::to_integer<unsigned char>(t->px[local + 3])) << 24);
}

std::byte* TileStore::span_for_write(std::size_t x, std::size_t y, std::size_t& run) {
    run = 0;
    if (x >= width_ || y >= height_) return nullptr;
    const std::size_t lx = x % tile_size_, ly = y % tile_size_;
    const std::size_t index = (y / tile_size_) * cols_ + x / tile_size_;
    Tile& t = tile_for_write(index);
    mark_dirty(index);
    run = std::min(tile_size_ - lx, width_ - x);
    return t.px.data() + (ly * tile_size_ + lx) * 4;
}

const std::byte* TileStore::span_for_read(std::size_t x, std::size_t y,
                                          std::size_t& run) const {
    run = 0;
    if (x >= width_ || y >= height_) return nullptr;
    const std::size_t lx = x % tile_size_, ly = y % tile_size_;
    run = std::min(tile_size_ - lx, width_ - x);
    const std::size_t index = (y / tile_size_) * cols_ + x / tile_size_;
    const Tile* t = tile_for_read(index);
    if (!t || t->px.empty()) return nullptr;   // `run` reste valide : zone vide
    return t->px.data() + (ly * tile_size_ + lx) * 4;
}

void TileStore::touch_region(std::size_t x, std::size_t y, std::size_t w,
                             std::size_t h, std::uint32_t rgba) {
    if (!width_ || !height_ || x >= width_ || y >= height_) return;
    const std::size_t x2 = std::min(width_,  x + w);
    const std::size_t y2 = std::min(height_, y + h);

    std::byte px[4] = { std::byte( rgba        & 0xff), std::byte((rgba >>  8) & 0xff),
                        std::byte((rgba >> 16) & 0xff), std::byte((rgba >> 24) & 0xff) };

    for (std::size_t py = y; py < y2; ++py) {
        std::size_t px_x = x;
        while (px_x < x2) {
            const std::size_t tx    = px_x / tile_size_;
            const std::size_t index = (py / tile_size_) * cols_ + tx;
            const std::size_t span  = std::min(x2, (tx + 1) * tile_size_) - px_x;
            Tile& t = tile_for_write(index);
            std::size_t local = ((py % tile_size_) * tile_size_ + px_x % tile_size_) * 4;
            for (std::size_t i = 0; i < span; ++i, local += 4)
                std::memcpy(t.px.data() + local, px, 4);
            mark_dirty(index);
            px_x += span;
        }
    }
    ++revision_; // une révision par opération, pas par pixel
}

void TileStore::clear() noexcept {
    auto& mm = MemoryManager::instance();
    for (auto& t : tiles_) {
        if (!t.px.empty()) { mm.on_free(t.px.size()); t.px.clear(); t.px.shrink_to_fit(); }
        if (t.scratch >= 0) { mm.free_block(t.scratch, tile_bytes()); t.scratch = -1; }
    }
    resident_bytes_ = resident_count_ = swapped_count_ = 0;
    mark_all_dirty();
    ++revision_;
}

// ── Snapshots ────────────────────────────────────────────────────────────────

void TileStore::snapshot_region(std::size_t x0, std::size_t y0,
                                std::size_t x1, std::size_t y1,
                                std::byte* out) const {
    if (!out) return;
    x1 = std::min(x1, width_);  y1 = std::min(y1, height_);
    if (x0 >= x1 || y0 >= y1) return;

    const std::size_t row_bytes = (x1 - x0) * 4;
    for (std::size_t py = y0; py < y1; ++py) {
        std::byte* drow = out + (py * width_) * 4;
        // Par défaut la ligne est transparente ; les tuiles présentes écrasent.
        std::memset(drow + x0 * 4, 0, row_bytes);
        std::size_t px = x0;
        while (px < x1) {
            const std::size_t tx    = px / tile_size_;
            const std::size_t index = (py / tile_size_) * cols_ + tx;
            const std::size_t span  = std::min(x1, (tx + 1) * tile_size_) - px;
            const Tile* t = tile_for_read(index);
            if (t && !t->px.empty()) {
                const std::size_t local =
                    ((py % tile_size_) * tile_size_ + px % tile_size_) * 4;
                std::memcpy(drow + px * 4, t->px.data() + local, span * 4);
            }
            px += span;
        }
    }
}

void TileStore::snapshot_into(std::byte* out) const {
    snapshot_region(0, 0, width_, height_, out);
}

std::vector<std::byte> TileStore::snapshot_rgba() const {
    std::vector<std::byte> result(width_ * height_ * 4, std::byte{0});
    snapshot_into(result.data());
    return result;
}

void TileStore::restore_rgba(const std::vector<std::byte>& pixels) {
    if (pixels.size() != width_ * height_ * 4) return;
    // Écriture par lignes de tuile (memcpy) au lieu de width*height appels à
    // write_pixel : l'annulation devient quasi instantanée.
    for (std::size_t py = 0; py < height_; ++py) {
        const std::byte* srow = pixels.data() + (py * width_) * 4;
        std::size_t px = 0;
        while (px < width_) {
            const std::size_t tx    = px / tile_size_;
            const std::size_t index = (py / tile_size_) * cols_ + tx;
            const std::size_t span  = std::min(width_, (tx + 1) * tile_size_) - px;
            Tile& t = tile_for_write(index);
            const std::size_t local =
                ((py % tile_size_) * tile_size_ + px % tile_size_) * 4;
            std::memcpy(t.px.data() + local, srow + px * 4, span * 4);
            mark_dirty(index);
            px += span;
        }
    }
    ++revision_;
}

// ── Accès tuile ──────────────────────────────────────────────────────────────

const std::byte* TileStore::tile_pixels(std::size_t index) const {
    if (index >= tiles_.size()) return nullptr;
    const Tile* t = tile_for_read(index);
    return (t && !t->px.empty()) ? t->px.data() : nullptr;
}

void TileStore::tile_origin(std::size_t index, std::size_t& x, std::size_t& y) const {
    if (index >= tiles_.size() || !cols_) { x = y = 0; return; }
    x = (index % cols_) * tile_size_;
    y = (index / cols_) * tile_size_;
}

// ── Export ───────────────────────────────────────────────────────────────────

bool TileStore::save_ppm(const std::string& path) const {
    const auto pixels = snapshot_rgba();
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    output << "P6\n" << width_ << ' ' << height_ << "\n255\n";
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        output.put(static_cast<char>(std::to_integer<unsigned char>(pixels[i + 0])));
        output.put(static_cast<char>(std::to_integer<unsigned char>(pixels[i + 1])));
        output.put(static_cast<char>(std::to_integer<unsigned char>(pixels[i + 2])));
    }
    return output.good();
}

} // namespace creative::engine
