#include "platform.h"
#include <cstdlib>
#include <thread>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
int main(int argc,char** argv) {
  std::filesystem::create_directory("logs");
  std::ofstream("logs/crackdown_001.log")<<"stub startup log\n";
  // Records every argument, one per line.
  std::string arg; for(int i=1;i<argc;++i) arg+=std::string(argv[i])+"|";
#ifdef _WIN32
  int count=0; auto wide=CommandLineToArgvW(GetCommandLineW(),&count);
  if(wide) { arg.clear(); for(int i=1;i<count;++i) { auto s=std::filesystem::path(wide[i]).u8string(); arg.append(s.begin(),s.end()); arg+="|"; } }
  LocalFree(wide);
#endif
  std::ofstream("received-argument.txt",std::ios::binary)<<arg;
  auto env=[](const char* name) { auto value=std::getenv(name); return value?value:""; };
  std::ofstream("received-environment.txt")<<env("REX_USER_DATA_ROOT")<<'|'<<env("REX_CACHE_ROOT")<<'|'<<env("LAUNCHER_TEST_KEEP");
  auto owner=launcher::current_process_owner(); std::ofstream("received-owner.txt")<<owner.pid<<' '<<owner.started<<'\n';
  if(std::getenv("LAUNCHER_TEST_DELAY")) std::this_thread::sleep_for(std::chrono::milliseconds(10100));
  return 7;
}
