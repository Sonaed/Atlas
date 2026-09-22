// Atlas v4 — Creative Studio Linux/Wayland
// "PaintStorm × Photoshop × CreativeSysteme 2.0"
// Fixes: CSS propre, GPU différé, cache composite, outils complets, Catmull-Rom
#include <gtk/gtk.h>
#include "ui/gtk/native_canvas_surface.h"
#include "engine/brush/brush_engine.h"
#include "engine/serialization/atlas_project.h"
#include "engine/memory/memory_manager.h"
#include "ui/gtk/color_wheel.h"
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
// Roue chromatique et libellé hexadécimal du panneau Couleur. Nuls tant que
// l'éditeur n'est pas construit, remis à nul quand leurs widgets disparaissent.
static creative::ui::ColorWheel* g_wheel   = nullptr;
static GtkWidget*                g_col_hex = nullptr;
static GtkWidget*                g_col_swatch = nullptr;

// À appeler après toute modification de brush_col qui ne vient pas de la roue
// (pipette, préréglage…), pour que l'interface reflète la couleur réelle.
static void sync_color_ui() {
    if(g_wheel) g_wheel->set_rgba(brush_col);
    if(g_col_hex) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X",
            unsigned(std::clamp(brush_col.red,  0.f,1.f)*255.f + .5f),
            unsigned(std::clamp(brush_col.green,0.f,1.f)*255.f + .5f),
            unsigned(std::clamp(brush_col.blue, 0.f,1.f)*255.f + .5f));
        gtk_label_set_text(GTK_LABEL(g_col_hex), buf);
    }
    if(g_col_swatch) gtk_widget_queue_draw(g_col_swatch);
}
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

// Dirty-rect : seule la zone touchée par le pinceau est recompositée.
static std::size_t dr_x0=0, dr_y0=0, dr_x1=0, dr_y1=0;
static bool dr_valid = false;
// Tampon de snapshot réutilisé : évite d'allouer le canvas entier à chaque frame.
static std::vector<std::byte> snap_buf;
static void dirty_expand(std::size_t cx, std::size_t cy, std::size_t r) {
    std::size_t x0 = cx > r ? cx-r : 0;
    std::size_t y0 = cy > r ? cy-r : 0;
    std::size_t x1 = std::min(doc_w, cx+r+1);
    std::size_t y1 = std::min(doc_h, cy+r+1);
    if(!dr_valid){ dr_x0=x0;dr_y0=y0;dr_x1=x1;dr_y1=y1;dr_valid=true; }
    else { dr_x0=std::min(dr_x0,x0);dr_y0=std::min(dr_y0,y0);
           dr_x1=std::max(dr_x1,x1);dr_y1=std::max(dr_y1,y1); }
}

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

// Variante région : même logique, mais seulement sur [rx0,rx1) × [ry0,ry1)
static void composite_layer_onto_region(
    std::vector<unsigned char>& dst,
    const std::vector<std::byte>& src,
    float lopacity, BlendMode lm,
    std::size_t rx0, std::size_t ry0, std::size_t rx1, std::size_t ry1)
{
    for(std::size_t py = ry0; py < ry1; ++py) {
        auto drow = py * std::size_t(cstride);
        auto srow = py * doc_w * 4;
        for(std::size_t px = rx0; px < rx1; ++px) {
            auto si = srow + px*4;
            unsigned sa8 = std::to_integer<unsigned>(src[si+3]);
            if(!sa8) continue;
            float sr = std::to_integer<unsigned>(src[si])   / 255.f;
            float sg = std::to_integer<unsigned>(src[si+1]) / 255.f;
            float sb = std::to_integer<unsigned>(src[si+2]) / 255.f;
            float sa = sa8 / 255.f * lopacity;
            auto di = drow + px*4;
            float da = dst[di+3] / 255.f;
            float dr2 = da > 0 ? dst[di+2] / 255.f / da : 0.f;
            float dg2 = da > 0 ? dst[di+1] / 255.f / da : 0.f;
            float db2 = da > 0 ? dst[di]   / 255.f / da : 0.f;
            blend_px(sr, sg, sb, sa, dr2, dg2, db2, da, lm);
            dst[di]   = (unsigned char)std::clamp(db2*da*255.f, 0.f, 255.f);
            dst[di+1] = (unsigned char)std::clamp(dg2*da*255.f, 0.f, 255.f);
            dst[di+2] = (unsigned char)std::clamp(dr2*da*255.f, 0.f, 255.f);
            dst[di+3] = (unsigned char)std::clamp(da*255.f,     0.f, 255.f);
        }
    }
}

// Invalide le cache bg (appeler quand la structure des calques change)
static void invalidate_bg_cache() {
    bg_cache_valid = false;
    active_rev_cached = (std::uint64_t)-1;
}

