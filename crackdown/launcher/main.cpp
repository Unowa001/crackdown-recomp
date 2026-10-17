#include "core.h"
#include "platform.h"
#include "v2.h"
#include "visuals.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <memory>
#include <thread>
#include <sstream>
#include <cmath>

using namespace launcher;
namespace {
struct Picker { std::mutex mutex; std::optional<fs::path> picked; bool folder=false; bool pending=false; std::string error; };
void SDLCALL picked(void* data,const char* const* files,int) {
  auto& p=*static_cast<Picker*>(data); std::lock_guard lock(p.mutex);
  if(!files) p.error=SDL_GetError(); else if(files[0]) p.picked=fs::u8path(files[0]);
  p.pending=false;
}
struct Task {
  std::thread thread; std::atomic_bool cancel=false, done=false;
  std::atomic<uint64_t> bytes=0,total=0;
  std::mutex mutex; std::string status,hash; fs::path folder; bool success=false;
  ~Task() { cancel=true; if(thread.joinable()) thread.join(); }
  void message(std::string s) { std::lock_guard lock(mutex); status=std::move(s); }
};
template<class T> T value(const toml::table& t,const char* key,T fallback) { auto n=setting(t,key); return n?n->value<T>().value_or(fallback):fallback; }
}
int main(int argc,char** argv) {
  // Headless diagnostic paths exercise exactly the core used by the UI.
  if((argc==4 || (argc==5 && std::string(argv[4])=="--full")) && std::string(argv[1])=="--verify-folder") {
    try { std::cout<<verify_folder(fs::u8path(argv[2]),load_manifest(fs::u8path(argv[3])),nullptr,argc==5?Check::full:Check::quick)<<'\n'; return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
  }
  if(argc==4 && std::string(argv[1])=="--inspect-iso") {
    try { Image image(fs::u8path(argv[2])); auto hash=image.xex_hash(); if(hash!=supported_xex) throw std::runtime_error("Unsupported version: "+hash); image.validate(load_manifest(fs::u8path(argv[3]))); std::cout<<image.files().size()<<" files; XEX "<<hash<<'\n'; return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
  }
  if(argc==4 && std::string(argv[1])=="--install-iso") {
    try {
      auto destination=fs::absolute(fs::u8path(argv[3]));
      InstanceLock lock(destination);
      std::atomic_bool cancel=false;
      auto progress=[](const char* phase) {
        return [phase,last=-1](uint64_t bytes,uint64_t total) mutable {
          int percent=total?static_cast<int>(100.0*bytes/total):0;
          if(percent/10!=last) { last=percent/10; std::cout<<phase<<": "<<percent<<"%\n"; }
        };
      };
      auto manifest=load_manifest(destination/"game-manifest.txt");
      install_image(fs::u8path(argv[2]),destination,manifest,cancel,
                    progress("Extracting"),progress("Verifying"));
      std::cout<<"Installed and fully verified "<<manifest.size()<<" files in "<<utf8(destination/"game")<<'\n';
      return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
  }
  bool review=argc==5 && std::string(argv[1])=="--review-ui";
  fs::path review_dir;
  int review_width=1280,review_height=720;
  if(review) {
    try {
      review_dir=fs::absolute(fs::u8path(argv[2]));
      review_width=std::stoi(argv[3]); review_height=std::stoi(argv[4]);
      if(review_width<960||review_width>3840||review_height<600||review_height>2160)
        throw std::runtime_error("Review size must be 960-3840 by 600-2160");
      fs::create_directories(review_dir);
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
  }
  if(!SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD)) { std::cerr<<SDL_GetError(); return 1; }
  const char* base=SDL_GetBasePath(); fs::path home=fs::absolute(base?fs::u8path(base):fs::current_path());
  std::unique_ptr<InstanceLock> instance;
  bool updated=argc==2 && std::string(argv[1])=="--updated";
  try {
    for(unsigned attempt=0;;++attempt) {
      try { instance=std::make_unique<InstanceLock>(home); break; }
      catch(...) { if(!updated||attempt==100) throw; SDL_Delay(100); }
    }
    recover_update(home);
  } catch(const std::exception& e) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Crackdown Recomp",e.what(),nullptr);
    SDL_Quit(); return 1;
  }
  auto show_folder=[](const fs::path& dir) { open_folder(dir,[](const char* url){return SDL_OpenURL(url);}); };
  float initial_dpi=std::max(1.f,SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay()));
  SDL_Window* window=SDL_CreateWindow("Crackdown Recomp",int(1280*initial_dpi),int(720*initial_dpi),SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY);
  SDL_Renderer* renderer=window?SDL_CreateRenderer(window,nullptr):nullptr;
  if(!renderer) { std::cerr<<SDL_GetError(); SDL_Quit(); return 1; }
  SDL_SetWindowMinimumSize(window,960,600);
  SDL_SetRenderVSync(renderer,1);
  ImGui::CreateContext(); ImGui::StyleColorsDark();
  auto& io=ImGui::GetIO(); io.IniFilename=nullptr;
  io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad;
  io.ConfigNavCursorVisibleAlways=true; io.ConfigNavEscapeClearFocusItem=false;
  io.ConfigNavEscapeClearFocusWindow=false;
  auto visuals=std::make_unique<Visuals>(renderer,home);
  ImGui_ImplSDL3_InitForSDLRenderer(window,renderer); ImGui_ImplSDLRenderer3_Init(renderer);
  fs::path config=home/"crackdown.toml", preferences=home/"launcher.toml";
  Task task; Picker picker; GameProcess process; std::vector<Entry> manifest;
  std::string error,report,hash,crash; fs::path game; bool verified=false,writable=true,close_on_play=false;
  Changes changes; int scale=1,window_size=0,rate=0; [[maybe_unused]] int filter=0; bool fullscreen=true,mute=false;
  float mouse_sens=1.0f;
  bool skip_intro=true,mnk=false,auto_updates=false,beta_channel=false,ask_updates=false;
  int monitor=0,language=1,tab=0; bool focus_page=true; std::string device="keyboard",input_backend="sdl";
  fs::file_time_type log_since{};
  fs::path user; bool portable=false; std::vector<std::string> profiles; std::string profile; char new_profile[48]{};
  try {
    auto probe=home/(".launcher-write-"+std::to_string(SDL_GetTicks()));
    if(fs::exists(probe)) throw std::runtime_error("Write probe already exists");
    std::ofstream out(probe); if(!out) throw std::runtime_error("Extract the beta into a writable folder, such as Downloads or Documents");
    out<<"probe"; out.close(); if(!out) throw std::runtime_error("Cannot write next to the launcher"); fs::remove(probe);
  } catch(const std::exception& e) { error=e.what(); writable=false; }
  try {

    auto t=read_config(config);
    std::string configured=value<std::string>(t,"game_data_root","");
    game=configured.empty()?home/"game":fs::u8path(configured);
    if(game.is_relative()) game=home/game;
    scale=static_cast<int>(value<int64_t>(t,"resolution_scale",1));
    fullscreen=value<bool>(t,"fullscreen",true); mute=value<bool>(t,"audio_mute",false);
    auto f=value<std::string>(t,"present_effect","bilinear"); filter=f=="cas"?1:f=="fsr"?2:0;
    rate=value<bool>(t,"vsync",false)?1:0;
    auto width=value<int64_t>(t,"window_width",0); window_size=width==1280?1:width==1920?2:width==2560?3:0;
    language=static_cast<int>(value<int64_t>(t,"user_language",1)); if(language<1||language>10) language=1;
    skip_intro=value<bool>(t,"skip_intro_videos",true); mnk=value<bool>(t,"mnk_mode",false);
    mouse_sens=std::clamp(static_cast<float>(value<double>(t,"mnk_sensitivity",1.0)),0.1f,5.0f);
    input_backend=value<std::string>(t,"input_backend","sdl"); monitor=std::clamp(static_cast<int>(value<int64_t>(t,"monitor",0)),0,16);
  } catch(const std::exception& e) { error=e.what(); }
  try { manifest=load_manifest(home/"game-manifest.txt"); } catch(const std::exception& e) { error=e.what(); }
  try {
    auto prefs=read_config(preferences);
    auto_updates=value<bool>(prefs,"auto_updates",false); beta_channel=value<bool>(prefs,"beta_updates",false);
    ask_updates=!value<bool>(prefs,"update_consent_asked",false);
    int lw=std::clamp(static_cast<int>(value<int64_t>(prefs,"launcher_width",int(1280*initial_dpi))),960,3840);
    int lh=std::clamp(static_cast<int>(value<int64_t>(prefs,"launcher_height",int(720*initial_dpi))),600,2160);
    SDL_SetWindowSize(window,lw,lh);
    close_on_play=value<bool>(prefs,"close_on_play",false);
    // Portable mode (portable.txt next to the launcher): saves live in userdata/ here, and the
    // game, which sees the same marker next to crackdown.exe, uses it too.
    portable=fs::is_regular_file(home/"portable.txt");
    user=fs::absolute(portable?home/"userdata":default_user_folder()).lexically_normal();
    profiles=list_profiles(user); profile=value<std::string>(prefs,"profile","");
    if(std::find(profiles.begin(),profiles.end(),profile)==profiles.end()) profile.clear();
  } catch(const std::exception& e) { error=e.what(); }
  // Archipelago settings belong to the selected (named) profile: one profile per seed.
  ApConnection ap; char ap_server[128]{},ap_slot[64]{},ap_password[128]{}; std::string ap_bound;
  // --connect <archipelago:// link> (from the Archipelago Launcher). It waits on the ARCHIPELAGO tab
  // until the player applies it to the profile they choose; it then replaces server, slot and password.
  std::optional<ApConnection> ap_link;
  for(int i=1;i+1<argc;++i) if(std::string(argv[i])=="--connect") {
    ap_link=parse_ap_uri(argv[i+1]);
    if(!ap_link) error="The Archipelago room link is not valid: expected archipelago://slot:password@host:port";
  }
  const bool ap_link_given=ap_link.has_value();
  auto load_ap=[&] {
    ap={}; ap_bound.clear();
    try { if(!user.empty()&&!profile.empty()) { auto saves=profile_folder(user,profile); ap=load_ap_connection(saves); ap_bound=ap_binding(saves); } }
    catch(const std::exception& e) { error=e.what(); }
    std::snprintf(ap_server,sizeof(ap_server),"%s",ap.server.c_str()); std::snprintf(ap_slot,sizeof(ap_slot),"%s",ap.slot.c_str());
    std::snprintf(ap_password,sizeof(ap_password),"%s",ap.password.c_str());
  };
  auto save_ap=[&] { ap.server=ap_server; ap.slot=ap_slot; ap.password=ap_password; save_ap_connection(profile_folder(user,profile),ap); };
  load_ap();
  // The on-screen keyboard (controller text entry) edits one buffer at a time.
  struct Keyboard { char* text; size_t size; const char* title; bool secret=false,ap=false; } keyboard{new_profile,sizeof(new_profile),"PROFILE NAME"};
  // An import or a newly selected folder gets the full hash check once; startup re-checks quickly.
  enum class Job { import_iso, full_check, quick_check };
  auto begin=[&](fs::path path,Job job) {
    if(task.thread.joinable()) task.thread.join();
    verified=false; hash.clear(); task.cancel=false; task.done=false; task.success=false; task.bytes=0; task.total=0;
    task.message(job==Job::import_iso?"Verifying disc version...":"Verifying game files...");
    task.thread=std::thread([&,path,job] {
      auto phase=[&](const char* text) {
        return [&task,text,started=false](uint64_t n,uint64_t total) mutable {
          if(!started) { task.message(text); started=true; }
          task.bytes=n; task.total=total;
        };
      };
      try {
        auto folder=path; auto check=job==Job::quick_check?Check::quick:Check::full;
        if(job==Job::import_iso) {
          install_image(path,home,manifest,task.cancel,phase("Extracting game files..."),phase("Checking every extracted file..."));
          folder=home/"game"; check=Check::quick;
        }
        auto h=verify_folder(folder,manifest,&task.cancel,check,phase(check==Check::full?"Checking every game file (one time)...":"Verifying game files..."));
        std::lock_guard lock(task.mutex); task.folder=fs::absolute(folder); task.hash=h; task.status="Game files verified"; task.success=true;
      } catch(const std::exception& e) { task.message(e.what()); }
      task.done=true;
    });
  };
  bool busy=false;
  if(writable && !manifest.empty() && fs::is_directory(game)) { begin(game,Job::quick_check); busy=true; }
  auto leftovers=writable?unfinished_imports(home,game):std::vector<fs::path>{};
  if(review) SDL_SetWindowSize(window,review_width,review_height);
  const char* tabs[]={"HOME","DISPLAY","GRAPHICS","GAMEPLAY","CONTROLS","SAVES","ARCHIPELAGO","ABOUT"};
  constexpr int tab_count=8, tab_ap=6, tab_about=7;
  if(ap_link_given) tab=tab_ap;
  // Smoke/review: two frames per tab, then each dialog for four frames (d = first dialog frame).
  constexpr unsigned d=2*tab_count;
  bool quit=false,review_failed=false; unsigned smoke=review || (argc==2 && std::string(argv[1])=="--smoke-ui")?d+22:0;
  bool is_smoke=smoke!=0;
  if(is_smoke) ask_updates=false;
  struct Online {
    std::thread worker; std::atomic_bool done=false,cancel=false;
    bool busy=false; std::atomic_bool install=false; std::vector<Release> releases; std::string message;
    ~Online() { cancel=true; if(worker.joinable()) worker.join(); }
  } online;
  bool github_authorized=auto_updates;
  auto check_updates=[&] {
    github_authorized=true;
    if(online.busy) return;
    if(online.worker.joinable()) online.worker.join();
    online.busy=true; online.install=false; online.done=false; online.cancel=false; online.message="Checking releases...";
    online.worker=std::thread([&] {
      try { auto r=check_releases(beta_channel,online.cancel); online.releases=std::move(r); online.message=online.releases.empty()?"No releases found.":compare_versions(online.releases.front().tag,LAUNCHER_VERSION)>0?"An update is available.":"You are up to date."; }
      catch(const std::exception& e) { online.message=e.what(); }
      online.done=true;
    });
  };
  auto install_update=[&](Release release) {
    if(online.worker.joinable()) online.worker.join();
    online.busy=true; online.install=true; online.done=false; online.cancel=false; online.message="Downloading, verifying and staging update...";
    online.worker=std::thread([&,release] {
      try { install_release(release,home,online.cancel); online.message="Update installed. Restarting..."; }
      catch(const std::exception& e) { online.message=e.what(); online.install=false; }
      online.done=true;
    });
  };
  // Explicit persisted consent is required before the first startup request.
  if(auto_updates&&!ask_updates&&!is_smoke) check_updates();
  enum class Dialog { none,install,browser,keyboard,updates,quit };
  Dialog dialog=Dialog::none; bool open_dialog=false,dialog_focus=false,folder_browser=false;
  fs::path browse=home; std::vector<fs::directory_entry> browse_entries; std::vector<fs::path> browse_roots;
  auto refresh_browser=[&] {
    browse_entries.clear(); std::error_code ec;
    for(fs::directory_iterator i(browse,fs::directory_options::skip_permission_denied,ec),end;!ec&&i!=end;i.increment(ec)) {
      auto& entry=*i; bool dir=entry.is_directory(ec); if(ec) { ec.clear(); continue; }
      auto extension=utf8(entry.path().extension()); std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return char(std::tolower(c));});
      if(dir||(!folder_browser&&extension==".iso")) browse_entries.push_back(entry);
    }
    std::sort(browse_entries.begin(),browse_entries.end(),[](const auto& a,const auto& b){std::error_code ec; bool ad=a.is_directory(ec),bd=b.is_directory(ec); if(ad!=bd) return ad; return a.path().filename()<b.path().filename();});
  };
  browse_roots.push_back(home); if(!user.empty()) browse_roots.push_back(user.parent_path());
