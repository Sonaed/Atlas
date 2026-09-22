// Atlas v4 — Creative Studio Linux/Wayland
// "PaintStorm × Photoshop × CreativeSysteme 2.0"
// Fixes: CSS propre, GPU différé, cache composite, outils complets, Catmull-Rom
#include <gtk/gtk.h>
#include "ui/gtk/native_canvas_surface.h"
#include "engine/brush/brush_engine.h"
#include "engine/serialization/atlas_project.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

// ── Compat: G_ACTION_CALLBACK absent sur certaines versions de GLib ──────────
#ifndef G_ACTION_CALLBACK
#define G_ACTION_CALLBACK(f) \
    ((void(*)(GSimpleAction*,GVariant*,gpointer))(f))
#endif

// ── Icônes Phosphore ─────────────────────────────────────────────────────────
static const char ICON_PATH[] =
    "/home/deanos/Documents/CreativeSysteme v1.0/UI/icons/";

static GtkWidget* icon_btn(const char* icon, const char* tip) {
    std::string p = std::string(ICON_PATH) + icon;
    GtkWidget* img = gtk_image_new_from_file(p.c_str());
    gtk_image_set_pixel_size(GTK_IMAGE(img), 20);
    GtkWidget* b = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(b), img);
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_add_css_class(b, "tool-btn");
    return b;
}

// ── Blend modes ──────────────────────────────────────────────────────────────
enum class BlendMode : int {
    Normal=0,Darken,Multiply,ColorBurn,Lighten,Screen,ColorDodge,Overlay,
    SoftLight,HardLight,Difference,Exclusion,Hue,Saturation,Color,Luminosity
};
static const char* BLEND_NAMES[] = {
    "Normal","Darken","Multiply","Colour Burn","Lighten","Screen","Colour Dodge",
    "Overlay","Soft Light","Hard Light","Difference","Exclusion",
    "Hue","Saturation","Color","Luminosity",nullptr
};
static const char* BLEND_KEYS[] = {
    "normal","darken","multiply","color_burn","lighten","screen","color_dodge","overlay",
    "soft_light","hard_light","difference","exclusion","hue","saturation","color","luminosity"
};
static constexpr int BLEND_COUNT = 16;

static BlendMode blend_from_string(const std::string& s) {
    for(int i=0;i<BLEND_COUNT;++i)
        if(s==BLEND_KEYS[i]) return static_cast<BlendMode>(i);
    return BlendMode::Normal;
}

// HSL helpers
static void rgb2hsl(float r,float g,float b,float&h,float&s,float&l){
    float mx=std::max({r,g,b}),mn=std::min({r,g,b});
    l=(mx+mn)*.5f;
    if(mx==mn){h=s=0;return;}
    float d=mx-mn;
    s=d/(1.f-std::abs(2.f*l-1.f));
    if(mx==r)      h=std::fmod((g-b)/d+6.f,6.f)/6.f;
    else if(mx==g) h=((b-r)/d+2.f)/6.f;
    else           h=((r-g)/d+4.f)/6.f;
}
static float hue2rgb(float p,float q,float t){
    if(t<0)t+=1;if(t>1)t-=1;
    if(t<1.f/6)return p+(q-p)*6*t;
    if(t<.5f)return q;
    if(t<2.f/3)return p+(q-p)*(2.f/3-t)*6;
    return p;
}
static void hsl2rgb(float h,float s,float l,float&r,float&g,float&b){
    if(s==0){r=g=b=l;return;}
    float q=l<.5f?l*(1+s):l+s-l*s,p=2*l-q;
    r=hue2rgb(p,q,h+1.f/3);g=hue2rgb(p,q,h);b=hue2rgb(p,q,h-1.f/3);
}

static void blend_px(float sr,float sg,float sb,float sa,
                     float&dr,float&dg,float&db,float&da,BlendMode m){
    if(sa<=0)return;
    float cr=sr,cg=sg,cb=sb;
    switch(m){
    case BlendMode::Darken:  cr=std::min(sr,dr);cg=std::min(sg,dg);cb=std::min(sb,db);break;
    case BlendMode::Multiply:cr=sr*dr;cg=sg*dg;cb=sb*db;break;
    case BlendMode::ColorBurn:
        cr=dr<=0?0:1-std::min(1.f,(1-dr)/std::max(.001f,sr));
        cg=dg<=0?0:1-std::min(1.f,(1-dg)/std::max(.001f,sg));
        cb=db<=0?0:1-std::min(1.f,(1-db)/std::max(.001f,sb));break;
    case BlendMode::Lighten: cr=std::max(sr,dr);cg=std::max(sg,dg);cb=std::max(sb,db);break;
    case BlendMode::Screen:  cr=1-(1-sr)*(1-dr);cg=1-(1-sg)*(1-dg);cb=1-(1-sb)*(1-db);break;
    case BlendMode::ColorDodge:
        cr=dr>=1?1:std::min(1.f,dr/std::max(.001f,1-sr));
        cg=dg>=1?1:std::min(1.f,dg/std::max(.001f,1-sg));
        cb=db>=1?1:std::min(1.f,db/std::max(.001f,1-sb));break;
    case BlendMode::Overlay:
        cr=dr<.5f?2*dr*sr:1-2*(1-dr)*(1-sr);
        cg=dg<.5f?2*dg*sg:1-2*(1-dg)*(1-sg);
        cb=db<.5f?2*db*sb:1-2*(1-db)*(1-sb);break;
    case BlendMode::SoftLight:
        cr=sr<=.5f?dr-(1-2*sr)*dr*(1-dr):dr+(2*sr-1)*(std::sqrt(std::max(0.f,dr))-dr);
        cg=sg<=.5f?dg-(1-2*sg)*dg*(1-dg):dg+(2*sg-1)*(std::sqrt(std::max(0.f,dg))-dg);
        cb=sb<=.5f?db-(1-2*sb)*db*(1-db):db+(2*sb-1)*(std::sqrt(std::max(0.f,db))-db);break;
    case BlendMode::HardLight:
        cr=sr<.5f?2*sr*dr:1-2*(1-sr)*(1-dr);
        cg=sg<.5f?2*sg*dg:1-2*(1-sg)*(1-dg);
        cb=sb<.5f?2*sb*db:1-2*(1-sb)*(1-db);break;
    case BlendMode::Difference:cr=std::abs(dr-sr);cg=std::abs(dg-sg);cb=std::abs(db-sb);break;
    case BlendMode::Exclusion: cr=dr+sr-2*dr*sr;cg=dg+sg-2*dg*sg;cb=db+sb-2*db*sb;break;
    case BlendMode::Hue:{float hd,sd,ld,hs,ss,ls;rgb2hsl(dr,dg,db,hd,sd,ld);rgb2hsl(sr,sg,sb,hs,ss,ls);hsl2rgb(hs,sd,ld,cr,cg,cb);}break;
    case BlendMode::Saturation:{float hd,sd,ld,hs,ss,ls;rgb2hsl(dr,dg,db,hd,sd,ld);rgb2hsl(sr,sg,sb,hs,ss,ls);hsl2rgb(hd,ss,ld,cr,cg,cb);}break;
    case BlendMode::Color:{float hd,sd,ld,hs,ss,ls;rgb2hsl(dr,dg,db,hd,sd,ld);rgb2hsl(sr,sg,sb,hs,ss,ls);hsl2rgb(hs,ss,ld,cr,cg,cb);}break;
    case BlendMode::Luminosity:{float hd,sd,ld,hs,ss,ls;rgb2hsl(dr,dg,db,hd,sd,ld);rgb2hsl(sr,sg,sb,hs,ss,ls);hsl2rgb(hd,sd,ls,cr,cg,cb);}break;
    default:break;
    }
    float inv=1.f-sa, out_a=sa+da*inv;
    if(out_a<=0){dr=dg=db=da=0;return;}
    float wb=sa*da, ws=sa*(1-da), wd=da*inv;
    dr=(cr*wb+sr*ws+dr*wd)/out_a;
    dg=(cg*wb+sg*ws+dg*wd)/out_a;
    db=(cb*wb+sb*ws+db*wd)/out_a;
    da=out_a;
}

// ── Document ──────────────────────────────────────────────────────────────────
static std::size_t   doc_w   = 1024;
static std::size_t   doc_h   = 1024;
static std::uint32_t doc_dpi = 300;
static std::string   doc_bg  = "transparent";
static constexpr std::size_t TILE_SZ  = 64;
static constexpr double      GUTTER   = 28.0;

enum class Tool { Brush,Eraser,Fill,Eyedropper,Move,Select,Gradient,Text,Smudge,Blur };
static Tool active_tool = Tool::Brush;

// ── Brush state ───────────────────────────────────────────────────────────────
static GdkRGBA brush_col = {0.05f, 0.05f, 0.05f, 1.0f};
static int     brush_r   = 10;
static double  brush_op  = 1.0, brush_hard = 0.70, brush_space = 0.05;

struct BrushPreset { std::string name; int r; double op,hard,space; float cr,cg,cb; };
static std::vector<BrushPreset> presets;
static int active_preset = -1;

static std::string presets_path() {
    const char* h = std::getenv("HOME");
    return std::string(h ? h : ".") + "/.config/atlas/presets.atbr";
}
static void save_presets() {
    std::string p = presets_path();
    auto s = p.rfind('/');
    if(s != std::string::npos) ::system(("mkdir -p '" + p.substr(0,s) + "'").c_str());
    std::ofstream f(p);
    for(auto& pr : presets)
        f << pr.name << '\t' << pr.r << '\t' << pr.op << '\t' << pr.hard
          << '\t' << pr.space << '\t' << pr.cr << '\t' << pr.cg << '\t' << pr.cb << '\n';
}
static void load_presets() {
    std::ifstream f(presets_path()); std::string ln; presets.clear();
    while(std::getline(f, ln)) {
        std::istringstream ss(ln); BrushPreset p;
        if(ss >> p.name >> p.r >> p.op >> p.hard >> p.space >> p.cr >> p.cg >> p.cb)
            presets.push_back(p);
    }
}

// ── Layers ────────────────────────────────────────────────────────────────────
static std::vector<std::unique_ptr<creative::engine::TileStore>> layers;
static std::vector<std::string> lnames;
static std::vector<bool>        lvis;
static std::vector<float>       lop;
static std::vector<BlendMode>   lmode;
static int active_layer = 0;

static creative::engine::TileStore* active_store() {
    return layers.empty() ? nullptr : layers[std::size_t(active_layer)].get();
}

