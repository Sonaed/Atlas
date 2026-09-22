#pragma once
// Atlas — surface de rendu GPU native (Wayland + EGL + GLES2).
//
// Crée une sous-surface Wayland accrochée à la fenêtre GTK, lui associe un
// contexte EGL et y présente un TileStore via NativeRenderer.
//
// État : opt-in (CREATIVE_ENABLE_GPU=1). Limites connues, qui empêchent de
// l'activer par défaut — voir docs/ARCHITECTURE.md, « Rendu GPU » :
//   * la sous-surface est placée au-dessus de GTK et masque ce que Cairo
//     dessine sur le canvas (damier, curseur, sélection) ;
//   * NativeRenderer étire la texture sur toute la zone, sans marges ni zoom ;
//   * l'UI ne lui passe que le calque actif, pas la composition.
#include "engine/render/native_renderer.h"
#include "engine/tiles/tile_store.h"
#include "engine/layers/layer_compositor.h"
#include "engine/fusion/fusion_creator_engine.h"
#include <gtk/gtk.h>
#include <wayland-client.h>
#include <wayland-egl-core.h>
#include <EGL/egl.h>
#include <memory>
#include <mutex>

namespace creative::ui {

class NativeCanvasSurface {
public:
    explicit NativeCanvasSurface(GtkWidget* host) : host_(host) {}
    ~NativeCanvasSurface() { destroy(); }
    NativeCanvasSurface(const NativeCanvasSurface&)            = delete;
    NativeCanvasSurface& operator=(const NativeCanvasSurface&) = delete;

    // Crée sous-surface, contexte EGL et renderer. false si le backend n'est
    // pas Wayland ou si une étape EGL échoue — l'appelant repasse en CPU.
    bool initialize();
    void resize(int w, int h);
    // Replace la sous-surface sur l'allocation GTK du widget hôte.
    void sync_layout();

    // Présente directement un TileStore déjà composé.
    void render(const creative::engine::TileStore& store) {
        if (renderer_.ready()) renderer_.render(store);
    }

    // Compose la pile sur CPU puis la présente. Le tampon de composition prend
    // les dimensions du document ; l'ancienne version utilisait un TileStore
    // 256x256 en dur, quelle que soit la taille réelle. Le tampon est réutilisé
    // d'une frame à l'autre tant que la taille ne change pas.
    void render(const creative::engine::LayerStack& stack) {
        if (!renderer_.ready() || !stack.width() || !stack.height()) return;
        if (!compose_buf_ || compose_buf_->width() != stack.width()
                          || compose_buf_->height() != stack.height())
            compose_buf_ = std::make_unique<creative::engine::TileStore>(
                               stack.width(), stack.height());
        compositor_.compose(stack, *compose_buf_);
        renderer_.render(*compose_buf_);
    }

    bool gpu_ready() const { return renderer_.ready(); }

private:
    static void global_add(void*, wl_registry*, uint32_t, const char*, uint32_t);
    static void global_remove(void*, wl_registry*, uint32_t);
    void destroy();

    GtkWidget*        host_          = nullptr;
    GdkSurface*       gdk_           = nullptr;
    wl_display*       display_       = nullptr;
    wl_compositor*    wl_compositor_ = nullptr;
    wl_subcompositor* subcompositor_ = nullptr;
    wl_surface*       surface_       = nullptr;
    wl_subsurface*    subsurface_    = nullptr;
    wl_egl_window*    egl_window_    = nullptr;
    EGLDisplay        egl_display_   = EGL_NO_DISPLAY;
    EGLConfig         config_        = nullptr;
    EGLContext        context_       = EGL_NO_CONTEXT;
    EGLSurface        egl_surface_   = EGL_NO_SURFACE;

    creative::render::NativeRenderer             renderer_;
    creative::engine::FusionCreatorEngine        fusions_;
    creative::engine::LayerCompositor            compositor_;
    std::unique_ptr<creative::engine::TileStore> compose_buf_;

    // Un seul contexte EGL à la fois dans le processus.
    static std::mutex           context_mutex_;
    static EGLContext           active_context_;
    static NativeCanvasSurface* context_owner_;
};

} // namespace creative::ui
