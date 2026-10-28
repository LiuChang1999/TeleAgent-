#include "polling.h"
#include "installation.h"
#include "desk.h"
#include <chrono>
#include <set>
#include <algorithm>

namespace ta {
namespace {
int64_t timestamp(){return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
std::string reasonFor(const std::string& error){
    if(error.find("Login cookie has expired")!=std::string::npos)return "本地登录 Cookie 已到期，需要手动重新登录";
    if(error.find("no saved login session")!=std::string::npos)return "没有已保存的登录资料，需要手动重新登录";
    if(error.find("Cached identity")!=std::string::npos)return "分区身份不一致，已阻止复用登录，请手动重新登录";
    if(error.find("Could not confirm the target identity")!=std::string::npos)return "40 秒内未识别目标登录身份，可能已过期或需要手动登录";
    if(error.find("different account")!=std::string::npos)return "TeleAgent 打开了其他账号，未保存为本次目标账号";
    if(error.find("Active agent tasks")!=std::string::npos)return "检测到正在执行的任务，已停止轮询，请等待任务完成";
    if(error.find("did not finish shutting down")!=std::string::npos)return "TeleAgent 未能完整关闭，已停止轮询";
    if(error.find("executable is missing")!=std::string::npos)return "未找到 TeleAgent 程序，已停止轮询";
    return "操作未完成："+error;
}
bool recoverable(const std::string& error){
    for(auto text:{"no saved login session","Cached identity","Could not confirm the target identity","different account","Login cookie has expired"})if(error.find(text)!=std::string::npos)return true;
    return false;
}
struct Target{std::string owner,label;};
using Attempt=std::function<void(const Target&,Progress)>;
Json run(const Config& c,const std::vector<Target>& targets,std::atomic_bool& stop,const Attempt& login,const Attempt& save,const std::function<void()>& restore,Progress progress,PollNotice notice){
    Json report(Json::Object{{"startedAt",Json::num(timestamp())},{"running",true},{"total",Json::num(int64_t(targets.size()))},{"results",Json::Array{}},{"succeeded",Json::num(0)},{"failed",Json::num(0)},{"cancelled",false},{"halted",false}});
    int succeeded=0,failed=0;bool cancelled=false,halted=false;
    auto persist=[&]{report["succeeded"]=Json::num(succeeded);report["failed"]=Json::num(failed);report["cancelled"]=cancelled;report["halted"]=halted;writeAtomic(c.state/L"last-poll.json",report.dump());};
    auto log=[&](const std::string& action,const std::string& owner,bool success,const std::string& reason){try{recordOperation(c,action,owner,success,reason);}catch(...){report["historyWarning"]="操作历史写入失败，结果仍保存在轮询报告";}};
    privateDirectory(c.state);persist();
    for(size_t index=0;index<targets.size();++index){
        if(stop.load()){cancelled=true;break;}
        auto& target=targets[index];std::wstring prefix=L"轮询 "+std::to_wstring(index+1)+L"/"+std::to_wstring(targets.size())+L" · "+wide(target.label)+L"：";
        report["currentOwner"]=target.owner;persist();auto phase=[&](const std::wstring& text){if(progress)progress(prefix+text);};
        Json item(Json::Object{{"owner",target.owner},{"name",target.label},{"time",Json::num(timestamp())}});bool saving=false;
        try{
            login(target,phase);if(stop.load())throw PollCancelled();
            saving=true;save(target,phase);
            ++succeeded;item["status"]="success";item["reason"]="目标身份已核对，登录快照已保存";log("poll-account",target.owner,true,item["reason"].str());
        }catch(const PollCancelled&){cancelled=true;item["status"]="cancelled";item["reason"]="用户停止轮询，保留当前账号现场";log("poll-account",target.owner,false,item["reason"].str());}
        catch(const std::exception& e){
            ++failed;item["status"]="failed";item["reason"]=reasonFor(e.what());halted=saving||!recoverable(e.what());
            log("poll-account",target.owner,false,item["reason"].str());if(notice)notice(wide(target.label+"："+item["reason"].str()),true);
        }
        std::get<Json::Array>(report["results"].v).push_back(item);persist();
        if(cancelled||halted)break;
    }
    if(stop.load())cancelled=true;
    if(!cancelled&&!halted&&restore){
        if(progress)progress(L"轮询结束，正在回到开始时的账号…");
        try{restore();report["restoreSuccess"]=true;}
        catch(const PollCancelled&){cancelled=true;report["restoreSuccess"]=false;report["restoreReason"]="用户停止，已保留当前账号";}
        catch(const std::exception& e){report["restoreSuccess"]=false;report["restoreReason"]=reasonFor(e.what());log("poll-restore","",false,report["restoreReason"].str());if(notice)notice(L"未能回到原账号："+wide(report["restoreReason"].str()),true);}
    }
    report["running"]=false;report["completedAt"]=Json::num(timestamp());report["currentOwner"]="";
    report["remaining"]=Json::num(int64_t(targets.size()-std::get<Json::Array>(report["results"].v).size()));persist();
    std::string summary="成功保存 "+std::to_string(succeeded)+" 个，失败 "+std::to_string(failed)+" 个，未执行 "+std::to_string(report["remaining"].integer())+" 个";
    int interrupted=0;for(auto& item:std::get<Json::Array>(report["results"].v))if(item["status"].str()=="cancelled")++interrupted;
    report["cancelledCount"]=Json::num(interrupted);if(interrupted)summary+="，中途停止 "+std::to_string(interrupted)+" 个";
    if(cancelled)summary="轮询已停止；"+summary;else if(halted)summary="轮询因安全检查或保存异常中止；"+summary;else summary="轮询完成；"+summary;
    if(report.has("restoreSuccess")&&!report["restoreSuccess"].boolean())summary+="；未能回到原账号，请查看结果";
    report["summary"]=summary;persist();log("poll","",failed==0&&!cancelled&&!halted&&report["restoreSuccess"].boolean(true),summary);
    return report;
}
}
Json pollAccounts(const Config& c,const std::vector<std::string>& owners,std::atomic_bool& stop,Progress progress,PollNotice notice){
    if(c.fixture)throw std::runtime_error("Automatic app control is disabled for fixture data");
    if(!validExecutable(c.exe))throw std::runtime_error("TeleAgent executable is missing");
    auto scan=scanAccounts(c);if(!scan.error.empty())throw std::runtime_error("Cannot inspect account identity");
    if(owners.empty())throw std::runtime_error("Select at least one account for polling");
    std::vector<Target> targets;std::set<std::string> unique;
    for(auto& owner:owners){auto it=std::find_if(scan.accounts.begin(),scan.accounts.end(),[&](auto& a){return a.owner==owner;});if(!validOwner(owner)||it==scan.accounts.end()||!unique.insert(owner).second)throw std::runtime_error("Invalid polling account selection");targets.push_back({owner,it->note.empty()?(it->name.empty()?it->userId:it->name):it->note});}
    std::string original=scan.pointer["ownerDir"].str();
    std::function<void()> restore;if(validOwner(original))restore=[&]{automaticSwitch(c,original,progress,&stop,false);};
    return run(c,targets,stop,[&](auto& target,auto phase){
        automaticSwitch(c,target.owner,phase,&stop,false);
        // An owner startup record is not proof that a server accepts a stale
        // cookie. Reject locally known expiry instead of saving it as healthy.
        auto refreshed=scanAccounts(c);if(!refreshed.error.empty())throw std::runtime_error("Cannot inspect account identity");
        auto account=std::find_if(refreshed.accounts.begin(),refreshed.accounts.end(),[&](auto& a){return a.owner==target.owner;});
        if(account==refreshed.accounts.end()||!account->current||account->identityMismatch)throw std::runtime_error("TeleAgent restored a different account");
        if(!account->token||account->relogin)throw std::runtime_error("This account has no saved login session");
        for(auto& cookie:account->cookies)if((cookie.name=="operToken"||cookie.name=="operationToken"||cookie.name=="accessToken")&&cookie.persistent&&cookie.expires>0&&cookie.expires/1000000-11644473600LL<=timestamp())throw std::runtime_error("Login cookie has expired; sign in manually");
    },[&](auto& target,auto phase){saveVerifiedAccountSession(c,target.owner,phase);},restore,progress,notice);
}
int pollingSelfTest(const fs::path& directory){
    privateDirectory(directory);auto work=directory/(L"poll-fixture-"+std::to_wstring(GetTickCount64()));Config c;c.fixture=true;c.root=work/L"data";c.state=work/L"state";privateDirectory(c.state);Json::Array results;int failed=0;
    auto require=[](bool ok){if(!ok)throw std::runtime_error("Assertion failed");};auto test=[&](const char* name,auto f){try{f();results.emplace_back(Json::Object{{"test",name},{"passed",true}});}catch(const std::exception& e){++failed;results.emplace_back(Json::Object{{"test",name},{"passed",false},{"error",e.what()}});}};
    std::vector<Target> targets={{"v1_public_1001","测试 A"},{"v1_public_1002","测试 B"},{"v1_public_1003","测试 C"}};
    test("Polling continues after expired login, saves only successes and records failure notices",[&]{std::atomic_bool stop=false;int saved=0,notices=0,restored=0;auto result=run(c,targets,stop,[&](auto& target,auto){if(target.owner==targets[1].owner)throw std::runtime_error("Could not confirm the target identity");},[&](auto&,auto){++saved;},[&]{++restored;},{},[&](auto&,bool){++notices;});require(saved==2&&notices==1&&restored==1&&result["succeeded"].integer()==2&&result["failed"].integer()==1&&std::get<Json::Array>(loadJson(c.state/L"last-poll.json")["results"].v).size()==3);});
    test("Active tasks halt the batch and do not close or restore another account",[&]{std::atomic_bool stop=false;int attempts=0,restored=0;auto result=run(c,targets,stop,[&](auto&,auto){++attempts;throw std::runtime_error("Active agent tasks must finish before switching accounts");},[](auto&,auto){},[&]{++restored;},{},{});require(attempts==1&&restored==0&&result["halted"].boolean()&&result["remaining"].integer()==2);});
    test("Cancellation during identity verification does not save unverified data or continue",[&]{std::atomic_bool stop=false;int attempts=0,saved=0;auto result=run(c,targets,stop,[&](auto&,auto){++attempts;stop=true;},[&](auto&,auto){++saved;},[]{throw std::runtime_error("Must not restore after cancellation");},{},{});require(attempts==1&&saved==0&&result["cancelled"].boolean()&&result["failed"].integer()==0);});
    test("Snapshot failures halt polling and never report the account as saved",[&]{std::atomic_bool stop=false;auto result=run(c,targets,stop,[](auto&,auto){},[](auto&,auto){throw std::runtime_error("snapshot failure");},{},{},{});require(result["succeeded"].integer()==0&&result["failed"].integer()==1&&result["halted"].boolean()&&result["remaining"].integer()==2);});
    test("Restoration failures are recorded and included in the final summary",[&]{std::atomic_bool stop=false;auto result=run(c,{targets.front()},stop,[](auto&,auto){},[](auto&,auto){},[]{throw std::runtime_error("Could not confirm the target identity");},{},{});require(!result["restoreSuccess"].boolean()&&result["summary"].str().find("未能回到原账号")!=std::string::npos);});
    test("Known cookie expiry records a manual login hint and continues",[&]{std::atomic_bool stop=false;int saved=0;auto result=run(c,targets,stop,[&](auto& target,auto){if(target.owner==targets.front().owner)throw std::runtime_error("Login cookie has expired");},[&](auto&,auto){++saved;},{},{},{});require(saved==2&&result["failed"].integer()==1&&!result["halted"].boolean()&&std::get<Json::Array>(result["results"].v).front()["reason"].str().find("手动")!=std::string::npos);});
    test("A stop request before the first account never launches or restores",[&]{std::atomic_bool stop=true;auto unexpected=[](auto&,auto){throw std::runtime_error("Must not launch");};auto result=run(c,targets,stop,unexpected,unexpected,[]{throw std::runtime_error("Must not restore");},{},{});require(result["cancelled"].boolean()&&result["remaining"].integer()==3&&std::get<Json::Array>(result["results"].v).empty());});
    test("Fixture mode cannot poll or launch real accounts",[&]{std::atomic_bool stop=false;bool blocked=false;try{pollAccounts(c,{targets.front().owner},stop);}catch(...){blocked=true;}require(blocked);});
    writeAtomic(directory/L"polling-tests.json",Json(Json::Object{{"failed",Json::num(failed)},{"tests",results}}).dump());return failed?2:0;
}
}
