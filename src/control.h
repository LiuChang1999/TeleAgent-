#pragma once
#include "storage.h"
#include <functional>

namespace ta {
using Progress=std::function<void(const std::wstring&)>;
// Native process control and local session snapshots; no debugging interface.
void syncLiveAccount(const Config& c);
void automaticSwitch(const Config& c,const std::string& owner,Progress progress={});
void automaticNewLogin(const Config& c,Progress progress={});
void automaticStop(const Config& c,Progress progress={});
void launchManaged(const Config& c);
void saveAccountNote(const Config& c,const std::string& owner,const std::string& note);
int controlSelfTest(const fs::path& directory);
}
