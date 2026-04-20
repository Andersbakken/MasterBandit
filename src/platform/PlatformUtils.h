#pragma once

#include "PlatformDawn.h"
#include <unistd.h>

inline std::string paneProcessCWD(const Terminal* pane)
{
    if (!pane) return {};
    if (!pane->cwd().empty()) return pane->cwd();
    pid_t pgid = tcgetpgrp(pane->masterFD());
    if (pgid <= 0) return {};
    return platformProcessCWD(pgid);
}
