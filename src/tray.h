#pragma once
#include "storage.h"
#include <shellapi.h>
namespace ta {
std::wstring traySummary(const Scan& scan,bool running,bool busy,const std::wstring& progress);
struct TrayAccount {std::string owner;std::wstring label;bool current=false,enabled=false;};
std::vector<TrayAccount> trayAccounts(const Scan& scan,const Json& organization,const Json& history);
class TrayIcon {
    NOTIFYICONDATAW data{};
    bool added=false;
    bool enabled=true;
public:
    bool setEnabled(bool value);
    void initialize(HWND window,UINT callback,HICON icon);
    bool update(const std::wstring& text);
    void notify(const std::wstring& text,bool error);
    bool ensure();
    void explorerRestarted();
    void remove();
};
int traySelfTest(const fs::path& directory);
}
