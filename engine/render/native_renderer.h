#pragma once
#include "engine/tiles/tile_store.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES3/gl3.h>
#include <vector>

namespace creative::render {

// Présente un TileStore à l'écran via EGL/GLES.
//
// Tout ce qui est ici tourne sur le GPU : upload des tuiles modifiées puis
// blit plein écran. Aucune composition n'est faite côté CPU dans ce chemin.
class NativeRenderer {
public:
    ~NativeRenderer();
    bool initialize(EGLDisplay, EGLSurface, int w, int h);
    void resize(int w, int h);
    void render(const creative::engine::TileStore&);
    void destroy();
    bool ready() const { return ready_; }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    GLuint     program_ = 0, texture_ = 0;
    GLint      tex_loc_ = -1;
    int        w_ = 0, h_ = 0;
    GLsizei    tex_w_ = 0, tex_h_ = 0;   // taille actuellement allouée
    bool       ready_ = false;
    std::vector<std::byte> staging_;      // tuiles de bord (rognées)
};

} // namespace creative::render
