#include "engine/tools/tool_system.h"
#include <Python.h>
#include <fstream>
namespace creative::engine {
bool ToolCreator::validate_script(const std::string& s,std::string& e)const{if(s.empty()){e="tool script is empty";return false;}if(s.find("on_press")==std::string::npos&&s.find("on_move")==std::string::npos){e="tool script needs an event handler";return false;}e.clear();return true;}
std::shared_ptr<ITool> ToolCreator::create_script_tool(const std::string& n,const std::string& s,std::string& e)const{if(n.empty()){e="tool name is empty";return {}; }if(!validate_script(s,e))return {};class ScriptTool final:public ITool{std::string n_;public:explicit ScriptTool(std::string n):n_(std::move(n)){}std::string name()const override{return n_;}void on_press(const ToolEvent&)override{}void on_move(const ToolEvent&)override{}void on_release(const ToolEvent&)override{}};return std::make_shared<ScriptTool>(n);}
bool ToolCreator::execute_python(const std::string& source,std::string& error)const{bool owned=!Py_IsInitialized();if(owned)Py_Initialize();int result=PyRun_SimpleStringFlags(source.c_str(),nullptr);if(result!=0){PyErr_Print();error="Python tool execution failed";}else error.clear();if(owned)Py_Finalize();return result==0;}
bool ToolCreator::reload_python(const std::string& path,std::string& error)const{std::ifstream f(path);if(!f){error="cannot open Python tool";return false;}return execute_python(std::string((std::istreambuf_iterator<char>(f)),{}),error);}
}
