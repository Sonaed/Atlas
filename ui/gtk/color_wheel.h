#pragma once
// Atlas — roue chromatique « anneau + triangle » (façon Krita / Painter).
//
//   * L'anneau choisit la teinte.
//   * Le triangle choisit saturation et valeur : son sommet pur est la teinte
//     pleine, les deux autres le blanc et le noir.
//
// Le triangle est fixe (sommet de teinte en haut) pour que la main mémorise
// les positions. Le rendu de l'anneau est mis en cache par taille, celui du
// triangle par taille et par teinte : on ne rasterise que ce qui a changé.
#include <gtk/gtk.h>
#include <functional>

namespace creative::ui {

struct Hsv { double h = 0, s = 0, v = 0; };   // h en degrés [0,360), s et v en [0,1]

Hsv     rgb_to_hsv(double r, double g, double b);
GdkRGBA hsv_to_rgba(const Hsv& c);

class ColorWheel {
public:
    // Appelé à chaque changement de couleur par l'utilisateur (pendant le
    // glissement compris), avec la couleur en RGBA.
    using OnChange = std::function<void(const GdkRGBA&)>;

    // Crée une roue sur le tas. Sa durée de vie est liée à celle de son widget :
    // elle est détruite automatiquement quand GTK finalise ce widget. Ne pas
    // la supprimer à la main, et ne pas garder le pointeur au-delà du widget.
    static ColorWheel* create();
    ~ColorWheel();
    ColorWheel(const ColorWheel&)            = delete;
    ColorWheel& operator=(const ColorWheel&) = delete;

    GtkWidget* widget() const { return area_; }
    void       set_on_change(OnChange cb) { on_change_ = std::move(cb); }

    // Met à jour la roue depuis l'extérieur (pipette, préréglage…) sans
    // rappeler on_change. Conserve la teinte quand la couleur est grise, pour
    // que la roue ne saute pas au rouge quand on choisit du noir.
    void set_rgba(const GdkRGBA& c);
    GdkRGBA rgba() const { return hsv_to_rgba(hsv_); }

private:
    ColorWheel();
    enum class Drag { None, Ring, Triangle };

    static void draw_cb(GtkDrawingArea*, cairo_t*, int w, int h, gpointer self);
    static void on_press(GtkGestureDrag*, double x, double y, gpointer self);
    static void on_update(GtkGestureDrag*, double dx, double dy, gpointer self);
    static void on_end(GtkGestureDrag*, double dx, double dy, gpointer self);

    void geometry(int w, int h);              // recalcule centre et rayons
    void pick(double x, double y, bool start); // applique un clic / glissement
    void triangle_vertices(double& hx, double& hy, double& wx, double& wy,
                           double& bx, double& by) const;
    void ensure_ring();
    void ensure_triangle();

    GtkWidget* area_ = nullptr;
    Hsv        hsv_{0, 0, 0.05};
    OnChange   on_change_;
    Drag       drag_ = Drag::None;
    double     press_x_ = 0, press_y_ = 0;

    // Géométrie courante
    int    w_ = 0, h_ = 0;
    double cx_ = 0, cy_ = 0, r_out_ = 0, r_in_ = 0;

    // Caches de rendu
    cairo_surface_t* ring_ = nullptr;
    int              ring_w_ = 0, ring_h_ = 0;
    cairo_surface_t* tri_  = nullptr;
    int              tri_w_ = 0, tri_h_ = 0;
    double           tri_hue_ = -1;
};

} // namespace creative::ui
