#pragma once
#include "control.h"
namespace ta {
using PollNotice=std::function<void(const std::wstring&,bool)>;
Json pollAccounts(const Config& c,const std::vector<std::string>& owners,std::atomic_bool& stop,Progress progress={},PollNotice notice={});
int pollingSelfTest(const fs::path& directory);
}
