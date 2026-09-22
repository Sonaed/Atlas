#include "ui/gtk/color_wheel.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace creative::ui {

namespace {
constexpr double kPi   = 3.14159265358979323846;
constexpr double kSin60 = 0.86602540378443864676;

// Pixel ARGB32 prémultiplié (format natif de Cairo, little-endian).
inline std::uint32_t argb(double r, double g, double b, double a) {
    a = std::clamp(a, 0.0, 1.0);
    const auto c = [a](double v) { return std::uint32_t(std::clamp(v, 0.0, 1.0) * a * 255.0 + 0.5); };
    return (std::uint32_t(a * 255.0 + 0.5) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
}

// Couleur pleine d'une teinte (s = v = 1).
inline void hue_rgb(double h, double& r, double& g, double& b) {
    const GdkRGBA c = hsv_to_rgba({h, 1.0, 1.0});
    r = c.red; g = c.green; b = c.blue;
}

// Angle écran → teinte : rouge à droite, sens trigonométrique (y écran vers le bas).
inline double angle_to_hue(double dx, double dy) {
    double deg = std::atan2(-dy, dx) * 180.0 / kPi;
    if (deg < 0) deg += 360.0;
    return deg;
}
} // namespace

// ── Conversions ──────────────────────────────────────────────────────────────

Hsv rgb_to_hsv(double r, double g, double b) {
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    Hsv o;
    o.v = mx;
    o.s = mx > 0 ? d / mx : 0;
    if (d <= 1e-12) { o.h = 0; return o; }
    if      (mx == r) o.h = 60.0 * std::fmod((g - b) / d + 6.0, 6.0);
    else if (mx == g) o.h = 60.0 * ((b - r) / d + 2.0);
    else              o.h = 60.0 * ((r - g) / d + 4.0);
    return o;
}

GdkRGBA hsv_to_rgba(const Hsv& c) {
    const double h = std::fmod(std::fmod(c.h, 360.0) + 360.0, 360.0) / 60.0;
    const double s = std::clamp(c.s, 0.0, 1.0), v = std::clamp(c.v, 0.0, 1.0);
    const int    i = int(h) % 6;
    const double f = h - std::floor(h);
    const double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    double r = v, g = t, b = p;
    switch (i) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return GdkRGBA{float(r), float(g), float(b), 1.0f};
}

// ── Cycle de vie ─────────────────────────────────────────────────────────────

ColorWheel* ColorWheel::create() { return new ColorWheel(); }

ColorWheel::ColorWheel() {
    area_ = gtk_drawing_area_new();
    gtk_widget_set_size_request(area_, 176, 176);
    gtk_widget_set_hexpand(area_, TRUE);
    gtk_widget_add_css_class(area_, "color-wheel");
    // La notification de destruction de la fonction de dessin est appelée
    // quand le widget est finalisé : c'est elle qui libère la roue.
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(area_), draw_cb, this,
        [](gpointer p) { delete static_cast<ColorWheel*>(p); });

    auto* drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0); // tous boutons, stylet compris
    g_signal_connect(drag, "drag-begin",  G_CALLBACK(on_press),  this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(on_update), this);
    g_signal_connect(drag, "drag-end",    G_CALLBACK(on_end),    this);
    gtk_widget_add_controller(area_, GTK_EVENT_CONTROLLER(drag));
}

ColorWheel::~ColorWheel() {
    if (ring_) cairo_surface_destroy(ring_);
    if (tri_)  cairo_surface_destroy(tri_);
}

void ColorWheel::set_rgba(const GdkRGBA& c) {
    Hsv n = rgb_to_hsv(c.red, c.green, c.blue);
    // Couleur grise ou noire : la teinte (et pour le noir la saturation) est
    // indéterminée. On garde les précédentes pour que la roue ne saute pas.
    if (n.v <= 1e-6)      { n.h = hsv_.h; n.s = hsv_.s; }
    else if (n.s <= 1e-6) { n.h = hsv_.h; }
    hsv_ = n;
    gtk_widget_queue_draw(area_);
}

// ── Géométrie ────────────────────────────────────────────────────────────────

void ColorWheel::geometry(int w, int h) {
    w_ = w; h_ = h;
    cx_ = w * 0.5; cy_ = h * 0.5;
    r_out_ = std::max(10.0, std::min(w, h) * 0.5 - 3.0);
    r_in_  = r_out_ * 0.82;                     // épaisseur d'anneau : 18 %
}

