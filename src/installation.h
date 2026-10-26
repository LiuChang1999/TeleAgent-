#pragma once
#include "storage.h"
namespace ta {
struct Installation {fs::path exe;std::string source;};
bool validExecutable(const fs::path& path);
fs::path executableFromValue(const std::wstring& value);
Installation findInstallation(const fs::path& state);
void rememberInstallation(const fs::path& state,const fs::path& exe);
fs::path userStateDirectory();
int installationSelfTest(const fs::path& directory);
}