// Ordre des calques : l'indice 0 est le calque du DESSUS (un nouveau calque
// est inséré en tête), le dernier indice est le fond. La composition va donc
// du dernier indice vers 0.
//
// bg_cache ne contient que les calques SOUS le calque actif (indices > actif).
// L'ancienne version y mettait tous les calques sauf l'actif, y compris ceux du
// dessus, puis composait l'actif par-dessus le tout : peindre sur un calque qui
// n'était pas au sommet l'affichait devant les calques censés le recouvrir.
// Les calques du dessus sont recomposés à chaque frame par-dessus l'actif (voir
// refresh_composite) : on ne peut pas les précomposer à part, car un mode de
// fusion autre que Normal dépend de ce qu'ils recouvrent, actif compris.
static void rebuild_bg_cache() {
    if(layers.empty()) return;
    std::fill(bg_cache.begin(), bg_cache.end(), 0u);
    const int al = active_layer;
    for(int li = int(layers.size())-1; li > al; --li) {
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

    if(snap_buf.size() != doc_w*doc_h*4) snap_buf.assign(doc_w*doc_h*4, std::byte{0});

    // Région à recompositer : la dirty-rect du pinceau si elle est exploitable,
    // sinon le canvas entier (fill, changement de calque, chargement…).
    const bool use_region = dr_valid && bg_cache_valid
                         && bg_cache_active_idx == active_layer;
    const std::size_t rx0 = use_region ? dr_x0 : 0;
    const std::size_t ry0 = use_region ? dr_y0 : 0;
    const std::size_t rx1 = use_region ? dr_x1 : doc_w;
    const std::size_t ry1 = use_region ? dr_y1 : doc_h;

    // bg_cache → cbuf, sur la région seulement
    for(std::size_t py=ry0; py<ry1; ++py){
        const auto row = py*std::size_t(cstride);
        std::copy(bg_cache.begin()+row+rx0*4,
                  bg_cache.begin()+row+rx1*4,
                  cbuf.begin()+row+rx0*4);
    }

    // Calque actif, puis calques du dessus (indices actif-1 … 0), dans l'ordre
    // bas → haut, sur la même région. snapshot_region n'alloue rien et ne lit
    // que les tuiles concernées ; le coût reste proportionnel à la zone
    // modifiée, multiplié par le nombre de calques au-dessus de l'actif.
    for(int li = active_layer; li >= 0; --li) {
        const auto idx = std::size_t(li);
        if(idx >= layers.size() || !lvis[idx]) continue;
        layers[idx]->snapshot_region(rx0, ry0, rx1, ry1, snap_buf.data());
        composite_layer_onto_region(cbuf, snap_buf, lop[idx], lmode[idx],
                                    rx0, ry0, rx1, ry1);
    }

    dr_valid = false;
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
static GtkWidget* g_tool_btns[10] = {}; // pour maj highlight outil

// Cursor
static double cur_wx = -9999, cur_wy = -9999;
static bool   cur_in = false;

// Selection state
static bool   sel_active = false;
static double sel_x0 = 0, sel_y0 = 0, sel_x1 = 0, sel_y1 = 0;

// Stroke state
static double sw0=0, sh0=0, slx=0, sly=0;
static bool   in_stroke = false;

// Move tool state
static double move_dx = 0, move_dy = 0;  // accumulated pixel offset (canvas space)
static double move_wx0 = 0, move_wy0 = 0; // widget coords at drag start

// Catmull-Rom stroke buffer
struct StrokePt { double x, y, p; };
static std::deque<StrokePt> stroke_pts;

static creative::engine::TileStore dummy_store(1024, 1024, TILE_SZ);
static creative::engine::BrushEngine brush_eng(dummy_store);
static std::unique_ptr<creative::ui::NativeCanvasSurface> native_surf;

// Rendu GPU : opt-in via CREATIVE_ENABLE_GPU=1, volontairement.
// Le chemin actuel (sous-surface Wayland + EGL) n'est pas prêt à être activé
// par défaut : il n'affiche que le calque actif, l'étire sur toute la zone, et
// se place au-dessus de GTK, masquant damier, curseur et sélection dessinés
// par Cairo. Voir docs/ARCHITECTURE.md, section « Rendu GPU ».
// Cette fonction dit seulement si l'on *tente* le GPU ; le libellé de la barre
// d'état reflète ensuite le résultat réel de l'initialisation.
static bool gpu_off() {
    const char* e = std::getenv("CREATIVE_ENABLE_GPU");
    return !(e && std::string(e) == "1");
}
// Libellé de la barre d'état, mis à jour une fois le résultat de l'init connu.
static GtkWidget* g_render_mode_lbl = nullptr;
static void set_render_mode_label(const char* txt) {
    if(g_render_mode_lbl) gtk_label_set_text(GTK_LABEL(g_render_mode_lbl), txt);
}

static void refresh_view() {
    static gint64 last = 0; gint64 now = g_get_monotonic_time();
    if(now - last > 14000) { refresh_composite(); last = now; }
    if(native_surf && native_surf->gpu_ready() && active_store()) {
        int ww = gtk_widget_get_width(canvas_w), wh = gtk_widget_get_height(canvas_w);
        native_surf->resize(ww, wh);
        native_surf->sync_layout();
        native_surf->render(*active_store());
    }
    if(canvas_w) gtk_widget_queue_draw(canvas_w);
}
static void rebuild_eng() {
    if(active_store()) brush_eng.set_store(*active_store());
}

// ── Annulation ───────────────────────────────────────────────────────────────
// Un pas ne retient que les tuiles réellement modifiées par l'opération.
// Appliquer un pas échange contenu courant et pré-image : le pas devient son
// propre inverse, donc le rétablissement ne coûte aucun stockage de plus.
struct UndoEntry {
    std::size_t layer;
    creative::engine::TileStore::UndoStep step;
};
static std::deque<UndoEntry> undo_stack, redo_stack;
static constexpr std::size_t UNDO_MAX = 40;

static void undo_begin() {
    if(auto* s = active_store()) s->begin_record();
}

static void undo_commit() {
    auto* s = active_store();
    if(!s) return;
    auto step = s->commit_record();
    if(step.empty()) return;                 // rien n'a bougé : pas de pas vide
    undo_stack.push_back({std::size_t(active_layer), std::move(step)});
    while(undo_stack.size() > UNDO_MAX) undo_stack.pop_front();
    redo_stack.clear();                      // une action neuve invalide le redo
}


static bool undo_apply(std::deque<UndoEntry>& from, std::deque<UndoEntry>& to) {
    if(from.empty()) return false;
    UndoEntry e = std::move(from.back());
    from.pop_back();
    if(e.layer >= layers.size()) return false;
    layers[e.layer]->apply_step(e.step);     // e.step devient l'inverse
    to.push_back(std::move(e));
    invalidate_bg_cache();
    return true;
}

// ── Préférences mémoire / scratch disk ───────────────────────────────────────
static std::string human_bytes(std::size_t b) {
    char buf[64];
    const double K = 1024.0, M = K*1024, G = M*1024;
    if      (b >= (std::size_t)G) std::snprintf(buf,sizeof buf,"%.2f Gio", double(b)/G);
    else if (b >= (std::size_t)M) std::snprintf(buf,sizeof buf,"%.0f Mio", double(b)/M);
    else if (b >= (std::size_t)K) std::snprintf(buf,sizeof buf,"%.0f Kio", double(b)/K);
    else                          std::snprintf(buf,sizeof buf,"%zu o", b);
    return buf;
}

struct MemPrefs {
    GtkWidget* lbl_budget   = nullptr;
    GtkWidget* lbl_stats    = nullptr;
    GtkWidget* entry_dir    = nullptr;
    GtkWidget* lbl_msg      = nullptr;
    guint      tick         = 0;
};

static void mem_update_budget_label(MemPrefs* mp, int percent) {
    auto& mm = creative::engine::MemoryManager::instance();
    const std::size_t total = mm.physical_ram_bytes();
    std::string s = std::to_string(percent) + " % de "
                  + human_bytes(total) + "  →  " + human_bytes(mm.budget_bytes());
    gtk_label_set_text(GTK_LABEL(mp->lbl_budget), s.c_str());
}

static gboolean mem_prefs_tick(gpointer data) {
    auto* mp = static_cast<MemPrefs*>(data);
    auto& mm = creative::engine::MemoryManager::instance();
    std::string s = "Résident : "   + human_bytes(mm.resident_bytes())
                  + "     Sur scratch : " + human_bytes(mm.swapped_bytes())
                  + "\nPic d'usage : " + human_bytes(mm.peak_bytes())
                  + "     Fichier scratch : " + human_bytes(mm.scratch_bytes());
    gtk_label_set_text(GTK_LABEL(mp->lbl_stats), s.c_str());
    return G_SOURCE_CONTINUE;
}

static void action_mem_prefs(GSimpleAction*, GVariant*, gpointer w) {
    auto& mm = creative::engine::MemoryManager::instance();
    auto* mp = new MemPrefs();

    auto* dlg = gtk_window_new();
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(w));
    gtk_window_set_title(GTK_WINDOW(dlg), "Mémoire et scratch disk");
    gtk_window_set_default_size(GTK_WINDOW(dlg), 460, 0);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);

    auto* vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(vb,20);    gtk_widget_set_margin_bottom(vb,20);
    gtk_widget_set_margin_start(vb,24);  gtk_widget_set_margin_end(vb,24);

    // ── Budget RAM ──
    { auto* t = gtk_label_new("Budget mémoire");
      gtk_widget_add_css_class(t,"brand");
      gtk_widget_set_halign(t, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(vb), t); }

    mp->lbl_budget = gtk_label_new("");
    gtk_widget_set_halign(mp->lbl_budget, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vb), mp->lbl_budget);

    const std::size_t total = mm.physical_ram_bytes();
    int cur_pct = total ? int(mm.budget_bytes() * 100 / total) : 60;
    cur_pct = std::clamp(cur_pct, 1, 95);

    auto* sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 95, 1);
    gtk_range_set_value(GTK_RANGE(sc), cur_pct);
    gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
    gtk_widget_set_hexpand(sc, TRUE);
    g_signal_connect(sc, "value-changed", (GCallback)(+[](GtkRange* r, gpointer d){
        auto* mp2 = static_cast<MemPrefs*>(d);
        const int pct = int(gtk_range_get_value(r));
        creative::engine::MemoryManager::instance().set_budget_percent(pct);
        mem_update_budget_label(mp2, pct);
    }), mp);
    gtk_box_append(GTK_BOX(vb), sc);
    mem_update_budget_label(mp, cur_pct);

    { auto* h = gtk_label_new("Au-delà de ce seuil, les tuiles les moins "
                              "récemment utilisées sont écrites sur le scratch "
                              "disk et rechargées à la demande. Tant que le "
                              "résident reste sous le budget, rien n'est évincé.");
      gtk_label_set_wrap(GTK_LABEL(h), TRUE);
      gtk_widget_set_halign(h, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(vb), h); }

    gtk_box_append(GTK_BOX(vb), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    // ── Scratch disk ──
    { auto* t = gtk_label_new("Scratch disk");
      gtk_widget_add_css_class(t,"brand");
      gtk_widget_set_halign(t, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(vb), t); }

    auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    mp->entry_dir = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(mp->entry_dir), mm.scratch_dir().c_str());
    gtk_widget_set_hexpand(mp->entry_dir, TRUE);
    gtk_box_append(GTK_BOX(row), mp->entry_dir);

    auto* apply = gtk_button_new_with_label("Appliquer");
    g_signal_connect(apply, "clicked", (GCallback)(+[](GtkButton*, gpointer d){
        auto* mp2 = static_cast<MemPrefs*>(d);
        const char* txt = gtk_editable_get_text(GTK_EDITABLE(mp2->entry_dir));
        auto& m = creative::engine::MemoryManager::instance();
        if(txt && *txt && m.set_scratch_dir(txt))
            gtk_label_set_text(GTK_LABEL(mp2->lbl_msg),
                "Répertoire appliqué. Le scratch en cours a été vidé.");
        else
            gtk_label_set_text(GTK_LABEL(mp2->lbl_msg),
                "Répertoire inexistant ou non accessible en écriture — inchangé.");
    }), mp);
    gtk_box_append(GTK_BOX(row), apply);
    gtk_box_append(GTK_BOX(vb), row);

    mp->lbl_msg = gtk_label_new("");
    gtk_widget_set_halign(mp->lbl_msg, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(mp->lbl_msg), TRUE);
    gtk_box_append(GTK_BOX(vb), mp->lbl_msg);

    { auto* h = gtk_label_new("Le fichier est supprimé du système de fichiers "
                              "dès sa création : il ne survit pas à la fermeture "
                              "d'Atlas, ni à un plantage.");
      gtk_label_set_wrap(GTK_LABEL(h), TRUE);
      gtk_widget_set_halign(h, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(vb), h); }

    gtk_box_append(GTK_BOX(vb), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    // ── Statistiques en direct ──
    mp->lbl_stats = gtk_label_new("");
    gtk_widget_set_halign(mp->lbl_stats, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vb), mp->lbl_stats);
    mem_prefs_tick(mp);
    mp->tick = g_timeout_add(500, mem_prefs_tick, mp);

    auto* purge = gtk_button_new_with_label("Vider la RAM vers le scratch maintenant");
    g_signal_connect(purge, "clicked", (GCallback)(+[](GtkButton*, gpointer d){
        auto* mp2 = static_cast<MemPrefs*>(d);
        std::size_t freed = 0;
        for(auto& s : layers) if(s) freed += s->evict_cold(s->bytes_resident());
        std::string m = freed
            ? "Éviction forcée : " + human_bytes(freed) + " écrits sur le scratch. "
              "Les tuiles seront rechargées à la demande quand tu dessineras."
            : std::string("Rien à évincer, ou scratch indisponible.");
        gtk_label_set_text(GTK_LABEL(mp2->lbl_msg), m.c_str());
    }), mp);
    gtk_box_append(GTK_BOX(vb), purge);

    auto* ok = gtk_button_new_with_label("Fermer");
    gtk_widget_add_css_class(ok, "primary");
    g_signal_connect(ok, "clicked", (GCallback)(+[](GtkButton*, gpointer d){
        gtk_window_destroy(GTK_WINDOW(d));
    }), dlg);
    gtk_box_append(GTK_BOX(vb), ok);

    // Le minuteur référence `mp` : il doit mourir avec la fenêtre.
    g_signal_connect(dlg, "destroy", (GCallback)(+[](GtkWidget*, gpointer d){
        auto* mp2 = static_cast<MemPrefs*>(d);
        if(mp2->tick) g_source_remove(mp2->tick);
        delete mp2;
    }), mp);

    gtk_window_set_child(GTK_WINDOW(dlg), vb);
    gtk_window_present(GTK_WINDOW(dlg));
}