// ── Composite (Cairo ARGB32 premultiplied) ────────────────────────────────────
// cbuf = composite résultat, utilisé par draw_cb
// bg_cache = composite de tous les calques SAUF le calque actif (cache)
// bg_cache_valid = vrai si bg_cache est à jour
// active_rev = revision du calque actif au moment du dernier composite
static std::vector<unsigned char> cbuf;
static std::vector<unsigned char> bg_cache;
static bool   bg_cache_valid = false;
static int    bg_cache_active_idx = -1;   // quel calque était actif lors du dernier bg_cache
static std::size_t active_rev_cached = (std::size_t)-1;
static int cstride = 0;

static void init_cbuf() {
    cstride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, int(doc_w));
    cbuf.assign(std::size_t(cstride) * doc_h, 0u);
    bg_cache.assign(std::size_t(cstride) * doc_h, 0u);
    bg_cache_valid = false;
    active_rev_cached = (std::size_t)-1;
}

// Compose une couche source (RGBA linéaire) sur dst (ARGB32 premul Cairo)
static void composite_layer_onto(
    std::vector<unsigned char>& dst,
    const std::vector<std::byte>& src,
    float lopacity, BlendMode lm)
{
    for(std::size_t py = 0; py < doc_h; ++py) {
        auto drow = py * std::size_t(cstride);
        auto srow = py * doc_w * 4;
        for(std::size_t px = 0; px < doc_w; ++px) {
            auto si = srow + px*4;
            unsigned sa8 = std::to_integer<unsigned>(src[si+3]);
            if(!sa8) continue;
            float sr = std::to_integer<unsigned>(src[si])   / 255.f;
            float sg = std::to_integer<unsigned>(src[si+1]) / 255.f;
            float sb = std::to_integer<unsigned>(src[si+2]) / 255.f;
            float sa = sa8 / 255.f * lopacity;
            auto di = drow + px*4;
            float da = dst[di+3] / 255.f;
            float dr = da > 0 ? dst[di+2] / 255.f / da : 0.f;
            float dg = da > 0 ? dst[di+1] / 255.f / da : 0.f;
            float db = da > 0 ? dst[di]   / 255.f / da : 0.f;
            blend_px(sr, sg, sb, sa, dr, dg, db, da, lm);
            dst[di]   = (unsigned char)std::clamp(db*da*255.f, 0.f, 255.f);
            dst[di+1] = (unsigned char)std::clamp(dg*da*255.f, 0.f, 255.f);
            dst[di+2] = (unsigned char)std::clamp(dr*da*255.f, 0.f, 255.f);
            dst[di+3] = (unsigned char)std::clamp(da*255.f,    0.f, 255.f);
        }
    }
}

// Invalide le cache bg (appeler quand la structure des calques change)
static void invalidate_bg_cache() {
    bg_cache_valid = false;
    active_rev_cached = (std::uint64_t)-1;
}

static void rebuild_bg_cache() {
    if(layers.empty()) return;
    std::fill(bg_cache.begin(), bg_cache.end(), 0u);
    int al = active_layer;
    // Render bottom→top, skip active layer
    for(int li = int(layers.size())-1; li >= 0; --li) {
        if(li == al) continue;
        auto idx = std::size_t(li);
        if(!lvis[idx]) continue;
        auto src = layers[idx]->snapshot_rgba();
        composite_layer_onto(bg_cache, src, lop[idx], lmode[idx]);
    }
    bg_cache_valid = true;
    bg_cache_active_idx = al;
}

static void refresh_composite() {
    if(layers.empty()) return;
    if(cbuf.empty()) init_cbuf();

    // Check if we need to rebuild bg_cache
    if(!bg_cache_valid || bg_cache_active_idx != active_layer) {
        rebuild_bg_cache();
    }

    // Check if active layer changed since last composite
    std::size_t cur_rev = active_store() ? active_store()->revision() : 0;
    bool active_changed = (cur_rev != active_rev_cached);

    if(!active_changed && bg_cache_valid && bg_cache_active_idx == active_layer) {
        // Nothing changed, skip recomposite
        return;
    }

    // Copy bg_cache into cbuf
    std::copy(bg_cache.begin(), bg_cache.end(), cbuf.begin());

    // Composite active layer on top
    if(active_store() && lvis[std::size_t(active_layer)]) {
        auto src = active_store()->snapshot_rgba();
        composite_layer_onto(cbuf, src, lop[std::size_t(active_layer)], lmode[std::size_t(active_layer)]);
    }

    active_rev_cached = cur_rev;
}

// ── Canvas helpers ─────────────────────────────────────────────────────────────
static double cscale(int ww, int wh) {
    return std::max(.001, std::min((ww - 2.*GUTTER)/doc_w, (wh - 2.*GUTTER)/doc_h));
}
static void corigin(int ww, int wh, double& ox, double& oy) {
    double s = cscale(ww, wh);
    ox = (ww - doc_w*s)*.5;
    oy = (wh - doc_h*s)*.5;
}
static bool w2c(GtkWidget* w, double wx, double wy, std::size_t& cx, std::size_t& cy) {
    int ww = gtk_widget_get_width(w), wh = gtk_widget_get_height(w);
    double s = cscale(ww,wh), ox, oy; corigin(ww,wh,ox,oy);
    double rx = (wx-ox)/s, ry = (wy-oy)/s;
    if(rx < 0 || ry < 0 || rx >= doc_w || ry >= doc_h) return false;
    cx = (std::size_t)rx; cy = (std::size_t)ry; return true;
}

// ── UI globals ─────────────────────────────────────────────────────────────────
static GtkWidget* canvas_w   = nullptr;
static GtkWidget* layer_lb   = nullptr;
static GtkWidget* preset_lb  = nullptr;
static GtkLabel*  status_lbl = nullptr;
static GtkLabel*  coord_lbl  = nullptr;
static GtkWindow* welcome_win = nullptr;
static GtkWindow* editor_win  = nullptr;
static std::string doc_path;

// Cursor
static double cur_wx = -9999, cur_wy = -9999;
static bool   cur_in = false;

// Selection state
static bool   sel_active = false;
static double sel_x0 = 0, sel_y0 = 0, sel_x1 = 0, sel_y1 = 0;

// Stroke state
static double sw0=0, sh0=0, slx=0, sly=0;
static bool   in_stroke = false;
static double sty_lx=0, sty_ly=0;
static bool   sty_active = false;

// Move tool state
static double move_dx = 0, move_dy = 0;  // accumulated pixel offset (canvas space)
static double move_wx0 = 0, move_wy0 = 0; // widget coords at drag start

// Catmull-Rom stroke buffer
struct StrokePt { double x, y, p; };
static std::deque<StrokePt> stroke_pts;

static creative::engine::TileStore dummy_store(1024, 1024, TILE_SZ);
static creative::engine::BrushEngine brush_eng(dummy_store);
static std::unique_ptr<creative::ui::NativeCanvasSurface> native_surf;

static bool gpu_off() {
    const char* e = std::getenv("CREATIVE_ENABLE_GPU");
    return !(e && std::string(e) == "1");
}

static void refresh_view() {
    static gint64 last = 0; gint64 now = g_get_monotonic_time();
    if(now - last > 14000) { refresh_composite(); last = now; }
    if(canvas_w) gtk_widget_queue_draw(canvas_w);
}
static void rebuild_eng() {
    if(active_store()) brush_eng.set_store(*active_store());
}

// ── Dab / Catmull-Rom interpolation ──────────────────────────────────────────
static void dab_at(std::size_t cx, std::size_t cy, double p=1.0, double tx=0, double ty=0) {
    if(!active_store()) return;
    p = std::clamp(p, 0.05, 1.0);
    double tm = std::hypot(tx, ty) / 90.0;
    auto r = (std::size_t)std::max(1.0, brush_r * p * (1 + tm*.3));
    if(active_tool == Tool::Eraser) {
        auto a = (unsigned)(brush_op * p * 255);
        brush_eng.dab(cx, cy, r, a << 24, brush_hard);
    } else {
        auto cr = (unsigned)(std::clamp(brush_col.red,  0.0f,1.0f)*255);
        auto cg = (unsigned)(std::clamp(brush_col.green,0.0f,1.0f)*255);
        auto cb = (unsigned)(std::clamp(brush_col.blue, 0.0f,1.0f)*255);
        auto a  = (unsigned)(brush_op * p * 255);
        brush_eng.dab(cx, cy, r, cr|(cg<<8)|(cb<<16)|(a<<24), brush_hard);
    }
}

// Catmull-Rom spline: evaluate position at t in [0,1] for 4 control points
static double catmull_rom(double p0, double p1, double p2, double p3, double t) {
    return 0.5 * ((2*p1)
        + (-p0 + p2)*t
        + (2*p0 - 5*p1 + 4*p2 - p3)*t*t
        + (-p0 + 3*p1 - 3*p2 + p3)*t*t*t);
}

// Emit dabs along the stroke using Catmull-Rom when we have ≥4 points,
// otherwise fall back to linear.
static void flush_stroke_segment(GtkWidget* w) {
    if(stroke_pts.size() < 2) return;
    int ww = gtk_widget_get_width(w), wh = gtk_widget_get_height(w);
    double sc = cscale(ww, wh);
    double sp = std::max(0.5, brush_r * brush_space * sc);

    std::size_t n = stroke_pts.size();
    // We emit from point [n-2] to [n-1] using Catmull-Rom (needs [n-3]..[n])
    // But we always emit the last segment
    std::size_t i1 = n >= 2 ? n-2 : 0;
    std::size_t i2 = n-1;
    std::size_t i0 = n >= 3 ? n-3 : i1;
    std::size_t i3 = i2; // clamp: no future point

    const auto& p0 = stroke_pts[i0];
    const auto& p1 = stroke_pts[i1];
    const auto& p2 = stroke_pts[i2];
    const auto& p3 = stroke_pts[i3];

    double dx = p2.x - p1.x, dy = p2.y - p1.y;
    double dist = std::hypot(dx, dy);
    int steps = std::max(1, (int)std::ceil(dist / sp));
    for(int s = 1; s <= steps; ++s) {
        double t = double(s) / steps;
        double wx, wy;
        if(n >= 4) {
            wx = catmull_rom(p0.x, p1.x, p2.x, p3.x, t);
            wy = catmull_rom(p0.y, p1.y, p2.y, p3.y, t);
        } else {
            wx = p1.x + dx*t;
            wy = p1.y + dy*t;
        }
        double pp = p1.p + (p2.p - p1.p) * t;
        std::size_t cx, cy;
        if(w2c(w, wx, wy, cx, cy)) dab_at(cx, cy, pp);
    }
}

