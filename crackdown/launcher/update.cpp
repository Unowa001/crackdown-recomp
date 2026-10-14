#include "v2.h"
#include "platform.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#endif
namespace launcher {
namespace {
[[noreturn]] void fail(const std::string& s) { throw std::runtime_error(s); }
struct Version { std::vector<uint64_t> numbers; std::vector<std::string> pre; };
uint64_t numeric(const std::string& s) {
  if(s.empty() || (s.size()>1 && s[0]=='0')) fail("Invalid version number");
  uint64_t n=0; auto [p,e]=std::from_chars(s.data(),s.data()+s.size(),n); if(e!=std::errc{}||p!=s.data()+s.size()) fail("Invalid version number"); return n;
}
std::vector<std::string> split(const std::string& s,char sep) {
  std::vector<std::string> r; size_t p=0;
  for(;;) { auto e=s.find(sep,p); r.push_back(s.substr(p,e==s.npos?e:e-p)); if(e==s.npos) break; p=e+1; } return r;
}
bool digits(const std::string& s) { return !s.empty()&&std::all_of(s.begin(),s.end(),[](unsigned char c){return c>='0'&&c<='9';}); }
Version version(std::string s) {
  if(!s.empty() && s[0]=='v') s.erase(0,1);
  auto plus=s.find('+');
  auto identifiers=[](const std::string& ids) { auto r=split(ids,'.'); for(const auto& id:r) if(id.empty() || !std::all_of(id.begin(),id.end(),[](unsigned char c){return std::isalnum(c)||c=='-';})) fail("Invalid version identifier"); return r; };
  if(plus!=s.npos) { (void)identifiers(s.substr(plus+1)); s.resize(plus); }
  Version v; auto dash=s.find('-');
  if(dash!=s.npos) { v.pre=identifiers(s.substr(dash+1)); for(const auto& id:v.pre) if(digits(id)) (void)numeric(id); s.resize(dash); }
  for(const auto& part:split(s,'.')) v.numbers.push_back(numeric(part));
  if(v.numbers.size()!=3) fail("Release tags must use SemVer, for example v0.2.0"); return v;
}
// Small strict JSON reader for GitHub metadata. Bounded response and nesting, no dependency.
struct Json { char type='n'; std::string text; std::map<std::string,Json> object; std::vector<Json> array; };
class JsonReader {
  const std::string& s; size_t p=0;
  void ws() { while(p<s.size() && (s[p]==' '||s[p]=='\n'||s[p]=='\r'||s[p]=='\t')) ++p; }
  char take() { if(p==s.size()) fail("Truncated release JSON"); return s[p++]; }
  bool accept(char c) { ws(); if(p<s.size()&&s[p]==c) { ++p; return true; } return false; }
  unsigned hex() { unsigned n=0; for(int i=0;i<4;++i) { char c=take(); n<<=4; if(c>='0'&&c<='9') n+=c-'0'; else if(c>='a'&&c<='f') n+=c-'a'+10; else if(c>='A'&&c<='F') n+=c-'A'+10; else fail("Invalid JSON escape"); } return n; }
  std::string string() {
    if(take()!='"') fail("Expected JSON string"); std::string out;
    for(;;) { unsigned char c=take(); if(c=='"') return out; if(c<32) fail("Invalid JSON string");
      if(c!='\\') { out+=char(c); continue; } c=take();
      if(c=='"'||c=='\\'||c=='/') out+=char(c);
      else if(c=='n') out+='\n'; else if(c=='r') out+='\r'; else if(c=='t') out+='\t'; else if(c=='b') out+='\b'; else if(c=='f') out+='\f';
      else if(c=='u') {
        unsigned cp=hex(); if(cp>=0xd800&&cp<=0xdbff) { if(take()!='\\'||take()!='u') fail("Invalid JSON surrogate"); unsigned lo=hex(); if(lo<0xdc00||lo>0xdfff) fail("Invalid JSON surrogate"); cp=0x10000+((cp-0xd800)<<10)+lo-0xdc00; }
        else if(cp>=0xdc00&&cp<=0xdfff) fail("Invalid JSON surrogate");
        if(cp==0) fail("NUL in release metadata");
        if(cp<128) out+=char(cp); else if(cp<2048) { out+=char(0xc0|(cp>>6)); out+=char(0x80|(cp&63)); }
        else if(cp<65536) { out+=char(0xe0|(cp>>12)); out+=char(0x80|((cp>>6)&63)); out+=char(0x80|(cp&63)); }
        else { out+=char(0xf0|(cp>>18)); out+=char(0x80|((cp>>12)&63)); out+=char(0x80|((cp>>6)&63)); out+=char(0x80|(cp&63)); }
      } else fail("Invalid JSON escape");
    }
  }
  Json parse(unsigned depth) {
    if(depth>32) fail("Release JSON nesting limit"); ws(); if(p==s.size()) fail("Empty JSON"); Json v;
    if(accept('{')) { v.type='o'; if(accept('}')) return v; do { ws(); auto k=string(); if(!accept(':')) fail("Invalid JSON object"); if(!v.object.emplace(k,parse(depth+1)).second) fail("Duplicate JSON key"); } while(accept(',')); if(!accept('}')) fail("Invalid JSON object"); }
    else if(accept('[')) { v.type='a'; if(accept(']')) return v; do { v.array.push_back(parse(depth+1)); } while(accept(',')); if(!accept(']')) fail("Invalid JSON array"); }
    else if(s[p]=='"') { v.type='s'; v.text=string(); }
    else { size_t start=p; while(p<s.size() && s[p]!=','&&s[p]!=']'&&s[p]!='}' && !std::isspace(static_cast<unsigned char>(s[p]))) ++p; v.text=s.substr(start,p-start);
      if(v.text=="true"||v.text=="false") v.type='b'; else if(v.text=="null") v.type='n';
      else { // GitHub also has numeric ids; validate JSON number syntax with from_chars.
        size_t q=0; auto digit=[&]{return q<v.text.size()&&v.text[q]>='0'&&v.text[q]<='9';};
        if(q<v.text.size()&&v.text[q]=='-') ++q;
        if(!digit()) fail("Invalid JSON number");
        if(v.text[q]=='0') ++q; else while(digit()) ++q;
        if(q<v.text.size()&&v.text[q]=='.') { ++q; if(!digit()) fail("Invalid JSON fraction"); while(digit()) ++q; }
        if(q<v.text.size()&&(v.text[q]=='e'||v.text[q]=='E')) { ++q; if(q<v.text.size()&&(v.text[q]=='+'||v.text[q]=='-')) ++q; if(!digit()) fail("Invalid JSON exponent"); while(digit()) ++q; }
        if(q!=v.text.size()) fail("Invalid JSON value"); v.type='d'; }
    } return v;
  }
 public:
  explicit JsonReader(const std::string& text):s(text) {}
  Json read() { if(s.size()>4*1024*1024) fail("Release metadata size limit"); auto v=parse(0); ws(); if(p!=s.size()) fail("Trailing JSON data"); return v; }
};
std::string jstr(const Json& j,const char* key) { auto it=j.object.find(key); return it!=j.object.end() && it->second.type=='s'?it->second.text:""; }
bool jbool(const Json& j,const char* key) { auto it=j.object.find(key); return it!=j.object.end()&&it->second.type=='b'&&it->second.text=="true"; }
bool linked(const fs::path& p) {
#ifdef _WIN32
  auto a=GetFileAttributesW(p.c_str()); return a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT);
#else
  return fs::is_symlink(fs::symlink_status(p));
#endif
}
void plain_path(const fs::path& root,const fs::path& relative) {
  auto path=root; if(linked(path)) fail("Linked update root is not allowed");
  for(const auto& part:relative) { path/=part; if(linked(path)) fail("Linked update path is not allowed: "+utf8(path)); }
}
std::vector<std::string> staged_files(const fs::path& stage) {
  if(!fs::is_directory(stage)||linked(stage)) fail("Invalid update staging folder"); std::vector<std::string> files; std::set<std::string> seen;
  for(const auto& item:fs::recursive_directory_iterator(stage)) {
    if(linked(item.path())) fail("Linked update input"); if(item.is_directory()) continue;
    // Package inventories and journals use '/' on every host. Native Windows
    // path separators would otherwise reject all nested assets in the allowlist.
    auto generic=item.path().lexically_relative(stage).generic_u8string();
    std::string rel(generic.begin(),generic.end()); std::string lower=rel; std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(std::tolower(c));});
    if(!item.is_regular_file()||!update_path_allowed(rel)||!seen.insert(lower).second) fail("Forbidden update file: "+rel); files.push_back(rel);
  }
  for(const auto& required:{"CrackdownLauncher.exe","crackdown.exe","game-manifest.txt","xui/Scene.xpatch","xui/menuHUD_3.xpatch"}) if(std::find(files.begin(),files.end(),required)==files.end()) fail("Incomplete update package");
  std::sort(files.begin(),files.end()); return files;
}
}
int compare_versions(const std::string& a,const std::string& b) {
  auto x=version(a),y=version(b); if(x.numbers!=y.numbers) return x.numbers<y.numbers?-1:1;
  if(x.pre.empty()&&y.pre.empty()) return 0; if(x.pre.empty()) return 1; if(y.pre.empty()) return -1;
  for(size_t i=0;i<std::min(x.pre.size(),y.pre.size());++i) {
    const auto& u=x.pre[i]; const auto& v=y.pre[i]; if(u==v) continue;
    bool un=digits(u),vn=digits(v); if(un&&vn) return numeric(u)<numeric(v)?-1:1; if(un!=vn) return un?-1:1; return u<v?-1:1;
  } return x.pre.size()==y.pre.size()?0:x.pre.size()<y.pre.size()?-1:1;
}
std::vector<Release> parse_releases(const std::string& json) {
  auto root=JsonReader(json).read(); std::vector<Json> objects;
  if(root.type=='a') objects=std::move(root.array); else if(root.type=='o') objects.push_back(std::move(root)); else fail("Invalid release response");
  std::vector<Release> releases;
  for(const auto& j:objects) {
    if(j.type!='o'||jbool(j,"draft")) continue;
    Release r; r.tag=jstr(j,"tag_name"); if(r.tag.empty()) fail("GitHub returned no release tag");
    (void)version(r.tag); r.notes=jstr(j,"body"); r.url=jstr(j,"html_url"); r.prerelease=jbool(j,"prerelease");
    auto assets=j.object.find("assets");
    if(assets!=j.object.end()&&assets->second.type=='a') {
      std::map<std::string,std::string> urls;
      for(const auto& a:assets->second.array) { auto name=jstr(a,"name"); if(!urls.emplace(name,jstr(a,"browser_download_url")).second) fail("Duplicate release asset"); }
      for(const auto& [name,url]:urls) if(name.starts_with("CrackdownRecomp-")&&name.ends_with(".zip")&&urls.contains(name+".sha256")) {
        if(!r.zip_url.empty()) fail("Release has multiple package ZIPs"); r.zip_url=url; r.sha_url=urls.at(name+".sha256");
      }
    }
    releases.push_back(std::move(r));
  }
  std::sort(releases.begin(),releases.end(),[](const auto& a,const auto& b){return compare_versions(a.tag,b.tag)>0;}); return releases;
}
void verify_download(const fs::path& zip,const std::string& checksum) {
  std::string hash=checksum.substr(0,64);
  if(hash.size()!=64||!std::all_of(hash.begin(),hash.end(),[](unsigned char c){return std::isxdigit(c);}) || (checksum.size()>64 && !std::isspace(static_cast<unsigned char>(checksum[64])))) fail("Invalid SHA256 asset");
  std::transform(hash.begin(),hash.end(),hash.begin(),[](unsigned char c){return char(std::tolower(c));});
  if(hash_file(zip)!=hash) fail("Update ZIP failed SHA256 verification");
}
bool update_path_allowed(const std::string& relative) {
  if(relative.empty()||relative.find('\\')!=relative.npos) return false; auto parts=split(relative,'/');
  for(const auto& p:parts) if(!safe_name(p)) return false;
  if(parts.size()==1) { static const std::set<std::string> names={"CrackdownLauncher.exe","crackdown.exe","rexruntime.dll","rexgpu-xenos.dll","TracyClient.dll","game-manifest.txt","README.txt","VERSION.txt"}; return names.contains(relative); }
  if(parts.size()==2&&parts[0]=="LICENSES") return parts[1].ends_with(".txt");
  if(parts.size()==2&&parts[0]=="xui") return parts[1]=="Scene.xpatch"||parts[1]=="menuHUD_3.xpatch";
  if(parts.size()==3&&parts[0]=="assets"&&parts[1]=="fonts") { static const std::set<std::string> fonts={"Barlow-Regular.ttf","Barlow-SemiBold.ttf","BarlowCondensed-SemiBold.ttf","BarlowCondensed-Bold.ttf","OFL.txt"}; return fonts.contains(parts[2]); }
  if(parts.size()==3&&parts[0]=="assets"&&parts[1]=="glyphs") return parts[2]=="LICENSE.txt";
  return parts.size()==4&&parts[0]=="assets"&&parts[1]=="glyphs"&&(parts[2]=="ps5"||parts[2]=="xbox_series"||parts[2]=="keyboard")&&parts[3].ends_with(".png");
}
void recover_update(const fs::path& package) {
  auto backup=package/".launcher-update-backup";
  auto cleanup_exes=[&] { std::error_code ec; fs::remove(package/"CrackdownLauncher.old.exe",ec); fs::remove(package/"CrackdownLauncher.failed.exe",ec); };
  if(!fs::exists(backup)) { cleanup_exes(); return; }
  plain_path(package,".launcher-update-backup/journal.toml");
  if(!fs::exists(backup/"journal.toml")&&!fs::exists(backup/"completed.toml")) fail("Unmarked update backup requires manual inspection");
  if(fs::exists(backup/"journal.toml")) {
    auto journal=read_config(backup/"journal.toml"); if(journal["format"].value<int>()!=1) fail("Invalid update rollback journal");
    auto entries=journal["files"].as_array(); if(!entries) fail("Invalid update rollback journal");
    std::vector<std::pair<std::string,bool>> files; std::set<std::string> seen;
    for(const auto& e:*entries) {
      auto t=e.as_table(); if(!t) fail("Invalid rollback entry"); auto name=(*t)["path"].value<std::string>(); auto existed=(*t)["existed"].value<bool>();
      if(!name||!existed||!update_path_allowed(*name)||!seen.insert(*name).second) fail("Forbidden rollback path");
      plain_path(package,fs::u8path(*name)); plain_path(backup,fs::u8path(*name));
      if(*existed&&!fs::is_regular_file(backup/fs::u8path(*name))) fail("Missing rollback file"); files.emplace_back(*name,*existed);
    }
    // On Windows the current image can be renamed but cannot be overwritten while mapped.
    // This also permits recovery after an interruption once the new launcher was copied.
    auto exe=package/"CrackdownLauncher.exe",failed=package/"CrackdownLauncher.failed.exe";
    if(seen.contains("CrackdownLauncher.exe")&&fs::exists(exe)) {
      plain_path(package,"CrackdownLauncher.failed.exe"); if(fs::exists(failed)) fs::remove(failed); fs::rename(exe,failed);
    }
    for(const auto& [name,existed]:files) {
      auto target=package/fs::u8path(name);
      if(existed) { fs::create_directories(target.parent_path()); fs::copy_file(backup/fs::u8path(name),target,fs::copy_options::overwrite_existing); }
      else fs::remove(target);
    }
  } else {
    plain_path(package,".launcher-update-backup/completed.toml");
    if(read_config(backup/"completed.toml")["format"].value<int>()!=1) fail("Invalid completed update marker");
  }
  fs::remove_all(backup); cleanup_exes();
}
void replace_staged(const fs::path& package,const fs::path& staging,const std::function<void(size_t)>& before_copy) {
  auto files=staged_files(staging); auto backup=package/".launcher-update-backup";
  if(fs::exists(backup)||fs::exists(package/"CrackdownLauncher.old.exe")) fail("An earlier update needs recovery; restart the launcher");
  for(const auto& name:files) { plain_path(package,fs::u8path(name)); if(fs::exists(package/fs::u8path(name))&&!fs::is_regular_file(package/fs::u8path(name))) fail("Update destination is not a file"); }
  if(!fs::create_directory(backup)) fail("Cannot create update backup");
  bool journal_written=false;
  try {
    std::string journal="format = 1\n";
    for(const auto& name:files) {
      auto source=package/fs::u8path(name), saved=backup/fs::u8path(name); bool exists=fs::exists(source);
      journal+="[[files]]\npath = "+toml_string(name)+"\nexisted = "+(exists?"true":"false")+"\n";
      if(exists) { fs::create_directories(saved.parent_path()); fs::copy_file(source,saved); }
    }
    ConfigTemp marker(backup/"journal.toml"); marker.replace(backup/"journal.toml",journal); journal_written=true;
    auto exe=package/"CrackdownLauncher.exe"; if(fs::exists(exe)) fs::rename(exe,package/"CrackdownLauncher.old.exe");
    for(size_t i=0;i<files.size();++i) {
      if(before_copy) before_copy(i); auto target=package/fs::u8path(files[i]); fs::create_directories(target.parent_path());
      // Sibling atomic replacement avoids partially written DLLs/files.
      auto bytes=read_bytes(staging/fs::u8path(files[i]),1024ull*1024*1024);
      ConfigTemp temporary(target); temporary.replace(target,std::string(bytes.begin(),bytes.end()));
    }
    fs::rename(backup/"journal.toml",backup/"completed.toml"); // Commit point. Retain rollback until restart succeeds.
  } catch(...) { if(journal_written) recover_update(package); else { std::error_code ec; fs::remove_all(backup,ec); } throw; }
}
#ifdef _WIN32
namespace {
struct Internet { HINTERNET h=nullptr; explicit Internet(HINTERNET handle):h(handle) { if(!h) fail("WinHTTP failed: "+std::to_string(GetLastError())); } ~Internet(){WinHttpCloseHandle(h);} };
std::wstring wide(const std::string& s) { auto p=fs::u8path(s); return p.native(); }
// Redirects are explicit so HTTPS and the GitHub host restriction apply at every hop.
Bytes request(std::string url,uint64_t limit,const std::atomic_bool& cancel) {
  Internet session(WinHttpOpen(L"CrackdownLauncher/" LAUNCHER_VERSION_W,WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0));
  WinHttpSetTimeouts(session.h,10000,10000,15000,15000);
  for(int redirect=0;redirect<6;++redirect) {
    if(cancel) fail("Update cancelled"); auto w=wide(url); URL_COMPONENTS u{}; u.dwStructSize=sizeof(u); u.dwHostNameLength=u.dwUrlPathLength=u.dwExtraInfoLength=DWORD(-1);
    if(!WinHttpCrackUrl(w.c_str(),0,0,&u)||u.nScheme!=INTERNET_SCHEME_HTTPS||u.nPort!=443) fail("Update URL must use HTTPS");
    std::wstring host(u.lpszHostName,u.dwHostNameLength),path(u.lpszUrlPath,u.dwUrlPathLength); if(u.dwExtraInfoLength) path.append(u.lpszExtraInfo,u.dwExtraInfoLength);
    if(host!=L"api.github.com"&&host!=L"github.com"&&host!=L"release-assets.githubusercontent.com"&&host!=L"objects.githubusercontent.com") fail("Untrusted update host");
    Internet connection(WinHttpConnect(session.h,host.c_str(),443,0));
    Internet req(WinHttpOpenRequest(connection.h,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE));
    DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_NEVER; if(!WinHttpSetOption(req.h,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy))) fail("Cannot restrict redirects");
    const wchar_t* headers=L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if(!WinHttpSendRequest(req.h,headers,DWORD(-1),WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(req.h,nullptr)) fail("GitHub request failed: "+std::to_string(GetLastError()));
    DWORD status=0,size=sizeof(status); if(!WinHttpQueryHeaders(req.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)) fail("Cannot read HTTP status");
    if(status>=300&&status<400) {
      wchar_t location[16384]; size=sizeof(location);
      if(!WinHttpQueryHeaders(req.h,WINHTTP_QUERY_LOCATION,WINHTTP_HEADER_NAME_BY_INDEX,location,&size,WINHTTP_NO_HEADER_INDEX)) fail("Invalid update redirect");
      url=utf8(fs::path(location)); continue;
    }
    if(status!=200) fail("GitHub HTTP "+std::to_string(status)+" (repository may be private or unconfirmed)");
    Bytes bytes; unsigned char chunk[65536]; DWORD n=0;
    do { if(cancel) fail("Update cancelled"); if(!WinHttpReadData(req.h,chunk,sizeof(chunk),&n)) fail("Update download failed");
      if(n>limit-bytes.size()) fail("Update download size limit"); bytes.insert(bytes.end(),chunk,chunk+n);
    } while(n); return bytes;
  } fail("Too many update redirects");
}
std::string ps_literal(const fs::path& p) { std::string s=utf8(p),r="'"; for(char c:s) { r+=c; if(c=='\'') r+=c; } return r+"'"; }
void extract_zip(const fs::path& zip,const fs::path& staging,const std::atomic_bool& cancel) {
  // .NET validates/decompresses ZIPs; our script validates every name before writing anything.
  // No external archive executable/dependency ships with the launcher.
  auto script=staging.parent_path()/"extract.ps1";
  std::string text="\xEF\xBB\xBF$ErrorActionPreference = 'Stop'\nAdd-Type -AssemblyName System.IO.Compression.FileSystem\n";
  text+="$zip = [IO.Compression.ZipFile]::OpenRead("+ps_literal(zip)+")\n$stage = "+ps_literal(staging)+"\ntry {\n$items = @()\n$seen = @{}\n$root = $null\n$total = 0L\nforeach ($e in $zip.Entries) {\n";
  text+=R"PS($name = $e.FullName
if ($name -match '[\\:]|^/|(^|/)\.\.?(/|$)' -or $name -match '[\x00-\x1f]') { throw 'Unsafe ZIP path' }
$parts = $name.Split('/')
if ($parts.Count -lt 2 -or -not $parts[0].StartsWith('CrackdownRecomp-')) { throw 'ZIP must contain one package directory' }
if ($null -eq $root) { $root = $parts[0] }
if ($root -cne $parts[0]) { throw 'Multiple ZIP roots' }
if ((($e.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw 'ZIP symlink' }
if ($name.EndsWith('/')) { continue }
$relative = $name.Substring($root.Length + 1)
foreach ($part in $relative.Split('/')) {
  if ($part -eq '' -or $part -match '[<>:"|?*\\]' -or $part.EndsWith('.') -or $part.EndsWith(' ') -or $part -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\.|$)') { throw 'Unsafe ZIP name' }
}
$allowed = $relative -cmatch '^(CrackdownLauncher\.exe|crackdown\.exe|rexruntime\.dll|rexgpu-xenos\.dll|TracyClient\.dll|game-manifest\.txt|README\.txt|VERSION\.txt)$'
$allowed = $allowed -or $relative -cmatch '^LICENSES/[^/]+\.txt$'
$allowed = $allowed -or $relative -cmatch '^xui/(Scene|menuHUD_3)\.xpatch$'
$allowed = $allowed -or $relative -cmatch '^assets/fonts/(Barlow-Regular\.ttf|Barlow-SemiBold\.ttf|BarlowCondensed-SemiBold\.ttf|BarlowCondensed-Bold\.ttf|OFL\.txt)$'
$allowed = $allowed -or $relative -ceq 'assets/glyphs/LICENSE.txt'
$allowed = $allowed -or $relative -cmatch '^assets/glyphs/(ps5|xbox_series|keyboard)/[^/]+\.png$'
if (-not $allowed -or $seen.ContainsKey($relative)) { throw "Forbidden or duplicate ZIP file: $relative" }
$seen[$relative] = $true
$total += $e.Length
if ($e.Length -gt 1073741824 -or $total -gt 2147483648) { throw 'Uncompressed ZIP size limit' }
$items += @{ Entry = $e; Relative = $relative }
}
foreach ($item in $items) {
  $target = Join-Path $stage $item.Relative
  [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
  [IO.Compression.ZipFileExtensions]::ExtractToFile($item.Entry, $target, $false)
}
} finally { $zip.Dispose() }
)PS";
  write_bytes(script,Bytes(text.begin(),text.end()));
  wchar_t system[MAX_PATH]; if(!GetSystemDirectoryW(system,MAX_PATH)) fail("Cannot find PowerShell");
  fs::path powershell=fs::path(system)/L"WindowsPowerShell/v1.0/powershell.exe";
  // Script path is controlled by the launcher, with no user-supplied command text.
  std::wstring command=L"\""+powershell.native()+L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""+script.native()+L"\"";
  STARTUPINFOW info{}; info.cb=sizeof(info); PROCESS_INFORMATION child{};
  if(!CreateProcessW(powershell.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&info,&child)) fail("Cannot start ZIP extraction");
  CloseHandle(child.hThread);
  while(WaitForSingleObject(child.hProcess,100)==WAIT_TIMEOUT) if(cancel) { TerminateProcess(child.hProcess,1); WaitForSingleObject(child.hProcess,INFINITE); CloseHandle(child.hProcess); fail("Update cancelled"); }
  DWORD code=1; GetExitCodeProcess(child.hProcess,&code); CloseHandle(child.hProcess);
  if(code!=0) fail("ZIP extraction rejected the package (exit "+std::to_string(code)+")");
}
}
#endif
std::vector<Release> check_releases(bool beta,const std::atomic_bool& cancel) {
#ifdef _WIN32
  std::string endpoint="https://api.github.com/repos/"+std::string(release_repository)+"/releases";
  // Stable endpoint first, then fetch history. Beta uses the release list.
  if(!beta) { auto latest=request(endpoint+"/latest",4*1024*1024,cancel); (void)parse_releases(std::string(latest.begin(),latest.end())); }
  auto bytes=request(endpoint+"?per_page=20",4*1024*1024,cancel); auto releases=parse_releases(std::string(bytes.begin(),bytes.end()));
  if(!beta) std::erase_if(releases,[](const auto& r){return r.prerelease;}); return releases;
#else
  (void)beta; (void)cancel; fail("Update checks are available on Windows.");
#endif
}
void install_release(const Release& release,const fs::path& package,const std::atomic_bool& cancel) {
#ifdef _WIN32
  auto prefix="https://github.com/"+std::string(release_repository)+"/releases/download/";
  if(!release.zip_url.starts_with(prefix)||!release.sha_url.starts_with(prefix)) fail("Release assets do not belong to the configured repository");
  auto scratch=package/("launcher-update-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
  if(!fs::create_directory(scratch)) fail("Cannot create update staging directory");
  try {
    auto checksum=request(release.sha_url,8192,cancel); auto bytes=request(release.zip_url,1024ull*1024*1024,cancel);
    auto zip=scratch/"package.zip"; write_bytes(zip,bytes); bytes.clear(); bytes.shrink_to_fit();
    verify_download(zip,std::string(checksum.begin(),checksum.end()));
    fs::create_directory(scratch/"package"); extract_zip(zip,scratch/"package",cancel);
    if(cancel) fail("Update cancelled"); replace_staged(package,scratch/"package",[&](size_t){if(cancel) fail("Update cancelled");});
    std::error_code cleanup_error; fs::remove_all(scratch,cleanup_error);
  } catch(...) { std::error_code ec; fs::remove_all(scratch,ec); throw; }
#else
  (void)release; (void)package; (void)cancel; fail("Installing updates is available on Windows.");
#endif
}
void rollback_completed_update(const fs::path& package) {
  auto backup=package/".launcher-update-backup";
  if(fs::exists(backup/"completed.toml")) { fs::rename(backup/"completed.toml",backup/"journal.toml"); recover_update(package); }
}
void restart_launcher(const fs::path& package) {
#ifdef _WIN32
  auto exe=package/L"CrackdownLauncher.exe";
  std::wstring command=L"\""+exe.native()+L"\" --updated";
  STARTUPINFOW info{}; info.cb=sizeof(info); PROCESS_INFORMATION child{};
  if(!CreateProcessW(exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,package.c_str(),&info,&child)) fail("Update installed, but restart failed. Close and open the launcher again.");
  CloseHandle(child.hThread); CloseHandle(child.hProcess);
#else
  (void)package; fail("Restart after updating is available on Windows.");
#endif
}
}
