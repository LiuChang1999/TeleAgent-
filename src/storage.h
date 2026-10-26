#pragma once
#include "json.h"
#include <windows.h>
#include <filesystem>
#include <vector>
#include <string>

namespace ta {
namespace fs=std::filesystem;
std::wstring wide(const std::string& s);
std::string utf8(const std::wstring& s);
std::string readFile(const fs::path& p,size_t limit=64*1024*1024);
void writeAtomic(const fs::path& p,const std::string& data);
void privateDirectory(const fs::path& p);
bool validOwner(const std::string& owner);
Json loadJson(const fs::path& path);
struct Cookie {std::string host,name;int64_t expires=0;bool persistent=false;};
struct Account {
    std::string owner,userId,name,phone,cachedId,cachedName,cachedPhone,note,infoSource;
    bool current=false,token=false,relogin=false,identityMismatch=false,cacheRead=false;
    bool cookieRead=false,identityFromOtherPartition=false,live=false,cookieFromCache=false;
    int64_t observedAt=0,cookieObservedAt=0;
    std::vector<Cookie> cookies;
    fs::path partition;
    std::wstring warning;
};
struct Config {fs::path root,configRoot,state,exe;bool fixture=false;};
struct Scan {std::vector<Account> accounts;Json pointer;std::wstring error;};
std::vector<DWORD> teleagentProcesses(const Config& c);
Scan scanAccounts(const Config& c);
void switchAccount(const Config& c,const std::string& owner);
void localLogout(const Config& c,const std::string& owner,bool selectForRelogin=false);
void prepareNewLogin(const Config& c);
void restoreLatest(const Config& c);
void launchTeleagent(const Config& c);
std::wstring formatExpiry(int64_t chromiumMicroseconds);
std::wstring cookieSummary(const Account& a);
Json publicReport(const Scan& s);
int selfTest(const fs::path& directory);
}
