#include "ui/gtk/native_canvas_surface.h"
#include <gdk/wayland/gdkwayland.h>
#include <algorithm>
#include <cstring>
#include <string>

namespace creative::ui {

std::mutex           NativeCanvasSurface::context_mutex_;
EGLContext           NativeCanvasSurface::active_context_ = EGL_NO_CONTEXT;
NativeCanvasSurface* NativeCanvasSurface::context_owner_  = nullptr;

// Registre Wayland : on ne cherche que wl_subcompositor, nécessaire pour
// accrocher notre surface EGL sous la surface GTK.
void NativeCanvasSurface::global_add(void* data, wl_registry* r, uint32_t name,
                                     const char* iface, uint32_t version) {
    auto* self = static_cast<NativeCanvasSurface*>(data);
    if (!std::strcmp(iface, "wl_subcompositor"))
        self->subcompositor_ = static_cast<wl_subcompositor*>(
            wl_registry_bind(r, name, &wl_subcompositor_interface, std::min(version, 1u)));
}
void NativeCanvasSurface::global_remove(void*, wl_registry*, uint32_t) {}

bool NativeCanvasSurface::initialize() {
    std::lock_guard<std::mutex> lock(context_mutex_);
    if (context_ != EGL_NO_CONTEXT || active_context_ != EGL_NO_CONTEXT) {
        g_warning("Atlas : second contexte EGL refusé");
        return false;
    }

    // 1. Surface GDK du widget hôte — doit être Wayland (pas de repli X11).
    gdk_ = gtk_native_get_surface(gtk_widget_get_native(host_));
    if (!gdk_ || !GDK_IS_WAYLAND_SURFACE(gdk_)) return false;

    display_       = gdk_wayland_display_get_wl_display(gdk_display_get_default());
    wl_compositor_ = gdk_wayland_display_get_wl_compositor(gdk_display_get_default());
    if (!display_ || !wl_compositor_) return false;

    // 2. Notre propre wl_surface.
    surface_ = wl_compositor_create_surface(wl_compositor_);
    if (!surface_) return false;

    // Région d'entrée vide : la surface n'est qu'un calque visuel. Si elle
    // captait le pointeur, les gestes de dessin n'atteindraient plus GTK.
    if (auto* input_region = wl_compositor_create_region(wl_compositor_)) {
        wl_surface_set_input_region(surface_, input_region);
        wl_region_destroy(input_region);
    }

    // 3. Sous-surface rattachée à la surface GTK, en mode désynchronisé pour
    //    pouvoir présenter sans attendre le cycle de rendu de GTK.
    auto* registry = wl_display_get_registry(display_);
    static const wl_registry_listener listener{ global_add, global_remove };
    wl_registry_add_listener(registry, &listener, this);
    wl_display_roundtrip(display_);
    if (subcompositor_) {
        subsurface_ = wl_subcompositor_get_subsurface(
            subcompositor_, surface_, gdk_wayland_surface_get_wl_surface(gdk_));
        if (subsurface_) wl_subsurface_set_desync(subsurface_);
    }

    // 4. EGL : display, config RGBA8, contexte GLES2.
    egl_display_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(display_));
    if (egl_display_ == EGL_NO_DISPLAY || !eglInitialize(egl_display_, nullptr, nullptr))
        return false;
    eglBindAPI(EGL_OPENGL_ES_API);

    const EGLint cfg[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE };
    EGLint count = 0;
    if (!eglChooseConfig(egl_display_, cfg, &config_, 1, &count) || !count) return false;

    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    context_ = eglCreateContext(egl_display_, config_, EGL_NO_CONTEXT, ctx_attr);
    if (context_ == EGL_NO_CONTEXT) return false;
    active_context_ = context_;
    context_owner_  = this;

    // 5. Fenêtre EGL (1x1, redimensionnée par resize()) et contexte courant.
    egl_window_ = wl_egl_window_create(surface_, 1, 1);
    if (!egl_window_) return false;
    egl_surface_ = eglCreateWindowSurface(egl_display_, config_,
        reinterpret_cast<EGLNativeWindowType>(egl_window_), nullptr);
    if (egl_surface_ == EGL_NO_SURFACE) return false;
    if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, context_)) return false;

    // 6. Renderer + validation des shaders de fusion.
    if (!renderer_.initialize(egl_display_, egl_surface_, 1, 1)) return false;
    std::string shader_error;
    if (!fusions_.compile_gpu(shader_error)) {
        g_warning("Atlas : validation GLSL des fusions échouée : %s", shader_error.c_str());
        return false;
    }
    g_message("Atlas : shaders de fusion GLSL compilés et liés");
    return true;
}

void NativeCanvasSurface::resize(int w, int h) {
    if (egl_window_) wl_egl_window_resize(egl_window_, w, h, 0, 0);
    if (renderer_.ready()) renderer_.resize(w, h);
}

void NativeCanvasSurface::sync_layout() {
    if (!subsurface_ || !host_) return;
    // Position du widget dans la fenêtre native, là où la sous-surface doit
    // se placer. gtk_widget_get_allocation (déprécié) donnait une position
    // relative au widget parent, pas à la fenêtre : décalage dès que le
    // canvas n'est pas un enfant direct de celle-ci.
    graphene_rect_t b;
    GtkWidget* native = GTK_WIDGET(gtk_widget_get_native(host_));
    if (!native || !gtk_widget_compute_bounds(host_, native, &b)) return;
    wl_subsurface_set_position(subsurface_, int(b.origin.x), int(b.origin.y));
    wl_subsurface_place_above(subsurface_, gdk_wayland_surface_get_wl_surface(gdk_));
    wl_surface_commit(surface_);
    wl_display_flush(display_);
}

void NativeCanvasSurface::destroy() {
    renderer_.destroy();
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_surface_ != EGL_NO_SURFACE) eglDestroySurface(egl_display_, egl_surface_);
        if (context_ != EGL_NO_CONTEXT)     eglDestroyContext(egl_display_, context_);
        eglTerminate(egl_display_);
    }
    if (egl_window_) wl_egl_window_destroy(egl_window_);
    if (subsurface_) wl_subsurface_destroy(subsurface_);
    if (surface_)    wl_surface_destroy(surface_);
    {
        std::lock_guard<std::mutex> lock(context_mutex_);
        if (context_owner_ == this) { active_context_ = EGL_NO_CONTEXT; context_owner_ = nullptr; }
    }
    egl_surface_ = EGL_NO_SURFACE;
    context_     = EGL_NO_CONTEXT;
    egl_display_ = EGL_NO_DISPLAY;
    egl_window_  = nullptr;
    subsurface_  = nullptr;
    surface_     = nullptr;
}

} // namespace creative::ui