static void interp_dab(GtkWidget* w, double x0,double y0, double x1,double y1,
                       double p=1, double tx=0, double ty=0) {
    int ww = gtk_widget_get_width(w), wh = gtk_widget_get_height(w);
    double s  = cscale(ww, wh);
    double sp = std::max(0.5, brush_r * brush_space * s);
    double dx = x1-x0, dy = y1-y0, dist = std::hypot(dx, dy);
    int n = std::max(1, (int)std::ceil(dist / sp));
    for(int i = 1; i <= n; ++i) {
        double t = double(i)/n;
        std::size_t cx, cy;
        if(w2c(w, x0+dx*t, y0+dy*t, cx, cy)) dab_at(cx, cy, p, tx, ty);
    }
}

// ── Fill tool (BFS flood fill) ────────────────────────────────────────────────
static void do_fill(std::size_t fx, std::size_t fy) {
    auto* store = active_store();
    if(!store) return;

    // Read target color at (fx,fy)
    unsigned tpx = store->read_pixel(fx, fy);
    unsigned tar = (tpx >>  0) & 0xFF;
    unsigned tag = (tpx >>  8) & 0xFF;
    unsigned tab = (tpx >> 16) & 0xFF;
    unsigned taa = (tpx >> 24) & 0xFF;

    unsigned fill_r = (unsigned)(std::clamp(brush_col.red,  0.f,1.f)*255);
    unsigned fill_g = (unsigned)(std::clamp(brush_col.green,0.f,1.f)*255);
    unsigned fill_b = (unsigned)(std::clamp(brush_col.blue, 0.f,1.f)*255);
    unsigned fill_a = (unsigned)(brush_op * 255);
    unsigned fill_px = fill_r|(fill_g<<8)|(fill_b<<16)|(fill_a<<24);

    if(tpx == fill_px) return;  // Already that color

    // BFS
    std::queue<std::pair<std::size_t,std::size_t>> q;
    q.push({fx, fy});
    std::vector<bool> visited(doc_w * doc_h, false);
    visited[fy * doc_w + fx] = true;

    constexpr int tolerance = 8; // per-channel tolerance

    auto color_match = [&](unsigned px) -> bool {
        int dr = (int)((px >>  0) & 0xFF) - (int)tar;
        int dg = (int)((px >>  8) & 0xFF) - (int)tag;
        int db = (int)((px >> 16) & 0xFF) - (int)tab;
        int da = (int)((px >> 24) & 0xFF) - (int)taa;
        return std::abs(dr)<=tolerance && std::abs(dg)<=tolerance
            && std::abs(db)<=tolerance && std::abs(da)<=tolerance;
    };

    while(!q.empty()) {
        auto [x, y] = q.front(); q.pop();
        store->write_pixel(x, y, fill_px);
        // 4-connected neighbors
        if(x>0 && !visited[(y)*doc_w+(x-1)]) {
            unsigned npx = store->read_pixel(x-1, y);
            if(color_match(npx)) { visited[y*doc_w+(x-1)]=true; q.push({x-1,y}); }
        }
        if(x+1<doc_w && !visited[y*doc_w+(x+1)]) {
            unsigned npx = store->read_pixel(x+1, y);
            if(color_match(npx)) { visited[y*doc_w+(x+1)]=true; q.push({x+1,y}); }
        }
        if(y>0 && !visited[(y-1)*doc_w+x]) {
            unsigned npx = store->read_pixel(x, y-1);
            if(color_match(npx)) { visited[(y-1)*doc_w+x]=true; q.push({x,y-1}); }
        }
        if(y+1<doc_h && !visited[(y+1)*doc_w+x]) {
            unsigned npx = store->read_pixel(x, y+1);
            if(color_match(npx)) { visited[(y+1)*doc_w+x]=true; q.push({x,y+1}); }
        }
    }
    invalidate_bg_cache();
    refresh_view();
}

// ── Eyedropper ────────────────────────────────────────────────────────────────
// Read from cbuf (Cairo ARGB32 premul): B=byte0, G=byte1, R=byte2, A=byte3
static void do_eyedropper(std::size_t cx, std::size_t cy) {
    if(cbuf.empty() || cx >= doc_w || cy >= doc_h) return;
    auto idx = cy * std::size_t(cstride) + cx*4;
    unsigned char ba = cbuf[idx+3];
    if(ba == 0) { brush_col = {0,0,0,1}; return; }
    // Unpremultiply
    float a = ba / 255.f;
    float r = (cbuf[idx+2] / 255.f) / a;
    float g = (cbuf[idx+1] / 255.f) / a;
    float b = (cbuf[idx+0] / 255.f) / a;
    brush_col = {
        (float)std::clamp(r,0.f,1.f),
        (float)std::clamp(g,0.f,1.f),
        (float)std::clamp(b,0.f,1.f),
        1.f
    };
}

// ── Move tool: translate pixels of active layer ───────────────────────────────
static void do_move_apply(int dx, int dy) {
    auto* store = active_store();
    if(!store || (dx==0 && dy==0)) return;
    auto src = store->snapshot_rgba();
    store->clear();
    // Write shifted pixels
    for(std::size_t py = 0; py < doc_h; ++py) {
        int sy = (int)py - dy;
        if(sy < 0 || sy >= (int)doc_h) continue;
        for(std::size_t px = 0; px < doc_w; ++px) {
            int sx = (int)px - dx;
            if(sx < 0 || sx >= (int)doc_w) continue;
            auto si = (std::size_t(sy) * doc_w + std::size_t(sx)) * 4;
            unsigned r = std::to_integer<unsigned>(src[si]);
            unsigned g = std::to_integer<unsigned>(src[si+1]);
            unsigned b = std::to_integer<unsigned>(src[si+2]);
            unsigned a = std::to_integer<unsigned>(src[si+3]);
            if(a == 0) continue;
            store->write_pixel(px, py, r|(g<<8)|(b<<16)|(a<<24));
        }
    }
    invalidate_bg_cache();
    refresh_view();
}

// ── Draw callback ─────────────────────────────────────────────────────────────
static void draw_cb(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    cairo_set_source_rgb(cr, .094, .094, .11); cairo_paint(cr);
    double s = cscale(w,h), ox, oy; corigin(w,h,ox,oy);
    // Checkerboard background for transparency
    double cell = std::max(1.0, 16.0 * s);
    int cols = (int)std::ceil(doc_w*s/cell), rows = (int)std::ceil(doc_h*s/cell);
    for(int r2 = 0; r2 < rows; ++r2)
        for(int c = 0; c < cols; ++c) {
            bool lt = (r2+c)%2 == 0;
            cairo_set_source_rgb(cr, lt?.76:.66, lt?.76:.66, lt?.76:.66);
            cairo_rectangle(cr, ox+c*cell, oy+r2*cell, cell, cell);
            cairo_fill(cr);
        }
    // Pixels
    if(!cbuf.empty()) {
        auto* surf = cairo_image_surface_create_for_data(
            cbuf.data(), CAIRO_FORMAT_ARGB32, int(doc_w), int(doc_h), cstride);
        cairo_save(cr); cairo_translate(cr, ox, oy); cairo_scale(cr, s, s);
        cairo_set_source_surface(cr, surf, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr),
            s >= 1.0 ? CAIRO_FILTER_NEAREST : CAIRO_FILTER_BILINEAR);
        cairo_paint(cr); cairo_restore(cr); cairo_surface_destroy(surf);
    }
    // Canvas border (bordeaux)
    cairo_set_source_rgba(cr, .609, .122, .208, .6); cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, ox-.5, oy-.5, doc_w*s+1, doc_h*s+1); cairo_stroke(cr);
    // Selection rect overlay
    if(sel_active) {
        double sx0=std::min(sel_x0,sel_x1), sy0=std::min(sel_y0,sel_y1);
        double sx1=std::max(sel_x0,sel_x1), sy1=std::max(sel_y0,sel_y1);
        cairo_set_source_rgba(cr, .15,.6,1,.3);
        cairo_rectangle(cr, sx0, sy0, sx1-sx0, sy1-sy0); cairo_fill(cr);
        static double dash[]={6,4};
        cairo_set_dash(cr, dash, 2, 0);
        cairo_set_source_rgba(cr, .3,.75,1,1); cairo_set_line_width(cr, 1.5);
        cairo_rectangle(cr, sx0, sy0, sx1-sx0, sy1-sy0); cairo_stroke(cr);
        cairo_set_dash(cr, nullptr, 0, 0);
    }
    // Brush cursor ring (for paint tools)
    if(cur_in && cur_wx > -999 &&
       (active_tool==Tool::Brush||active_tool==Tool::Eraser||
        active_tool==Tool::Smudge||active_tool==Tool::Blur)) {
        double rr = brush_r * s;
        cairo_set_line_width(cr, 1.5);
        cairo_set_source_rgba(cr, 0, 0, 0, .7);
        cairo_arc(cr, cur_wx, cur_wy, rr+1, 0, 2*G_PI); cairo_stroke(cr);
        if(active_tool == Tool::Eraser) cairo_set_source_rgba(cr, .61,.12,.21,.9);
        else cairo_set_source_rgba(cr, 1, 1, 1, .9);
        cairo_arc(cr, cur_wx, cur_wy, rr, 0, 2*G_PI); cairo_stroke(cr);
        cairo_set_line_width(cr, 1); cairo_set_source_rgba(cr, 1,1,1,.5);
        cairo_move_to(cr, cur_wx-5, cur_wy); cairo_line_to(cr, cur_wx+5, cur_wy);
        cairo_move_to(cr, cur_wx, cur_wy-5); cairo_line_to(cr, cur_wx, cur_wy+5);
        cairo_stroke(cr);
    } else if(cur_in && active_tool == Tool::Eyedropper) {
        // Crosshair cursor
        cairo_set_line_width(cr, 1.5);
        cairo_set_source_rgba(cr, 1,1,0,.9);
        cairo_move_to(cr, cur_wx-8, cur_wy); cairo_line_to(cr, cur_wx+8, cur_wy);
        cairo_move_to(cr, cur_wx, cur_wy-8); cairo_line_to(cr, cur_wx, cur_wy+8);
        cairo_stroke(cr);
    }
}

// ── Layer panel ────────────────────────────────────────────────────────────────
static void refresh_layer_panel();
static void refresh_preset_panel();

static void add_layer(GtkButton*, gpointer) {
    lnames.insert(lnames.begin(), "Calque " + std::to_string(lnames.size()+1));
    layers.insert(layers.begin(),
        std::make_unique<creative::engine::TileStore>(doc_w, doc_h, TILE_SZ));
    lvis.insert(lvis.begin(), true); lop.insert(lop.begin(), 1.f);
    lmode.insert(lmode.begin(), BlendMode::Normal);
    active_layer = 0; rebuild_eng(); invalidate_bg_cache(); refresh_view(); refresh_layer_panel();
}
static void del_layer(GtkButton*, gpointer) {
    if(lnames.size() <= 1) return;
    auto i = std::size_t(active_layer);
    lnames.erase(lnames.begin()+i); layers.erase(layers.begin()+i);
    lvis.erase(lvis.begin()+i); lop.erase(lop.begin()+i); lmode.erase(lmode.begin()+i);
    active_layer = std::min(active_layer, int(lnames.size())-1);
    rebuild_eng(); invalidate_bg_cache(); refresh_view(); refresh_layer_panel();
}