// ── Dab / Catmull-Rom interpolation ──────────────────────────────────────────
static void dab_at(std::size_t cx, std::size_t cy, double p=1.0, double tx=0, double ty=0) {
    if(!active_store()) return;
    p = std::clamp(p, 0.05, 1.0);
    // tx/ty sont normalisés (-1..1) par l'appelant : pas de division par 90,
    // qui réduisait l'effet de l'inclinaison à 1,6 % de sa valeur.
    double tm = std::clamp(std::hypot(tx, ty), 0.0, 1.0);
    // Léger recouvrement supplémentaire pour éviter l'effet de chapelet
    // lorsque le pilote envoie des positions espacées.
    auto r = (std::size_t)std::max(1.0, brush_r * p * (1 + tm*.3) + 1.0);
    dirty_expand(cx, cy, r); // alimenter le dirty-rect avant de modifier les pixels
    if(active_tool == Tool::Eraser) {
        // Gomme : réduire l'alpha de chaque pixel dans la zone circulaire
        auto* store = active_store();
        if(!store) return;
        const double str = brush_op * p;
        const double R   = std::max(1.0, (double)r);
        const std::int64_t iy = (std::int64_t)cy, ix = (std::int64_t)cx;
        const std::int64_t ir = (std::int64_t)R;
        for(std::int64_t py = iy-ir; py <= iy+ir; ++py) {
            if(py < 0 || py >= (std::int64_t)doc_h) continue;
            const double dy  = double(py - iy);
            const double rem = R*R - dy*dy;
            if(rem < 0) continue;
            // Demi-largeur du disque sur cette ligne : pas de balayage du carré.
            const double hw = std::sqrt(rem);
            std::int64_t lo = std::max<std::int64_t>((std::int64_t)std::ceil (ix-hw), 0);
            std::int64_t hi = std::min<std::int64_t>((std::int64_t)std::floor(ix+hw),
                                                     (std::int64_t)doc_w-1);
            if(lo > hi) continue;

            std::size_t px = (std::size_t)lo;
            const std::size_t end = (std::size_t)hi;
            while(px <= end) {
                std::size_t run = 0;
                // Un seul calcul de tuile pour tout le segment, au lieu de
                // read_pixel + write_pixel (huit divisions) par pixel.
                std::byte* q = store->span_for_write(px, (std::size_t)py, run);
                if(!q || !run) break;
                run = std::min(run, end - px + 1);
                for(std::size_t i = 0; i < run; ++i) {
                    const double dx = double((std::int64_t)(px+i) - ix);
                    const double d  = std::sqrt(dx*dx + dy*dy);
                    if(d > R) continue;
                    const double fade   = brush_hard + (1.0-brush_hard)*(1.0 - d/R);
                    const double reduce = std::min(1.0, str * fade);
                    std::byte* t = q + i*4;
                    const unsigned pa = std::to_integer<unsigned>(t[3]);
                    t[3] = std::byte((unsigned)(pa * (1.0 - reduce)));
                }
                px += run;
            }
        }
        store->mark_modified();
        return;
    } else {
        auto cr = (unsigned)(std::clamp(brush_col.red,  0.0f,1.0f)*255);
        auto cg = (unsigned)(std::clamp(brush_col.green,0.0f,1.0f)*255);
        auto cb = (unsigned)(std::clamp(brush_col.blue, 0.0f,1.0f)*255);
        auto a  = (unsigned)(brush_op * p * 255);
        brush_eng.dab(cx, cy, r, cr|(cg<<8)|(cb<<16)|(a<<24), brush_hard);
    }
}

