#pragma once
// Atlas — petits utilitaires de ressources : pool de tampons, profileur,
// cache de textures de brosses.
#include "engine/cache/lru_cache.h"
#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace creative::engine {

// Pool de tampons octets réutilisables, pour éviter de réallouer des blocs
// temporaires dans les chemins chauds.
//
// Sémantique par valeur : acquire() *sort* un tampon du pool et le rend à
// l'appelant, release() l'y remet. L'ancienne version renvoyait une référence
// vers un élément d'un std::vector interne ; le push_back suivant pouvait
// réallouer ce vector et laisser la référence pendante (crash différé).
// Ici personne ne garde de pointeur vers le stockage du pool.
class BufferManager {
public:
    // Renvoie un tampon d'au moins `bytes` octets (contenu non initialisé si
    // recyclé). Le tampon appartient à l'appelant jusqu'à release().
    std::vector<std::byte> acquire(std::size_t bytes) {
        for (std::size_t i = 0; i < free_.size(); ++i) {
            if (free_[i].capacity() >= bytes) {
                std::vector<std::byte> b = std::move(free_[i]);
                free_[i] = std::move(free_.back());
                free_.pop_back();
                b.resize(bytes);
                active_ += b.capacity();
                return b;
            }
        }
        active_ += bytes;
        return std::vector<std::byte>(bytes);
    }

    // Remet un tampon dans le pool pour réutilisation ultérieure.
    void release(std::vector<std::byte>&& b) {
        const std::size_t cap = b.capacity();
        active_ = cap > active_ ? 0 : active_ - cap;
        if (free_.size() < kMaxPooled) free_.push_back(std::move(b));
    }

    // Libère tous les tampons en attente dans le pool.
    void trim() { free_.clear(); free_.shrink_to_fit(); }

    std::size_t active_bytes() const { return active_; }
    std::size_t pooled_count() const { return free_.size(); }

private:
    static constexpr std::size_t kMaxPooled = 32; // borne la mémoire retenue
    std::vector<std::vector<std::byte>> free_;
    std::size_t active_ = 0;
};

// Profileur minimal : cumule le temps passé par étiquette.
//   { Profiler::Scope s(prof, "composite"); ... }  // mesure le bloc
class Profiler {
public:
    class Scope {
    public:
        Scope(Profiler& p, std::string n)
            : p_(p), n_(std::move(n)), start_(std::chrono::steady_clock::now()) {}
        ~Scope() {
            p_.record(n_, std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start_).count());
        }
    private:
        Profiler& p_;
        std::string n_;
        std::chrono::steady_clock::time_point start_;
    };
    void   record(const std::string& n, double ms) { samples_[n] += ms; }
    double total_ms(const std::string& n) const {
        auto i = samples_.find(n);
        return i == samples_.end() ? 0 : i->second;
    }
private:
    std::unordered_map<std::string, double> samples_;
};

// Cache LRU des pixels de brosses bitmap destinés au GPU (clé = identifiant
// de brosse). Stocke des copies CPU ; l'upload effectif reste à la charge du
// renderer.
class GpuTextureCache {
public:
    explicit GpuTextureCache(std::size_t capacity) : cache_(capacity) {}
    void upload(std::size_t tile, std::vector<std::byte> pixels) {
        cache_.put(tile, std::move(pixels));
        ++uploads_;
    }
    auto        texture(std::size_t tile) { return cache_.get(tile); }
    std::size_t uploads() const { return uploads_; }
    std::size_t size()    const { return cache_.size(); }
private:
    LruCache<std::size_t, std::vector<std::byte>> cache_;
    std::size_t uploads_ = 0;
};

} // namespace creative::engine