static void refresh_layer_panel() {
    if(!layer_lb) return;
    while(auto* c = gtk_widget_get_first_child(layer_lb))
        gtk_list_box_remove(GTK_LIST_BOX(layer_lb), c);
    for(int i = 0; i < int(lnames.size()); ++i) {
        auto idx = std::size_t(i);
        auto* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_hexpand(box, TRUE);
        auto* row1 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        // Eye toggle
        auto* eye = gtk_button_new_with_label(lvis[idx] ? "👁" : "◌");
        gtk_widget_set_size_request(eye, 28, 28);
        gtk_widget_add_css_class(eye, "eye-btn");
        g_object_set_data(G_OBJECT(eye), "li", GINT_TO_POINTER(i));
        g_signal_connect(eye, "clicked", (GCallback)(+[](GtkButton* b, gpointer) {
            int li = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "li"));
            if(li >= 0 && li < int(lvis.size())) lvis[std::size_t(li)] = !lvis[std::size_t(li)];
            invalidate_bg_cache(); refresh_view(); refresh_layer_panel();
        }), nullptr);
        gtk_box_append(GTK_BOX(row1), eye);
        // Name button
        auto nm = (i == active_layer ? "◉ " : "◌ ") + lnames[idx];
        auto* nb2 = gtk_button_new_with_label(nm.c_str());
        gtk_widget_set_hexpand(nb2, TRUE);
        gtk_widget_add_css_class(nb2, "layer-name-btn");
        if(i == active_layer) gtk_widget_add_css_class(nb2, "active");
        g_object_set_data(G_OBJECT(nb2), "li", GINT_TO_POINTER(i));
        g_signal_connect(nb2, "clicked", (GCallback)(+[](GtkButton* b, gpointer) {
            active_layer = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "li"));
            rebuild_eng(); invalidate_bg_cache(); refresh_layer_panel();
        }), nullptr);
        gtk_box_append(GTK_BOX(row1), nb2);
        gtk_box_append(GTK_BOX(box), row1);
        // Row2: opacity slider + blend mode
        auto* row2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_set_margin_start(row2, 32);
        auto* opsl = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
        gtk_range_set_value(GTK_RANGE(opsl), lop[idx] * 100.0);
        gtk_scale_set_draw_value(GTK_SCALE(opsl), FALSE);
        gtk_widget_set_hexpand(opsl, TRUE);
        gtk_widget_set_size_request(opsl, 60, 20);
        g_object_set_data(G_OBJECT(opsl), "li", GINT_TO_POINTER(i));
        g_signal_connect(opsl, "value-changed", (GCallback)(+[](GtkRange* r, gpointer) {
            int li = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "li"));
            if(li >= 0 && li < int(lop.size()))
                lop[std::size_t(li)] = float(gtk_range_get_value(r) / 100.0);
            invalidate_bg_cache(); refresh_view();
        }), nullptr);
        gtk_box_append(GTK_BOX(row2), opsl);
        auto* bdd = gtk_drop_down_new_from_strings(BLEND_NAMES);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(bdd), int(lmode[idx]));
        gtk_widget_add_css_class(bdd, "blend-dd");
        g_object_set_data(G_OBJECT(bdd), "li", GINT_TO_POINTER(i));
        g_signal_connect(bdd, "notify::selected", (GCallback)(+[](GObject* obj, GParamSpec*, gpointer) {
            int li = GPOINTER_TO_INT(g_object_get_data(obj, "li"));
            if(li >= 0 && li < int(lmode.size()))
                lmode[std::size_t(li)] = static_cast<BlendMode>(
                    gtk_drop_down_get_selected(GTK_DROP_DOWN(obj)));
            invalidate_bg_cache(); refresh_view();
        }), nullptr);
        gtk_box_append(GTK_BOX(row2), bdd);
        gtk_box_append(GTK_BOX(box), row2);
        gtk_list_box_append(GTK_LIST_BOX(layer_lb), box);
    }
}

// ── Presets ────────────────────────────────────────────────────────────────────
static void apply_preset(int pi) {
    if(pi < 0 || pi >= int(presets.size())) return;
    auto& p = presets[std::size_t(pi)];
    brush_r = p.r; brush_op = p.op; brush_hard = p.hard; brush_space = p.space;
    brush_col = {p.cr, p.cg, p.cb, 1.0}; active_preset = pi;
    refresh_preset_panel();
    if(canvas_w) gtk_widget_queue_draw(canvas_w);
}
static void refresh_preset_panel() {
    if(!preset_lb) return;
    while(auto* c = gtk_widget_get_first_child(preset_lb))
        gtk_list_box_remove(GTK_LIST_BOX(preset_lb), c);
    for(int i = 0; i < int(presets.size()); ++i) {
        auto* btn = gtk_button_new_with_label(presets[std::size_t(i)].name.c_str());
        gtk_widget_add_css_class(btn, "preset-btn");
        if(i == active_preset) gtk_widget_add_css_class(btn, "active");
        g_object_set_data(G_OBJECT(btn), "pi", GINT_TO_POINTER(i));
        g_signal_connect(btn, "clicked", (GCallback)(+[](GtkButton* b, gpointer) {
            apply_preset(GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "pi")));
        }), nullptr);
        gtk_list_box_append(GTK_LIST_BOX(preset_lb), btn);
    }
}

// ── File actions ───────────────────────────────────────────────────────────────
static void do_save(const std::string& path) {
    creative::engine::AtlasProject proj;
    proj.name = "Untitled"; proj.version = "0.1";
    proj.width = doc_w; proj.height = doc_h; proj.dpi = doc_dpi; proj.background = doc_bg;
    for(std::size_t i = 0; i < layers.size(); ++i) {
        creative::engine::AtlasLayerRecord r;
        r.name = lnames[i]; r.pixels = layers[i]->snapshot_rgba();
        r.mask.assign(doc_w * doc_h * 4, std::byte{0xff});
        r.opacity = lop[i]; r.visible = lvis[i];
        r.blend = BLEND_KEYS[int(lmode[i])];
        proj.layers.push_back(std::move(r));
    }
    if(creative::engine::AtlasSerializer::save(path, proj))
        if(status_lbl) gtk_label_set_text(status_lbl, ("Sauvegardé → " + path).c_str());
}

static void on_save_done(GObject* src, GAsyncResult* res, gpointer win) {
    GError* e = nullptr;
    auto* f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &e);
    if(f) { doc_path = g_file_get_path(f); do_save(doc_path); g_object_unref(f); }
    if(e) g_error_free(e);
    (void)win;
}
static void action_save(GSimpleAction*, GVariant*, gpointer win) {
    if(!doc_path.empty()) { do_save(doc_path); return; }
    auto* d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Sauvegarder");
    gtk_file_dialog_set_initial_name(d, "sans-titre.atlas");
    gtk_file_dialog_save(d, GTK_WINDOW(win), nullptr, on_save_done, win);
    g_object_unref(d);
}
static void action_save_as(GSimpleAction*, GVariant*, gpointer win) {
    doc_path.clear(); action_save(nullptr, nullptr, win);
}
static void on_export_png_done(GObject* src, GAsyncResult* res, gpointer) {
    GError* e = nullptr;
    auto* f = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &e);
    if(f) {
        creative::engine::AtlasProject p;
        p.width = doc_w; p.height = doc_h; p.dpi = doc_dpi;
        for(std::size_t i = 0; i < layers.size(); ++i) {
            creative::engine::AtlasLayerRecord r;
            r.name = lnames[i]; r.visible = lvis[i];
            r.pixels = layers[i]->snapshot_rgba(); p.layers.push_back(std::move(r));
        }
        creative::engine::AtlasSerializer::export_png(g_file_get_path(f), p);
        g_object_unref(f);
    }
    if(e) g_error_free(e);
}
static void action_export_png(GSimpleAction*, GVariant*, gpointer win) {
    auto* d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Exporter PNG");
    gtk_file_dialog_set_initial_name(d, "export.png");
    gtk_file_dialog_save(d, GTK_WINDOW(win), nullptr, on_export_png_done, nullptr);
    g_object_unref(d);
}
static void action_undo(GSimpleAction*, GVariant*, gpointer) {
    if(brush_eng.undo()) { invalidate_bg_cache(); refresh_view(); }
}
static void action_redo(GSimpleAction*, GVariant*, gpointer) {
    if(brush_eng.redo()) { invalidate_bg_cache(); refresh_view(); }
}

// ── Open ───────────────────────────────────────────────────────────────────────
static void reload_doc(const creative::engine::AtlasProject& proj, const std::string& path) {
    doc_path = path; doc_w = proj.width; doc_h = proj.height;
    doc_dpi = proj.dpi; doc_bg = proj.background;
    layers.clear(); lnames.clear(); lvis.clear(); lop.clear(); lmode.clear();
    for(auto& l : proj.layers) {
        lnames.push_back(l.name); lvis.push_back(l.visible); lop.push_back(l.opacity);
        lmode.push_back(blend_from_string(l.blend));
        layers.push_back(std::make_unique<creative::engine::TileStore>(doc_w, doc_h, TILE_SZ));
        if(l.pixels.size() == doc_w*doc_h*4) layers.back()->restore_rgba(l.pixels);
    }
    active_layer = 0; init_cbuf(); rebuild_eng();
    invalidate_bg_cache(); refresh_view(); refresh_layer_panel();
}
static void on_open_done(GObject* src, GAsyncResult* res, gpointer) {
    GError* e = nullptr;
    auto* f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, &e);
    if(f) {
        creative::engine::AtlasProject p;
        if(creative::engine::AtlasSerializer::load(g_file_get_path(f), p))
            reload_doc(p, g_file_get_path(f));
        g_object_unref(f);
    }
    if(e) g_error_free(e);
}
static void action_open(GSimpleAction*, GVariant*, gpointer win) {
    auto* d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Ouvrir");
    auto* flt = g_list_store_new(GTK_TYPE_FILE_FILTER);
    auto* ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, "Fichiers Atlas (*.atlas)");
    gtk_file_filter_add_pattern(ff, "*.atlas");
    g_list_store_append(flt, ff); g_object_unref(ff);
    gtk_file_dialog_set_filters(d, G_LIST_MODEL(flt)); g_object_unref(flt);
    gtk_file_dialog_open(d, GTK_WINDOW(win), nullptr, on_open_done, nullptr);
    g_object_unref(d);
}

