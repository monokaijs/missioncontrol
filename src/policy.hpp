#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mc {
using json = nlohmann::json;
enum class Feature { Status, Capture, Windows, Audio, Lock, Power, Shell, Download, Upload };
inline const std::vector<std::pair<Feature, const char*>> features = {
 {Feature::Status,"System status"},{Feature::Capture,"Screenshots"},{Feature::Windows,"List / close windows"},
 {Feature::Audio,"Volume / mute"},{Feature::Lock,"Lock workstation"},{Feature::Power,"Shutdown / restart"},
 {Feature::Shell,"Shell commands (full account access)"},{Feature::Download,"Files: PC to Telegram"},{Feature::Upload,"Files: Telegram to PC"}};
inline std::string key(Feature f) { return std::to_string(static_cast<int>(f)); }
struct Config {
 std::string token;
 std::vector<int64_t> users, chats;
 std::vector<std::string> roots;
 std::map<Feature,bool> permissions;
 bool enabled(Feature f) const { auto i=permissions.find(f); return i!=permissions.end() && i->second; }
 bool authorized(int64_t user,int64_t chat,const std::string& type,bool bot=false) const {
  return !bot && user>0 && chat>0 && type=="private" &&
   std::find(users.begin(),users.end(),user)!=users.end() && std::find(chats.begin(),chats.end(),chat)!=chats.end();
 }
};
struct Identity { int64_t user{},chat{}; };
inline std::optional<Identity> identity(const json& update,const Config& c) {
 try {
  const json* m=nullptr; const json* from=nullptr;
  if(update.contains("callback_query")) {
   const auto& q=update.at("callback_query");
   if(q.contains("inline_message_id") || !q.contains("message") || q.at("message").value("date",int64_t(0))==0) return {};
   m=&q.at("message"); from=&q.at("from");
  } else if(update.contains("message")) { m=&update.at("message"); from=&m->at("from"); }
  else return {};
  auto u=from->at("id").get<int64_t>(); auto ch=m->at("chat").at("id").get<int64_t>();
  if(!c.authorized(u,ch,m->at("chat").at("type").get<std::string>(),from->value("is_bot",true))) return {};
  return Identity{u,ch};
 } catch(...) { return {}; }
}
struct Pending { Identity who; std::string action; std::chrono::steady_clock::time_point expires; };
class Confirmations {
 std::map<std::string,Pending> pending;
public:
 using Time=std::chrono::steady_clock::time_point;
 void clear() { pending.clear(); }
 void issue(const std::string& nonce,Identity who,std::string action,Time now) {
  if(nonce.size()!=32 || pending.contains(nonce)) throw std::runtime_error("Invalid confirmation nonce");
  for(auto i=pending.begin();i!=pending.end();) { if(i->second.expires<=now || (i->second.who.user==who.user && i->second.who.chat==who.chat)) i=pending.erase(i); else ++i; }
  if(pending.size()>=128) throw std::runtime_error("Too many confirmations");
  pending.emplace(nonce,Pending{who,std::move(action),now+std::chrono::seconds(30)});
 }
 std::optional<std::string> consume(const std::string& nonce,Identity who,Time now) {
  auto i=pending.find(nonce); if(i==pending.end()) return {};
  if(i->second.expires<=now) { pending.erase(i); return {}; }
  if(i->second.who.user!=who.user || i->second.who.chat!=who.chat) return {};
  auto action=i->second.action; pending.erase(i); return action;
 }
};
inline std::vector<std::string> components(std::string path,bool allowEmpty=false) {
 if(path.empty()) { if(allowEmpty) return {}; throw std::runtime_error("Empty path"); }
 std::replace(path.begin(),path.end(),'\\','/');
 std::vector<std::string> out; std::stringstream ss(path); std::string p;
 if(path.back()=='/') throw std::runtime_error("Trailing separator");
 while(std::getline(ss,p,'/')) {
  if(p.empty()||p=="."||p==".."||p.back()=='.'||p.back()==' ') throw std::runtime_error("Unsafe path component");
  for(unsigned char ch:p) if(ch<32 || ch==127 || std::string(":*?\"<>|").find(static_cast<char>(ch))!=std::string::npos) throw std::runtime_error("Unsafe path character");
  auto stem=p.substr(0,p.find('.')); std::transform(stem.begin(),stem.end(),stem.begin(),[](unsigned char v){return static_cast<char>(std::toupper(v));});
  if(stem=="CON"||stem=="PRN"||stem=="AUX"||stem=="NUL"||stem=="CONIN$"||stem=="CONOUT$"||
    (stem.size()==4&&(stem.substr(0,3)=="COM"||stem.substr(0,3)=="LPT")&&stem[3]>='0'&&stem[3]<='9')) throw std::runtime_error("Device name rejected");
  // Also reject Windows superscript-digit device aliases and ambiguous whitespace.
  if(stem.starts_with("COM")||stem.starts_with("LPT")) for(unsigned char ch:stem) if(ch>=128) throw std::runtime_error("Ambiguous device alias");
  if(p.front()==' ' || p.size()>240) throw std::runtime_error("Invalid component");
  out.push_back(p);
 }
 return out;
}
inline std::vector<std::string> rootComponents(const std::string& root) {
 if(root.size()<4 || !((root[0]>='A'&&root[0]<='Z')||(root[0]>='a'&&root[0]<='z')) || root[1]!=':' || (root[2]!='\\'&&root[2]!='/')) throw std::runtime_error("Root must be a local absolute folder such as C:\\RemoteFiles");
 return components(root.substr(3));
}
struct FileRequest { size_t root; std::string relative; };
inline FileRequest fileRequest(const std::string& text,size_t roots,bool directory=false) {
 std::istringstream s(text); size_t n=0; if(!(s>>n)||n<1||n>roots) throw std::runtime_error("Use a configured root number, starting at 1");
 std::string path; std::getline(s,path); if(!path.empty()&&path[0]==' ') path.erase(0,1);
 components(path,directory); return {n-1,path};
}
struct CommandPolicy {
 static constexpr uint32_t timeoutMs=15000;
 static constexpr size_t outputCap=65536;
 static bool valid(const std::string& s) { return !s.empty()&&s.size()<=4096&&s.find('\0')==std::string::npos&&s.find('\r')==std::string::npos&&s.find('\n')==std::string::npos; }
};
struct Intent { Identity who; std::string verb,arg; Feature feature; bool callback{}; std::string callbackId; };
inline std::optional<Intent> route(const json& update,const Config& c) {
 auto who=identity(update,c); if(!who) return {};
 try {
  bool cb=update.contains("callback_query");
  const auto& m=cb?update.at("callback_query").at("message"):update.at("message");
  auto text=cb?update.at("callback_query").at("data").get<std::string>():m.value("text",m.value("caption",std::string{}));
  if(text.size()>8192) return {};
  auto sep=text.find(' '); auto verb=text.substr(0,sep); auto arg=sep==std::string::npos?"":text.substr(sep+1);
  if(!verb.empty()&&verb[0]=='/') verb.erase(0,1);
  if(verb=="start"||verb=="help") verb="menu";
  static const std::map<std::string,Feature> verbs={
   {"menu",Feature::Status},{"status",Feature::Status},{"screen",Feature::Capture},{"shot",Feature::Capture},
   {"windows",Feature::Windows},{"close",Feature::Windows},{"volume",Feature::Audio},{"mute",Feature::Audio},
   {"lock",Feature::Lock},{"shutdown",Feature::Power},{"restart",Feature::Power},{"confirm",Feature::Power},
   {"cancel",Feature::Power},{"exec",Feature::Shell},{"get",Feature::Download},{"put",Feature::Upload},{"roots",Feature::Download}};
  auto v=verbs.find(verb); if(v==verbs.end()) return {};
  // Menu is always available to an authorized peer. All action handlers require their permission.
  if(verb!="menu" && !(verb=="roots" ? c.enabled(Feature::Download)||c.enabled(Feature::Upload) : c.enabled(v->second))) return {};
  if(m.contains("document") && (cb||verb!="put"||!c.enabled(Feature::Upload))) return {};
  if(verb=="put"&&!m.contains("document")) return {};
  return Intent{*who,verb,arg,v->second,cb,cb?update.at("callback_query").at("id").get<std::string>():""};
 } catch(...) { return {}; }
}
}