#ifdef _WIN32
  for(char drive='C';drive<='Z';++drive) { std::string root; root+=drive; root+=":/"; std::error_code ec; if(fs::is_directory(fs::u8path(root),ec)) browse_roots.push_back(fs::u8path(root)); }
#else
  browse_roots.push_back("/");
#endif
  auto show_dialog=[&](Dialog next) { dialog=next; open_dialog=true; dialog_focus=true; };
  auto launch=[&] {
    try {
      for(const char* key:kApConfigKeys) changes[key]=std::nullopt;
      update_config(config,changes); changes.clear();
      hash=verify_folder(game,manifest); log_since=fs::file_time_type::clock::now();
      auto args=profile_args(user,profile); args.insert(args.begin(),"--game_data_root="+utf8(game));
      // The game reads the password from the profile's connection.toml itself.
      if(!profile.empty()) save_ap();
      auto extra=ap_args(profile.empty()?ApConnection{}:ap); args.insert(args.end(),extra.begin(),extra.end());
      process.start(home,args); error.clear(); crash.clear(); if(close_on_play) quit=true;
    } catch(const std::exception& e) { error=e.what(); verified=false; visuals->clear_art(); }
  };
  auto save_profile=[&](const std::string& name) {
    update_config(preferences,{{"profile",name.empty()?std::nullopt:std::optional<std::string>(toml_string(name))}}); profile=name; load_ap();
  };
  auto save_settings=[&] { try { update_config(config,changes); changes.clear(); error.clear(); } catch(const std::exception& e) { error=e.what(); } };
  bool auto_focus=true;
  bool modal_child_open=false;
  unsigned smoke_frame=0;
  while(!quit) {
    if(is_smoke && (!review||!busy)) {
      if(smoke_frame<d) { tab=int(smoke_frame/2); focus_page=true; }
      if(smoke_frame==d) show_dialog(Dialog::install);
      if(smoke_frame==d+4) { folder_browser=true; refresh_browser(); dialog=Dialog::browser; dialog_focus=true; }
      if(smoke_frame==d+8) { dialog=Dialog::keyboard; dialog_focus=true; }
      if(smoke_frame==d+12) { dialog=Dialog::updates; dialog_focus=true; }
      if(smoke_frame==d+16) { dialog=Dialog::quit; dialog_focus=true; }
      ++smoke_frame;
    }
    bool popup_before=ImGui::IsPopupOpen("",ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel);
    bool editing_before=io.WantTextInput,modal_child_before=modal_child_open; modal_child_open=false;
    bool back=false; int switch_tab=0;
    SDL_Event event;
    while(SDL_PollEvent(&event)) {
      ImGui_ImplSDL3_ProcessEvent(&event);
      if(event.type==SDL_EVENT_QUIT) { if(online.busy&&online.install) online.cancel=true; else if(!is_smoke&&!changes.empty()) show_dialog(Dialog::quit); else quit=true; }
      if(event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat) {
        device="keyboard";
        if(event.key.key==SDLK_ESCAPE) back=true;
        if(!io.WantTextInput && dialog==Dialog::none) { if(event.key.key==SDLK_Q) --switch_tab; if(event.key.key==SDLK_E) ++switch_tab; }
      }
      if(event.type==SDL_EVENT_MOUSE_MOTION && (std::abs(event.motion.xrel)>1||std::abs(event.motion.yrel)>1)) device="keyboard";
      if(event.type==SDL_EVENT_MOUSE_BUTTON_DOWN||event.type==SDL_EVENT_MOUSE_WHEEL) device="keyboard";
      if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN||event.type==SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        auto id=event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN?event.gbutton.which:event.gaxis.which;
        if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN||std::abs(int(event.gaxis.value))>12000) {
          auto type=SDL_GetGamepadTypeForID(id); device=type==SDL_GAMEPAD_TYPE_PS3||type==SDL_GAMEPAD_TYPE_PS4||type==SDL_GAMEPAD_TYPE_PS5?"ps5":"xbox_series";
        }
        if(event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
          if(event.gbutton.button==SDL_GAMEPAD_BUTTON_EAST) back=true;
          if(dialog==Dialog::none) { if(event.gbutton.button==SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) --switch_tab; if(event.gbutton.button==SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) ++switch_tab; }
        }
      }
      if(event.type==SDL_EVENT_WINDOW_RESIZED && writable && !is_smoke && !(online.busy&&online.install)) {
        try { update_config(preferences,{{"launcher_width",std::to_string(event.window.data1)},{"launcher_height",std::to_string(event.window.data2)}}); }
        catch(const std::exception& e) { error=e.what(); }
      }
    }
    if(switch_tab && !ask_updates && !popup_before && !editing_before) { tab=(tab+switch_tab%tab_count+tab_count)%tab_count; focus_page=true; }
    {
      std::lock_guard lock(picker.mutex);
      if(!picker.error.empty()) { error=picker.error; picker.error.clear(); }
      if(picker.picked) { visuals->clear_art(); begin(*picker.picked,picker.folder?Job::full_check:Job::import_iso); busy=true; picker.picked.reset(); error.clear(); tab=0; focus_page=true; }
    }
    if(busy && task.done) {
      task.thread.join(); busy=false;
      std::lock_guard lock(task.mutex);
      if(task.success) {
        try { update_config(config,{{"game_data_root",toml_string(utf8(task.folder))}}); game=task.folder; hash=task.hash; verified=true; leftovers=unfinished_imports(home,game); visuals->load_art(game,user,manifest); }
        catch(const std::exception& e) { error=e.what(); }
      } else error=task.status;
      focus_page=true;
    }
    if(online.busy&&online.done) {
      online.worker.join(); online.busy=false;
      if(online.install) {
        // The successor waits for this process to release the package lock.
        try { restart_launcher(home); quit=true; }
        catch(const std::exception& e) { error=e.what(); try { rollback_completed_update(home); online.install=false; online.message="Update rolled back: "+error; } catch(const std::exception& rollback) { error+="; rollback requires restart: "+std::string(rollback.what()); } }
      }
    }
    if(auto code=process.poll()) {
      if(*code!=0 || process.elapsed()<10) {
        char text[96]; std::snprintf(text,sizeof(text),"Game %s (exit code 0x%08X)",*code!=0?"crashed":"exited during startup",static_cast<unsigned>(*code)); error=text; crash=newest_log_tail(home,log_since);
      }
      focus_page=true;
    }
    ImGui_ImplSDLRenderer3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
    bool nested_popup=ImGui::IsPopupOpen("",ImGuiPopupFlags_AnyPopupId|ImGuiPopupFlags_AnyPopupLevel);
    if(back&&!ask_updates) {
      if(dialog!=Dialog::none) {
        // ImGui handles B/Escape for a nested combo first; a modal closes at its own level below.
        if(!nested_popup) { dialog=Dialog::none; focus_page=true; }
      } else if(!nested_popup && !popup_before && !editing_before) {
        if(tab!=0) { tab=0; focus_page=true; }
        else if(busy) task.cancel=true;
        else show_dialog(Dialog::quit);
      }
    }
    ImVec2 size=io.DisplaySize; float ui_scale=std::clamp(std::min(size.x/1280.f,size.y/720.f),.75f,2.f);
    auto& style=ImGui::GetStyle(); style.FontScaleMain=ui_scale;
    // Window dimensions include initial display DPI; one scale keeps fonts and controls aligned.
    style.FontScaleDpi=1.f;
    style.WindowPadding={24*ui_scale,16*ui_scale}; style.ItemSpacing={14*ui_scale,10*ui_scale}; style.FramePadding={12*ui_scale,8*ui_scale};
    style.FrameRounding=2; style.WindowRounding=4;
    style.Colors[ImGuiCol_Button]={.12f,.22f,.22f,.95f}; style.Colors[ImGuiCol_ButtonHovered]={.19f,.42f,.34f,1}; style.Colors[ImGuiCol_ButtonActive]={.23f,.57f,.41f,1};
    style.Colors[ImGuiCol_Header]={.13f,.35f,.28f,.92f}; style.Colors[ImGuiCol_HeaderHovered]={.2f,.5f,.38f,1}; style.Colors[ImGuiCol_HeaderActive]={.25f,.6f,.43f,1};
    style.Colors[ImGuiCol_FrameBg]={.08f,.18f,.17f,.94f}; style.Colors[ImGuiCol_FrameBgHovered]={.12f,.30f,.24f,1}; style.Colors[ImGuiCol_FrameBgActive]={.14f,.40f,.30f,1};
    style.Colors[ImGuiCol_TitleBg]={.06f,.14f,.13f,1}; style.Colors[ImGuiCol_TitleBgActive]={.12f,.30f,.24f,1};
    style.Colors[ImGuiCol_SliderGrab]={.28f,.70f,.49f,1}; style.Colors[ImGuiCol_SliderGrabActive]={.38f,.98f,.63f,1};
    style.Colors[ImGuiCol_TextDisabled]={.62f,.69f,.70f,1};
    style.Colors[ImGuiCol_NavCursor]={.38f,.98f,.63f,1}; style.Colors[ImGuiCol_CheckMark]=style.Colors[ImGuiCol_NavCursor]; style.Colors[ImGuiCol_WindowBg]={.04f,.08f,.10f,.96f};
    visuals->backdrop(size);
    ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(size);
    ImGui::Begin("Launcher",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBackground);
    visuals->logo(ui_scale);
    ImGui::SameLine();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY()+std::max(0.f,(ImGui::GetItemRectSize().y-ImGui::GetTextLineHeight())*.5f));
    ImGui::TextDisabled("RECOMP / BETA %s",LAUNCHER_VERSION);
    for(int i=0;i<tab_count;++i) {
      if(i) ImGui::SameLine(0,6*ui_scale);
      bool active=i==tab;
      if(active) ImGui::PushStyleColor(ImGuiCol_Button,{.16f,.43f,.32f,1});
      if(ImGui::Button(tabs[i])) { tab=i; focus_page=true; }
      if(active) ImGui::PopStyleColor();
    }
    ImGui::Separator();
    bool pending; { std::lock_guard lock(picker.mutex); pending=picker.pending; }
    bool blocked=busy||pending||process.running()||!writable||online.busy;
    ImGui::BeginChild("Page",{0,-100*ui_scale},false);
    auto_focus=focus_page; focus_page=false;
    auto focus_first=[&](bool enabled=true) { if(auto_focus&&enabled) { ImGui::SetKeyboardFocusHere(); auto_focus=false; } };
    auto menu=[&](const char* label,bool enabled=true) {
      ImGui::BeginDisabled(!enabled); focus_first(enabled); ImGui::PushFont(visuals->heading);
      bool clicked=ImGui::Selectable(label,false,0,{430*ui_scale,48*ui_scale});
      if(ImGui::IsItemFocused()||ImGui::IsItemHovered()) { auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax(); ImGui::GetWindowDrawList()->AddRectFilled({a.x-8*ui_scale,a.y},{a.x-4*ui_scale,b.y},IM_COL32(96,244,161,255)); }
      ImGui::PopFont(); ImGui::EndDisabled(); return clicked;
    };
    if(tab==0) {
      if(busy) {
        { std::lock_guard lock(task.mutex); ImGui::TextWrapped("%s",task.status.c_str()); }
        auto total=task.total.load(),bytes=task.bytes.load(); ImGui::ProgressBar(total?static_cast<float>(double(bytes)/total):0,{430*ui_scale,0});
        if(total) ImGui::Text("%.1f / %.1f GB",bytes/1e9,total/1e9);
        focus_first(); if(ImGui::Button("Cancel verification / import")) task.cancel=true;
      } else {
        if(menu(process.running()?"PLAYING...":verified?"PLAY":"INSTALL GAME",!blocked&&!manifest.empty())) { if(verified) launch(); else show_dialog(Dialog::install); }
        if(menu("VERIFY GAME FILES",!blocked&&!manifest.empty()&&fs::is_directory(game))) { visuals->clear_art(); begin(game,Job::full_check); busy=true; error.clear(); focus_page=true; }
        if(menu("CHECK FOR UPDATES",!online.busy)) { show_dialog(Dialog::updates); check_updates(); }
        if(menu("SETTINGS")) { tab=1; focus_page=true; }
        if(menu("QUIT",!(online.busy&&online.install))) show_dialog(Dialog::quit);
        ImGui::BeginDisabled(blocked||manifest.empty());
        if(ImGui::Button("Change game source...")) show_dialog(Dialog::install);
        if(!leftovers.empty()) {
          ImGui::TextWrapped("%zu unfinished import(s) can be removed.",leftovers.size());
          if(ImGui::Button("Delete unfinished import")) {
            leftovers=unfinished_imports(home,game);
            for(const auto& dir:leftovers) { std::error_code ec; fs::remove_all(dir,ec); if(ec) { error="Cannot delete "+utf8(dir)+": "+ec.message(); break; } }
            leftovers=unfinished_imports(home,game);
          }
        }
        ImGui::EndDisabled();
      }
      // A separate focusable card, reachable by D-pad/keyboard as well as mouse.
      ImGui::SetCursorPos({std::max(0.f,ImGui::GetContentRegionAvail().x-330*ui_scale),std::max(0.f,ImGui::GetWindowHeight()-90*ui_scale)});
      ImGui::BeginDisabled(blocked); if(ImGui::Button("Add Crackdown to Steam",{310*ui_scale,48*ui_scale})) { try { report=add_to_steam(home/"CrackdownLauncher.exe"); error.clear(); } catch(const std::exception& e) { error=e.what(); } } ImGui::EndDisabled();
    }
    if(tab>=1&&tab<=4) {
      ImGui::BeginDisabled(blocked); if(tab!=4) focus_first(!blocked);
      if(tab==1) {
        if(ImGui::Checkbox("Fullscreen",&fullscreen)) changes["fullscreen"]=fullscreen?std::nullopt:std::optional<std::string>("false");
        if(ImGui::Combo("Window size",&window_size,"Automatic\0 1280 x 720\0 1920 x 1080\0 2560 x 1440\0")) {
          const int widths[]={0,1280,1920,2560}, heights[]={0,720,1080,1440}; changes["window_width"]=window_size?std::optional<std::string>(std::to_string(widths[window_size])):std::nullopt; changes["window_height"]=window_size?std::optional<std::string>(std::to_string(heights[window_size])):std::nullopt;
        }
        if(ImGui::SliderInt("Monitor (0 = system default)",&monitor,0,16)) changes["monitor"]=monitor?std::optional<std::string>(std::to_string(monitor)):std::nullopt;
        ImGui::TextDisabled("Game display settings take effect on the next Play.");
      }
      if(tab==2) {
        int resolution=scale==2?1:0;
        if(ImGui::Combo("Internal resolution",&resolution,"720p (default)\0 1440p (experimental)\0")) { scale=resolution+1; changes["resolution_scale"]=scale==1?std::nullopt:std::optional<std::string>("2"); changes["draw_resolution_scale_x"]=std::nullopt; changes["draw_resolution_scale_y"]=std::nullopt; }
#if LAUNCHER_FIDELITYFX
        if(ImGui::Combo("Upscale filter",&filter,"Bilinear\0CAS\0FSR\0")) changes["present_effect"]=filter==0?std::nullopt:std::optional<std::string>(toml_string(filter==1?"cas":"fsr"));
#else
        ImGui::TextDisabled("Upscale filter: Bilinear (this build)");
#endif
        if(ImGui::Combo("Frame rate",&rate,"60 fps (default)\0 30 fps (stock)\0")) { changes["vsync"]=rate?std::optional<std::string>("true"):std::nullopt; changes["fps_cap"]=std::nullopt; }
      }
      if(tab==3) {
        int language_index=language-1;
        if(ImGui::Combo("Language",&language_index,"English\0Japanese\0German\0French\0Spanish\0Italian\0Korean\0Chinese (Traditional)\0Portuguese\0Chinese (Simplified)\0")) { language=language_index+1; changes["user_language"]=language==1?std::nullopt:std::optional<std::string>(std::to_string(language)); }
        ImGui::TextDisabled("Takes effect on the next Play. The disc may not include every language.");
        if(ImGui::Checkbox("Skip intro logos",&skip_intro)) changes["skip_intro_videos"]=skip_intro?std::nullopt:std::optional<std::string>("false");
        if(ImGui::Checkbox("Mute audio",&mute)) changes["audio_mute"]=mute?std::optional<std::string>("true"):std::nullopt;
        if(ImGui::Checkbox("Close launcher after Play",&close_on_play)) {
          try { update_config(preferences,{{"close_on_play",close_on_play?std::optional<std::string>("true"):std::nullopt}}); } catch(const std::exception& e) { error=e.what(); }
        }
      }
      if(tab==4) {
        ImGui::Text("Input backend: %s",input_backend.c_str());
        focus_first(!blocked);
        if(ImGui::Checkbox("Mouse / keyboard mode",&mnk)) changes["mnk_mode"]=mnk?std::optional<std::string>("true"):std::nullopt;
        if(mnk) {
          if(ImGui::SliderFloat("Mouse sensitivity",&mouse_sens,0.1f,5.0f,"%.2f")) {
            char text[16]; std::snprintf(text,sizeof(text),"%.2f",mouse_sens);
            changes["mnk_sensitivity"]=std::abs(mouse_sens-1.0f)<0.005f?std::nullopt:std::optional<std::string>(text);
          }
          ImGui::TextWrapped("Game: WASD moves, the mouse aims, Enter or X is Start, left click fires, right click locks on, Space is A (jump), Backspace is B, L is the X button, P is Y, 1 / 3 are the bumpers, Tab is Back. Shift+arrows is the D-pad.");
        }
        ImGui::TextWrapped("Launcher: D-pad or left stick moves, the confirm button selects, and the back button returns. Shoulder buttons switch tabs. Keyboard: arrows, Enter, Esc, Q/E.");
      }
      if(ImGui::Button(changes.empty()?"Save settings":"Save settings *")) save_settings();
      ImGui::EndDisabled();
    }
    if(tab==5) {
      ImGui::BeginDisabled(blocked||user.empty()); focus_first(!blocked&&!user.empty());
      if(ImGui::BeginCombo("Profile",profile.empty()?"Default (existing saves)":profile.c_str())) {
        try { if(ImGui::Selectable("Default (existing saves)",profile.empty())) save_profile(""); for(const auto& name:profiles) if(ImGui::Selectable(name.c_str(),name==profile)) save_profile(name); }
        catch(const std::exception& e) { error=e.what(); } ImGui::EndCombo();
      }
      ImGui::SetNextItemWidth(280*ui_scale); ImGui::InputTextWithHint("##newprofile","New profile name",new_profile,sizeof(new_profile));
      if(ImGui::Button("Enter profile name...")) { keyboard={new_profile,sizeof(new_profile),"PROFILE NAME"}; show_dialog(Dialog::keyboard); }
      ImGui::SameLine(); if(ImGui::Button("Create profile")) {
        try { create_profile(user,new_profile); profiles=list_profiles(user); save_profile(new_profile); new_profile[0]=0; error.clear(); } catch(const std::exception& e) { error=e.what(); }
      }
      if(ImGui::Button("Open saves folder")) {
        try { (void)profile_args(user,profile); auto dir=profile.empty()?user:user/"profiles"/fs::u8path(profile); fs::create_directories(dir); show_folder(dir); } catch(const std::exception& e) { error=e.what(); }
      }
      if(ImGui::Checkbox("Keep saves with the game (portable)",&portable)) {
        // Copies, never moves: the other location keeps its saves, and switching back finds them.
        const bool want=portable; bool ok=true;
        auto documents=fs::absolute(default_user_folder()).lexically_normal(), local=home/"userdata";
        try {
          if(want) {
            if(!fs::exists(local)) {
              // The first copy goes to a staging folder that becomes userdata only once complete, so a
              // failed copy never passes for a finished one on the next try.
              auto staging=home/"userdata.partial";
              fs::remove_all(staging); fs::create_directories(staging);
              if(fs::is_directory(documents)) for(const auto& item:fs::directory_iterator(documents)) {
                auto name=item.path().filename();
                if(name=="cache"||name=="logs") continue;  // rebuilt on their own
                fs::copy(item.path(),staging/name,fs::copy_options::recursive|fs::copy_options::copy_symlinks);
              }
              fs::rename(staging,local);
            }
            std::ofstream marker(home/"portable.txt",std::ios::binary|std::ios::trunc);
            marker<<"Saves are in userdata\\ next to this file. Delete this file to use Documents\\crackdown again.\n";
            marker.close(); if(!marker) throw std::runtime_error("Cannot write portable.txt next to the launcher");
          } else fs::remove(home/"portable.txt");
        } catch(const std::exception& e) { ok=false; error=e.what(); }
        // Mode, save root, profile and its Archipelago settings all follow the marker on disk, so
        // they stay consistent whichever step failed; the profile state is reloaded from the new root
        // even when a profile of the same name exists in both.
        std::error_code marker_ec; portable=fs::is_regular_file(home/"portable.txt",marker_ec);
        user=fs::absolute(portable?local:documents).lexically_normal();
        try { profiles=list_profiles(user); } catch(const std::exception& e) { profiles.clear(); ok=false; error=e.what(); }
        if(std::find(profiles.begin(),profiles.end(),profile)==profiles.end()) {
          profile.clear();
          try { update_config(preferences,{{"profile",std::nullopt}}); } catch(const std::exception& e) { ok=false; error=e.what(); }
        }
        load_ap();
        if(ok) error.clear();
      }
      ImGui::EndDisabled(); ImGui::TextWrapped("%s",portable?"Saves are in userdata next to the launcher. Turning this off goes back to Documents\\crackdown; nothing is deleted.":"A new profile starts with no save: choose New Game. Profiles share the shader cache. Portable mode copies your saves next to the launcher and uses them from there.");
    }
    if(tab==tab_ap) {
      bool named=!user.empty()&&!profile.empty();
      ImGui::BeginDisabled(blocked||!named); focus_first(!blocked&&named);
      bool edited=ImGui::Checkbox("Connect to Archipelago when this profile plays",&ap.enabled);
      auto field=[&](const char* label,const char* hint,char* text,size_t size,bool secret) {
        ImGui::PushID(label); ImGui::SetNextItemWidth(430*ui_scale);
        ImGui::InputTextWithHint(label,hint,text,size,secret?ImGuiInputTextFlags_Password:0); edited|=ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine(); if(ImGui::Button("Keyboard...")) { keyboard={text,size,label,secret,true}; show_dialog(Dialog::keyboard); }
        ImGui::PopID();
      };
      field("Server","archipelago.gg:38281",ap_server,sizeof(ap_server),false);
      field("Slot name","Your player name in the seed",ap_slot,sizeof(ap_slot),false);
      field("Password","Leave empty if the room has none",ap_password,sizeof(ap_password),true);
      if(edited) { try { save_ap(); error.clear(); } catch(const std::exception& e) { error=e.what(); } }
      ImGui::EndDisabled();
      if(!named) ImGui::TextWrapped("Archipelago needs its own save profile, one per seed. Create one on the SAVES tab, then come back here.");
      if(ap_link) {
        ImGui::Separator();
        ImGui::TextWrapped("Room link: slot \"%s\" on %s.",ap_link->slot.c_str(),ap_link->server.c_str());
        if(!named) ImGui::TextWrapped("Select or create the save profile for this seed on the SAVES tab, then apply the link here.");
        ImGui::BeginDisabled(blocked||!named);
        if(ImGui::Button("Use this room link for this profile")) {
          if(ap_link->server.size()>=sizeof(ap_server)||ap_link->slot.size()>=sizeof(ap_slot)||ap_link->password.size()>=sizeof(ap_password))
            error="The room link's server, slot name or password is too long";
          else {
            ap.enabled=true;
            std::snprintf(ap_server,sizeof(ap_server),"%s",ap_link->server.c_str()); std::snprintf(ap_slot,sizeof(ap_slot),"%s",ap_link->slot.c_str());
            std::snprintf(ap_password,sizeof(ap_password),"%s",ap_link->password.c_str());
            try { save_ap(); ap_link.reset(); error.clear(); } catch(const std::exception& e) { error=e.what(); }
          }
        }
        ImGui::EndDisabled();
      }
      else if(!ap_bound.empty()) ImGui::TextWrapped("This profile is bound to %s. Use a new profile for a different seed.",ap_bound.c_str());
      else ImGui::TextWrapped("Not bound to a seed yet. Press Play, then choose Campaign > Play Now (New Game) while connected: that binds this profile to the seed.");
      ImGui::TextDisabled("Preview: the orbs around Pacific City Bank are checks, and Progressive Agility is the only item.");
      ImGui::TextDisabled("The password is stored only in this profile's folder and is not included in system info.");
    }
    if(tab==tab_about) {
      ImGui::Text("Crackdown Recomp / Launcher %s",LAUNCHER_VERSION);
      ImGui::TextWrapped("Update source: %s (preview configuration)",release_repository);
      ImGui::TextWrapped("Standalone front end using SDL3, Dear ImGui, toml++ and stb_image. Barlow fonts: SIL OFL 1.1. Controller prompts: CC0. Game art comes from your verified files.");
      focus_first(); if(ImGui::Button("Open licenses")) { try { show_folder(home/"LICENSES"); } catch(const std::exception& e) { error=e.what(); } }
      ImGui::SameLine(); ImGui::BeginDisabled(!github_authorized); if(ImGui::Button("Open repository")) { auto url="https://github.com/"+std::string(release_repository); if(!SDL_OpenURL(url.c_str())) error=SDL_GetError(); } ImGui::EndDisabled();
      if(ImGui::Button("Open logs folder")) { try { fs::create_directories(home/"logs"); show_folder(home/"logs"); } catch(const std::exception& e) { error=e.what(); } }
      ImGui::SameLine(); if(ImGui::Button("Copy system info")) {
        try { report="Launcher: " LAUNCHER_VERSION "\n"+system_info()+"Game XEX SHA256: "+hash+"\nSave profile: "+(profile.empty()?"default":profile)+"\ncrackdown.toml:\n"+redacted_config(config); if(!SDL_SetClipboardText(report.c_str())) error=SDL_GetError(); }
        catch(const std::exception& e) { error=e.what(); }
      }
      ImGui::BeginDisabled(online.busy||!writable);
      bool proposed=auto_updates;
      if(ImGui::Checkbox("Check for updates when the launcher opens",&proposed)) {
        try { update_config(preferences,{{"auto_updates",proposed?"true":"false"},{"update_consent_asked","true"}}); auto_updates=proposed; if(auto_updates) check_updates(); } catch(const std::exception& e) { error=e.what(); }
      }
      proposed=beta_channel;
      if(ImGui::Checkbox("Include prereleases",&proposed)) { try { update_config(preferences,{{"beta_updates",proposed?"true":"false"}}); beta_channel=proposed; } catch(const std::exception& e) { error=e.what(); } }
      ImGui::EndDisabled();
      if(ImGui::Button("Version history / updates")) show_dialog(Dialog::updates);
      if(!visuals->art_status.empty()) ImGui::TextWrapped("Art fallback: %s",visuals->art_status.c_str());
    }
    if(online.busy) ImGui::TextUnformatted(online.install?"Installing update...":"Checking updates...");
    if(!error.empty()) { ImGui::Spacing(); ImGui::TextWrapped("%s",error.c_str()); }
    if(!report.empty()&&tab==0) ImGui::TextWrapped("%s",report.c_str());
    if(!crash.empty()) { ImGui::BeginChild("Startup log",{0,130*ui_scale},true); ImGui::TextUnformatted(crash.c_str()); ImGui::EndChild(); }
    ImGui::EndChild();
    ImGui::Separator(); ImGui::Text("GAME  %s     PROFILE  %s     ARCHIPELAGO  %s     VERSION  %s",busy?"Checking...":verified?"Ready to play":"Not installed",profile.empty()?"Default":profile.c_str(),!profile.empty()&&ap.enabled?"On":"Off",LAUNCHER_VERSION);
    visuals->hints(device,ui_scale);
    ImGui::End();
    if(ask_updates) ImGui::OpenPopup("Update preferences");
    ImGui::SetNextWindowSize({610*ui_scale,0},ImGuiCond_Always);
    if(ImGui::BeginPopupModal("Update preferences",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextWrapped("Allow the launcher to check GitHub Releases when it opens? No request is made until you enable this. You can always check manually.");
      ImGui::TextWrapped("Update source: %s (preview configuration).",release_repository);
      auto consent=[&](bool enabled) {
        try { update_config(preferences,{{"auto_updates",enabled?"true":"false"},{"update_consent_asked","true"}}); auto_updates=enabled; ask_updates=false; ImGui::CloseCurrentPopup(); focus_page=true; if(enabled) check_updates(); }
        catch(const std::exception& e) { error=e.what(); }
      };
      if(ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
      ImGui::BeginDisabled(!writable); if(ImGui::Button("Enable update checks")) consent(true); ImGui::SameLine(); if(ImGui::Button("Keep checks off")||back) consent(false); ImGui::EndDisabled();
      if(!writable && ImGui::Button("Continue with checks off")) { ask_updates=false; auto_updates=false; ImGui::CloseCurrentPopup(); focus_page=true; }
      if(!error.empty()) ImGui::TextWrapped("%s",error.c_str()); ImGui::EndPopup();
    }
    if(open_dialog) { ImGui::OpenPopup("Launcher action"); open_dialog=false; }
    ImGui::SetNextWindowSize({std::min(size.x-48*ui_scale,800*ui_scale),std::min(size.y-80*ui_scale,580*ui_scale)},ImGuiCond_Always);
    if(ImGui::BeginPopupModal("Launcher action",nullptr,ImGuiWindowFlags_NoSavedSettings)) {
      bool child_popup=ImGui::IsPopupOpen("",ImGuiPopupFlags_AnyPopupId);
      auto close_dialog=[&] { ImGui::CloseCurrentPopup(); dialog=Dialog::none; focus_page=true; };
      auto modal_focus=[&] { if(dialog_focus) { ImGui::SetKeyboardFocusHere(); dialog_focus=false; } };
      if(dialog==Dialog::install) {
        ImGui::TextUnformatted("INSTALL GAME"); ImGui::TextWrapped("Select your supported World multi-language disc image or an extracted game folder. Your source files are kept in place.");
        modal_focus();
        auto browser=[&](bool folder) { folder_browser=folder; browse=fs::is_directory(game)?game:home; refresh_browser(); dialog=Dialog::browser; dialog_focus=true; };
        if(ImGui::Button("Browse ISO")) browser(false);
        ImGui::SameLine(); if(ImGui::Button("Browse extracted folder")) browser(true);
        if(ImGui::Button("Native ISO picker (keyboard / mouse)")) {
          { std::lock_guard lock(picker.mutex); picker.pending=true; picker.folder=false; }
          static const SDL_DialogFileFilter filters[]={{"Xbox disc image","iso"}};
          SDL_ShowOpenFileDialog(picked,&picker,window,filters,1,nullptr,false); close_dialog();
        }
        if(ImGui::Button("Native folder picker (keyboard / mouse)")) {
          { std::lock_guard lock(picker.mutex); picker.pending=true; picker.folder=true; }
          SDL_ShowOpenFolderDialog(picked,&picker,window,nullptr,false); close_dialog();
        }
      } else if(dialog==Dialog::browser) {
        ImGui::TextUnformatted(folder_browser?"SELECT GAME FOLDER":"SELECT DISC IMAGE"); ImGui::TextWrapped("%s",utf8(browse).c_str()); modal_focus();
        if(ImGui::BeginCombo("Location",utf8(browse).c_str())) {
          for(const auto& root:browse_roots) if(ImGui::Selectable(utf8(root).c_str())) { browse=root; refresh_browser(); dialog_focus=true; } ImGui::EndCombo();
        }
        if(ImGui::Button("Parent folder")) { if(browse.has_parent_path()) { browse=browse.parent_path(); refresh_browser(); dialog_focus=true; } }
        if(folder_browser) { ImGui::SameLine(); if(ImGui::Button("Use this folder")) { visuals->clear_art(); begin(browse,Job::full_check); busy=true; tab=0; close_dialog(); } }
        ImGui::BeginChild("Files",{0,-58*ui_scale},true);
        std::optional<fs::path> chosen; bool directory=false;
        for(size_t i=0;i<browse_entries.size();++i) {
          const auto& e=browse_entries[i]; std::error_code ec; bool dir=e.is_directory(ec); auto name=(dir?"[Folder] ":"[ISO] ")+utf8(e.path().filename()); ImGui::PushID(static_cast<int>(i));
          if(ImGui::Selectable(name.c_str())) { chosen=e.path(); directory=dir; } ImGui::PopID();
        }
        ImGui::EndChild();
        if(chosen) { if(directory) { browse=*chosen; refresh_browser(); dialog_focus=true; } else { visuals->clear_art(); begin(*chosen,Job::import_iso); busy=true; tab=0; close_dialog(); } }
      } else if(dialog==Dialog::keyboard) {
        auto& k=keyboard; auto n=std::char_traits<char>::length(k.text);
        ImGui::TextUnformatted(k.title); ImGui::Text("%s",k.secret?std::string(n,'*').c_str():k.text); modal_focus();
        auto type=[&](char c) { if(n<k.size-1) { k.text[n]=c; k.text[++n]=0; } };
        const char* keys=k.ap?"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.:/@!#$%&*+=?~":"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-.";
        for(size_t i=0;keys[i];++i) { if(i%10) ImGui::SameLine(); char label[]={keys[i],0}; ImGui::PushID(static_cast<int>(i)); if(ImGui::Button(label,{40*ui_scale,35*ui_scale})) type(keys[i]); ImGui::PopID(); }
        if(ImGui::Button("Space")) type(' '); ImGui::SameLine();
        if(ImGui::Button("Backspace")) { if(n) k.text[--n]=0; } ImGui::SameLine();
        if(ImGui::Button("Done")) {
          if(k.ap) { try { save_ap(); error.clear(); } catch(const std::exception& e) { error=e.what(); } }
          close_dialog();
        }
      } else if(dialog==Dialog::updates) {
        ImGui::TextUnformatted("UPDATES / VERSION HISTORY"); ImGui::TextWrapped("Update source: %s",release_repository);
        modal_focus(); ImGui::BeginDisabled(online.busy);
        if(ImGui::Button("Check for updates")) check_updates(); ImGui::EndDisabled();
        if(online.busy) {
          ImGui::TextUnformatted(online.install?"Downloading, verifying and staging update...":"Checking releases..."); ImGui::ProgressBar(-float(SDL_GetTicks()%10000)/1000.f,{0,0});
          if(ImGui::Button("Cancel update operation")) online.cancel=true;
        } else {
          ImGui::TextWrapped("%s",online.message.c_str());
          ImGui::BeginChild("Release history",{0,-58*ui_scale},true);
          for(size_t i=0;i<online.releases.size();++i) {
            const auto& r=online.releases[i]; ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%s%s",r.tag.c_str(),r.prerelease?" (prerelease)":""); ImGui::TextWrapped("%s",r.notes.c_str());
            bool newer=compare_versions(r.tag,LAUNCHER_VERSION)>0;
            ImGui::BeginDisabled(!newer||r.zip_url.empty()||r.sha_url.empty()||blocked);
            if(ImGui::Button("Update now")) install_update(r); ImGui::EndDisabled(); ImGui::Separator(); ImGui::PopID();
            if(online.busy) break;
          }
          ImGui::EndChild();
        }
      } else if(dialog==Dialog::quit) {
        ImGui::TextUnformatted("QUIT LAUNCHER?"); if(!changes.empty()) ImGui::TextWrapped("There are unsaved game settings."); modal_focus();
        if(ImGui::Button("Quit")) { quit=true; close_dialog(); }
        if(!changes.empty()) { ImGui::SameLine(); if(ImGui::Button("Save settings and quit")) { save_settings(); if(changes.empty()) { quit=true; close_dialog(); } } }
      }
      if(dialog!=Dialog::none) {
        if(dialog!=Dialog::browser) ImGui::Spacing();
        if(ImGui::Button("Back") || (back&&!child_popup&&!modal_child_before&&!editing_before)) {
          if(dialog==Dialog::browser) { dialog=Dialog::install; dialog_focus=true; }
          else close_dialog();
        }
      }
      if(!error.empty()) ImGui::TextWrapped("%s",error.c_str());
      modal_child_open=ImGui::IsPopupOpen("",ImGuiPopupFlags_AnyPopupId); ImGui::EndPopup();
    }
    ImGui::Render(); SDL_SetRenderDrawColor(renderer,8,14,20,255); SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),renderer);
    if(review&&!busy && ((smoke_frame<=d&&smoke_frame%2==0)||(smoke_frame>d&&smoke_frame<=d+20&&(smoke_frame-d)%4==0))) {
      auto* surface=SDL_RenderReadPixels(renderer,nullptr);
      auto file=review_dir/("frame-"+std::to_string(smoke_frame)+".bmp");
      if(!surface||!SDL_SaveBMP(surface,utf8(file).c_str())) { std::cerr<<SDL_GetError()<<'\n'; review_failed=true; quit=true; }
      if(surface) SDL_DestroySurface(surface);
    }
    SDL_RenderPresent(renderer);
    if(smoke&&(!review||!busy) && --smoke==0) quit=true;
  }
  task.cancel=true; if(task.thread.joinable()) task.thread.join();
  online.cancel=true; if(online.worker.joinable()) online.worker.join();
  for(;;) { { std::lock_guard lock(picker.mutex); if(!picker.pending) break; } SDL_PumpEvents(); SDL_Delay(10); }
  visuals.reset();
  ImGui_ImplSDLRenderer3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext();
  SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit(); return review_failed?1:0;
}