// ── Nouveau document dialog ───────────────────────────────────────────────────
struct NewDocCtx {
    GtkWindow* dlg;
    GtkWidget* ws; GtkWidget* hs; GtkWidget* dpis; GtkWidget* bgdd;
};

static void on_new_doc_ok(GtkButton*, gpointer ud) {
    auto* ctx = static_cast<NewDocCtx*>(ud);
    doc_w = std::max(1, (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(ctx->ws)));
    doc_h = std::max(1, (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(ctx->hs)));
    doc_dpi = std::max(1u, (uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(ctx->dpis)));
    switch(gtk_drop_down_get_selected(GTK_DROP_DOWN(ctx->bgdd))) {
        case 1: doc_bg = "white"; break;
        case 2: doc_bg = "black"; break;
        default: doc_bg = "transparent";
    }
    doc_path.clear();
    layers.clear(); lnames.clear(); lvis.clear(); lop.clear(); lmode.clear();
    init_cbuf();
    lnames.push_back("Fond");
    layers.push_back(std::make_unique<creative::engine::TileStore>(doc_w, doc_h, TILE_SZ));
    lvis.push_back(true); lop.push_back(1.f); lmode.push_back(BlendMode::Normal);
    if(doc_bg == "white")      layers[0]->touch_region(0,0,doc_w,doc_h,0xffffffff);
    else if(doc_bg == "black") layers[0]->touch_region(0,0,doc_w,doc_h,0xff000000);
    active_layer = 0; rebuild_eng(); invalidate_bg_cache(); refresh_view(); refresh_layer_panel();
    gtk_window_destroy(ctx->dlg);
    delete ctx;
}

static void show_new_doc_dialog(GtkWindow* parent) {
    auto* dlg = gtk_window_new();
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), parent);
    gtk_window_set_title(GTK_WINDOW(dlg), "Nouveau document");
    gtk_window_set_default_size(GTK_WINDOW(dlg), 370, 0);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);

    auto* vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(vb,16); gtk_widget_set_margin_bottom(vb,16);
    gtk_widget_set_margin_start(vb,20); gtk_widget_set_margin_end(vb,20);

    auto* grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);

    auto glbl = [&](const char* t, int r) {
        auto* l = gtk_label_new(t); gtk_label_set_xalign(GTK_LABEL(l), 0);
        gtk_grid_attach(GTK_GRID(grid), l, 0, r, 1, 1);
    };

    int row = 0;
    glbl("Format :", row);
    static const char* fmts[] = {"Personnalisé","800×600","1920×1080","2560×1440","3840×2160",nullptr};
    auto* fdd = gtk_drop_down_new_from_strings(fmts);
    gtk_grid_attach(GTK_GRID(grid), fdd, 1, row++, 2, 1);

    glbl("Largeur :", row);
    auto* ws = gtk_spin_button_new_with_range(1, 20000, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ws), 1024);
    gtk_grid_attach(GTK_GRID(grid), ws, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("px"), 2, row++, 1, 1);

    glbl("Hauteur :", row);
    auto* hs = gtk_spin_button_new_with_range(1, 20000, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(hs), 1024);
    gtk_grid_attach(GTK_GRID(grid), hs, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("px"), 2, row++, 1, 1);

    glbl("Résolution :", row);
    auto* dpis = gtk_spin_button_new_with_range(1, 2400, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(dpis), 300);
    gtk_grid_attach(GTK_GRID(grid), dpis, 1, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new("DPI"), 2, row++, 1, 1);

    glbl("Fond :", row);
    static const char* bgs[] = {"Transparent","Blanc","Noir",nullptr};
    auto* bgdd = gtk_drop_down_new_from_strings(bgs);
    gtk_grid_attach(GTK_GRID(grid), bgdd, 1, row++, 2, 1);
    gtk_box_append(GTK_BOX(vb), grid);

    struct PH { GtkWidget* ws; GtkWidget* hs; };
    auto* ph = new PH{ws, hs};
    g_signal_connect(fdd, "notify::selected", (GCallback)(+[](GObject* obj, GParamSpec*, gpointer u){
        auto* ph2 = static_cast<PH*>(u);
        guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
        static const int p[][2] = {{0,0},{800,600},{1920,1080},{2560,1440},{3840,2160}};
        if(sel > 0 && sel < 5) {
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(ph2->ws), p[sel][0]);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(ph2->hs), p[sel][1]);
        }
    }), ph);

    auto* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(vb), sep);
    auto* brow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    auto* ok = gtk_button_new_with_label("Créer");
    auto* ca = gtk_button_new_with_label("Annuler");
    gtk_widget_add_css_class(ok, "primary");
    gtk_widget_set_hexpand(ok, TRUE);

    auto* ctx = new NewDocCtx{GTK_WINDOW(dlg), ws, hs, dpis, bgdd};
    g_signal_connect(ok, "clicked", G_CALLBACK(on_new_doc_ok), ctx);
    g_signal_connect(ca, "clicked", (GCallback)(+[](GtkButton*, gpointer dlg2){
        gtk_window_destroy(GTK_WINDOW(dlg2));
    }), dlg);

    gtk_box_append(GTK_BOX(brow), ok);
    gtk_box_append(GTK_BOX(brow), ca);
    gtk_box_append(GTK_BOX(vb), brow);
    gtk_window_set_child(GTK_WINDOW(dlg), vb);
    gtk_window_present(GTK_WINDOW(dlg));
}

static void action_new(GSimpleAction*, GVariant*, gpointer win) {
    show_new_doc_dialog(GTK_WINDOW(win));
}

// ── Gesture callbacks ──────────────────────────────────────────────────────────
static void drag_begin(GtkGestureDrag*, double wx, double wy, gpointer a) {
    GtkWidget* w = GTK_WIDGET(a);
    switch(active_tool) {
    case Tool::Brush:
    case Tool::Eraser:
        in_stroke = true; sw0 = slx = wx; sh0 = sly = wy;
        stroke_pts.clear();
        stroke_pts.push_back({wx, wy, 1.0});
        { std::size_t cx, cy;
          if(w2c(w, wx, wy, cx, cy)) dab_at(cx, cy); }
        refresh_view();
        break;
    case Tool::Fill:
        { std::size_t cx, cy;
          if(w2c(w, wx, wy, cx, cy)) do_fill(cx, cy); }
        break;
    case Tool::Eyedropper:
        { refresh_composite();  // make sure cbuf is fresh
          std::size_t cx, cy;
          if(w2c(w, wx, wy, cx, cy)) do_eyedropper(cx, cy); }
        break;
    case Tool::Move:
        move_wx0 = wx; move_wy0 = wy; move_dx = 0; move_dy = 0;
        in_stroke = true;
        break;
    case Tool::Select:
        sel_x0 = sel_x1 = wx; sel_y0 = sel_y1 = wy;
        sel_active = true; in_stroke = true;
        sw0 = wx; sh0 = wy;
        if(canvas_w) gtk_widget_queue_draw(canvas_w);
        break;
    default:
        if(status_lbl) gtk_label_set_text(status_lbl, "Outil en cours d'implémentation");
        break;
    }
}

static void drag_update(GtkGestureDrag*, double tdx, double tdy, gpointer a) {
    if(!in_stroke) return;
    GtkWidget* w = GTK_WIDGET(a);
    double wx = sw0+tdx, wy = sh0+tdy;
    switch(active_tool) {
    case Tool::Brush:
    case Tool::Eraser:
        { // Catmull-Rom: add point then flush segment
          stroke_pts.push_back({wx, wy, 1.0});
          if(stroke_pts.size() > 8) stroke_pts.pop_front();
          flush_stroke_segment(w);
          slx = wx; sly = wy; refresh_view(); }
        break;
    case Tool::Move:
        { int ww = gtk_widget_get_width(w), wh = gtk_widget_get_height(w);
          double s = cscale(ww, wh);
          move_dx = tdx / s; move_dy = tdy / s; }
        break;
    case Tool::Select:
        sel_x1 = wx; sel_y1 = wy;
        if(canvas_w) gtk_widget_queue_draw(canvas_w);
        break;
    default: break;
    }
}

static void drag_end(GtkGestureDrag*, double tdx, double tdy, gpointer a) {
    GtkWidget* w = GTK_WIDGET(a);
    if(in_stroke) {
        switch(active_tool) {
        case Tool::Brush:
        case Tool::Eraser:
            { double wx = sw0+tdx, wy = sh0+tdy;
              stroke_pts.push_back({wx, wy, 1.0});
              flush_stroke_segment(w); }
            refresh_view();
            break;
        case Tool::Move:
            do_move_apply((int)std::round(move_dx), (int)std::round(move_dy));
            move_dx = move_dy = 0;
            break;
        case Tool::Select:
            sel_x1 = sw0+tdx; sel_y1 = sh0+tdy;
            if(canvas_w) gtk_widget_queue_draw(canvas_w);
            break;
        default: break;
        }
        in_stroke = false;
    }
    stroke_pts.clear();
}

// Stylus axis helper
static double ax(GtkGestureStylus* g, GdkAxisUse axis, double fb) {
    double v = 0;
    return gtk_gesture_stylus_get_axis(g, axis, &v) ? v : fb;
}
static void sty_down(GtkGestureStylus* g, double wx, double wy, gpointer a) {
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    sty_lx = wx; sty_ly = wy; sty_active = true;
    double p  = ax(g, GDK_AXIS_PRESSURE, 1.0);
    double tx = ax(g, GDK_AXIS_XTILT,    0.0);
    double ty = ax(g, GDK_AXIS_YTILT,    0.0);
    stroke_pts.clear();
    stroke_pts.push_back({wx, wy, p});
    std::size_t cx, cy;
    if(w2c(GTK_WIDGET(a), wx, wy, cx, cy)) dab_at(cx, cy, p, tx, ty);
    refresh_view();
}
static void sty_motion(GtkGestureStylus* g, double wx, double wy, gpointer a) {
    if(!sty_active) return;
    double p  = ax(g, GDK_AXIS_PRESSURE, 1.0);
    double tx = ax(g, GDK_AXIS_XTILT,    0.0);
    double ty = ax(g, GDK_AXIS_YTILT,    0.0);
    GtkWidget* w = GTK_WIDGET(a);
    int ww = gtk_widget_get_width(w), wh = gtk_widget_get_height(w);
    if(std::hypot(wx-sty_lx, wy-sty_ly) >=
       std::max(0.5, brush_r * brush_space * cscale(ww, wh))) {
        stroke_pts.push_back({wx, wy, p});
        if(stroke_pts.size() > 8) stroke_pts.pop_front();
        flush_stroke_segment(w);
        sty_lx = wx; sty_ly = wy; refresh_view();
    }
    cur_wx = wx; cur_wy = wy;
    if(canvas_w) gtk_widget_queue_draw(canvas_w);
}
static void sty_up(GtkGestureStylus*, double, double, gpointer) {
    sty_active = false; stroke_pts.clear();
}