// Tangente avec laquelle le tracé est arrivé sur le dernier point émis.
// Le segment suivant la reprend telle quelle comme tangente de départ : c'est
// ce qui garantit l'absence de cassure, sans avoir besoin d'un point futur.
static double tan_x = 0, tan_y = 0;
static bool   tan_valid = false;

// Empreintes le long d'une courbe d'Hermite cubique de `a` vers `b`,
// avec les tangentes m0 (en a) et m1 (en b).
static void emit_hermite(GtkWidget* w, const StrokePt& a, const StrokePt& b,
                         double m0x, double m0y, double m1x, double m1y) {
    const int ww = gtk_widget_get_width(w), wh = gtk_widget_get_height(w);
    const double sc = cscale(ww, wh);
    // Tolérer les anciens presets (qui stockaient 5 au lieu de 0.05) et
    // garantir un recouvrement suffisant entre deux empreintes.
    const double spacing = std::clamp(brush_space, 0.01, 0.15);
    const double sp = std::max(0.25, brush_r * spacing * sc);

    const double chord = std::hypot(b.x-a.x, b.y-a.y);
    if(chord < 1e-9 && std::hypot(m0x,m0y) < 1e-9) return;
    // La courbe est plus longue que sa corde : estimer la longueur d'arc à
    // partir des tangentes pour ne pas sous-échantillonner les virages serrés.
    const double arc = chord * 1.2;  // la courbe est un peu plus longue que sa corde
    const int steps = std::max(1, (int)std::ceil(arc / sp));

    for(int s = 1; s <= steps; ++s) {
        const double t = double(s)/steps, t2 = t*t, t3 = t2*t;
        const double h00 =  2*t3 - 3*t2 + 1;
        const double h10 =      t3 - 2*t2 + t;
        const double h01 = -2*t3 + 3*t2;
        const double h11 =      t3 -   t2;
        const double wx = h00*a.x + h10*m0x + h01*b.x + h11*m1x;
        const double wy = h00*a.y + h10*m0y + h01*b.y + h11*m1y;
        const double pp = a.p + (b.p - a.p) * t;
        std::size_t cx, cy;
        if(w2c(w, wx, wy, cx, cy)) dab_at(cx, cy, pp);
    }
}

