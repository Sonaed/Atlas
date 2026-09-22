#include "engine/memory/memory_manager.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace creative::engine {

MemoryManager& MemoryManager::instance() {
    static MemoryManager m;
    return m;
}

std::size_t MemoryManager::physical_ram_bytes() {
#if defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long psize = ::sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0)
        return static_cast<std::size_t>(pages) * static_cast<std::size_t>(psize);
#endif
    return std::size_t(4) << 30; // 4 Gio par défaut si indétectable
}

MemoryManager::MemoryManager() {
    // 60 % de la RAM physique, comme le réglage par défaut de Photoshop.
    budget_ = physical_ram_bytes() / 100 * 60;
    // Scratch par défaut : $XDG_CACHE_HOME/atlas, sinon ~/.cache/atlas.
    // Pas /tmp : sur Arch (et beaucoup de distributions) /tmp est un tmpfs,
    // donc en RAM — y évincer des tuiles ne libérerait rien du tout.
    // Dernier recours seulement : $TMPDIR puis /tmp.
    std::string dir;
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) dir = std::string(x) + "/atlas";
    else if (const char* h = std::getenv("HOME"); h && *h)      dir = std::string(h) + "/.cache/atlas";
    if (!dir.empty()) {
        ::mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0700); // ~/.cache si absent
        ::mkdir(dir.c_str(), 0700);
    }
    struct ::stat st {};
    if (dir.empty() || ::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)
                    || ::access(dir.c_str(), W_OK) != 0) {
        const char* tmp = std::getenv("TMPDIR");
        dir = tmp && *tmp ? tmp : "/tmp";
    }
    scratch_dir_ = dir;
}

MemoryManager::~MemoryManager() {
    std::lock_guard<std::mutex> lk(mu_);
    if (fd_ >= 0) ::close(fd_);
    if (!scratch_file_.empty()) ::unlink(scratch_file_.c_str());
}

// ── Budget ───────────────────────────────────────────────────────────────────

void MemoryManager::set_budget_bytes(std::size_t bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    // Plancher à 16 Mio : en dessous, Atlas passerait son temps à swapper.
    // Volontairement bas pour qu'un budget serré reste atteignable sur une
    // machine très dotée, où 5 % de la RAM représente déjà plusieurs Gio.
    budget_ = std::max<std::size_t>(bytes, std::size_t(16) << 20);
}

void MemoryManager::set_budget_percent(int percent) {
    percent = std::clamp(percent, 5, 95);
    set_budget_bytes(physical_ram_bytes() / 100 * std::size_t(percent));
}

std::size_t MemoryManager::budget_bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return budget_;
}

// ── Comptabilité ─────────────────────────────────────────────────────────────

void MemoryManager::on_alloc(std::size_t bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    resident_ += bytes;
    peak_ = std::max(peak_, resident_);
}

void MemoryManager::on_free(std::size_t bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    resident_ = bytes > resident_ ? 0 : resident_ - bytes;
}

std::size_t MemoryManager::resident_bytes() const {
    std::lock_guard<std::mutex> lk(mu_); return resident_;
}
std::size_t MemoryManager::swapped_bytes() const {
    std::lock_guard<std::mutex> lk(mu_); return swapped_;
}
std::size_t MemoryManager::peak_bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    // Le pic ne peut pas être sous le résident : si un chemin d'allocation
    // oubliait d'appeler on_alloc, on afficherait une valeur absurde plutôt
    // qu'un simple sous-comptage.
    return std::max(peak_, resident_);
}
bool MemoryManager::over_budget() const {
    std::lock_guard<std::mutex> lk(mu_); return resident_ > budget_;
}
std::size_t MemoryManager::overage_bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return resident_ > budget_ ? resident_ - budget_ : 0;
}

// ── Scratch disk ─────────────────────────────────────────────────────────────

