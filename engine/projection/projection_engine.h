#pragma once
#include <cmath>
#include <algorithm>
#include <array>
namespace creative::engine {
struct Projection { double zoom=1.0, pan_x=0.0, pan_y=0.0, rotation=0.0; };
struct ProjectionEvent { enum class Type { Wheel, Pan, Rotate, Reset }; Type type; double a=0,b=0,c=0; };
class ProjectionEngine {
public:
    void zoom(double factor, double cx, double cy) { if (factor<=0) return; const double next=std::clamp(zoom_*factor,min_zoom_,max_zoom_); const double actual=next/zoom_; pan_x_ = cx-(cx-pan_x_)*actual; pan_y_ = cy-(cy-pan_y_)*actual; zoom_=next; }
    void pan(double dx,double dy) { pan_x_+=dx; pan_y_+=dy; }
    void rotate(double radians) { rotation_ += radians; }
    Projection state() const { return {zoom_,pan_x_,pan_y_,rotation_}; }
    void reset() { zoom_=1; pan_x_=pan_y_=rotation_=0; }
    void set_zoom_limits(double min_zoom,double max_zoom){min_zoom_=std::max(.0001,min_zoom);max_zoom_=std::max(min_zoom_,max_zoom);zoom_=std::clamp(zoom_,min_zoom_,max_zoom_);}
    void handle(const ProjectionEvent& e){switch(e.type){case ProjectionEvent::Type::Wheel:zoom(std::exp(e.a*.1),e.b,e.c);break;case ProjectionEvent::Type::Pan:pan(e.a,e.b);break;case ProjectionEvent::Type::Rotate:rotate(e.a);break;case ProjectionEvent::Type::Reset:reset();break;}}
    void view_to_document(double x,double y,double& ox,double& oy) const {const double c=std::cos(rotation_),s=std::sin(rotation_);const double dx=(x-pan_x_)/zoom_,dy=(y-pan_y_)/zoom_;ox=c*dx+s*dy;oy=-s*dx+c*dy;}
    std::array<float,16> mvp(double width,double height) const {const double c=std::cos(rotation_),s=std::sin(rotation_);const double sx=2*zoom_/width,sy=2*zoom_/height;return {float(c*sx),float(s*sy),0,0,float(-s*sx),float(c*sy),0,0,0,0,1,0,float(2*pan_x_/width-1),float(2*pan_y_/height-1),0,1};}
    void document_to_view(double x,double y,double& ox,double& oy) const { const auto c=std::cos(rotation_),s=std::sin(rotation_); ox=pan_x_+zoom_*(c*x-s*y); oy=pan_y_+zoom_*(s*x+c*y); }
private: double zoom_=1,pan_x_=0,pan_y_=0,rotation_=0,min_zoom_=.05,max_zoom_=64;
};
}
