#include "storage.h"
#include <tlhelp32.h>
#include <sddl.h>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <set>
#include <map>
#include <optional>
#include <functional>
#include <sstream>
#include <iomanip>
#include <limits>
#include <cstring>

namespace ta {
namespace {
struct Handle {HANDLE h=INVALID_HANDLE_VALUE;explicit Handle(HANDLE x):h(x){}~Handle(){if(h!=INVALID_HANDLE_VALUE&&h)CloseHandle(h);}operator HANDLE()const{return h;}};
[[noreturn]]void fail(const char* msg){throw std::runtime_error(msg);}
template<class T>T address(HMODULE dll,const char* name){auto proc=GetProcAddress(dll,name);static_assert(sizeof(T)==sizeof(proc));T out=nullptr;std::memcpy(&out,&proc,sizeof(out));return out;}
std::wstring lower(std::wstring s){for(auto& c:s)c=towlower(c);return s;}
bool inside(const fs::path& root,const fs::path& p){auto r=lower(fs::absolute(root).lexically_normal().wstring());auto s=lower(fs::absolute(p).lexically_normal().wstring());return s==r||(s.size()>r.size()&&s.substr(0,r.size())==r&&(s[r.size()]=='\\'||s[r.size()]=='/'));}
void noReparse(const fs::path& root,const fs::path& path){
    if(!inside(root,path))fail("Path outside application data directory");
    fs::path cur=fs::absolute(root).lexically_normal();auto check=[](const fs::path& q){DWORD a=GetFileAttributesW(q.c_str());if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT))fail("Reparse-point paths are not supported for account changes");};
    // Check ancestors too: a junction above the data root must not redirect writes.
    for(auto q=cur;!q.empty();){check(q);auto parent=q.parent_path();if(parent==q)break;q=parent;}
    auto rel=fs::absolute(path).lexically_normal().lexically_relative(cur);for(auto& part:rel){cur/=part;check(cur);}
}
std::string uniqueId(){GUID g{};if(FAILED(CoCreateGuid(&g)))fail("Cannot create transaction ID");wchar_t b[48];StringFromGUID2(g,b,48);auto s=utf8(b);s.erase(std::remove_if(s.begin(),s.end(),[](char c){return c=='{'||c=='}';}),s.end());return s;}
struct Temp {
    fs::path path;
    explicit Temp(const fs::path& parent){privateDirectory(parent);path=parent/wide(uniqueId());privateDirectory(path);}
    ~Temp(){std::error_code ec;if(inside(path.parent_path(),path)&&!path.empty())fs::remove_all(path,ec);}
};
struct Stamp {uint64_t size=0,time=0;bool exists=false;bool operator==(const Stamp&)const=default;};
Stamp stamp(const fs::path& p){WIN32_FILE_ATTRIBUTE_DATA a{};if(!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&a))return {};return {(uint64_t(a.nFileSizeHigh)<<32)|a.nFileSizeLow,(uint64_t(a.ftLastWriteTime.dwHighDateTime)<<32)|a.ftLastWriteTime.dwLowDateTime,true};}
void copyRead(const fs::path& from,const fs::path& to){auto b=readFile(from,256*1024*1024);writeAtomic(to,b);}
void copyStable(const std::vector<std::pair<fs::path,fs::path>>& files){
    for(int attempt=0;attempt<3;++attempt){std::vector<Stamp> before;for(auto& p:files)before.push_back(stamp(p.first));
        for(size_t i=0;i<files.size();++i)if(before[i].exists)copyRead(files[i].first,files[i].second);
        bool ok=true;for(size_t i=0;i<files.size();++i)if(!(stamp(files[i].first)==before[i]))ok=false;
        if(ok)return;
    }fail("Files changed while taking a read-only snapshot; retry after TeleAgent exits");
}
uint32_t u32(std::string_view s,size_t p){if(p+4>s.size())fail("Truncated binary data");uint32_t v=0;for(int i=0;i<4;++i)v|=uint32_t(uint8_t(s[p+i]))<<(8*i);return v;}
uint64_t u64(std::string_view s,size_t p){return uint64_t(u32(s,p))|(uint64_t(u32(s,p+4))<<32);}
uint64_t varint(std::string_view s,size_t& p){uint64_t n=0;for(int shift=0;shift<=63;shift+=7){if(p>=s.size())fail("Truncated varint");auto c=uint8_t(s[p++]);if(shift==63&&c>1)fail("Varint overflow");n|=uint64_t(c&127)<<shift;if(!(c&128))return n;}fail("Invalid varint");}
std::string_view field(std::string_view s,size_t& p){auto n=varint(s,p);if(n>s.size()-p)fail("Truncated binary field");auto v=s.substr(p,size_t(n));p+=size_t(n);return v;}
uint32_t crc32c(std::string_view s){static auto table=[](){std::array<uint32_t,256> t{};for(uint32_t i=0;i<256;++i){uint32_t c=i;for(int j=0;j<8;++j)c=(c>>1)^((c&1)?0x82f63b78:0);t[i]=c;}return t;}();uint32_t c=~0u;for(unsigned char b:s)c=table[(c^b)&255]^(c>>8);return ~c;}
uint32_t maskedCrc(std::string_view s){uint32_t c=crc32c(s);return ((c>>15)|(c<<17))+0xa282ead8u;}
std::vector<std::string> logRecords(const std::string& b){
    std::vector<std::string> out;std::string fragment;bool assembling=false;
    for(size_t p=0;p+7<=b.size();){size_t remaining=32768-p%32768;if(remaining<7){p+=remaining;continue;}
        auto len=uint16_t(uint8_t(b[p+4]))|(uint16_t(uint8_t(b[p+5]))<<8);auto type=uint8_t(b[p+6]);
        if(!len&&!type){p+=remaining;continue;}if(size_t(len)+7>remaining)fail("Invalid LevelDB record boundary");if(p+7+len>b.size())break;
        std::string piece=b.substr(p+7,len),check(1,char(type));check+=piece;if(maskedCrc(check)!=u32(b,p))fail("LevelDB checksum mismatch");p+=7+len;
        if(type==1){out.push_back(std::move(piece));fragment.clear();assembling=false;}
        else if(type==2){fragment=std::move(piece);assembling=true;}
        else if(type==3&&assembling)fragment+=piece;
        else if(type==4&&assembling){fragment+=piece;out.push_back(std::move(fragment));fragment.clear();assembling=false;}
        else fail("Invalid LevelDB fragmented record");
        if(fragment.size()>32*1024*1024)fail("LevelDB record too large");
    }return out;
}
std::string snappy(std::string_view s){
    size_t p=0;auto size=varint(s,p);if(size>32*1024*1024)fail("Snappy block too large");std::string out;out.reserve(size);
    while(p<s.size()&&out.size()<size){uint8_t tag=s[p++];unsigned type=tag&3;size_t len=0,offset=0;
        if(type==0){len=tag>>2;if(len<60)++len;else{unsigned bytes=unsigned(len-59);if(bytes>4||p+bytes>s.size())fail("Invalid Snappy literal");len=0;for(unsigned j=0;j<bytes;++j)len|=size_t(uint8_t(s[p++]))<<(j*8);++len;}if(len>s.size()-p||len>size-out.size())fail("Invalid Snappy literal length");out.append(s.substr(p,len));p+=len;continue;}
        if(type==1){len=4+((tag>>2)&7);if(p>=s.size())fail("Truncated Snappy copy");offset=((tag&0xe0)<<3)|uint8_t(s[p++]);}
        else if(type==2){len=1+(tag>>2);if(p+2>s.size())fail("Truncated Snappy copy");offset=uint8_t(s[p])|(size_t(uint8_t(s[p+1]))<<8);p+=2;}
        else{len=1+(tag>>2);offset=u32(s,p);p+=4;}
        if(!offset||offset>out.size()||len>size-out.size())fail("Invalid Snappy copy offset");
        for(size_t j=0;j<len;++j)out+=out[out.size()-offset];
    }if(out.size()!=size||p!=s.size())fail("Invalid Snappy block size");return out;
}
std::string tableBlock(std::string_view file,uint64_t offset,uint64_t size){
    if(offset>file.size()||size>file.size()-offset||file.size()-offset-size<5)fail("Invalid LevelDB block handle");
    auto data=file.substr(size_t(offset),size_t(size));uint8_t type=file[size_t(offset+size)];std::string check(data);check+=char(type);
    if(maskedCrc(check)!=u32(file,size_t(offset+size+1)))fail("LevelDB table checksum mismatch");
    if(type==0)return std::string(data);
    if(type==1)return snappy(data);
    fail("Unsupported LevelDB compression");
}
using Entries=std::vector<std::pair<std::string,std::string>>;
Entries blockEntries(std::string_view b){
    if(b.size()<4)fail("Short LevelDB block");
    auto count=u32(b,b.size()-4);if(count>(b.size()-4)/4)fail("Invalid LevelDB restarts");size_t end=b.size()-4-size_t(count)*4,p=0;std::string previous;Entries out;
    while(p<end){auto shared=varint(b,p),nonshared=varint(b,p),valuesize=varint(b,p);if(shared>previous.size()||p>end||nonshared>end-p||valuesize>end-p-nonshared)fail("Invalid LevelDB entry");std::string key=previous.substr(0,size_t(shared));key.append(b.substr(p,size_t(nonshared)));p+=size_t(nonshared);std::string value(b.substr(p,size_t(valuesize)));p+=size_t(valuesize);out.emplace_back(key,std::move(value));previous=std::move(key);}
    return out;
}
struct Value {uint64_t sequence=0;bool deleted=false;std::string value;};
void applyValue(std::map<std::string,Value>& state,std::string key,uint64_t seq,bool deleted,std::string val){auto it=state.find(key);if(it==state.end()||seq>=it->second.sequence)state[std::move(key)]={seq,deleted,std::move(val)};}
std::map<std::string,Value> readLevelDb(const fs::path& source,const fs::path& stateRoot){
    if(!fs::is_directory(source))return {};
    Temp snap(stateRoot/L"runtime");std::vector<std::pair<fs::path,fs::path>> files;
    auto current=readFile(source/L"CURRENT",1024);while(!current.empty()&&(current.back()=='\n'||current.back()=='\r'))current.pop_back();
    if(current.rfind("MANIFEST-",0)!=0||current.find_first_not_of("MANIFEST-0123456789")!=std::string::npos)fail("Invalid LevelDB CURRENT");
    files.push_back({source/L"CURRENT",snap.path/L"CURRENT"});files.push_back({source/wide(current),snap.path/wide(current)});
    for(auto& e:fs::directory_iterator(source)){auto ext=e.path().extension().wstring();if(ext==L".ldb"||ext==L".sst"||ext==L".log")files.push_back({e.path(),snap.path/e.path().filename()});}
    copyStable(files);if(readFile(snap.path/L"CURRENT",1024)!=readFile(source/L"CURRENT",1024))fail("LevelDB manifest changed during snapshot");
    std::set<uint64_t> tables;uint64_t log=0,prevlog=0;
    for(auto& rec:logRecords(readFile(snap.path/wide(current)))){size_t p=0;while(p<rec.size()){auto tag=varint(rec,p);switch(tag){
        case 1:(void)field(rec,p);break;case 2:log=varint(rec,p);break;case 3:case 4:(void)varint(rec,p);break;case 9:prevlog=varint(rec,p);break;
        case 5:(void)varint(rec,p);(void)field(rec,p);break;
        case 6:(void)varint(rec,p);tables.erase(varint(rec,p));break;
        case 7:{(void)varint(rec,p);auto num=varint(rec,p);(void)varint(rec,p);(void)field(rec,p);(void)field(rec,p);tables.insert(num);break;}
        default:fail("Unsupported LevelDB manifest version");
    }}}
    std::map<std::string,Value> state;
    for(auto number:tables){std::wostringstream stem;stem<<std::setw(6)<<std::setfill(L'0')<<number;fs::path path=snap.path/(stem.str()+L".ldb");if(!fs::exists(path))path=snap.path/(stem.str()+L".sst");auto b=readFile(path);
        if(b.size()<48||u64(b,b.size()-8)!=0xdb4775248b80fb57ULL)fail("Invalid LevelDB table footer");
        size_t p=b.size()-48;(void)varint(b,p);(void)varint(b,p);auto io=varint(b,p),is=varint(b,p);
        for(auto& [key,handle]:blockEntries(tableBlock(b,io,is))){(void)key;size_t h=0;auto off=varint(handle,h),len=varint(handle,h);for(auto& [internal,val]:blockEntries(tableBlock(b,off,len))){if(internal.size()<8)fail("Short LevelDB internal key");auto trailer=u64(internal,internal.size()-8);unsigned type=unsigned(trailer&255);if(type>1)fail("Unsupported LevelDB value type");applyValue(state,internal.substr(0,internal.size()-8),trailer>>8,type==0,std::move(val));}}
    }
    for(auto& e:fs::directory_iterator(snap.path)){if(e.path().extension()!=L".log")continue;uint64_t num=0;try{num=std::stoull(e.path().stem().wstring());}catch(...){continue;}if(num<log&&num!=prevlog)continue;
        for(auto& rec:logRecords(readFile(e.path()))){if(rec.size()<12)fail("Short LevelDB batch");auto seq=u64(rec,0);auto count=u32(rec,8);if(count>1000000)fail("LevelDB batch too large");size_t p=12;for(uint32_t i=0;i<count;++i){if(p>=rec.size())fail("Short LevelDB batch entry");uint8_t type=rec[p++];if(type>1)fail("Invalid LevelDB batch type");std::string key(field(rec,p)),val;if(type==1)val=std::string(field(rec,p));applyValue(state,std::move(key),seq+i,type==0,std::move(val));}if(p!=rec.size())fail("Invalid LevelDB batch trailing data");}
    }return state;
}
std::string chromiumText(std::string_view v){if(v.empty())return {};if(v[0]==0){if((v.size()-1)%2)fail("Invalid Chromium UTF-16");std::wstring s;for(size_t i=1;i+1<v.size();i+=2)s+=wchar_t(uint8_t(v[i])|(uint16_t(uint8_t(v[i+1]))<<8));return utf8(s);}if(v[0]==1){std::string s;for(size_t i=1;i<v.size();++i)Json::utf8(s,uint8_t(v[i]));return s;}fail("Unsupported Chromium string encoding");}
void readIdentity(Account& a,const Config& c){
    auto state=readLevelDb(a.partition/L"Local Storage"/L"leveldb",c.state);uint64_t newest=0;bool found=false;std::string value;
    for(auto& [key,val]:state){if(key.empty()||key[0]!='_')continue;auto zero=key.find('\0');if(zero==std::string::npos)continue;auto name=chromiumText(std::string_view(key).substr(zero+1));if(name!="opencowork-auth")continue;if(!found||val.sequence>=newest){found=true;newest=val.sequence;value=val.deleted?"":val.value;}}
    if(value.empty()){a.cacheRead=true;return;}
    auto j=Json::parse(chromiumText(value));const auto& user=j["state"]["user"];a.cachedId=user["id"].str();
    a.cachedName=user["name"].str();a.cachedPhone=user["tel"].str();if(a.cachedPhone.empty())a.cachedPhone=user["phone"].str();
    a.identityMismatch=!a.cachedId.empty()&&a.cachedId!=a.userId;
    if(!a.identityMismatch&&!a.cachedId.empty()){a.name=a.cachedName;a.phone=a.cachedPhone;}
    a.cacheRead=true;
}
struct sqlite3;struct sqlite3_stmt;
struct Sqlite {
    HMODULE dll=nullptr;
    int (*open)(const char*,sqlite3**,int,const char*)=nullptr;
    int (*close)(sqlite3*)=nullptr;int (*prepare)(sqlite3*,const char*,int,sqlite3_stmt**,const char**)=nullptr;
    int (*step)(sqlite3_stmt*)=nullptr;int (*finalize)(sqlite3_stmt*)=nullptr;
    const unsigned char* (*text)(sqlite3_stmt*,int)=nullptr;int64_t (*integer)(sqlite3_stmt*,int)=nullptr;
    Sqlite(){wchar_t system[MAX_PATH];if(!GetSystemDirectoryW(system,MAX_PATH))fail("Cannot find system directory");auto p=fs::path(system)/L"winsqlite3.dll";dll=LoadLibraryExW(p.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!dll)fail("Windows SQLite component unavailable");
#define LOAD(member,name) member=address<decltype(member)>(dll,name);if(!member)fail("Windows SQLite API unavailable")
        LOAD(open,"sqlite3_open_v2");LOAD(close,"sqlite3_close");LOAD(prepare,"sqlite3_prepare_v2");LOAD(step,"sqlite3_step");LOAD(finalize,"sqlite3_finalize");LOAD(text,"sqlite3_column_text");LOAD(integer,"sqlite3_column_int64");
#undef LOAD
    }
    ~Sqlite(){if(dll)FreeLibrary(dll);}
};
void readCookies(Account& a,const Config& c){
    fs::path db=a.partition/L"Network"/L"Cookies";if(!fs::is_regular_file(db))db=a.partition/L"Cookies";if(!fs::is_regular_file(db)){a.cookieRead=true;return;}
    Temp snap(c.state/L"runtime");fs::path dest=snap.path/L"Cookies";std::vector<std::pair<fs::path,fs::path>> files={{db,dest},{fs::path(db.wstring()+L"-wal"),fs::path(dest.wstring()+L"-wal")}};copyStable(files);
    Sqlite sql;sqlite3* handle=nullptr;if(sql.open(utf8(dest.wstring()).c_str(),&handle,2,nullptr)!=0){if(handle)sql.close(handle);fail("Cannot open cookie snapshot");}
    struct DbCloser{Sqlite& sql;sqlite3* h;~DbCloser(){sql.close(h);}}dbcloser{sql,handle};sqlite3_stmt* stmt=nullptr;
    const char* query="SELECT host_key,name,expires_utc,is_persistent FROM cookies WHERE (host_key='teleai.com.cn' OR host_key='.teleai.com.cn' OR host_key LIKE '%.teleai.com.cn' OR host_key='teleagi.cn' OR host_key='.teleagi.cn' OR host_key LIKE '%.teleagi.cn') AND name IN ('operToken','operationToken','accessToken') ORDER BY name,host_key";
    if(sql.prepare(handle,query,-1,&stmt,nullptr)!=0)fail("Cookie schema is unsupported");
    struct StmtCloser{Sqlite& sql;sqlite3_stmt* s;~StmtCloser(){sql.finalize(s);}}stmtcloser{sql,stmt};int result=0;
    while((result=sql.step(stmt))==100){const auto* host=sql.text(stmt,0);const auto* name=sql.text(stmt,1);if(!host||!name)continue;a.cookies.push_back({reinterpret_cast<const char*>(host),reinterpret_cast<const char*>(name),sql.integer(stmt,2),sql.integer(stmt,3)!=0});}
    if(result!=101)fail("Cannot read cookie snapshot");
    a.cookieRead=true;
}
Json checkedPointer(const Config& c){auto p=loadJson(c.root/L"current-owner.json");if(!p.object())return {};auto owner=p["ownerDir"].str();if(!validOwner(owner)||p["tenantId"].str()!="public"||owner!="v1_public_"+p["userId"].str()||p["generation"].integer(-1)<1)fail("Current owner pointer has an unsupported format");return p;}
void ready(const Config& c){
    noReparse(c.root,c.root);noReparse(c.state,c.state);
    if(!teleagentProcesses(c).empty())fail("TeleAgent or its background process is running. Exit it completely from the tray, then retry.");
    auto marker=loadJson(c.root/L"owner-scope-ready.json");if(marker["version"].integer()!=1||marker["status"].str()!="ready")fail("Account isolation has not completed; changes are disabled");
    auto p=checkedPointer(c);if(p.has("pendingOwner"))fail("An account transition is pending; finish it in TeleAgent first");
    auto journal=loadJson(c.root/L"owner-claim-journal.json");if(journal["phase"].str()=="running")fail("Account migration is in progress; changes are disabled");
    noReparse(c.root,c.root/L"current-owner.json");noReparse(c.root,c.root/L"users");noReparse(c.root,c.root/L"Partitions");
}
fs::path transaction(const Config& c,const std::string& action){privateDirectory(c.state);privateDirectory(c.state/L"backups");auto path=c.state/L"backups"/wide(uniqueId());privateDirectory(path);Json j(Json::Object{});j["action"]=action;j["status"]="prepared";j["root"]=utf8(fs::absolute(c.root).lexically_normal().wstring());j["moves"]=Json::Array{};
    auto pointer=c.root/L"current-owner.json";if(fs::exists(pointer))copyRead(pointer,path/L"pointer-before.json");writeAtomic(path/L"transaction.json",j.dump());return path;
}
void finish(const Config& c,const fs::path& txn){auto j=loadJson(txn/L"transaction.json");j["status"]="completed";writeAtomic(txn/L"transaction.json",j.dump());writeAtomic(c.state/L"last-backup.json",Json(Json::Object{{"id",utf8(txn.filename().wstring())}}).dump());}
void replacePointer(const Config& c,const std::string& owner){auto old=checkedPointer(c);auto gen=old["generation"].integer();if(gen>=9007199254740990LL)fail("Owner generation counter exhausted");Json p(Json::Object{{"ownerDir",owner},{"tenantId","public"},{"userId",owner.substr(10)},{"generation",Json::num(gen+1)}});writeAtomic(c.root/L"current-owner.json",p.dump()+"\n");}
void moveIntoBackup(const Config& c,const fs::path& txn,const fs::path& source){
    if(!fs::exists(source))return;
    noReparse(c.root,source);auto j=loadJson(txn/L"transaction.json");auto& moves=std::get<Json::Array>(j["moves"].v);std::string name="item-"+std::to_string(moves.size());auto dest=txn/wide(name);
    Json move(Json::Object{{"source",utf8(source.lexically_relative(c.root).wstring())},{"backup",name}});moves.push_back(move);writeAtomic(txn/L"transaction.json",j.dump());
    if(!MoveFileExW(source.c_str(),dest.c_str(),MOVEFILE_WRITE_THROUGH))fail("Cannot move login files into backup; they may still be in use");
}
void undoMoves(const Config& c,const fs::path& txn){auto j=loadJson(txn/L"transaction.json");if(!j["moves"].array())return;const auto& moves=std::get<Json::Array>(j["moves"].v);for(auto it=moves.rbegin();it!=moves.rend();++it){auto source=c.root/wide((*it)["source"].str());auto back=txn/wide((*it)["backup"].str());noReparse(c.root,source);noReparse(txn,back);if(fs::exists(back)){if(fs::exists(source))fail("New login files exist; restore would overwrite them");if(!MoveFileExW(back.c_str(),source.c_str(),MOVEFILE_WRITE_THROUGH))fail("Unable to restore login files from backup");}}}
}

