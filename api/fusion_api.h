#pragma once
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace creative::api {
struct FusionMode {
    std::string id;
    std::string name;
    std::function<void(const float*, const float*, float*, std::size_t)> cpu_kernel;
    std::string gpu_shader;
    std::unordered_map<std::string, float> parameters;
};
struct FusionParameter { std::string name; float value=0; float minimum=0; float maximum=1; };
}
