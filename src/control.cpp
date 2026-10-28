#include "control.h"
#include "installation.h"
#include "desk.h"
#include <tlhelp32.h>
#include <chrono>
#include <thread>
#include <set>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace ta {
namespace {
[[noreturn]]void error(const char* message){throw std::runtime_error(message);}
struct Handle {
    HANDLE value=nullptr;
    explicit Handle(HANDLE h=nullptr):value(h){}
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
int64_t nowSeconds(){return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::wstring lower(std::wstring s){std::transform(s.begin(),s.end(),s.begin(),towlower);return s;}
std::string fileTime(const fs::path& path){WIN32_FILE_ATTRIBUTE_DATA a{};if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&a))return {};return std::to_string((uint64_t(a.ftLastWriteTime.dwHighDateTime)<<32)|a.ftLastWriteTime.dwLowDateTime);}
struct Process {DWORD id=0,parent=0;fs::path path;std::string creation;};
std::vector<Process> processes(){
    std::vector<Process> out;Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));
    if(snapshot.value==INVALID_HANDLE_VALUE)error("Cannot inspect TeleAgent processes");
    PROCESSENTRY32W e{};e.dwSize=sizeof(e);DWORD ownSession=0;ProcessIdToSessionId(GetCurrentProcessId(),&ownSession);
    for(BOOL ok=Process32FirstW(snapshot.value,&e);ok;ok=Process32NextW(snapshot.value,&e)){
        DWORD processSession=0;if(!ProcessIdToSessionId(e.th32ProcessID,&processSession)||processSession!=ownSession)continue;
        Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,e.th32ProcessID));
        if(!h.value)continue;
        wchar_t name[32768];DWORD n=32768;FILETIME create{},exit{},kernel{},user{};
        if(!QueryFullProcessImageNameW(h.value,0,name,&n)||!GetProcessTimes(h.value,&create,&exit,&kernel,&user))continue;
        out.push_back({e.th32ProcessID,e.th32ParentProcessID,fs::path(std::wstring(name,n)),std::to_string((uint64_t(create.dwHighDateTime)<<32)|create.dwLowDateTime)});
    }
    return out;
}
bool samePath(const fs::path& a,const fs::path& b){return lower(fs::absolute(a).lexically_normal().wstring())==lower(fs::absolute(b).lexically_normal().wstring());}
bool below(const fs::path& root,const fs::path& path){
    auto r=lower(fs::absolute(root).lexically_normal().wstring()),p=lower(fs::absolute(path).lexically_normal().wstring());
    return p.size()>r.size()&&p.substr(0,r.size())==r&&(p[r.size()]=='\\'||p[r.size()]=='/');
}
std::vector<Process> mainProcesses(const Config& c){
    auto all=processes();std::set<DWORD> app;
    for(const auto& p:all)if(samePath(p.path,c.exe))app.insert(p.id);
    std::vector<Process> roots;for(const auto& p:all)if(app.count(p.id)&&!app.count(p.parent))roots.push_back(p);
    return roots;
}
bool alive(const Process& p){for(auto& q:processes())if(q.id==p.id&&q.creation==p.creation&&samePath(q.path,p.path))return true;return false;}
void requestNormalShutdown(const std::vector<Process>& roots){
    struct Windows {std::set<DWORD> pids;std::vector<HWND> handles;} target;
    for(const auto& p:roots)target.pids.insert(p.id);
    EnumWindows([](HWND window,LPARAM data)->BOOL{auto* target=reinterpret_cast<Windows*>(data);DWORD pid=0;GetWindowThreadProcessId(window,&pid);if(target->pids.count(pid))target->handles.push_back(window);return TRUE;},reinterpret_cast<LPARAM>(&target));
    bool accepted=!target.handles.empty();
    for(auto handle:target.handles){DWORD_PTR result=0;if(!SendMessageTimeoutW(handle,WM_QUERYENDSESSION,0,ENDSESSION_CLOSEAPP,SMTO_ABORTIFHUNG|SMTO_BLOCK,700,&result)||!result)accepted=false;}
    if(accepted)for(auto handle:target.handles)SendNotifyMessageW(handle,WM_ENDSESSION,TRUE,ENDSESSION_CLOSEAPP);
    else for(auto handle:target.handles)PostMessageW(handle,WM_CLOSE,0,0);
}
std::string tail(const fs::path& path){
    Handle h(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(h.value==INVALID_HANDLE_VALUE)return {};
    LARGE_INTEGER size{};if(!GetFileSizeEx(h.value,&size))return {};auto count=std::min<LONGLONG>(size.QuadPart,512*1024);LARGE_INTEGER offset{};offset.QuadPart=size.QuadPart-count;SetFilePointerEx(h.value,offset,nullptr,FILE_BEGIN);std::string data(size_t(count),'\0');DWORD read=0;if(!ReadFile(h.value,data.data(),DWORD(data.size()),&read,nullptr))return {};data.resize(read);return data;
}
int64_t lineTime(const std::string& line){
    SYSTEMTIME t{};if(sscanf(line.c_str(),"[%hu-%hu-%huT%hu:%hu:%hu",&t.wYear,&t.wMonth,&t.wDay,&t.wHour,&t.wMinute,&t.wSecond)!=6)return 0;FILETIME f{};if(!SystemTimeToFileTime(&t,&f))return 0;return int64_t(((uint64_t(f.dwHighDateTime)<<32)|f.dwLowDateTime)/10000000)-11644473600LL;
}
bool nativeOwnerActive(const Config& c,const std::string& owner){
    auto roots=mainProcesses(c);if(roots.empty())return false;int64_t started=nowSeconds();for(auto& p:roots)started=std::min(started,int64_t(std::stoull(p.creation)/10000000)-11644473600LL);
    auto pointer=loadJson(c.root/L"current-owner.json");if(pointer["ownerDir"].str()!=owner)return false;std::string expected="ownerDir: '"+owner+"'",generation="generation: "+std::to_string(pointer["generation"].integer())+" }";
    auto logs=c.root/L"logs";if(!fs::is_directory(logs))return false;std::vector<fs::path> files;for(auto& e:fs::directory_iterator(logs))if(e.is_regular_file()&&e.path().extension()==L".log")files.push_back(e.path());std::sort(files.begin(),files.end(),[](auto& a,auto& b){return fs::last_write_time(a)>fs::last_write_time(b);});
    if(files.size()>2)files.resize(2);
    for(auto& p:files){auto data=tail(p);size_t at=0;while(at<data.size()){auto end=data.find('\n',at);if(end==std::string::npos)end=data.size();auto line=data.substr(at,end-at);at=end+1;if(line.find("[OwnerContext] Owner active")!=std::string::npos&&line.find(expected)!=std::string::npos&&line.find(generation)!=std::string::npos&&lineTime(line)>=started)return true;}}
    return false;
}
void recordObservedAccounts(const Config& c){
    privateDirectory(c.state);auto scan=scanAccounts(c);if(!scan.error.empty())error("Cannot inspect account identity");auto path=c.state/L"session-cache.json";auto cache=loadJson(path);
    for(const auto& a:scan.accounts){
        if(!a.token||a.relogin)continue;
        auto entry=cache[a.owner];bool coherent=a.cachedId==a.userId||entry["identityVerified"].boolean();if(!coherent)continue;
        entry["name"]=a.name;entry["phone"]=a.phone;entry["identityVerified"]=true;bool active=a.current&&nativeOwnerActive(c,a.owner);entry["live"]=active;if(active)entry["observedAt"]=Json::num(nowSeconds());entry["tokenStamp"]=fileTime(c.root/L"users"/wide(a.owner)/L"app-auth"/L"token.json");
        if(a.cookieRead&&!a.cookieFromCache){Json::Array cookies;for(const auto& cookie:a.cookies)cookies.emplace_back(Json::Object{{"host",cookie.host},{"name",cookie.name},{"expires",Json::num(cookie.expires)},{"persistent",cookie.persistent}});entry["cookies"]=cookies;entry["cookieObservedAt"]=Json::num(nowSeconds());}
        cache[a.owner]=entry;
    }
    writeAtomic(path,cache.dump());
}
void step(Progress& progress,const wchar_t* text){if(progress)progress(text);}
void ensureIdle(const Config& c){
    auto owner=loadJson(c.root/L"current-owner.json")["ownerDir"].str();if(!validOwner(owner))return;
    auto path=c.root/L"users"/wide(owner)/L"state"/L"session-status.json";auto statuses=loadJson(path);
    if(statuses.object())for(auto& [id,status]:std::get<Json::Object>(statuses.v)){(void)id;if(status.str()=="running")error("Active agent tasks must finish before switching accounts");}
}
void copyTree(const fs::path& source,const fs::path& destination){
    if(!fs::exists(source))return;
    if(fs::is_symlink(source)||GetFileAttributesW(source.c_str())&FILE_ATTRIBUTE_REPARSE_POINT)error("Unsupported link in login snapshot");
    if(fs::is_regular_file(source)){writeAtomic(destination,readFile(source,256*1024*1024));return;}
    privateDirectory(destination);for(auto& entry:fs::directory_iterator(source)){
        auto name=entry.path().filename().wstring();
        if(name==L"Cache"||name==L"Code Cache"||name==L"GPUCache"||name==L"DawnGraphiteCache"||name==L"DawnWebGPUCache"||name==L"LOCK")continue;
        copyTree(entry.path(),destination/entry.path().filename());
    }
}
void saveCurrentSession(const Config& c){
    auto scan=scanAccounts(c);if(!scan.error.empty())error("Cannot save current login state");
    auto current=std::find_if(scan.accounts.begin(),scan.accounts.end(),[](auto& a){return a.current;});if(current==scan.accounts.end()||!current->token)return;
    privateDirectory(c.state/L"sessions");auto path=c.state/L"sessions"/wide(current->owner);privateDirectory(path);
    // Immutable snapshot directories avoid leaving a half-overwritten saved session.
    auto snapshot=path/wide(std::to_string(nowSeconds())+"-"+std::to_string(GetTickCount64()));privateDirectory(snapshot);
    copyTree(c.root/L"users"/wide(current->owner)/L"app-auth",snapshot/L"app-auth");
    if(!current->partition.empty())copyTree(current->partition,snapshot/L"partition");
    writeAtomic(snapshot/L"manifest.json",Json(Json::Object{{"owner",current->owner},{"savedAt",Json::num(nowSeconds())},{"name",current->name},{"phone",current->phone}}).dump());
    writeAtomic(path/L"latest.json",Json(Json::Object{{"snapshot",utf8(snapshot.filename().wstring())}}).dump());
}
void appendHistory(const Config& c,const std::string& owner,bool success,const std::string& reason={}){
    recordOperation(c,"switch",owner,success,reason);
}
} // namespace

