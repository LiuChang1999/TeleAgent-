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
void TrayIcon::initialize(HWND window,UINT callback,HICON icon){data={};data.cbSize=sizeof(data);data.hWnd=window;data.uID=1;data.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP|NIF_SHOWTIP;data.uCallbackMessage=callback;data.hIcon=icon;update(L"TeleAgent 账号工作台\n正在读取本机账号资料…");}
bool TrayIcon::ensure(){if(added)return true;if(!data.hWnd)return false;added=Shell_NotifyIconW(NIM_ADD,&data)!=FALSE;if(added){data.uVersion=NOTIFYICON_VERSION_4;Shell_NotifyIconW(NIM_SETVERSION,&data);}return added;}
bool TrayIcon::update(const std::wstring& text){auto tip=boundedTip(text);std::fill(std::begin(data.szTip),std::end(data.szTip),L'\0');std::copy(tip.begin(),tip.end(),data.szTip);if(added&&!Shell_NotifyIconW(NIM_MODIFY,&data))added=false;return ensure();}
void TrayIcon::explorerRestarted(){added=false;ensure();}
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
    writeAtomic(directory/L"tray-tests.json",Json(Json::Object{{"failed",Json::num(failed)},{"tests",results}}).dump());return failed?2:0;
}
}
