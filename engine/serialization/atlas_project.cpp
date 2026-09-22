#include "engine/serialization/atlas_project.h"
#include <fstream>
#include <png.h>
#include <zlib.h>
#include <tiffio.h>
namespace creative::engine {
namespace {
void str(std::ostream& o, const std::string& s) {
    std::uint32_t n = s.size();
    o.write(reinterpret_cast<const char*>(&n), 4);
    o.write(s.data(), n);
}
bool read_str(std::istream& i, std::string& s) {
    std::uint32_t n = 0;
    if (!i.read(reinterpret_cast<char*>(&n), 4) || n > (1u << 20)) return false;
    s.resize(n);
    return bool(i.read(s.data(), n));
}
void blob(std::ostream& o, const std::vector<std::byte>& in) {
    uLongf n = compressBound(in.size());
    std::vector<std::byte> c(n);
    compress(reinterpret_cast<Bytef*>(c.data()), &n,
             reinterpret_cast<const Bytef*>(in.data()), in.size());
    std::uint32_t raw = in.size(), packed = n;
    o.write(reinterpret_cast<char*>(&raw), 4);
    o.write(reinterpret_cast<char*>(&packed), 4);
    o.write(reinterpret_cast<char*>(c.data()), n);
}
bool unblob(std::istream& i, std::vector<std::byte>& out) {
    std::uint32_t raw = 0, n = 0;
    if (!i.read(reinterpret_cast<char*>(&raw), 4) || !i.read(reinterpret_cast<char*>(&n), 4) ||
        raw > (1u << 30) || n > (1u << 30)) return false;
    std::vector<std::byte> c(n);
    if (!i.read(reinterpret_cast<char*>(c.data()), n)) return false;
    out.resize(raw);
    uLongf size = raw;
    return uncompress(reinterpret_cast<Bytef*>(out.data()), &size,
                      reinterpret_cast<const Bytef*>(c.data()), n) == Z_OK;
}
} // namespace

// Format v2: original (no DPI)
// Format v3: adds dpi (uint32) and background string after width/height
bool AtlasSerializer::save(const std::string& path, const AtlasProject& p) {
    std::ofstream o(path, std::ios::binary);
    if (!o) return false;
    o.write("ATLS", 4);
    std::uint32_t format = 3;
    o.write(reinterpret_cast<char*>(&format), 4);
    str(o, p.name);
    str(o, p.author);
    str(o, p.version);
    o.write(reinterpret_cast<const char*>(&p.width),  4);
    o.write(reinterpret_cast<const char*>(&p.height), 4);
    o.write(reinterpret_cast<const char*>(&p.dpi),    4); // v3
    str(o, p.background);                                  // v3
    o.write(reinterpret_cast<const char*>(&p.projection), sizeof(Projection));
    std::uint32_t count = p.layers.size();
    o.write(reinterpret_cast<char*>(&count), 4);
    for (const auto& l : p.layers) {
        str(o, l.name);
        o.write(reinterpret_cast<const char*>(&l.opacity), 4);
        o.write(reinterpret_cast<const char*>(&l.visible), 1);
        str(o, l.blend);
        blob(o, l.pixels);
        blob(o, l.mask);
    }
    return bool(o);
}

bool AtlasSerializer::load(const std::string& path, AtlasProject& p) {
    std::ifstream i(path, std::ios::binary);
    char magic[4];
    std::uint32_t f = 0, count = 0;
    if (!i || !i.read(magic, 4) || std::string(magic, 4) != "ATLS" ||
        !i.read(reinterpret_cast<char*>(&f), 4) || (f != 2 && f != 3)) return false;
    if (!read_str(i, p.name) || !read_str(i, p.author) || !read_str(i, p.version) ||
        !i.read(reinterpret_cast<char*>(&p.width),  4) ||
        !i.read(reinterpret_cast<char*>(&p.height), 4)) return false;
    if (f >= 3) {
        if (!i.read(reinterpret_cast<char*>(&p.dpi), 4)) return false;
        if (!read_str(i, p.background)) return false;
    }
    if (!i.read(reinterpret_cast<char*>(&p.projection), sizeof(Projection)) ||
        !i.read(reinterpret_cast<char*>(&count), 4) || count > 100000) return false;
    p.layers.clear();
    for (std::uint32_t n = 0; n < count; ++n) {
        AtlasLayerRecord l;
        if (!read_str(i, l.name) ||
            !i.read(reinterpret_cast<char*>(&l.opacity), 4) ||
            !i.read(reinterpret_cast<char*>(&l.visible), 1) ||
            !read_str(i, l.blend) ||
            !unblob(i, l.pixels) ||
            !unblob(i, l.mask)) return false;
        p.layers.push_back(std::move(l));
    }
    return true;
}

bool AtlasSerializer::export_png(const std::string& path, const AtlasProject& p) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    png_structp w = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(w);
    if (!w || !info || setjmp(png_jmpbuf(w))) { fclose(f); return false; }
    png_init_io(w, f);
    png_set_IHDR(w, info, p.width, p.height, 8, PNG_COLOR_TYPE_RGBA,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    // Embed DPI metadata (metres: dpi / 0.0254)
    png_uint_32 dpm = static_cast<png_uint_32>(p.dpi / 0.0254 + 0.5);
    png_set_pHYs(w, info, dpm, dpm, PNG_RESOLUTION_METER);
    png_write_info(w, info);
    // Flatten all visible layers
    std::vector<std::byte> pixels(p.width * p.height * 4, std::byte{0});
    for (const auto& l : p.layers) {
        if (!l.visible || l.pixels.size() != pixels.size()) continue;
        for (std::size_t j = 0; j < pixels.size(); j += 4) {
            const unsigned sa = std::to_integer<unsigned>(l.pixels[j + 3]);
            if (!sa) continue;
            const unsigned da = std::to_integer<unsigned>(pixels[j + 3]);
            const unsigned inv = 255u - sa;
            for (int c = 0; c < 3; ++c)
                pixels[j + c] = std::byte((std::to_integer<unsigned>(l.pixels[j+c]) * sa +
                                           std::to_integer<unsigned>(pixels[j+c]) * inv + 127u) / 255u);
            pixels[j + 3] = std::byte(std::min(255u, sa + da * inv / 255u));
        }
    }
    std::vector<png_bytep> rows(p.height);
    for (std::size_t y = 0; y < p.height; ++y)
        rows[y] = reinterpret_cast<png_bytep>(pixels.data() + y * p.width * 4);
    png_write_image(w, rows.data());
    png_write_end(w, nullptr);
    png_destroy_write_struct(&w, &info);
    fclose(f);
    return true;
}

bool AtlasSerializer::export_tiff(const std::string& path, const AtlasProject& p) {
    TIFF* t = TIFFOpen(path.c_str(), "w");
    if (!t) return false;
    std::vector<std::byte> pixels(p.width * p.height * 4, std::byte{0});
    for (const auto& l : p.layers) {
        if (!l.visible || l.pixels.size() != pixels.size()) continue;
        for (std::size_t j = 0; j < pixels.size(); j += 4) {
            const unsigned sa = std::to_integer<unsigned>(l.pixels[j + 3]);
            if (!sa) continue;
            const unsigned da = std::to_integer<unsigned>(pixels[j + 3]);
            const unsigned inv = 255u - sa;
            for (int c = 0; c < 3; ++c)
                pixels[j + c] = std::byte((std::to_integer<unsigned>(l.pixels[j+c]) * sa +
                                           std::to_integer<unsigned>(pixels[j+c]) * inv + 127u) / 255u);
            pixels[j + 3] = std::byte(std::min(255u, sa + da * inv / 255u));
        }
    }
    TIFFSetField(t, TIFFTAG_IMAGEWIDTH,      p.width);
    TIFFSetField(t, TIFFTAG_IMAGELENGTH,     p.height);
    TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, 4);
    TIFFSetField(t, TIFFTAG_BITSPERSAMPLE,   8);
    TIFFSetField(t, TIFFTAG_ORIENTATION,     ORIENTATION_TOPLEFT);
    TIFFSetField(t, TIFFTAG_PLANARCONFIG,    PLANARCONFIG_CONTIG);
    TIFFSetField(t, TIFFTAG_PHOTOMETRIC,     PHOTOMETRIC_RGB);
    TIFFSetField(t, TIFFTAG_XRESOLUTION,     (double)p.dpi);
    TIFFSetField(t, TIFFTAG_YRESOLUTION,     (double)p.dpi);
    TIFFSetField(t, TIFFTAG_RESOLUTIONUNIT,  RESUNIT_INCH);
    uint16_t extra = EXTRASAMPLE_UNASSALPHA;
    TIFFSetField(t, TIFFTAG_EXTRASAMPLES, 1, &extra);
    for (std::uint32_t y = 0; y < p.height; ++y)
        if (TIFFWriteScanline(t, pixels.data() + y * p.width * 4, y, 0) < 0) {
            TIFFClose(t); return false;
        }
    TIFFClose(t);
    return true;
}
} // namespace creative::engine
