#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace Resources {

// Initialize once at startup. exeDir is parent_path(argv[0]).
void init(std::string exeDir);

// Absolute path for a resource. Name is a relative path like
// "fonts/nerd/SymbolsNerdFontMono-Regular.ttf" or "shaders".
std::filesystem::path path(std::string_view name);

// Root directory under which all resources live.
const std::filesystem::path& root();

}
