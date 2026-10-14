#pragma once
#include "v2.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <map>
namespace launcher {
class Visuals {
 public:
  Visuals(SDL_Renderer* renderer,const fs::path& home);
  ~Visuals();
  Visuals(const Visuals&)=delete;
  void load_art(const fs::path& game,const fs::path& user,const std::vector<Entry>& manifest);
  void clear_art();
  void backdrop(ImVec2 size);
  void logo(float scale);
  void hints(const std::string& device,float scale);
  ImFont* heading=nullptr;
  ImFont* body=nullptr;
  std::string art_status;
 private:
  SDL_Renderer* renderer_;
  fs::path home_;
  SDL_Texture* background_=nullptr; SDL_Texture* logo_=nullptr; SDL_Texture* agency_=nullptr;
  std::map<std::string,SDL_Texture*> glyphs_;
  SDL_Texture* decode(std::span<const unsigned char> png,const fs::path& cache={},bool clear_black=false);
  SDL_Texture* rgba(std::span<const unsigned char> bytes,int w,int h);
};
}
