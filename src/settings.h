#pragma once
#include "storage.h"
namespace ta {
struct DesktopSettings {
    bool trayEnabled=true,minimizeToTray=true,startInTray=false;
    bool autostart=false,autostartInTray=true,notifications=true;
    std::string closeAction="tray";
    int refreshSeconds=3;
};
DesktopSettings parseDesktopSettings(const Json& value);
Json settingsJson(const DesktopSettings& value);
DesktopSettings loadDesktopSettings(const fs::path& state,std::string* warning=nullptr);
void saveDesktopSettings(const Config& c,const DesktopSettings& value,const fs::path& executable);
Json desktopSettingsState(const Config& c,const DesktopSettings& value,const fs::path& executable);
enum class CloseDecision {Tray,Exit,KeepVisible};
CloseDecision windowCloseDecision(const DesktopSettings& value,bool busy);
bool startHidden(const DesktopSettings& value,bool fromAutostart);
int settingsSelfTest(const fs::path& directory);
}
