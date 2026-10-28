#include "desk.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <set>
#include <wincrypt.h>
#include <winioctl.h>
#include <cstring>

namespace ta {
namespace {
[[noreturn]] void reject(const char* message){throw std::runtime_error(message);}
int64_t now(){return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
bool simpleId(const std::string& id){return !id.empty()&&id.size()<128&&id.find_first_not_of("0123456789abcdefABCDEF-")==std::string::npos;}
void safePath(const fs::path& root,const fs::path& path){
    auto absolute=fs::absolute(root).lexically_normal();auto target=fs::absolute(path).lexically_normal();
    auto relative=target.lexically_relative(absolute);
    if(relative.empty())reject("Backup path outside manager directory");
    for(auto& part:relative)if(part==L"..")reject("Backup path outside manager directory");
    auto check=[](const fs::path& p){auto attr=GetFileAttributesW(p.c_str());if(attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_REPARSE_POINT))reject("Backup contains a reparse point; cleanup blocked");};
    for(auto parent=target;!parent.empty();){check(parent);auto next=parent.parent_path();if(next==parent)break;parent=next;}
}
int64_t treeSize(const fs::path& root,const fs::path& path,std::string* stamp=nullptr){
    safePath(root,path);int64_t bytes=0;
    if(stamp)*stamp+=utf8(path.lexically_relative(root).wstring())+":"+std::to_string(fs::last_write_time(path).time_since_epoch().count())+";";
    if(fs::is_regular_file(path))return int64_t(fs::file_size(path));
    if(!fs::is_directory(path))reject("Unsupported backup file type");
    std::vector<fs::path> children;for(auto& entry:fs::directory_iterator(path))children.push_back(entry.path());std::sort(children.begin(),children.end());
    for(auto& child:children)bytes+=treeSize(root,child,stamp);
    return bytes;
}
struct Backup {fs::path path;std::string kind,owner,status,reason,stamp;int64_t bytes=0,created=0;bool removable=false;};
std::vector<fs::path> directories(const fs::path& root){
    std::vector<fs::path> out;if(!fs::exists(root))return out;safePath(root,root);
    for(auto& entry:fs::directory_iterator(root)){safePath(root,entry.path());if(entry.is_directory())out.push_back(entry.path());}
    std::sort(out.begin(),out.end(),[](auto& a,auto& b){auto x=fs::last_write_time(a),y=fs::last_write_time(b);return x==y?a.filename().wstring()>b.filename().wstring():x>y;});return out;
}
std::vector<Backup> inventory(const Config& c,int keep){
    if(keep<1||keep>100)reject("Backup retention must be between 1 and 100");
    std::vector<Backup> out;auto root=fs::absolute(c.state).lexically_normal();safePath(root,root);
    auto add=[&](const fs::path& path,const std::string& kind,const std::string& owner){
        Backup b;b.path=path;b.kind=kind;b.owner=owner;b.bytes=treeSize(root,path,&b.stamp);return b;
    };
    auto last=loadJson(root/L"last-backup.json")["id"].str();bool lastValid=simpleId(last)&&fs::is_directory(root/L"backups"/wide(last));int retained=0;
    for(auto& path:directories(root/L"backups")){
        auto b=add(path,"operation","");auto j=loadJson(path/L"transaction.json");b.status=j["status"].str();b.created=j["created"].integer();
        b.stamp+=j.dump();auto id=utf8(path.filename().wstring());
        if(!lastValid)b.reason="恢复指针异常，保留操作备份";
        else if(id==last)b.reason="最近一次恢复所需";
        else if(!simpleId(id)||!j.object()||j["root"].str()!=utf8(fs::absolute(c.root).lexically_normal().wstring()))b.reason="来源不明，保留";
        else if(b.status!="completed"&&b.status!="restored")b.reason="操作未完成，保留";
        else if(retained++<keep)b.reason="最近保留的操作备份";
        else b.removable=true;
        out.push_back(std::move(b));
    }
    for(auto& ownerPath:directories(root/L"sessions")){
        auto owner=utf8(ownerPath.filename().wstring());auto latest=loadJson(ownerPath/L"latest.json")["snapshot"].str();
        bool latestValid=validOwner(owner)&&simpleId(latest)&&fs::is_directory(ownerPath/wide(latest));
        if(latestValid){auto active=loadJson(ownerPath/wide(latest)/L"manifest.json");latestValid=active["owner"].str()==owner&&active["savedAt"].integer()>0;}
        int count=0;
        for(auto& path:directories(ownerPath)){
            auto b=add(path,"session",owner);auto j=loadJson(path/L"manifest.json");b.created=j["savedAt"].integer();b.stamp+=j.dump();auto id=utf8(path.filename().wstring());
            if(!latestValid)b.reason="当前快照指针异常，保留此账号快照";
            else if(id==latest)b.reason="当前有效会话快照";
            else if(!simpleId(id)||j["owner"].str()!=owner||b.created<=0)b.reason="快照信息异常，保留";
            else if(count++<keep-1)b.reason="最近保留的会话快照";
            else b.removable=true;
            out.push_back(std::move(b));
        }
    }
    return out;
}
std::string previewOf(const std::vector<Backup>& entries,int keep){
    // Hash metadata instead of sending transaction contents to the WebView.
    Json::Array rows;for(auto& b:entries)rows.emplace_back(Json::Object{{"kind",b.kind},{"owner",b.owner},{"id",utf8(b.path.filename().wstring())},{"bytes",Json::num(b.bytes)},{"stamp",b.stamp},{"remove",b.removable},{"reason",b.reason}});
    auto data=Json(Json::Object{{"keep",Json::num(keep)},{"rows",rows}}).dump();
    HCRYPTPROV provider=0;HCRYPTHASH hash=0;
    if(!CryptAcquireContextW(&provider,nullptr,nullptr,PROV_RSA_AES,CRYPT_VERIFYCONTEXT))reject("Cannot create backup preview digest");
    BYTE digest[32]{};DWORD length=sizeof(digest);
    bool ok=CryptCreateHash(provider,CALG_SHA_256,0,0,&hash)&&CryptHashData(hash,reinterpret_cast<const BYTE*>(data.data()),DWORD(data.size()),0)&&CryptGetHashParam(hash,HP_HASHVAL,digest,&length,0);
    if(hash)CryptDestroyHash(hash);
    CryptReleaseContext(provider,0);
    if(!ok)reject("Cannot create backup preview digest");
    static const char hex[]="0123456789abcdef";std::string out;for(auto byte:digest){out+=hex[byte>>4];out+=hex[byte&15];}return out;
}
}
Json accountOrganization(const Config& c){auto j=loadJson(c.state/L"account-organization.json");return j.object()?j:Json(Json::Object{});}
void saveAccountOrganization(const Config& c,const std::string& owner,const std::string& group,bool pinned){
    if(!validOwner(owner)||!fs::is_directory(c.root/L"users"/wide(owner)))reject("Unknown account for organization");
    auto text=wide(group);if(text.size()>32||std::any_of(text.begin(),text.end(),[](wchar_t ch){return ch<32;}))reject("Group must be one line with at most 32 characters");
    privateDirectory(c.state);auto j=accountOrganization(c);j[owner]=Json(Json::Object{{"group",group},{"pinned",pinned}});writeAtomic(c.state/L"account-organization.json",j.dump());
}
Json operationHistory(const Config& c){
    Json::Array rows;auto old=loadJson(c.state/L"switch-history.json");if(old.array())for(auto row:std::get<Json::Array>(old.v)){row["action"]="switch";rows.push_back(row);}
    auto recent=loadJson(c.state/L"operation-history.json");if(recent.array())for(auto& row:std::get<Json::Array>(recent.v))rows.push_back(row);
    std::reverse(rows.begin(),rows.end());
    std::stable_sort(rows.begin(),rows.end(),[](auto& a,auto& b){return a["time"].integer()>b["time"].integer();});if(rows.size()>200)rows.resize(200);return Json(rows);
}
void recordOperation(const Config& c,const std::string& action,const std::string& owner,bool success,const std::string& reason){
    privateDirectory(c.state);auto history=loadJson(c.state/L"operation-history.json");Json::Array rows;if(history.array())rows=std::get<Json::Array>(history.v);
    rows.emplace_back(Json::Object{{"action",action},{"owner",owner},{"success",success},{"time",Json::num(now())},{"reason",reason}});
    if(rows.size()>200)rows.erase(rows.begin(),rows.end()-200);
    writeAtomic(c.state/L"operation-history.json",Json(rows).dump());
}
Json backupInventory(const Config& c,int keep){
    auto entries=inventory(c,keep);Json::Array rows;int64_t total=0,reclaim=0;int removable=0;
    for(auto& b:entries){total+=b.bytes;if(b.removable){reclaim+=b.bytes;++removable;}rows.emplace_back(Json::Object{{"id",utf8(b.path.filename().wstring())},{"kind",b.kind},{"owner",b.owner},{"bytes",Json::num(b.bytes)},{"created",Json::num(b.created)},{"removable",b.removable},{"protection",b.reason}});}
    return Json(Json::Object{{"rows",rows},{"bytes",Json::num(total)},{"reclaimableBytes",Json::num(reclaim)},{"removableCount",Json::num(removable)},{"keep",Json::num(keep)},{"preview",previewOf(entries,keep)}});
}
int cleanupBackups(const Config& c,int keep,const std::string& preview){
    auto entries=inventory(c,keep);if(preview.empty()||preview!=previewOf(entries,keep))reject("Backup preview changed; scan again before cleanup");
    // Complete preflight before the first deletion. Inventory has checked every
    // ancestor and descendant for reparse points and protected active pointers.
    int count=0;auto root=fs::absolute(c.state).lexically_normal();
    for(auto& b:entries)if(b.removable){safePath(root,b.path);treeSize(root,b.path);}
    for(auto& b:entries)if(b.removable){safePath(root,b.path);fs::remove_all(b.path);++count;}
    return count;
}
int deskSelfTest(const fs::path& directory){
    privateDirectory(directory);auto work=directory/(L"desk-fixture-"+std::to_wstring(GetTickCount64()));Config c;c.fixture=true;c.root=work/L"data";c.state=work/L"state";
    privateDirectory(c.root/L"users"/L"v1_public_1001");privateDirectory(c.state);Json::Array results;int failed=0;
    auto require=[](bool ok){if(!ok)reject("Assertion failed");};auto test=[&](const char* name,auto f){try{f();results.emplace_back(Json::Object{{"test",name},{"passed",true}});}catch(const std::exception& e){++failed;results.emplace_back(Json::Object{{"test",name},{"passed",false},{"error",e.what()}});}};
    auto rejected=[&](auto f){bool blocked=false;try{f();}catch(...){blocked=true;}require(blocked);};
    test("Organization persists Unicode groups and pinning; invalid owners do not write",[&]{saveAccountOrganization(c,"v1_public_1001","工作",true);auto before=readFile(c.state/L"account-organization.json");require(accountOrganization(c)["v1_public_1001"]["pinned"].boolean());require(accountOrganization(c)["v1_public_1001"]["group"].str()=="工作");rejected([&]{saveAccountOrganization(c,"../outside","x",true);});rejected([&]{saveAccountOrganization(c,"v1_public_1001","x\ny",true);});require(readFile(c.state/L"account-organization.json")==before);});
    test("History merges legacy entries and bounds new records",[&]{writeAtomic(c.state/L"switch-history.json",R"([{"owner":"v1_public_1001","success":true,"time":1}])");recordOperation(c,"logout","v1_public_1001",false,"test failure");auto h=operationHistory(c);require(h.array()&&std::get<Json::Array>(h.v).size()==2);require(std::get<Json::Array>(h.v).back()["action"].str()=="switch");
        Json::Array many;for(int i=0;i<205;++i)many.emplace_back(Json::Object{{"time",Json::num(i)},{"action","switch"}});writeAtomic(c.state/L"operation-history.json",Json(many).dump());recordOperation(c,"logout","v1_public_1001",true);require(std::get<Json::Array>(operationHistory(c).v).size()==200);require(std::get<Json::Array>(loadJson(c.state/L"operation-history.json").v).size()==200);
    });
    auto snapshots=c.state/L"sessions"/L"v1_public_1001";
    for(auto id:{L"100-1",L"200-2",L"300-3"}){privateDirectory(snapshots/id);writeAtomic(snapshots/id/L"manifest.json",R"({"owner":"v1_public_1001","savedAt":100})");writeAtomic(snapshots/id/L"payload","test snapshot");}
    writeAtomic(snapshots/L"latest.json",R"({"snapshot":"100-1"})");
    for(auto id:{L"aa-01",L"bb-02",L"cc-03"}){auto path=c.state/L"backups"/id;privateDirectory(path);writeAtomic(path/L"transaction.json",Json(Json::Object{{"root",utf8(fs::absolute(c.root).lexically_normal().wstring())},{"status","completed"}}).dump());}
    writeAtomic(c.state/L"last-backup.json",R"({"id":"aa-01"})");
    test("Cleanup protects active snapshot and rollback pointer even when older",[&]{auto j=backupInventory(c,1);require(j["removableCount"].integer()==3);for(auto& row:std::get<Json::Array>(j["rows"].v))if(row["id"].str()=="100-1"||row["id"].str()=="aa-01")require(!row["removable"].boolean());});
    test("Stale previews cannot delete any backup",[&]{auto j=backupInventory(c,1);writeAtomic(snapshots/L"latest.json",R"({"snapshot":"300-3"})");rejected([&]{cleanupBackups(c,1,j["preview"].str());});require(fs::exists(snapshots/L"100-1"));});
    test("Changes inside a nested snapshot invalidate the preview",[&]{auto j=backupInventory(c,1);writeAtomic(snapshots/L"100-1"/L"payload","modified fixture snapshot");rejected([&]{cleanupBackups(c,1,j["preview"].str());});require(fs::exists(snapshots/L"100-1"));});
    test("Unknown snapshot pointers and foreign backups are retained",[&]{writeAtomic(snapshots/L"latest.json",R"({"snapshot":"../escape"})");auto foreign=c.state/L"backups"/L"dd-04";privateDirectory(foreign);writeAtomic(foreign/L"transaction.json",R"({"root":"foreign","status":"completed"})");auto j=backupInventory(c,1);for(auto& row:std::get<Json::Array>(j["rows"].v))if(row["kind"].str()=="session"||row["id"].str()=="dd-04")require(!row["removable"].boolean());});
    test("Cleanup removes only previewed old copies and preserves source data",[&]{writeAtomic(snapshots/L"latest.json",R"({"snapshot":"300-3"})");writeAtomic(c.root/L"source-sentinel","unchanged");auto j=backupInventory(c,1);auto n=cleanupBackups(c,1,j["preview"].str());require(n==j["removableCount"].integer());require(fs::exists(snapshots/L"300-3")&&fs::exists(c.state/L"backups"/L"aa-01"));require(readFile(c.root/L"source-sentinel")=="unchanged");});
    test("Invalid retention is rejected",[&]{rejected([&]{backupInventory(c,0);});rejected([&]{backupInventory(c,101);});});
    test("Paths outside manager data are rejected",[&]{rejected([&]{safePath(c.state,work/L"outside");});rejected([&]{safePath(c.state,c.state/L".."/L"outside");});});
    test("Junctions block cleanup without touching linked files",[&]{
        auto outside=work/L"outside";privateDirectory(outside);writeAtomic(outside/L"sentinel","preserved");auto link=snapshots/L"300-3"/L"junction";fs::create_directory(link);
        auto target=fs::absolute(outside).wstring();std::wstring substitute=L"\\??\\"+target;std::wstring names=substitute+L'\0'+target+L'\0';
        struct Header{DWORD tag;WORD length,reserved,subOffset,subLength,printOffset,printLength;};
        Header header{IO_REPARSE_TAG_MOUNT_POINT,WORD(8+names.size()*sizeof(wchar_t)),0,0,WORD(substitute.size()*sizeof(wchar_t)),WORD((substitute.size()+1)*sizeof(wchar_t)),WORD(target.size()*sizeof(wchar_t))};
        std::vector<BYTE> buffer(sizeof(header)+names.size()*sizeof(wchar_t));std::memcpy(buffer.data(),&header,sizeof(header));std::memcpy(buffer.data()+sizeof(header),names.data(),names.size()*sizeof(wchar_t));
        HANDLE handle=CreateFileW(link.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);if(handle==INVALID_HANDLE_VALUE)reject("Cannot open fixture junction");DWORD returned=0;bool created=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,buffer.data(),DWORD(buffer.size()),nullptr,0,&returned,nullptr)!=FALSE;CloseHandle(handle);require(created);
        bool blocked=false;try{backupInventory(c,1);}catch(...){blocked=true;}bool preserved=readFile(outside/L"sentinel")=="preserved";bool removed=RemoveDirectoryW(link.c_str())!=FALSE;require(blocked&&preserved&&removed);
    });
    writeAtomic(directory/L"desk-tests.json",Json(Json::Object{{"failed",Json::num(failed)},{"tests",results}}).dump());return failed?2:0;
}
}
