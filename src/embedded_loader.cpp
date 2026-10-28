#include "embedded_loader.h"
#include <cstring>
namespace ta {
namespace {
std::string resource(UINT id){
    auto module=GetModuleHandleW(nullptr);auto item=FindResourceW(module,MAKEINTRESOURCEW(id),RT_RCDATA);
    if(!item)throw std::runtime_error("Embedded WebView2 resource is missing");
    auto loaded=LoadResource(module,item);auto bytes=LockResource(loaded);auto size=SizeofResource(module,item);
    if(!bytes||!size)throw std::runtime_error("Embedded WebView2 resource is invalid");
    return std::string(static_cast<const char*>(bytes),size);
}
fs::path loaderPath(const fs::path& state){return state/L"runtime"/L"webview2-1.0.3800.47-x64-86545b66"/L"WebView2Loader.dll";}
void checkFile(const fs::path& path){auto attributes=GetFileAttributesW(path.c_str());if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Embedded loader cache must not contain links");}
}
HMODULE loadEmbeddedWebView(const fs::path& state){
    auto bytes=resource(103);auto path=loaderPath(state);privateDirectory(path.parent_path());checkFile(path);
    bool matches=false;try{matches=readFile(path,1024*1024)==bytes;}catch(...){}
    if(!matches)writeAtomic(path,bytes);
    // Hold a read-only handle through comparison and mapping so cache bytes
    // cannot be replaced between verification and LoadLibraryEx.
    checkFile(path);HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot lock embedded WebView2 loader cache");
    BY_HANDLE_FILE_INFORMATION info{};DWORD read=0;std::string actual(bytes.size(),'\0');
    bool valid=GetFileInformationByHandle(file,&info)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&info.nFileSizeHigh==0&&info.nFileSizeLow==bytes.size()&&ReadFile(file,actual.data(),DWORD(actual.size()),&read,nullptr)&&read==actual.size()&&actual==bytes;
    HMODULE module=valid?LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32):nullptr;CloseHandle(file);
    if(!module)throw std::runtime_error("Cannot verify or load embedded WebView2 loader");
    return module;
}
std::string embeddedWebViewLicense(){return resource(104);}
int loaderSelfTest(const fs::path& directory){
    privateDirectory(directory);auto state=directory/(L"loader-fixture-"+std::to_wstring(GetTickCount64()));Json::Array results;int failed=0;
    auto test=[&](const char* name,auto f){try{if(!f())throw std::runtime_error("Assertion failed");results.emplace_back(Json::Object{{"test",name},{"passed",true}});}catch(const std::exception& e){++failed;results.emplace_back(Json::Object{{"test",name},{"passed",false},{"error",e.what()}});}};
    test("Embedded loader extracts and exports the WebView2 entry point",[&]{auto module=loadEmbeddedWebView(state);bool ok=GetProcAddress(module,"CreateCoreWebView2EnvironmentWithOptions")!=nullptr;FreeLibrary(module);return ok&&readFile(loaderPath(state))==resource(103);});
    test("Corrupt cache is repaired from executable resources",[&]{writeAtomic(loaderPath(state),"corrupt-test-loader");auto module=loadEmbeddedWebView(state);FreeLibrary(module);return readFile(loaderPath(state))==resource(103);});
    test("WebView2 license is embedded in the executable",[&]{return embeddedWebViewLicense().find("Microsoft Corporation")!=std::string::npos;});
    writeAtomic(directory/L"loader-tests.json",Json(Json::Object{{"failed",Json::num(failed)},{"tests",results}}).dump());return failed?2:0;
}
}
