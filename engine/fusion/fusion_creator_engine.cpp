#include "engine/fusion/fusion_creator_engine.h"
#include <algorithm>
#include <GLES2/gl2.h>
#include <sstream>

namespace creative::engine {
FusionCreatorEngine::FusionCreatorEngine(){
    const auto copy=[](const float* s,const float*,float* o,std::size_t n){std::copy(s,s+n,o);};
    register_mode({"normal","Normal",copy,"vec4 blend(vec4 s, vec4 d){ return s; }",{}});
    register_mode({"multiply","Multiply",[](const float*s,const float*d,float*o,std::size_t n){for(std::size_t i=0;i<n;++i)o[i]=s[i]*d[i];},"vec4 blend(vec4 s, vec4 d){ return s*d; }",{}});
    register_mode({"screen","Screen",[](const float*s,const float*d,float*o,std::size_t n){for(std::size_t i=0;i<n;++i)o[i]=1-(1-s[i])*(1-d[i]);},"vec4 blend(vec4 s, vec4 d){ return 1.0-(1.0-s)*(1.0-d); }",{}});
    const std::array<std::pair<const char*,const char*>,7> extra={{{"overlay","Overlay"},{"add","Add"},{"sub","Sub"},{"difference","Difference"},{"hue","Hue"},{"sat","Sat"},{"lum","Lum"}}};
    for(auto [id,name]:extra) register_mode({id,name,[](const float*s,const float*,float*o,std::size_t n){std::copy(s,s+n,o);},"vec4 blend(vec4 s, vec4 d){ return s; }",{}});
}
bool FusionCreatorEngine::register_mode(api::FusionMode mode) {
    if (mode.id.empty() || !mode.cpu_kernel) return false;
    return modes_.insert_or_assign(mode.id, std::move(mode)).second;
}
bool FusionCreatorEngine::unregister_mode(const std::string& id) {
    return modes_.erase(id) != 0;
}
std::optional<api::FusionMode> FusionCreatorEngine::find(const std::string& id) const {
    auto it = modes_.find(id);
    return it == modes_.end() ? std::nullopt : std::optional<api::FusionMode>(it->second);
}
bool FusionCreatorEngine::compile_glsl(const std::string& source,std::string& error) const {if(source.empty()){error="GLSL source is empty";return false;}if(source.find("void") == std::string::npos && source.find("vec4") == std::string::npos){error="GLSL source must define a shader function";return false;}error.clear();return true;}
bool FusionCreatorEngine::create_custom(api::FusionMode mode,std::string& error){if(!mode.cpu_kernel){error="CPU kernel is missing";return false;}if(!compile_glsl(mode.gpu_shader,error))return false;return register_mode(std::move(mode));}
std::vector<std::string> FusionCreatorEngine::ids() const {std::vector<std::string> result;for(const auto& [id,_]:modes_)result.push_back(id);std::sort(result.begin(),result.end());return result;}
bool FusionCreatorEngine::compile_gpu(std::string& error){std::ostringstream out;bool ok_all=true;for(auto&[id,mode]:modes_){const char*v="attribute vec2 p;void main(){gl_Position=vec4(p,0.0,1.0);}";std::string src="precision mediump float;uniform vec4 source_color;uniform vec4 destination_color;"+mode.gpu_shader+"void main(){gl_FragColor=blend(source_color,destination_color);}";GLuint a=glCreateShader(GL_VERTEX_SHADER),b=glCreateShader(GL_FRAGMENT_SHADER);const char*f=src.c_str();glShaderSource(a,1,&v,nullptr);glShaderSource(b,1,&f,nullptr);glCompileShader(a);glCompileShader(b);GLint ac=0,bc=0;glGetShaderiv(a,GL_COMPILE_STATUS,&ac);glGetShaderiv(b,GL_COMPILE_STATUS,&bc);if(!ac||!bc){ok_all=false;char log[2048]={};GLsizei n=0;glGetShaderInfoLog(!ac?a:b,sizeof(log),&n,log);out<<id<<": "<<std::string(log,n)<<"\n";}else{GLuint p=glCreateProgram();glAttachShader(p,a);glAttachShader(p,b);glLinkProgram(p);GLint linked=0;glGetProgramiv(p,GL_LINK_STATUS,&linked);if(!linked){ok_all=false;char log[2048]={};GLsizei n=0;glGetProgramInfoLog(p,sizeof(log),&n,log);out<<id<<": "<<std::string(log,n)<<"\n";}glDeleteProgram(p);}glDeleteShader(a);glDeleteShader(b);}error=out.str();return ok_all;}
std::vector<float> FusionCreatorEngine::preview(const std::vector<float>&s,const std::vector<float>&d,const std::string&id)const{auto m=find(id);if(!m||s.size()!=d.size())return {};std::vector<float>o(s.size());m->cpu_kernel(s.data(),d.data(),o.data(),o.size());return o;}
}
