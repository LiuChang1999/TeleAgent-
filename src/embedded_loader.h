#pragma once
#include "storage.h"
namespace ta {
HMODULE loadEmbeddedWebView(const fs::path& state);
std::string embeddedWebViewLicense();
int loaderSelfTest(const fs::path& directory);
}