// Sommets du triangle inscrit dans l'anneau : teinte en haut, blanc en bas à
// gauche, noir en bas à droite.
void ColorWheel::triangle_vertices(double& hx, double& hy, double& wx, double& wy,
                                   double& bx, double& by) const {
    const double R = r_in_ - 4.0;
    hx = cx_;               hy = cy_ - R;
    wx = cx_ - R * kSin60;  wy = cy_ + R * 0.5;
    bx = cx_ + R * kSin60;  by = cy_ + R * 0.5;
}

// ── Rendu ────────────────────────────────────────────────────────────────────

void ColorWheel::ensure_ring() {
    if (ring_ && ring_w_ == w_ && ring_h_ == h_) return;
    if (ring_) cairo_surface_destroy(ring_);
    ring_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w_, h_);
    ring_w_ = w_; ring_h_ = h_;
    cairo_surface_flush(ring_);
    auto* px = reinterpret_cast<std::uint32_t*>(cairo_image_surface_get_data(ring_));
    const int stride = cairo_image_surface_get_stride(ring_) / 4;
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x) {
            const double dx = x + 0.5 - cx_, dy = y + 0.5 - cy_;
            const double d  = std::hypot(dx, dy);
            // Couverture anticrénelée des deux bords de l'anneau.
            const double cov = std::clamp(r_out_ - d + 0.5, 0.0, 1.0)
                             * std::clamp(d - r_in_ + 0.5, 0.0, 1.0);
            if (cov <= 0) { px[y * stride + x] = 0; continue; }
            double r, g, b;
            hue_rgb(angle_to_hue(dx, dy), r, g, b);
            px[y * stride + x] = argb(r, g, b, cov);
        }
    cairo_surface_mark_dirty(ring_);
}

void ColorWheel::ensure_triangle() {
    if (tri_ && tri_w_ == w_ && tri_h_ == h_ && tri_hue_ == hsv_.h) return;
    if (!tri_ || tri_w_ != w_ || tri_h_ != h_) {
        if (tri_) cairo_surface_destroy(tri_);
        tri_ = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w_, h_);
        tri_w_ = w_; tri_h_ = h_;
    }
    tri_hue_ = hsv_.h;
    cairo_surface_flush(tri_);
    auto* px = reinterpret_cast<std::uint32_t*>(cairo_image_surface_get_data(tri_));
    const int stride = cairo_image_surface_get_stride(tri_) / 4;

    double hx, hy, wx, wy, bx, by;
    triangle_vertices(hx, hy, wx, wy, bx, by);
    double Hr, Hg, Hb;
    hue_rgb(hsv_.h, Hr, Hg, Hb);
    const double det  = (wy - by) * (hx - bx) + (bx - wx) * (hy - by);
    const double alt  = (r_in_ - 4.0) * 1.5;   // hauteur du triangle, en px

    const int y0 = std::max(0, int(hy) - 1), y1 = std::min(h_, int(by) + 2);
    for (int y = 0; y < h_; ++y) {
        auto* row = px + y * stride;
        if (y < y0 || y >= y1) { std::fill(row, row + w_, 0u); continue; }
        for (int x = 0; x < w_; ++x) {
            const double X = x + 0.5, Y = y + 0.5;
            // Coordonnées barycentriques : a = teinte, b = blanc, c = noir.
            const double a = ((wy - by) * (X - bx) + (bx - wx) * (Y - by)) / det;
            const double b = ((by - hy) * (X - bx) + (hx - bx) * (Y - by)) / det;
            const double c = 1.0 - a - b;
            const double m = std::min({a, b, c});
            const double cov = std::clamp(m * alt + 0.5, 0.0, 1.0);
            if (cov <= 0) { row[x] = 0; continue; }
            const double A = std::max(a, 0.0), B = std::max(b, 0.0), C = std::max(c, 0.0);
            const double S = A + B + C;
            const double ka = A / S, kb = B / S;
            // couleur = a·teinte + b·blanc + c·noir
            row[x] = argb(ka * Hr + kb, ka * Hg + kb, ka * Hb + kb, cov);
        }
    }
    cairo_surface_mark_dirty(tri_);
}