bool MemoryManager::set_scratch_dir(const std::string& dir) {
    // Vérifier que le répertoire est accessible en écriture avant de l'adopter.
    struct ::stat st {};
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (::access(dir.c_str(), W_OK) != 0) return false;

    std::lock_guard<std::mutex> lk(mu_);
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (!scratch_file_.empty()) { ::unlink(scratch_file_.c_str()); scratch_file_.clear(); }
    free_list_.clear();
    scratch_end_ = 0;
    swapped_     = 0;
    scratch_dir_ = dir;
    return true;
}

const std::string& MemoryManager::scratch_dir()  const {
    std::lock_guard<std::mutex> lk(mu_); return scratch_dir_;
}
const std::string& MemoryManager::scratch_file() const {
    std::lock_guard<std::mutex> lk(mu_); return scratch_file_;
}
std::size_t MemoryManager::scratch_bytes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<std::size_t>(scratch_end_);
}
bool MemoryManager::scratch_available() const {
    std::lock_guard<std::mutex> lk(mu_);
    return fd_ >= 0 || !scratch_dir_.empty();
}

bool MemoryManager::ensure_open() {
    if (fd_ >= 0) return true;
    if (scratch_dir_.empty()) return false;

    std::string tmpl = scratch_dir_ + "/atlas-scratch-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    const int fd = ::mkstemp(buf.data());
    if (fd < 0) return false;

    scratch_file_.assign(buf.data());
    // Suppression immédiate du lien : le fichier vit tant que le fd est ouvert
    // et disparaît même si Atlas crashe. Pas de scratch orphelin sur le disque.
    ::unlink(scratch_file_.c_str());
    fd_          = fd;
    scratch_end_ = 0;
    return true;
}

std::int64_t MemoryManager::write_block(const std::byte* data, std::size_t bytes) {
    if (!data || !bytes) return -1;
    std::lock_guard<std::mutex> lk(mu_);
    if (!ensure_open()) return -1;

    // Recycler un bloc libre de taille exacte (les tuiles ont toutes la même
    // taille, donc la correspondance exacte est le cas courant).
    std::int64_t off = -1;
    for (std::size_t i = 0; i < free_list_.size(); ++i) {
        if (free_list_[i].bytes == bytes) {
            off = free_list_[i].off;
            free_list_[i] = free_list_.back();
            free_list_.pop_back();
            break;
        }
    }
    if (off < 0) { off = scratch_end_; scratch_end_ += static_cast<std::int64_t>(bytes); }

    std::size_t done = 0;
    while (done < bytes) {
        const ssize_t n = ::pwrite(fd_, data + done, bytes - done,
                                   static_cast<off_t>(off) + static_cast<off_t>(done));
        if (n <= 0) {
            if (errno == EINTR) continue;
            // Échec d'écriture (disque plein ?) : on rend le bloc et on abandonne.
            free_list_.push_back({off, bytes});
            return -1;
        }
        done += static_cast<std::size_t>(n);
    }
    swapped_ += bytes;
    return off;
}

bool MemoryManager::read_block(std::int64_t offset, std::byte* out, std::size_t bytes) {
    if (offset < 0 || !out || !bytes) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (fd_ < 0) return false;

    std::size_t done = 0;
    while (done < bytes) {
        const ssize_t n = ::pread(fd_, out + done, bytes - done,
                                  static_cast<off_t>(offset) + static_cast<off_t>(done));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

void MemoryManager::free_block(std::int64_t offset, std::size_t bytes) {
    if (offset < 0 || !bytes) return;
    std::lock_guard<std::mutex> lk(mu_);
    free_list_.push_back({offset, bytes});
    swapped_ = bytes > swapped_ ? 0 : swapped_ - bytes;
}

void MemoryManager::reset_scratch() {
    std::lock_guard<std::mutex> lk(mu_);
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    scratch_file_.clear();
    free_list_.clear();
    scratch_end_ = 0;
    swapped_     = 0;
}

} // namespace creative::engine
