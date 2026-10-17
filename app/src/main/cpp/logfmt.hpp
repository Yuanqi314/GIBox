#pragma once
// Log file in the XXMI Launcher format (src/xxmi_launcher/app.py):
//   '%(asctime)s {instance_id:06} %(name)s %(levelname)s %(message)s'
//   2026-10-17 21:03:12,345 482113 runtime INFO attached Mod/Part0 -> Body
// instance_id = int(time.time()*1000) % 1000000, once per process, so lines of one game session can be told apart.
// The game (runtime, bootstrap) and the app (importer, converter) append to the same file.
#include "json.hpp"
#include <chrono>
#include <cstdio>
#include <ctime>
namespace ms {
enum class Level:int{Debug=10,Info=20,Warning=30,Error=40};
inline constexpr int logDisabled=1000;
inline constexpr const char* logFileName="GIBox Log.txt";
inline const char* levelName(Level l){switch(l){case Level::Debug:return "DEBUG";case Level::Info:return "INFO";case Level::Warning:return "WARNING";default:return "ERROR";}}
inline unsigned logInstance(){static const unsigned id=unsigned(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()%1000000);return id;}
inline std::string logLine(const char* logger,Level level,const std::string& message,std::chrono::system_clock::time_point at=std::chrono::system_clock::now()){
 auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();std::time_t sec=std::time_t(ms/1000);std::tm t{};localtime_r(&sec,&t);
 char head[64];std::snprintf(head,sizeof(head),"%04d-%02d-%02d %02d:%02d:%02d,%03d %06u ",t.tm_year+1900,t.tm_mon+1,t.tm_mday,t.tm_hour,t.tm_min,t.tm_sec,int(ms%1000),logInstance());
 std::string body=message;for(auto& c:body)if(c=='\r')c=' ';
 // Multi-line details (class dumps) stay readable: continuation lines are indented under the header.
 std::string out=std::string(head)+logger+' '+levelName(level)+' ';for(char c:body){out+=c;if(c=='\n')out+="    ";}return out+'\n';}
// Runtime state keys (the first word of a message) by level. Everything not listed is INFO.
inline Level levelOf(const std::string& state){
 auto ends=[&](const char* s){size_t n=std::strlen(s);return state.size()>=n&&state.compare(state.size()-n,n,s)==0;};
 if(state=="error"||state=="buildid-mismatch"||ends("-error"))return Level::Error;
 for(const char* w:{"find-disabled","bind-incompatible","restore-materials-dead","hook-skip","attach-skip","attachment-dropped","renderer-type-dump","target-missing","handoff-type-wait","bone-export-skip","build-retry","world-renderers","bone-library-partial"})if(state==w)return Level::Warning;
 if(ends("-disabled"))return Level::Warning;
 for(const char* d:{"bind-passed","discover-skip","direct-mesh-prepared","handoff-type","array-classes","bone-library-unchanged","bone-library-wait"})if(state==d)return Level::Debug;
 return Level::Info;}
// control.json "logLevel": disabled / warning / info / debug (the d3dx.ini [Logging] values XXMI writes). Default info.
inline int logThreshold(const Json& control){const auto& v=control.at("logLevel");if(v.type!=Json::String)return int(Level::Info);std::string s;for(char c:v.string)s+=char(std::tolower(static_cast<unsigned char>(c)));
 if(s=="disabled"){return logDisabled;}if(s=="warning"){return int(Level::Warning);}if(s=="debug"){return int(Level::Debug);}return int(Level::Info);}
inline int logThresholdOf(const std::string& sharedDir){try{return logThreshold(JsonParser(text(sharedDir+"/control.json")).parse());}catch(const std::exception&){return int(Level::Info);}}
// Appends one line; the file is moved to "GIBox Log.txt.1" past 2 MB.
inline void appendLog(const std::string& sharedDir,const std::string& line){std::error_code ec;auto file=sharedDir+"/"+logFileName;
 if(std::filesystem::file_size(file,ec)>2*1024*1024&&!ec){std::filesystem::rename(file,file+".1",ec);}std::ofstream f(file,std::ios::app);f<<line;}
}
