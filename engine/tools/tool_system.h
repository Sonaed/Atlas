#pragma once
#include "engine/brush/brush_engine.h"
#include "engine/layers/layer_stack.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
namespace creative::engine {
struct ToolEvent { double x=0,y=0,pressure=1,tilt_x=0,tilt_y=0; };
struct ToolParam { std::string name; float value=0,minimum=0,maximum=1; };
class ITool { public: virtual ~ITool()=default; virtual std::string name()const=0; virtual void on_press(const ToolEvent&)=0; virtual void on_move(const ToolEvent&)=0; virtual void on_release(const ToolEvent&)=0; virtual void on_tablet_event(const ToolEvent& e){on_move(e);} };
class BrushTool final: public ITool { public: explicit BrushTool(BrushEngine& b):brush_(b){}std::string name()const override{return "Brush";}void on_press(const ToolEvent& e)override{brush_.stroke({{e.x,e.y,e.pressure,e.tilt_x,e.tilt_y,0}});}void on_move(const ToolEvent& e)override{brush_.stroke({{e.x,e.y,e.pressure,e.tilt_x,e.tilt_y,0}});}void on_release(const ToolEvent&)override{}private:BrushEngine& brush_;};
class ToolRegistry { public: void register_tool(std::shared_ptr<ITool> t){if(t)tools_[t->name()]=std::move(t);}void unregister_tool(const std::string& n){tools_.erase(n);}std::shared_ptr<ITool> find(const std::string& n)const{auto i=tools_.find(n);return i==tools_.end()?nullptr:i->second;}std::size_t size()const{return tools_.size();}private:std::unordered_map<std::string,std::shared_ptr<ITool>> tools_;};
class ToolCreator { public: bool validate_script(const std::string& source,std::string& error)const; std::shared_ptr<ITool> create_script_tool(const std::string& name,const std::string& source,std::string& error)const; bool execute_python(const std::string& source,std::string& error)const; bool reload_python(const std::string& path,std::string& error)const; };
}
