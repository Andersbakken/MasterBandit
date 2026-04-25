#include "Resources.h"

#include <cstdlib>

#ifdef __APPLE__
extern "C" const char* macResourcePathOrNull();
#endif

namespace Resources {
namespace {
    std::filesystem::path g_root;
}

void init(std::string exeDir)
{
    if (const char* env = std::getenv("MB_RESOURCES_DIR"); env && *env) {
        g_root = env;
        return;
    }
#ifdef __APPLE__
    if (const char* p = macResourcePathOrNull()) {
        g_root = p;
        return;
    }
#endif
    g_root = std::move(exeDir);
}

std::filesystem::path path(std::string_view name)
{
    return g_root / name;
}

const std::filesystem::path& root()
{
    return g_root;
}

}
