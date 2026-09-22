#pragma once
#include "engine/brush/brush_engine.h"
#include "engine/optimization/resource_manager.h"
#include <string>
#include <vector>
namespace creative::engine { class BitmapBrush final: public IBrush { public: std::string name()const override{return "Bitmap";}bool load(const std::string& path);void stamp(TileStore&,const BrushSample&,double,double,std::uint32_t,BrushMode) override;void upload_gpu(GpuTextureCache& cache,std::size_t key)const{cache.upload(key,pixels_);}std::size_t width()const{return width_;}std::size_t height()const{return height_;}const std::vector<std::byte>& gpu_pixels()const{return pixels_;}private:std::size_t width_=0,height_=0;std::vector<std::byte> pixels_;}; }