// ── CSS — propre, sans letter-spacing ni font-family multilistes ──────────────
static const char CSS[] =
    "window{background:#111216;color:#dde1ed;font-family:sans-serif;font-size:13px}"
    ".topbar{background:#18191f;border-bottom:1px solid #24263a;padding:6px 14px}"
    ".brand{font-size:15px;font-weight:900;color:#9B1F35}"
    ".ver{font-size:10px;color:#4a4e6a;margin-left:4px}"
    ".left-panel{background:#16171d;border-right:1px solid #22243a;"
    "padding:8px 6px;min-width:190px}"
    ".right-panel{background:#16171d;border-left:1px solid #22243a;"
    "padding:8px 6px;min-width:200px}"
    ".section-head{font-size:9px;font-weight:700;color:#4a4e6a;margin:6px 0 3px 2px}"
    "button{background:#1e2030;color:#ccd0e8;border:1px solid #2b2f48;"
    "border-radius:5px;padding:5px 8px;min-height:0}"
    "button:hover{background:#252840}"
    "button.active{background:#7a1929;color:#ffd5dd;border-color:#9B1F35}"
    "button.primary{background:#7a1929;color:#ffd5dd;border-color:#9B1F35}"
    "button.primary:hover{background:#9B1F35}"
    "button.tool-btn{padding:6px;min-width:32px;border-radius:6px}"
    "button.tool-btn.active{background:#7a1929;border-color:#9B1F35}"
    "button.layer-name-btn{border-radius:4px;padding:4px 6px;font-size:12px}"
    "button.eye-btn{padding:3px;font-size:14px}"
    "button.preset-btn{padding:4px 6px;margin:2px 0;font-size:11px}"
    ".blend-dd{font-size:10px;min-height:0;padding:0}"
    "scale{padding:8px 4px}"
    "scale trough{background:#24263a;border-radius:3px;min-height:4px;min-width:4px}"
    "scale highlight{background:#9B1F35;border-radius:3px;min-height:4px}"
    "scale slider{background:#c24462;border:1px solid #9B1F35;border-radius:50%;-gtk-icon-size:16px}"
    ".slider-label{color:#6a6e8a;font-size:11px;min-width:65px}"
    ".statusbar{background:#0f1014;border-top:1px solid #1c1e2a;"
    "padding:4px 12px;font-size:11px;color:#4a4e6a}"
    "menubar{background:#16171d;border-bottom:1px solid #22243a}"
    "menubar>item{padding:4px 10px}"
    "menubar>item:hover{background:#1e2030}"
    "popover{background:#1e2030;border:1px solid #2b2f48;border-radius:6px}"
    "popover modelbutton{padding:6px 14px}"
    "popover modelbutton:hover{background:#7a1929;color:#fff}";