std::wstring wide(const std::string& s){if(s.empty())return {};int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);if(!n)throw std::runtime_error("Invalid UTF-8");std::wstring out(n,L'\0');MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),out.data(),n);return out;}
std::string utf8(const std::wstring& s){if(s.empty())return {};int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0,nullptr,nullptr);if(!n)throw std::runtime_error("Invalid UTF-16");std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s.data(),int(s.size()),out.data(),n,nullptr,nullptr);return out;}
std::string readFile(const fs::path& p,size_t limit){Handle h(CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));if(h.h==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot read file: "+utf8(p.filename().wstring()));LARGE_INTEGER n{};if(!GetFileSizeEx(h,&n)||n.QuadPart<0||uint64_t(n.QuadPart)>limit)fail("File exceeds read limit");std::string out(size_t(n.QuadPart),'\0');size_t at=0;while(at<out.size()){DWORD got=0;if(!ReadFile(h,out.data()+at,DWORD(std::min<size_t>(out.size()-at,1024*1024)),&got,nullptr)||!got)fail("File changed or became unreadable");at+=got;}return out;}
void privateDirectory(const fs::path& p){
    if(p.empty())fail("Empty state directory");
    if(!p.is_absolute()){privateDirectory(fs::absolute(p).lexically_normal());return;}
    if(fs::exists(p)){if(!fs::is_directory(p))fail("State path is not a directory");noReparse(p,p);return;}
    if(!fs::exists(p.parent_path()))privateDirectory(p.parent_path());
    HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))fail("Cannot read Windows user identity");Handle t(token);DWORD n=0;GetTokenInformation(t,TokenUser,nullptr,0,&n);std::vector<char> b(n);if(!GetTokenInformation(t,TokenUser,b.data(),n,&n))fail("Cannot read Windows user SID");wchar_t* sid=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid,&sid))fail("Cannot format Windows SID");std::wstring sddl=L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;"+std::wstring(sid)+L")";LocalFree(sid);PSECURITY_DESCRIPTOR sd=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&sd,nullptr))fail("Cannot protect state directory");SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};BOOL ok=CreateDirectoryW(p.c_str(),&sa);DWORD err=GetLastError();LocalFree(sd);if(!ok&&err!=ERROR_ALREADY_EXISTS)fail("Cannot create private state directory");
}
void writeAtomic(const fs::path& p,const std::string& data){
    fs::path tmp=p;tmp+=L".tmp-"+wide(uniqueId());Handle h(CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));if(h.h==INVALID_HANDLE_VALUE)fail("Cannot create transaction file");size_t pos=0;bool ok=true;while(pos<data.size()){DWORD written=0;if(!WriteFile(h,data.data()+pos,DWORD(std::min<size_t>(data.size()-pos,1024*1024)),&written,nullptr)||!written){ok=false;break;}pos+=written;}if(!FlushFileBuffers(h))ok=false;CloseHandle(h.h);h.h=INVALID_HANDLE_VALUE;
    if(!ok||!MoveFileExW(tmp.c_str(),p.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(tmp.c_str());fail("Atomic file replacement failed");}
}
bool validOwner(const std::string& o){if(o.rfind("v1_public_",0)!=0||o.size()<=10||o.size()>138)return false;return std::all_of(o.begin()+10,o.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-';});}
Json loadJson(const fs::path& p){if(!fs::exists(p))return {};return Json::parse(readFile(p,4*1024*1024));}
std::vector<DWORD> teleagentProcesses(const Config& c){
    std::vector<DWORD> ids;Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0));if(snapshot.h==INVALID_HANDLE_VALUE)fail("Cannot enumerate processes safely");PROCESSENTRY32W e{};e.dwSize=sizeof(e);
    for(BOOL ok=Process32FirstW(snapshot,&e);ok;ok=Process32NextW(snapshot,&e)){if(e.th32ProcessID==GetCurrentProcessId())continue;bool byName=_wcsicmp(e.szExeFile,L"TeleAgent.exe")==0;Handle p(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,e.th32ProcessID));wchar_t path[32768];DWORD n=32768;bool known=p.h&&p.h!=INVALID_HANDLE_VALUE&&QueryFullProcessImageNameW(p,0,path,&n);bool match=c.fixture?false:byName;if(known&&c.fixture)match=inside(c.root,fs::path(std::wstring(path,n)));if(known&&!c.fixture){auto q=fs::path(std::wstring(path,n));match=match||inside(c.root,q)||(!c.exe.empty()&&inside(c.exe.parent_path(),q));}if(match)ids.push_back(e.th32ProcessID);}
    return ids;
}
Scan scanAccounts(const Config& c){
    Scan s;try{s.pointer=checkedPointer(c);if(!fs::is_directory(c.root/L"users")){s.error=L"没有找到 TeleAgent 账号数据目录。";return s;}
        Json notes,sessionCache;
        try{notes=loadJson(c.state/L"account-notes.json");}catch(...){}
        try{sessionCache=loadJson(c.state/L"session-cache.json");}catch(...){}
        std::vector<fs::path> paths;for(auto& e:fs::directory_iterator(c.root/L"users"))if(e.is_directory()&&validOwner(utf8(e.path().filename().wstring())))paths.push_back(e.path());std::sort(paths.begin(),paths.end());
        for(auto& path:paths){Account a;a.owner=utf8(path.filename().wstring());a.userId=a.owner.substr(10);a.current=a.owner==s.pointer["ownerDir"].str();a.token=fs::is_regular_file(path/L"app-auth"/L"token.json");a.relogin=fs::exists(path/L"app-auth"/L"relogin-required");
            if(fs::is_directory(c.root/L"Partitions"))for(auto& p:fs::directory_iterator(c.root/L"Partitions")){auto name=lower(p.path().filename().wstring());auto expected=L"owner%3a"+lower(wide(a.owner));if(p.is_directory()&&(name==expected||name==L"owner_"+lower(wide(a.owner))||name==L"owner:"+lower(wide(a.owner)))){if(!a.partition.empty())fail("Multiple partitions match one owner");a.partition=p.path();}}
            if(!a.partition.empty()){
                try{readIdentity(a,c);}catch(const std::exception&){a.warning=L"身份缓存读取失败（可能正在写入或格式已变化）";}
                try{readCookies(a,c);}catch(const std::exception&){if(!a.warning.empty())a.warning+=L"；";a.warning+=L"Cookie 读取失败，未将其当作无 Cookie";}
            }else{a.warning=L"未找到对应浏览器分区";a.cookieRead=true;a.cacheRead=true;}
            a.note=notes[a.owner].str();
            const auto& saved=sessionCache[a.owner];
            if(saved["identityVerified"].boolean()){
                a.observedAt=saved["observedAt"].integer();FILETIME now{};GetSystemTimeAsFileTime(&now);int64_t seconds=int64_t(((uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime)/10000000)-11644473600LL;
                a.live=saved["live"].boolean()&&seconds-a.observedAt>=0&&seconds-a.observedAt<12;
                if(a.live||a.name.empty()){a.name=saved["name"].str();a.phone=saved["phone"].str();}
                if(a.live){a.identityMismatch=false;a.cacheRead=true;}
                if(!a.cookieRead&&saved["cookies"].array()){
                    a.cookies.clear();for(auto& cookie:std::get<Json::Array>(saved["cookies"].v))a.cookies.push_back({cookie["host"].str(),cookie["name"].str(),cookie["expires"].integer(),cookie["persistent"].boolean()});
                    a.cookieRead=true;a.cookieFromCache=true;a.cookieObservedAt=saved["cookieObservedAt"].integer();a.warning=L"应用正在占用 Cookie 文件，显示最近一次保存的到期信息；切换时自动更新";
                }
                a.infoSource=a.live?"正在运行的 TeleAgent":"最近保存的本机会话";
            }
            if(a.identityMismatch){if(!a.warning.empty())a.warning+=L"；";a.warning+=L"该分区缓存属于其他账号，禁止直接复用；姓名和手机号另按账号 ID 匹配";}
            s.accounts.push_back(std::move(a));
        }
        // Legacy partitions can hold another owner's identity. Attribute display
        // metadata by the explicit user ID, never by the partition's directory.
        for(auto& target:s.accounts)if(target.name.empty()){
            for(const auto& source:s.accounts)if(source.cachedId==target.userId&&!source.cachedName.empty()){
                target.name=source.cachedName;target.phone=source.cachedPhone;
                target.identityFromOtherPartition=source.owner!=target.owner;
                if(target.identityFromOtherPartition){if(!target.warning.empty())target.warning+=L"；";target.warning+=L"姓名/手机号来自其他分区内匹配此账号 ID 的历史缓存，尚未在线核验";}
                break;
            }
        }
    }catch(const std::exception& e){s.error=L"读取失败："+wide(e.what());}return s;
}
void switchAccount(const Config& c,const std::string& owner){ready(c);if(!validOwner(owner))fail("Invalid owner directory");auto path=c.root/L"users"/wide(owner);noReparse(c.root,path);if(!fs::is_directory(path))fail("Account directory is missing");auto scan=scanAccounts(c);if(!scan.error.empty())fail("Cannot validate account data");auto it=std::find_if(scan.accounts.begin(),scan.accounts.end(),[&](auto& a){return a.owner==owner;});if(it==scan.accounts.end())fail("Account not found");if(it->identityMismatch&&!it->relogin)fail("Cached identity does not match this account. Use Re-login to reset only its login state with a backup.");if(!it->cacheRead&&!it->relogin)fail("Cannot validate cached identity; changes are disabled");auto txn=transaction(c,"switch");ready(c);try{replacePointer(c,owner);finish(c,txn);}catch(...){if(fs::exists(txn/L"pointer-before.json"))writeAtomic(c.root/L"current-owner.json",readFile(txn/L"pointer-before.json"));throw;}}
void localLogout(const Config& c,const std::string& owner,bool selectForRelogin){
    ready(c);if(!validOwner(owner))fail("Invalid owner directory");auto auth=c.root/L"users"/wide(owner)/L"app-auth";noReparse(c.root,auth);if(!fs::is_directory(auth))fail("Account authentication directory is missing");auto scan=scanAccounts(c);if(!scan.error.empty())fail("Cannot validate account data");auto it=std::find_if(scan.accounts.begin(),scan.accounts.end(),[&](auto& a){return a.owner==owner;});if(it==scan.accounts.end())fail("Account not found");auto txn=transaction(c,"local-logout");auto j=loadJson(txn/L"transaction.json");j["owner"]=owner;j["restorePointer"]=selectForRelogin;writeAtomic(txn/L"transaction.json",j.dump());bool markerWritten=false;ready(c);
    try{moveIntoBackup(c,txn,auth/L"token.json");moveIntoBackup(c,txn,auth/L"local-key");moveIntoBackup(c,txn,auth/L"relogin-required");if(!it->partition.empty())moveIntoBackup(c,txn,it->partition);writeAtomic(auth/L"relogin-required","");markerWritten=true;if(selectForRelogin)replacePointer(c,owner);finish(c,txn);}catch(...){if(markerWritten)DeleteFileW((auth/L"relogin-required").c_str());undoMoves(c,txn);if(selectForRelogin&&fs::exists(txn/L"pointer-before.json"))writeAtomic(c.root/L"current-owner.json",readFile(txn/L"pointer-before.json"));throw;}
}
void prepareNewLogin(const Config& c){
    ready(c);auto pointer=checkedPointer(c);if(!validOwner(pointer["ownerDir"].str()))fail("No active owner for a new login");
    auto txn=transaction(c,"new-login");ready(c);
    try{
        // Only the device login context is moved. All owner partitions, tokens,
        // chat databases and settings remain in their original directories.
        for(auto name:{L"Network",L"Local Storage",L"Session Storage",L"WebStorage",L"partition-handoff.json"})moveIntoBackup(c,txn,c.root/name);
        for(auto name:{L"token.json",L"local-key"})moveIntoBackup(c,txn,c.root/L"app-auth"/name);
        if(fs::is_directory(c.root/L"Partitions"))for(auto& entry:fs::directory_iterator(c.root/L"Partitions")){
            auto name=lower(entry.path().filename().wstring());
            if(name==L"owner%3a__device_login__"||name==L"owner:__device_login__"||name==L"owner___device_login__")moveIntoBackup(c,txn,entry.path());
        }
        moveIntoBackup(c,txn,c.root/L"current-owner.json");finish(c,txn);
    }catch(...){undoMoves(c,txn);throw;}
}
void restoreLatest(const Config& c){
    ready(c);auto id=loadJson(c.state/L"last-backup.json")["id"].str();if(id.empty()||id.find_first_not_of("0123456789abcdefABCDEF-")!=std::string::npos)fail("No valid backup is available");auto txn=c.state/L"backups"/wide(id);noReparse(c.state,txn);auto j=loadJson(txn/L"transaction.json");if(j["root"].str()!=utf8(fs::absolute(c.root).lexically_normal().wstring()))fail("Backup belongs to a different data root");if(j["status"].str()!="completed")fail("Backup has already been restored or did not finish");
    if(j["action"].str()=="new-login"){
        // A completed sign-in, or any new device-login files, take precedence.
        if(fs::exists(c.root/L"current-owner.json"))fail("New login data exists; restore stopped to avoid overwriting it");
        if(!j["moves"].array())fail("Invalid new login backup");
        for(const auto& move:std::get<Json::Array>(j["moves"].v))if(fs::exists(c.root/wide(move["source"].str())))fail("New login data exists; restore stopped to avoid overwriting it");
        // Also guard paths which were absent when the backup was created.
        for(auto name:{L"Network",L"Local Storage",L"Session Storage",L"WebStorage",L"partition-handoff.json"})if(fs::exists(c.root/name))fail("New login data exists; restore stopped to avoid overwriting it");
        if(fs::exists(c.root/L"app-auth"/L"token.json")||fs::exists(c.root/L"app-auth"/L"local-key"))fail("New login data exists; restore stopped to avoid overwriting it");
        for(auto name:{L"owner%3a__device_login__",L"owner:__device_login__",L"owner___device_login__"})if(fs::exists(c.root/L"Partitions"/name))fail("New login data exists; restore stopped to avoid overwriting it");
        auto old=loadJson(txn/L"pointer-before.json")["ownerDir"].str();if(!validOwner(old)||!fs::is_directory(c.root/L"users"/wide(old)))fail("Previous owner cannot be restored");
        undoMoves(c,txn);replacePointer(c,old);
    }else if(j["action"].str()=="local-logout"){
        auto owner=j["owner"].str();if(!validOwner(owner))fail("Invalid account in backup");auto marker=c.root/L"users"/wide(owner)/L"app-auth"/L"relogin-required";noReparse(c.root,marker);
        const auto& moves=std::get<Json::Array>(j["moves"].v);for(auto& m:moves){auto target=c.root/wide(m["source"].str());noReparse(c.root,target);if(fs::exists(target)&&target!=marker)fail("New login data exists; restore stopped to avoid overwriting it");}
        auto markerBackup=txn/L"marker-before-restore";if(fs::exists(marker)&&!MoveFileExW(marker.c_str(),markerBackup.c_str(),MOVEFILE_WRITE_THROUGH))fail("Cannot preserve relogin marker");
        try{undoMoves(c,txn);}catch(...){if(fs::exists(markerBackup)&&!fs::exists(marker))MoveFileExW(markerBackup.c_str(),marker.c_str(),MOVEFILE_WRITE_THROUGH);throw;}
        if(j["restorePointer"].boolean()){auto previous=loadJson(txn/L"pointer-before.json");auto previousOwner=previous["ownerDir"].str();if(validOwner(previousOwner)&&fs::is_directory(c.root/L"users"/wide(previousOwner)))replacePointer(c,previousOwner);}
    }else if(j["action"].str()=="switch"){
        auto p=loadJson(txn/L"pointer-before.json");auto owner=p["ownerDir"].str();if(!validOwner(owner)||!fs::is_directory(c.root/L"users"/wide(owner)))fail("Previous owner cannot be restored");replacePointer(c,owner);
    }else fail("Unsupported backup action");
    j["status"]="restored";writeAtomic(txn/L"transaction.json",j.dump());
}
void launchTeleagent(const Config& c){if(c.fixture)fail("Launching TeleAgent is disabled in fixture mode");if(!fs::is_regular_file(c.exe))fail("TeleAgent executable is missing");auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",c.exe.c_str(),nullptr,c.exe.parent_path().c_str(),SW_SHOWNORMAL));if(result<=32)fail("Cannot launch TeleAgent");}
std::wstring formatExpiry(int64_t us){if(us<=0)return L"会话 Cookie（无固定到期时间）";uint64_t ticks=uint64_t(us)*10;FILETIME utc{DWORD(ticks),DWORD(ticks>>32)},local{};SYSTEMTIME st{};if(!FileTimeToLocalFileTime(&utc,&local)||!FileTimeToSystemTime(&local,&st))return L"无法解析时间";wchar_t b[64];swprintf(b,64,L"%04u-%02u-%02u %02u:%02u:%02u",st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);return b;}
std::wstring cookieSummary(const Account& a){if(!a.cookieRead)return L"读取失败";std::vector<const Cookie*> tokens;for(auto& c:a.cookies)if(c.name=="operToken")tokens.push_back(&c);if(tokens.empty())return L"未发现 operToken";int64_t earliest=std::numeric_limits<int64_t>::max();for(auto* c:tokens){if(!c->persistent||c->expires<=0)return L"会话 Cookie";earliest=std::min(earliest,c->expires);}FILETIME now{};GetSystemTimeAsFileTime(&now);auto micros=int64_t((uint64_t(now.dwHighDateTime)<<32)|now.dwLowDateTime)/10;return formatExpiry(earliest)+(earliest<=micros?L"（已到期）":L"（本地未到期）");}
Json publicReport(const Scan& s){
    Json report(Json::Object{});report["error"]=utf8(s.error);Json::Array rows;
    for(const auto& a:s.accounts){
        Json j(Json::Object{{"owner",a.owner},{"userId",a.userId},{"current",a.current},{"name",a.name},{"phone",a.phone},{"note",a.note},{"tokenFilePresent",a.token},{"savedSession",a.token&&!a.relogin},{"reloginRequired",a.relogin},{"identityMismatch",a.identityMismatch},{"cookieRead",a.cookieRead},{"cookieSummary",utf8(cookieSummary(a))},{"warning",utf8(a.warning)},{"identityFromOtherPartition",a.identityFromOtherPartition},{"live",a.live},{"infoSource",a.infoSource},{"observedAt",Json::num(a.observedAt)},{"cookieFromCache",a.cookieFromCache},{"cookieObservedAt",Json::num(a.cookieObservedAt)}});
        Json::Array cookies;for(const auto& c:a.cookies)cookies.emplace_back(Json::Object{{"host",c.host},{"name",c.name},{"expiresChromiumMicroseconds",Json::num(c.expires)},{"expiresLocal",utf8(formatExpiry(c.expires))},{"persistent",c.persistent}});
        j["cookies"]=std::move(cookies);rows.push_back(std::move(j));
    }
    report["accounts"]=std::move(rows);report["serverValidity"]="Local app identity and cookie metadata; server acceptance is checked by TeleAgent";return report;
}

int selfTest(const fs::path& directory){
    Json::Array results;fs::path work;int failed=0;
    auto test=[&](const std::string& name,const std::function<void()>& f){try{f();results.emplace_back(Json::Object{{"test",name},{"passed",true}});}catch(const std::exception& e){++failed;results.emplace_back(Json::Object{{"test",name},{"passed",false},{"error",std::string(e.what())}});}};
    auto require=[](bool ok,const char* message){if(!ok)fail(message);};
    auto rejected=[&](const std::function<void()>& f){bool threw=false;try{f();}catch(...){threw=true;}require(threw,"Expected operation to be rejected");};
    try{
        privateDirectory(directory);work=directory/wide("case-"+uniqueId());privateDirectory(work);
        auto le32=[](uint32_t n){std::string s;for(int i=0;i<4;++i)s+=char(n>>(8*i));return s;};
        auto le64=[](uint64_t n){std::string s;for(int i=0;i<8;++i)s+=char(n>>(8*i));return s;};
        auto vi=[](uint64_t n){std::string s;while(n>=128){s+=char((n&127)|128);n>>=7;}s+=char(n);return s;};
        auto fld=[&](const std::string& s){return vi(s.size())+s;};
        auto record=[&](const std::string& payload){std::string check(1,'\1');check+=payload;return le32(maskedCrc(check))+std::string{char(payload.size()),char(payload.size()>>8),char(1)}+payload;};
        auto batch=[&](uint64_t seq,const std::string& key,const std::optional<std::string>& val){return le64(seq)+le32(1)+std::string(1,val?'\1':'\0')+fld(key)+(val?fld(*val):"");};
        auto storageKey=[](){std::string k="_file://";k+='\0';k+='\1';k+="opencowork-auth";return k;};
        auto auth=[](const std::string& id,const std::string& name){Json user(Json::Object{{"id",id},{"name",name},{"tel","135****1234"}});Json j(Json::Object{{"state",Json(Json::Object{{"user",user},{"token","secret-token-sentinel"}})}});auto text=wide(j.dump());std::string out(1,'\0');for(auto ch:text){out+=char(ch&255);out+=char((ch>>8)&255);}return out;};
        Config c;c.fixture=true;c.root=work/L"data";c.state=work/L"state";c.configRoot=work/L"config";c.exe=work/L"never-launch.exe";privateDirectory(c.root);privateDirectory(c.root/L"users");privateDirectory(c.root/L"Partitions");
        auto profile=[&](const std::string& id,const std::string& name){auto owner="v1_public_"+id;auto path=c.root/L"users"/wide(owner);privateDirectory(path);privateDirectory(path/L"app-auth");writeAtomic(path/L"teleagent.db","CHAT_DATABASE_SENTINEL");auto partition=c.root/L"Partitions"/wide("owner%3a"+owner);privateDirectory(partition);privateDirectory(partition/L"Local Storage");auto lv=partition/L"Local Storage"/L"leveldb";privateDirectory(lv);writeAtomic(lv/L"CURRENT","MANIFEST-000001\n");writeAtomic(lv/L"MANIFEST-000001",record(vi(2)+vi(3)));writeAtomic(lv/L"000003.log",record(batch(10,storageKey(),auth(id,name))));return owner;};
        auto a=profile("1001","测试账号 A"),b=profile("1002","Test Account B");writeAtomic(c.root/L"owner-scope-ready.json",R"({"version":1,"status":"ready"})");writeAtomic(c.root/L"current-owner.json",Json(Json::Object{{"ownerDir",a},{"tenantId","public"},{"userId","1001"},{"generation",Json::num(4)}}).dump());writeAtomic(c.root/L"users"/wide(a)/L"app-auth"/L"token.json",R"({"encryptedToken":"TEST_ONLY_NOT_A_REAL_CREDENTIAL"})");
        test("JSON Unicode, surrogate pairs and integer precision",[&]{auto j=Json::parse(R"({"name":"\u6d4b\u8bd5\ud83d\ude00","generation":9007199254740990})");require(j["name"].str()=="测试😀","Unicode decoding failed");require(Json::parse(j.dump())["generation"].integer()==9007199254740990LL,"Integer precision lost");rejected([&]{Json::parse(R"({"x":1,"x":2})");});rejected([&]{Json::parse(R"("\ud800")");});});
        test("CRC32C known vector and corrupted record rejection",[&]{require(crc32c("123456789")==0xe3069283,"CRC32C mismatch");auto r=record("hello");require(logRecords(r)[0]=="hello","Log round trip failed");r.back()^=1;rejected([&]{logRecords(r);});});
        test("Snappy overlap copies and bounds checks",[&]{std::string compressed="\x09\x08";compressed+="abc";compressed+="\x09\x03";require(snappy(compressed)=="abcabcabc","Snappy copy failed");rejected([&]{snappy("\x09\x09\x00");});});
        test("Local identity replay and safe report redaction",[&]{auto s=scanAccounts(c);require(s.error.empty()&&s.accounts.size()==2,"Fixture account scan failed");require(s.accounts[0].name=="测试账号 A"&&s.accounts[0].phone=="135****1234","Identity extraction failed");require(publicReport(s).dump().find("secret-token-sentinel")==std::string::npos,"Secret leaked into public report");});
        auto lv=c.root/L"Partitions"/wide("owner%3a"+b)/L"Local Storage"/L"leveldb";
        test("Deleted values supersede old login data",[&]{auto log=readFile(lv/L"000003.log");writeAtomic(lv/L"000003.log",log+record(batch(20,storageKey(),std::nullopt)));auto st=readLevelDb(lv,c.state);require(st.at(storageKey()).deleted,"Tombstone did not win");writeAtomic(lv/L"000003.log",log);});
        test("Orphan log files are ignored",[&]{writeAtomic(lv/L"000001.log",record(batch(999,storageKey(),auth("9999","WRONG"))));auto s=scanAccounts(c);require(s.accounts[1].name=="Test Account B","Orphan log was read");});
        test("Identity mismatch is not displayed as the selected account",[&]{auto log=readFile(lv/L"000003.log");writeAtomic(lv/L"000003.log",record(batch(30,storageKey(),auth("1001","Wrong owner"))));auto s=scanAccounts(c);require(s.accounts[1].identityMismatch&&s.accounts[1].name.empty()&&s.accounts[1].phone.empty(),"Mismatched user information was exposed");rejected([&]{switchAccount(c,b);});writeAtomic(lv/L"000003.log",log);});
        test("Historical metadata is attributed by explicit account ID",[&]{auto lvA=c.root/L"Partitions"/wide("owner%3a"+a)/L"Local Storage"/L"leveldb";auto aBefore=readFile(lvA/L"000003.log"),bBefore=readFile(lv/L"000003.log");writeAtomic(lvA/L"000003.log",record(batch(40,storageKey(),auth("1002","Correct B"))));writeAtomic(lv/L"000003.log",record(batch(40,storageKey(),auth("1001","Correct A"))));auto s=scanAccounts(c);require(s.accounts[0].name=="Correct A"&&s.accounts[1].name=="Correct B","Cached metadata was attributed by directory instead of account ID");require(s.accounts[0].identityFromOtherPartition&&s.accounts[1].identityFromOtherPartition,"Historical source was not marked");writeAtomic(lvA/L"000003.log",aBefore);writeAtomic(lv/L"000003.log",bBefore);});
        test("Invalid identity JSON prevents direct account switching",[&]{auto original=readFile(lv/L"000003.log");writeAtomic(lv/L"000003.log",record(batch(40,storageKey(),std::string(1,'\1')+"{broken")));require(!scanAccounts(c).accounts[1].cacheRead,"Malformed JSON was treated as verified cache");rejected([&]{switchAccount(c,b);});writeAtomic(lv/L"000003.log",original);});
        test("SST manifest selection, block checksums and Snappy",[&]{
            auto key=storageKey(),value=auth("1002","SST identity");auto internal=key+le64((50ULL<<8)|1);std::string block=vi(0)+vi(internal.size())+vi(value.size())+internal+value+le32(0)+le32(1);
            std::string compressed=vi(block.size());size_t len=block.size()-1;if(len<60)compressed+=char(len<<2);else{compressed+=char(61<<2);compressed+=char(len);compressed+=char(len>>8);}compressed+=block;
            auto frame=[&](const std::string& data,char type){std::string check=data+type;return check+le32(maskedCrc(check));};std::string table=frame(compressed,1);
            auto handle=vi(0)+vi(compressed.size());std::string index=vi(0)+vi(internal.size())+vi(handle.size())+internal+handle+le32(0)+le32(1);uint64_t indexOff=table.size();table+=frame(index,0);std::string footer=vi(0)+vi(0)+vi(indexOff)+vi(index.size());footer.resize(40,'\0');footer+=le64(0xdb4775248b80fb57ULL);table+=footer;writeAtomic(lv/L"000009.ldb",table);
            auto old=readFile(lv/L"MANIFEST-000001");auto m=vi(2)+vi(3)+vi(7)+vi(0)+vi(9)+vi(table.size())+fld(internal)+fld(internal);writeAtomic(lv/L"MANIFEST-000001",record(m));auto s=scanAccounts(c);require(s.accounts[1].name=="SST identity","SST was not decoded");writeAtomic(lv/L"MANIFEST-000001",old);require(scanAccounts(c).accounts[1].name=="Test Account B","Orphan SST was read");
        });
        test("Cookie metadata from a live WAL without changing source files",[&]{
            auto net=c.root/L"Partitions"/wide("owner%3a"+a)/L"Network";privateDirectory(net);auto db=net/L"Cookies";Sqlite sql;sqlite3* h=nullptr;if(sql.open(utf8(db.wstring()).c_str(),&h,6,nullptr)!=0)fail("Cannot create fixture cookie database");struct Close{Sqlite& s;sqlite3* h;~Close(){s.close(h);}}close{sql,h};
            using Exec=int(*)(sqlite3*,const char*,int(*)(void*,int,char**,char**),void*,char**);auto exec=address<Exec>(sql.dll,"sqlite3_exec");require(exec!=nullptr,"SQLite exec unavailable");
            const char* query="PRAGMA journal_mode=WAL; CREATE TABLE cookies(host_key TEXT,name TEXT,expires_utc INTEGER,is_persistent INTEGER); INSERT INTO cookies VALUES('.teleai.com.cn','operToken',13439741910695000,1),('teleai.com.cn.evil','operToken',1,1),('.teleai.com.cn','operationToken',0,0);";
            require(exec(h,query,nullptr,nullptr,nullptr)==0,"Cannot populate fixture cookies");auto before=readFile(db);auto walBefore=readFile(fs::path(db.wstring()+L"-wal"));auto s=scanAccounts(c);require(s.accounts[0].cookieRead&&s.accounts[0].cookies.size()==2,"Cookie snapshot did not read WAL or filter hosts");require(readFile(db)==before&&readFile(fs::path(db.wstring()+L"-wal"))==walBefore,"Source cookie database was modified");
        });
        test("Switch and rollback preserve chat databases and generation",[&]{auto chat=readFile(c.root/L"users"/wide(a)/L"teleagent.db");switchAccount(c,b);require(checkedPointer(c)["ownerDir"].str()==b,"Switch failed");require(checkedPointer(c)["generation"].integer()==5,"Generation did not advance");restoreLatest(c);require(checkedPointer(c)["ownerDir"].str()==a&&checkedPointer(c)["generation"].integer()==6,"Rollback did not restore owner with a new generation");require(readFile(c.root/L"users"/wide(a)/L"teleagent.db")==chat,"Chat database changed");});
        test("Pending transition and migration block mutation",[&]{auto original=readFile(c.root/L"current-owner.json");auto p=Json::parse(original);p["pendingOwner"]=Json(Json::Object{{"ownerDir",b}});writeAtomic(c.root/L"current-owner.json",p.dump());rejected([&]{switchAccount(c,b);});writeAtomic(c.root/L"current-owner.json",original);writeAtomic(c.root/L"owner-claim-journal.json",R"({"phase":"running"})");rejected([&]{switchAccount(c,b);});writeAtomic(c.root/L"owner-claim-journal.json",R"({"phase":"completed"})");});
        test("Traversal owner IDs are rejected",[&]{rejected([&]{switchAccount(c,"../../outside");});require(!validOwner("v1_public_../x"),"Traversal accepted");});
        test("Local logout and credential rollback",[&]{auto token=c.root/L"users"/wide(a)/L"app-auth"/L"token.json";auto original=readFile(token);auto part=c.root/L"Partitions"/wide("owner%3a"+a);localLogout(c,a);require(!fs::exists(token)&&!fs::exists(part),"Logout left login data active");require(fs::exists(token.parent_path()/L"relogin-required"),"Logout did not block bootstrap token");restoreLatest(c);require(readFile(token)==original&&fs::exists(part),"Logout rollback failed");require(!fs::exists(token.parent_path()/L"relogin-required"),"Relogin marker was not removed");});
        test("Re-login restores both login files and selected account",[&]{localLogout(c,b,true);require(checkedPointer(c)["ownerDir"].str()==b,"Re-login did not select target");restoreLatest(c);require(checkedPointer(c)["ownerDir"].str()==a,"Re-login undo lost previous owner");require(fs::exists(c.root/L"Partitions"/wide("owner%3a"+b)),"Re-login undo did not restore partition");});
        test("Rollback refuses to overwrite a new login",[&]{localLogout(c,b);auto part=c.root/L"Partitions"/wide("owner%3a"+b);privateDirectory(part);writeAtomic(part/L"new-session","NEW_SESSION");rejected([&]{restoreLatest(c);});require(readFile(part/L"new-session")=="NEW_SESSION","New login was overwritten");fs::rename(part,work/L"new-login-preserved");restoreLatest(c);});
        test("New login preserves all existing owner credentials and chat data",[&]{
            auto pointer=readFile(c.root/L"current-owner.json"),token=readFile(c.root/L"users"/wide(a)/L"app-auth"/L"token.json"),chat=readFile(c.root/L"users"/wide(a)/L"teleagent.db");
            for(auto p:{c.root/L"Network",c.root/L"Local Storage",c.root/L"app-auth",c.root/L"Partitions"/L"owner%3a__device_login__"})privateDirectory(p);
            writeAtomic(c.root/L"Network"/L"fixture-cookie","DEVICE_COOKIE");writeAtomic(c.root/L"Local Storage"/L"fixture-state","DEVICE_STATE");writeAtomic(c.root/L"app-auth"/L"token.json","LEGACY_DEVICE_TOKEN");writeAtomic(c.root/L"Partitions"/L"owner%3a__device_login__"/L"login","DEVICE_LOGIN");
            prepareNewLogin(c);
            require(!fs::exists(c.root/L"current-owner.json")&&!fs::exists(c.root/L"Network")&&!fs::exists(c.root/L"Local Storage"),"Device login was not isolated");
            require(!fs::exists(c.root/L"Partitions"/L"owner%3a__device_login__"),"Device login partition was reused");
            require(readFile(c.root/L"users"/wide(a)/L"app-auth"/L"token.json")==token&&readFile(c.root/L"users"/wide(a)/L"teleagent.db")==chat,"Existing owner data changed");
            require(fs::exists(c.root/L"Partitions"/wide("owner%3a"+a))&&fs::exists(c.root/L"Partitions"/wide("owner%3a"+b)),"Owner partition moved");
            restoreLatest(c);require(checkedPointer(c)["ownerDir"].str()==Json::parse(pointer)["ownerDir"].str(),"Previous account was not restored");require(readFile(c.root/L"Network"/L"fixture-cookie")=="DEVICE_COOKIE"&&readFile(c.root/L"app-auth"/L"token.json")=="LEGACY_DEVICE_TOKEN","Device login backup not restored");
        });
        test("New login restore refuses a newly selected owner",[&]{
            auto pointer=readFile(c.root/L"current-owner.json");prepareNewLogin(c);writeAtomic(c.root/L"current-owner.json",pointer);rejected([&]{restoreLatest(c);});require(readFile(c.root/L"current-owner.json")==pointer,"New login pointer overwritten");fs::remove(c.root/L"current-owner.json");restoreLatest(c);
        });
        test("New login restore refuses newly created browser state",[&]{
            prepareNewLogin(c);privateDirectory(c.root/L"Network");writeAtomic(c.root/L"Network"/L"new-device-cookie","NEW_LOGIN");rejected([&]{restoreLatest(c);});require(readFile(c.root/L"Network"/L"new-device-cookie")=="NEW_LOGIN","New browser login overwritten");fs::rename(c.root/L"Network",work/L"device-login-preserved");restoreLatest(c);
        });
        test("New login preparation refuses an unfinished migration",[&]{
            auto pointer=readFile(c.root/L"current-owner.json");writeAtomic(c.root/L"owner-claim-journal.json",R"({"phase":"running"})");rejected([&]{prepareNewLogin(c);});require(readFile(c.root/L"current-owner.json")==pointer,"Pointer changed during migration");writeAtomic(c.root/L"owner-claim-journal.json",R"({"phase":"completed"})");
        });
        test("Fixture mode cannot launch real TeleAgent",[&]{rejected([&]{launchTeleagent(c);});});
        test("Running-process guard rejects writes without changing the pointer",[&]{wchar_t self[32768];GetModuleFileNameW(nullptr,self,32768);auto helper=fs::absolute(c.root/L"TeleAgent.exe");copyRead(self,helper);std::wstring command=L"\""+helper.wstring()+L"\" --self-test-hold";STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION info{};if(!CreateProcessW(helper.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&info))fail("Cannot create synthetic guard-test process");Handle process(info.hProcess),thread(info.hThread);auto original=readFile(c.root/L"current-owner.json");bool blocked=false;try{switchAccount(c,b);}catch(const std::exception& e){blocked=std::string(e.what()).find("running")!=std::string::npos;}WaitForSingleObject(process,5000);require(blocked,"Running-process guard did not block mutation");require(readFile(c.root/L"current-owner.json")==original,"Pointer changed despite running process");});
    }catch(const std::exception& e){++failed;results.emplace_back(Json::Object{{"test","fixture setup or finalization"},{"passed",false},{"error",std::string(e.what())}});}
    try{privateDirectory(directory);Json result(Json::Object{{"failed",Json::num(failed)},{"total",Json::num(int64_t(results.size()))},{"fixture",utf8(work.wstring())},{"results",results}});writeAtomic(directory/L"selftest-results.json",result.dump()+"\n");}catch(...){return 3;}return failed?2:0;
}

} // namespace ta
