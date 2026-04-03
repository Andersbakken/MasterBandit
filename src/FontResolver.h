#pragma once

#include <string>

// Resolves a font family name (e.g. "JetBrains Mono") to an absolute font file path.
// Returns empty string if the family cannot be found.
std::string resolveFontFamily(const std::string& family, bool bold = false);
