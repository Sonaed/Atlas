#include "engine/render/native_renderer.h"
#include <algorithm>
#include <cstring>

namespace creative::render {

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { glDeleteShader(s); return 0; }
    return s;
}

bool NativeRenderer::initialize(EGLDisplay d, EGLSurface s, int w, int h) {
    display_ = d; surface_ = s; w_ = w; h_ = h;

    // v est inversé : la ligne 0 du canvas est en haut de l'écran.
    // Sans cette inversion le document s'affiche à l'envers.
    static const char* VS =
        "attribute vec2 p;varying vec2 u;"
        "void main(){u=vec2((p.x+1.0)*0.5,1.0-(p.y+1.0)*0.5);gl_Position=vec4(p,0,1);}";
    static const char* FS =
        "precision mediump float;varying vec2 u;uniform sampler2D t;"
        "void main(){gl_FragColor=texture2D(t,u);}";

    GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return false; }

    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glBindAttribLocation(program_, 0, "p");
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(program_); program_ = 0; return false; }

    tex_loc_ = glGetUniformLocation(program_, "t");
    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    // Pas de mipmaps : le blit est 1:1, les générer à chaque frame coûtait
    // plus cher que le rendu lui-même.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    return ready_ = true;
}

void NativeRenderer::resize(int w, int h) {
    w_ = w; h_ = h;
    if (ready_) glViewport(0, 0, w_, h_);
}

void NativeRenderer::render(const creative::engine::TileStore& store) {
    if (!ready_ || !store.width() || !store.height()) return;

    const GLsizei sw = static_cast<GLsizei>(store.width());
    const GLsizei sh = static_cast<GLsizei>(store.height());
    const std::size_t ts = store.tile_size();

    glBindTexture(GL_TEXTURE_2D, texture_);

    if (tex_w_ != sw || tex_h_ != sh) {
        // Allocation unique du stockage : les frames suivantes ne font que des
        // glTexSubImage2D, au lieu de réallouer la texture entière.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sw, sh, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        tex_w_ = sw; tex_h_ = sh;
        store.mark_all_dirty();
    }

    // Upload incrémental : seules les tuiles touchées depuis la dernière frame
    // remontent sur le GPU.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    for (std::size_t idx : store.dirty_tiles()) {
        const std::byte* px = store.tile_pixels(idx);
        if (!px) continue;
        std::size_t ox = 0, oy = 0;
        store.tile_origin(idx, ox, oy);
        if (ox >= store.width() || oy >= store.height()) continue;

        const std::size_t tw = std::min(ts, store.width()  - ox);
        const std::size_t th = std::min(ts, store.height() - oy);

        if (tw == ts && th == ts) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, GLint(ox), GLint(oy),
                            GLsizei(tw), GLsizei(th),
                            GL_RGBA, GL_UNSIGNED_BYTE, px);
        } else {
            // Tuile de bord : compacter la sous-région avant l'upload (évite
            // GL_UNPACK_ROW_LENGTH, absent en GLES2).
            staging_.resize(tw * th * 4);
            for (std::size_t r = 0; r < th; ++r)
                std::memcpy(staging_.data() + r * tw * 4,
                            px + (r * ts) * 4, tw * 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, GLint(ox), GLint(oy),
                            GLsizei(tw), GLsizei(th),
                            GL_RGBA, GL_UNSIGNED_BYTE, staging_.data());
        }
    }
    store.clear_dirty();

    // Blit plein écran dans le framebuffer par défaut.
    // L'ancien code liait un FBO sans attachement : le framebuffer était
    // incomplet, tous les dessins étaient silencieusement ignorés et la
    // fenêtre restait vide — d'où le repli permanent sur le rendu CPU.
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w_, h_);
    glClearColor(.08f, .10f, .14f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    if (tex_loc_ >= 0) glUniform1i(tex_loc_, 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    static const GLfloat quad[] = { -1,-1,  1,-1,  -1,1,  1,1 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);

    glDisable(GL_BLEND);
    eglSwapBuffers(display_, surface_);
}

void NativeRenderer::destroy() {
    if (display_ != EGL_NO_DISPLAY)
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (texture_) glDeleteTextures(1, &texture_);
    if (program_) glDeleteProgram(program_);
    texture_ = program_ = 0;
    tex_w_ = tex_h_ = 0;
    ready_ = false;
}

NativeRenderer::~NativeRenderer() { destroy(); }

} // namespace creative::render
