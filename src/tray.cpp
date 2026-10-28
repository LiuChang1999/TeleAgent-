#include "tray.h"
#include <algorithm>
namespace ta {
namespace {
std::wstring oneLine(std::wstring text,size_t limit){for(auto& c:text)if(c<L' ')c=L' ';if(text.size()>limit){text.resize(limit-1);if(!text.empty()&&text.back()>=0xd800&&text.back()<=0xdbff)text.pop_back();text+=L'…';}return text;}
std::wstring boundedTip(const std::wstring& text){auto out=text.substr(0,127);if(!out.empty()&&out.back()>=0xd800&&out.back()<=0xdbff)out.pop_back();return out;}
}
std::wstring traySummary(const Scan& scan,bool running,bool busy,const std::wstring& progress){
    std::wstring account=L"未选择";size_t saved=0;
    for(const auto& a:scan.accounts){if(a.token&&!a.relogin)++saved;if(a.current)account=wide(a.note.empty()?(a.name.empty()?a.userId:a.name):a.note);}
    std::wstring status=busy?(progress.empty()?L"正在处理账号操作":progress):!scan.error.empty()?L"账号信息读取异常":running?L"TeleAgent 运行中":L"TeleAgent 已关闭";
    return boundedTip(L"TeleAgent 账号工作台\n当前账号："+oneLine(account,28)+L"\n"+oneLine(status,36)+L"\n账号 "+std::to_wstring(scan.accounts.size())+L" 个 · 已保存 "+std::to_wstring(saved)+L" 个");
}
std::vector<TrayAccount> trayAccounts(const Scan& scan,const Json& organization,const Json& history){
    std::map<std::string,int64_t> recent;if(history.array())for(auto& row:std::get<Json::Array>(history.v))if(row["action"].str()=="switch"&&row["success"].boolean())recent[row["owner"].str()]=std::max(recent[row["owner"].str()],row["time"].integer());
    auto accounts=scan.accounts;std::stable_sort(accounts.begin(),accounts.end(),[&](auto& a,auto& b){auto x=organization[a.owner]["pinned"].boolean(),y=organization[b.owner]["pinned"].boolean();if(x!=y)return x;return recent[a.owner]>recent[b.owner];});
    std::vector<TrayAccount> out;for(auto& a:accounts){auto label=wide(a.note.empty()?(a.name.empty()?a.userId:a.name):a.note);auto group=organization[a.owner]["group"].str();if(!group.empty())label=L"["+wide(group)+L"] "+label;
        if(organization[a.owner]["pinned"].boolean())label=L"★ "+label;
        if(a.current)label+=L"（当前）";else if(!a.token||a.relogin)label+=L"（需登录）";else if(a.identityMismatch)label+=L"（身份待核对）";
        label=oneLine(label,70);std::wstring escaped;for(auto ch:label){escaped+=ch;if(ch==L'&')escaped+=L'&';}
        out.push_back({a.owner,escaped,a.current,scan.error.empty()&&!a.current&&a.token&&!a.relogin&&!a.identityMismatch});
    }return out;
}
void TrayIcon::initialize(HWND window,UINT callback,HICON icon){data={};data.cbSize=sizeof(data);data.hWnd=window;data.uID=1;data.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;data.uCallbackMessage=callback;data.hIcon=icon;update(L"TeleAgent 账号工作台\n正在读取本机账号资料…");}
bool TrayIcon::setEnabled(bool value){enabled=value;if(!enabled){remove();return true;}return ensure();}
bool TrayIcon::ensure(){if(!enabled)return false;if(added)return true;if(!data.hWnd)return false;added=Shell_NotifyIconW(NIM_ADD,&data)!=FALSE;if(added){data.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&data);}return added;}
bool TrayIcon::update(const std::wstring& text){auto tip=boundedTip(text);std::fill(std::begin(data.szTip),std::end(data.szTip),L'\0');std::copy(tip.begin(),tip.end(),data.szTip);if(added&&!Shell_NotifyIconW(NIM_MODIFY,&data))added=false;return ensure();}
void TrayIcon::explorerRestarted(){added=false;ensure();}
void TrayIcon::notify(const std::wstring& text,bool error){if(!ensure())return;auto notification=data;notification.uFlags=NIF_INFO;notification.dwInfoFlags=error?NIIF_WARNING:NIIF_INFO;auto message=oneLine(text,240);wcsncpy(notification.szInfo,message.c_str(),std::size(notification.szInfo)-1);wcsncpy(notification.szInfoTitle,L"TeleAgent 轮询登录",std::size(notification.szInfoTitle)-1);Shell_NotifyIconW(NIM_MODIFY,&notification);}
void TrayIcon::remove(){if(added)Shell_NotifyIconW(NIM_DELETE,&data);added=false;data.hWnd=nullptr;}
int traySelfTest(const fs::path& directory){
    privateDirectory(directory);Json::Array results;int failed=0;auto test=[&](const char* name,bool ok){if(!ok)++failed;results.emplace_back(Json::Object{{"test",name},{"passed",ok}});};
    Scan scan;Account a;a.current=true;a.token=true;a.name="测试账号";a.note="工作账号";scan.accounts.push_back(a);Account b;b.token=true;b.relogin=true;scan.accounts.push_back(b);
    auto tip=traySummary(scan,true,false,L"");test("Current note and saved account counts",tip.find(L"当前账号：工作账号")!=std::wstring::npos&&tip.find(L"账号 2 个 · 已保存 1 个")!=std::wstring::npos);
    test("Operation progress takes priority",traySummary(scan,true,true,L"正在选择目标账号…").find(L"正在选择目标账号…")!=std::wstring::npos);
    scan.error=L"read failure";test("Read failure is visible",traySummary(scan,false,false,L"").find(L"读取异常")!=std::wstring::npos);
    scan.accounts[0].note=std::string(300,'x');auto longTip=traySummary(scan,true,true,std::wstring(300,L'中'));test("Tooltip fits the Windows 128-unit buffer",longTip.size()<=127&&longTip.find(L"账号 2 个")!=std::wstring::npos);
    std::wstring unicode(126,L'a');unicode+=L"\U0001f600";auto clipped=boundedTip(unicode);test("Tooltip clipping does not split a surrogate pair",clipped.size()==126);
    scan.accounts[0].note="line1\nline2";test("Account labels cannot inject tooltip lines",traySummary(scan,true,false,L"").find(L"line1 line2")!=std::wstring::npos);
    scan.error.clear();scan.accounts[0].owner="v1_public_1";scan.accounts[1].owner="v1_public_2";Account c;c.owner="v1_public_3";c.token=true;c.name="A&B";scan.accounts.push_back(c);
    Json organization(Json::Object{{c.owner,Json(Json::Object{{"pinned",true},{"group","工作"}})}});auto menu=trayAccounts(scan,organization,Json(Json::Array{}));
    test("Pinned accounts lead the menu and labels escape ampersands",menu.front().owner==c.owner&&menu.front().enabled&&menu.front().label.find(L"A&&B")!=std::wstring::npos);
    test("Current and relogin accounts cannot be selected from tray",!menu[1].enabled&&!menu[2].enabled);
    scan.accounts[2].identityMismatch=true;test("Identity mismatches disable quick switching",!trayAccounts(scan,organization,Json(Json::Array{})).front().enabled);
    writeAtomic(directory/L"tray-tests.json",Json(Json::Object{{"failed",Json::num(failed)},{"tests",results}}).dump());return failed?2:0;
}
}
