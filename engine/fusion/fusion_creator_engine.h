#pragma once
#include "api/fusion_api.h"
#include <optional>
#include <unordered_map>
#include <vector>
#include <array>

namespace creative::engine {
class FusionCreatorEngine {
public:
    FusionCreatorEngine();
    bool register_mode(api::FusionMode mode);
    bool unregister_mode(const std::string& id);
    std::optional<api::FusionMode> find(const std::string& id) const;
    std::size_t size() const noexcept { return modes_.size(); }
    bool compile_glsl(const std::string& source, std::string& error) const;
    bool create_custom(api::FusionMode mode, std::string& error);
    std::vector<std::string> ids() const;
    bool compile_gpu(std::string& error);
    std::vector<float> preview(const std::vector<float>&, const std::vector<float>&, const std::string&) const;
private:
    std::unordered_map<std::string, api::FusionMode> modes_;
};
}
