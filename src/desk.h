#pragma once
#include "storage.h"

namespace ta {
Json accountOrganization(const Config& c);
void saveAccountOrganization(const Config& c,const std::string& owner,const std::string& group,bool pinned);
Json operationHistory(const Config& c);
void recordOperation(const Config& c,const std::string& action,const std::string& owner,bool success,const std::string& reason={});
// Inventory and deletion share the same protection rules. Cleanup requires an
// unchanged preview and accepts no paths from the WebView.
Json backupInventory(const Config& c,int keep);
int cleanupBackups(const Config& c,int keep,const std::string& preview);
int deskSelfTest(const fs::path& directory);
}
