#include "storage.h"
#include "control.h"
#include "installation.h"
#include "tray.h"
#include "desk.h"
#include "polling.h"
#include "embedded_loader.h"
#include "settings.h"
#include <commdlg.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <WebView2.h>
#include <atomic>
#include <functional>
#include <future>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <mutex>

using namespace ta;
namespace {
HWND window=nullptr;
TrayIcon tray;
constexpr UINT trayMessage=WM_APP+2,trayShow=40001,trayExit=40002,trayAccountBase=41000;
std::vector<std::string> trayOwners;
Json backupSpace;
UINT taskbarCreated=0,restoreMessage=0;
std::wstring instanceKey;
bool restoreMaximized=false;
Config config;fs::path appDirectory;
fs::path managerExecutable;
DesktopSettings preferences;
std::string settingsWarning;
bool fromAutostart=false;
Installation installation;fs::path legacyState;
Scan current;std::future<Scan> pending;
struct ActionResult {bool success=false;std::wstring message;Json space={};};
std::future<ActionResult> operation;
std::mutex progressMutex;
std::wstring operationProgress,lastProgress;
bool scanning=false,acting=false,loaded=false;
bool polling=false;
std::atomic_bool stopPolling=false;
std::vector<std::pair<std::wstring,bool>> pollNotices;
fs::path uiSmokeDirectory;
bool uiReady=false;
bool uiSmokeNoTray=false;
std::wstring uiSmokeFailure;
ULONGLONG uiSmokeStarted=0;
ULONGLONG lastScan=0;
ICoreWebView2Environment* environment=nullptr;
ICoreWebView2Controller* controller=nullptr;
ICoreWebView2* browser=nullptr;
HMODULE loader=nullptr;
std::wstring trustedDocumentUri;
std::string base64(std::string_view data){static const char* alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";std::string out;for(size_t i=0;i<data.size();i+=3){uint32_t n=uint32_t(uint8_t(data[i]))<<16;if(i+1<data.size())n|=uint32_t(uint8_t(data[i+1]))<<8;if(i+2<data.size())n|=uint8_t(data[i+2]);out+=alphabet[(n>>18)&63];out+=alphabet[(n>>12)&63];out+=i+1<data.size()?alphabet[(n>>6)&63]:'=';out+=i+2<data.size()?alphabet[n&63]:'=';}return out;}
void diagnostic(const std::string& key,const Json& value){try{auto path=config.state/L"ui-diagnostics.json";auto data=loadJson(path);data[key]=value;writeAtomic(path,data.dump());}catch(...){}}

template<class Interface,class... Args>class Callback final:public Interface{
    std::atomic<ULONG> refs{1};GUID iid{};std::function<HRESULT(Args...)> fn;
public:
    Callback(const wchar_t* id,std::function<HRESULT(Args...)> callback):fn(std::move(callback)){IIDFromString(id,&iid);}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{if(!out)return E_POINTER;*out=nullptr;if(IsEqualIID(id,IID_IUnknown)||IsEqualIID(id,iid)){*out=static_cast<Interface*>(this);AddRef();return S_OK;}return E_NOINTERFACE;}
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{ULONG n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Invoke(Args... args)override{try{return fn(args...);}catch(...){return E_FAIL;}}
};
using EnvCallback=Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,HRESULT,ICoreWebView2Environment*>;
using ControllerCallback=Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,HRESULT,ICoreWebView2Controller*>;
using MessageCallback=Callback<ICoreWebView2WebMessageReceivedEventHandler,ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs*>;
using NavigationCallback=Callback<ICoreWebView2NavigationStartingEventHandler,ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs*>;
using CompletedCallback=Callback<ICoreWebView2NavigationCompletedEventHandler,ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs*>;

void send(const Json& j){if(browser&&loaded)browser->PostWebMessageAsJson(wide(j.dump()).c_str());}
void toast(const std::wstring& message,bool error=false){send(Json(Json::Object{{"type","toast"},{"message",utf8(message)},{"error",error}}));}
void updateTray(){if(!preferences.trayEnabled){tray.setEnabled(false);return;}try{std::wstring progress;{std::lock_guard<std::mutex> lock(progressMutex);progress=operationProgress;}tray.update(traySummary(current,!teleagentProcesses(config).empty(),acting,progress));}catch(...){tray.update(L"TeleAgent 账号工作台\n账号状态暂时不可用\n点击图标打开工作台");}}
void showWindow(){if(!IsWindowVisible(window)||IsIconic(window))ShowWindow(window,restoreMaximized?SW_SHOWMAXIMIZED:SW_RESTORE);if(controller)controller->put_IsVisible(TRUE);SetForegroundWindow(window);}
void hideToTray(){if(!preferences.trayEnabled){showWindow();return;}updateTray();if(!tray.ensure()){showWindow();MessageBoxW(window,L"暂时无法创建托盘图标，窗口已保留。请稍后重试。",L"TeleAgent",MB_OK|MB_ICONINFORMATION);return;}if(!IsIconic(window))restoreMaximized=IsZoomed(window)!=FALSE;if(controller)controller->put_IsVisible(FALSE);ShowWindow(window,SW_HIDE);}
void requestExit(){if(acting){showWindow();toast(L"账号操作进行中，完成或停止后即可退出",true);return;}DestroyWindow(window);}
void action(const Json& request);
void trayMenu(){
    if(!preferences.trayEnabled)return;
    HMENU menu=CreatePopupMenu();if(!menu)return;AppendMenuW(menu,MF_STRING,trayShow,L"打开账号工作台");AppendMenuW(menu,MF_SEPARATOR,0,nullptr);trayOwners.clear();
    try{
        auto organization=accountOrganization(config);auto history=operationHistory(config);
        auto items=trayAccounts(current,organization,history);
        HMENU accounts=CreatePopupMenu();
        if(accounts){for(auto& item:items){trayOwners.push_back(item.owner);UINT flags=MF_STRING|(item.current?MF_CHECKED:0)|((acting||!item.enabled||config.fixture)?MF_GRAYED:0);AppendMenuW(accounts,flags,trayAccountBase+UINT(trayOwners.size()-1),item.label.c_str());}
            if(items.empty())AppendMenuW(accounts,MF_STRING|MF_GRAYED,0,L"暂无已发现账号");
            AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(accounts),acting?L"切换账号（操作进行中）":L"切换账号 · 置顶优先");}
    }catch(...){AppendMenuW(menu,MF_STRING|MF_GRAYED,0,L"账号列表读取失败，请打开工作台");}
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);AppendMenuW(menu,MF_STRING|(acting?MF_GRAYED:0),trayExit,acting?L"退出（账号操作进行中）":L"退出账号工作台");SetMenuDefaultItem(menu,trayShow,FALSE);POINT point{};GetCursorPos(&point);SetForegroundWindow(window);UINT chosen=UINT(TrackPopupMenu(menu,TPM_RIGHTBUTTON|TPM_RETURNCMD|TPM_NONOTIFY,point.x,point.y,0,window,nullptr));DestroyMenu(menu);PostMessageW(window,WM_NULL,0,0);
    if(chosen>=trayAccountBase&&chosen<trayAccountBase+trayOwners.size()){auto owner=trayOwners[chosen-trayAccountBase];showWindow();action(Json(Json::Object{{"action","switch"},{"owner",owner}}));}
    else if(chosen)PostMessageW(window,WM_COMMAND,chosen,0);
}
std::wstring translate(const std::exception& ex){std::string s=ex.what();
    if(s.find("TeleAgent executable is missing")!=std::string::npos)return L"未找到 TeleAgent 程序，请点击“程序路径”选择已安装的 TeleAgent.exe。账号资料未改动。";
    if(s.find("valid TeleAgent.exe")!=std::string::npos)return L"请选择已安装的 TeleAgent.exe 程序文件。";
    if(s.find("Active agent tasks")!=std::string::npos)return L"TeleAgent 正在执行任务，已保留当前会话。任务结束后即可自动切换。";
    if(s.find("no saved login session")!=std::string::npos)return L"这个账号还没有已保存的登录会话。请先使用“重新登录”完成一次登录。";
    if(s.find("Cached identity")!=std::string::npos)return L"该分区缓存的身份属于其他账号，直接切换已被拦截。请使用“重新登录”，先备份再清理此账号的登录状态。";
    if(s.find("New login")!=std::string::npos)return L"检测到新的登录数据，为避免覆盖，恢复已停止。旧资料仍在备份目录中。";
    if(s.find("No valid backup")!=std::string::npos)return L"还没有可恢复的操作备份。";
    if(s.find("already been restored")!=std::string::npos)return L"最近一次操作已恢复，或备份尚未完整结束。请查看备份目录。";
    if(s.find("transition")!=std::string::npos||s.find("migration")!=std::string::npos||s.find("isolation")!=std::string::npos)return L"TeleAgent 的账号迁移或切换尚未完成。请先打开应用完成当前流程，再进行修改。";
    if(s.find("Could not confirm the target identity")!=std::string::npos)return L"TeleAgent 已打开，但尚未确认目标身份。若出现登录页，请完成登录，程序会自动保存。";
    if(s.find("Cannot validate cached identity")!=std::string::npos)return L"暂时无法核对该账号的身份缓存，已保留现有资料。请刷新，或使用重新登录。";
    if(s.find("Backup preview changed")!=std::string::npos)return L"备份已发生变化，请重新扫描空间后再清理。";
    if(s.find("reparse point")!=std::string::npos)return L"备份路径包含链接或重定向，已停止清理。";
    if(s.find("Group must")!=std::string::npos)return L"分组名称最多 32 个字符，不能包含换行或控制字符。";
    return L"操作未完成："+wide(s)+L"。已有的操作备份保留在本机。";
}
std::string dateOf(const fs::path& path){WIN32_FILE_ATTRIBUTE_DATA a{};if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&a))return {};uint64_t ticks=(uint64_t(a.ftLastWriteTime.dwHighDateTime)<<32)|a.ftLastWriteTime.dwLowDateTime;return utf8(formatExpiry(int64_t(ticks/10)));}
Json::Array backups(){
    Json::Array rows;auto root=config.state/L"backups";if(!fs::is_directory(root))return rows;
    std::vector<fs::path> paths;for(auto& e:fs::directory_iterator(root))if(e.is_directory())paths.push_back(e.path());
    std::sort(paths.begin(),paths.end(),[](auto& a,auto& b){return fs::last_write_time(a)>fs::last_write_time(b);});
    for(auto& path:paths){if(rows.size()>=30)break;try{auto j=loadJson(path/L"transaction.json");if(!j.object())continue;rows.emplace_back(Json::Object{{"id",utf8(path.filename().wstring())},{"action",j["action"]},{"status",j["status"]},{"created",dateOf(path/L"transaction.json")}});}catch(...){}}
    return rows;
}
void publish(){updateTray();if(!loaded)return;try{Json j=publicReport(current);j["type"]="state";j["fixture"]=config.fixture;j["busy"]=acting;j["executable"]=utf8(config.exe.wstring());j["installationSource"]=installation.source;j["executableAvailable"]=validExecutable(config.exe);j["stateDirectory"]=utf8(config.state.wstring());j["dataRoot"]=utf8(config.root.wstring());j["legacyAvailable"]=!legacyState.empty();{std::lock_guard<std::mutex> lock(progressMutex);j["progress"]=utf8(operationProgress);}j["running"]=!teleagentProcesses(config).empty();j["backups"]=backups();j["organization"]=accountOrganization(config);j["history"]=operationHistory(config);j["backupSpace"]=backupSpace;j["polling"]=polling;j["pollStopRequested"]=polling&&stopPolling.load();j["pollReport"]=loadJson(config.state/L"last-poll.json");j["desktopSettings"]=desktopSettingsState(config,preferences,managerExecutable);j["settingsWarning"]=settingsWarning;SYSTEMTIME now{};GetLocalTime(&now);wchar_t date[64];swprintf(date,64,L"%02u:%02u:%02u",now.wHour,now.wMinute,now.wSecond);j["updated"]=utf8(date);send(j);}catch(const std::exception& e){toast(translate(e),true);}}
bool selectExecutable(){
    if(config.fixture){toast(L"模拟模式不保存真实程序路径",true);return false;}
    wchar_t file[32768]{};if(!config.exe.empty())wcsncpy(file,config.exe.c_str(),32767);
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;dialog.lpstrFile=file;dialog.nMaxFile=32768;dialog.lpstrFilter=L"TeleAgent 程序 (TeleAgent.exe)\0TeleAgent.exe\0\0";dialog.lpstrTitle=L"选择已安装的 TeleAgent.exe";dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR|OFN_DONTADDTORECENT;
    if(!GetOpenFileNameW(&dialog))return false;
    rememberInstallation(config.state,fs::path(file));config.exe=fs::path(file);installation={config.exe,"手动选择并保存的路径"};publish();return true;
}
bool ensureExecutable(){
    if(config.fixture)throw std::runtime_error("Automatic app control is disabled for fixture data");
    if(validExecutable(config.exe))return true;
    installation=findInstallation(config.state);config.exe=installation.exe;publish();
    if(!config.exe.empty())return true;
    toast(L"未找到 TeleAgent，请选择已安装的 TeleAgent.exe");return selectExecutable();
}
void collect(){if(!scanning)return;current=pending.get();scanning=false;publish();}
void refresh(){if(scanning||acting)return;lastScan=GetTickCount64();scanning=true;pending=std::async(std::launch::async,[]{try{syncLiveAccount(config);}catch(...){}return scanAccounts(config);});}
void action(const Json& request){
    auto command=request["action"].str(),owner=request["owner"].str();
    if(command=="ready"){uiReady=true;loaded=true;publish();refresh();return;}
    if(command=="refresh"){lastScan=0;refresh();return;}
    if(command=="tray"){hideToTray();return;}
    if(command=="tray-menu"){trayMenu();return;}
    if(command=="poll-stop"){if(polling){stopPolling=true;toast(L"已请求停止，正在等待当前安全步骤完成");}return;}
    if(command=="exit"){requestExit();return;}
    if(acting)return;
    if(scanning)collect();
    try{
        if(command=="settings-save"){
            auto value=parseDesktopSettings(request["settings"]);saveDesktopSettings(config,value,managerExecutable);
            preferences=value;settingsWarning.clear();if(!preferences.trayEnabled&&(!IsWindowVisible(window)||IsIconic(window)))showWindow();
            bool trayReady=tray.setEnabled(preferences.trayEnabled);lastScan=GetTickCount64();publish();send(Json(Json::Object{{"type","settings-saved"}}));
            toast(preferences.trayEnabled&&!trayReady?L"设置已保存，但暂时无法显示托盘图标，窗口会保留":L"设置已保存并生效",preferences.trayEnabled&&!trayReady);return;
        }
        if(command=="state-folder"){privateDirectory(config.state);if(reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",config.state.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)throw std::runtime_error("Cannot open manager data directory");return;}
        if(command=="webview-license"){send(Json(Json::Object{{"type","license"},{"text",embeddedWebViewLicense()}}));return;}
        if(command=="poll-start"){
            if(!ensureExecutable())return;
            std::vector<std::string> owners;if(request["owners"].array())for(auto& item:std::get<Json::Array>(request["owners"].v))owners.push_back(item.str());
            if(owners.empty())throw std::runtime_error("Select at least one account for polling");
            stopPolling=false;polling=true;acting=true;
            {std::lock_guard<std::mutex> lock(progressMutex);operationProgress=L"正在准备轮询登录…";pollNotices.clear();}publish();
            operation=std::async(std::launch::async,[owners](){try{
                auto progress=[](const std::wstring& text){std::lock_guard<std::mutex> lock(progressMutex);operationProgress=text;};
                auto notice=[](const std::wstring& text,bool error){std::lock_guard<std::mutex> lock(progressMutex);pollNotices.emplace_back(text,error);};
                auto report=pollAccounts(config,owners,stopPolling,progress,notice);
                return ActionResult{report["failed"].integer()==0&&!report["halted"].boolean()&&!report["cancelled"].boolean()&&report["restoreSuccess"].boolean(true),wide(report["summary"].str())};
            }catch(const std::exception& e){try{recordOperation(config,"poll","",false,e.what());}catch(...){}return ActionResult{false,translate(e)};}});return;
        }
        if(command=="organize"){saveAccountOrganization(config,owner,request["group"].str(),request["pinned"].boolean());publish();toast(L"分组与置顶已保存");return;}
        if(command=="backup-scan"||command=="backup-cleanup"){
            int keep=int(request["keep"].integer(3));auto preview=request["preview"].str();acting=true;
            {std::lock_guard<std::mutex> lock(progressMutex);operationProgress=command=="backup-scan"?L"正在统计备份占用…":L"正在清理旧备份…";}publish();
            operation=std::async(std::launch::async,[command,keep,preview](){try{
                int removed=0;if(command=="backup-cleanup")removed=cleanupBackups(config,keep,preview);
                auto space=backupInventory(config,keep);
                if(command=="backup-cleanup")recordOperation(config,"cleanup","",true,"已清理 "+std::to_string(removed)+" 份旧备份");
                return ActionResult{true,command=="backup-scan"?L"备份空间统计完成":L"已清理 "+std::to_wstring(removed)+L" 份旧备份",space};
            }catch(const std::exception& e){if(command=="backup-cleanup")try{recordOperation(config,"cleanup","",false,e.what());}catch(...){}return ActionResult{false,translate(e)};}});return;
        }
        if(command=="note"){saveAccountNote(config,owner,request["note"].str());toast(L"账号备注已保存");lastScan=0;refresh();return;}
        if(command=="backups"){privateDirectory(config.state);privateDirectory(config.state/L"backups");auto r=reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",(config.state/L"backups").c_str(),nullptr,nullptr,SW_SHOWNORMAL));if(r<=32)throw std::runtime_error("Cannot open backup directory");return;}
        if(command=="legacy-backups"){if(!legacyState.empty())ShellExecuteW(window,L"open",legacyState.c_str(),nullptr,nullptr,SW_SHOWNORMAL);return;}
        if(command=="choose-exe"){selectExecutable();return;}
        if(command=="open"){if(!ensureExecutable())return;if(teleagentProcesses(config).empty())launchManaged(config);else launchTeleagent(config);toast(L"已打开 TeleAgent");return;}
        if(command!="switch"&&command!="logout"&&command!="relogin"&&command!="restore"&&command!="new-login")return;
        // Resolve before stopping the application or changing any account data.
        if(!ensureExecutable())return;
        acting=true;{std::lock_guard<std::mutex> lock(progressMutex);operationProgress=L"正在准备账号操作…";}
        publish();operation=std::async(std::launch::async,[command,owner](){
            auto progress=[](const std::wstring& phase){std::lock_guard<std::mutex> lock(progressMutex);operationProgress=phase;};
            try{
                if(command=="switch")automaticSwitch(config,owner,progress);
                else if(command=="new-login")automaticNewLogin(config,progress);
                else if(command=="restore"){automaticStop(config,progress);restoreLatest(config);launchManaged(config);}
                else if(command=="logout"){automaticStop(config,progress);localLogout(config,owner);}
                else if(command=="relogin"){automaticStop(config,progress);localLogout(config,owner,true);launchManaged(config);}
                if(command!="switch")recordOperation(config,command,owner,true);
                return ActionResult{true,command=="switch"?L"已自动切换，目标账号登录身份已确认":command=="new-login"?L"已打开新账号登录入口，请手动登录；已有账号资料已保留":command=="relogin"?L"已自动关闭并打开登录界面，请完成一次登录":command=="restore"?L"已自动恢复并重新打开 TeleAgent":L"已备份并在本机退出所选账号"};
            }catch(const std::exception& e){if(command!="switch")try{recordOperation(config,command,owner,false,e.what());}catch(...){}return ActionResult{false,translate(e)};}
        });
    }catch(const std::exception& e){acting=false;polling=false;toast(translate(e),true);}
}
void resize(){if(controller){RECT bounds{};GetClientRect(window,&bounds);controller->put_Bounds(bounds);}}
void startupFailure(const wchar_t* text){if(!uiSmokeDirectory.empty()){uiSmokeFailure=text;return;}MessageBoxW(window,text,L"TeleAgent 账号工作台",MB_OK|MB_ICONERROR);}
void initWebView(){
    privateDirectory(config.state);privateDirectory(config.state/L"webview");
    loader=loadEmbeddedWebView(config.state);
    using Create=HRESULT(STDAPICALLTYPE*)(PCWSTR,PCWSTR,ICoreWebView2EnvironmentOptions*,ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler*);
    auto raw=GetProcAddress(loader,"CreateCoreWebView2EnvironmentWithOptions");Create create=nullptr;static_assert(sizeof(raw)==sizeof(create));std::memcpy(&create,&raw,sizeof(create));
    if(!create){startupFailure(L"WebView2 加载库不完整。");return;}
    auto envHandler=new EnvCallback(L"{4e8a3389-c9d8-4bd2-b6b5-124fee6cc14d}",[](HRESULT result,ICoreWebView2Environment* env)->HRESULT{
        if(FAILED(result)||!env){startupFailure(L"无法启动 WebView2。请确认 Microsoft Edge WebView2 Runtime 可用。");return S_OK;}environment=env;environment->AddRef();
        auto controllerHandler=new ControllerCallback(L"{6c4819f3-c9b7-4260-8127-c9f5bde7f68c}",[](HRESULT hr,ICoreWebView2Controller* value)->HRESULT{
            if(FAILED(hr)||!value){startupFailure(L"无法创建账号工作台界面。");return S_OK;}controller=value;controller->AddRef();controller->put_IsVisible(IsWindowVisible(window));controller->get_CoreWebView2(&browser);if(!browser)return E_FAIL;resize();
            ICoreWebView2Settings* settings=nullptr;if(SUCCEEDED(browser->get_Settings(&settings))&&settings){settings->put_IsStatusBarEnabled(FALSE);settings->put_AreDefaultContextMenusEnabled(FALSE);settings->put_AreDevToolsEnabled(FALSE);settings->put_IsZoomControlEnabled(FALSE);settings->put_IsWebMessageEnabled(TRUE);settings->Release();}
            auto messageHandler=new MessageCallback(L"{57213f19-00e6-49fa-8e07-898ea01ecbd2}",[](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* args)->HRESULT{
                LPWSTR source=nullptr;args->get_Source(&source);bool trusted=source&&(std::wstring(source)==L"about:blank"||std::wstring(source)==trustedDocumentUri);CoTaskMemFree(source);if(!trusted)return S_OK;
                LPWSTR text=nullptr;if(SUCCEEDED(args->TryGetWebMessageAsString(&text))&&text){try{auto request=Json::parse(utf8(text));if(request.object())action(request);}catch(const std::exception& e){toast(translate(e),true);}CoTaskMemFree(text);}return S_OK;
            });EventRegistrationToken token{};browser->add_WebMessageReceived(messageHandler,&token);messageHandler->Release();
            auto navigationHandler=new NavigationCallback(L"{9adbe429-f36d-432b-9ddc-f8881fbd76e3}",[](ICoreWebView2*,ICoreWebView2NavigationStartingEventArgs* args)->HRESULT{LPWSTR uri=nullptr;args->get_Uri(&uri);bool trusted=uri&&(std::wstring(uri)==L"about:blank"||std::wstring(uri)==trustedDocumentUri);diagnostic("navigation",Json(trusted?"embedded-document":"blocked"));if(!trusted)args->put_Cancel(TRUE);CoTaskMemFree(uri);return S_OK;});browser->add_NavigationStarting(navigationHandler,&token);navigationHandler->Release();
            auto completeHandler=new CompletedCallback(L"{d33a35bf-1c49-4f98-93ab-006e0533fe1c}",[](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs* args)->HRESULT{BOOL success=FALSE;args->get_IsSuccess(&success);COREWEBVIEW2_WEB_ERROR_STATUS error{};args->get_WebErrorStatus(&error);diagnostic("navigationSuccess",Json(success!=FALSE));diagnostic("navigationError",Json::num(error));if(success){loaded=true;publish();}return S_OK;});browser->add_NavigationCompleted(completeHandler,&token);completeHandler->Release();
            HRSRC resource=FindResourceW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101),RT_RCDATA);if(!resource){startupFailure(L"界面资源缺失，请重新编译程序。");return E_FAIL;}auto h=LoadResource(GetModuleHandleW(nullptr),resource);auto bytes=static_cast<const char*>(LockResource(h));auto size=SizeofResource(GetModuleHandleW(nullptr),resource);std::string content(bytes,size);trustedDocumentUri=L"data:text/html;charset=utf-8;base64,"+wide(base64(content));auto html=wide(content);auto result=browser->NavigateToString(html.c_str());diagnostic("htmlBytes",Json::num(size));diagnostic("navigateResult",Json::num(result));return S_OK;
        });HRESULT hr=environment->CreateCoreWebView2Controller(window,controllerHandler);controllerHandler->Release();return hr;
    });
    HRESULT hr=create(nullptr,(config.state/L"webview").c_str(),nullptr,envHandler);envHandler->Release();if(FAILED(hr))startupFailure(L"WebView2 初始化失败。");
}
LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){
    if(restoreMessage&&msg==restoreMessage){showWindow();return 0;}
    if(taskbarCreated&&msg==taskbarCreated){if(preferences.trayEnabled){tray.explorerRestarted();if(!tray.ensure())showWindow();}return 0;}
    switch(msg){
    case WM_CREATE:window=h;SetPropW(h,instanceKey.c_str(),reinterpret_cast<HANDLE>(1));tray.setEnabled(preferences.trayEnabled);tray.initialize(h,trayMessage,LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(102)));SetTimer(h,1,250,nullptr);refresh();PostMessageW(h,WM_APP+1,0,0);return 0;
    case WM_APP+1:try{initWebView();}catch(const std::exception& e){startupFailure(wide(e.what()).c_str());}return 0;
    case WM_SYSCOMMAND:if((wp&0xfff0)==SC_MINIMIZE&&preferences.trayEnabled&&preferences.minimizeToTray){hideToTray();return 0;}break;
    case WM_SIZE:if(wp==SIZE_MINIMIZED){if(preferences.trayEnabled&&preferences.minimizeToTray)hideToTray();else if(controller)controller->put_IsVisible(FALSE);return 0;}if(controller)controller->put_IsVisible(IsWindowVisible(h));resize();return 0;
    case trayMessage:switch(LOWORD(lp)){case NIN_SELECT:case NIN_KEYSELECT:case WM_LBUTTONUP:case WM_LBUTTONDBLCLK:showWindow();break;case WM_CONTEXTMENU:trayMenu();break;}return 0;
    case WM_COMMAND:if(LOWORD(wp)==trayShow){showWindow();return 0;}if(LOWORD(wp)==trayExit){requestExit();return 0;}break;
    case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={1000,700};return 0;
    case WM_TIMER:
        if(!uiSmokeDirectory.empty()&&(uiReady||!uiSmokeFailure.empty()||GetTickCount64()-uiSmokeStarted>20000)){
            if(uiSmokeNoTray&&tray.ensure())uiSmokeFailure=L"Tray unexpectedly enabled";
            bool passed=uiReady&&uiSmokeFailure.empty();writeAtomic(uiSmokeDirectory/(uiSmokeNoTray?L"ui-no-tray-tests.json":L"ui-tests.json"),Json(Json::Object{{"passed",passed},{"javascriptReady",uiReady},{"embeddedLoader",loader!=nullptr},{"trayEnabled",preferences.trayEnabled},{"error",utf8(uiSmokeFailure.empty()?(passed?L"":L"WebView2 startup timed out"):uiSmokeFailure)}}).dump());if(uiSmokeNoTray)SendMessageW(h,WM_CLOSE,0,0);else DestroyWindow(h);return 0;
        }
        {std::vector<std::pair<std::wstring,bool>> notices;{std::lock_guard<std::mutex> lock(progressMutex);notices.swap(pollNotices);}for(auto& entry:notices){toast(entry.first,entry.second);if(preferences.notifications&&preferences.trayEnabled)tray.notify(entry.first,entry.second);}}
        if(acting){
            if(operation.valid()&&operation.wait_for(std::chrono::seconds(0))==std::future_status::ready){auto result=operation.get();bool wasPolling=polling;acting=false;polling=false;backupSpace=result.space;{std::lock_guard<std::mutex> lock(progressMutex);operationProgress.clear();}toast(result.message,!result.success);if(wasPolling&&preferences.notifications&&preferences.trayEnabled)tray.notify(result.message,!result.success);publish();lastScan=0;refresh();}
            else{std::wstring phase;{std::lock_guard<std::mutex> lock(progressMutex);phase=operationProgress;}if(phase!=lastProgress){lastProgress=phase;publish();}}
        }else{if(scanning&&pending.wait_for(std::chrono::seconds(0))==std::future_status::ready)collect();if(!scanning&&preferences.refreshSeconds>0&&GetTickCount64()-lastScan>=ULONGLONG(preferences.refreshSeconds)*1000)refresh();}return 0;
    case WM_CLOSE:switch(windowCloseDecision(preferences,acting)){case CloseDecision::Tray:hideToTray();break;case CloseDecision::Exit:requestExit();break;case CloseDecision::KeepVisible:showWindow();toast(L"账号操作进行中，请完成或停止后再退出",true);break;}return 0;
    case WM_DESTROY:tray.remove();RemovePropW(h,instanceKey.c_str());KillTimer(h,1);if(scanning){pending.wait();scanning=false;}if(controller){controller->Close();controller->Release();controller=nullptr;}if(browser){browser->Release();browser=nullptr;}if(environment){environment->Release();environment=nullptr;}PostQuitMessage(0);return 0;
    case WM_ERASEBKGND:{RECT r{};GetClientRect(h,&r);auto brush=CreateSolidBrush(RGB(245,247,251));FillRect(reinterpret_cast<HDC>(wp),&r,brush);DeleteObject(brush);return 1;}
    }return DefWindowProcW(h,msg,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int show){
    try{
        SetProcessDPIAware();SetCurrentProcessExplicitAppUserModelID(L"Local.TeleAgentAccountDesk");wchar_t path[32768];GetModuleFileNameW(nullptr,path,32768);managerExecutable=fs::path(path);appDirectory=managerExecutable.parent_path();wchar_t home[MAX_PATH];if(FAILED(SHGetFolderPathW(nullptr,CSIDL_PROFILE,nullptr,SHGFP_TYPE_CURRENT,home)))throw std::runtime_error("Cannot find Windows profile");
        config.root=fs::path(home)/L".local"/L"share"/L"TeleAgent";config.configRoot=fs::path(home)/L".config"/L"TeleAgent";config.state=userStateDirectory();
        for(auto p:{appDirectory/L"manager-data",appDirectory.filename()==L"build"?appDirectory.parent_path()/L"manager-data":appDirectory/L"manager-data"}){std::error_code ec;if(fs::is_directory(p,ec)&&!ec){legacyState=p;break;}}
        int argc=0;LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);fs::path report,test,controlTest,pathTest,pathReport,trayTest,deskTest,pollTest,loaderTest,settingsTest;std::string autoOwner;if(argc==2&&std::wstring(argv[1])==L"--self-test-hold"){LocalFree(argv);Sleep(2500);return 0;}
        for(int i=1;i<argc;++i){std::wstring arg=argv[i];if(arg==L"--data-root"&&i+1<argc){config.root=argv[++i];config.fixture=true;}else if(arg==L"--state-root"&&i+1<argc)config.state=argv[++i];else if(arg==L"--teleagent-exe"&&i+1<argc)config.exe=argv[++i];else if(arg==L"--autostart")fromAutostart=true;else if(arg==L"--settings-test"&&i+1<argc)settingsTest=argv[++i];else if(arg==L"--ui-no-tray-test"&&i+1<argc){uiSmokeDirectory=argv[++i];uiSmokeNoTray=true;}else if(arg==L"--ui-test"&&i+1<argc)uiSmokeDirectory=argv[++i];else if(arg==L"--poll-test"&&i+1<argc)pollTest=argv[++i];else if(arg==L"--loader-test"&&i+1<argc)loaderTest=argv[++i];else if(arg==L"--desk-test"&&i+1<argc)deskTest=argv[++i];else if(arg==L"--tray-test"&&i+1<argc)trayTest=argv[++i];else if(arg==L"--path-test"&&i+1<argc)pathTest=argv[++i];else if(arg==L"--path-report"&&i+1<argc)pathReport=argv[++i];else if(arg==L"--auto-switch"&&i+1<argc)autoOwner=utf8(argv[++i]);else if(arg==L"--control-test"&&i+1<argc)controlTest=argv[++i];else if(arg==L"--report"&&i+1<argc)report=argv[++i];else if(arg==L"--self-test"&&i+1<argc)test=argv[++i];else throw std::runtime_error("Unsupported command-line argument");}LocalFree(argv);
        if(!test.empty())return selfTest(test);
        if(!controlTest.empty())return controlSelfTest(controlTest);
        if(!pathTest.empty())return installationSelfTest(pathTest);
        if(!trayTest.empty())return traySelfTest(trayTest);
        if(!deskTest.empty())return deskSelfTest(deskTest);
        if(!pollTest.empty())return pollingSelfTest(pollTest);
        if(!loaderTest.empty())return loaderSelfTest(loaderTest);
        if(!settingsTest.empty())return settingsSelfTest(settingsTest);
        if(!uiSmokeDirectory.empty()){privateDirectory(uiSmokeDirectory);auto fixture=fs::absolute(uiSmokeDirectory)/(L"ui-fixture-"+std::to_wstring(GetTickCount64()));config.fixture=true;config.root=fixture/L"data";config.state=fixture/L"state";privateDirectory(config.root);uiSmokeStarted=GetTickCount64();show=SW_HIDE;}
        preferences=loadDesktopSettings(config.state,&settingsWarning);
        if(uiSmokeNoTray){auto value=settingsJson(preferences);value["trayEnabled"]=false;preferences=parseDesktopSettings(value);}
        if(config.exe.empty()&&!config.fixture){installation=findInstallation(config.state);config.exe=installation.exe;}
        if(!pathReport.empty()){writeAtomic(pathReport,Json(Json::Object{{"executable",utf8(config.exe.wstring())},{"available",validExecutable(config.exe)},{"source",installation.source},{"stateDirectory",utf8(config.state.wstring())},{"dataRoot",utf8(config.root.wstring())}}).dump());return validExecutable(config.exe)?0:2;}
        if(!autoOwner.empty()){
            if(report.empty()){privateDirectory(config.state);report=config.state/L"last-switch-result.json";}
            bool success=false;std::string reason;Json::Array phases;
            try{automaticSwitch(config,autoOwner,[&](const std::wstring& phase){phases.push_back(utf8(phase));writeAtomic(fs::path(report.wstring()+L".progress.json"),Json(Json::Object{{"phase",utf8(phase)}}).dump());});success=true;}catch(const std::exception& e){reason=e.what();}
            auto result=publicReport(scanAccounts(config));result["operationSuccess"]=success;result["operationError"]=reason;result["phases"]=phases;writeAtomic(report,result.dump());return success?0:2;
        }
        if(!report.empty()){auto scan=scanAccounts(config);writeAtomic(report,publicReport(scan).dump()+"\n");return scan.error.empty()?0:2;}
        instanceKey=L"Local\\TeleAgentAccounts-"+std::to_wstring(std::hash<std::wstring>{}(fs::absolute(config.root).wstring()));restoreMessage=RegisterWindowMessageW((instanceKey+L".Show").c_str());taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
        HANDLE mutex=CreateMutexW(nullptr,TRUE,instanceKey.c_str());if(!mutex||GetLastError()==ERROR_ALREADY_EXISTS){if(fromAutostart){if(mutex)CloseHandle(mutex);return 0;}HWND existing=nullptr;EnumWindows([](HWND h,LPARAM data)->BOOL{if(GetPropW(h,instanceKey.c_str())){*reinterpret_cast<HWND*>(data)=h;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&existing));if(existing&&restoreMessage){DWORD pid=0;GetWindowThreadProcessId(existing,&pid);AllowSetForegroundWindow(pid);PostMessageW(existing,restoreMessage,0,0);}else MessageBoxW(nullptr,L"账号工作台已经打开。",L"TeleAgent",MB_OK|MB_ICONINFORMATION);if(mutex)CloseHandle(mutex);return 0;}
        HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);if(FAILED(com))throw std::runtime_error("Cannot initialize the desktop UI");
        WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.lpfnWndProc=proc;cls.hInstance=instance;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(102));cls.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(102),IMAGE_ICON,16,16,0));cls.lpszClassName=L"TeleAgentModernAccountDesk";RegisterClassExW(&cls);RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);int width=std::min(1380L,work.right-work.left-60),height=std::min(880L,work.bottom-work.top-50);window=CreateWindowExW(0,cls.lpszClassName,config.fixture?L"TeleAgent · 账号工作台（模拟数据）":L"TeleAgent · 账号工作台",WS_OVERLAPPEDWINDOW,work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,width,height,nullptr,nullptr,instance,nullptr);if(!window)throw std::runtime_error("Cannot create application window");
        BOOL dark=FALSE;DwmSetWindowAttribute(window,20,&dark,sizeof(dark));if(uiSmokeDirectory.empty()&&startHidden(preferences,fromAutostart))hideToTray();else ShowWindow(window,show);UpdateWindow(window);MSG message{};while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}if(loader)FreeLibrary(loader);CoUninitialize();ReleaseMutex(mutex);CloseHandle(mutex);return !uiSmokeDirectory.empty()&&(!uiReady||!uiSmokeFailure.empty())?2:int(message.wParam);
    }catch(const std::exception& e){MessageBoxW(nullptr,wide(e.what()).c_str(),L"TeleAgent · 启动失败",MB_OK|MB_ICONERROR);return 1;}
}
