#include "visuals.h"
#include "platform.h"
#include <sha256.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 4096
#include <stb_image.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace launcher {
namespace {
ImTextureID id(SDL_Texture* t) { return static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(t)); }
bool cache_link(const fs::path& p) {
#ifdef _WIN32
  auto attributes=GetFileAttributesW(p.c_str()); return attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT);
#else
  return fs::is_symlink(fs::symlink_status(p));
#endif
}
ImVec2 texture_size(SDL_Texture* t) { float w=0,h=0; SDL_GetTextureSize(t,&w,&h); return {w,h}; }
}
Visuals::Visuals(SDL_Renderer* r,const fs::path& home):renderer_(r),home_(home) {
  auto font=[&](const char* name,float size) { auto file=home_/"assets/fonts"/name; if(!fs::is_regular_file(file)) return static_cast<ImFont*>(nullptr); auto path=utf8(file); return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(),size); };
  body=font("Barlow-Regular.ttf",22); if(!body) body=ImGui::GetIO().Fonts->AddFontDefault();
  heading=font("BarlowCondensed-Bold.ttf",38); if(!heading) heading=body;
  ImGui::GetIO().FontDefault=body;
}
Visuals::~Visuals() { clear_art(); for(const auto& [k,t]:glyphs_) if(t) SDL_DestroyTexture(t); }
SDL_Texture* Visuals::rgba(std::span<const unsigned char> b,int w,int h) {
  if(w<1||h<1||w>4096||h>4096||b.size()!=uint64_t(w)*h*4) throw std::runtime_error("Invalid art dimensions");
  auto surface=SDL_CreateSurfaceFrom(w,h,SDL_PIXELFORMAT_RGBA32,const_cast<unsigned char*>(b.data()),w*4);
  if(!surface) throw std::runtime_error(SDL_GetError()); auto texture=SDL_CreateTextureFromSurface(renderer_,surface); SDL_DestroySurface(surface);
  if(!texture) throw std::runtime_error(SDL_GetError()); SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND); return texture;
}
SDL_Texture* Visuals::decode(std::span<const unsigned char> png,const fs::path& requested_cache,bool clear_black) {
  fs::path cache=requested_cache;
  if(!cache.empty()&&(cache_link(cache)||cache_link(cache.parent_path())||cache_link(cache.parent_path().parent_path()))) cache.clear();
  if(!cache.empty()&&fs::exists(cache)) {
    try { auto raw=read_bytes(cache,64*1024*1024+8); if(raw.size()<8) throw std::runtime_error("Invalid cache");
      auto num=[&](size_t p) { uint32_t v=0; for(int i=0;i<4;++i) v|=uint32_t(raw[p+i])<<(i*8); return v; };
      return rgba(std::span(raw).subspan(8),static_cast<int>(num(0)),static_cast<int>(num(4)));
    } catch(const std::exception&) { /* Rebuild an invalid cache from the verified source. */ }
  }
  int w=0,h=0,n=0;
  std::unique_ptr<unsigned char,decltype(&stbi_image_free)> pixels(stbi_load_from_memory(png.data(),static_cast<int>(png.size()),&w,&h,&n,4),stbi_image_free);
  if(!pixels) throw std::runtime_error("PNG decoding failed"); auto bytes=std::span(pixels.get(),size_t(w)*h*4);
  // The verified menu logo is authored on opaque black. Remove that matte only
  // for its derived UI texture; keep source files and all other textures intact.
  if(clear_black) for(size_t i=0;i<bytes.size();i+=4) {
    int brightness=std::max({bytes[i],bytes[i+1],bytes[i+2]});
    int coverage=std::clamp((brightness-8)*255/16,0,255);
    bytes[i+3]=static_cast<unsigned char>(int(bytes[i+3])*coverage/255);
  }
  auto texture=rgba(bytes,w,h);
  if(!cache.empty()) {
    try { fs::create_directories(cache.parent_path()); Bytes raw(8); for(int i=0;i<4;++i) { raw[i]=static_cast<unsigned char>(uint32_t(w)>>(i*8)); raw[i+4]=static_cast<unsigned char>(uint32_t(h)>>(i*8)); }
      raw.insert(raw.end(),bytes.begin(),bytes.end()); ConfigTemp temporary(cache); temporary.replace(cache,std::string(raw.begin(),raw.end()));
    } catch(const std::exception&) { /* Cache failure does not hide usable art. */ }
  }
  return texture;
}
void Visuals::clear_art() { for(auto* t:{background_,logo_,agency_}) if(t) SDL_DestroyTexture(t); background_=logo_=agency_=nullptr; }
void Visuals::load_art(const fs::path& game,const fs::path& user,const std::vector<Entry>& manifest) {
  clear_art(); art_status.clear();
  try {
    auto row=std::find_if(manifest.begin(),manifest.end(),[](const auto& e){return e.path=="XUIFrontEnd/Scene.xzp";});
    if(row==manifest.end()||row->hash.size()!=64) throw std::runtime_error("Scene.xzp has no trusted manifest hash");
    auto bytes=read_bytes(game/fs::u8path(row->path));
    // Hash the bytes actually decoded, even when the core used its quick check.
    if(bytes.size()!=row->size||sha256::SHA256()(bytes.data(),bytes.size())!=row->hash) throw std::runtime_error("Scene.xzp failed its art hash check");
    auto entries=read_xzp(bytes); auto cache=user.empty()?fs::path{}:user/"launcher-art"/row->hash;
    auto load=[&](const char* name,SDL_Texture*& out,bool clear_black=false) {
      try {
        const Bytes* png=nullptr;
        for(const auto& e:entries) {
          auto separator=e.name.find_last_of("/\\"); auto filename=e.name.substr(separator==std::string::npos?0:separator+1);
          if(filename==name) { if(png) throw std::runtime_error("Ambiguous art filename"); png=&e.data; }
        }
        if(png) out=decode(*png,cache.empty()?fs::path{}:cache/(std::string(name)+(clear_black?".transparent-v1.rgba":".rgba")),clear_black); else art_status+="Missing "+std::string(name)+". ";
      }
      catch(const std::exception& e) { art_status+=std::string(name)+": "+e.what()+". "; }
    };
    load("AgencyBuildingRotation.png",background_); load("LogoCrackdown.png",logo_,true); load("Agency-logo-big.png",agency_);
  } catch(const std::exception& e) { art_status=e.what(); }
}
void Visuals::backdrop(ImVec2 size) {
  auto* d=ImGui::GetBackgroundDrawList();
  d->AddRectFilledMultiColor({0,0},size,IM_COL32(15,31,33,255),IM_COL32(8,14,22,255),IM_COL32(4,9,13,255),IM_COL32(8,15,18,255));
  if(background_) {
    auto s=texture_size(background_); float scale=std::max(size.x/s.x,size.y/s.y); ImVec2 drawn{s.x*scale,s.y*scale};
    ImVec2 lo{(size.x-drawn.x)/2,(size.y-drawn.y)/2},hi{lo.x+drawn.x,lo.y+drawn.y}; d->AddImage(id(background_),lo,hi);
    d->AddRectFilledMultiColor({0,0},size,IM_COL32(5,10,14,235),IM_COL32(5,10,14,100),IM_COL32(5,10,14,150),IM_COL32(5,10,14,245));
  }
  if(agency_) { float edge=std::min(size.y*.48f,360.f); d->AddImage(id(agency_),{size.x-edge-40,120},{size.x-40,120+edge},{0,0},{1,1},IM_COL32(255,255,255,28)); }
}
void Visuals::logo(float scale) {
  if(logo_) {
    auto s=texture_size(logo_); float w=220*scale;
    // The supported source has broad menu-safe margins around its central logo.
    ImGui::Image(id(logo_),{w,w*(s.y*.70f)/(s.x*.60f)},{.20f,.15f},{.80f,.85f});
  }
  else { ImGui::PushFont(heading); ImGui::TextUnformatted("CRACKDOWN"); ImGui::PopFont(); }
}
void Visuals::hints(const std::string& device,float scale) {
  float width=4*(28*scale+5*scale)+3*ImGui::GetStyle().ItemSpacing.x;
  for(const char* label:{"Select","Back","Previous tab","Next tab"}) width+=ImGui::CalcTextSize(label).x;
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(),ImGui::GetWindowWidth()-ImGui::GetStyle().WindowPadding.x-width));
  auto hint=[&](const char* pad,const char* key,const char* label) {
    std::string path=device+"/"+(device=="keyboard"?key:pad)+".png";
    if(!glyphs_.contains(path)) { try { glyphs_[path]=decode(read_bytes(home_/"assets/glyphs"/fs::u8path(path),1024*1024)); } catch(...) { glyphs_[path]=nullptr; } }
    if(auto* t=glyphs_[path]) { ImGui::Image(id(t),{28*scale,28*scale}); ImGui::SameLine(0,5*scale); }
    ImGui::TextUnformatted(label);
  };
  hint("a","Enter","Select"); ImGui::SameLine(); hint("b","Escape","Back"); ImGui::SameLine(); hint("lb","Q","Previous tab"); ImGui::SameLine(); hint("rb","E","Next tab");
}
}