// Émet le segment qui se termine sur le point qui vient d'arriver.
//
// Aucun lookahead : le tracé ne traîne jamais derrière le curseur et rien ne
// surgit au relâchement. La continuité vient de la reprise de la direction
// d'arrivée du segment précédent comme direction de départ.
static void flush_latest_segment(GtkWidget* w) {
    const std::size_t n = stroke_pts.size();
    if(n < 2) return;
    const StrokePt& a = stroke_pts[n-2];
    const StrokePt& b = stroke_pts[n-1];

    double cx = b.x - a.x, cy = b.y - a.y;
    const double chord = std::hypot(cx, cy);
    if(chord < 1e-9) return;
    double dirx = cx / chord, diry = cy / chord;

    // La corde n'est pas la tangente : sur un arc elle est en retard d'un
    // demi-angle de virage. Prendre la corde pour tangente fait donc tourner
    // la courbe trop tard, puis rattraper — c'est l'oscillation visible à
    // vitesse élevée, où chaque corde couvre un angle plus grand.
    // On fait tourner la corde de la moitié du virage observé entre les deux
    // dernières cordes : pour un mouvement circulaire, cela redonne la
    // tangente exacte, sans jamais consulter le point suivant.
    if(n >= 3) {
        const StrokePt& z = stroke_pts[n-3];
        double px = a.x - z.x, py = a.y - z.y;
        const double plen = std::hypot(px, py);
        if(plen > 1e-9) {
            px /= plen; py /= plen;
            double turn = std::atan2(px*diry - py*dirx, px*dirx + py*diry);
            // Bridage : un virage aberrant (bruit, rebroussement) ne doit pas
            // envoyer la tangente n'importe où.
            turn = std::clamp(turn, -1.0, 1.0) * 0.5;
            const double cs = std::cos(turn), sn = std::sin(turn);
            const double rx = dirx*cs - diry*sn, ry = dirx*sn + diry*cs;
            dirx = rx; diry = ry;
        }
    }

    // Les deux tangentes sont ramenées à l'échelle de la corde courante.
    // Des magnitudes hétérogènes — héritées d'une corde plus longue — font
    // dépasser puis revenir la courbe dès que la vitesse du geste varie.
    const double m1x = dirx * chord, m1y = diry * chord;
    double m0x = m1x, m0y = m1y;
    if(tan_valid) { m0x = tan_x * chord; m0y = tan_y * chord; }

    emit_hermite(w, a, b, m0x, m0y, m1x, m1y);

    // On mémorise une direction unitaire : l'échelle est redonnée au segment
    // suivant par sa propre corde.
    tan_x = dirx; tan_y = diry; tan_valid = true;
}