// ── open_editor ────────────────────────────────────────────────────────────────
static void open_editor(GtkApplication* app, const std::string& init_path = "") {
    if(layers.empty()) {
        lnames.push_back("Fond");
        layers.push_back(std::make_unique<creative::engine::TileStore>(doc_w, doc_h, TILE_SZ));
        lvis.push_back(true); lop.push_back(1.f); lmode.push_back(BlendMode::Normal);
        if(doc_bg == "white")      layers[0]->touch_region(0,0,doc_w,doc_h,0xffffffff);
        else if(doc_bg == "black") layers[0]->touch_region(0,0,doc_w,doc_h,0xff000000);
        active_layer = 0;
    }
    if(!init_path.empty()) {
        creative::engine::AtlasProject p;
        if(creative::engine::AtlasSerializer::load(init_path, p) && !p.layers.empty())
            reload_doc(p, init_path);
    }
    rebuild_eng();
    if(welcome_win) { gtk_window_destroy(welcome_win); welcome_win = nullptr; }

    auto* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, CSS);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    auto* win = GTK_WINDOW(gtk_application_window_new(app));
    editor_win = win;
    gtk_window_set_title(win, "Atlas v4 — Créative Studio");
    gtk_window_set_default_size(win, 1500, 940);

    // ── GMenu (menubar) ────────────────────────────────────────────────────────
    auto* mm = g_menu_new();
    { auto* m = g_menu_new();
      g_menu_append(m,"Nouveau…","win.new");
      g_menu_append(m,"Ouvrir…","win.open");
      g_menu_append(m,"Sauvegarder","win.save");
      g_menu_append(m,"Sauvegarder sous…","win.save-as");
      g_menu_append(m,"Exporter PNG…","win.export-png");
      g_menu_append_submenu(mm,"Fichier",G_MENU_MODEL(m)); g_object_unref(m); }
    { auto* m = g_menu_new();
      g_menu_append(m,"Annuler","win.undo");
      g_menu_append(m,"Rétablir","win.redo");
      g_menu_append_submenu(mm,"Édition",G_MENU_MODEL(m)); g_object_unref(m); }
    { auto* m = g_menu_new();
      g_menu_append(m,"Nouveau calque","win.new-layer");
      g_menu_append(m,"Supprimer calque","win.del-layer");
      g_menu_append_submenu(mm,"Calques",G_MENU_MODEL(m)); g_object_unref(m); }
    { auto* m = g_menu_new();
      g_menu_append(m,"Plein écran","win.fullscreen");
      g_menu_append_submenu(mm,"Affichage",G_MENU_MODEL(m)); g_object_unref(m); }
    { auto* m = g_menu_new();
      g_menu_append(m,"Pinceau (B)","win.tool-brush");
      g_menu_append(m,"Gomme (E)","win.tool-eraser");
      g_menu_append(m,"Remplissage (G)","win.tool-fill");
      g_menu_append(m,"Pipette (I)","win.tool-pick");
      g_menu_append(m,"Déplacer (V)","win.tool-move");
      g_menu_append(m,"Sélection (M)","win.tool-select");
      g_menu_append_submenu(mm,"Outils",G_MENU_MODEL(m)); g_object_unref(m); }
    { auto* m = g_menu_new();
      g_menu_append(m,"À propos d'Atlas","win.about");
      g_menu_append_submenu(mm,"Aide",G_MENU_MODEL(m)); g_object_unref(m); }
    auto* menubar = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(mm));
    g_object_unref(mm);

    const GActionEntry acts[] = {
        {"new",         G_ACTION_CALLBACK(action_new),       nullptr,nullptr,nullptr},
        {"open",        G_ACTION_CALLBACK(action_open),      nullptr,nullptr,nullptr},
        {"save",        G_ACTION_CALLBACK(action_save),      nullptr,nullptr,nullptr},
        {"save-as",     G_ACTION_CALLBACK(action_save_as),   nullptr,nullptr,nullptr},
        {"export-png",  G_ACTION_CALLBACK(action_export_png),nullptr,nullptr,nullptr},
        {"undo",        G_ACTION_CALLBACK(action_undo),      nullptr,nullptr,nullptr},
        {"redo",        G_ACTION_CALLBACK(action_redo),      nullptr,nullptr,nullptr},
        {"new-layer",   G_ACTION_CALLBACK(add_layer),        nullptr,nullptr,nullptr},
        {"del-layer",   G_ACTION_CALLBACK(del_layer),        nullptr,nullptr,nullptr},
        {"tool-brush",  G_ACTION_CALLBACK(+[](GSimpleAction*,GVariant*,gpointer){active_tool=Tool::Brush;}),    nullptr,nullptr,nullptr},
        {"tool-eraser", G_ACTION_CALLBACK(+[](GSimpleAction*,GVariant*,gpointer){active_tool=Tool::Eraser;}),   nullptr,nullptr,nullptr},
        {"tool-fill",   G_ACTION_CALLBACK(+[](GSimpleAction*,GVariant*,gpointer){active_tool=Tool::Fill;}),     nullptr,nullptr,nullptr},
        {"tool-pick",   G_ACTION_CALLBACK(+[](GSimpleAction*,GVariant*,gpointer){active_tool=Tool::Eyedropper;}),nullptr,nullptr,nullptr},
        {"tool-move",   G_ACTION_CALLBACK(+[](GSimpleAction*,GVariant*,gpointer){active_tool=Tool::Move;}),     nullptr,nullptr,nullptr},
        {"tool-select", G_ACTION_CALLBACK(+[](GSimpleAction*,GVariant*,gpointer){active_tool=Tool::Select;}),   nullptr,nullptr,nullptr},
        {"fullscreen",  G_ACTION_CALLBACK(+[](GSimpleAction*,GVariant*,gpointer w){gtk_window_fullscreen(GTK_WINDOW(w));}),nullptr,nullptr,nullptr},
        {"about",G_ACTION_CALLBACK(+[](GSimpleAction*,GVariant*,gpointer w){
            auto* dlg2=gtk_window_new();
            gtk_window_set_transient_for(GTK_WINDOW(dlg2),GTK_WINDOW(w));
            gtk_window_set_title(GTK_WINDOW(dlg2),"À propos d'Atlas");
            gtk_window_set_default_size(GTK_WINDOW(dlg2),340,0);
            gtk_window_set_modal(GTK_WINDOW(dlg2),TRUE);
            auto* vb=gtk_box_new(GTK_ORIENTATION_VERTICAL,10);
            gtk_widget_set_margin_top(vb,20);gtk_widget_set_margin_bottom(vb,20);
            gtk_widget_set_margin_start(vb,24);gtk_widget_set_margin_end(vb,24);
            auto* t=gtk_label_new("ATLAS v4");gtk_widget_add_css_class(t,"brand");gtk_box_append(GTK_BOX(vb),t);
            gtk_box_append(GTK_BOX(vb),gtk_label_new("Créative Studio 2.0\nLinux/Wayland\nPaintStorm x Photoshop x CreativeSysteme\n\n© 2024 CreativeSystem"));
            auto* ok2=gtk_button_new_with_label("OK");gtk_widget_add_css_class(ok2,"primary");
            g_signal_connect(ok2,"clicked",(GCallback)(+[](GtkButton*,gpointer d){gtk_window_destroy(GTK_WINDOW(d));}),dlg2);
            gtk_box_append(GTK_BOX(vb),ok2);gtk_window_set_child(GTK_WINDOW(dlg2),vb);gtk_window_present(GTK_WINDOW(dlg2));
        }),nullptr,nullptr,nullptr},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(win), acts, G_N_ELEMENTS(acts), win);

    // Keyboard shortcuts
    auto* kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", (GCallback)(
        +[](GtkEventControllerKey*, guint key, guint, GdkModifierType m, gpointer wp) -> gboolean {
        bool ctrl=(m&GDK_CONTROL_MASK)!=0, shift=(m&GDK_SHIFT_MASK)!=0;
        if(ctrl && !shift && (key==GDK_KEY_z||key==GDK_KEY_Z)){action_undo(nullptr,nullptr,nullptr);return TRUE;}
        if(ctrl && (key==GDK_KEY_y||key==GDK_KEY_Y||(shift&&(key==GDK_KEY_z||key==GDK_KEY_Z)))){action_redo(nullptr,nullptr,nullptr);return TRUE;}
        if(ctrl && (key==GDK_KEY_s||key==GDK_KEY_S)){action_save(nullptr,nullptr,wp);return TRUE;}
        if(ctrl && (key==GDK_KEY_n||key==GDK_KEY_N)){show_new_doc_dialog(GTK_WINDOW(wp));return TRUE;}
        if(ctrl && (key==GDK_KEY_o||key==GDK_KEY_O)){action_open(nullptr,nullptr,wp);return TRUE;}
        if(key==GDK_KEY_bracketleft  && brush_r > 1)   {--brush_r;if(canvas_w)gtk_widget_queue_draw(canvas_w);return TRUE;}
        if(key==GDK_KEY_bracketright && brush_r < 300)  {++brush_r;if(canvas_w)gtk_widget_queue_draw(canvas_w);return TRUE;}
        if(key==GDK_KEY_b||key==GDK_KEY_B){active_tool=Tool::Brush;return TRUE;}
        if(key==GDK_KEY_e||key==GDK_KEY_E){active_tool=Tool::Eraser;return TRUE;}
        if(key==GDK_KEY_g||key==GDK_KEY_G){active_tool=Tool::Fill;return TRUE;}
        if(key==GDK_KEY_i||key==GDK_KEY_I){active_tool=Tool::Eyedropper;return TRUE;}
        if(key==GDK_KEY_v||key==GDK_KEY_V){active_tool=Tool::Move;return TRUE;}
        if(key==GDK_KEY_m||key==GDK_KEY_M){active_tool=Tool::Select;return TRUE;}
        if(key==GDK_KEY_Escape){sel_active=false;if(canvas_w)gtk_widget_queue_draw(canvas_w);return TRUE;}
        return FALSE;
    }), win);
    gtk_widget_add_controller(GTK_WIDGET(win), kc);

    // ── Layout ─────────────────────────────────────────────────────────────────
    auto* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    // Topbar
    auto* topbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(topbar, "topbar");
    { auto* b = gtk_label_new("ATLAS"); gtk_widget_add_css_class(b,"brand"); gtk_box_append(GTK_BOX(topbar),b); }
    { auto* v = gtk_label_new("v4");    gtk_widget_add_css_class(v,"ver");   gtk_box_append(GTK_BOX(topbar),v); }
    { auto* sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0); gtk_widget_set_hexpand(sp,TRUE); gtk_box_append(GTK_BOX(topbar),sp); }
    { auto* u = gtk_button_new_with_label("↩ Annuler"); gtk_widget_add_css_class(u,"tool-btn");
      g_signal_connect(u,"clicked",(GCallback)(+[](GtkButton*,gpointer){action_undo(nullptr,nullptr,nullptr);}),nullptr);
      gtk_box_append(GTK_BOX(topbar),u); }
    { auto* r = gtk_button_new_with_label("↪ Rétablir"); gtk_widget_add_css_class(r,"tool-btn");
      g_signal_connect(r,"clicked",(GCallback)(+[](GtkButton*,gpointer){action_redo(nullptr,nullptr,nullptr);}),nullptr);
      gtk_box_append(GTK_BOX(topbar),r); }
    gtk_box_append(GTK_BOX(root), topbar);
    gtk_box_append(GTK_BOX(root), menubar);

    // Body: left | centre | right
    auto* hpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    // ── Left panel ─────────────────────────────────────────────────────────────
    auto* left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(left, "left-panel");
    gtk_widget_set_vexpand(left, TRUE);

    { auto* l = gtk_label_new("OUTILS"); gtk_widget_add_css_class(l,"section-head"); gtk_box_append(GTK_BOX(left),l); }
    auto* tgrid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(tgrid), 4);
    gtk_grid_set_row_spacing(GTK_GRID(tgrid), 4);
    struct TD { const char* icon; const char* tip; Tool t; };
    static const TD TDEFS[] = {
        {"brush.svg",            "Pinceau (B)",    Tool::Brush},
        {"eraser.svg",           "Gomme (E)",      Tool::Eraser},
        {"fill.svg",             "Remplissage (G)",Tool::Fill},
        {"picker.svg",           "Pipette (I)",    Tool::Eyedropper},
        {"move.svg",             "Déplacer (V)",   Tool::Move},
        {"select_rectangle.svg", "Sélection (M)",  Tool::Select},
        {"gradient.svg",         "Dégradé",        Tool::Gradient},
        {"text.svg",             "Texte",          Tool::Text},
        {"smudge.svg",           "Barbouillage",   Tool::Smudge},
        {"blur.svg",             "Flou",           Tool::Blur},
    };
    for(int ti = 0; ti < 10; ++ti) {
        auto* tb = icon_btn(TDEFS[ti].icon, TDEFS[ti].tip);
        if(TDEFS[ti].t == active_tool) gtk_widget_add_css_class(tb, "active");
        const Tool* tp = &TDEFS[ti].t;
        g_signal_connect(tb, "clicked", (GCallback)(+[](GtkButton*, gpointer u){
            active_tool = *static_cast<const Tool*>(u);
            if(canvas_w) gtk_widget_queue_draw(canvas_w);
        }), const_cast<Tool*>(tp));
        gtk_grid_attach(GTK_GRID(tgrid), tb, ti%2, ti/2, 1, 1);
    }
    gtk_box_append(GTK_BOX(left), tgrid);

    // Colour picker
    { auto* l = gtk_label_new("COULEUR"); gtk_widget_add_css_class(l,"section-head"); gtk_box_append(GTK_BOX(left),l); }
    auto* colbtn = gtk_color_dialog_button_new(gtk_color_dialog_new());
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(colbtn), &brush_col);
    gtk_widget_set_hexpand(colbtn, TRUE);
    g_signal_connect(colbtn, "notify::rgba", (GCallback)(+[](GObject* obj, GParamSpec*, gpointer){
        const GdkRGBA* c = gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(obj));
        if(c) brush_col = *c;
    }), nullptr);
    gtk_box_append(GTK_BOX(left), colbtn);

    // Quick brush/eraser toggle
    { auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      auto* bb = gtk_button_new_with_label("Pinceau");
      auto* eb = gtk_button_new_with_label("Gomme");
      gtk_widget_set_hexpand(bb,TRUE); gtk_widget_set_hexpand(eb,TRUE);
      gtk_widget_add_css_class(bb, "active");
      g_signal_connect(bb,"clicked",(GCallback)(+[](GtkButton* b,gpointer e){
          active_tool=Tool::Brush;gtk_widget_add_css_class(GTK_WIDGET(b),"active");
          gtk_widget_remove_css_class(GTK_WIDGET(e),"active");}),eb);
      g_signal_connect(eb,"clicked",(GCallback)(+[](GtkButton* b,gpointer br){
          active_tool=Tool::Eraser;gtk_widget_add_css_class(GTK_WIDGET(b),"active");
          gtk_widget_remove_css_class(GTK_WIDGET(br),"active");}),bb);
      gtk_box_append(GTK_BOX(row),bb); gtk_box_append(GTK_BOX(row),eb);
      gtk_box_append(GTK_BOX(left),row); }

    // Brush sliders
    { auto* l = gtk_label_new("PINCEAU"); gtk_widget_add_css_class(l,"section-head"); gtk_box_append(GTK_BOX(left),l); }
    auto msl = [&](const char* lbl, double mn, double mx, double val, GCallback cb){
        auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        auto* l2 = gtk_label_new(lbl); gtk_widget_add_css_class(l2,"slider-label");
        gtk_box_append(GTK_BOX(row),l2);
        auto* sl = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, mn, mx, 1);
        gtk_scale_set_draw_value(GTK_SCALE(sl), TRUE);
        gtk_scale_set_value_pos(GTK_SCALE(sl), GTK_POS_RIGHT);
        gtk_range_set_value(GTK_RANGE(sl), val);
        gtk_widget_set_hexpand(sl, TRUE);
        g_signal_connect(sl, "value-changed", cb, nullptr);
        gtk_box_append(GTK_BOX(row),sl);
        gtk_box_append(GTK_BOX(left),row);
        return sl;
    };
    msl("Taille",    1, 300, 10, (GCallback)(+[](GtkRange* r,gpointer){brush_r   =int(gtk_range_get_value(r));}));
    msl("Dureté %",  1, 100, 70, (GCallback)(+[](GtkRange* r,gpointer){brush_hard=gtk_range_get_value(r)/100.0;}));
    msl("Opacité %", 1, 100,100, (GCallback)(+[](GtkRange* r,gpointer){brush_op  =gtk_range_get_value(r)/100.0;}));
    msl("Espac. %",  1,  50,  5, (GCallback)(+[](GtkRange* r,gpointer){brush_space=gtk_range_get_value(r)/100.0;}));

    { auto* cb = gtk_button_new_with_label("Effacer calque");
      g_signal_connect(cb,"clicked",(GCallback)(+[](GtkButton*,gpointer){
          if(auto* s=active_store()){s->clear();invalidate_bg_cache();refresh_view();}
      }),nullptr);
      gtk_box_append(GTK_BOX(left),cb); }

    // Presets panel
    { auto* l = gtk_label_new("PRERÉGLAGES"); gtk_widget_add_css_class(l,"section-head"); gtk_box_append(GTK_BOX(left),l); }
    { auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
      auto* ap = gtk_button_new_with_label("+ Enreg.");
      auto* dp = gtk_button_new_with_label("- Suppr.");
      gtk_widget_set_hexpand(ap,TRUE); gtk_widget_set_hexpand(dp,TRUE);
      g_signal_connect(ap,"clicked",(GCallback)(+[](GtkButton*,gpointer){
          BrushPreset p;
          p.name="Preset "+std::to_string(presets.size()+1);
          p.r=brush_r;p.op=brush_op;p.hard=brush_hard;p.space=brush_space;
          p.cr=brush_col.red;p.cg=brush_col.green;p.cb=brush_col.blue;
          presets.push_back(p);save_presets();refresh_preset_panel();
      }),nullptr);
      g_signal_connect(dp,"clicked",(GCallback)(+[](GtkButton*,gpointer){
          if(active_preset>=0&&active_preset<int(presets.size())){
              presets.erase(presets.begin()+active_preset);
              active_preset=-1;save_presets();refresh_preset_panel();}
      }),nullptr);
      gtk_box_append(GTK_BOX(row),ap); gtk_box_append(GTK_BOX(row),dp);
      gtk_box_append(GTK_BOX(left),row); }
    { auto* sc = gtk_scrolled_window_new();
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
      gtk_widget_set_size_request(sc,-1,100);
      preset_lb = gtk_list_box_new();
      gtk_list_box_set_selection_mode(GTK_LIST_BOX(preset_lb),GTK_SELECTION_NONE);
      gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc),preset_lb);
      gtk_box_append(GTK_BOX(left),sc); }

    gtk_paned_set_start_child(GTK_PANED(hpaned), left);
    gtk_paned_set_resize_start_child(GTK_PANED(hpaned), FALSE);

    // ── Centre + right ────────────────────────────────────────────────────────
    auto* rpaned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    auto* cbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(cbox, TRUE); gtk_widget_set_vexpand(cbox, TRUE);

    auto* cda = GTK_DRAWING_AREA(gtk_drawing_area_new());
    gtk_widget_set_hexpand(GTK_WIDGET(cda), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(cda), TRUE);
    gtk_drawing_area_set_draw_func(cda, draw_cb, nullptr, nullptr);
    canvas_w = GTK_WIDGET(cda);

    // Mouse gestures
    auto* drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 1);
    g_signal_connect(drag,"drag-begin", G_CALLBACK(drag_begin), cda);
    g_signal_connect(drag,"drag-update",G_CALLBACK(drag_update),cda);
    g_signal_connect(drag,"drag-end",   G_CALLBACK(drag_end),   cda);
    gtk_widget_add_controller(GTK_WIDGET(cda), GTK_EVENT_CONTROLLER(drag));

    // Motion (cursor)
    auto* mot = gtk_event_controller_motion_new();
    g_signal_connect(mot,"motion",(GCallback)(+[](GtkEventControllerMotion*,double x,double y,gpointer){
        cur_wx=x; cur_wy=y; cur_in=true;
        if(canvas_w) gtk_widget_queue_draw(canvas_w);
        if(coord_lbl && canvas_w){
            std::size_t cx=0,cy=0;
            if(w2c(canvas_w,x,y,cx,cy)){
                char buf[64]; std::snprintf(buf,64,"  %zu, %zu px",cx,cy);
                gtk_label_set_text(coord_lbl,buf);
            }
        }
    }),nullptr);
    g_signal_connect(mot,"leave",(GCallback)(+[](GtkEventControllerMotion*,gpointer){
        cur_in=false; if(canvas_w)gtk_widget_queue_draw(canvas_w);
    }),nullptr);
    gtk_widget_add_controller(GTK_WIDGET(cda), GTK_EVENT_CONTROLLER(mot));

    // Stylus (XPPEN pressure + tilt)
    auto* sty = GTK_GESTURE_STYLUS(gtk_gesture_stylus_new());
    gtk_gesture_stylus_set_stylus_only(sty, TRUE);
    g_signal_connect(sty,"down",  G_CALLBACK(sty_down),  cda);
    g_signal_connect(sty,"motion",G_CALLBACK(sty_motion),cda);
    g_signal_connect(sty,"up",    G_CALLBACK(sty_up),    cda);
    gtk_widget_add_controller(GTK_WIDGET(cda), GTK_EVENT_CONTROLLER(sty));

    gtk_box_append(GTK_BOX(cbox), GTK_WIDGET(cda));
    gtk_paned_set_start_child(GTK_PANED(rpaned), cbox);

    // ── Right panel: layers ─────────────────────────────────────────────────
    auto* right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(right, "right-panel");
    gtk_widget_set_vexpand(right, TRUE);

    { auto* l = gtk_label_new("CALQUES"); gtk_widget_add_css_class(l,"section-head"); gtk_box_append(GTK_BOX(right),l); }
    { auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
      auto* al = gtk_button_new_with_label("+ Ajouter");
      auto* dl = gtk_button_new_with_label("- Suppr.");
      gtk_widget_set_hexpand(al,TRUE); gtk_widget_set_hexpand(dl,TRUE);
      g_signal_connect(al,"clicked",G_CALLBACK(add_layer),nullptr);
      g_signal_connect(dl,"clicked",G_CALLBACK(del_layer),nullptr);
      gtk_box_append(GTK_BOX(row),al); gtk_box_append(GTK_BOX(row),dl);
      gtk_box_append(GTK_BOX(right),row); }
    { auto* sc = gtk_scrolled_window_new();
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),GTK_POLICY_NEVER,GTK_POLICY_AUTOMATIC);
      gtk_widget_set_vexpand(sc, TRUE);
      layer_lb = gtk_list_box_new();
      gtk_list_box_set_selection_mode(GTK_LIST_BOX(layer_lb),GTK_SELECTION_NONE);
      gtk_widget_set_hexpand(layer_lb, TRUE);
      gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), layer_lb);
      gtk_box_append(GTK_BOX(right), sc); }

    gtk_paned_set_end_child(GTK_PANED(rpaned), right);
    gtk_paned_set_resize_end_child(GTK_PANED(rpaned), FALSE);
    gtk_paned_set_end_child(GTK_PANED(hpaned), rpaned);
    gtk_widget_set_hexpand(hpaned, TRUE); gtk_widget_set_vexpand(hpaned, TRUE);
    gtk_box_append(GTK_BOX(root), hpaned);

    // Statusbar
    auto* sb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class(sb, "statusbar");
    status_lbl = GTK_LABEL(gtk_label_new("Prêt"));
    coord_lbl  = GTK_LABEL(gtk_label_new(""));
    gtk_box_append(GTK_BOX(sb), GTK_WIDGET(status_lbl));
    gtk_box_append(GTK_BOX(sb), GTK_WIDGET(coord_lbl));
    { auto* sp = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0); gtk_widget_set_hexpand(sp,TRUE); gtk_box_append(GTK_BOX(sb),sp); }
    std::string info = std::to_string(doc_w)+"x"+std::to_string(doc_h)+" · "+std::to_string(doc_dpi)+" DPI";
    gtk_box_append(GTK_BOX(sb), gtk_label_new(info.c_str()));
    gtk_box_append(GTK_BOX(sb), gtk_label_new(gpu_off() ? "Mode CPU" : "Mode GPU"));
    gtk_box_append(GTK_BOX(root), sb);

    gtk_window_set_child(win, root);
    load_presets(); refresh_layer_panel(); refresh_preset_panel();
    init_cbuf(); refresh_composite();
    gtk_window_present(win);

    // ── GPU init différé: attendre realize du widget canvas ──────────────────
    if(!gpu_off()) {
        g_signal_connect(GTK_WIDGET(cda), "realize", (GCallback)(+[](GtkWidget* w, gpointer) {
            // Widget est maintenant réalisé — GdkSurface disponible
            if(!native_surf) {
                native_surf = std::make_unique<creative::ui::NativeCanvasSurface>(w);
                if(!native_surf->initialize()) {
                    native_surf.reset();
                    std::fprintf(stderr, "Atlas v4: GPU init échoué après realize → CPU\n");
                } else {
                    std::fprintf(stderr, "Atlas v4: GPU init réussi\n");
                }
            }
        }), nullptr);
    }
}

