#pragma once
#include <gtk/gtk.h>
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
namespace creative::ui {
class IPanel { public: virtual ~IPanel()=default; virtual std::string id()const=0; virtual GtkWidget* widget()=0; };
class BasicPanel final: public IPanel { public: BasicPanel(std::string id,std::string title):id_(std::move(id)),widget_(gtk_frame_new(title.c_str())){}std::string id()const override{return id_;}GtkWidget* widget()override{return widget_;}private:std::string id_;GtkWidget* widget_;};
class PanelRegistry { public: void add(std::shared_ptr<IPanel> p){if(p)panels_[p->id()]=std::move(p);}void remove(const std::string& id){panels_.erase(id);}std::shared_ptr<IPanel> find(const std::string& id)const{auto i=panels_.find(id);return i==panels_.end()?nullptr:i->second;}std::vector<std::string> ids()const{std::vector<std::string> r;for(auto&[id,_]:panels_)r.push_back(id);std::sort(r.begin(),r.end());return r;}private:std::unordered_map<std::string,std::shared_ptr<IPanel>> panels_;};
enum class DockPosition { Left, Right, Bottom, Floating };
struct DockEntry { std::string panel_id; DockPosition position; bool visible=true; };
class DockManager { public: void dock(std::string id,DockPosition p){for(auto& e:entries_)if(e.panel_id==id){e.position=p;e.visible=true;return;}entries_.push_back({std::move(id),p,true});}void hide(const std::string& id){for(auto& e:entries_)if(e.panel_id==id)e.visible=false;}void show(const std::string& id){for(auto& e:entries_)if(e.panel_id==id)e.visible=true;}const auto& entries()const{return entries_;}std::string serialize()const{std::string out;for(auto& e:entries_)out+=e.panel_id+":"+std::to_string(static_cast<int>(e.position))+":"+(e.visible?"1":"0")+"\n";return out;}void restore(const std::string& text){entries_.clear();std::size_t start=0;while(start<text.size()){auto end=text.find('\n',start);auto line=text.substr(start,end==std::string::npos?end-start:end-start);auto a=line.find(':');auto b=line.find(':',a+1);if(a!=std::string::npos&&b!=std::string::npos)entries_.push_back({line.substr(0,a),static_cast<DockPosition>(std::stoi(line.substr(a+1,b-a-1))),line.substr(b+1)=="1"});if(end==std::string::npos)break;start=end+1;}}private:std::vector<DockEntry> entries_;};
class LayoutPresets { public: static DockManager painting(){DockManager d;d.dock("tools",DockPosition::Left);d.dock("brushes",DockPosition::Right);d.dock("layers",DockPosition::Right);return d;}static DockManager illustration(){DockManager d;d.dock("tools",DockPosition::Left);d.dock("colors",DockPosition::Right);d.dock("layers",DockPosition::Right);return d;}static DockManager composition(){DockManager d;d.dock("layers",DockPosition::Right);d.dock("history",DockPosition::Bottom);return d;}};
}