void syncLiveAccount(const Config& c){if(!c.fixture)recordObservedAccounts(c);}
void launchManaged(const Config& c){
    if(c.fixture)error("Managed launching is disabled for fixture data");
    if(!fs::is_regular_file(c.exe))error("TeleAgent executable is missing");
    privateDirectory(c.state);std::wstring command=L"\""+c.exe.wstring()+L"\"";STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION info{};
    BOOL ok=CreateProcessW(c.exe.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_BREAKAWAY_FROM_JOB,nullptr,c.exe.parent_path().c_str(),&startup,&info);
    if(!ok)ok=CreateProcessW(c.exe.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,c.exe.parent_path().c_str(),&startup,&info);
    if(!ok)error("Cannot start TeleAgent");
    Handle process(info.hProcess),thread(info.hThread);FILETIME create{},exit{},kernel{},user{};GetProcessTimes(process.value,&create,&exit,&kernel,&user);
    writeAtomic(c.state/L"managed-app.json",Json(Json::Object{{"pid",Json::num(info.dwProcessId)},{"creation",std::to_string((uint64_t(create.dwHighDateTime)<<32)|create.dwLowDateTime)},{"startedAt",Json::num(nowSeconds())}}).dump());
}
void automaticStop(const Config& c,Progress progress){
    if(c.fixture)error("Automatic app control is disabled for fixture data");
    auto roots=mainProcesses(c);
    if(roots.empty()&&teleagentProcesses(c).empty()){recordObservedAccounts(c);return;}
    ensureIdle(c);
    step(progress,L"正在保存当前登录状态…");
    try{recordObservedAccounts(c);}catch(...){}
    step(progress,L"正在自动关闭 TeleAgent…");
    requestNormalShutdown(roots);
    ULONGLONG deadline=GetTickCount64()+2500;
    while(GetTickCount64()<deadline){if(teleagentProcesses(c).empty()){recordObservedAccounts(c);return;}if(mainProcesses(c).empty())break;Sleep(200);}
    // Only validated TeleAgent executables and its own runtime leftovers are
    // eligible. Never terminate arbitrary processes just because names match.
    ensureIdle(c);step(progress,L"正在结束 TeleAgent 残留进程并保存会话…");
    auto all=processes();for(auto& p:all){
        auto name=lower(p.path.filename().wstring());
        bool knownRuntime=(name==L"teleagent.exe"||name==L"node.exe"||name==L"python.exe"||name==L"pythonw.exe"||name==L"super-agent-code.exe"||name==L"im-service.exe")&&(below(c.root/L"runtimes",p.path)||below(c.root/L"users",p.path));
        if(!samePath(p.path,c.exe)&&!knownRuntime)continue;
        if(!alive(p))continue;
        Handle h(OpenProcess(PROCESS_TERMINATE|SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,p.id));
        if(h.value){FILETIME create{},exit{},kernel{},user{};if(!GetProcessTimes(h.value,&create,&exit,&kernel,&user))continue;auto creation=std::to_string((uint64_t(create.dwHighDateTime)<<32)|create.dwLowDateTime);if(creation!=p.creation)continue;TerminateProcess(h.value,0);WaitForSingleObject(h.value,3000);}
    }
    deadline=GetTickCount64()+5000;while(GetTickCount64()<deadline){if(teleagentProcesses(c).empty()){recordObservedAccounts(c);return;}Sleep(100);}
    error("TeleAgent did not finish shutting down; account data was not replaced");
}
void automaticSwitch(const Config& c,const std::string& owner,Progress progress,const std::atomic_bool* stop,bool history){
    auto cancelled=[&]{if(stop&&stop->load())throw PollCancelled();};
    try{
        cancelled();
        if(c.fixture)error("Automatic app control is disabled for fixture data");
        if(!validExecutable(c.exe))error("TeleAgent executable is missing");
        if(!validOwner(owner))error("Invalid account");
        auto before=scanAccounts(c);auto target=std::find_if(before.accounts.begin(),before.accounts.end(),[&](auto& a){return a.owner==owner;});
        if(!before.error.empty()||target==before.accounts.end())error("Account not found");
        if(!target->token||target->relogin)error("This account has no saved login session; sign in once first");
        privateDirectory(c.state);
        automaticStop(c,progress);step(progress,L"正在保存当前账号的会话备份…");saveCurrentSession(c);
        cancelled();
        step(progress,L"正在选择目标账号…");switchAccount(c,owner);
        step(progress,L"正在打开 TeleAgent 并恢复登录…");launchManaged(c);
        ULONGLONG deadline=GetTickCount64()+40000;std::string lastError;
        while(GetTickCount64()<deadline){
            cancelled();
            try{syncLiveAccount(c);auto cache=loadJson(c.state/L"session-cache.json");auto active=loadJson(c.root/L"current-owner.json")["ownerDir"].str();
                if(active!=owner&&validOwner(active))error("TeleAgent restored a different account");
                if(active==owner&&cache[owner]["live"].boolean()&&cache[owner]["identityVerified"].boolean()&&nowSeconds()-cache[owner]["observedAt"].integer()<4){if(history)appendHistory(c,owner,true);step(progress,L"切换成功，已识别目标账号");return;}
            }catch(const std::exception& e){lastError=e.what();}
            Sleep(500);
        }
        (void)lastError;error("Could not confirm the target identity; inspect the TeleAgent window");
    }catch(const std::exception& e){
        if(history)try{appendHistory(c,owner,false,e.what());}catch(...){}
        // Retain the target window for a normal expired-session sign-in. All
        // source login files and the previous account's snapshot remain intact.
        throw;
    }
}
void saveVerifiedAccountSession(const Config& c,const std::string& owner,Progress progress){
    auto currentOwner=[&]{return loadJson(c.root/L"current-owner.json")["ownerDir"].str();};
    auto base=c.state/L"sessions"/wide(owner);auto previous=loadJson(base/L"latest.json")["snapshot"].str();
    if(currentOwner()!=owner)error("TeleAgent restored a different account");
    automaticStop(c,progress);
    if(currentOwner()!=owner)error("TeleAgent restored a different account");
    step(progress,L"正在保存已核对账号的登录快照…");saveCurrentSession(c);
    auto snapshot=loadJson(base/L"latest.json")["snapshot"].str();
    if(snapshot.empty()||snapshot==previous||fs::path(wide(snapshot)).filename()!=fs::path(wide(snapshot))||loadJson(base/wide(snapshot)/L"manifest.json")["owner"].str()!=owner)error("Cannot save verified account session");
}
void automaticNewLogin(const Config& c,Progress progress){
    if(c.fixture)error("Automatic app control is disabled for fixture data");
    if(!validExecutable(c.exe))error("TeleAgent executable is missing");
    // A first launch lets TeleAgent create its own supported data format.
    if(!fs::is_directory(c.root/L"users")){launchManaged(c);return;}
    automaticStop(c,progress);
    // Reopening an in-progress sign-in must not erase its new browser session.
    auto active=loadJson(c.root/L"current-owner.json")["ownerDir"].str();
    if(validOwner(active)){
        step(progress,L"正在保存已有账号的登录会话…");saveCurrentSession(c);
        step(progress,L"正在备份并准备新的登录入口…");prepareNewLogin(c);
    }
    step(progress,L"正在打开 TeleAgent，请手动登录新账号…");launchManaged(c);
}
void saveAccountNote(const Config& c,const std::string& owner,const std::string& note){
    if(!validOwner(owner)||!fs::is_directory(c.root/L"users"/wide(owner)))error("Unknown account for note");
    auto text=wide(note);if(text.size()>64||text.find_first_of(L"\r\n\t")!=std::wstring::npos)error("Account note must be one line with at most 64 characters");
    privateDirectory(c.state);auto path=c.state/L"account-notes.json";auto notes=loadJson(path);
    if(!notes.object())notes=Json(Json::Object{});
    if(note.empty())std::get<Json::Object>(notes.v).erase(owner);else notes[owner]=note;writeAtomic(path,notes.dump());
}
int controlSelfTest(const fs::path& directory){
    privateDirectory(directory);Config c;c.fixture=true;c.root=directory/L"control-fixture";c.state=directory/L"control-state";privateDirectory(c.root/L"users"/L"v1_public_1001");saveAccountNote(c,"v1_public_1001","工作账号");
    bool ok=loadJson(c.state/L"account-notes.json")["v1_public_1001"].str()=="工作账号";saveAccountNote(c,"v1_public_1001","");ok=ok&&!loadJson(c.state/L"account-notes.json").has("v1_public_1001");bool blocked=false;try{automaticStop(c);}catch(...){blocked=true;}ok=ok&&blocked;
    bool newLoginBlocked=false;try{automaticNewLogin(c);}catch(...){newLoginBlocked=true;}ok=ok&&newLoginBlocked;
    Config missing=c;missing.fixture=false;missing.exe=directory/L"not-installed"/L"TeleAgent.exe";auto pointer=missing.root/L"current-owner.json";writeAtomic(pointer,"POINTER_UNCHANGED");bool preflightSwitch=false,preflightLogin=false;
    try{automaticSwitch(missing,"v1_public_1001");}catch(const std::exception& e){preflightSwitch=std::string(e.what())=="TeleAgent executable is missing";}
    try{automaticNewLogin(missing);}catch(const std::exception& e){preflightLogin=std::string(e.what())=="TeleAgent executable is missing";}
    ok=ok&&preflightSwitch&&preflightLogin&&readFile(pointer)=="POINTER_UNCHANGED";
    writeAtomic(directory/L"control-tests.json",Json(Json::Object{{"passed",ok},{"missingExecutableBlocksBeforeMutation",preflightSwitch&&preflightLogin},{"fixtureNewLoginBlocked",newLoginBlocked}}).dump());return ok?0:2;
}
} // namespace ta