// ── Fill tool (BFS flood fill) ────────────────────────────────────────────────
static void do_fill(std::size_t fx, std::size_t fy) {
    auto* store = active_store();
    if(!store) return;
    undo_begin();

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
    // Zone transparente : on garde la couleur courante. L'ancienne version
    // passait au noir, ce qui faisait perdre sa couleur sur un clic raté.
    if(ba == 0) return;
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
    sync_color_ui();
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
// ── Fond « carte du ciel » autour du document ────────────────────────────────
// Dégradé bleu nuit, graticule très discret et champ d'étoiles fixe. Rendu une
// seule fois par taille de zone puis réutilisé : son coût par frame est un
// simple blit. Les étoiles sont tirées d'un générateur à graine fixe, pour
// qu'elles ne bougent pas d'une frame ni d'un lancement à l'autre.
static cairo_surface_t* g_sky = nullptr;
static int g_sky_w = 0, g_sky_h = 0;

static void ensure_sky(int w, int h) {
    if(g_sky && g_sky_w == w && g_sky_h == h) return;
    if(g_sky) cairo_surface_destroy(g_sky);
    g_sky = cairo_image_surface_create(CAIRO_FORMAT_RGB24, std::max(1,w), std::max(1,h));
    g_sky_w = w; g_sky_h = h;
    cairo_t* c = cairo_create(g_sky);

    // Dégradé radial : légèrement plus clair au centre, comme un halo.
    auto* g = cairo_pattern_create_radial(w*.5, h*.45, 0, w*.5, h*.5, std::max(w,h)*.75);
    cairo_pattern_add_color_stop_rgb(g, 0.0, .047, .071, .125);
    cairo_pattern_add_color_stop_rgb(g, 1.0, .020, .031, .063);
    cairo_set_source(c, g); cairo_paint(c); cairo_pattern_destroy(g);

    // Graticule : lignes fines tous les 96 px, comme les méridiens d'une carte.
    cairo_set_line_width(c, 1.0);
    cairo_set_source_rgba(c, .34, .78, .86, .035);
    for(int x = 48; x < w; x += 96) { cairo_move_to(c, x + .5, 0); cairo_line_to(c, x + .5, h); }
    for(int y = 48; y < h; y += 96) { cairo_move_to(c, 0, y + .5); cairo_line_to(c, w, y + .5); }
    cairo_stroke(c);

    // Étoiles : densité proportionnelle à la surface, rares étoiles plus vives.
    std::uint32_t seed = 0x5A71A5u;
    auto rnd = [&seed]{ seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
    const int n = int(double(w) * h / 2600.0);
    for(int i = 0; i < n; ++i) {
        const double x = rnd() * w, y = rnd() * h, k = rnd();
        const double b = k > .985 ? .85 : .25 + rnd() * .35;
        const double r = k > .985 ? 1.1 : .55;
        const bool warm = rnd() > .8;   // quelques étoiles dorées parmi les bleutées
        cairo_set_source_rgba(c, warm ? .95 : .78, warm ? .86 : .86, warm ? .66 : 1.0, b);
        cairo_arc(c, x, y, r, 0, 2 * G_PI); cairo_fill(c);
    }
    cairo_destroy(c);
}

// Repères d'angle façon planche d'atlas, autour du document.
static void draw_plate_ticks(cairo_t* cr, double x, double y, double w, double h) {
    const double L = 10.0, o = 4.0;
    cairo_set_source_rgba(cr, .85, .72, .42, .75);   // or
    cairo_set_line_width(cr, 1.0);
    const double xs[2] = { x - o, x + w + o }, ys[2] = { y - o, y + h + o };
    for(int i = 0; i < 2; ++i) for(int j = 0; j < 2; ++j) {
        const double cx = xs[i] + .5, cy = ys[j] + .5;
        const double dx = i ? -L : L, dy = j ? -L : L;
        cairo_move_to(cr, cx + dx, cy); cairo_line_to(cr, cx, cy); cairo_line_to(cr, cx, cy + dy);
    }
    cairo_stroke(cr);
}

static void draw_cb(GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer) {
    ensure_sky(w, h);
    cairo_set_source_surface(cr, g_sky, 0, 0); cairo_paint(cr);
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
    // Bordure du document (cyan discret) et repères d'angle dorés.
    cairo_set_source_rgba(cr, .34, .78, .86, .45); cairo_set_line_width(cr, 1.0);
    cairo_rectangle(cr, ox-.5, oy-.5, doc_w*s+1, doc_h*s+1); cairo_stroke(cr);
    draw_plate_ticks(cr, ox, oy, doc_w*s, doc_h*s);
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
    sync_color_ui();
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
    if(undo_apply(undo_stack, redo_stack)) refresh_view();
}
static void action_redo(GSimpleAction*, GVariant*, gpointer) {
    if(undo_apply(redo_stack, undo_stack)) refresh_view();
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
        stroke_pts.clear(); tan_valid = false;
        stroke_pts.push_back({wx, wy, 1.0});
        { std::size_t cx, cy;
          if(w2c(w, wx, wy, cx, cy)) dab_at(cx, cy); }
        refresh_view();
        break;
    case Tool::Fill:
        { std::size_t cx, cy;
          if(w2c(w, wx, wy, cx, cy)) { do_fill(cx, cy); undo_commit(); } }
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
        { // Le contrôleur motion fournit l'échantillonnage continu.
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
            // Le segment final est déjà émis par raw_tablet_event (GDK_BUTTON_RELEASE).
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
    stroke_pts.clear(); tan_valid = false;
}

// Filtre de pression. Le pilote XP-Pen peut changer la pression plus vite que
// la distance minimale d'échantillonnage : un lissage léger évite les marches
// de rayon. Le coefficient est volontairement haut — à 0.28 il fallait environ
// huit événements pour suivre un changement, soit ~130 ms de retard, et un tap
// rapide n'atteignait jamais sa pression réelle.
static double g_pressure_filtered = 1.0;

// À appeler au début de chaque trait. Sans cette remise à zéro, l'état du
// filtre survivait d'un trait au suivant : relever le stylet en douceur faisait
// démarrer le trait suivant à la pression de fin du précédent.
static void reset_pressure_filter(double p) {
    g_pressure_filtered = std::clamp(std::isfinite(p) ? p : 1.0, 0.0, 1.0);
}

static double smooth_stylus_pressure(double p) {
    g_pressure_filtered += (p - g_pressure_filtered) * 0.55;
    return std::clamp(g_pressure_filtered, 0.0, 1.0);
}
// XP-Pen/Wayland fallback: certains backends exposent le stylet comme un
// pointeur et GtkGestureStylus ne reçoit alors pas la séquence complète.
static gboolean raw_tablet_event(GtkEventControllerLegacy*, GdkEvent* e, gpointer data) {
    auto* device = gdk_event_get_device(e);
    if(!device) return FALSE;
    const bool pen = gdk_device_get_source(device) == GDK_SOURCE_PEN;
    if(active_tool != Tool::Brush && active_tool != Tool::Eraser) return FALSE;
    auto* w = GTK_WIDGET(data); double x=0, y=0;
    if(!gdk_event_get_position(e, &x, &y)) return TRUE;
    // GdkEvent fournit des coordonnées relatives à la surface native, tandis
    // que w2c() attend des coordonnées relatives au DrawingArea.
    if(auto* native = gtk_widget_get_native(w)) {
#if GTK_CHECK_VERSION(4, 12, 0)
        // gtk_widget_translate_coordinates est déprécié depuis GTK 4.12.
        const graphene_point_t in = GRAPHENE_POINT_INIT(float(x), float(y));
        graphene_point_t out;
        if(gtk_widget_compute_point(GTK_WIDGET(native), w, &in, &out)) {
            x = out.x; y = out.y;
        }
#else
        double local_x=0, local_y=0;
        if(gtk_widget_translate_coordinates(GTK_WIDGET(native), w, x, y,
                                            &local_x, &local_y)) {
            x=local_x; y=local_y;
        }
#endif
    }
    double p=1.0, tx=0.0, ty=0.0;
    // gdk_event_get_axis renvoie FALSE quand l'axe est absent : sans ce test,
    // la variable garderait sa valeur initiale sans qu'on sache si elle vient
    // du matériel ou du défaut.
    const bool has_p  = pen && gdk_event_get_axis(e, GDK_AXIS_PRESSURE, &p);
    const bool has_tx = gdk_event_get_axis(e, GDK_AXIS_XTILT, &tx);
    const bool has_ty = gdk_event_get_axis(e, GDK_AXIS_YTILT, &ty);
    if(!has_p)  p = 1.0;                  // souris, ou stylet sans pression
    if(!has_tx) tx = 0.0;
    if(!has_ty) ty = 0.0;
    // L'inclinaison arrive soit normalisée (-1..1), soit en degrés selon le
    // backend. On ramène aux deux cas plutôt que de parier sur l'unité : la
    // XP-Pen rapporte des degrés côté libinput (relevés à 10, 15, 16…), mais
    // GDK peut les avoir déjà normalisés.
    auto norm_tilt = [](double v){
        if(!std::isfinite(v)) return 0.0;
        if(std::abs(v) > 1.5) v /= 60.0;  // degrés → -1..1 (±60° en butée)
        return std::clamp(v, -1.0, 1.0);
    };
    tx = norm_tilt(tx); ty = norm_tilt(ty);
    if(p > 1.0) p /= 65535.0;
    const double p_raw = std::clamp(std::isfinite(p) ? p : 1.0, 0.0, 1.0);
    const auto ev_type = gdk_event_get_event_type(e);
    // Amorcer le filtre sur la première pression du trait, avant tout lissage :
    // sinon le trait hérite de l'état laissé par le précédent.
    if(ev_type == GDK_BUTTON_PRESS) reset_pressure_filter(p_raw);
    p = smooth_stylus_pressure(p_raw);
    switch(ev_type) {
    case GDK_BUTTON_PRESS:
        in_stroke=true; slx=x; sly=y;
        undo_begin();
        stroke_pts.clear(); tan_valid = false;
        stroke_pts.push_back({x, y, p});
        { std::size_t cx,cy; if(w2c(w,x,y,cx,cy)) dab_at(cx,cy,p,tx,ty); }
        refresh_view(); return TRUE;
    case GDK_MOTION_NOTIFY:
        if(in_stroke) {
            stroke_pts.push_back({x, y, p});
            flush_latest_segment(w);
            // Seuls les 3 derniers points servent de points de contrôle :
            // inutile de laisser la file grossir pendant tout le trait.
            while(stroke_pts.size() > 4) stroke_pts.pop_front();
            slx=x; sly=y; refresh_view();
        }
        return TRUE;
    case GDK_BUTTON_RELEASE:
        // Rien à émettre ici : chaque segment a déjà été tracé à l'arrivée de
        // son point. Flusher à nouveau repeindrait le dernier segment.
        if(in_stroke) undo_commit();
        stroke_pts.clear(); tan_valid = false;
        in_stroke=false; refresh_view(); return TRUE;
    default: return TRUE;
    }
}

// ── CSS GTK4-strict : shorthands splittés, font-family retiré ────────────────
// ── Thème « Observatoire » ───────────────────────────────────────────────────
// Atlas est un atlas d'univers : bleu nuit profond, lignes fines comme sur une
// carte stellaire, cyan pour ce qui est actif, or pâle pour la marque et les
// titres. Les couleurs restent volontairement peu saturées autour du canvas,
// pour ne pas fausser la perception des couleurs peintes.
//   fond        #070b16   panneaux  #0a1020   lignes   #16233d
//   texte       #d3dbea   atténué   #62739a
//   cyan actif  #56c8dc   or        #d9b86c
static const char CSS[] =
    "window{background-color:#070b16;color:#d3dbea;font-size:13px;}"
    ".topbar{background-color:#0a1122;"
    "border-bottom-width:1px;border-bottom-style:solid;border-bottom-color:#16233d;"
    "padding-top:6px;padding-bottom:6px;padding-left:14px;padding-right:14px;}"
    ".brand{font-size:15px;font-weight:bold;color:#d9b86c;letter-spacing:5px;}"
    ".ver{font-size:10px;color:#62739a;margin-left:6px;letter-spacing:1px;}"
    ".left-panel{background-color:#0a1020;"
    "border-right-width:1px;border-right-style:solid;border-right-color:#16233d;"
    "padding-top:8px;padding-bottom:8px;padding-left:6px;padding-right:6px;min-width:190px;}"
    ".right-panel{background-color:#0a1020;"
    "border-left-width:1px;border-left-style:solid;border-left-color:#16233d;"
    "padding-top:8px;padding-bottom:8px;padding-left:6px;padding-right:6px;min-width:200px;}"
    ".section-head{font-size:9px;font-weight:bold;color:#b39659;letter-spacing:3px;"
    "margin-top:8px;margin-bottom:4px;margin-left:2px;margin-right:0;}"
    "button{background-color:#0f1a31;color:#c8d3e8;"
    "border-width:1px;border-style:solid;border-color:#1b2b4a;"
    "border-radius:5px;"
    "padding-top:5px;padding-bottom:5px;padding-left:8px;padding-right:8px;}"
    "button:hover{background-color:#142443;border-color:#2a4570;}"
    "button.active{background-color:#0c3340;color:#c9f5ff;border-color:#56c8dc;}"
    "button.primary{background-color:#0c3340;color:#c9f5ff;border-color:#56c8dc;}"
    "button.primary:hover{background-color:#11485a;}"
    "button.tool-btn{padding-top:6px;padding-bottom:6px;padding-left:6px;padding-right:6px;min-width:32px;border-radius:6px;}"
    "button.tool-btn.active{background-color:#0c3340;border-color:#56c8dc;}"
    "button.layer-name-btn{border-radius:4px;padding-top:4px;padding-bottom:4px;padding-left:6px;padding-right:6px;font-size:12px;}"
    "button.eye-btn{padding-top:3px;padding-bottom:3px;padding-left:3px;padding-right:3px;font-size:14px;}"
    "button.preset-btn{padding-top:4px;padding-bottom:4px;padding-left:6px;padding-right:6px;margin-top:2px;margin-bottom:2px;font-size:11px;}"
    ".blend-dd{font-size:10px;padding-top:0;padding-bottom:0;padding-left:0;padding-right:0;}"
    "scale{padding-top:8px;padding-bottom:8px;padding-left:4px;padding-right:4px;}"
    "scale trough{background-color:#15213a;border-radius:3px;min-height:4px;min-width:4px;}"
    "scale highlight{background-color:#56c8dc;border-radius:3px;min-height:4px;}"
    "scale slider{background-color:#d9b86c;border-radius:8px;}"
    ".slider-label{color:#62739a;font-size:11px;min-width:65px;}"
    ".color-wheel{margin-top:4px;margin-bottom:4px;}"
    ".color-row{margin-left:4px;margin-bottom:6px;}"
    ".color-hex{font-family:monospace;font-size:12px;color:#c8d3e8;letter-spacing:1px;}"
    ".statusbar{background-color:#050810;"
    "border-top-width:1px;border-top-style:solid;border-top-color:#111b30;"
    "padding-top:4px;padding-bottom:4px;padding-left:12px;padding-right:12px;"
    "font-size:11px;color:#62739a;letter-spacing:1px;}"
    "menubar{background-color:#0a1020;"
    "border-bottom-width:1px;border-bottom-style:solid;border-bottom-color:#16233d;}"
    "menubar button{padding-top:4px;padding-bottom:4px;padding-left:10px;padding-right:10px;"
    "border-width:0;border-style:none;border-radius:0;background-color:transparent;}"
    "menubar button:hover{background-color:#142443;}"
    "popover{background-color:#0c1426;"
    "border-width:1px;border-style:solid;border-color:#1f3354;border-radius:6px;}"
    "popover button{padding-top:6px;padding-bottom:6px;padding-left:14px;padding-right:14px;"
    "border-width:0;border-style:none;border-radius:0;background-color:transparent;}"
    "popover button:hover{background-color:#0c3340;color:#c9f5ff;}"
    "entry{background-color:#0f1a31;color:#d3dbea;border-color:#1b2b4a;}"
    "separator{background-color:#16233d;}";

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
      g_menu_append(m,"Mémoire et scratch disk…","win.mem-prefs");
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
        {"mem-prefs",   G_ACTION_CALLBACK(action_mem_prefs), nullptr,nullptr,nullptr},
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
    { auto* v = gtk_label_new("ATLAS DE L'UNIVERS · v4"); gtk_widget_add_css_class(v,"ver");   gtk_box_append(GTK_BOX(topbar),v); }
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
        gtk_widget_add_css_class(tb, "tool-btn");
        if(TDEFS[ti].t == active_tool) gtk_widget_add_css_class(tb, "active");
        g_tool_btns[ti] = tb;
        g_signal_connect(tb, "clicked", (GCallback)(+[](GtkButton*, gpointer u){
            active_tool = static_cast<Tool>(GPOINTER_TO_INT(u));
            // Mettre à jour l'accent visuel sur tous les boutons d'outils
            for(int k = 0; k < 10; ++k) {
                if(!g_tool_btns[k]) continue;
                if(k == (int)active_tool)
                    gtk_widget_add_css_class(g_tool_btns[k], "active");
                else
                    gtk_widget_remove_css_class(g_tool_btns[k], "active");
            }
            if(canvas_w) gtk_widget_queue_draw(canvas_w);
        }), GINT_TO_POINTER((int)TDEFS[ti].t));
        gtk_grid_attach(GTK_GRID(tgrid), tb, ti%2, ti/2, 1, 1);
    }
    gtk_box_append(GTK_BOX(left), tgrid);

    // Colour picker
    { auto* l = gtk_label_new("COULEUR"); gtk_widget_add_css_class(l,"section-head"); gtk_box_append(GTK_BOX(left),l); }
    // Roue anneau + triangle (ui/gtk/color_wheel.*) : teinte sur l'anneau,
    // saturation et valeur dans le triangle.
    g_wheel = creative::ui::ColorWheel::create();
    g_wheel->set_rgba(brush_col);
    g_wheel->set_on_change([](const GdkRGBA& c){
        brush_col = c;
        // La roue est déjà à jour : on ne rafraîchit que l'affichage texte.
        auto* w = g_wheel; g_wheel = nullptr; sync_color_ui(); g_wheel = w;
    });
    g_signal_connect(g_wheel->widget(), "destroy", (GCallback)(+[](GtkWidget*, gpointer){
        g_wheel = nullptr;
    }), nullptr);
    gtk_box_append(GTK_BOX(left), g_wheel->widget());

    // Pastille de la couleur courante + code hexadécimal.
    { auto* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
      gtk_widget_add_css_class(row, "color-row");
      g_col_swatch = gtk_drawing_area_new();
      gtk_widget_set_size_request(g_col_swatch, 34, 20);
      gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(g_col_swatch),
          [](GtkDrawingArea*, cairo_t* cr, int w, int h, gpointer){
              cairo_set_source_rgb(cr, brush_col.red, brush_col.green, brush_col.blue);
              cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
              cairo_fill_preserve(cr);
              cairo_set_source_rgba(cr, 0.85, 0.72, 0.42, 0.7);   // liseré or
              cairo_set_line_width(cr, 1.0);
              cairo_stroke(cr);
          }, nullptr, nullptr);
      g_signal_connect(g_col_swatch, "destroy", (GCallback)(+[](GtkWidget*, gpointer){
          g_col_swatch = nullptr; }), nullptr);
      g_col_hex = gtk_label_new("");
      gtk_widget_add_css_class(g_col_hex, "color-hex");
      gtk_label_set_selectable(GTK_LABEL(g_col_hex), TRUE);
      g_signal_connect(g_col_hex, "destroy", (GCallback)(+[](GtkWidget*, gpointer){
          g_col_hex = nullptr; }), nullptr);
      gtk_box_append(GTK_BOX(row), g_col_swatch);
      gtk_box_append(GTK_BOX(row), g_col_hex);
      gtk_box_append(GTK_BOX(left), row);
      sync_color_ui(); }

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
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drag), GTK_PHASE_BUBBLE);
    g_signal_connect(drag,"drag-begin", G_CALLBACK(drag_begin), cda);
    g_signal_connect(drag,"drag-update",G_CALLBACK(drag_update),cda);
    g_signal_connect(drag,"drag-end",   G_CALLBACK(drag_end),   cda);
    gtk_widget_add_controller(GTK_WIDGET(cda), GTK_EVENT_CONTROLLER(drag));

    // Motion (cursor)
    auto* mot = gtk_event_controller_motion_new();
    g_signal_connect(mot,"motion",(GCallback)(+[](GtkEventControllerMotion*,double x,double y,gpointer data){
        cur_wx=x; cur_wy=y; cur_in=true;
        // Le dessin (souris et stylet) est géré entièrement par raw_tablet_event
        // (GTK_PHASE_CAPTURE) : ne pas redessiner ici pour éviter le doublon.
        if(canvas_w) gtk_widget_queue_draw(canvas_w);
        if(coord_lbl && canvas_w){
            std::size_t cx=0,cy=0;
            if(w2c(canvas_w,x,y,cx,cy)){
                char buf[64]; std::snprintf(buf,64,"  %zu, %zu px",cx,cy);
                gtk_label_set_text(coord_lbl,buf);
            }
        }
    }),cda);
    g_signal_connect(mot,"leave",(GCallback)(+[](GtkEventControllerMotion*,gpointer){
        cur_in=false; if(canvas_w)gtk_widget_queue_draw(canvas_w);
    }),nullptr);
    gtk_widget_add_controller(GTK_WIDGET(cda), GTK_EVENT_CONTROLLER(mot));

    // Le stylet est traité uniquement par le contrôleur brut ci-dessous.
    auto* raw_tablet = gtk_event_controller_legacy_new();
    gtk_event_controller_set_propagation_phase(raw_tablet, GTK_PHASE_CAPTURE);
    g_signal_connect(raw_tablet, "event", G_CALLBACK(raw_tablet_event), cda);
    gtk_widget_add_controller(GTK_WIDGET(cda), raw_tablet);

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
    // Le libellé part sur l'intention ; il est corrigé au « realize » selon le
    // résultat réel de l'init EGL, au lieu de mentir si l'init échoue.
    g_render_mode_lbl = gtk_label_new(gpu_off() ? "Mode CPU" : "Mode GPU…");
    gtk_box_append(GTK_BOX(sb), g_render_mode_lbl);
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
                    set_render_mode_label("Mode CPU (GPU indisponible)");
                    std::fprintf(stderr, "Atlas v4: GPU init échoué après realize → CPU\n");
                } else {
                    native_surf->resize(gtk_widget_get_width(w), gtk_widget_get_height(w));
                    native_surf->sync_layout();
                    set_render_mode_label("Mode GPU");
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
        "window{background-color:#070b16;color:#d3dbea;}"
        ".brand{font-size:42px;font-weight:bold;color:#d9b86c;letter-spacing:14px;}"
        ".sub{font-size:12px;color:#62739a;margin-bottom:28px;letter-spacing:4px;}"
        "button{background-color:#0f1a31;color:#c8d3e8;"
        "border-width:1px;border-style:solid;border-color:#1b2b4a;"
        "border-radius:8px;"
        "padding-top:10px;padding-bottom:10px;padding-left:20px;padding-right:20px;}"
        "button:hover{background-color:#142443;}"
        "button.primary{background-color:#0c3340;color:#c9f5ff;border-color:#56c8dc;}"
        "button.primary:hover{background-color:#56c8dc;}");
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
    { auto* s = gtk_label_new("ATLAS DE L'UNIVERS CRÉATIF  ·  v4");
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