void ColorWheel::draw_cb(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer data) {
    auto* self = static_cast<ColorWheel*>(data);
    self->geometry(w, h);
    self->ensure_ring();
    self->ensure_triangle();

    cairo_set_source_surface(cr, self->ring_, 0, 0); cairo_paint(cr);
    cairo_set_source_surface(cr, self->tri_,  0, 0); cairo_paint(cr);

    // Fin cercle d'orbite autour de l'anneau, dans l'esprit carte stellaire.
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 0.34, 0.78, 0.86, 0.35);
    cairo_arc(cr, self->cx_, self->cy_, self->r_out_ + 1.5, 0, 2 * kPi);
    cairo_stroke(cr);

    // Repère de teinte sur l'anneau.
    const double ang = self->hsv_.h * kPi / 180.0;
    const double rm  = (self->r_in_ + self->r_out_) * 0.5;
    const double mx  = self->cx_ + std::cos(ang) * rm;
    const double my  = self->cy_ - std::sin(ang) * rm;
    const double mr  = (self->r_out_ - self->r_in_) * 0.42;
    cairo_set_line_width(cr, 2.0);
    cairo_set_source_rgb(cr, 0.02, 0.03, 0.06);
    cairo_arc(cr, mx, my, mr + 1, 0, 2 * kPi); cairo_stroke(cr);
    cairo_set_line_width(cr, 1.5);
    cairo_set_source_rgb(cr, 0.96, 0.97, 1.0);
    cairo_arc(cr, mx, my, mr - 0.5, 0, 2 * kPi); cairo_stroke(cr);

    // Repère saturation / valeur dans le triangle.
    double hx, hy, wx, wy, bx, by;
    self->triangle_vertices(hx, hy, wx, wy, bx, by);
    const double a = self->hsv_.s * self->hsv_.v;
    const double b = self->hsv_.v * (1 - self->hsv_.s);
    const double c = 1 - self->hsv_.v;
    const double px = a * hx + b * wx + c * bx, py = a * hy + b * wy + c * by;
    cairo_set_line_width(cr, 2.0);
    cairo_set_source_rgb(cr, 0.02, 0.03, 0.06);
    cairo_arc(cr, px, py, 6, 0, 2 * kPi); cairo_stroke(cr);
    cairo_set_line_width(cr, 1.5);
    cairo_set_source_rgb(cr, 0.96, 0.97, 1.0);
    cairo_arc(cr, px, py, 4.5, 0, 2 * kPi); cairo_stroke(cr);
}

// ── Interaction ──────────────────────────────────────────────────────────────

void ColorWheel::pick(double x, double y, bool start) {
    const double dx = x - cx_, dy = y - cy_;
    const double d  = std::hypot(dx, dy);

    if (start) {
        double hx, hy, wx, wy, bx, by;
        triangle_vertices(hx, hy, wx, wy, bx, by);
        const double det = (wy - by) * (hx - bx) + (bx - wx) * (hy - by);
        const double a = ((wy - by) * (x - bx) + (bx - wx) * (y - by)) / det;
        const double b = ((by - hy) * (x - bx) + (hx - bx) * (y - by)) / det;
        const double m = std::min({a, b, 1 - a - b});
        if (d >= r_in_ - 3 && d <= r_out_ + 6) drag_ = Drag::Ring;
        else if (m >= -0.08)                   drag_ = Drag::Triangle;
        else                                   drag_ = Drag::None;
    }

    if (drag_ == Drag::Ring) {
        hsv_.h = angle_to_hue(dx, dy);
    } else if (drag_ == Drag::Triangle) {
        double hx, hy, wx, wy, bx, by;
        triangle_vertices(hx, hy, wx, wy, bx, by);
        const double det = (wy - by) * (hx - bx) + (bx - wx) * (hy - by);
        double a = ((wy - by) * (x - bx) + (bx - wx) * (y - by)) / det;
        double b = ((by - hy) * (x - bx) + (hx - bx) * (y - by)) / det;
        double c = 1 - a - b;
        // Hors du triangle : on ramène le point sur son bord, pour pouvoir
        // glisser au-delà sans perdre la sélection.
        a = std::max(a, 0.0); b = std::max(b, 0.0); c = std::max(c, 0.0);
        const double S = a + b + c;
        a /= S; b /= S;
        hsv_.v = std::clamp(a + b, 0.0, 1.0);
        if (hsv_.v > 1e-6) hsv_.s = std::clamp(a / hsv_.v, 0.0, 1.0);
    } else {
        return;
    }
    gtk_widget_queue_draw(area_);
    if (on_change_) on_change_(rgba());
}

void ColorWheel::on_press(GtkGestureDrag*, double x, double y, gpointer data) {
    auto* self = static_cast<ColorWheel*>(data);
    self->press_x_ = x; self->press_y_ = y;
    self->pick(x, y, true);
}
void ColorWheel::on_update(GtkGestureDrag*, double dx, double dy, gpointer data) {
    auto* self = static_cast<ColorWheel*>(data);
    self->pick(self->press_x_ + dx, self->press_y_ + dy, false);
}
void ColorWheel::on_end(GtkGestureDrag*, double, double, gpointer data) {
    static_cast<ColorWheel*>(data)->drag_ = Drag::None;
}

} // namespace creative::ui
