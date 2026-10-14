#include "core.h"
#include "platform.h"
#include <array>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>

using namespace launcher;
namespace {
int checks=0;
void require(bool b,const char* message) { ++checks; if(!b) throw std::runtime_error(message); }
template<class F> void rejects(F f,const char* message) { bool thrown=false; try { f(); } catch(const std::exception&) { thrown=true; } require(thrown,message); }
void environment(const char* name,const char* value) {
#ifdef _WIN32
  _putenv_s(name,value?value:"");
#else
  if(value) setenv(name,value,1); else unsetenv(name);
#endif
}
void put32(std::vector<unsigned char>& b,size_t p,uint32_t n) { for(unsigned i=0;i<4;++i) b.at(p+i)=static_cast<unsigned char>(n>>(8*i)); }
void put16(std::vector<unsigned char>& b,size_t p,uint16_t n) { b.at(p)=n&255; b.at(p+1)=n>>8; }
void entry(std::vector<unsigned char>& b,size_t p,const std::string& name,uint32_t sector,uint32_t size,bool dir=false) {
  put32(b,p+4,sector); put32(b,p+8,size); b.at(p+12)=dir?0x10:0; b.at(p+13)=static_cast<unsigned char>(name.size());
  std::copy(name.begin(),name.end(),b.begin()+p+14);
}
std::vector<unsigned char> fixture() {
  std::vector<unsigned char> b(44*2048);
  std::string magic="MICROSOFT*XBOX*MEDIA";
  std::copy(magic.begin(),magic.end(),b.begin()+0x10000);
  std::copy(magic.begin(),magic.end(),b.begin()+0x10000+2048-20);
  put32(b,0x10000+20,40); put32(b,0x10000+24,96);
  entry(b,40*2048,"dir",41,32,true); put16(b,40*2048,8); put16(b,40*2048+2,16);
  entry(b,40*2048+32,"alpha.txt",42,5); entry(b,40*2048+64,"zero.bin",43,0);
  entry(b,41*2048,"nested.bin",43,3);
  std::copy_n("hello",5,b.begin()+42*2048); std::copy_n("abc",3,b.begin()+43*2048); return b;
}
void save(const fs::path& p,const std::vector<unsigned char>& b,uint64_t partition=0) {
  std::ofstream out(p,std::ios::binary); out.seekp(static_cast<std::streamoff>(partition)); out.write(reinterpret_cast<const char*>(b.data()),b.size());
  if(!out) throw std::runtime_error("Cannot create synthetic image");
}
std::string text(const fs::path& p) { std::ifstream in(p,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}}; }
void write(const fs::path& p,const std::string& s) { std::ofstream out(p,std::ios::binary); out<<s; }
}
int main(int argc,char** argv) {
  auto home=fs::temp_directory_path()/("crackdown-launcher-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directory(home);
  try {
    write(home/"abc","abc"); require(hash_file(home/"abc")=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 vector");
    write(home/"empty",""); require(hash_file(home/"empty")=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","empty SHA256");
    for(const auto& n:{"..",".","../oops","CON.txt","LPT1","aux","foo.","foo ","C:bad","/abs","a\\b","a:b","a?b"}) require(!safe_name(n),"unsafe path accepted");
    require(safe_name("$SystemUpdate"),"valid dollar name"); require(safe_name("normal.bin"),"normal name");
    auto b=fixture(); save(home/"test.iso",b); Image image(home/"test.iso");
    require(image.files().size()==3,"nested directory, left/right traversal");
    std::atomic_bool cancel=false; uint64_t progress=0;
    image.extract(home/"out",cancel,[&](uint64_t n,uint64_t total){require(total==8,"byte total");progress=n;});
    require(progress==8,"byte progress"); require(text(home/"out/alpha.txt")=="hello","streamed file");
    require(text(home/"out/dir/nested.bin")=="abc","nested file"); require(fs::file_size(home/"out/zero.bin")==0,"zero-length file");
    image.validate({{"alpha.txt",0,5,{}},{"dir/nested.bin",0,3,{}},{"zero.bin",0,0,{}}});
    rejects([&]{image.validate({{"alpha.txt",0,6,{}}});},"mismatching image manifest");
    rejects([&]{image.extract(home/"out",cancel,{});},"never overwrite existing extraction");
    require(text(home/"out/alpha.txt")=="hello","existing game preserved");
    cancel=true; rejects([&]{image.extract(home/"cancelled",cancel,{});},"cancellation"); require(!fs::exists(home/"cancelled"),"cancel cleanup"); cancel=false;
    rejects([&]{image.extract(home/"failed",cancel,[](uint64_t,uint64_t){throw std::runtime_error("disk failure");});},"failed write"); require(!fs::exists(home/"failed"),"failure cleanup");
    auto bad=b; put16(bad,40*2048+32,8); save(home/"bad.iso",bad); rejects([&]{Image x(home/"bad.iso");},"tree self-cycle");
    bad=b; put32(bad,40*2048+32+4,1000); save(home/"bad.iso",bad); rejects([&]{Image x(home/"bad.iso");},"file out of bounds");
    bad=b; put16(bad,40*2048+2,65535); save(home/"bad.iso",bad); rejects([&]{Image x(home/"bad.iso");},"subtree out of bounds");
    bad=b; entry(bad,41*2048,"loop",40,96,true); save(home/"bad.iso",bad); rejects([&]{Image x(home/"bad.iso");},"directory cycle");
    bad=b; entry(bad,40*2048+32,"CON.txt",42,5); save(home/"bad.iso",bad); rejects([&]{Image x(home/"bad.iso");},"reserved filename");
    bad=b; entry(bad,40*2048+32,"../bad",42,5); save(home/"bad.iso",bad); rejects([&]{Image x(home/"bad.iso");},"traversal name");
    bad=b; entry(bad,40*2048+64,"ALPHA.TXT",43,0); save(home/"bad.iso",bad); rejects([&]{Image x(home/"bad.iso");},"case collision");
    bad=b; bad[0x10000+2048-1]=0; save(home/"bad.iso",bad); rejects([&]{Image x(home/"bad.iso");},"invalid descriptor trailer");
    write(home/"bad.iso","random"); rejects([&]{Image x(home/"bad.iso");},"non-image");
    for(uint64_t offset:{0xFD90000ULL,0x18300000ULL}) { save(home/"full.iso",b,offset); require(Image(home/"full.iso").files().size()==3,"full image partition"); fs::remove(home/"full.iso"); }
    save(home/"full.iso",b,0x2080000); rejects([&]{Image x(home/"full.iso");},"XGD3 rejection"); fs::remove(home/"full.iso");
    write(home/"manifest","default.xex\t3\t-\n"); auto manifest=load_manifest(home/"manifest"); require(manifest.size()==1,"manifest parse");
    write(home/"manifest","../default.xex\t3\t-\n"); rejects([&]{load_manifest(home/"manifest");},"unsafe manifest");
    write(home/"manifest","default.xex\t3\t-\nDEFAULT.XEX\t3\t-\n"); rejects([&]{load_manifest(home/"manifest");},"duplicate manifest");
    fs::create_directory(home/"folder"); write(home/"folder/default.xex","abc");
    rejects([&]{verify_folder(home/"folder",manifest);},"unsupported XEX");
    rejects([&]{verify_folder(home/"folder",{{"default.xex",0,4,{}}});},"wrong sized folder file");
    {
      std::string what; try { verify_folder(home/"folder",{{"default.xex",0,3,{}},{"big.bin",0,2*quick_hash_limit,{}}},nullptr,Check::full); } catch(const std::exception& e) { what=e.what(); }
      require(what.find("no hash for big.bin")!=what.npos,"full check needs every hash");
    }
    auto xex=hash_file(home/"folder/default.xex");
    write(home/"folder/small.bin","small"); write(home/"folder/big.bin",std::string(9*quick_hash_limit,'b'));
    std::vector<Entry> supported{{"default.xex",0,3,{}},{"small.bin",0,5,hash_file(home/"folder/small.bin")},
                                 {"big.bin",0,9*quick_hash_limit,hash_file(home/"folder/big.bin")}};
    for(auto check:{Check::quick,Check::full}) {
      uint64_t last=0, total_seen=0; unsigned reports=0;
      auto expected=check==Check::quick?5:5+9*quick_hash_limit;
      require(verify_folder(home/"folder",supported,nullptr,check,[&](uint64_t n,uint64_t total) {
        require(n>=last && n<=total,"verification monotonic progress");
        require(total==expected,"verification byte total"); last=n; total_seen=total; ++reports;
      },xex)==xex,"successful verification");
      require(reports>=2 && last==total_seen,"verification completed progress");
    }
    write(home/"folder/big.bin",std::string(9*quick_hash_limit,'c'));
    require(verify_folder(home/"folder",supported,nullptr,Check::quick,{},xex)==xex,"quick skips large corruption");
    rejects([&]{verify_folder(home/"folder",supported,nullptr,Check::full,{},xex);},"full catches same-size corruption");
    write(home/"folder/small.bin","wrong");
    rejects([&]{verify_folder(home/"folder",supported,nullptr,Check::quick,{},xex);},"quick catches small corruption");
    write(home/"folder/small.bin","small"); write(home/"folder/big.bin",std::string(9*quick_hash_limit,'b'));
    bool hashing=false; std::string cancellation;
    try { verify_folder(home/"folder",supported,&cancel,Check::full,[&](uint64_t n,uint64_t) {
      if(n>quick_hash_limit) { hashing=true; cancel=true; }
    },xex); } catch(const std::exception& e) { cancellation=e.what(); }
    require(hashing && cancellation=="Cancelled","cancel during hashing"); cancel=false;
    rejects([&]{verify_folder(home/"folder",{{"default.xex",0,4,{}}},nullptr,Check::quick,{},xex);},"supported XEX wrong size");
    auto staging=home/"game-install-1"; fs::create_directory(staging);
    require(unfinished_imports(home).empty(),"unmarked folder excluded");
    write(staging/".crackdown-import","garbage"); require(unfinished_imports(home).empty(),"invalid marker excluded");
    mark_import(staging); require(unfinished_imports(home).empty(),"live import owner excluded");
    auto owner=current_process_owner(); require(owner_alive(owner),"current process alive");
    // A mismatching start identity represents a dead owner whose PID was reused.
    write(staging/".crackdown-import","crackdown-import-v1\n"+std::to_string(owner.pid)+" "+std::to_string(owner.started+1)+"\n");
    require(unfinished_imports(home)==std::vector<fs::path>{staging},"orphaned marked import found");
    require(unfinished_imports(home,staging/".").empty(),"canonical selected folder excluded");
    write(staging/".crackdown-import","crackdown-import-v1\n0 1\n"); require(unfinished_imports(home).empty(),"zero PID marker excluded");
    fs::remove_all(staging);
    { InstanceLock first(home); rejects([&]{InstanceLock second(home/".");},"duplicate package instance");
      fs::create_directory(home/"another-package"); InstanceLock different(home/"another-package"); }
    { InstanceLock reacquired(home); require(true,"instance lock released"); }
    fs::create_directory(home/"game"); write(home/"game/keep","keep");
    rejects([&]{install_image(home/"test.iso",home,manifest,cancel,{});},"never replace a game folder"); require(text(home/"game/keep")=="keep","owned folder retained");
    auto user=home/"user";
    auto default_args=profile_args(user,"");
    require(list_profiles(user).empty() && default_args==std::vector<std::string>{"--user_data_root="+utf8(user),"--cache_root="+utf8(user/"cache")},"default roots explicit");
    require(profile_args(fs::relative(user),"")==default_args,"relative root normalized to absolute");
    for(const auto& n:{"","..",".hidden","a/b","CON","$SystemUpdate","this name is far too long for a save profile folder"}) require(!valid_profile_name(n),"bad profile name rejected");
    create_profile(user,"Seed 42"); create_profile(user,"alpha");
    rejects([&]{create_profile(user,"seed 42");},"duplicate profile (case-insensitive)");
    require(list_profiles(user)==std::vector<std::string>{"alpha","Seed 42"},"profiles listed in order");
    auto pargs=profile_args(user,"Seed 42");
    require(pargs.size()==2 && pargs[0]=="--user_data_root="+utf8(user/"profiles"/"Seed 42") && pargs[1]=="--cache_root="+utf8(user/"cache"),"profile arguments share the cache");
    rejects([&]{profile_args(user,"missing");},"unknown profile");
    std::error_code link_error;
    fs::create_directory_symlink(user,user/"profiles"/"Linked",link_error);
    if(!link_error) {
      rejects([&]{profile_args(user,"Linked");},"leaf profile symlink rejected");
      auto listed=list_profiles(user); require(std::find(listed.begin(),listed.end(),"Linked")==listed.end(),"linked profile not listed");
      fs::remove(user/"profiles"/"Linked");
      fs::rename(user/"profiles",user/"real-profiles");
      fs::create_directory_symlink(user/"real-profiles",user/"profiles");
      rejects([&]{profile_args(user,"Seed 42");},"parent profile symlink rejected");
      rejects([&]{create_profile(user,"unsafe");},"creation rejects linked parent");
      require(list_profiles(user).empty(),"linked parent not listed");
      fs::remove(user/"profiles"); fs::rename(user/"real-profiles",user/"profiles");
      auto redirected=home/"redirected-user"; fs::create_directory_symlink(user,redirected);
      require(profile_args(redirected,"Seed 42").size()==2,"redirected Documents root allowed");
      fs::remove(redirected);
    } else std::cout<<"SKIP profile links: "<<link_error.message()<<'\n';
    auto seed=profile_folder(user,"Seed 42");
    require(seed==user/"profiles"/"Seed 42" && !load_ap_connection(seed).enabled && ap_binding(seed).empty(),"no AP connection by default");
    save_ap_connection(seed,{true," host.example:38281 ","Player 1","p\"w=1"});
    auto ap=load_ap_connection(seed);
    require(ap.enabled && ap.slot=="Player 1" && ap.password=="p\"w=1","AP connection round trip");
    require(ap_args(ap)==std::vector<std::string>{"--ap_server=host.example:38281","--ap_slot=Player 1"},"AP args trimmed, password not in argv");
    rejects([&]{ap_args({true,"","Player 1",""});},"AP needs a server");
    rejects([&]{ap_args({true,"host:1"," ",""});},"AP needs a slot");
    auto link=parse_ap_uri("archipelago://Player%201:p%40ss:w@archipelago.gg:38281");
    require(link && link->enabled && link->server=="archipelago.gg:38281" && link->slot=="Player 1" && link->password=="p@ss:w","AP room link parsed");
    link=parse_ap_uri("ARCHIPELAGO://Agent@localhost/");
    require(link && link->server=="localhost:38281" && link->slot=="Agent" && link->password.empty(),"AP link default port, no password");
    link=parse_ap_uri("archipelago://host.example:1234");
    require(link && link->server=="host.example:1234" && link->slot.empty() && link->password.empty(),"AP link without slot");
    link=parse_ap_uri("archipelago://Agent@[::1]:38282");
    require(link && link->server=="[::1]:38282","AP link IPv6 host");
    link=parse_ap_uri("archipelago://Agent@[::1]");
    require(link && link->server=="[::1]:38281","AP link IPv6 default port");
    require(!parse_ap_uri("https://archipelago.gg") && !parse_ap_uri("archipelago://") && !parse_ap_uri("archipelago://a@:1") &&
            !parse_ap_uri("archipelago://a@host:x") && !parse_ap_uri("archipelago://a%2@host") && !parse_ap_uri("archipelago://a@ho st") &&
            !parse_ap_uri("archipelago://Player:1234/secret@host:38281") && !parse_ap_uri("archipelago://a@host:65536") &&
            !parse_ap_uri("archipelago://a@host:0") && !parse_ap_uri("archipelago://a:p%00w@host") && !parse_ap_uri("archipelago://a@host:123456") &&
            !parse_ap_uri("archipelago://Agent@ho\nst:38281") && !parse_ap_uri("archipelago://Agent:p@ss@host:38281") &&
            !parse_ap_uri("archipelago://Agent@host:65536]") && !parse_ap_uri("archipelago://a@[::1") && !parse_ap_uri("archipelago://a@[]:1") &&
            !parse_ap_uri("archipelago://a@::1:38281") && !parse_ap_uri("archipelago://a@[::1]x") &&
            !parse_ap_uri("archipelago://Agent@[[::1]:38281") && !parse_ap_uri("archipelago://a@[::g]:1") &&
            !parse_ap_uri("archipelago://Agent:pw@[::::]:38281") && !parse_ap_uri("archipelago://a@[12345::]"),
            "bad AP links rejected");
    require(ap_args({false,"x:1","y","z"}).empty(),"AP off passes no AP args");
    for(const auto& a:ap_args(ap)) require(a.back()!='=',"no empty-valued arg (CLI11 drops the whole command line)");
    { std::ofstream(home/"report.toml")<<"fullscreen = false\nap_password = \"\"\"\nline-two-secret\"\"\"\n\"AP_PassWord\" = 'q'\n[nested]\nroom_password = 'n'\n[[list]]\npassword = 'l'\n"; }
    auto redacted=redacted_config(home/"report.toml");
    require(redacted.find("secret")==std::string::npos && redacted.find("'q'")==std::string::npos && redacted.find("'n'")==std::string::npos &&
            redacted.find("'l'")==std::string::npos && redacted.find("fullscreen = false")!=std::string::npos,"system info redacts every password key");
    { std::ofstream(home/"broken.toml")<<"ap_password = \"unterminated\nsecret2\n"; }
    require(redacted_config(home/"broken.toml").find("secret2")==std::string::npos,"unparsable settings left out");
    { std::ofstream(seed/"archipelago"/"session.toml")<<"format = 2\nseed = \"31315\"\nslot = 2\n"; }
    require(ap_binding(seed)=="seed 31315, slot 2","AP binding shown from the game's sidecar");
    auto unicode_user=home/fs::path(u8"Documents \u00e9"); create_profile(unicode_user,"Seed#1%");
    auto unicode_args=profile_args(unicode_user,"Seed#1%");
    require(unicode_args[0]=="--user_data_root="+utf8(unicode_user/"profiles"/"Seed#1%"),"Unicode save root arguments");
#ifndef _WIN32
    std::string opened; open_folder(unicode_user/"profiles"/"Seed#1%",[&](const char* url){opened=url; return true;});
    require(opened.find("Documents%20%C3%A9/profiles/Seed%231%25")!=std::string::npos,"folder URL percent encoding");
#endif
    auto config=home/"config.toml";
    auto collision=config; collision+=".new."+std::to_string(current_process_owner().pid)+".0"; write(collision,"other writer");
    {
      ConfigTemp first(config),second(config);
      require(first.path()!=collision && first.path()!=second.path(),"exclusive unique temporary files");
      require(text(collision)=="other writer","existing temp never truncated");
      auto second_path=second.path(); first.replace(config,"first");
      require(fs::exists(second_path) && text(config)=="first","replace preserves another writer temp");
      second.replace(config,"second"); require(text(config)=="second","second atomic replace");
    }
    require(text(collision)=="other writer","other temp never removed");
    fs::create_directory(home/"blocked-config");
    fs::path failed_temp;
    { ConfigTemp temp(home/"blocked-config"); failed_temp=temp.path();
      rejects([&]{temp.replace(home/"blocked-config","failure");},"replace errors propagated"); }
    require(!fs::exists(failed_temp) && fs::is_directory(home/"blocked-config"),"only failed writer temp cleaned up");
    write(config,"# retain comment\nfullscreen = false # inline\ncustom = { a = 42 }\n[draw]\nresolution_scale_x = 2\n[unknown]\nvalue = 'literal'\n");
    update_config(config,{{"fullscreen","true"},{"audio_mute","true"},{"draw_resolution_scale_x",std::nullopt}});
    auto cfg=read_config(config); require(setting(cfg,"fullscreen")->value<bool>()==true,"flat TOML setting");
    require(setting(cfg,"audio_mute")->value<bool>()==true,"new root setting"); require(!setting(cfg,"draw_resolution_scale_x"),"nested flat removal");
    require(cfg["custom"]["a"].value<int>()==42,"unknown inline table");
    auto content=text(config); require(content.find("# retain comment")!=content.npos && content.find("# inline")!=content.npos && content.find("value = 'literal'")!=content.npos,"comments and unknown formatting preserved");
    update_config(config,{{"game_data_root",toml_string("C:\\Games\\Crackdown \"test\"")}});
    require(setting(read_config(config),"game_data_root")->value<std::string>()=="C:\\Games\\Crackdown \"test\"","Windows path roundtrip");
    write(config,"# Unicode: \xC3\xA9\nname = '\xC3\xA9'\nfullscreen = false\n"); update_config(config,{{"fullscreen","true"}}); require(setting(read_config(config),"fullscreen")->value<bool>()==true,"UTF8 positions");
    write(config,"game_data_root = 'C:/\xC3\xA9/test' # path\n"); update_config(config,{{"game_data_root",toml_string("C:/new")}}); require(setting(read_config(config),"game_data_root")->value<std::string>()=="C:/new","UTF8 value source range");
    write(config,"fullscreen = false\n[full]\nscreen = false\n"); // different flattened name; not ambiguous
    update_config(config,{{"fullscreen",std::nullopt}}); require(!setting(read_config(config),"fullscreen"),"default removal");
    auto stale=config; stale+=".new"; write(config,"fullscreen = false\n"); write(stale,"stale"); update_config(config,{{"fullscreen","true"}});
    require(setting(read_config(config),"fullscreen")->value<bool>()==true && text(stale)=="stale","unowned temp file preserved while saving");
    write(config,"bad = ["); auto before=text(config); rejects([&]{update_config(config,{{"fullscreen","true"}});},"invalid TOML retained"); require(text(config)==before,"invalid TOML not overwritten");
    // The child records its argument and cwd, then exits early with a known code.
    fs::path child=fs::absolute(argv[0]).parent_path()/
#ifdef _WIN32
      "launcher_test_child.exe";
    fs::copy_file(child,home/"crackdown.exe");
#else
      "launcher_test_child";
    fs::copy_file(child,home/"crackdown");
#endif
    GameProcess process; auto original_cwd=fs::current_path(); auto since=fs::file_time_type::clock::now();
    auto referenced=home/fs::path(u8"game folder \u00e9");
    std::vector<std::string> args{"--game_data_root="+utf8(referenced),"--user_data_root=C:\\trailing slash\\","--cache_root=C:\\embedded \"quote\""};
    environment("REX_USER_DATA_ROOT","unwanted saves"); environment("REX_CACHE_ROOT","unwanted cache");
    environment("LAUNCHER_TEST_KEEP","preserved");
    process.start(home,args);
    rejects([&]{process.start(home,args);},"duplicate process");
    std::optional<int> code;
    for(unsigned i=0;i<10000 && !code;++i) { code=process.poll(); if(!code) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    require(code && *code==7,"child exit code"); require(fs::current_path()==original_cwd,"parent cwd preserved");
    require(text(home/"received-argument.txt")==args[0]+"|"+args[1]+"|"+args[2]+"|","three arguments, Unicode, trailing backslash and quote");
    require(text(home/"received-environment.txt")=="||preserved","child root overrides removed, other environment retained");
    require(std::string(std::getenv("REX_USER_DATA_ROOT"))=="unwanted saves","parent environment unchanged");
    auto dead_identity=text(home/"received-owner.txt");
    fs::create_directory(staging); write(staging/".crackdown-import","crackdown-import-v1\n"+dead_identity);
    require(unfinished_imports(home)==std::vector<fs::path>{staging},"exited owner import found");
    require(unfinished_imports(home,staging).empty(),"selected exited owner import protected");
    fs::remove_all(staging);
    environment("LAUNCHER_TEST_DELAY","1"); process.start(home,args); code.reset();
    for(unsigned i=0;i<1500 && !code;++i) { code=process.poll(); if(!code) std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    require(code && *code==7 && process.elapsed()>10,"later nonzero child exit");
    for(auto name:{"REX_USER_DATA_ROOT","REX_CACHE_ROOT","LAUNCHER_TEST_KEEP","LAUNCHER_TEST_DELAY"}) environment(name,nullptr);
    require(newest_log_tail(home,since).find("stub startup log")!=std::string::npos,"early-exit log tail");
    require(newest_log_tail(home,fs::file_time_type::clock::now()+std::chrono::seconds(1))=="No new game log was created.","old logs excluded");
    fs::remove_all(home); std::cout<<checks<<" checks passed\n"; return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<" (after "<<checks<<" checks); fixture: "<<utf8(home)<<'\n'; return 1; }
}