// ── Welcome screen ─────────────────────────────────────────────────────────────
static void activate(GtkApplication* app, gpointer) {
    if(editor_win) { gtk_window_present(editor_win); return; }

    auto* css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window{background:#111216;color:#dde1ed;font-family:sans-serif}"
        ".brand{font-size:42px;font-weight:900;color:#9B1F35}"
        ".sub{font-size:14px;color:#4a4e6a;margin-bottom:28px}"
        "button{background:#1e2030;color:#ccd0e8;border:1px solid #2b2f48;"
        "border-radius:8px;padding:10px 20px}"
        "button:hover{background:#252840}"
        "button.primary{background:#7a1929;color:#ffd5dd;border-color:#9B1F35}"
        "button.primary:hover{background:#9B1F35}");
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    welcome_win = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_title(welcome_win, "Atlas v4 — Bienvenue");
    gtk_window_set_default_size(welcome_win, 700, 440);

    auto* vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_top(vb,56); gtk_widget_set_margin_bottom(vb,56);
    gtk_widget_set_margin_start(vb,64); gtk_widget_set_margin_end(vb,64);

    { auto* b = gtk_label_new("ATLAS"); gtk_widget_add_css_class(b,"brand"); gtk_box_append(GTK_BOX(vb),b); }
    { auto* s = gtk_label_new("v4 — Creative Studio 2.0 — PaintStorm x Photoshop x CreativeSysteme");
      gtk_widget_add_css_class(s,"sub"); gtk_box_append(GTK_BOX(vb),s); }

    auto* brow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    auto* nb = gtk_button_new_with_label("+ Nouveau document");
    auto* ob = gtk_button_new_with_label("Ouvrir .atlas...");
    gtk_widget_add_css_class(nb, "primary");
    gtk_widget_set_size_request(nb, 220, 48);
    gtk_widget_set_size_request(ob, 180, 48);
    gtk_box_append(GTK_BOX(brow), nb); gtk_box_append(GTK_BOX(brow), ob);
    gtk_box_append(GTK_BOX(vb), brow);

    g_signal_connect(nb, "clicked", (GCallback)(+[](GtkButton*, gpointer d){
        open_editor(GTK_APPLICATION(d));
    }), app);

    g_signal_connect(ob, "clicked", (GCallback)(+[](GtkButton*, gpointer d){
        auto* dlg = gtk_file_dialog_new();
        gtk_file_dialog_set_title(dlg, "Ouvrir un projet Atlas");
        gtk_file_dialog_open(dlg, welcome_win, nullptr,
            +[](GObject* src, GAsyncResult* res, gpointer ud){
                GError* e = nullptr;
                auto* f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, &e);
                if(f){ open_editor(GTK_APPLICATION(ud), g_file_get_path(f)); g_object_unref(f); }
                if(e) g_error_free(e);
            }, d);
        g_object_unref(dlg);
    }), app);

    { auto* gl = gtk_label_new(gpu_off() ? "Mode : CPU" : "Mode : GPU (Wayland/EGL)");
      gtk_widget_add_css_class(gl,"sub"); gtk_box_append(GTK_BOX(vb),gl); }

    gtk_window_set_child(welcome_win, vb);
    gtk_window_present(welcome_win);
}

static void shutdown_app(GtkApplication*, gpointer) {
    native_surf.reset();
    save_presets();
}

int main(int argc, char** argv) {
    auto* app = gtk_application_new("org.creativesystem.atlas",
                                    G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate),     nullptr);
    g_signal_connect(app, "shutdown", G_CALLBACK(shutdown_app), nullptr);
    int s = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return s;
}
