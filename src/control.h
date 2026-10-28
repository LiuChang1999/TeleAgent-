#pragma once
#include "storage.h"
#include <functional>
#include <atomic>

namespace ta {
using Progress=std::function<void(const std::wstring&)>;
// Native process control and local session snapshots; no debugging interface.
void syncLiveAccount(const Config& c);
struct PollCancelled:std::runtime_error {PollCancelled():std::runtime_error("Account polling cancelled"){} };
void automaticSwitch(const Config& c,const std::string& owner,Progress progress={},const std::atomic_bool* stop=nullptr,bool history=true);
void saveVerifiedAccountSession(const Config& c,const std::string& owner,Progress progress={});
void automaticNewLogin(const Config& c,Progress progress={});
void automaticStop(const Config& c,Progress progress={});
void launchManaged(const Config& c);
void saveAccountNote(const Config& c,const std::string& owner,const std::string& note);
int controlSelfTest(const fs::path& directory);
}
