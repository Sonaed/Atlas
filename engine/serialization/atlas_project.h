#pragma once
#include "engine/projection/projection_engine.h"
#include <cstdint>
#include <string>
#include <vector>
namespace creative::engine {
struct AtlasLayerRecord {
    std::string name;
    float opacity = 1.0f;
    bool visible  = true;
    std::string blend = "normal";
    std::vector<std::byte> pixels;
    std::vector<std::byte> mask;
};
struct AtlasProject {
    std::string name    = "Untitled";
    std::string author;
    std::string version = "0.1";
    std::uint32_t width  = 0;
    std::uint32_t height = 0;
    std::uint32_t dpi    = 72;   // NEW in format v3
    std::string background = "transparent"; // "white" | "black" | "transparent"
    Projection projection;
    std::vector<AtlasLayerRecord> layers;
};
class AtlasSerializer {
public:
    static bool save(const std::string&, const AtlasProject&);
    static bool load(const std::string&, AtlasProject&);
    static bool export_png(const std::string&, const AtlasProject&);
    static bool export_tiff(const std::string&, const AtlasProject&);
};
}
