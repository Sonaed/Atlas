#pragma once
// Atlas — gestion mémoire centralisée + scratch disk (façon Photoshop).
//
// Principe : toute la mémoire pixel du document passe par ici. Quand le budget
// RAM est dépassé, les tuiles les plus froides sont écrites dans un fichier
// scratch et libérées. Elles sont rechargées à la demande, de façon
// transparente pour l'appelant.
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace creative::engine {

class MemoryManager {
public:
    static MemoryManager& instance();

    // ── Budget ────────────────────────────────────────────────────────────
    // Par défaut : 60 % de la RAM physique détectée (comme Photoshop).
    void        set_budget_bytes(std::size_t bytes);
    std::size_t budget_bytes() const;
    // Confort : pourcentage de la RAM physique totale.
    void        set_budget_percent(int percent);
    static std::size_t physical_ram_bytes();

    // ── Comptabilité ──────────────────────────────────────────────────────
    void        on_alloc(std::size_t bytes);
    void        on_free(std::size_t bytes);
    std::size_t resident_bytes() const;
    std::size_t swapped_bytes()  const;
    std::size_t peak_bytes()     const;
    bool        over_budget()    const;
    // Combien d'octets il faut libérer pour repasser sous le budget (0 sinon).
    std::size_t overage_bytes()  const;

    // ── Scratch disk ──────────────────────────────────────────────────────
    // Répertoire du fichier scratch. Le fichier est créé à la 1re écriture et
    // supprimé à la destruction. Retourne false si le répertoire est inutilisable.
    bool               set_scratch_dir(const std::string& dir);
    const std::string& scratch_dir()  const;
    const std::string& scratch_file() const;
    std::size_t        scratch_bytes() const;
    bool               scratch_available() const;

    // Écrit un bloc et renvoie son offset, ou -1 en cas d'échec.
    // Les blocs libérés sont recyclés (free-list par taille).
    std::int64_t write_block(const std::byte* data, std::size_t bytes);
    bool         read_block(std::int64_t offset, std::byte* out, std::size_t bytes);
    void         free_block(std::int64_t offset, std::size_t bytes);

    // Vide le scratch (appelé à la fermeture d'un document).
    void reset_scratch();

    MemoryManager(const MemoryManager&)            = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

private:
    MemoryManager();
    ~MemoryManager();
    bool ensure_open();          // ouvre le fichier scratch si besoin (appelant verrouillé)

    mutable std::mutex mu_;
    std::size_t budget_   = 0;
    std::size_t resident_ = 0;
    std::size_t swapped_  = 0;
    std::size_t peak_     = 0;

    std::string  scratch_dir_;
    std::string  scratch_file_;
    int          fd_        = -1;
    std::int64_t scratch_end_ = 0;   // taille logique du fichier

    struct FreeBlock { std::int64_t off; std::size_t bytes; };
    std::vector<FreeBlock> free_list_;
};

} // namespace creative::engine
