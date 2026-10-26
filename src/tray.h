#pragma once
#include "storage.h"
#include <shellapi.h>
namespace ta {
std::wstring traySummary(const Scan& scan,bool running,bool busy,const std::wstring& progress);
class TrayIcon {
    NOTIFYICONDATAW data{};
    bool added=false;
public:
    void initialize(HWND window,UINT callback,HICON icon);
    bool update(const std::wstring& text);
    bool ensure();
    void explorerRestarted();
    void remove();
};
int traySelfTest(const fs::path& directory);
}
