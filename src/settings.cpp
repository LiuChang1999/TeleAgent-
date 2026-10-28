#include "settings.h"
#include <algorithm>
#include <functional>
#include <cstring>

namespace ta {
namespace {
constexpr const wchar_t* runKey=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* runValue=L"TeleAgentAccountDesk";
struct Key {HKEY value=nullptr;~Key(){if(value)RegCloseKey(value);}};
struct RegistryValue {bool exists=false;DWORD type=REG_NONE;std::vector<BYTE> bytes;bool operator==(const RegistryValue&)const=default;};
[[noreturn]]void fail(const std::string& text){throw std::runtime_error(text);}
RegistryValue readStartup(const std::wstring& keyPath){
    Key key;auto status=RegOpenKeyExW(HKEY_CURRENT_USER,keyPath.c_str(),0,KEY_QUERY_VALUE,&key.value);
    if(status==ERROR_FILE_NOT_FOUND)return {};
    if(status!=ERROR_SUCCESS)fail("无法读取开机启动项（Windows 错误 "+std::to_string(status)+"）");
    RegistryValue result;DWORD size=0;status=RegQueryValueExW(key.value,runValue,nullptr,&result.type,nullptr,&size);
    if(status==ERROR_FILE_NOT_FOUND)return result;
    if(status!=ERROR_SUCCESS||size>65536)fail("开机启动项无法读取或内容过长");
    result.bytes.resize(size);status=RegQueryValueExW(key.value,runValue,nullptr,&result.type,result.bytes.data(),&size);
    if(status!=ERROR_SUCCESS)fail("开机启动项在读取时发生变化，请重试");
    result.bytes.resize(size);result.exists=true;return result;
}
std::wstring textOf(const RegistryValue& value){
    if(!value.exists||value.type!=REG_SZ||value.bytes.size()%sizeof(wchar_t))return {};
    std::wstring text(value.bytes.size()/sizeof(wchar_t),L'\0');if(!text.empty())std::memcpy(text.data(),value.bytes.data(),value.bytes.size());
    if(!text.empty()&&text.back()==L'\0')text.pop_back();
    return text;
}
std::wstring startupCommand(const fs::path& executable){
    if(!executable.is_absolute()||_wcsicmp(executable.extension().c_str(),L".exe")!=0)fail("开机启动程序必须是完整的 EXE 路径");
    auto path=executable.lexically_normal().wstring();if(path.find(L'"')!=std::wstring::npos||std::any_of(path.begin(),path.end(),[](wchar_t c){return c<32;}))fail("开机启动程序路径无效");
    auto command=L"\""+path+L"\" --autostart";
    if(command.size()>260)fail("程序路径过长，请移到较短目录后再启用开机自启");
    return command;
}
RegistryValue startupValue(const fs::path& executable){
    auto command=startupCommand(executable);RegistryValue value;value.exists=true;value.type=REG_SZ;value.bytes.resize((command.size()+1)*sizeof(wchar_t));std::memcpy(value.bytes.data(),command.c_str(),value.bytes.size());return value;
}
void writeStartup(const std::wstring& keyPath,const RegistryValue& value){
    Key key;LSTATUS status;
    if(value.exists)status=RegCreateKeyExW(HKEY_CURRENT_USER,keyPath.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key.value,nullptr);
    else {status=RegOpenKeyExW(HKEY_CURRENT_USER,keyPath.c_str(),0,KEY_SET_VALUE,&key.value);if(status==ERROR_FILE_NOT_FOUND)return;}
    if(status!=ERROR_SUCCESS)fail("无法修改开机启动项（Windows 错误 "+std::to_string(status)+"）");
    status=value.exists?RegSetValueExW(key.value,runValue,0,value.type,value.bytes.data(),DWORD(value.bytes.size())):RegDeleteValueW(key.value,runValue);
    if(status!=ERROR_SUCCESS&&!(status==ERROR_FILE_NOT_FOUND&&!value.exists))fail("开机启动项保存失败（Windows 错误 "+std::to_string(status)+"）");
}
DesktopSettings normalized(DesktopSettings value){
    if(value.closeAction!="tray"&&value.closeAction!="exit")fail("关闭窗口行为无效");
    if(value.refreshSeconds!=0&&value.refreshSeconds!=3&&value.refreshSeconds!=5&&value.refreshSeconds!=10&&value.refreshSeconds!=30)fail("刷新间隔只能选择手动、3、5、10 或 30 秒");
    if(!value.trayEnabled){value.closeAction="exit";value.minimizeToTray=false;value.startInTray=false;value.autostartInTray=false;}
    return value;
}
void save(const Config& c,DesktopSettings value,const fs::path& executable,const std::wstring& keyPath){
    value=normalized(value);privateDirectory(c.state);
    if(c.fixture){if(value.autostart)fail("模拟模式不允许登记真实开机启动项");writeAtomic(c.state/L"desktop-settings.json",settingsJson(value).dump());return;}
    auto before=readStartup(keyPath),after=value.autostart?startupValue(executable):RegistryValue{};
    bool changed=before!=after;if(changed)writeStartup(keyPath,after);
    try{writeAtomic(c.state/L"desktop-settings.json",settingsJson(value).dump());}
    catch(...){if(changed)try{writeStartup(keyPath,before);}catch(...){fail("设置文件保存失败，且启动项回滚失败，请检查设置中心中的启动项状态");}throw;}
}
}
Json settingsJson(const DesktopSettings& settings){auto value=normalized(settings);return Json(Json::Object{{"trayEnabled",value.trayEnabled},{"minimizeToTray",value.minimizeToTray},{"closeAction",value.closeAction},{"startInTray",value.startInTray},{"autostart",value.autostart},{"autostartInTray",value.autostartInTray},{"notifications",value.notifications},{"refreshSeconds",Json::num(value.refreshSeconds)}});}
DesktopSettings parseDesktopSettings(const Json& value){
    if(!value.object())fail("设置内容格式无效");
    DesktopSettings out;
    auto boolean=[&](const char* name,bool& target){if(value.has(name)){if(!std::holds_alternative<bool>(value[name].v))fail("设置开关必须为布尔值");target=value[name].boolean();}};
    boolean("trayEnabled",out.trayEnabled);boolean("minimizeToTray",out.minimizeToTray);boolean("startInTray",out.startInTray);boolean("autostart",out.autostart);boolean("autostartInTray",out.autostartInTray);boolean("notifications",out.notifications);
    if(value.has("closeAction"))out.closeAction=value["closeAction"].str();
    if(value.has("refreshSeconds")){auto n=value["refreshSeconds"].integer(-1);if(n<0||n>30)fail("刷新间隔无效");out.refreshSeconds=int(n);}
    return normalized(out);
}
DesktopSettings loadDesktopSettings(const fs::path& state,std::string* warning){
    try{auto value=loadJson(state/L"desktop-settings.json");if(value.null())return {};return parseDesktopSettings(value);}
    catch(...){if(warning)*warning="设置文件无法读取，当前使用默认设置；保存后可修复。";return {};}
}
void saveDesktopSettings(const Config& c,const DesktopSettings& value,const fs::path& executable){save(c,value,executable,runKey);}
Json desktopSettingsState(const Config& c,const DesktopSettings& value,const fs::path& executable){
    auto state=settingsJson(value);state["currentExecutable"]=utf8(executable.wstring());state["autostartSupported"]=!c.fixture;
    if(c.fixture){state["autostart"]=false;state["startupStatus"]="模拟模式：不会修改 Windows 启动项";return state;}
    try{auto registered=readStartup(runKey);state["autostart"]=registered.exists;bool matches=registered.exists&&registered==startupValue(executable);state["startupMatchesCurrent"]=matches;state["startupCommand"]=utf8(textOf(registered));state["startupStatus"]=registered.exists?(matches?"已登记当前程序，登录 Windows 后启动":"启动项指向其他位置，保存设置可更新为当前程序"):"未启用开机自启";}
    catch(const std::exception& e){state["startupError"]=e.what();state["startupStatus"]="无法核对启动项，请查看错误后重试";}
    return state;
}
CloseDecision windowCloseDecision(const DesktopSettings& value,bool busy){if(busy)return value.trayEnabled?CloseDecision::Tray:CloseDecision::KeepVisible;return value.trayEnabled&&value.closeAction=="tray"?CloseDecision::Tray:CloseDecision::Exit;}
bool startHidden(const DesktopSettings& value,bool fromAutostart){return value.trayEnabled&&(fromAutostart?value.autostartInTray:value.startInTray);}
int settingsSelfTest(const fs::path& directory){
    privateDirectory(directory);auto work=directory/(L"settings-fixture-"+std::to_wstring(GetTickCount64()));Config c;c.fixture=false;c.root=work/L"data";c.state=work/L"state";privateDirectory(c.state);auto executable=fs::absolute(work/L"中文 路径"/L"TeleAgentAccounts.exe");
    // Only an isolated test key is mutated; never the real Windows Run key.
    const std::wstring prefix=L"Software\\TeleAgentAccountDeskTests\\";auto testKey=prefix+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"\\Run";
    Json::Array results;int failed=0;auto require=[](bool ok){if(!ok)fail("Assertion failed");};auto rejected=[&](auto f){bool blocked=false;try{f();}catch(...){blocked=true;}require(blocked);};
    auto test=[&](const char* name,auto f){try{f();results.emplace_back(Json::Object{{"test",name},{"passed",true}});}catch(const std::exception& e){++failed;results.emplace_back(Json::Object{{"test",name},{"passed",false},{"error",e.what()}});}};
    test("Defaults retain tray behavior and leave autostart disabled",[&]{auto v=loadDesktopSettings(c.state);require(v.trayEnabled&&v.minimizeToTray&&v.closeAction=="tray"&&!v.autostart&&!v.startInTray&&v.refreshSeconds==3);});
    test("Disabling tray removes every hidden startup and close path",[&]{auto j=settingsJson({});j["trayEnabled"]=false;j["startInTray"]=true;auto v=parseDesktopSettings(j);require(!v.minimizeToTray&&!v.startInTray&&!v.autostartInTray&&v.closeAction=="exit"&&!startHidden(v,true)&&!startHidden(v,false));require(windowCloseDecision(v,false)==CloseDecision::Exit&&windowCloseDecision(v,true)==CloseDecision::KeepVisible);});
    test("Busy operations cannot be exited by close preferences",[&]{DesktopSettings v;v.closeAction="exit";require(windowCloseDecision(v,true)==CloseDecision::Tray&&windowCloseDecision(v,false)==CloseDecision::Exit);});
    test("Manual and logon launches have independent tray preferences",[&]{DesktopSettings v;require(!startHidden(v,false)&&startHidden(v,true));v.startInTray=true;v.autostartInTray=false;require(startHidden(v,false)&&!startHidden(v,true));});
    test("Invalid settings fail before persistence or startup changes",[&]{auto j=settingsJson({});j["refreshSeconds"]=Json::num(1);rejected([&]{parseDesktopSettings(j);});j["refreshSeconds"]=Json::num(3);j["trayEnabled"]="yes";rejected([&]{parseDesktopSettings(j);});});
    test("Autostart quotes Chinese paths and toggles only its own test value",[&]{DesktopSettings v;v.autostart=true;save(c,v,executable,testKey);require(textOf(readStartup(testKey))==L"\""+executable.wstring()+L"\" --autostart");require(loadDesktopSettings(c.state).autostart);v.autostart=false;save(c,v,executable,testKey);require(!readStartup(testKey).exists);});
    test("Moving the executable repairs startup only on explicit save",[&]{DesktopSettings v;v.autostart=true;save(c,v,executable,testKey);auto before=readStartup(testKey);auto moved=executable.parent_path()/L"Renamed.exe";require(before!=startupValue(moved));save(c,v,moved,testKey);require(readStartup(testKey)==startupValue(moved));});
    test("A failed settings file write rolls back the startup value",[&]{auto before=readStartup(testKey);Config blocked=c;blocked.state=work/L"blocked";privateDirectory(blocked.state/L"desktop-settings.json");DesktopSettings v;v.autostart=false;rejected([&]{save(blocked,v,executable,testKey);});require(readStartup(testKey)==before);});
    test("Fixture saves never alter startup and reject enabling it",[&]{Config fixture=c;fixture.fixture=true;auto before=readStartup(testKey);DesktopSettings v;v.autostart=true;rejected([&]{save(fixture,v,executable,testKey);});v.autostart=false;v.refreshSeconds=0;save(fixture,v,executable,testKey);require(readStartup(testKey)==before&&loadDesktopSettings(fixture.state).refreshSeconds==0);});
    test("Corrupt preferences fall back without overwriting the file",[&]{writeAtomic(c.state/L"desktop-settings.json","broken-json");std::string warning;auto v=loadDesktopSettings(c.state,&warning);require(!warning.empty()&&!v.autostart&&readFile(c.state/L"desktop-settings.json")=="broken-json");});
    if(testKey.rfind(prefix,0)==0)RegDeleteTreeW(HKEY_CURRENT_USER,testKey.substr(0,testKey.size()-4).c_str());
    writeAtomic(directory/L"settings-tests.json",Json(Json::Object{{"failed",Json::num(failed)},{"tests",results}}).dump());return failed?2:0;
}
}
