#pragma once

#include "PlatformDawn.h"
#include <unistd.h>

inline std::string paneProcessCWD(const Pane* pane)
{
    if (!pane) return {};
    if (!pane->cwd().empty()) return pane->cwd();
    const Terminal* t = pane->terminal();
    if (!t) return {};
    pid_t pgid = tcgetpgrp(t->masterFD());
    if (pgid <= 0) return {};
    return platformProcessCWD(pgid);
}
